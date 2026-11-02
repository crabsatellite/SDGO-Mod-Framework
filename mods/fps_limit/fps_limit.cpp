// Capture the render cap while preserving the native 60 Hz simulation.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include "sdgo/sdgo_api.h"
#include "sdgo/fps_limit_observer.h"

namespace {
    const SdgoApi* g_api;
    SdgoMod* g_mod;
    void* g_resume;
    void* g_original;
    using IdleFn = void(__thiscall*)(uint8_t*);
    IdleFn g_idle;
    using PresentFn=HRESULT(__thiscall*)(void*,HWND);
    PresentFn g_present;
    HANDLE g_paceTimer;
    LARGE_INTEGER g_frequency;
    LONGLONG g_nextRender;
    int g_renderCap;
    int g_configuredCap=60;
    constexpr int kNativeUpdateHz=60;
    uint32_t g_presentCalls,g_waitedCalls;
    uint64_t g_periodTicks;
    bool g_logFps;
    LONGLONG g_sampleStart;
    uint32_t g_sampleFrames,g_sampleRejected;
    const DWORD g_defaultMxcsr = 0x1F80;

    void __stdcall ConfiguredCap(uint8_t* app)
    {
        const DWORD error = GetLastError();
        // The native XML reader has just populated CApplication+0x80. Missing
        // configuration retains the native default; invalid caps use 60.
        const int requested = *reinterpret_cast<int*>(app + 0x80);
        const int effective = requested >= 1 && requested <= 1000 ? requested : 60;
        g_configuredCap=effective;
        // This field drives fixed-per-update gameplay, not just presentation.
        // Preserve the native forced-60 assignment after retaining the user's
        // render setting separately. Higher values accelerate movement and AI.
        *reinterpret_cast<int*>(app + 0x80) = kNativeUpdateHz;
        g_api->Log(g_mod, requested == effective ? SDGO_LOG_INFO : SDGO_LOG_WARN,
                   "config.xml updatefps=%d; effective FPS cap=%d; native update rate=60 Hz%s", requested, effective,
                   requested == effective ? "" : " (invalid value; using 60)");
        SetLastError(error);
    }

    void ApplyCap(uint8_t* app)
    {
        auto* timer = *reinterpret_cast<uint8_t**>(app + 0x74);
        if (!timer)return;
        // Logic retains its native frequency even when rendering faster.
        // Never derive the render setting from app+0x80: the original client
        // correctly keeps that gameplay field at 60.
        const float interval=1.0f / static_cast<float>(kNativeUpdateHz);
        auto* current=reinterpret_cast<float*>(timer+0x18);
        // State changes can recreate/reset the native timer after initialization.
        // Read on each original idle boundary; write only when the native value
        // or render gate actually differs. Never reset accumulated time or the
        // clock source. Presentation is paced separately at its native wrapper.
        if (*current==interval && app[0x44A]==0)return;
        const DWORD error=GetLastError();
        *current=interval;
        // Keep native rendering independent: its RenderSync gate drops draws
        // while the update timer catches up after a stall. The Present detour
        // below supplies the actual presentation limit without altering time.
        app[0x44A] = 0;
        g_api->Log(g_mod, SDGO_LOG_INFO, "applied native FPS timer: render_cap=%d update_cap=%d interval=%.9f seconds; render pacing active",
                   g_configuredCap,kNativeUpdateHz,static_cast<double>(*reinterpret_cast<const float*>(timer + 0x18)));
        SetLastError(error);
    }

    void __fastcall Idle(uint8_t* app, void*)
    {
        ApplyCap(app);
        g_idle(app);
    }

