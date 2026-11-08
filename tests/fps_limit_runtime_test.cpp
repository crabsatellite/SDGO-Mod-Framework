// Real core + production FPS Mod + native forced-60, Idle and Present fixtures.
// This never starts the game or reads a real player's config.xml.
#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "sdgo/fps_limit_observer.h"
#include "sdgo/sdgo_api.h"

namespace {
    constexpr size_t kAppBytes = 0x500;
    constexpr size_t kFpsOffset = 0x80;
    constexpr size_t kTimerPointerOffset = 0x74;
    constexpr size_t kRenderSyncOffset = 0x44A;
    constexpr size_t kTimerBytes = 0x40;
    constexpr size_t kTimerIntervalOffset = 0x18;
    constexpr size_t kFixturePageBytes = 0x1000;
    constexpr size_t kHookExpectedBytes = 16;
    constexpr size_t kMaxPresentSamples = 8;
    constexpr int kNativeDefaultFps = 60;
    constexpr BYTE kNativeSite[] = {
        0xC7, 0x87, 0x80, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, // mov dword ptr [edi+80h],60
        0xC3                                                              // ret
    };
    constexpr BYTE kNativeRenderSyncSite[] = {
        0x80, 0xBE, 0x4A, 0x04, 0x00, 0x00, 0x01 // cmp byte ptr [esi+44Ah],1
    };
    constexpr BYTE kNativeBacklogSite[] = {
        0x0F,0x2F,0xC8,                          // comiss xmm1,xmm0
        0x0F,0x87,0x06,0x00,0x00,0x00,           // ja skip_render
        0xB8,0x01,0x00,0x00,0x00,0xC3,           // render: mov eax,1; ret
        0x33,0xC0,0xC3                           // skip_render: xor eax,eax; ret
    };
    constexpr DWORD kExpectedError = 0x5A160F5;
    constexpr DWORD kNativeInitError = 0x51A7E112;
    constexpr DWORD kNativeIdleError = 0x1D1ECA11;
    constexpr DWORD kNativePresentError = 0x7E4D3E21;
    constexpr HRESULT kNativePresentResult = static_cast<HRESULT>(0x8876086Cu);
    constexpr uintptr_t kWindowSentinel = 0x2468ACE1;
    constexpr uintptr_t kFirstArgument = 0x13579BDF;
    constexpr uintptr_t kSecondArgument = 0x2468ACE0;

    __declspec(align(16)) BYTE g_code[64];
    __declspec(align(16)) BYTE g_beforeFx[512], g_afterFx[512];
    __declspec(align(16)) BYTE g_xmm[8][16];
    BYTE g_beforeApp[kAppBytes];
    BYTE* g_app = nullptr;
    BYTE* g_timer = nullptr;
    BYTE g_stateBeforeApp[kAppBytes];
    BYTE g_stateBeforeTimer[kTimerBytes];
    void* g_site = g_code;
    void* g_renderSyncSite = g_code + 16;
    void* g_backlogSite = g_code + 32;
    void* g_initEntry = nullptr;
    void* g_idleEntry = nullptr;
    void* g_presentEntry = nullptr;
    BYTE g_presentOriginal[kHookExpectedBytes];
    BYTE g_graphics[64];
    DWORD g_beforeFlags = 0, g_afterFlags = 0;
    DWORD g_afterEax = 0, g_afterEbx = 0, g_afterEcx = 0, g_afterEdx = 0;
    DWORD g_afterEsi = 0, g_afterEdi = 0, g_afterEbp = 0, g_expectedEbp = 0;
    DWORD g_initEspBefore = 0, g_initEspAfter = 0, g_initReturnEax = 0;
    uint8_t* g_nativeThis = nullptr;
    uintptr_t g_nativeFirst = 0, g_nativeSecond = 0;
    BYTE g_nativeReturn = 1, g_nativeWriteTimer = 1;
    DWORD g_idleEspBefore = 0, g_idleEspAfter = 0, g_idleCalls = 0;
    uint8_t* g_idleThis = nullptr;
    DWORD g_idleIncomingError = 0;
    DWORD g_idleObservedInterval = 0;
    BYTE g_idleObservedRenderSync = 0, g_idleObservedTimer = 0;
    DWORD g_presentEspBefore = 0, g_presentEspAfter = 0, g_presentCalls = 0;
    void* g_presentThis = nullptr;
    HWND g_presentWindow = nullptr;
    DWORD g_presentIncomingError = 0, g_presentReturnEax = 0;
    LARGE_INTEGER g_presentSamples[kMaxPresentSamples];
    struct PresentObservation {
        DWORD callsBefore, callsAfter, espBefore, espAfter;
        DWORD incomingError, outgoingError, returnEax;
        void* thisPointer;
        HWND window;
        HRESULT result;
    };
    PresentObservation g_presentObservations[kMaxPresentSamples];
    using SetLastErrorFn = void (WINAPI*)(DWORD);
    SetLastErrorFn g_setLastError = &SetLastError;
    int g_failures = 0;
    int g_expectedPresentCap = 60;
    bool g_active = false;

