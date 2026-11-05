// No CRT entry/cookie/time import: exercise a real worker-thread fallback boot.
// The static fixture imports the actual d3d9 shim. Dynamic and legacy fixtures
// prove that the same worker is never promoted to the process-initial thread.
#include <windows.h>
#include "sdgo/sdgo_api.h"

#if defined(SDGO_STATIC_THREAD_FIXTURE)
extern "C" __declspec(dllimport) void* WINAPI Direct3DCreate9(UINT);
#endif

namespace {
DWORD g_initial = 0;

bool Sibling(const wchar_t* suffix, wchar_t* out) {
    DWORD length = GetModuleFileNameW(nullptr, out, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    while (length && out[length - 1] != L'\\') --length;
    if (!length) return false;
    while (*suffix && length + 1 < MAX_PATH) out[length++] = *suffix++;
    out[length] = 0;
    return !*suffix;
}

DWORD WINAPI Worker(void*) {
#if defined(SDGO_STATIC_THREAD_FIXTURE)
    Direct3DCreate9(~0u); // Invalid SDK: exercise boot without creating a device.
#elif defined(SDGO_LEGACY_THREAD_FIXTURE)
    wchar_t path[MAX_PATH];
    if (!Sibling(L"sdgo\\core.dll", path)) return 2;
    HMODULE core = LoadLibraryW(path);
    auto init = core ? reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")) : nullptr;
    if (!init) return 3;
    // Pass the physical old prefix, rather than a new struct with a zero tail.
    struct OldBoot { uint32_t size; const char* loader; void* host; };
    static_assert(sizeof(OldBoot) == offsetof(SdgoBoot, process_initial_thread_id));
    OldBoot legacyBoot = {sizeof(OldBoot), "legacy-thread-fixture", GetModuleHandleW(nullptr)};
    if (!init(reinterpret_cast<const SdgoBoot*>(&legacyBoot))) return 4;
#else
    wchar_t path[MAX_PATH];
    if (!Sibling(L"d3d9.dll", path)) return 2;
    HMODULE shim = LoadLibraryW(path);
    auto forward = shim ? reinterpret_cast<void*(WINAPI*)(UINT)>(GetProcAddress(shim, "Direct3DCreate9")) : nullptr;
    if (!forward) return 3;
    forward(~0u);
#endif
    HMODULE probe = GetModuleHandleW(L"loader_thread_probe.dll");
    auto read = probe ? reinterpret_cast<void(__cdecl*)(DWORD*, DWORD*)>(GetProcAddress(probe, "ReadThreadProbe")) : nullptr;
    if (!read) return 5;
    DWORD initial = 0, boot = 0;
    read(&initial, &boot);
#if defined(SDGO_STATIC_THREAD_FIXTURE)
    if (initial != g_initial || !initial) return 6;
#else
    if (initial != 0) return 6;
#endif
    return boot == GetCurrentThreadId() && boot != g_initial ? 0 : 7;
}
}

extern "C" void __cdecl Run() {
    g_initial = GetCurrentThreadId();
    HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!thread) ExitProcess(10);
    if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) ExitProcess(11);
    DWORD code = 12;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    const char* text = code == 0 ? "PASS real loader boot-thread identity\n" : "FAIL real loader boot-thread identity\n";
    DWORD size = 0, written = 0;
    while (text[size]) ++size;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, size, &written, nullptr);
    ExitProcess(code);
}
