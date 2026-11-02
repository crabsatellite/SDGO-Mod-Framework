// Let a verified accepted offline friend open the game's existing conversation UI.
// Message transport, errors and any server-side delivery policy remain native.
#include <windows.h>
#include <cstddef>
#include "sdgo/sdgo_api.h"
#include "chat_open_policy.h"

namespace
{
    void* g_originalGate = nullptr;

    // Native site 0x00964549 follows:
    //   cmp dword ptr [eax+1Ch], 0
    //   je  0x00964553
    // EAX is the selected FriendTree row. The earlier native selection guard has
    // already returned when the tree has no selected payload. Preserve the
    // original flags unless this is a complete accepted offline-friend record.
    __declspec(naked) void OfflineChatGate()
    {
        __asm {
            pushfd
            cmp dword ptr [eax], 0
            je preserve_native
            cmp byte ptr [eax + 0Ah], 0
            je preserve_native
            cmp byte ptr [eax + 1Bh], 7
            jne preserve_native
            cmp dword ptr [eax + 1Ch], 0
            jne preserve_native
            and dword ptr [esp], 0FFFFFFBFh
        preserve_native:
            popfd
            jmp dword ptr [g_originalGate]
        }
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->Symbol || !api->HookSite || !api->Log) {
        return 0;
    }

    // This independent layout receipt pins the preceding presence comparison.
    // The hook symbol itself starts at the conditional branch so its trampoline
    // can replay the original JE after the detour selectively clears ZF.
    if (!api->Symbol(mod, friend_list_client_fix::kLayoutSymbol) ||
        !api->HookSite(mod, friend_list_client_fix::kGateSymbol,
                       reinterpret_cast<void*>(&OfflineChatGate), &g_originalGate)) {
        return 0;
    }

    api->Log(mod, SDGO_LOG_INFO,
             "accepted offline friends may open the native conversation UI; "
             "message send, rejection and delivery remain native");
    return 1;
}
