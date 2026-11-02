#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "../mods/friend_list_client_fix/chat_open_policy.h"

extern "C" __declspec(dllimport) int __cdecl SdgoModLoad(const SdgoApi*, SdgoMod*);

namespace
{
    constexpr std::uint32_t kZf = 0x40;
    constexpr std::uint32_t kInjectedFlags = 0x0C01; // CF/DF/OF
    constexpr std::uint32_t kObservedFlags = 0x0FD5; // CF/PF/AF/ZF/SF/TF/IF/DF/OF
    constexpr std::uint32_t kEbx = 0x11223344;
    constexpr std::uint32_t kEcx = 0x55667788;
    constexpr std::uint32_t kEdx = 0x99AABBCC;
    constexpr std::uint32_t kEsi = 0x13579BDF;
    constexpr std::uint32_t kEdi = 0x2468ACE0;
    constexpr std::uint32_t kEbp = 0x0BADF00D;

    struct GateState
    {
        std::uint32_t eax;
        std::uint32_t ebx;
        std::uint32_t ecx;
        std::uint32_t edx;
        std::uint32_t esi;
        std::uint32_t edi;
        std::uint32_t ebp;
        std::uint32_t entryEsp;
        std::uint32_t exitEsp;
    };

    int g_checks = 0;
    int g_failures = 0;
    bool g_rejectLayout = false;
    bool g_rejectHook = false;
    bool g_layoutRequested = false;
    bool g_gateRequested = false;
    void* g_gateCode = nullptr;
    std::uint32_t g_openCalls = 0;
    std::uint32_t g_openFlags = 0;
    std::uint32_t g_blockedFlags = 0;
    GateState g_state = {};

    void Check(bool value, const char* message)
    {
        ++g_checks;
        if (!value) {
            ++g_failures;
            std::printf("FAIL %s\n", message);
        }
    }

    bool Same(const char* left, const char* right)
    {
        return left && right && std::strcmp(left, right) == 0;
    }

    void FakeLog(SdgoMod*, int, const char*, ...) {}

    void* FakeSymbol(SdgoMod*, const char* name)
    {
        if (!Same(name, friend_list_client_fix::kLayoutSymbol)) return nullptr;
        g_layoutRequested = true;
        return g_rejectLayout ? nullptr : g_gateCode;
    }

    int FakeHookSite(SdgoMod*, const char* name, void* detour, void** original)
    {
        if (!Same(name, friend_list_client_fix::kGateSymbol)) return 0;
        g_gateRequested = true;
        if (g_rejectHook || !g_gateCode || !detour || !original) return 0;
        if (MH_CreateHook(g_gateCode, detour, original) != MH_OK) return 0;
        return MH_EnableHook(g_gateCode) == MH_OK;
    }

    // Called by the native instruction fixture after its `push eax; mov ecx,esi`.
    // It snapshots flags at the real open-conversation boundary, then emulates
    // thiscall cleanup of the one stack argument.
    __declspec(naked) void OpenConversationFixture()
    {
        __asm {
            pushfd
            pop dword ptr [g_openFlags]
            inc dword ptr [g_openCalls]
            mov eax, 1
            ret 4
        }
    }

    bool BuildGateFixture()
    {
        auto* code = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!code) return false;

