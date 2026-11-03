#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE shim = LoadLibraryW(argv[1]);
    wchar_t path[MAX_PATH] = {};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dbghelp.dll");
    HMODULE system = LoadLibraryW(path);
    if (!shim || !system || shim == system) return 1;
    const char* names[] = {"SymCleanup", "SymGetLineFromAddr64", "SymFromAddr",
        "StackWalk", "SymFunctionTableAccess", "SymGetModuleBase", "SymInitialize",
        "SymSetOptions", "EnumerateLoadedModules", "GetTimestampForLoadedLibrary",
        "MiniDumpWriteDump"};
    for (const char* name : names)
        if (!GetProcAddress(shim, name) || !GetProcAddress(system, name)) return 1;
    using Options = DWORD(WINAPI*)(DWORD);
    using Initialize = BOOL(WINAPI*)(HANDLE, const char*, BOOL);
    using Cleanup = BOOL(WINAPI*)(HANDLE);
    using Timestamp = DWORD(WINAPI*)(HMODULE);
    auto directOptions = reinterpret_cast<Options>(GetProcAddress(system, "SymSetOptions"));
    auto options = reinterpret_cast<Options>(GetProcAddress(shim, "SymSetOptions"));
    const DWORD original = directOptions(0x10);
    if (options(0x20) != directOptions(0x20)) return 1;
    auto initialize = reinterpret_cast<Initialize>(GetProcAddress(shim, "SymInitialize"));
    auto cleanup = reinterpret_cast<Cleanup>(GetProcAddress(shim, "SymCleanup"));
    if (!initialize(GetCurrentProcess(), nullptr, FALSE) || !cleanup(GetCurrentProcess())) return 1;
    auto timestamp = reinterpret_cast<Timestamp>(GetProcAddress(shim, "GetTimestampForLoadedLibrary"));
    auto directTimestamp = reinterpret_cast<Timestamp>(GetProcAddress(system, "GetTimestampForLoadedLibrary"));
    if (timestamp(GetModuleHandleW(nullptr)) != directTimestamp(GetModuleHandleW(nullptr))) return 1;
    directOptions(original);
    FreeLibrary(shim);
    FreeLibrary(system);
    std::puts("native monitor fixture loader: 11 exports, real stdcall forwarding and system identity passed");
    return 0;
}
