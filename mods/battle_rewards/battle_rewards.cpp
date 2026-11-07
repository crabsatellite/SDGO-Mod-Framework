// Native battle reward integration. Combat telemetry comes from the verified
// server HP-loss helper; normal G delivery remains in the game's own pipeline.
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "sdgo/battle_eligibility_observer.h"
#include "sdgo/battle_rewards_damage_observer.h"
#include "sdgo/battle_reward_stats.h"
#include "sdgo/battle_reward_mission.h"
#include "sdgo/operator_reward_filter.h"
#include "boss_catalog.h"
#include "combat_tracker.h"
#include "daily_schedule.h"
#include "native_cash_notify.h"
#include "native_combat_hook.h"
#include "native_sites.h"
#include "runtime_state.h"
#include "reward_filter.h"
#include "result_presentation.h"
#include "site_bridge.h"
#include "wallet_jobs.h"

namespace {
using namespace sdgo::battle_rewards;
namespace fs = std::filesystem;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
std::unique_ptr<RuntimeState> g_runtime;
std::mutex g_mutex;
BossCatalog g_bosses;
bool g_bosses_loaded = false, g_trace = false;
std::atomic<bool> g_contract_failure{false};
std::atomic<bool> g_contribution_failure{false};
DailySchedule g_daily;
std::string g_boot, g_error;
PayoutCounters g_counters;
uint64_t g_serial = 0;
std::array<BYTE*, 12> g_sites{};
std::atomic<unsigned> g_nativeSettlementSamples{0};
eligibility_observer::GameTestGate g_eligibilityGate;
eligibility_observer::BattleRowLimiter g_eligibilityLimiter;
SRWLOCK g_eligibilityLock = SRWLOCK_INIT;
constexpr char kEligibilitySymbol[] = "battle.rewards.observe_eligibility.team";
volatile PVOID g_damageObserverCallback = nullptr;
volatile PVOID g_damageObserverUser = nullptr;
volatile LONG g_damageObserverRegistration = 0;
volatile PVOID g_operatorRewardFilterCallback = nullptr;
volatile PVOID g_operatorRewardFilterUser = nullptr;
volatile LONG g_operatorRewardFilterRegistration = 0;
volatile LONG g_settlementHooksReady = 0;
constexpr char kMissionFinalSymbol[] = "battle.rewards.final.mission";
constexpr ptrdiff_t kBattlePilotOperatorDefinition = 0x50;

enum class GpPolicy : uint8_t {
    Native,
    Replace
};
GpPolicy g_gpPolicy = GpPolicy::Replace;

struct OperatorSettlementIdentity {
    BYTE* room = nullptr;
    uint32_t room_id = 0;
    uint32_t started = 0;
    uint32_t account = 0;
};

struct OperatorSettlementRecord {
    OperatorSettlementIdentity identity;
    uint8_t phases = 0;
    bool mission = false;
    uint64_t serial = 0;
};

constexpr size_t kOperatorSettlementCapacity = 4096;
std::array<OperatorSettlementRecord, kOperatorSettlementCapacity>
    g_operatorSettlements{};
size_t g_operatorSettlementCount = 0;
uint64_t g_operatorSettlementSerial = 0;

struct MissionPending {
    bool active = false;
    bool replace_gp = false;
    bool gp_writable = false;
    bool definition_read = false;
    bool identity_read = false;
    BYTE* frame = nullptr;
    BYTE* room = nullptr;
    BYTE* room_pilot = nullptr;
    BYTE* battle_pilot = nullptr;
    void* operator_definition = nullptr;
    OperatorSettlementIdentity identity;
    uint32_t account = 0;
    uint16_t replacement_gp = 0;
};
thread_local MissionPending g_missionPending;

void __stdcall ForwardDamageObserver(
    void* room, void* victimPilot, void* attackerPilot, void* victimUnit,
    uint32_t effectiveLoss, int32_t deathFlag) noexcept {
    if (InterlockedCompareExchange(
            &g_damageObserverRegistration, 0, 0) != 2) {
        return;
    }
    const auto callback =
        reinterpret_cast<SdgoBattleRewardsDamageObserverCallbackV1>(
            InterlockedCompareExchangePointer(
                &g_damageObserverCallback, nullptr, nullptr));
    if (!callback) return;
    SdgoBattleRewardsDamageEventV1 event = {
        sizeof(event),
        SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION,
        room,
        victimPilot,
        attackerPilot,
        victimUnit,
        effectiveLoss,
        deathFlag};
    void* user = InterlockedCompareExchangePointer(
        &g_damageObserverUser, nullptr, nullptr);
    __try {
        callback(&event, user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool MissionMode(uint8_t mode) { return mode == 5 || mode == 8 || mode == 10; }

bool ReadNativeBytes(const void* base, size_t offset, void* output, size_t size) {
    const BYTE* address = nullptr;
    if (!output || offset > static_cast<size_t>(PTRDIFF_MAX) ||
        !native_stats::AddOffset(base, static_cast<ptrdiff_t>(offset), &address)) return false;
    __try { memcpy(output, address, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadEligibilityAscii(const std::wstring& path, const wchar_t* section,
                          const wchar_t* key, char* output, size_t capacity) {
    if (!output || capacity == 0) return false;
    wchar_t wide[128] = {};
    const DWORD length = GetPrivateProfileStringW(
        section, key, L"", wide, static_cast<DWORD>(_countof(wide)), path.c_str());
    if (length == 0 || static_cast<size_t>(length) >= capacity) return false;
    for (DWORD i = 0; i < length; ++i) {
        if (wide[i] > 0x7F) return false;
        output[i] = static_cast<char>(wide[i]);
    }
    output[length] = '\0';
    return true;
}

bool LoadEligibilityGate(eligibility_observer::GameTestGate* gate) {
    if (!gate) return false;
    char environmentRun[64] = {};
    char environmentRole[16] = {};
    const DWORD runLength = GetEnvironmentVariableA(
        "SDGO_GAME_TEST_RUN", environmentRun, static_cast<DWORD>(sizeof(environmentRun)));
    const DWORD roleLength = GetEnvironmentVariableA(
        "SDGO_GAME_TEST_ROLE", environmentRole, static_cast<DWORD>(sizeof(environmentRole)));
    if (runLength == 0 && roleLength == 0) return false;
    if (runLength == 0 || runLength >= sizeof(environmentRun) ||
        roleLength == 0 || roleLength >= sizeof(environmentRole)) {
        return false;
    }

    wchar_t executable[32768] = {};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executable, static_cast<DWORD>(_countof(executable)));
    if (executableLength == 0 || executableLength >= _countof(executable)) return false;
    std::wstring runIni(executable, executableLength);
    const size_t slash = runIni.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    runIni.resize(slash);
    runIni += L"\\sdgo\\gametest\\run.ini";

    char configuredRun[64] = {};
    char configuredRole[16] = {};
    char nonce[16] = {};
    if (!ReadEligibilityAscii(runIni, L"run", L"id",
                              configuredRun, sizeof(configuredRun)) ||
        !ReadEligibilityAscii(runIni, L"run", L"role",
                              configuredRole, sizeof(configuredRole)) ||
        !ReadEligibilityAscii(runIni, L"observer", L"nonce",
                              nonce, sizeof(nonce))) {
        return false;
    }
    return eligibility_observer::ValidateGate(
        environmentRun, environmentRole, configuredRun, configuredRole, nonce, gate);
}

void __stdcall ObserveEligibility(BYTE* frame) noexcept {
    __try {
        eligibility_observer::Row row{};
        eligibility_observer::CaptureError capture =
            eligibility_observer::CaptureError::None;
        if (!eligibility_observer::ReadTeamFrame(
                frame, g_eligibilityGate.title_nonce, &row, &capture)) {
            return;
        }
        AcquireSRWLockExclusive(&g_eligibilityLock);
        const bool accepted =
            g_eligibilityLimiter.TryAccept(row.battle, row.native_rank);
        ReleaseSRWLockExclusive(&g_eligibilityLock);
        if (!accepted) return;
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "battle eligibility runUUID=%s nativeRoomCookie=%u "
            "nativeRoomTitleNonce=%s native_id=%u AIbit=%u slot=%u rank=%u "
            "pilotState=%u eligibility_flags=%u selectedUnit=%u branch=%s",
            g_eligibilityGate.run_uuid,
            row.battle.room_cookie,
            row.title_nonce,
            row.native_id,
            row.ai ? 1u : 0u,
            row.physical_slot,
            row.native_rank,
            static_cast<unsigned>(row.pilot_state),
            row.eligibility_flags,
            row.selected_unit,
            row.branch == eligibility_observer::Branch::SkipParticipant
                ? "skip" : "continue");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // This GameTest-only observer must never alter native settlement flow.
    }
}

struct Session {
    uint32_t room_id = 0, started = 0, mission_id = 0;
    uint8_t mode = 0;
    uint64_t last_seen = 0;
    uint64_t completed_unix = 0;
    std::string match_id;
    CombatTracker combat;
    BossCatalog bosses;
    bool boss_catalog_loaded = false, frozen = false, declined = false;
    std::string declined_error;
    Policy policy;
    MatchContext context;
    std::string wallet_scope;
    std::array<uint32_t, 12> accounts{}, settled{};
    std::vector<WalletJob::Attendance> attendance;
    unsigned account_count = 0, settled_count = 0;
};
std::map<uintptr_t, Session> g_sessions;

struct Pending {
    WalletJob job;
    native_cash_notify::SessionRoute route;
    uint64_t since = 0;
};
std::vector<Pending> g_pending;

bool HooksActive() {
    for (BYTE* site : g_sites) {
        BYTE instruction = 0;
        if (!native_stats::Read(site, 0, &instruction) || instruction != 0xE9) return false;
    }
    return true;
}

bool NetworkReady() {
    void* net = nullptr;
    void* transport = nullptr;
    return native_cash_notify::detail::SafeRead(native_cash_notify::net_frame_proc_global, 0, &net) && net &&
           native_cash_notify::detail::SafeRead(net, native_cash_notify::kNetFrameTransport, &transport) && transport;
}

Session* CurrentSession(BYTE* room) {
    uint32_t room_id = 0, start = 0, mission = 0;
    uint8_t mode = 0;
    if (!native_stats::Read(room, 0, &room_id) || !native_stats::Read(room, 0x2EEB4, &start) ||
        !native_stats::Read(room, 5, &mode) || !native_stats::Read(room, 0x1B, &mission) ||
        !start || mode < 1 || mode > 14) return nullptr;
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    auto found = g_sessions.find(key);
    if (found != g_sessions.end() && found->second.started == start && found->second.room_id == room_id) {
        found->second.last_seen = GetTickCount64();
        return &found->second;
    }
    if (g_sessions.size() >= 1024 && found == g_sessions.end()) {
        const uint64_t now = GetTickCount64();
        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            if (now - it->second.last_seen > 24 * 60 * 60 * 1000ULL) it = g_sessions.erase(it);
            else ++it;
        }
        if (g_sessions.size() >= 1024) { g_error = "native match registry capacity exceeded"; return nullptr; }
    }
    Session fresh;
    fresh.room_id = room_id; fresh.started = start; fresh.mission_id = mission; fresh.mode = mode;
    fresh.last_seen = GetTickCount64(); fresh.bosses = g_bosses; fresh.boss_catalog_loaded = g_bosses_loaded;
    if (!BossCatalog::Sha256(g_boot + ":" + std::to_string(++g_serial) + ":" + std::to_string(room_id) + ":" + std::to_string(start), &fresh.match_id))
        return nullptr;
    auto result = g_sessions.insert_or_assign(key, std::move(fresh));
    ++g_counters.matches_seen;
    return &result.first->second;
}

bool __stdcall EventGate(const native_combat_hook::NativeDamageSnapshot* event) {
    if (!event) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* session = CurrentSession(static_cast<BYTE*>(event->room));
    return session && !g_contract_failure && !g_contribution_failure &&
           !session->frozen && session->combat.Valid();
}

bool __stdcall Classify(const native_combat_hook::NativeDamageSnapshot* event,
                        native_combat_hook::VictimClassification* result) {
    if (!event || !result) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* session = CurrentSession(static_cast<BYTE*>(event->room));
    if (!session || session->frozen) return false;
    result->npc = (event->victim_entity & 0x80000000u) != 0;
    if (MissionMode(event->room_mode)) {
        if (!session->boss_catalog_loaded) {
            g_error = "mission Boss identity registry is unavailable"; return false;
        }
        if (result->npc) {
            mission_native::MissionVictimIdentity identity;
            auto reader = &ReadNativeBytes;
            if (mission_native::ReadMissionVictim(event->room, event->victim_pilot, reader, &identity) != mission_native::ReadError::None) {
                g_error = "native mission NPC identity did not match the verified contract";
                session->combat.Invalidate("native mission NPC identity could not be verified");
                return false;
            }
            result->boss = session->bosses.IsBoss(identity.mission_id, identity.npc_data_id);
        }
        // Native mission actors outside the twelve party slots are not wallet
        // recipients. Retain their Boss damage/death so a wingman's final hit
        // does not erase a human's contribution; their other hits are irrelevant.
        if (event->attacker_slot >= 12 && !result->boss) return false;
    }
    result->accepted = true; return true;
}

void __stdcall Record(const native_combat_hook::NativeDamageSnapshot* event,
                      const native_combat_hook::VictimClassification* classification) {
    if (!event || !classification) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Session* session = CurrentSession(static_cast<BYTE*>(event->room));
    if (!session || session->frozen) return;
    CombatEvent hit{event->attacker_account, event->victim_entity, event->attacker_team, event->victim_team,
                    event->effective_damage, event->death_flag != 0, classification->npc, classification->boss};
    if (!session->combat.Record(hit, MissionMode(event->room_mode) ? Mode::Pve : Mode::Pvp))
        g_error = session->combat.Error();
}

void __stdcall CombatDiagnostic(native_combat_hook::DiagnosticReason reason,
                                const native_combat_hook::NativeDamageSnapshot* event) noexcept {
    using Reason = native_combat_hook::DiagnosticReason;
    if (reason != Reason::CallbackFault && reason != Reason::SnapshotFault && reason != Reason::InvalidClassification) return;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        Session* session = event ? CurrentSession(static_cast<BYTE*>(event->room)) : nullptr;
        if (session && !session->frozen) {
            session->combat.Invalidate("native combat collection failed; partial statistics cannot be paid");
            g_error = session->combat.Error();
        }
    } catch (...) {
        // Contribution collection cannot continue after an allocation failure,
        // but the independently validated operator reward filter remains safe.
        g_contribution_failure = true;
    }
}

bool Freeze(Session& session, BYTE* room, uint32_t duration) {
    if (session.frozen) return true;
    if (session.declined) { g_error = session.declined_error; return false; }
    if (!session.combat.Valid()) { g_error = session.combat.Error(); return false; }
    if (MissionMode(session.mode) && !session.boss_catalog_loaded) {
        g_error = "mission Boss identity registry is unavailable"; return false;
    }
    if (!g_runtime->SnapshotPolicy(&session.policy)) { g_error = g_runtime->LastError(); return false; }
    const WalletBinding wallet = ReadWalletBinding(g_runtime->RuntimeRoot());
    if (!wallet.valid) { g_error = wallet.error; return false; }
    native_stats::Participant roster[12]{};
    unsigned count = 0;
    if (!native_stats::ReadParticipants(room, roster, 12, &count)) { g_error = "unable to snapshot native participants"; return false; }
    unsigned humans = 0;
    std::array<uint32_t, 12> frozen_accounts{};
    std::set<uint32_t> accounts;
    for (unsigned i = 0; i < count; ++i) {
        if (roster[i].kind == native_stats::ParticipantKind::Human && roster[i].active) {
            if (!roster[i].account || !accounts.insert(roster[i].account).second) { g_error = "duplicate native wallet account"; return false; }
            frozen_accounts[humans] = roster[i].account;
            ParticipantStats attendance;
            attendance.active_seconds=duration;
            attendance.kills=roster[i].kills;
            if(!session.combat.Apply(roster[i].native_id,&attendance)){g_error="unable to freeze human contribution";return false;}
            const bool contributed=attendance.damage>0 || attendance.kills>0 || attendance.assists>0 || attendance.minion_kills>0 || attendance.boss_credit_micro>0;
            session.attendance.push_back({duration,contributed});
            ++humans;
        }
    }
    if (!humans || humans > 12 || duration > 86400) { g_error = "invalid native attendance/duration"; return false; }
    session.context.mode = MissionMode(session.mode) ? Mode::Pve : Mode::Pvp;
    session.context.human_count = humans;
    session.context.duration_seconds = duration;
    session.context.bosses_defeated = session.combat.BossesDefeated();
    SYSTEMTIME local{}; GetLocalTime(&local);
    session.context.event_multiplier = g_daily.Factor(local.wHour * 60u + local.wMinute);
    if (session.context.mode == Mode::Pve) {
        mission_native::MissionSettlementIdentity identity;
        auto reader = &ReadNativeBytes;
        if (mission_native::ReadMissionSettlement(room, reader, &identity) != mission_native::ReadError::None) {
            g_error = "unknown native mission difficulty or completion state"; return false;
        }
        session.context.difficulty = identity.difficulty;
        session.context.outcome = identity.outcome;
    }
    session.wallet_scope = wallet.scope;
    session.accounts = frozen_accounts;
    session.account_count = humans;
    session.completed_unix = job_detail::UnixNow();
    session.frozen = true;
    return true;
}

bool WriteG(BYTE* frame, unsigned site, uint16_t amount) {
    return WriteNativeGResult(frame, site, amount);
}

struct ContributionDecision {
    bool replace_gp = false;
    uint16_t gp = 0;
};

struct FilterDecision {
    uint32_t write_mask = 0;
    uint32_t pilot_exp = 0;
    uint32_t unit_exp = 0;
    uint32_t gp = 0;
};

bool ReadOperatorDefinition(BYTE* battle_pilot, void** definition) {
    uintptr_t value = 0;
    if (!definition ||
        !native_stats::Read(
            battle_pilot, kBattlePilotOperatorDefinition, &value)) {
        return false;
    }
    *definition = reinterpret_cast<void*>(value);
    return true;
}

bool ReadOperatorSettlementIdentity(
    const native_stats::NativeFrame& native,
    OperatorSettlementIdentity* identity) {
    if (!identity || !native.room || !native.account) return false;
    OperatorSettlementIdentity result;
    result.room = native.room;
    result.account = native.account;
    if (!native_stats::Read(native.room, 0, &result.room_id) ||
        !native_stats::Read(
            native.room, native_stats::kRoomStartedAtUnix,
            &result.started) ||
        result.started == 0) {
        return false;
    }
    *identity = result;
    return true;
}

bool SameOperatorSettlementIdentity(
    const OperatorSettlementIdentity& left,
    const OperatorSettlementIdentity& right) {
    return left.room == right.room &&
           left.room_id == right.room_id &&
           left.started == right.started &&
           left.account == right.account;
}

bool OperatorFilterAvailable() {
    return InterlockedCompareExchange(
               &g_operatorRewardFilterRegistration, 0, 0) == 2 &&
           InterlockedCompareExchangePointer(
               &g_operatorRewardFilterCallback, nullptr, nullptr) != nullptr;
}

bool ClaimOperatorFilterPhase(
    const OperatorSettlementIdentity& identity, uint32_t phase,
    bool mission) {
    const uint8_t phase_bit =
        phase == SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT
            ? static_cast<uint8_t>(1)
            : phase == SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS
                ? static_cast<uint8_t>(2)
                : static_cast<uint8_t>(0);
    if (!phase_bit) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t serial = ++g_operatorSettlementSerial;
    for (size_t i = 0; i < g_operatorSettlementCount; ++i) {
        OperatorSettlementRecord& record = g_operatorSettlements[i];
        if (!SameOperatorSettlementIdentity(record.identity, identity)) {
            continue;
        }
        record.serial = serial;
        if ((record.phases & phase_bit) != 0) return false;
        record.phases |= phase_bit;
        record.mission = record.mission || mission;
        return true;
    }

    size_t target = g_operatorSettlementCount;
    if (target < g_operatorSettlements.size()) {
        ++g_operatorSettlementCount;
    } else {
        target = 0;
        for (size_t i = 1; i < g_operatorSettlements.size(); ++i) {
            const OperatorSettlementRecord& candidate =
                g_operatorSettlements[i];
            const OperatorSettlementRecord& current =
                g_operatorSettlements[target];
            const bool candidate_complete =
                !candidate.mission || (candidate.phases & 2) != 0;
            const bool current_complete =
                !current.mission || (current.phases & 2) != 0;
            if ((candidate_complete && !current_complete) ||
                (candidate_complete == current_complete &&
                 candidate.serial < current.serial)) {
                target = i;
            }
        }
    }
    g_operatorSettlements[target] = {
        identity, phase_bit, mission, serial};
    return true;
}

FilterDecision DispatchOperatorRewardFilter(
    unsigned site, uint32_t phase, uint32_t writable_mask, BYTE* frame,
    const native_stats::NativeFrame& native,
    const NativeRewardAmounts& native_amounts,
    const NativeRewardAmounts& current_amounts, uint32_t gp_policy,
    void* operator_definition) {
    FilterDecision decision;
    decision.pilot_exp = current_amounts.pilot_exp;
    decision.unit_exp = current_amounts.unit_exp;
    decision.gp = current_amounts.gp;
    if (InterlockedCompareExchange(
            &g_operatorRewardFilterRegistration, 0, 0) != 2) {
        return decision;
    }
    const auto callback =
        reinterpret_cast<SdgoOperatorRewardFilterCallbackV1>(
            InterlockedCompareExchangePointer(
                &g_operatorRewardFilterCallback, nullptr, nullptr));
    if (!callback) return decision;
    void* user = InterlockedCompareExchangePointer(
        &g_operatorRewardFilterUser, nullptr, nullptr);
    const SdgoOperatorRewardContextV1 context = {
        sizeof(SdgoOperatorRewardContextV1),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        site,
        phase,
        writable_mask,
        frame,
        native.room,
        native.room_pilot,
        native.battle_pilot,
        operator_definition,
        native.account,
        static_cast<uint32_t>(
            native.kind == native_stats::ParticipantKind::Human
                ? SDGO_OPERATOR_REWARD_ROLE_HUMAN
                : SDGO_OPERATOR_REWARD_ROLE_AI),
        static_cast<uint32_t>(native.outcome),
        native.outcome == native_stats::NativeOutcome::WinOrClear ? 1u : 0u,
        native.native_eligible ? 1u : 0u,
        gp_policy,
        native_amounts.pilot_exp,
        native_amounts.pilot_event,
        native_amounts.unit_exp,
        native_amounts.unit_event,
        native_amounts.gp,
        native_amounts.gp_event,
        current_amounts.pilot_exp,
        current_amounts.pilot_event,
        current_amounts.unit_exp,
        current_amounts.unit_event,
        current_amounts.gp,
        current_amounts.gp_event};
    SdgoOperatorRewardResultV1 result = {
        sizeof(SdgoOperatorRewardResultV1),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        0,
        current_amounts.pilot_exp,
        current_amounts.unit_exp,
        current_amounts.gp};
    int status = SDGO_OPERATOR_REWARD_FILTER_REJECT;
    __try {
        status = callback(&context, &result, user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return decision;
    }
    if (status != SDGO_OPERATOR_REWARD_FILTER_APPLY ||
        result.size < sizeof(SdgoOperatorRewardResultV1) ||
        result.version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        (result.write_mask & ~writable_mask) != 0) {
        return decision;
    }
    decision.write_mask = result.write_mask;
    decision.pilot_exp = result.target_pilot_exp;
    decision.unit_exp = result.target_unit_exp;
    decision.gp = result.target_gp;
    return decision;
}

bool PrepareContribution(const native_stats::NativeFrame& native,
                         ContributionDecision* decision) {
    if (!decision) return false;
    *decision = {};
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_contract_failure) return false;
    if (g_contribution_failure) {
        g_error = "native contribution collection is unavailable";
        ++g_counters.payouts_rejected;
        return false;
    }
    Session* session = CurrentSession(native.room);
    if (!session || !Freeze(*session, native.room, native.duration_seconds)) {
        if (session && !session->declined) {
            session->declined = true;
            session->declined_error = g_error;
        }
        ++g_counters.settlements_failed;
        g_api->Log(
            g_mod, SDGO_LOG_WARN,
            "new reward settlement declined for account %u: %s "
            "(original game path retained)",
            native.account, g_error.c_str());
        return false;
    }
    if (std::find(
            session->accounts.begin(),
            session->accounts.begin() + session->account_count,
            native.account) ==
        session->accounts.begin() + session->account_count) {
        g_error = "settlement account changed after the match snapshot";
        ++g_counters.payouts_rejected;
        return false;
    }
    if (std::find(
            session->settled.begin(),
            session->settled.begin() + session->settled_count,
            native.account) !=
        session->settled.begin() + session->settled_count) {
        decision->replace_gp = g_gpPolicy == GpPolicy::Replace;
        ++g_counters.idempotent_replays;
        return true;
    }
    MatchContext context = session->context;
    if (context.mode == Mode::Pve) {
        const auto outcome =
            native.outcome == native_stats::NativeOutcome::WinOrClear
                ? Outcome::Clear
                : Outcome::Fail;
        if (outcome != context.outcome) {
            g_error = "native mission completion sources disagree";
            ++g_counters.settlements_failed;
            return false;
        }
    } else {
        context.outcome =
            native.outcome == native_stats::NativeOutcome::WinOrClear
                ? Outcome::Win
                : native.outcome == native_stats::NativeOutcome::LoseOrFail
                    ? Outcome::Lose
                    : Outcome::Draw;
    }
    ParticipantStats stats;
    stats.kind = ParticipantKind::Human;
    stats.active_seconds =
        (std::min)(native.duration_seconds, context.duration_seconds);
    stats.kills = native.kills;
    stats.is_mvp =
        context.mode == Mode::Pvp && native.native_rank_first;
    if (!session->combat.Apply(native.native_id, &stats)) {
        g_error = "native combat statistics unavailable";
        ++g_counters.payouts_rejected;
        return false;
    }
    Reward reward;
    Error error;
    if (!Calculate(
            session->policy, context, stats, &reward, &error,
            NativeSettlementLimits())) {
        g_error = error.message;
        ++g_counters.payouts_rejected;
        return false;
    }
    if (reward.eligible) {
        Pending pending;
        pending.job = {
            session->match_id, "", session->policy.revision.data(),
            session->wallet_scope, native.account,
            static_cast<uint32_t>(reward.mb)};
        pending.job.completed_unix = session->completed_unix;
        pending.job.active_seconds = stats.active_seconds;
        pending.job.contributed =
            stats.damage > 0 || stats.kills > 0 || stats.assists > 0 ||
            stats.minion_kills > 0 || stats.boss_credit_micro > 0;
        pending.job.mode = context.mode == Mode::Pvp ? "pvp" : "pve";
        pending.job.participants = session->attendance;
        const auto self = std::find(
            session->accounts.begin(),
            session->accounts.begin() + session->account_count,
            native.account);
        pending.job.participants[static_cast<size_t>(
            self - session->accounts.begin())] = {
            stats.active_seconds, pending.job.contributed};
        const std::string intent =
            pending.job.match_id + ":" + std::to_string(native.account) +
            ":" + pending.job.policy_revision + ":" +
            std::to_string(reward.gb) + ":" + std::to_string(reward.mb);
        if (!BossCatalog::Sha256(intent, &pending.job.plan_hash)) {
            g_error = "unable to hash native reward intent";
            return false;
        }
        if (!native_cash_notify::Capture(
                native.room_pilot, &pending.route)) {
            g_api->Log(
                g_mod, SDGO_LOG_INFO,
                "MB notification deferred to the native cash query for "
                "account %u (session route unavailable)",
                native.account);
        }
        pending.since = GetTickCount64();
        std::string name;
        if (!PublishWalletJob(
                g_runtime->RuntimeRoot(), pending.job, &name, &g_error)) {
            ++g_counters.payouts_rejected;
            return false;
        }
        if (g_pending.size() < 4096) {
            g_pending.push_back(std::move(pending));
        } else {
            g_api->Log(
                g_mod, SDGO_LOG_WARN,
                "cash notification deferred; earned job is durable and the "
                "next native cash query refreshes its balance");
        }
    }
    decision->replace_gp = g_gpPolicy == GpPolicy::Replace;
    decision->gp = static_cast<uint16_t>(reward.gb);
    session->settled[session->settled_count++] = native.account;
    if (!reward.eligible) ++g_counters.payouts_committed;
    ++g_counters.settlements_committed;
    if (g_trace) {
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "native reward prepared match %s account %u G=%d (%s) MB=%d "
            "event=%u humans=%u policy=%s",
            session->match_id.c_str(), native.account, reward.gb,
            decision->replace_gp ? "replace" : "native", reward.mb,
            context.event_multiplier, context.human_count,
            session->policy.revision.data());
    }
    return true;
}

bool TryPrepareContribution(const native_stats::NativeFrame& native,
                            ContributionDecision* decision) noexcept {
    try {
        return PrepareContribution(native, decision);
    } catch (...) {
        g_contribution_failure = true;
        if (decision) *decision = {};
        return false;
    }
}

bool ApplyFilterAndGp(BYTE* frame, unsigned site, uint32_t phase,
                      uint32_t writable_mask,
                      const native_stats::NativeFrame& native,
                      const NativeRewardAmounts& native_amounts,
                      const ContributionDecision& contribution,
                      bool definition_read, void* operator_definition,
                      const OperatorSettlementIdentity* identity,
                      bool mission) {
    NativeRewardAmounts current = native_amounts;
    if (contribution.replace_gp) {
        current.gp = contribution.gp;
        current.gp_event = 0;
    }
    FilterDecision filter;
    const bool run_filter =
        definition_read && identity && OperatorFilterAvailable() &&
        ClaimOperatorFilterPhase(*identity, phase, mission);
    if (run_filter) {
        filter = DispatchOperatorRewardFilter(
            site, phase, writable_mask, frame, native, native_amounts, current,
            contribution.replace_gp ? SDGO_OPERATOR_REWARD_GP_REPLACE
                                    : SDGO_OPERATOR_REWARD_GP_NATIVE,
            operator_definition);
    } else {
        filter.pilot_exp = current.pilot_exp;
        filter.unit_exp = current.unit_exp;
        filter.gp = current.gp;
    }
    const uint32_t exp_mask =
        filter.write_mask &
        (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
         SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP);
    if (exp_mask &&
        !WriteNativeExpTargets(
            frame, site, exp_mask, filter.pilot_exp, filter.unit_exp)) {
        return false;
    }
    const bool filter_gp =
        (filter.write_mask & SDGO_OPERATOR_REWARD_FIELD_GP) != 0;
    if (writable_mask & SDGO_OPERATOR_REWARD_FIELD_GP) {
        if (contribution.replace_gp) {
            if (!WriteG(
                    frame, site,
                    SaturateReward16(
                        filter_gp ? filter.gp : contribution.gp))) {
                return false;
            }
        } else if (filter_gp &&
                   !WriteNativeGpTarget(frame, site, filter.gp)) {
            return false;
        }
    }
    return true;
}

void LatchContractFailure(const char* error, bool count_settlement) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (error) g_error = error;
    g_contract_failure = true;
    if (count_settlement) ++g_counters.settlements_failed;
}

void SettleMissionFinal(BYTE* frame) {
    MissionPending pending = g_missionPending;
    g_missionPending = {};
    if (!pending.active) return;
    native_stats::NativeFrame native;
    native_stats::CaptureError capture;
    if (!native_stats::ReadSettlementFrame(9, frame, &native, &capture) ||
        pending.frame != frame || pending.room != native.room ||
        pending.room_pilot != native.room_pilot ||
        pending.battle_pilot != native.battle_pilot ||
        pending.account != native.account) {
        LatchContractFailure(
            "native settlement frame did not match the verified contract",
            true);
        return;
    }
    NativeRewardAmounts amounts;
    if (!ReadNativeRewardAmounts(frame, 9, &amounts)) {
        LatchContractFailure(
            "unable to read final native mission rewards", true);
        return;
    }
    void* current_definition = nullptr;
    const bool definition_read =
        ReadOperatorDefinition(native.battle_pilot, &current_definition);
    const bool stable_definition =
        pending.definition_read && definition_read &&
        pending.operator_definition == current_definition;
    OperatorSettlementIdentity identity;
    const bool identity_read =
        ReadOperatorSettlementIdentity(native, &identity);
    const bool stable_identity =
        pending.identity_read && identity_read &&
        SameOperatorSettlementIdentity(pending.identity, identity);
    ContributionDecision contribution;
    contribution.replace_gp = pending.replace_gp;
    contribution.gp = pending.replacement_gp;
    uint32_t writable_mask = SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP;
    if (pending.gp_writable) {
        writable_mask |= SDGO_OPERATOR_REWARD_FIELD_GP;
    }
    if (!ApplyFilterAndGp(
            frame, 9, SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS,
            writable_mask,
            native, amounts, contribution, stable_definition,
            pending.operator_definition,
            stable_identity ? &pending.identity : nullptr, true)) {
        LatchContractFailure(
            "unable to write final native mission rewards", true);
    }
}

void SettleImpl(unsigned site, BYTE* frame) {
    if (g_contract_failure.load(std::memory_order_acquire)) {
        if (site == kSettlementSites.size()) g_missionPending = {};
        return;
    }
    if (site == kSettlementSites.size()) {
        SettleMissionFinal(frame);
        return;
    }
    native_stats::NativeFrame native;
    native_stats::CaptureError capture;
    if (!native_stats::ReadSettlementFrame(site, frame, &native, &capture)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_error =
            "native settlement frame did not match the verified contract";
        g_contract_failure = true;
        ++g_counters.settlements_failed;
        return;
    }
    if (g_trace &&
        g_nativeSettlementSamples.fetch_add(
            1, std::memory_order_relaxed) < 64) {
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "native settlement sample site=%u account=%u native_id=%u "
            "kind=%u eligible=%u state=%u duration=%u",
            site, native.account, native.native_id,
            static_cast<unsigned>(native.kind),
            native.native_eligible ? 1u : 0u,
            static_cast<unsigned>(native.pilot_state),
            native.duration_seconds);
    }
    if (native.kind != native_stats::ParticipantKind::Human) {
        return;
    }
    NativeRewardAmounts amounts;
    if (!ReadNativeRewardAmounts(frame, site, &amounts)) {
        LatchContractFailure(
            "unable to read native settlement rewards", true);
        return;
    }
    void* operator_definition = nullptr;
    const bool definition_read =
        ReadOperatorDefinition(native.battle_pilot, &operator_definition);
    OperatorSettlementIdentity identity;
    const bool identity_read =
        ReadOperatorSettlementIdentity(native, &identity);
    ContributionDecision contribution;
    const bool contribution_prepared =
        native.native_eligible &&
        TryPrepareContribution(native, &contribution);
    if (native.native_eligible && !contribution_prepared && g_trace) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            error = g_error;
        }
        g_api->Log(
            g_mod, SDGO_LOG_WARN,
            "native reward preparation skipped for account %u: %s",
            native.account,
            error.empty() ? "unspecified contribution failure"
                          : error.c_str());
    }
    if (site == 9) {
        if (g_missionPending.active) {
            LatchContractFailure(
                "nested native mission settlement frame", true);
            return;
        }
        g_missionPending = {};
        g_missionPending.active = true;
        g_missionPending.replace_gp = contribution.replace_gp;
        g_missionPending.gp_writable = contribution_prepared;
        g_missionPending.definition_read = definition_read;
        g_missionPending.identity_read = identity_read;
        g_missionPending.frame = frame;
        g_missionPending.room = native.room;
        g_missionPending.room_pilot = native.room_pilot;
        g_missionPending.battle_pilot = native.battle_pilot;
        g_missionPending.operator_definition = operator_definition;
        g_missionPending.identity = identity;
        g_missionPending.account = native.account;
        g_missionPending.replacement_gp = contribution.gp;
        if (!ApplyFilterAndGp(
                frame, site,
                SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT,
                SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP, native, amounts,
                contribution, definition_read, operator_definition,
                identity_read ? &identity : nullptr, true)) {
            LatchContractFailure(
                "unable to write early native mission rewards", true);
        }
        return;
    }
    uint32_t writable_mask =
        SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
        SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP;
    if (contribution_prepared) {
        writable_mask |= SDGO_OPERATOR_REWARD_FIELD_GP;
    }
    if (!ApplyFilterAndGp(
            frame, site, SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT,
            writable_mask,
            native, amounts, contribution, definition_read,
            operator_definition,
            identity_read ? &identity : nullptr, false)) {
        LatchContractFailure(
            "unable to write native settlement rewards", true);
    }
}

