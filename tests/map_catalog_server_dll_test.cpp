#ifdef NDEBUG
#undef NDEBUG
#endif

#include <windows.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"

namespace {
void* g_hook = nullptr;
std::wstring g_directory;

__declspec(naked) void ContinueNative() {
    __asm { mov eax, edx }
    __asm { ret }
}

__declspec(naked) std::uint32_t Invoke(void*, void*, std::uint32_t) {
    __asm { push ebp }
    __asm { mov eax, [esp + 8] }
    __asm { mov ebp, [esp + 12] }
    __asm { mov edx, [esp + 16] }
    __asm { call eax }
    __asm { pop ebp }
    __asm { ret }
}

int HookSite(SdgoMod*, const char* name, void* detour, void** original) {
    if (std::strcmp(name, "map_catalog_server.phase_success_deadline") != 0) return 0;
    g_hook = detour;
    *original = reinterpret_cast<void*>(&ContinueNative);
    return 1;
}
const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }
void Log(SdgoMod*, int, const char*, ...) {}

template<class T>
void Put(void* base, std::ptrdiff_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto temp = std::filesystem::temp_directory_path() /
        std::filesystem::path(L"sdgo-map-pacing-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(temp);
    g_directory = temp.wstring();
    const auto config = (temp / L"config.ini").wstring();
    assert(WritePrivateProfileStringW(L"settings", L"ImmediateTransitionMissionIds", L"90001,90002", config.c_str()));

    const auto library = LoadLibraryW(std::filesystem::path(argv[1]).c_str());
    assert(library);
    using Entry = int(__cdecl*)(const SdgoApi*, SdgoMod*);
    const auto entry = reinterpret_cast<Entry>(GetProcAddress(library, "SdgoModLoad"));
    assert(entry);
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.host_key = "BattleServer-548A891D-0046C000";
    api.HookSite = &HookSite;
    api.ModDirectory = &Directory;
    api.Log = &Log;
    assert(entry(&api, reinterpret_cast<SdgoMod*>(1)) == 1 && g_hook);

    std::vector<std::uint8_t> room(0x31800);
    std::vector<std::uint8_t> frame_storage(0x200);
    std::uint32_t mission_mode[2] = {90001, 0};
    std::uint8_t clear_row[0x30]{};
    clear_row[0x2c] = 7;
    auto* frame = frame_storage.data() + 0x100;
    Put(frame, -0x68, room.data());
    Put(frame, 0x08, clear_row);
    Put(room.data(), 0x316a0, mission_mode);

    SetLastError(73);
    assert(Invoke(g_hook, frame, 102) == 99);
    assert(GetLastError() == 73);
    clear_row[0x2c] = 0;
    assert(Invoke(g_hook, frame, 102) == 102);  // final settlement keeps stock delay
    clear_row[0x2c] = 7;
    mission_mode[0] = 77;
    assert(Invoke(g_hook, frame, 102) == 102);
    Put<void*>(room.data(), 0x316a0, nullptr);
    assert(Invoke(g_hook, frame, 102) == 102);

    using Count = std::uint32_t(__cdecl*)();
    const auto count = reinterpret_cast<Count>(
        GetProcAddress(library, "SdgoMapCatalogServerAcceleratedV1"));
    assert(count && count() == 1);

    FreeLibrary(library);
    std::filesystem::remove_all(temp);
    return 0;
}
