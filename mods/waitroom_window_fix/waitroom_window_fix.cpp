// The original wait-room service forwards WM_GETICON through its +4 view
// before that view is attached. Returning its usual false result lets the
// application's existing window procedure use its normal default path.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/waitroom_window_observer.h"

namespace {
static_assert(sizeof(void*) == 4, "the admitted wait-room message ABI is Win32");
using Dispatch = std::uint8_t(__thiscall*)(void*, HWND, UINT, WPARAM, LPARAM);
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Dispatch g_original = nullptr;
volatile LONG g_guarded = 0, g_forwarded = 0, g_lastThread = 0;

bool MissingView(const void* service) {
    if (!service) return false;
    __try {
        const void* view = nullptr;
        std::memcpy(&view, static_cast<const std::uint8_t*>(service) + 4, sizeof(view));
        return view == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::uint8_t __fastcall WaitRoomMessage(void* service, void*, HWND window,
                                      UINT message, WPARAM wparam, LPARAM lparam) {
    const DWORD error = GetLastError();
    if (message == WM_GETICON && MissingView(service)) {
        const LONG count = InterlockedIncrement(&g_guarded);
        InterlockedExchange(&g_lastThread, static_cast<LONG>(GetCurrentThreadId()));
        if (count <= 4) g_api->Log(g_mod, SDGO_LOG_INFO,
            "wait-room icon query used native default processing before view attachment; count=%ld", count);
        SetLastError(error);
        return 0;
    }
    InterlockedIncrement(&g_forwarded);
    SetLastError(error);
    return g_original(service, window, message, wparam, lparam);
}

bool OriginalBody(const std::uint8_t* bytes) {
    if (!bytes) return false;
    std::uint32_t hash = 2166136261u;
    __try {
        for (std::size_t i = 0; i < 79; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return hash == 0x2db51eaau;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoWaitroomWindowSnapshotV1(
    struct SdgoWaitroomWindowSnapshotV1* out) {
    if (!out || out->size != sizeof(*out) || out->version != 1 || !g_original) return 0;
    *out = {sizeof(*out), 1, GetCurrentProcessId(),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_guarded, 0, 0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_forwarded, 0, 0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_lastThread, 0, 0))};
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api || api->size < offsetof(SdgoApi, HookCode) + sizeof(api->HookCode) ||
        !api->Symbol || !api->HookCode || !api->Log || g_original) return 0;
    auto* body = static_cast<const std::uint8_t*>(api->Symbol(mod, "window.waitroom_message"));
    if (!OriginalBody(body)) {
        api->Log(mod, SDGO_LOG_ERROR, "wait-room message body differs; no lifecycle hook installed");
        return 0;
    }
    g_api = api; g_mod = mod;
    if (!api->HookCode(mod, "window.waitroom_message", reinterpret_cast<void*>(&WaitRoomMessage),
                       reinterpret_cast<void**>(&g_original))) return 0;
    api->Log(mod, SDGO_LOG_INFO, "wait-room icon queries retain default processing until the native view is attached");
    return 1;
}