void __stdcall Settle(unsigned site, BYTE* frame) noexcept {
    try { SettleImpl(site, frame); }
    catch (...) {
        LatchContractFailure(
            "native settlement callback raised an exception", true);
    }
}

void ReadReceipts() {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        WalletReceipt receipt;
        std::string error;
        if (!ReadWalletReceipt(g_runtime->RuntimeRoot(), it->job, &receipt, &error)) {
            if (error != "wallet receipt pending") g_error = error;
            ++it; continue;
        }
        const auto now = job_detail::UnixNow();
        auto notified = native_cash_notify::NotifyResult::StaleSession;
        // Never replay a historical absolute balance into an active wallet UI.
        if (WalletReceiptFresh(receipt, now, 15)) {
            if(receipt.has_current_gb) native_cash_notify::NotifyCurrentGb(it->route,it->job.account,receipt.current_gb);
            notified = native_cash_notify::NotifyCurrentMb(it->route, it->job.account, receipt.current_mb);
        }
        if ((notified == native_cash_notify::NotifyResult::TransportFailure || notified == native_cash_notify::NotifyResult::BindingUnavailable) &&
            GetTickCount64() - it->since < 15000) { ++it; continue; }
        ++g_counters.payouts_committed;
        if (receipt.already_applied) ++g_counters.idempotent_replays;
        it = g_pending.erase(it);
    }
}

