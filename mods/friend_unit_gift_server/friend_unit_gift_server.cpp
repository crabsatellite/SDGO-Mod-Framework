#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/sdgo_api.h"
#include "server_database.h"

namespace {

using namespace sdgo::friend_unit_gift;

constexpr char kAgentHost[] = "AgentServer-548A86BC-000D7000";
constexpr char kWorldHost[] = "WorldServer-548A86E3-0010C000";
const DWORD kDefaultMxcsr = 0x1F80;

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;

enum class AgentCandidate : std::uint8_t {
    none,
    native_probe,
    expanded,
};

struct AgentThreadState {
    AgentCandidate candidate = AgentCandidate::none;
    std::uint8_t* packet = nullptr;
    std::size_t actual_size = 0;
    std::array<std::uint8_t, kCarrierBytes> expanded{};
};

thread_local AgentThreadState g_agent_state;
void* g_agent_capture_original = nullptr;
void* g_agent_send_original = nullptr;

using WorldHandler = int(__thiscall*)(void* request);
using WorldSend = int(__thiscall*)(
    void* network,
    std::uint32_t destination,
    const std::uint8_t* packet,
    int size,
    std::uint32_t flags);
using WorldRouteLookup = int(__thiscall*)(
    void* registry,
    int route,
    void* record);

WorldHandler g_world_original = nullptr;
WorldSend g_world_send = nullptr;
WorldRouteLookup g_world_route_lookup = nullptr;
void** g_world_network_slot = nullptr;
void* g_world_sip_registry = nullptr;
LONG g_world_length_receipt = 0;
LONG g_world_custom_replies = 0;
LONG g_world_unauthorized_routes = 0;
ServerDatabase g_database;
std::mutex g_database_gate;
std::atomic<bool> g_database_binding_proven{false};
std::atomic<bool> g_database_ready{false};

struct LastErrorGuard {
    DWORD value = GetLastError();
    ~LastErrorGuard() { SetLastError(value); }
};

bool CandidateExpanded(
    const std::uint8_t* packet,
    std::size_t actual_size) noexcept {
    if (!packet || actual_size != kCarrierBytes) return false;
    __try {
        if (wire::Read16(packet) != kNativeRequestOpcode ||
            wire::Read32(packet + 2) != 0 ||
            wire::Read32(packet + 6) != 1)
            return false;
        auto copy = g_agent_state.expanded;
        std::memcpy(copy.data(), packet, copy.size());
        wire::Write32(copy.data() + 2, 1);
        return MarkAgentValidated(copy.data(), copy.size());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CaptureAgentRequestUnsafe(
    std::uint8_t* packet,
    std::uint32_t actual_size) noexcept {
    g_agent_state.candidate = AgentCandidate::none;
    g_agent_state.packet = nullptr;
    g_agent_state.actual_size = 0;
    __try {
        if (IsProbeBeforeAuthentication(packet, actual_size)) {
            g_agent_state.candidate = AgentCandidate::native_probe;
        } else if (CandidateExpanded(packet, actual_size)) {
            g_agent_state.candidate = AgentCandidate::expanded;
        } else {
            return;
        }
        g_agent_state.packet = packet;
        g_agent_state.actual_size = actual_size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_agent_state = {};
    }
}

void __stdcall CaptureAgentRequest(
    std::uint8_t* packet,
    std::uint32_t actual_size) noexcept {
    const DWORD error = GetLastError();
    CaptureAgentRequestUnsafe(packet, actual_size);
    SetLastError(error);
}

void PrepareAgentSendUnsafe(
    std::uint32_t* native_stack) noexcept {
    if (!native_stack) return;
    __try {
        auto* packet = reinterpret_cast<std::uint8_t*>(
            static_cast<std::uintptr_t>(native_stack[1]));
        const auto native_length = native_stack[2];
        const auto candidate = g_agent_state.candidate;
        if (!packet || packet != g_agent_state.packet ||
            native_length != kNativeProbeBytes) {
            g_agent_state = {};
            return;
        }
        if (candidate == AgentCandidate::native_probe) {
            const auto authenticated_actor =
                wire::Read32(packet + 2);
            if (!authenticated_actor) {
                g_agent_state = {};
                return;
            }
            Request hello;
            hello.command = Command::hello;
            if (!EncodeRequest(
                    hello, &g_agent_state.expanded, true)) {
                g_agent_state = {};
                return;
            }
            wire::Write32(
                g_agent_state.expanded.data() + 2,
                authenticated_actor);
            if (!MarkAgentValidated(
                    g_agent_state.expanded.data(),
                    g_agent_state.expanded.size())) {
                g_agent_state = {};
                return;
            }
            native_stack[1] = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(
                    g_agent_state.expanded.data()));
            native_stack[2] =
                static_cast<std::uint32_t>(kCarrierBytes);
        } else if (candidate == AgentCandidate::expanded &&
                   g_agent_state.actual_size == kCarrierBytes &&
                   MarkAgentValidated(packet, kCarrierBytes)) {
            native_stack[2] =
                static_cast<std::uint32_t>(kCarrierBytes);
        }
        g_agent_state.candidate = AgentCandidate::none;
        g_agent_state.packet = nullptr;
        g_agent_state.actual_size = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_agent_state = {};
    }
}

void __stdcall PrepareAgentSend(
    std::uint32_t* native_stack) noexcept {
    const DWORD error = GetLastError();
    PrepareAgentSendUnsafe(native_stack);
    SetLastError(error);
}

// HookSite trampolines replay the intercepted native instruction/call. These
// bridges preserve flags, all integer registers, x87/SSE state, stack layout
// and the caller's last-error value before jumping to that trampoline.
__declspec(naked) void AgentCaptureSite() {
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
    __asm { mov eax, [eax + 0x1C] }
    __asm { push dword ptr [ebp + 0x18] }
    __asm { push eax }
    __asm { call CaptureAgentRequest }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp + 0x200] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_agent_capture_original] }
}