    void Check(bool value, const char* message)
    {
        printf("%s %s\n", value ? "ok  " : "FAIL", message);
        if (!value) ++g_failures;
    }

    std::wstring Root()
    {
        wchar_t path[32768] = {};
        GetModuleFileNameW(nullptr, path, 32768);
        std::wstring result = path;
        return result.substr(0, result.find_last_of(L"\\/"));
    }

    void Write(const std::wstring& path, const std::string& text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        Check(file != INVALID_HANDLE_VALUE &&
              WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size(), "write isolated fixture data");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    std::string HexBytes(const void* address, size_t size)
    {
        const auto* bytes = static_cast<const BYTE*>(address);
        std::string result;
        char value[4] = {};
        for (size_t i = 0; i < size; ++i) {
            sprintf_s(value, "%02X", bytes[i]);
            if (!result.empty()) result += ' ';
            result += value;
        }
        return result;
    }

    __declspec(naked) bool __cdecl NativeInitialize()
    {
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { mov dword ptr [g_nativeThis], ecx }
        __asm { mov eax, [ebp + 8] }
        __asm { mov dword ptr [g_nativeFirst], eax }
        __asm { mov eax, [ebp + 0x0C] }
        __asm { mov dword ptr [g_nativeSecond], eax }
        __asm { cmp byte ptr [g_nativeWriteTimer], 0 }
        __asm { je no_timer_write }
        __asm { mov eax, [ecx + 0x74] }
        __asm { test eax, eax }
        __asm { je no_timer_write }
        // The shipped initializer resets the simulation timer to exactly 60 Hz.
        // The production Idle preserves the native gameplay rate.
        __asm { mov dword ptr [eax + 0x18], 0x3C888889 }
        __asm { no_timer_write: }
        __asm { push 0x51A7E112 }
        __asm { call dword ptr [g_setLastError] }
        __asm { xor eax, eax }
        __asm { mov al, byte ptr [g_nativeReturn] }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret 8 }
    }

