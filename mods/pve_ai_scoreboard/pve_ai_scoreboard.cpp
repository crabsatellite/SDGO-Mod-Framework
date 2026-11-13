// Adds mission AI wingmen to the shipped PvE TAB roster without changing the
// UnitManager player count used by mission scripts. The native panel already
// renders names, unit icons, kills, assists and points for any registered slot;
// this Mod supplies both native drawing passes with the missing display order
// and resolves exact live wingmen at the native HP lookup result sites.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/pve_ai_scoreboard_observer.h"
#include "sdgo/sdgo_api.h"

namespace {

constexpr std::size_t kModeRoom = 0x08;
constexpr std::size_t kRoomHumansBegin = 0x1C;
constexpr std::size_t kRoomHumansEnd = 0x20;
constexpr std::size_t kRoomAiBegin = 0x3C;
constexpr std::size_t kRoomAiEnd = 0x40;
constexpr std::size_t kRecordSize = 0x344;
constexpr std::size_t kRecordPilot = 0x00;
constexpr std::size_t kRecordConnection = 0x04;
constexpr std::size_t kRecordTeam = 0x1A;
constexpr std::size_t kRecordNpcData = 0x44;

constexpr std::size_t kUnitObjectId = 0x04;
constexpr std::size_t kUnitTeam = 0x20;
constexpr std::size_t kUnitPilot = 0x2B68;
constexpr std::size_t kUnitNpcDataId = 0x3238;

constexpr std::size_t kUmPlayerCount = 0x8C;
constexpr std::size_t kUmRosterOrder = 0x1D94;
constexpr std::size_t kUmRosterSecondaryOrder = 0x1C94;
constexpr std::size_t kSlotObjectId = 0x04;
constexpr std::size_t kSlotConnection = 0x2C;
constexpr std::size_t kSlotRecord = 0x44;

constexpr std::size_t kPanelState = 0x38;
constexpr std::size_t kPanelVisibleA = 0x3C;
constexpr std::size_t kPanelVisibleB = 0x3D;
constexpr std::size_t kPanelRows = 0x150;
constexpr std::size_t kPanelOffsets = 0x170;
constexpr int kMaximumRows = 12;
constexpr uint32_t kBattleState = 22;

using GameAppFn = uint8_t*(__cdecl*)();
using PlayerSlotFn = uint8_t*(__thiscall*)(void*, int);
using FindObjectFn = uint8_t*(__thiscall*)(void*, uint32_t);
using RosterRenderFn = void(__thiscall*)(uint8_t*);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
GameAppFn g_gameApp = nullptr;
PlayerSlotFn g_playerSlot = nullptr;
FindObjectFn g_findObject = nullptr;
RosterRenderFn g_originalRender = nullptr;
RosterRenderFn g_originalTextRender = nullptr;
void* g_hpUnitOriginal = nullptr;
void* g_hpObserverUnitOriginal = nullptr;
void** g_unitManager = nullptr;
const void* g_missionVtable = nullptr;
const void* g_rosterInit = nullptr;
bool g_logged = false;
uint32_t g_rejectionLogged = 0;
bool RejectRoster(unsigned reason, uint32_t a=0, uint32_t b=0, uint32_t c=0) {
    if (reason < 32 && (g_rejectionLogged & (1u << reason)) == 0) {
        g_rejectionLogged |= 1u << reason;
        if (g_api && g_api->Log) g_api->Log(g_mod, SDGO_LOG_INFO,
            "PvE TAB roster admission reason=%u a=%08X b=%08X c=%08X", reason,a,b,c);
    }
    return false;
}
SRWLOCK g_statusLock = SRWLOCK_INIT;
SdgoPveAiScoreboardStatusV1 g_status{
    sizeof(SdgoPveAiScoreboardStatusV1),
    SDGO_PVE_AI_SCOREBOARD_STATUS_VERSION,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

struct Span {
    uint8_t* first;
    int count;
    bool valid;
};

struct RosterPlan {
    void* unitManager;
    int humans;
    int ais;
    int rows;
    int order[kMaximumRows];
};

// A missing native FindPlayer lookup may resolve an exact registered wingman
// only while this thread is drawing this panel. No gameplay lookup is changed.
__declspec(thread) const RosterPlan* g_drawingPlan = nullptr;

struct RosterPatch {
    int oldRows;
    int orderCount;
    int oldOrder[kMaximumRows];
    int oldSecondaryOrder[kMaximumRows];
    int offsetBegin;
    int offsetCount;
    float oldOffsets[kMaximumRows][3];
    bool applied;
};

template <typename T>
bool Bind(T& output, const char* name) {
    output = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return output != nullptr;
}

Span RecordSpan(uint8_t* room, std::size_t beginOffset,
                std::size_t endOffset) {
    Span span{};
    span.first = *reinterpret_cast<uint8_t**>(room + beginOffset);
    uint8_t* end = *reinterpret_cast<uint8_t**>(room + endOffset);
    if (!span.first && !end) {
        span.valid = true;
        return span;
    }
    if (!span.first || !end || end < span.first) return span;
    const uintptr_t bytes = static_cast<uintptr_t>(end - span.first);
    if (bytes % kRecordSize) return span;
    const uintptr_t count = bytes / kRecordSize;
    if (count > kMaximumRows) return span;
    span.count = static_cast<int>(count);
    span.valid = true;
    return span;
}

bool UniqueHumanOrder(const int* order, int humans) {
    bool seen[kMaximumRows]{};
    for (int i = 0; i < humans; ++i) {
        const int slot = order[i];
        if (slot < 0 || slot >= humans || seen[slot]) return false;
        seen[slot] = true;
    }
    return true;
}

bool RegisteredAiSlot(void* unitManager, int slotIndex,
                      const uint8_t* authoritativeRecord) {
    uint8_t* slot = g_playerSlot(unitManager, slotIndex);
    if (!slot) return RejectRoster(10,static_cast<uint32_t>(slotIndex));
    const uint32_t pilot = *reinterpret_cast<const uint32_t*>(
        authoritativeRecord + kRecordPilot);
    const uint32_t connection = *reinterpret_cast<const uint32_t*>(
        authoritativeRecord + kRecordConnection);
    const uint32_t npcData = *reinterpret_cast<const uint32_t*>(
        authoritativeRecord + kRecordNpcData);
    const uint8_t team = authoritativeRecord[kRecordTeam];
    // Server-owned AI records have no player transport connection. Their
    // zero connection is still checked against the copied record and slot.
    if (!(pilot & 0x80000000u) || !npcData || (team != 1 && team != 2))
        return RejectRoster(11,pilot,connection,npcData);
    if (*reinterpret_cast<const uint32_t*>(slot + kSlotConnection) !=
        connection)
        return RejectRoster(12,*reinterpret_cast<const uint32_t*>(slot+kSlotConnection),connection,slotIndex);
    const uint8_t* copiedRecord =
        *reinterpret_cast<uint8_t* const*>(slot + kSlotRecord);
    if (!copiedRecord ||
        *reinterpret_cast<const uint32_t*>(copiedRecord + kRecordPilot) !=
            pilot ||
        *reinterpret_cast<const uint32_t*>(copiedRecord + kRecordConnection) !=
            connection ||
        copiedRecord[kRecordTeam] != team ||
        *reinterpret_cast<const uint32_t*>(copiedRecord + kRecordNpcData) !=
            npcData)
        return RejectRoster(13,pilot,slotIndex,reinterpret_cast<uintptr_t>(copiedRecord));
    const uint32_t objectId =
        *reinterpret_cast<const uint32_t*>(slot + kSlotObjectId);
    // The shipped roster draws identity and scores from the registered slot
    // and copied room record. During death/respawn the live unit may already
    // be absent while those authoritative records remain valid; the native
    // icon/status helpers also handle a missing object. Keep that row, and
    // apply the stronger live-object identity check only while an object is
    // actually present.
    if (!objectId) return true;
    uint8_t* unit = g_findObject(unitManager, objectId);
    if (!unit) return true;
    const bool valid =
        *reinterpret_cast<const uint32_t*>(unit + kUnitObjectId) == objectId &&
        *reinterpret_cast<const uint32_t*>(unit + kUnitPilot) == pilot &&
        *reinterpret_cast<const uint32_t*>(unit + kUnitNpcDataId) == npcData &&
        unit[kUnitTeam] == team;
    return valid || RejectRoster(14,*reinterpret_cast<const uint32_t*>(unit+kUnitNpcDataId),npcData,
                                uint32_t(unit[kUnitTeam])|(uint32_t(team)<<8));
}

bool BuildRosterPlan(uint8_t* panel, RosterPlan& plan,
                     bool& hadAuthoritativeAi) {
    hadAuthoritativeAi = false;
    if (!panel || !g_gameApp || !g_unitManager || !g_playerSlot ||
        !g_findObject || !g_rosterInit || !g_missionVtable)
        return false;
    void** panelVtable = *reinterpret_cast<void***>(panel);
    if (!panelVtable || panelVtable[10] != g_rosterInit)
        return RejectRoster(1,reinterpret_cast<uintptr_t>(panelVtable),
                            panelVtable?reinterpret_cast<uintptr_t>(panelVtable[10]):0,
                            reinterpret_cast<uintptr_t>(g_rosterInit));

    uint8_t* app = g_gameApp();
    if (!app || *reinterpret_cast<uint32_t*>(app + 0x4A0) != kBattleState)
        return false;
    uint8_t* service = *reinterpret_cast<uint8_t**>(app + 0x4A4);
    uint8_t* mode = service
        ? *reinterpret_cast<uint8_t**>(service + 0x0C)
        : nullptr;
    if (!mode || *reinterpret_cast<const void* const*>(mode) !=
                     g_missionVtable)
        return RejectRoster(2,reinterpret_cast<uintptr_t>(mode),
                            mode?*reinterpret_cast<uintptr_t*>(mode):0,
                            reinterpret_cast<uintptr_t>(g_missionVtable));
    uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
    void* unitManager = *g_unitManager;
    if (!room || !unitManager) return false;

    const Span humans = RecordSpan(
        room, kRoomHumansBegin, kRoomHumansEnd);
    const Span ais = RecordSpan(room, kRoomAiBegin, kRoomAiEnd);
    if (!humans.valid || !ais.valid || humans.count < 1 ||
        humans.count > kMaximumRows || ais.count < 1 ||
        humans.count + ais.count > kMaximumRows)
        return RejectRoster(3,humans.count,ais.count,uint32_t(humans.valid)|(uint32_t(ais.valid)<<8));
    hadAuthoritativeAi = true;
    if (*reinterpret_cast<const int*>(
            static_cast<const uint8_t*>(unitManager) + kUmPlayerCount) !=
        humans.count)
        return RejectRoster(4,*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(unitManager)+kUmPlayerCount),humans.count);

    const int* nativeOrder = reinterpret_cast<const int*>(
        static_cast<const uint8_t*>(unitManager) + kUmRosterOrder);
    if (!UniqueHumanOrder(nativeOrder, humans.count))
        return RejectRoster(5,nativeOrder[0],humans.count);

    plan.unitManager = unitManager;
    plan.humans = humans.count;
    plan.ais = 0;
    for (int i = 0; i < humans.count; ++i) plan.order[i] = nativeOrder[i];
    for (int i = 0; i < ais.count; ++i) {
        const int slot = humans.count + i;
        const uint8_t* record = ais.first + i * kRecordSize;
        if (RegisteredAiSlot(unitManager, slot, record))
            plan.order[humans.count + plan.ais++] = slot;
    }
    plan.rows = plan.humans + plan.ais;
    return plan.ais > 0;
}

bool PanelVisible(const uint8_t* panel, uint32_t& state) {
    state = panel ? *reinterpret_cast<const uint32_t*>(panel + kPanelState) : 0u;
    return panel && panel[kPanelVisibleA] != 0 && panel[kPanelVisibleB] != 0 &&
           state == 2u;
}

void PublishStatus(bool active, const RosterPlan* plan, bool synced,
                   bool rejected, const uint8_t* panel) {
    uint32_t state = 0;
    const bool visible = PanelVisible(panel, state);
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.active = active ? 1u : 0u;
    g_status.human_rows = plan ? static_cast<uint32_t>(plan->humans) : 0u;
    g_status.ai_rows = plan ? static_cast<uint32_t>(plan->ais) : 0u;
    g_status.total_rows = plan ? static_cast<uint32_t>(plan->rows) : 0u;
    g_status.panel_visible = visible ? 1u : 0u;
    g_status.panel_state = state;
    ++g_status.render_count;
    if (synced) ++g_status.sync_count;
    if (rejected) ++g_status.rejected_count;
    ReleaseSRWLockExclusive(&g_statusLock);
}

void PublishVisibleRender(const uint8_t* panel, const RosterPlan& plan,
                          bool text) {
    uint32_t state = 0;
    const bool visible = PanelVisible(panel, state);
    uint32_t named = 0;
    if (visible && text) {
        for (int i = plan.humans; i < plan.rows; ++i) {
            const uint8_t* slot = g_playerSlot(plan.unitManager, plan.order[i]);
            const uint32_t length = *reinterpret_cast<const uint32_t*>(slot + 0x18);
            if (length > 0 && length <= 128) ++named;
        }
    }
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.panel_visible = visible ? 1u : 0u;
    g_status.panel_state = state;
    if (visible) {
        if (text) {
            ++g_status.visible_text_render_count;
            g_status.named_ai_rows = named;
        } else ++g_status.visible_render_count;
    }
    ReleaseSRWLockExclusive(&g_statusLock);
}

bool ApplyRosterPlan(uint8_t* panel, const RosterPlan& plan,
                     RosterPatch& patch) {
    int& rows = *reinterpret_cast<int*>(panel + kPanelRows);
    if (rows != plan.humans) return false;
    patch.oldRows = rows;
    patch.orderCount = plan.rows;
    patch.offsetBegin = plan.humans;
    patch.offsetCount = plan.ais;

    int* order = reinterpret_cast<int*>(
        static_cast<uint8_t*>(plan.unitManager) + kUmRosterOrder);
    int* secondaryOrder = reinterpret_cast<int*>(
        static_cast<uint8_t*>(plan.unitManager) + kUmRosterSecondaryOrder);
    for (int i = 0; i < patch.orderCount; ++i) {
        patch.oldOrder[i] = order[i];
        patch.oldSecondaryOrder[i] = secondaryOrder[i];
    }
    for (int i = 0; i < patch.offsetCount; ++i) {
        const float* offset = reinterpret_cast<const float*>(
            panel + kPanelOffsets + (patch.offsetBegin + i) *
                3 * sizeof(float));
        for (int axis = 0; axis < 3; ++axis)
            patch.oldOffsets[i][axis] = offset[axis];
    }
    // From this point every touched byte has a complete snapshot. If a later
    // native memory access faults, the caller's SEH path can restore it.
    patch.applied = true;
    for (int i = 0; i < patch.orderCount; ++i)
        order[i] = plan.order[i];
    for (int i = 0; i < patch.orderCount; ++i)
        secondaryOrder[i] = plan.order[i];
    const float* nativeOffset = reinterpret_cast<const float*>(
        panel + kPanelOffsets + (plan.humans - 1) * 3 * sizeof(float));
    for (int i = 0; i < patch.offsetCount; ++i) {
        float* offset = reinterpret_cast<float*>(
            panel + kPanelOffsets + (patch.offsetBegin + i) *
                3 * sizeof(float));
        for (int axis = 0; axis < 3; ++axis)
            offset[axis] = nativeOffset[axis];
    }
    rows = plan.rows;
    if (!g_logged) {
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "PvE TAB roster uses %d human and %d registered AI rows",
                   plan.humans, plan.ais);
        g_logged = true;
    }
    return true;
}