__declspec(naked) void AgentSendSite() {
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
    __asm { call PrepareAgentSend }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp + 0x200] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_agent_send_original] }
}

struct WorldRequestView {
    std::uint32_t route = 0;
    std::uint32_t logical_size = 0;
    std::uint8_t* packet = nullptr;
    void* member_database = nullptr;
    void* game_database = nullptr;
};

bool ReadWorldRequest(
    void* self,
    WorldRequestView* output) noexcept {
    if (!self || !output) return false;
    __try {
        const auto* bytes =
            static_cast<const std::uint8_t*>(self);
        std::memcpy(
            &output->route, bytes + 0x00,
            sizeof(output->route));
        std::memcpy(
            &output->logical_size, bytes + 0x04,
            sizeof(output->logical_size));
        std::memcpy(
            &output->packet, bytes + 0x08,
            sizeof(output->packet));
        std::memcpy(
            &output->member_database, bytes + 0x10,
            sizeof(output->member_database));
        std::memcpy(
            &output->game_database, bytes + 0x14,
            sizeof(output->game_database));
        // route is not supplied by the client frame. The native receive
        // callback copies it through the request queue into CRequest. Its
        // registered server role is verified separately before custom use.
        return output->route > 0 &&
               output->route <= 0x7FFFFFFFu &&
               output->packet != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = {};
        return false;
    }
}