    __declspec(naked) bool __cdecl InvokeInitialize(BYTE*, uintptr_t, uintptr_t)
    {
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { mov dword ptr [g_initEspBefore], esp }
        __asm { push dword ptr [ebp + 0x10] }
        __asm { push dword ptr [ebp + 0x0C] }
        __asm { mov ecx, [ebp + 8] }
        __asm { call dword ptr [g_initEntry] }
        __asm { mov dword ptr [g_initReturnEax], eax }
        __asm { mov dword ptr [g_initEspAfter], esp }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret }
    }

    __declspec(naked) void __cdecl NativeIdle()
    {
        __asm { mov eax, fs:[0x34] }
        __asm { mov dword ptr [g_idleIncomingError], eax }
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { mov dword ptr [g_idleThis], ecx }
        __asm { inc dword ptr [g_idleCalls] }
        __asm { mov al, byte ptr [ecx + 0x44A] }
        __asm { mov byte ptr [g_idleObservedRenderSync], al }
        __asm { mov eax, [ecx + 0x74] }
        __asm { test eax, eax }
        __asm { je idle_without_timer }
        __asm { mov byte ptr [g_idleObservedTimer], 1 }
        __asm { mov eax, [eax + 0x18] }
        __asm { mov dword ptr [g_idleObservedInterval], eax }
        __asm { jmp idle_observed }
        __asm { idle_without_timer: }
        __asm { mov byte ptr [g_idleObservedTimer], 0 }
        __asm { mov dword ptr [g_idleObservedInterval], 0 }
        __asm { idle_observed: }
        __asm { push 0x1D1ECA11 }
        __asm { call dword ptr [g_setLastError] }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret }
    }

    __declspec(naked) void __cdecl InvokeIdle(BYTE*)
    {
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { mov dword ptr [g_idleEspBefore], esp }
        __asm { mov ecx, [ebp + 8] }
        __asm { call dword ptr [g_idleEntry] }
        __asm { mov dword ptr [g_idleEspAfter], esp }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret }
    }

    void SetTimer(BYTE* timer)
    {
        memcpy(g_app + kTimerPointerOffset, &timer, sizeof(timer));
    }

    float TimerInterval()
    {
        float value = 0.0f;
        memcpy(&value, g_timer + kTimerIntervalOffset, sizeof(value));
        return value;
    }

    void SetTimerInterval(float value)
    {
        memcpy(g_timer + kTimerIntervalOffset, &value, sizeof(value));
    }

    bool SameTimerGuards()
    {
        for (size_t i = 0; i < kTimerBytes; ++i) {
            if (i >= kTimerIntervalOffset && i < kTimerIntervalOffset + sizeof(float)) continue;
            if (g_timer[i] != g_stateBeforeTimer[i]) return false;
        }
        return true;
    }

    bool SameAppGuards(bool allowRenderSync)
    {
        for (size_t i = 0; i < kAppBytes; ++i) {
            if (allowRenderSync && i == kRenderSyncOffset) continue;
            if (g_app[i] != g_stateBeforeApp[i]) return false;
        }
        return true;
    }

    void RunNativeReset()
    {
        memset(g_timer, 0x6D, kTimerBytes);
        SetTimerInterval(0.25f);
        SetTimer(g_timer);
        g_app[kRenderSyncOffset] = 0;
        memcpy(g_stateBeforeApp, g_app, kAppBytes);
        memcpy(g_stateBeforeTimer, g_timer, kTimerBytes);
        g_nativeThis = nullptr;
        g_nativeFirst = 0;
        g_nativeSecond = 0;
        g_nativeReturn = 1;
        g_nativeWriteTimer = 1;
        g_initEspBefore = g_initEspAfter = g_initReturnEax = 0;

        SetLastError(kExpectedError);
        const bool result = InvokeInitialize(g_app, kFirstArgument, kSecondArgument);
        const DWORD error = GetLastError();
        Check(result && (g_initReturnEax & 0xFF) == 1,
              "native initializer simulation returns BOOL true in AL");
        Check(g_nativeThis == g_app && g_nativeFirst == kFirstArgument &&
              g_nativeSecond == kSecondArgument,
              "native initializer simulation receives this and both arguments");
        Check(g_initEspBefore == g_initEspAfter,
              "native initializer simulation uses ret 8");
        Check(error == kNativeInitError,
              "native initializer simulation preserves its last-error value");
        Check(SameAppGuards(false) && g_app[kRenderSyncOffset] == 0,
              "native initializer reset leaves CApplication guards and RenderSync unchanged");
        Check(SameTimerGuards() &&
              std::fabs(TimerInterval() - 1.0f / kNativeDefaultFps) < 0.0000001f,
              "native initializer simulation resets only the timer interval to 60 Hz");
    }

    void RunIdleOnce(const char* scenario, bool withTimer, bool protectReadOnly)
    {
        const float initialInterval = TimerInterval();
        const BYTE initialRenderSync = g_app[kRenderSyncOffset];
        const float expectedInterval =
            g_active && withTimer
                ? 1.0f / static_cast<float>(kNativeDefaultFps)
                : initialInterval;
        const BYTE expectedRenderSync = g_active && withTimer ? 0 : initialRenderSync;
        const bool writeExpected = g_active && withTimer &&
            (initialInterval != expectedInterval || initialRenderSync != 0);
        memcpy(g_stateBeforeApp, g_app, kAppBytes);
        memcpy(g_stateBeforeTimer, g_timer, kTimerBytes);
        g_idleThis = nullptr;
        g_idleIncomingError = 0;
        g_idleObservedInterval = 0;
        g_idleObservedRenderSync = 0xFF;
        g_idleObservedTimer = 0;
        g_idleEspBefore = g_idleEspAfter = 0;
        const DWORD callsBefore = g_idleCalls;

        DWORD oldApp = 0, oldTimer = 0;
        if (protectReadOnly) {
            Check(VirtualProtect(g_app, kFixturePageBytes, PAGE_READONLY, &oldApp) != 0 &&
                  VirtualProtect(g_timer, kFixturePageBytes, PAGE_READONLY, &oldTimer) != 0,
                  "protect idempotent Idle state read-only");
        }
        SetLastError(kExpectedError);
        InvokeIdle(g_app);
        const DWORD error = GetLastError();
        if (protectReadOnly) {
            DWORD ignored = 0;
            Check(VirtualProtect(g_app, kFixturePageBytes, oldApp, &ignored) != 0 &&
                  VirtualProtect(g_timer, kFixturePageBytes, oldTimer, &ignored) != 0,
                  "restore idempotent Idle fixture protection");
        }

        char message[176] = {};
#define IDLE_CHECK(condition, text) do { \
            sprintf_s(message, "%s: %s", scenario, text); \
            Check((condition), message); \
        } while (0)
        IDLE_CHECK(g_idleCalls == callsBefore + 1 && g_idleThis == g_app,
                   "original Idle is forwarded exactly once with the real this pointer");
        IDLE_CHECK(g_idleEspBefore == g_idleEspAfter,
                   "void thiscall Idle preserves its no-argument stack boundary");
        IDLE_CHECK(g_idleIncomingError == kExpectedError,
                   "original Idle receives the caller last-error value");
        IDLE_CHECK(error == kNativeIdleError,
                   "caller observes the original Idle last-error value");
        IDLE_CHECK(SameAppGuards(writeExpected),
                   "only RenderSync may change in CApplication");
        IDLE_CHECK(SameTimerGuards(), "only the timer interval may change");
        IDLE_CHECK(g_app[kRenderSyncOffset] == expectedRenderSync,
                   "RenderSync matches the active timer policy");
        IDLE_CHECK(std::fabs(TimerInterval() - expectedInterval) < 0.0000001f,
                   "timer interval retains the native 60 Hz gameplay rate");
        IDLE_CHECK(g_idleObservedTimer == (withTimer ? 1 : 0),
                   "original Idle observes the expected timer presence");
        IDLE_CHECK(g_idleObservedRenderSync == expectedRenderSync,
                   "cap and independent render state are applied before original Idle");
        if (withTimer) {
            float observed = 0.0f;
            memcpy(&observed, &g_idleObservedInterval, sizeof(observed));
            IDLE_CHECK(std::fabs(observed - expectedInterval) < 0.0000001f,
                       "original Idle observes the native 60 Hz update interval");
        }