void RestoreRosterPlan(uint8_t* panel, const RosterPlan& plan,
                       const RosterPatch& patch) {
    if (!patch.applied) return;
    int* order = reinterpret_cast<int*>(
        static_cast<uint8_t*>(plan.unitManager) + kUmRosterOrder);
    int* secondaryOrder = reinterpret_cast<int*>(
        static_cast<uint8_t*>(plan.unitManager) + kUmRosterSecondaryOrder);
    for (int i = 0; i < patch.orderCount; ++i) {
        order[i] = patch.oldOrder[i];
        secondaryOrder[i] = patch.oldSecondaryOrder[i];
    }
    for (int i = 0; i < patch.offsetCount; ++i) {
        float* offset = reinterpret_cast<float*>(
            panel + kPanelOffsets + (patch.offsetBegin + i) *
                3 * sizeof(float));
        for (int axis = 0; axis < 3; ++axis)
            offset[axis] = patch.oldOffsets[i][axis];
    }
    *reinterpret_cast<int*>(panel + kPanelRows) = patch.oldRows;
}

uint8_t* __stdcall ResolveDrawnUnit(const uint8_t* slot, uint8_t* nativeUnit) {
    if (nativeUnit || !g_drawingPlan || !slot) return nativeUnit;
    const DWORD incomingError = GetLastError();
    uint8_t* result = nullptr;
    __try {
        const RosterPlan& plan = *g_drawingPlan;
        for (int i = plan.humans; i < plan.rows; ++i) {
            const int index = plan.order[i];
            if (g_playerSlot(plan.unitManager, index) != slot) continue;
            // Revalidate against the authoritative room record. A reused
            // object id, ordinary NPC or dead/missing unit cannot be admitted.
            uint8_t* app = g_gameApp();
            uint8_t* service = app ? *reinterpret_cast<uint8_t**>(app + 0x4A4) : nullptr;
            uint8_t* mode = service ? *reinterpret_cast<uint8_t**>(service + 0x0C) : nullptr;
            uint8_t* room = mode ? *reinterpret_cast<uint8_t**>(mode + kModeRoom) : nullptr;
            const Span ais = room ? RecordSpan(room, kRoomAiBegin, kRoomAiEnd) : Span{};
            const int ai = index - plan.humans;
            if (!ais.valid || ai < 0 || ai >= ais.count ||
                !RegisteredAiSlot(plan.unitManager, index, ais.first + ai * kRecordSize)) break;
            const uint32_t object = *reinterpret_cast<const uint32_t*>(slot + kSlotObjectId);
            uint8_t* unit = object ? g_findObject(plan.unitManager, object) : nullptr;
            if (unit) {
                const int hp = *reinterpret_cast<const int*>(unit + 0x38);
                const int maximum = *reinterpret_cast<const int*>(unit + 0x3C);
                // A defeated CNPC may retain negative HP. The player-only
                // renderer assumes a nonnegative bar width and nonzero divisor.
                if (hp >= 0 && maximum > 0 && hp <= maximum) result = unit;
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (result) {
        AcquireSRWLockExclusive(&g_statusLock);
        ++g_status.hp_resolve_count;
        ReleaseSRWLockExclusive(&g_statusLock);
    }
    SetLastError(incomingError);
    return result;
}

// Both native HP passes have just returned from FindPlayer. EBP-40 contains
// the slot and EAX the native result; the relocated mov/test consumes EAX.
// Preserve the complete live register/flags/FP state except that exact result.
#define SDGO_ROSTER_HP_SITE(name, continuation) \
__declspec(naked) void name() { \
    __asm { pushfd } \
    __asm { pushad } \
    __asm { sub esp, 528 } \
    __asm { lea eax, [esp+15] } \
    __asm { and eax, -16 } \
    __asm { fxsave [eax] } \
    __asm { push dword ptr [esp+556] } \
    __asm { push dword ptr [ebp-40h] } \
    __asm { call ResolveDrawnUnit } \
    __asm { mov dword ptr [esp+556], eax } \
    __asm { lea eax, [esp+15] } \
    __asm { and eax, -16 } \
    __asm { fxrstor [eax] } \
    __asm { add esp, 528 } \
    __asm { popad } \
    __asm { popfd } \
    __asm { jmp dword ptr [continuation] } \
}
SDGO_ROSTER_HP_SITE(RosterHpUnit, g_hpUnitOriginal)
SDGO_ROSTER_HP_SITE(RosterHpObserverUnit, g_hpObserverUnitOriginal)
#undef SDGO_ROSTER_HP_SITE

void RosterPass(uint8_t* panel, RosterRenderFn original, bool text) {
    const DWORD incomingError = GetLastError();
    RosterPlan plan{};
    RosterPatch patch{};
    bool authoritativeAi = false;
    bool valid = false;
    bool synced = false;
    __try {
        valid = BuildRosterPlan(panel, plan, authoritativeAi);
        if (valid) synced = ApplyRosterPlan(panel, plan, patch);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const DWORD exceptionError = GetLastError();
        RestoreRosterPlan(panel, plan, patch);
        SetLastError(exceptionError);
        valid = false;
        synced = false;
    }
    PublishStatus(valid && synced, valid ? &plan : nullptr, synced,
                  authoritativeAi && !synced, panel);
    SetLastError(incomingError);
    if (!synced) {
        original(panel);
        return;
    }
    const RosterPlan* previous = g_drawingPlan;
    g_drawingPlan = &plan;
    __try {
        original(panel);
        PublishVisibleRender(panel, plan, text);
    } __finally {
        const DWORD renderError = GetLastError();
        g_drawingPlan = previous;
        RestoreRosterPlan(panel, plan, patch);
        SetLastError(renderError);
    }
}

void __fastcall RosterRender(uint8_t* panel, void*) {
    RosterPass(panel, g_originalRender, false);
}
void __fastcall RosterTextRender(uint8_t* panel, void*) {
    RosterPass(panel, g_originalTextRender, true);
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl
SdgoPveAiScoreboardGetStatusV1(SdgoPveAiScoreboardStatusV1* status) {
    if (!status || status->size < sizeof(*status) ||
        status->version != SDGO_PVE_AI_SCOREBOARD_STATUS_VERSION)
        return 0;
    AcquireSRWLockShared(&g_statusLock);
    *status = g_status;
    ReleaseSRWLockShared(&g_statusLock);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl
SdgoModLoad(const SdgoApi* api, SdgoMod* mod) {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < sizeof(SdgoApi) || !api->Symbol || !api->HookCode ||
        !api->Log || !api->HookSite)
        return 0;
    g_api = api;
    g_mod = mod;
    if (!Bind(g_gameApp, "game.app") ||
        !Bind(g_unitManager, "game.unit_manager") ||
        !Bind(g_playerSlot, "um.player_slot") ||
        !Bind(g_findObject, "um.find_object") ||
        !Bind(g_missionVtable, "CMissionMode.vtbl") ||
        !Bind(g_rosterInit, "hud.battle_roster_init") ||
        !api->HookCode(mod, "hud.battle_roster_render",
                       reinterpret_cast<void*>(&RosterRender),
                       reinterpret_cast<void**>(&g_originalRender)) ||
        !api->HookCode(mod, "hud.battle_roster_text_render",
                       reinterpret_cast<void*>(&RosterTextRender),
                       reinterpret_cast<void**>(&g_originalTextRender)) ||
        !api->HookSite(mod, "hud.battle_roster_hp_unit",
                       reinterpret_cast<void*>(&RosterHpUnit), &g_hpUnitOriginal) ||
        !api->HookSite(mod, "hud.battle_roster_hp_observer_unit",
                       reinterpret_cast<void*>(&RosterHpObserverUnit), &g_hpObserverUnitOriginal))
        return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "ready: registered PvE AI slots use the native TAB roster");
    return 1;
}