    void Pace()
    {
        const int requested=g_configuredCap;
        const int cap=requested>=1 && requested<=1000 ? requested:60;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        if (cap!=g_renderCap) { g_renderCap=cap;g_nextRender=0; }
        g_periodTicks=static_cast<uint64_t>((g_frequency.QuadPart+cap-1)/cap);
        if (g_nextRender && now.QuadPart<g_nextRender)++g_waitedCalls;
        while (g_nextRender && now.QuadPart<g_nextRender) {
            const LONGLONG remaining=g_nextRender-now.QuadPart;
            LARGE_INTEGER due{};
            due.QuadPart=-((remaining*10000000+g_frequency.QuadPart-1)/g_frequency.QuadPart);
            if (SetWaitableTimer(g_paceTimer,&due,0,nullptr,nullptr,FALSE)) {
                const DWORD budget=static_cast<DWORD>(remaining*1000/g_frequency.QuadPart)+100;
                WaitForSingleObject(g_paceTimer,budget);
            } else Sleep(1);
            QueryPerformanceCounter(&now);
        }
        // Schedule from the actual start, never catch up with a burst of draws.
        g_nextRender=now.QuadPart+static_cast<LONGLONG>(g_periodTicks);
    }

    HRESULT __fastcall Present(void* graphics, void*, HWND window)
    {
        const DWORD error=GetLastError();
        Pace();
        SetLastError(error);
        ++g_presentCalls;
        const HRESULT result=g_present(graphics,window);
        if (g_logFps) {
            const DWORD nativeError=GetLastError();
            LARGE_INTEGER now{};QueryPerformanceCounter(&now);
            if (!g_sampleStart)g_sampleStart=now.QuadPart;
            else {
                // Count actual successful visible submissions. Occluded and
                // failed presentations must not masquerade as visible FPS.
                if (result==S_OK)++g_sampleFrames;
                else ++g_sampleRejected;
                const double seconds=static_cast<double>(now.QuadPart-g_sampleStart)/g_frequency.QuadPart;
                if (seconds>=5.0) {
                    g_api->Log(g_mod,SDGO_LOG_INFO,
                        "render FPS sample: fps=%.3f successful_presents=%u seconds=%.6f render_cap=%d logic_hz=%d occluded_or_failed=%u",
                        g_sampleFrames/seconds,g_sampleFrames,seconds,g_configuredCap,kNativeUpdateHz,g_sampleRejected);
                    g_sampleStart=now.QuadPart;g_sampleFrames=g_sampleRejected=0;
                }
            }
            SetLastError(nativeError);
        }
        return result;
    }

    // Capture the parsed render setting, perform the original forced-60 store
    // in ConfiguredCap, then resume after that same ten-byte instruction.
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
    if (!site || !api->Symbol(mod, "fps.app.render_sync")) return 0;
    if (!QueryPerformanceFrequency(&g_frequency) || g_frequency.QuadPart<=0)return 0;
    g_api = api;
    g_mod = mod;
    g_logFps=api->SettingInt(mod,"LogFps",0)!=0;
    g_resume = site + 10; // whole verified native instruction; never execute it
    if (!api->HookSite(mod, "fps.config.force60", reinterpret_cast<void*>(&PreserveConfiguredCap), &g_original))
        return 0;
    if (!api->HookCode(mod, "fps.app.idle", reinterpret_cast<void*>(&Idle),
                       reinterpret_cast<void**>(&g_idle)))
        return 0;
    if (!api->HookCode(mod, "fps.device.present", reinterpret_cast<void*>(&Present),
                       reinterpret_cast<void**>(&g_present)))return 0;
    using CreateTimerFn=HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES,LPCWSTR,DWORD,DWORD);
    const auto create=reinterpret_cast<CreateTimerFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"CreateWaitableTimerExW"));
    if (create)g_paceTimer=create(nullptr,nullptr,0x2,TIMER_MODIFY_STATE|SYNCHRONIZE);
    if (!g_paceTimer)g_paceTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    if (!g_paceTimer)return 0;
    api->Log(mod, SDGO_LOG_INFO, "native config.xml FPS cap enabled; applied when Framework/Timer is read");
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoFpsLimitGetSnapshotV1(SdgoFpsLimitSnapshotV1* out)
{
    if (!out || out->size<sizeof(*out) || out->version!=1)return 0;
    const SdgoFpsLimitSnapshotV1 state{sizeof(state),1,static_cast<uint32_t>(g_configuredCap),kNativeUpdateHz,
        g_presentCalls,g_waitedCalls,static_cast<uint64_t>(g_frequency.QuadPart),g_periodTicks};
    *out=state;return 1;
}