DWORD WINAPI Worker(void*) {
    for (;;) {
        try {
            std::string policyError;
            g_runtime->ReloadPolicy(&policyError);
            WalletHealth wallet = ReadWalletHealth(g_runtime->RuntimeRoot());
            g_runtime->SetWalletWorkerHealth(wallet.healthy, wallet.revision, wallet.error);
            BossCatalog catalog;
            std::string bossError;
            bool bossesLoaded = catalog.LoadFile((g_runtime->RuntimeRoot() / L"reward-bosses.txt").c_str(), &bossError);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_bosses_loaded = bossesLoaded;
                if (bossesLoaded) g_bosses = std::move(catalog);
                // A corrected config/worker connection may recover without a
                // server restart. A mismatched native frame stays latched.
                if (!g_contract_failure && wallet.healthy && policyError.empty() && bossesLoaded) g_error.clear();
                ReadReceipts();
                g_runtime->SetPayoutCounters(g_counters);
                const bool network = NetworkReady();
                g_runtime->SetSettlementReady(HooksActive() && network && bossesLoaded && !g_contract_failure &&
                                                  !g_contribution_failure && g_error.empty(),
                    !bossesLoaded ? bossError : !network ? "native network framework is not ready" :
                    g_contribution_failure ? "native contribution collection is unavailable" :
                    g_contract_failure && g_error.empty() ? "native reward capture failed; inspect log and restart" : g_error);
            }
            SYSTEMTIME local{}; GetLocalTime(&local);
            char from[6], to[6];
            sprintf_s(from, "%02u:%02u", g_daily.start / 60, g_daily.start % 60);
            sprintf_s(to, "%02u:%02u", g_daily.end / 60, g_daily.end % 60);
            g_runtime->SetActivityWindow(from, to, g_daily.Factor(local.wHour * 60 + local.wMinute) == 2);
            g_runtime->WriteHeartbeat();
        } catch (...) {
            g_runtime->SetSettlementReady(false, "native reward worker failure");
            g_runtime->WriteHeartbeat();
        }
        Sleep(1000);
    }
}

