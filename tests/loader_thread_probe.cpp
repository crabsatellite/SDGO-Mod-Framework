#include <windows.h>
#include "sdgo/sdgo_api.h"

namespace {
DWORD g_initial = 0;
DWORD g_boot = 0;
}

extern "C" __declspec(dllexport) void __cdecl ReadThreadProbe(DWORD* initial, DWORD* boot) {
    *initial = g_initial;
    *boot = g_boot;
}

SDGO_MOD_ENTRY {
    (void)mod;
    if (!api || api->size < offsetof(SdgoApi, process_initial_thread_id) + sizeof(api->process_initial_thread_id)) return 0;
    g_initial = api->process_initial_thread_id;
    g_boot = GetCurrentThreadId();
    return 1;
}
