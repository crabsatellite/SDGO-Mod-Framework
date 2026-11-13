#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/mod_settings.h"
#include "sdgo/sdgo_api.h"
#include "policy.h"
#include "boundary_contact.h"
#include "damage_gate.h"

#if !defined(_M_IX86)
#error map_catalog_server requires the 32-bit BattleServer ABI
#endif

namespace {
using sdgo::map_catalog_server::AcceleratedDeadline;
using sdgo::map_catalog_server::MissionAllowlist;
using sdgo::map_catalog_server::ParseMissionIds;

constexpr char kSupportedHost[] = "BattleServer-548A891D-0046C000";
constexpr std::ptrdiff_t kRoomFromFrame = -0x68;
constexpr std::ptrdiff_t kMissionModeFromRoom = 0x316a0;
constexpr std::ptrdiff_t kClearRowFromFrame = 0x08;
constexpr std::ptrdiff_t kEventNextFromClearRow = 0x2c;
const DWORD kDefaultMxcsr = 0x1f80;

MissionAllowlist g_allowlist{};
sdgo::map_catalog_server::BoundaryContact g_boundaryContact;
sdgo::map_catalog_server::DamageGate g_damageGate;
void* g_deadlineOriginal = nullptr;
volatile LONG g_accelerated = 0;

bool ReadPointer(const void* at, void*& output) noexcept {
    output = nullptr;
    if (!at) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(at, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(at);
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (address + sizeof(void*) < address || address + sizeof(void*) > end) return false;
    __try {
        std::memcpy(&output, at, sizeof(output));
        return output != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = nullptr;
        return false;
    }
}

bool ReadMissionContext(
    const void* frame, std::uint32_t& mission_id, bool& has_next_phase) noexcept {
    mission_id = 0;
    has_next_phase = false;
    if (!frame) return false;
    const auto* frame_bytes = static_cast<const std::uint8_t*>(frame);
    void* room = nullptr;
    void* clear_row = nullptr;
    if (!ReadPointer(frame_bytes + kRoomFromFrame, room) ||
        !ReadPointer(frame_bytes + kClearRowFromFrame, clear_row)) return false;
    void* mission_mode = nullptr;
    const auto* room_bytes = static_cast<const std::uint8_t*>(room);
    if (!ReadPointer(room_bytes + kMissionModeFromRoom, mission_mode)) return false;
    __try {
        std::memcpy(&mission_id, mission_mode, sizeof(mission_id));
        std::uint8_t event_next = 0;
        std::memcpy(&event_next,
                    static_cast<const std::uint8_t*>(clear_row) + kEventNextFromClearRow,
                    sizeof(event_next));
        has_next_phase = event_next != 0;
        return mission_id != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mission_id = 0;
        has_next_phase = false;
        return false;
    }
}

std::uint32_t __stdcall SelectDeadline(void* frame, std::uint32_t native_deadline) noexcept {
    const DWORD error = GetLastError();
    std::uint32_t mission_id = 0;
    bool has_next_phase = false;
    std::uint32_t selected = native_deadline;
    if (ReadMissionContext(frame, mission_id, has_next_phase) && has_next_phase) {
        selected = AcceleratedDeadline(native_deadline, mission_id, g_allowlist);
        if (selected != native_deadline) InterlockedIncrement(&g_accelerated);
    }
    SetLastError(error);
    return selected;
}

// The verified site runs immediately after stock code computes now + 2 in EDX.
// Only EDX is deliberately replaced. The HookSite trampoline then performs the
// original deadline stores and the native success/final-stage path exactly once.
// A zero EventNext keeps the final stage on its stock two-second settlement delay.
__declspec(naked) void DeadlineSite() {
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
    __asm { mov ecx, [esp + 0x200] }
    __asm { mov eax, [ecx + 0x08] }
    __asm { mov edx, [ecx + 0x14] }
    __asm { push edx }
    __asm { push eax }
    __asm { call SelectDeadline }
    __asm { mov ecx, [esp + 0x200] }
    __asm { mov [ecx + 0x14], eax }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp + 0x200] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_deadlineOriginal] }
}
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SdgoMapCatalogServerAcceleratedV1() {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(&g_accelerated, 0, 0));
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SdgoMapCatalogServerDamageBlockedV1() {
    return g_damageGate.Blocked();
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl SdgoMapCatalogServerGuardDefeatsV1() {
    return g_damageGate.Defeats();
}

SDGO_MOD_ENTRY {
    if (!api || !mod || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->host_key || std::strcmp(api->host_key, kSupportedHost) != 0 ||
        !api->HookSite || !api->ModDirectory || !api->Log) return 0;

    const sdgo::ModSettings settings(api, mod);
    char configured[2048]{};
    const auto length = settings.String(
        "ImmediateTransitionMissionIds", "", configured, static_cast<std::uint32_t>(sizeof(configured)));
    if (length >= sizeof(configured) - 1 || !ParseMissionIds(configured, g_allowlist)) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "invalid ImmediateTransitionMissionIds; pacing hook was not installed");
        return 0;
    }
    if (!g_boundaryContact.Install(api, mod) || !g_damageGate.Install(api, mod)) return 0;
    if (g_allowlist.count == 0) {
        api->Log(mod, SDGO_LOG_INFO,
                 "no imported pacing IDs declared; native mission pacing retained; boundary contact=%u; damage gates=%u",
                 g_boundaryContact.Enabled() ? 1u : 0u, g_damageGate.Enabled() ? 1u : 0u);
        return 1;
    }
    if (!api->HookSite(mod, "map_catalog_server.phase_success_deadline",
                       reinterpret_cast<void*>(&DeadlineSite), &g_deadlineOriginal)) return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "imported mission pacing active for %u declared mission IDs",
             static_cast<unsigned>(g_allowlist.count));
    return 1;
}