bool Settings(const SdgoApi* api, SdgoMod* mod) {
    sdgo::ModSettings settings(api, mod);
    char root[4096] = {}, identity[96] = {}, gpPolicy[16] = {};
    if (settings.String("RuntimeDirectory", "", root, sizeof(root)) >= sizeof(root) - 1 || !*root ||
        settings.String("InstanceId", "", identity, sizeof(identity)) >= sizeof(identity) - 1 ||
        settings.String("GpPolicy", "replace", gpPolicy, sizeof(gpPolicy)) >= sizeof(gpPolicy) - 1)
        return false;
    if (!strcmp(gpPolicy, "replace")) g_gpPolicy = GpPolicy::Replace;
    else if (!strcmp(gpPolicy, "native")) g_gpPolicy = GpPolicy::Native;
    else return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, nullptr, 0);
    if (n < 2) return false;
    std::wstring runtime(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, runtime.data(), n);
    runtime.pop_back();
    wchar_t exe[32768] = {}; GetModuleFileNameW(nullptr, exe, 32768);
    fs::path directory = fs::path(exe).parent_path();
    fs::path runtimePath(runtime);
    if (runtimePath.is_relative()) runtimePath = directory / runtimePath;
    std::string instance(identity);
    if (instance.empty()) {
        std::string hash;
        std::wstring path(exe);
        if (!BossCatalog::Sha256(std::string(reinterpret_cast<const char*>(path.data()), path.size() * sizeof(wchar_t)), &hash)) return false;
        instance = "battle-" + hash.substr(0, 16);
    }
    std::array<UCHAR, 32> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !BossCatalog::Sha256(std::string(reinterpret_cast<const char*>(random.data()), random.size()), &g_boot)) return false;
    g_runtime = std::make_unique<RuntimeState>(runtimePath.wstring(), instance, RuntimeRootKind::ExplicitRuntimeDirectory);
    g_pending.reserve(4096); // no allocation after a durable MB job is published
    std::string error;
    if (!g_runtime->ReloadPolicy(&error)) {
        api->Log(mod, SDGO_LOG_ERROR, "reward policy rejected: %s", error.c_str());
        g_runtime->SetSettlementReady(false, error); g_runtime->WriteHeartbeat(); return false;
    }
    if (!g_daily.Load(GetModuleHandleW(L"daily_rewards.dll"))) return false;
    char trace[8] = {};
    settings.String("Trace", "0", trace, sizeof(trace));
    if (strcmp(trace, "0") && strcmp(trace, "1")) return false;
    g_trace = trace[0] == '1';
    return true;
}
}

