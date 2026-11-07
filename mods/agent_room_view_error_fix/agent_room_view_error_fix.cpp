// The stock Agent recognizes BattleServer reply 0x0880, resolves its client
// route from packet+2, then forwards only four bytes. BattleServer supplied an
// eight-byte reply: opcode, destination pilot and the native error word. Keep
// that exact reply intact at the one verified Agent forwarding call.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"

#if !defined(_M_IX86)
#error agent_room_view_error_fix requires the 32-bit AgentServer ABI
#endif

namespace
{
    constexpr std::uint16_t kRoomViewErrorOpcode = 0x0880;
    constexpr std::uint32_t kTruncatedBytes = 4;
    constexpr std::uint32_t kCompleteBytes = 8;
    constexpr DWORD kDefaultMxcsr = 0x1F80;

    void* g_sendClientOriginal = nullptr;

    void CorrectLengthUnsafe(std::uint32_t* nativeStack) noexcept
    {
        if (!nativeStack || nativeStack[2] != kTruncatedBytes) return;
        const auto* packet = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(nativeStack[1]));
        if (!packet) return;
        std::uint16_t opcode = 0;
        std::memcpy(&opcode, packet, sizeof(opcode));
        if (opcode == kRoomViewErrorOpcode) nativeStack[2] = kCompleteBytes;
    }

    void __stdcall CorrectLength(std::uint32_t* nativeStack) noexcept
    {
        const DWORD error = GetLastError();
        __try {
            CorrectLengthUnsafe(nativeStack);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        SetLastError(error);
    }

    // HookSite enters before the original CALL with the four native
    // SendToClient arguments already on the stack:
    //   [0] client route, [1] packet, [2] byte length, [3] flags.
    // The trampoline replays that CALL exactly once. Only the length word is
    // corrected; ECX, every argument, flags, integer registers and FP/SIMD
    // state reach the original call unchanged.
    __declspec(naked) void SendClientSite()
    {
        __asm { pushfd }
        __asm { pushad }
        __asm { mov eax, esp }
        __asm { sub esp, 0x210 }
        __asm { and esp, 0xfffffff0 }
        __asm { mov [esp + 0x200], eax }
        __asm { fxsave [esp] }
        __asm { fninit }
        __asm { ldmxcsr dword ptr [kDefaultMxcsr] }
        __asm { cld }
        __asm { mov eax, [esp + 0x200] }
        __asm { mov eax, [eax + 0x0C] }
        __asm { add eax, 4 }
        __asm { push eax }
        __asm { call CorrectLength }
        __asm { fxrstor [esp] }
        __asm { mov esp, [esp + 0x200] }
        __asm { popad }
        __asm { popfd }
        __asm { jmp dword ptr [g_sendClientOriginal] }
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->host_key ||
        std::strcmp(api->host_key, "AgentServer-548A86BC-000D7000") != 0 ||
        !api->HookSite || !api->Log)
        return 0;
    if (!api->HookSite(
            mod, "agent_room_view_error_fix.send_client_call",
            reinterpret_cast<void*>(&SendClientSite),
            &g_sendClientOriginal))
        return 0;
    api->Log(
        mod, SDGO_LOG_INFO,
        "native room-view error replies preserve their complete 8-byte payload");
    return 1;
}
