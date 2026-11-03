#pragma once
// Bounded original-API replay only: no imports, executable entry, TLS callbacks,
// network, game window, or original process is started by this mapper.
#include <windows.h>
#include "sdgo/script_registration.h"
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

class NativeLuaImage {
public:
    static constexpr char kReservationCookie[] = "SDGO_PRIVATE_LUA_REPLAY_RESERVATION";
    explicit NativeLuaImage(const char* path, bool inheritedReservation = false) {
        // Reserve before allocating the input byte vector: the fixed source has
        // no relocation directory, and that large buffer can occupy this range.
        if (inheritedReservation) {
            MEMORY_BASIC_INFORMATION memory = {};
            if (VirtualQuery(reinterpret_cast<void*>(0x400000), &memory, sizeof(memory)) == sizeof(memory)
                && memory.AllocationBase == reinterpret_cast<void*>(0x400000) && memory.Type == MEM_PRIVATE
                && memory.State == MEM_COMMIT && memory.RegionSize >= 0xc97000 && memory.Protect == PAGE_READWRITE
                && std::memcmp(reinterpret_cast<void*>(0x400000), kReservationCookie, sizeof(kReservationCookie)) == 0)
                image_ = reinterpret_cast<std::uint8_t*>(0x400000);
        } else image_ = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x400000), 0xc97000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (image_ != reinterpret_cast<void*>(0x400000)) { if (image_) VirtualFree(image_, 0, MEM_RELEASE); image_ = nullptr; throw std::runtime_error("preferred native fixture image range unavailable before source read"); }
        try {
        std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("native API fixture input unavailable");
        std::vector<std::uint8_t> raw(std::istreambuf_iterator<char>(file), {});
        if (sdgo::scripts::Sha256(raw) != "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92") throw std::runtime_error("native API executable identity differs");
        if (raw.size() < sizeof(IMAGE_DOS_HEADER)) throw std::runtime_error("native API PE header truncated");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > raw.size()) throw std::runtime_error("native API PE header invalid");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.ImageBase != 0x400000 || nt->OptionalHeader.SizeOfImage != 0xc97000) throw std::runtime_error("native API PE layout differs");
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        if (reinterpret_cast<const std::uint8_t*>(sections + nt->FileHeader.NumberOfSections) > raw.data() + raw.size()) throw std::runtime_error("native API section table invalid");
            std::memcpy(image_, raw.data(), nt->OptionalHeader.SizeOfHeaders);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                const auto& s = sections[i];
                if (s.VirtualAddress > 0xc97000 || s.SizeOfRawData > 0xc97000 - s.VirtualAddress || s.PointerToRawData > raw.size() || s.SizeOfRawData > raw.size() - s.PointerToRawData) throw std::runtime_error("native API section extent invalid");
                if (s.SizeOfRawData) std::memcpy(image_ + s.VirtualAddress, raw.data() + s.PointerToRawData, s.SizeOfRawData);
            }
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                const auto& s = sections[i]; const auto size = s.Misc.VirtualSize > s.SizeOfRawData ? s.Misc.VirtualSize : s.SizeOfRawData; if (!size) continue;
                DWORD protection = (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) ? PAGE_EXECUTE_READ : (s.Characteristics & IMAGE_SCN_MEM_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
                DWORD prior = 0; if (!VirtualProtect(image_ + s.VirtualAddress, size, protection, &prior)) throw std::runtime_error("native API page protection failed");
            }
            FlushInstructionCache(GetCurrentProcess(), image_, 0xc97000);
        } catch (...) { VirtualFree(image_, 0, MEM_RELEASE); image_ = nullptr; throw; }
    }
    ~NativeLuaImage() { if (image_) VirtualFree(image_, 0, MEM_RELEASE); }
    NativeLuaImage(const NativeLuaImage&) = delete;
    NativeLuaImage& operator=(const NativeLuaImage&) = delete;
    template<class Fn> Fn at(std::uint32_t va) const { if (va < 0x400000 || va >= 0x1097000) throw std::runtime_error("native API address extent"); return reinterpret_cast<Fn>(image_ + va - 0x400000); }
private:
    std::uint8_t* image_ = nullptr;
};