#undef IDLE_CHECK
    }

    void RunIdleSuite()
    {
        RunNativeReset();
        RunIdleOnce("Idle after native 60 Hz reset", true, true);
        RunIdleOnce("repeated idempotent Idle", true, true);

        const int wrongCap =
            g_expectedPresentCap == 30 ? kNativeDefaultFps : 30;
        SetTimerInterval(1.0f / static_cast<float>(wrongCap));
        RunIdleOnce("Idle after corrupted native timer", true, false);

        SetTimerInterval(1.0f / static_cast<float>(kNativeDefaultFps));
        g_app[kRenderSyncOffset] = 1;
        RunIdleOnce("Idle after native RenderSync reset", true, false);

        SetTimer(nullptr);
        g_app[kRenderSyncOffset] = 0;
        RunIdleOnce("Idle without native timer", false, true);
    }

    __declspec(naked) int __cdecl RenderAdmission(float) {
        __asm { movss xmm1,dword ptr [esp+4] }
        __asm { xorps xmm0,xmm0 }
        __asm { call dword ptr [g_backlogSite] }
        __asm { ret }
    }
    void RunBacklogSuite() {
        BYTE appBefore[kAppBytes],timerBefore[kTimerBytes];
        memcpy(appBefore,g_app,sizeof(appBefore));memcpy(timerBefore,g_timer,sizeof(timerBefore));
        Check(RenderAdmission(0)==1&&RenderAdmission(-0.1f)==1,
              "native rendering remains admitted when simulation has no update debt");
        Check(RenderAdmission(0.25f)==(g_active?1:0)&&RenderAdmission(1.9f)==(g_active?1:0),
              g_active?"rendering continues while the native simulation catches up":
                       "disabled or rejected Mod preserves the original backlog render gate");
        Check(memcmp(appBefore,g_app,sizeof(appBefore))==0&&memcmp(timerBefore,g_timer,sizeof(timerBefore))==0,
              "backlog rendering never clears simulation debt, changes timer cadence or writes app state");
    }

    void __stdcall ObservePresent()
    {
        const DWORD sample = g_presentCalls++;
        if (sample < kMaxPresentSamples)
            QueryPerformanceCounter(&g_presentSamples[sample]);
    }

    __declspec(naked) HRESULT __cdecl NativePresent()
    {
        __asm { mov eax, fs:[0x34] }
        __asm { mov dword ptr [g_presentIncomingError], eax }
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { mov dword ptr [g_presentThis], ecx }
        __asm { mov eax, [ebp + 8] }
        __asm { mov dword ptr [g_presentWindow], eax }
        __asm { call ObservePresent }
        __asm { push 0x7E4D3E21 }
        __asm { call dword ptr [g_setLastError] }
        __asm { mov eax, 0x8876086C }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret 4 }
    }

    __declspec(naked) HRESULT __cdecl InvokePresent(void*, HWND)
    {
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { mov dword ptr [g_presentEspBefore], esp }
        __asm { push dword ptr [ebp + 0x0C] }
        __asm { mov ecx, [ebp + 8] }
        __asm { call dword ptr [g_presentEntry] }
        __asm { mov dword ptr [g_presentReturnEax], eax }
        __asm { mov dword ptr [g_presentEspAfter], esp }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret }
    }

    void CapturePresent(size_t index)
    {
        const HWND expectedWindow = reinterpret_cast<HWND>(kWindowSentinel);
        auto& observation = g_presentObservations[index];
        memset(&observation, 0, sizeof(observation));
        observation.callsBefore = g_presentCalls;
        g_presentThis = nullptr;
        g_presentWindow = nullptr;
        g_presentIncomingError = 0;
        g_presentReturnEax = 0;
        g_presentEspBefore = g_presentEspAfter = 0;
        SetLastError(kExpectedError);
        observation.result = InvokePresent(g_graphics, expectedWindow);
        observation.outgoingError = GetLastError();
        observation.callsAfter = g_presentCalls;
        observation.thisPointer = g_presentThis;
        observation.window = g_presentWindow;
        observation.espBefore = g_presentEspBefore;
        observation.espAfter = g_presentEspAfter;
        observation.returnEax = g_presentReturnEax;
        observation.incomingError = g_presentIncomingError;
    }

    void ValidatePresent(size_t index, const char* scenario)
    {
        const auto& observation = g_presentObservations[index];
        const HWND expectedWindow = reinterpret_cast<HWND>(kWindowSentinel);
        char message[176] = {};
#define PRESENT_CHECK(condition, text) do { \
            sprintf_s(message, "%s: %s", scenario, text); \
            Check((condition), message); \
        } while (0)
        PRESENT_CHECK(observation.callsAfter == observation.callsBefore + 1,
                      "original Present is forwarded exactly once");
        PRESENT_CHECK(observation.thisPointer == g_graphics,
                      "original Present receives the real graphics this pointer");
        PRESENT_CHECK(observation.window == expectedWindow,
                      "original Present receives the exact HWND argument");
        PRESENT_CHECK(observation.espBefore == observation.espAfter,
                      "HRESULT thiscall Present consumes its HWND with ret 4");
        PRESENT_CHECK(static_cast<HRESULT>(observation.returnEax) == kNativePresentResult,
                      "wrapper preserves the original Present HRESULT in EAX");
        PRESENT_CHECK(observation.result == kNativePresentResult,
                      "caller receives the original Present HRESULT");
        PRESENT_CHECK(observation.incomingError == kExpectedError,
                      "original Present receives the caller last-error value");
        PRESENT_CHECK(observation.outgoingError == kNativePresentError,
                      "caller observes the original Present last-error value");
