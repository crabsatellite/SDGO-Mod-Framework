// Keep the FPS value already parsed by the native config.xml reader.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include "sdgo/sdgo_api.h"

namespace {
    const SdgoApi* g_api;
    SdgoMod* g_mod;
    void* g_resume;
    void* g_original;
    const DWORD g_defaultMxcsr = 0x1F80;

    void __stdcall ConfiguredCap(uint8_t* app)
    {
        const DWORD error = GetLastError();
        // The native XML reader has just populated CApplication+0x80. Missing
        // configuration retains the native default; invalid caps use 60.
        const int requested = *reinterpret_cast<int*>(app + 0x80);
        const int effective = requested >= 1 && requested <= 1000 ? requested : 60;
        *reinterpret_cast<int*>(app + 0x80) = effective;
        g_api->Log(g_mod, requested == effective ? SDGO_LOG_INFO : SDGO_LOG_WARN,
                   "config.xml updatefps=%d; effective FPS cap=%d%s", requested, effective,
                   requested == effective ? "" : " (invalid value; using 60)");
        SetLastError(error);
    }

    // This replaces exactly the native ten-byte MOV [EDI+80h],60, then resumes
    // AFTER it. Calling the MinHook trampoline would reinstate the forced 60.
    // Preserve every register, flags, x87/SIMD state and Win32 last error.
    __declspec(naked) void PreserveConfiguredCap()
    {
        __asm { pushfd }
        __asm { pushad }
        __asm { mov eax, esp }
        __asm { sub esp, 0x210 }
        __asm { and esp, 0xfffffff0 }
        __asm { mov [esp + 0x200], eax }
        __asm { fxsave [esp] }
        __asm { fninit }
        __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
        __asm { cld }
        __asm { push edi }
        __asm { call ConfiguredCap }
        __asm { fxrstor [esp] }
        __asm { mov esp, [esp + 0x200] }
        __asm { popad }
        __asm { popfd }
        __asm { jmp dword ptr [g_resume] }
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->HookSite)
        return 0;
    auto* site = static_cast<uint8_t*>(api->Symbol(mod, "fps.config.force60"));
    if (!site) return 0;
    g_api = api;
    g_mod = mod;
    g_resume = site + 10; // whole verified native instruction; never execute it
    if (!api->HookSite(mod, "fps.config.force60", reinterpret_cast<void*>(&PreserveConfiguredCap), &g_original))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "native config.xml FPS cap enabled; applied when Framework/Timer is read");
    return 1;
}