        // Same gate instructions as GOnline 0x00964549 through 0x00964552:
        //   je +8; push eax; mov ecx,esi; call OpenConversationFixture
        // Both paths then snapshot flags and return to the test caller.
        const std::uint8_t prefix[] = {0x74, 0x08, 0x50, 0x8B, 0xCE, 0xE8};
        std::memcpy(code, prefix, sizeof(prefix));
        const auto next = reinterpret_cast<std::intptr_t>(code + 10);
        const auto destination = reinterpret_cast<std::intptr_t>(&OpenConversationFixture);
        const auto displacement = static_cast<std::int32_t>(destination - next);
        std::memcpy(code + 6, &displacement, sizeof(displacement));
        code[10] = 0x9C; // pushfd
        code[11] = 0x58; // pop eax
        code[12] = 0xA3; // mov [g_blockedFlags],eax
        const auto flagsAddress =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_blockedFlags));
        std::memcpy(code + 13, &flagsAddress, sizeof(flagsAddress));
        code[17] = 0xC3; // ret
        FlushInstructionCache(GetCurrentProcess(), code, 18);
        g_gateCode = code;
        return true;
    }

    __declspec(naked) std::uint32_t NativeCmpFlags(const std::uint8_t*)
    {
        __asm {
            mov eax, dword ptr [esp + 4]
            cmp dword ptr [eax + 1Ch], 0
            pushfd
            pop eax
            ret
        }
    }

    // Enters the actual hooked instruction fixture with the same live EAX and
    // flags as the client. Captures register and stack effects after the
    // trampoline either opens or skips the native conversation call.
    __declspec(naked) void RunGate(const std::uint8_t*)
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov dword ptr [g_state + 28], esp
            mov ebx, 11223344h
            mov ecx, 55667788h
            mov edx, 99AABBCCh
            mov esi, 13579BDFh
            mov edi, 2468ACE0h
            mov ebp, 0BADF00Dh
            mov eax, dword ptr [esp + 20]
            cmp dword ptr [eax + 1Ch], 0
            pushfd
            or dword ptr [esp], 0C01h
            popfd
            call dword ptr [g_gateCode]
            mov dword ptr [g_state + 0], eax
            mov dword ptr [g_state + 4], ebx
            mov dword ptr [g_state + 8], ecx
            mov dword ptr [g_state + 12], edx
            mov dword ptr [g_state + 16], esi
            mov dword ptr [g_state + 20], edi
            mov dword ptr [g_state + 24], ebp
            mov eax, esp
            mov dword ptr [g_state + 32], eax
            cld
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    std::array<std::uint8_t, 0x38> Row(std::uint32_t identity, std::uint8_t nameLead,
                                       std::uint8_t relationship, std::uint32_t presence)
    {
        std::array<std::uint8_t, 0x38> row = {};
        std::memcpy(row.data(), &identity, sizeof(identity));
        row[0x0A] = nameLead;
        row[0x1B] = relationship;
        std::memcpy(row.data() + 0x1C, &presence, sizeof(presence));
        return row;
    }

    void CheckGate(const std::array<std::uint8_t, 0x38>& row, bool opens,
                   bool clearsZeroFlag, const char* behavior)
    {
        g_openCalls = 0;
        g_openFlags = 0;
        g_blockedFlags = 0;
        g_state = {};
        const std::uint32_t nativeFlags = NativeCmpFlags(row.data());
        RunGate(row.data());

        Check((g_openCalls != 0) == opens, behavior);
        const std::uint32_t observedFlags = opens ? g_openFlags : g_blockedFlags;
        const std::uint32_t injectedFlags = nativeFlags | kInjectedFlags;
        const std::uint32_t expectedFlags =
            clearsZeroFlag ? injectedFlags & ~kZf : injectedFlags;
        Check(((observedFlags ^ expectedFlags) & kObservedFlags) == 0,
              "detour changes only the intended zero flag");
        Check(g_state.ebx == kEbx && g_state.edx == kEdx &&
                  g_state.esi == kEsi && g_state.edi == kEdi && g_state.ebp == kEbp,
              "detour and trampoline preserve unrelated general registers");
        Check(g_state.ecx == (opens ? kEsi : kEcx),
              "native mov ecx,esi runs only on the open path");
        Check(g_state.entryEsp == g_state.exitEsp,
              "detour, trampoline and native thiscall keep the stack balanced");
    }

    template <std::size_t N>
    bool Match(const std::vector<std::uint8_t>& image, std::size_t offset,
               const std::array<std::uint8_t, N>& expected)
    {
        return offset <= image.size() && N <= image.size() - offset &&
               std::equal(expected.begin(), expected.end(), image.begin() + offset);
    }

    bool NtHeaders(const std::vector<std::uint8_t>& image,
                   const IMAGE_NT_HEADERS32*& nt)
    {
        nt = nullptr;
        if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
        const std::size_t offset = static_cast<std::size_t>(dos->e_lfanew);
        if (offset > image.size() ||
            sizeof(IMAGE_NT_HEADERS32) > image.size() - offset) {
            return false;
        }
        nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + offset);
        return nt->Signature == IMAGE_NT_SIGNATURE &&
               nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    }

    bool RvaToOffset(const std::vector<std::uint8_t>& image, std::uint32_t rva,
                     std::size_t& offset)
    {
        const IMAGE_NT_HEADERS32* nt = nullptr;
        if (!NtHeaders(image, nt)) return false;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const auto sectionOffset =
            reinterpret_cast<const std::uint8_t*>(sections) - image.data();
        if (sectionOffset < 0 ||
            static_cast<std::size_t>(sectionOffset) > image.size() ||
            nt->FileHeader.NumberOfSections >
                (image.size() - static_cast<std::size_t>(sectionOffset)) /
                    sizeof(IMAGE_SECTION_HEADER)) {
            return false;
        }
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            const std::uint32_t span =
                (section.Misc.VirtualSize > section.SizeOfRawData)
                    ? section.Misc.VirtualSize
                    : section.SizeOfRawData;
            if (rva < section.VirtualAddress ||
                rva - section.VirtualAddress >= span) {
                continue;
            }
            const std::uint32_t within = rva - section.VirtualAddress;
            if (within >= section.SizeOfRawData) return false;
            offset = static_cast<std::size_t>(section.PointerToRawData) + within;
            return offset < image.size();
        }
        return false;
    }

    void CheckClientImage(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
        Check(input.good() || input.eof(), "supplied client image is readable");

        const IMAGE_NT_HEADERS32* nt = nullptr;
        Check(NtHeaders(image, nt) &&
                  nt->FileHeader.TimeDateStamp == 0x54BE1568 &&
                  nt->OptionalHeader.SizeOfImage == 0x00C97000,
              "fixture is the supported GOnline image");

        constexpr std::array<std::uint8_t, 17> layout = {
            0x83, 0x78, 0x1C, 0x00, 0x74, 0x08, 0x50, 0x8B, 0xCE,
            0xE8, 0x3D, 0xE9, 0xFF, 0xFF, 0x8B, 0x4D, 0xF4
        };
        constexpr std::array<std::uint8_t, 13> gate = {
            0x74, 0x08, 0x50, 0x8B, 0xCE, 0xE8, 0x3D,
            0xE9, 0xFF, 0xFF, 0x8B, 0x4D, 0xF4
        };
        std::size_t layoutOffset = 0;
        std::size_t gateOffset = 0;
        Check(RvaToOffset(image, friend_list_client_fix::kLayoutRva, layoutOffset) &&
                  Match(image, layoutOffset, layout),
              "exact presence comparison and native chat call are pinned");
        const bool gateMatches =
            RvaToOffset(image, friend_list_client_fix::kGateRva, gateOffset) &&
            Match(image, gateOffset, gate);
        Check(gateMatches,
              "HookSite begins at the exact conditional branch");

        if (gateMatches) {
            image[gateOffset] ^= 1;
            Check(!Match(image, gateOffset, gate),
                  "a one-byte host change fails the exact gate signature");
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace friend_list_client_fix;

    const FriendRow offline{1001, static_cast<std::uint8_t>('A'), 7, 0};
    const FriendRow online{1001, static_cast<std::uint8_t>('A'), 7, 1};
    const FriendRow inRoom{1001, static_cast<std::uint8_t>('A'), 7, 3};
    const FriendRow category{0, 0, 0, 0};
    const FriendRow pending{1001, static_cast<std::uint8_t>('A'), 2, 0};
    const FriendRow unnamed{1001, 0, 7, 0};
    Check(!OpensNativeConversation(false, &offline),
          "no FriendTree selection exits before the hook");
    Check(!OpensNativeConversation(true, nullptr),
          "a selection without a friend payload is not opened");
    Check(OpensNativeConversation(true, &online) &&
              OpensNativeConversation(true, &inRoom),
          "online and in-room friends retain the original open path");
    Check(OpensNativeConversation(true, &offline),
          "complete accepted offline friend is newly allowed");
    Check(!OpensNativeConversation(true, &category) &&
              !OpensNativeConversation(true, &pending) &&
              !OpensNativeConversation(true, &unnamed),
          "category, pending and unnamed rows remain blocked");

    if (argc > 2) {
        std::puts("FAIL usage: friend_list_client_fix_test [GOnline.exe]");
        return 2;
    }
    Check(BuildGateFixture(), "native gate fixture allocated");
    if (!g_gateCode) return 2;

    SdgoApi api = {};
    api.version = SDGO_API_VERSION;
    api.size = static_cast<std::uint32_t>(offsetof(SdgoApi, HookSite));
    api.Log = &FakeLog;
    api.Symbol = &FakeSymbol;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0,
          "mod declines an older core before reading HookSite");

    api.size = sizeof(api);
    api.HookSite = &FakeHookSite;
    g_rejectLayout = true;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0 && !g_gateRequested,
          "missing or mismatched layout receipt fails closed before hooking");
    g_rejectLayout = false;
    g_rejectHook = true;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0 && g_gateRequested,
          "rejected gate ownership or signature fails closed");

    g_layoutRequested = false;
    g_gateRequested = false;
    g_rejectHook = false;
    const MH_STATUS initialized = MH_Initialize();
    Check(initialized == MH_OK, "MinHook initialized for the actual site fixture");
    const bool active = initialized == MH_OK &&
        SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) != 0;
    Check(active && g_layoutRequested && g_gateRequested,
          "production DLL binds both verified symbols and activates");

    if (active) {
        CheckGate(Row(1001, 'A', 7, 1), true, false,
                  "online friend still opens through the native branch");
        CheckGate(Row(1001, 'A', 7, 3), true, false,
                  "in-room friend still opens through the native branch");
        CheckGate(Row(1001, 'A', 7, 0), true, true,
                  "accepted offline friend opens after clearing only ZF");
        CheckGate(Row(0, 'A', 7, 0), false, false,
                  "zero-identity category row stays blocked");
        CheckGate(Row(1001, 0, 7, 0), false, false,
                  "blank-name row stays blocked");
        CheckGate(Row(1001, 'A', 2, 0), false, false,
                  "non-accepted relationship stays blocked");
        Check(MH_DisableHook(g_gateCode) == MH_OK &&
                  MH_RemoveHook(g_gateCode) == MH_OK,
              "native gate fixture hook removed cleanly");
    }
    if (initialized == MH_OK) {
        Check(MH_Uninitialize() == MH_OK, "MinHook uninitialized cleanly");
    }

    if (argc == 2) CheckClientImage(argv[1]);

    VirtualFree(g_gateCode, 0, MEM_RELEASE);
    std::printf("%d FRIEND LIST CLIENT CHECKS, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