extern "C" __declspec(dllexport) int __cdecl
SdgoBattleRewardsRegisterDamageObserverV1(
    const SdgoBattleRewardsDamageObserverSinkV1* sink) {
    if (!sink || sink->size < sizeof(*sink) ||
        sink->version !=
            SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION ||
        !sink->callback) {
        return 0;
    }
    if (InterlockedCompareExchange(
            &g_damageObserverRegistration, 1, 0) != 0) {
        return 0;
    }
    InterlockedExchangePointer(&g_damageObserverUser, sink->user);
    InterlockedExchangePointer(
        &g_damageObserverCallback,
        reinterpret_cast<PVOID>(sink->callback));
    InterlockedExchange(&g_damageObserverRegistration, 2);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl
SdgoBattleRewardsRegisterOperatorRewardFilterV1(
    const SdgoOperatorRewardFilterSinkV1* sink) {
    if (!sink || sink->size < sizeof(*sink) ||
        sink->version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        !sink->callback) {
        return 0;
    }
    if (InterlockedCompareExchange(
            &g_settlementHooksReady, 0, 0) != 1) {
        return 0;
    }
    if (InterlockedCompareExchange(
            &g_operatorRewardFilterRegistration, 1, 0) != 0) {
        return 0;
    }
    InterlockedExchangePointer(&g_operatorRewardFilterUser, sink->user);
    InterlockedExchangePointer(
        &g_operatorRewardFilterCallback,
        reinterpret_cast<PVOID>(sink->callback));
    InterlockedExchange(&g_operatorRewardFilterRegistration, 2);
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION || api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->HookSite) return 0;
    g_api = api; g_mod = mod;
    if (!Settings(api, mod)) { api->Log(mod, SDGO_LOG_ERROR, "configure RuntimeDirectory using the owner reward assistant first"); return 0; }
    native_cash_notify::net_frame_proc_global = static_cast<void**>(api->Symbol(mod, "battle.rewards.net_frame_proc"));
    native_cash_notify::send_to_server = reinterpret_cast<native_cash_notify::SendToServerFn>(api->Symbol(mod, "battle.rewards.net_send"));
    if (!native_cash_notify::net_frame_proc_global || !native_cash_notify::send_to_server) return 0;
    native_combat_hook::event_gate = &EventGate;
    native_combat_hook::classify_victim = &Classify;
    native_combat_hook::record_sink = &Record;
    native_combat_hook::diagnostic_sink = &CombatDiagnostic;
    native_combat_hook::damage_observer = &ForwardDamageObserver;
    bridge::callback = &Settle;
    for (unsigned i = 0; i < kSettlementSites.size(); ++i) {
        g_sites[i] = static_cast<BYTE*>(api->Symbol(mod, kSettlementSites[i].symbol));
        if (!g_sites[i] || !api->HookSite(mod, kSettlementSites[i].symbol, bridge::detours[i], bridge::originals[i])) return 0;
    }
    g_sites[10] = static_cast<BYTE*>(api->Symbol(mod, kMissionFinalSymbol));
    if (!g_sites[10] ||
        !api->HookSite(
            mod, kMissionFinalSymbol, bridge::detours[10],
            bridge::originals[10])) {
        return 0;
    }
    if (LoadEligibilityGate(&g_eligibilityGate)) {
        BYTE* eligibilitySite =
            static_cast<BYTE*>(api->Symbol(mod, kEligibilitySymbol));
        if (!eligibilitySite) {
            api->Log(
                mod, SDGO_LOG_WARN,
                "GameTest eligibility observer inactive: proposed native symbol is unavailable");
        } else if (!api->HookSite(
                       mod, kEligibilitySymbol,
                       reinterpret_cast<void*>(&eligibility_observer::bridge::Site),
                       &eligibility_observer::bridge::original)) {
            api->Log(
                mod, SDGO_LOG_WARN,
                "GameTest eligibility observer inactive: native hook installation failed");
        } else {
            eligibility_observer::bridge::callback = &ObserveEligibility;
            api->Log(
                mod, SDGO_LOG_INFO,
                "GameTest eligibility observer active for run %s and title nonce %s",
                g_eligibilityGate.run_uuid, g_eligibilityGate.title_nonce);
        }
    }
    g_sites[11] = static_cast<BYTE*>(api->Symbol(mod, "battle.rewards.record_damage"));
    if (!g_sites[11] || !api->HookCode(mod, "battle.rewards.record_damage", reinterpret_cast<void*>(&native_combat_hook::Detour), reinterpret_cast<void**>(&native_combat_hook::original))) return 0;
    HANDLE thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!thread) return 0;
    if (!CloseHandle(thread)) return 0;
    InterlockedExchange(&g_settlementHooksReady, 1);
    api->Log(
        mod, SDGO_LOG_INFO,
        "native battle reward integration active; G policy=%s, MB uses the "
        "owner durable native wallet worker",
        g_gpPolicy == GpPolicy::Replace ? "replace" : "native");
    return 1;
}