bool SendWorldReply(
    const WorldRequestView& view,
    const std::array<std::uint8_t, kCarrierBytes>& bytes) noexcept {
    if (!g_world_send || !g_world_network_slot) return false;
    __try {
        void* network = nullptr;
        std::memcpy(
            &network, g_world_network_slot, sizeof(network));
        return network &&
               g_world_send(
                   network, view.route, bytes.data(),
                   static_cast<int>(bytes.size()), 0) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsRegisteredAgentRoute(std::uint32_t route) noexcept {
    if (!route || route > 0x7FFFFFFFu ||
        !g_world_route_lookup || !g_world_sip_registry)
        return false;
    // Native sip.txt records are 0x44 bytes. The connection handshake writes
    // the route at +0 and the parser writes the configured server role at
    // +0x40 (AGENT == 1).
    std::array<std::uint8_t, 0x44> record{};
    __try {
        if (!g_world_route_lookup(
                g_world_sip_registry,
                static_cast<int>(route),
                record.data()))
            return false;
        std::uint32_t registered_route = 0;
        std::int32_t server_role = 0;
        std::memcpy(
            &registered_route, record.data(),
            sizeof(registered_route));
        std::memcpy(
            &server_role, record.data() + 0x40,
            sizeof(server_role));
        return registered_route == route && server_role == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class BindingProof : std::uint8_t {
    failed,
    proven_now,
    already_proven,
};

BindingProof ProveDatabaseBindingReadOnly(
    const NativeDatabaseSessions& sessions) noexcept {
    if (g_database_binding_proven.load(std::memory_order_acquire))
        return BindingProof::already_proven;
    std::lock_guard<std::mutex> lock(g_database_gate);
    if (g_database_binding_proven.load(std::memory_order_relaxed))
        return BindingProof::already_proven;
    std::wstring error;
    if (!g_database.ValidateSessionsReadOnly(sessions, &error)) {
        if (g_api && g_api->Log)
            g_api->Log(
                g_mod, SDGO_LOG_ERROR,
                "friend unit gift native Member/Game session read-only "
                "validation failed; no database operation enabled");
        return BindingProof::failed;
    }
    g_database_binding_proven.store(true, std::memory_order_release);
    if (g_api && g_api->Log)
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "friend unit gift native database sessions proven read-only; "
            "owned policy remains unopened until the next request");
    return BindingProof::proven_now;
}

bool EnsureDatabasePolicy(
    const NativeDatabaseSessions& sessions) noexcept {
    if (g_database_ready.load(std::memory_order_acquire))
        return true;
    if (!g_database_binding_proven.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> lock(g_database_gate);
    if (g_database_ready.load(std::memory_order_relaxed))
        return true;
    std::wstring error;
    if (!g_database.EnsureOwnedPolicy(sessions, &error)) {
        if (g_api && g_api->Log)
            g_api->Log(
                g_mod, SDGO_LOG_ERROR,
                "friend unit gift owned policy unavailable after verified "
                "native sessions; no gift request executed");
        return false;
    }
    g_database_ready.store(true, std::memory_order_release);
    if (g_api && g_api->Log)
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "friend unit gift native database sessions and owned policy "
            "verified");
    return true;
}

int __fastcall WorldGiftHandler(
    void* self,
    void*) noexcept {
    LastErrorGuard error;
    WorldRequestView view;
    if (!ReadWorldRequest(self, &view) ||
        view.logical_size != kCarrierBytes) {
        return g_world_original(self);
    }

    Request request;
    std::uint32_t actor = 0;
    if (!DecodeRequest(
            view.packet, view.logical_size,
            &request, &actor, true)) {
        return g_world_original(self);
    }
    if (!IsRegisteredAgentRoute(view.route)) {
        if (g_api && g_api->Log &&
            InterlockedIncrement(&g_world_unauthorized_routes) <= 5) {
            g_api->Log(
                g_mod, SDGO_LOG_ERROR,
                "rejected authenticated gift carrier from a route that is "
                "not registered as an AGENT peer");
        }
        return 0;
    }

    if (InterlockedCompareExchange(
            &g_world_length_receipt, 1, 0) == 0 &&
        g_api && g_api->Log) {
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "authenticated gift carrier received with runtime logical "
            "length 0x%X; read-only protocol enabled",
            static_cast<unsigned>(view.logical_size));
    }

    const NativeDatabaseSessions sessions{
        view.member_database, view.game_database};
    Reply reply;
    reply.command = request.command;
    reply.flags = request.flags;
    reply.request_id = request.request_id;
    reply.gift_id = request.gift_id;
    reply.unit_uid = request.unit_uid;
    reply.receiver_id = request.receiver_id;
    reply.page = request.page;
    reply.server_epoch = ServerRuntimeEpoch();
    reply.capabilities =
        capability_protocol |
        capability_expanded_requests |
        capability_agent_validated;

    bool stop_after_binding_probe = false;
    if (!g_database_binding_proven.load(std::memory_order_acquire)) {
        if (request.command != Command::hello) {
            reply.status = WireStatus::database_unavailable;
            stop_after_binding_probe = true;
        } else {
            const auto proof =
                ProveDatabaseBindingReadOnly(sessions);
            if (proof == BindingProof::failed) {
                reply.status = WireStatus::database_unavailable;
                stop_after_binding_probe = true;
            } else if (proof == BindingProof::proven_now) {
                // The exact native probe is read-only. The expanded Hello is
                // the earliest request permitted to open/install owned policy.
                reply.status = WireStatus::success;
                stop_after_binding_probe = true;
            }
        }
    }

    const bool database_ready =
        !stop_after_binding_probe && EnsureDatabasePolicy(sessions);
    if (database_ready) {
        reply = g_database.ExecuteWireRequest(
            sessions, actor, request);
    } else if (!stop_after_binding_probe) {
        reply.status = request.command == Command::hello
                           ? WireStatus::success
                           : WireStatus::database_unavailable;
    }

    std::array<std::uint8_t, kCarrierBytes> encoded{};
    if (!EncodeReply(actor, reply, &encoded) ||
        !SendWorldReply(view, encoded)) {
        if (g_api && g_api->Log &&
            InterlockedIncrement(&g_world_custom_replies) <= 5) {
            g_api->Log(
                g_mod, SDGO_LOG_ERROR,
                "gift protocol reply transport failed; mutation outcome "
                "may require receipt reconciliation");
        }
        return 0;
    }
    InterlockedIncrement(&g_world_custom_replies);
    // CRequest::Run treats a nonzero handler result as "request process
    // failed" and emits a native failure log.
    return 0;
}

bool LoadAgent(const SdgoApi* api, SdgoMod* mod) {
    if (api->size <
            offsetof(SdgoApi, HookSite) +
                sizeof(api->HookSite) ||
        !api->HookSite)
        return false;
    if (!api->HookSite(
            mod, "friend_unit_gift.agent_capture",
            reinterpret_cast<void*>(&AgentCaptureSite),
            &g_agent_capture_original) ||
        !api->HookSite(
            mod, "friend_unit_gift.agent_send",
            reinterpret_cast<void*>(&AgentSendSite),
            &g_agent_send_original))
        return false;
    api->Log(
        mod, SDGO_LOG_INFO,
        "friend unit gift Agent transport active; exact native probe "
        "is expanded only after native authentication");
    return true;
}

bool LoadWorld(const SdgoApi* api, SdgoMod* mod) {
    g_world_send = reinterpret_cast<WorldSend>(
        api->Symbol(mod, "friend_unit_gift.world_send"));
    g_world_route_lookup = reinterpret_cast<WorldRouteLookup>(
        api->Symbol(mod, "friend_unit_gift.world_route_lookup"));
    g_world_network_slot = reinterpret_cast<void**>(
        api->Symbol(mod, "friend_unit_gift.world_network"));
    g_world_sip_registry =
        api->Symbol(mod, "friend_unit_gift.world_sip_registry");
    if (!g_world_send || !g_world_route_lookup ||
        !g_world_network_slot || !g_world_sip_registry ||
        !api->HookCode(
            mod, "friend_unit_gift.world_handler",
            reinterpret_cast<void*>(&WorldGiftHandler),
            reinterpret_cast<void**>(&g_world_original)))
        return false;
    api->Log(
        mod, SDGO_LOG_INFO,
        "friend unit gift World transport loaded read-only; runtime "
        "carrier-length proof is still required before custom handling");
    return true;
}

bool ConfigureDatabase(const SdgoApi* api, SdgoMod* mod) {
    wchar_t executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(
        nullptr, executable,
        static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable))
        return false;
    std::wstring ini(executable, executable + length);
    const auto slash = ini.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    ini.resize(slash + 1);
    ini += L"WorldServer.ini";
    wchar_t game[128] = {};
    wchar_t common[128] = {};
    const DWORD game_length = GetPrivateProfileStringW(
        L"DB Game", L"DBName", L"", game,
        static_cast<DWORD>(std::size(game)), ini.c_str());
    const DWORD common_length = GetPrivateProfileStringW(
        L"DB Member", L"DBName", L"", common,
        static_cast<DWORD>(std::size(common)), ini.c_str());
    if (!game_length || game_length >= std::size(game) - 1 ||
        !common_length ||
        common_length >= std::size(common) - 1)
        return false;
    EligibilityPolicy policy;
    policy.enabled =
        api->SettingInt(mod, "Enabled", 0) != 0;
    policy.minimum_battles =
        api->SettingInt(mod, "MinBattles", 20);
    policy.minimum_player_kills =
        api->SettingInt(mod, "MinPlayerKills", 1);
    const int required =
        api->SettingInt(mod, "RequiredTiers", 12);
    const int distinct =
        api->SettingInt(mod, "MinDistinctPerTier", 2);
    if (required < 0 || required > 0xFF ||
        distinct < 0 || distinct > 0xFFFF)
        return false;
    policy.required_tiers =
        static_cast<std::uint8_t>(required);
    policy.minimum_distinct_per_tier =
        static_cast<std::uint16_t>(distinct);
    return g_database.Configure(game, common, policy);
}

} // namespace

SDGO_MOD_ENTRY {
    if (!api || !mod ||
        api->version != SDGO_API_VERSION ||
        !api->host_key || !api->Symbol ||
        !api->HookCode || !api->Log)
        return 0;
    g_api = api;
    g_mod = mod;
    if (std::strcmp(api->host_key, kAgentHost) == 0)
        return LoadAgent(api, mod) ? 1 : 0;
    if (std::strcmp(api->host_key, kWorldHost) == 0) {
        if (!ConfigureDatabase(api, mod)) {
            api->Log(
                mod, SDGO_LOG_ERROR,
                "friend unit gift database names or Manager policy are "
                "invalid; no World hook installed");
            return 0;
        }
        return LoadWorld(api, mod) ? 1 : 0;
    }
    return 0;
}
