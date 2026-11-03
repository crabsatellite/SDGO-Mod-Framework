// Complete the shipped already-online recovery before returning login failure.
// No credentials, database connection, timer, or alternate authentication path.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include "sdgo/sdgo_api.h"
#include "retry_policy.h"

namespace {
const SdgoApi* g_api;
SdgoMod* g_mod;
using RequestFn = int(__thiscall*)(void*);
using BroadcastFn = int(__thiscall*)(void*, void*, int, const void*, int);
RequestFn g_request;
BroadcastFn g_broadcast;
void* g_agents;
void** g_network;
void* g_continue;
void* g_authenticate;
const DWORD g_mxcsr = 0x1f80;
volatile LONG g_retries = 0;

struct Frame {
    void* request;
    sdgo::login_recovery::Attempt attempt;
};
thread_local Frame* g_frame = nullptr;

int __fastcall Request(void* self, void*) {
    Frame frame{self, {}};
    Frame* previous = g_frame;
    g_frame = &frame;
    int result = 1000;
    __try { result = g_request(self); }
    __finally { g_frame = previous; }
    return result;
}

#pragma pack(push, 1)
struct Logout {
    uint16_t opcode = 0x1f4b;
    int account;
    uint16_t reason = 1006;
};
#pragma pack(pop)
static_assert(sizeof(Logout) == 8);

// The stock failed-login path sends this same notification after its reply.
// Send it before repeating authentication so its account-based old-session
// notification cannot follow and evict the newly admitted Agent connection.
bool NotifyOldSession(int account) {
    void* network = nullptr;
    __try { network = *g_network; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!network) return false;
    const Logout packet{0x1f4b, account, 1006};
    return g_broadcast(g_agents, network, 90, &packet, sizeof(packet)) != 0;
}

int __stdcall AfterCleanup(void* request, int account) {
    if (!g_frame) return 0;
    const bool retry = g_frame->attempt.AfterNativeCleanup(
        request == g_frame->request, account,
        [account] { return NotifyOldSession(account); });
    if (retry) {
        InterlockedIncrement(&g_retries);
        g_api->Log(g_mod, SDGO_LOG_INFO,
            "native old-session cleanup completed; retrying authentication once in the same request");
    }
    return retry ? 1 : 0;
}

__declspec(naked) void CleanupSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0fffffff0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_mxcsr] }
    __asm { cld }
    __asm { push dword ptr [ebp-5ch] }
    __asm { push dword ptr [ebp-5ach] }
    __asm { call AfterCleanup }
    // The replaced stock instructions do not consume EAX or prior flags.
    __asm { mov edx, [esp+200h] }
    __asm { mov [edx+1ch], eax }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { test eax, eax }
    __asm { jnz retry_authentication }
    __asm { jmp dword ptr [g_continue] }
retry_authentication:
    __asm { jmp dword ptr [g_authenticate] }
}
}

extern "C" __declspec(dllexport) unsigned __cdecl SdgoLoginRecoveryRetryCountV1() {
    return static_cast<unsigned>(InterlockedCompareExchange(&g_retries, 0, 0));
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite)) return 0;
    g_api = api; g_mod = mod;
    g_broadcast = reinterpret_cast<BroadcastFn>(api->Symbol(mod, "login_recovery.broadcast"));
    g_agents = api->Symbol(mod, "login_recovery.agents");
    g_network = static_cast<void**>(api->Symbol(mod, "login_recovery.network"));
    g_authenticate = api->Symbol(mod, "login_recovery.authenticate");
    if (!g_broadcast || !g_agents || !g_network || !g_authenticate) return 0;
    if (!api->HookCode(mod, "login_recovery.request", reinterpret_cast<void*>(&Request),
                       reinterpret_cast<void**>(&g_request)) ||
        !api->HookSite(mod, "login_recovery.after_cleanup", reinterpret_cast<void*>(&CleanupSite),
                       &g_continue)) return 0;
    api->Log(mod, SDGO_LOG_INFO,
        "native authenticated old-session recovery active; one retry after native cleanup and Agent notification");
    return 1;
}