#undef PRESENT_CHECK
    }

    bool PresentIntervalAtLeast(size_t first, size_t second,
                                LONGLONG period, LONGLONG tolerance)
    {
        return g_presentSamples[second].QuadPart - g_presentSamples[first].QuadPart +
               tolerance >= period;
    }

    void RunPresentSuite(bool proveNoBurst, SdgoFpsLimitGetSnapshotV1Fn snapshot)
    {
        memcpy(g_stateBeforeApp, g_app, kAppBytes);
        memcpy(g_stateBeforeTimer, g_timer, kTimerBytes);
        memset(g_presentSamples, 0, sizeof(g_presentSamples));
        memset(g_presentObservations, 0, sizeof(g_presentObservations));
        g_presentCalls = 0;

        DWORD oldApp = 0, oldTimer = 0;
        Check(VirtualProtect(g_app, kFixturePageBytes, PAGE_READONLY, &oldApp) != 0 &&
              VirtualProtect(g_timer, kFixturePageBytes, PAGE_READONLY, &oldTimer) != 0,
              "protect Present app and timer state read-only");

        const int baseCalls = g_active ? 3 : 2;
        for (int i = 0; i < baseCalls; ++i)
            CapturePresent(static_cast<size_t>(i));
        int totalCalls = baseCalls;
        if (g_active && proveNoBurst) {
            Sleep(30);
            CapturePresent(static_cast<size_t>(totalCalls++));
            CapturePresent(static_cast<size_t>(totalCalls++));
        }
        for (int i = 0; i < baseCalls; ++i) {
            char scenario[64] = {};
            sprintf_s(scenario, "Present call %d", i + 1);
            ValidatePresent(static_cast<size_t>(i), scenario);
        }
        if (g_active && proveNoBurst) {
            ValidatePresent(3, "Present after deliberate stall");
            ValidatePresent(4, "Present immediately after stalled frame");
        }

        DWORD ignored = 0;
        Check(VirtualProtect(g_app, kFixturePageBytes, oldApp, &ignored) != 0 &&
              VirtualProtect(g_timer, kFixturePageBytes, oldTimer, &ignored) != 0,
              "restore Present fixture protection");
        Check(memcmp(g_app, g_stateBeforeApp, kAppBytes) == 0 &&
              memcmp(g_timer, g_stateBeforeTimer, kTimerBytes) == 0,
              "Present pacing leaves all app and timer bytes unchanged");

        if (!g_active) return;
        LARGE_INTEGER frequency = {};
        Check(QueryPerformanceFrequency(&frequency) != 0 && frequency.QuadPart > 0,
              "query Present pacing clock frequency");
        if (frequency.QuadPart <= 0) return;
        const LONGLONG period =
            (frequency.QuadPart + g_expectedPresentCap - 1) / g_expectedPresentCap;
        const LONGLONG tolerance =
            frequency.QuadPart / 10000 > 0 ? frequency.QuadPart / 10000 : 1;
        for (DWORD i = 1; i < static_cast<DWORD>(baseCalls); ++i) {
            char message[128] = {};
            sprintf_s(message,
                      "Present interval %lu is not earlier than the configured period",
                      static_cast<unsigned long>(i));
            Check(PresentIntervalAtLeast(i - 1, i, period, tolerance), message);
        }
        if (proveNoBurst) {
            Check(g_presentCalls >= 5,
                  "160 FPS pacing records at least five native Present calls");
            Check(PresentIntervalAtLeast(2, 3, period * 3, tolerance),
                  "deliberate stall spans multiple configured Present periods");
            Check(PresentIntervalAtLeast(g_presentCalls - 2, g_presentCalls - 1,
                                         period, tolerance),
                  "post-stall Present schedules from actual start without a catch-up burst");
        }

        SdgoFpsLimitSnapshotV1 state = {};
        state.size = sizeof(state);
        state.version = 1;
        Check(snapshot && snapshot(&state) != 0,
              "read production FPS observer snapshot");
        if (!snapshot) return;
        Check(state.size == sizeof(state) && state.version == 1,
              "observer returns the requested snapshot ABI");
        Check(state.render_cap == static_cast<uint32_t>(g_expectedPresentCap),
              "observer reports the configured Present cap");
        Check(state.update_hz == static_cast<uint32_t>(kNativeDefaultFps),
              "observer reports native 60 Hz independently of the render cap");
        Check(state.qpc_frequency == static_cast<uint64_t>(frequency.QuadPart) &&
              state.period_ticks == static_cast<uint64_t>(period),
              "observer period is exactly ceil(QPC frequency / Present cap)");
        Check(state.present_calls == g_presentCalls,
              "observer counts every forwarded Present wrapper call");
        Check(state.waited_calls > 0 && state.waited_calls < state.present_calls,
              "active Present cap records real paced waits without delaying every call");
    }

    void __stdcall Inspect()
    {
        const DWORD error = GetLastError();
        int observed = 0;
        memcpy(&observed, g_app + kFpsOffset, sizeof(observed));
        Check(observed == kNativeDefaultFps,
              "native forced-60 gameplay field is preserved while capturing the render cap");

        bool guards = true;
        for (size_t i = 0; i < kAppBytes; ++i) {
            if (i >= kFpsOffset && i < kFpsOffset + sizeof(int)) continue;
            if (g_app[i] != g_beforeApp[i]) guards = false;
        }
        Check(guards, "only CApplication updatefps is changed");
        Check(g_afterEax == 0x11111111 && g_afterEbx == 0x22222222 &&
              g_afterEcx == 0x33333333 && g_afterEdx == 0x44444444 &&
              g_afterEsi == 0x66666666 &&
              g_afterEdi == reinterpret_cast<uintptr_t>(g_app) &&
              g_afterEbp == g_expectedEbp,
              "site detour preserves general registers");
        Check((g_afterFlags & 0xCD5) == (g_beforeFlags & 0xCD5),
              "site detour preserves arithmetic, direction and carry flags");
        Check(!memcmp(g_beforeFx, g_afterFx, sizeof(g_beforeFx)),
              "site detour preserves x87, MXCSR and all SIMD state");
        Check(error == kExpectedError, "site detour preserves Win32 last-error state");
    }

    __declspec(naked) void __cdecl Invoke(BYTE*)
    {
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { push ebx }
        __asm { push esi }
        __asm { push edi }
        __asm { fninit }
        __asm { fld1 }
        __asm { fldpi }
        __asm { movdqu xmm0, [g_xmm] }
        __asm { movdqu xmm1, [g_xmm + 16] }
        __asm { movdqu xmm2, [g_xmm + 32] }
        __asm { movdqu xmm3, [g_xmm + 48] }
        __asm { movdqu xmm4, [g_xmm + 64] }
        __asm { movdqu xmm5, [g_xmm + 80] }
        __asm { movdqu xmm6, [g_xmm + 96] }
        __asm { movdqu xmm7, [g_xmm + 112] }
        __asm { fxsave [g_beforeFx] }
        __asm { mov eax, 0x11111111 }
        __asm { mov ebx, 0x22222222 }
        __asm { mov ecx, 0x33333333 }
        __asm { mov edx, 0x44444444 }
        __asm { mov esi, 0x66666666 }
        __asm { mov edi, [ebp + 8] }
        __asm { mov [g_expectedEbp], ebp }
        __asm { stc }
        __asm { std }
        __asm { pushfd }
        __asm { pop [g_beforeFlags] }
        __asm { call dword ptr [g_site] }
        __asm { fxsave [g_afterFx] }
        __asm { pushfd }
        __asm { pop [g_afterFlags] }
        __asm { mov [g_afterEax], eax }
        __asm { mov [g_afterEbx], ebx }
        __asm { mov [g_afterEcx], ecx }
        __asm { mov [g_afterEdx], edx }
        __asm { mov [g_afterEsi], esi }
        __asm { mov [g_afterEdi], edi }
        __asm { mov [g_afterEbp], ebp }
        __asm { cld }
        __asm { fninit }
        __asm { call Inspect }
        __asm { lea esp, [ebp - 12] }
        __asm { pop edi }
        __asm { pop esi }
        __asm { pop ebx }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret }
    }

    int Requested(const std::wstring& mode)
    {
        if (mode == L"160") return 160;
        if (mode == L"120") return 120;
        if (mode == L"60") return 60;
        if (mode == L"disabled" || mode == L"badbytes" ||
            mode == L"wrongkind") return 160;
        if (mode == L"min") return 1;
        if (mode == L"max") return 1000;
        if (mode == L"zero") return 0;
        if (mode == L"over") return 1001;
        if (mode == L"negative") return -1;
        return 160;
    }

    bool ModShouldLoad(const std::wstring& mode)
    {
        return mode != L"disabled" && mode != L"badbytes" && mode != L"wrongkind";
    }
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring mode = argc > 1 ? argv[1] : L"160";
    const int requested = Requested(mode);
    const bool active = ModShouldLoad(mode);
    g_active = active;
    g_expectedPresentCap =
        active && requested >= 1 && requested <= 1000 ? requested : 60;

    g_app = static_cast<BYTE*>(VirtualAlloc(
        nullptr, kFixturePageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_timer = static_cast<BYTE*>(VirtualAlloc(
        nullptr, kFixturePageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(g_app != nullptr && g_timer != nullptr,
          "reserve isolated CApplication and timer fixture pages");
    if (!g_app || !g_timer) return 2;
    for (unsigned i = 0; i < sizeof(g_xmm); ++i)
        reinterpret_cast<BYTE*>(g_xmm)[i] = static_cast<BYTE>(i ^ 0xA5);
    memset(g_beforeFx, 0, sizeof(g_beforeFx));
    memset(g_afterFx, 0, sizeof(g_afterFx));
    memset(g_graphics, 0x7B, sizeof(g_graphics));
    memset(g_app, 0xA5, kAppBytes);
    memcpy(g_app + kFpsOffset, &requested, sizeof(requested));
    memcpy(g_beforeApp, g_app, kAppBytes);

    DWORD old = 0;
    Check(VirtualProtect(g_code, sizeof(g_code), PAGE_EXECUTE_READWRITE, &old) != 0,
          "reserve owned executable site fixture");
    memset(g_code, 0xCC, sizeof(g_code));
    memcpy(g_code, kNativeSite, sizeof(kNativeSite));
    memcpy(g_renderSyncSite, kNativeRenderSyncSite, sizeof(kNativeRenderSyncSite));
    memcpy(g_backlogSite,kNativeBacklogSite,sizeof(kNativeBacklogSite));
    FlushInstructionCache(GetCurrentProcess(), g_code, sizeof(g_code));

    const auto root = Root();
    auto* host = GetModuleHandleW(nullptr);
    auto* base = reinterpret_cast<BYTE*>(host);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_initEntry = reinterpret_cast<void*>(&NativeInitialize);
    g_idleEntry = reinterpret_cast<void*>(&NativeIdle);
    g_presentEntry = reinterpret_cast<void*>(&NativePresent);
    memcpy(g_presentOriginal, g_presentEntry, sizeof(g_presentOriginal));
    wchar_t key[160] = {};
    swprintf_s(key, L"fps_limit_runtime_test-%08X-%08X",
               nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    const auto idleExpected = HexBytes(g_idleEntry, kHookExpectedBytes);
    const auto presentExpected = HexBytes(g_presentEntry, kHookExpectedBytes);
    char symbols[1536] = {};
    sprintf_s(symbols,
              "[host]\n"
              "timestamp=0x%08X\n"
              "size_of_image=0x%08X\n"
              "[fps.config.force60]\n"
              "kind=%s\n"
              "rva=0x%08X\n"
              "expect=%s\n"
              "[fps.app.idle]\n"
              "kind=code\n"
              "rva=0x%08X\n"
              "expect=%s\n"
              "[fps.app.render_sync]\n"
              "kind=site\n"
              "rva=0x%08X\n"
               "expect=80 BE 4A 04 00 00 01\n"
               "[fps.app.render_backlog]\n"
               "kind=site\n"
               "rva=0x%08X\n"
               "expect=0F 2F C8 0F 87 06 00 00 00\n"
               "[fps.device.present]\n"
               "kind=code\n"
               "rva=0x%08X\n"
               "expect=%s\n",
              nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
              mode == L"wrongkind" ? "code" : "site",
              static_cast<unsigned>(g_code - base),
              mode == L"badbytes"
                  ? "CC CC CC CC CC CC CC CC CC CC"
                  : "C7 87 80 00 00 00 3C 00 00 00",
              static_cast<unsigned>(static_cast<BYTE*>(g_idleEntry) - base),
              idleExpected.c_str(),
               static_cast<unsigned>(static_cast<BYTE*>(g_renderSyncSite) - base),
               static_cast<unsigned>(static_cast<BYTE*>(g_backlogSite) - base),
               static_cast<unsigned>(static_cast<BYTE*>(g_presentEntry) - base),
               presentExpected.c_str());
    CreateDirectoryW((root + L"\\sdgo\\symbols").c_str(), nullptr);
    Write(root + L"\\sdgo\\symbols\\" + key + L".ini", symbols);

    HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
    auto init = core ? reinterpret_cast<SdgoCoreInitFn>(
                           GetProcAddress(core, "SdgoCoreInit")) : nullptr;
    Check(init != nullptr, "load the real core");
    if (!init) return 2;
    SdgoBoot boot = {sizeof(boot), "fps_limit_test", host};
    Check(init(&boot) != 0, "initialize real core and configured FPS Mod");
    Check((memcmp(g_presentEntry, g_presentOriginal, sizeof(g_presentOriginal)) != 0) == active,
          active ? "production FPS Mod installs the native Present hook"
                 : "disabled or rejected FPS Mod leaves native Present code unchanged");
    HMODULE fpsDll = GetModuleHandleW(L"fps_limit.dll");
    auto snapshot = fpsDll ? reinterpret_cast<SdgoFpsLimitGetSnapshotV1Fn>(
                                 GetProcAddress(fpsDll, SDGO_FPS_LIMIT_SNAPSHOT_EXPORT))
                           : nullptr;
    if (active)
        Check(snapshot != nullptr, "load the production FPS observer export");

    SetLastError(kExpectedError);
    Invoke(g_app);
    RunIdleSuite();
    RunBacklogSuite();
    RunPresentSuite(active && mode == L"160", snapshot);
    puts(g_failures ? "FPS LIMIT RUNTIME CHECKS FAILED"
                    : "ALL FPS LIMIT RUNTIME CHECKS PASSED");
    return g_failures ? 1 : 0;
}
