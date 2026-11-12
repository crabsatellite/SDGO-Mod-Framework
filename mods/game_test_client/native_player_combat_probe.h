#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sdgo/game_test.h"
#include "sdgo/mission_ai_client_observer.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "sdgo/sdgo_api.h"

#include "scoreboard_actions.h"
#include "../battle_ai/native_capabilities.h"

// Opt-in native player input for an isolated AI battle GameTest. The probe
// resolves the authenticated pilot's real local unit, supplies mapped forward
// and primary-fire actions to the original input consumers, and dispatches the
// resulting native FSM work through the unit's real control context. It never
// writes HP, position, controller flags, FSM state or weapon data.
//
// With BattlePlayerSpecial=1 the probe also fires the special attack. Charging
// the SP gauge (CUnit+0xBC8, 0..30000) in combat takes about 27,300 damage
// dealt, which a short isolated battle does not provide, so the special mode is
// a declared GameTest fixture: once a living enemy AI of the PvP snapshot is
// within the special weapon's reach, it fills the local gauge to 30000 (the
// only gameplay value it writes; the Battle host's own check is filled by the
// matching development-only server fixture). The mapped special-slot action 17
// then goes to the original weapon input consumer, which selects slot 3 only
// with a full gauge, and the attack FSM is dispatched until it binds that
// slot's AttackStyle-2 weapon. Primary input (plus the lock action for the
// melee families) fires the special until the caller's special-event source
// reports a new local special computation, or the attempt times out and the
// next one waits for a target again.
class NativePlayerCombatProbe
{
public:
    // Reads the cumulative count of local special computations (the OC client
    // observer's special_events). False when the source is unavailable.
    using SpecialEventsFn = bool(__cdecl*)(uint32_t* events);

private:
    using ObjectIdFromPilotFn =
        uint32_t(__thiscall*)(void*, uint32_t);
    using FindObjectFn =
        uint8_t*(__thiscall*)(void*, uint32_t);
    // Both consumers and both FSM dispatch slots take one stack argument and
    // return with RET 4. Their native owner is supplied through ECX.
    using InputBusinessFn =
        void(__thiscall*)(uint8_t*, void*);
    using WeaponInputFn = void(__thiscall*)(uint8_t*, void*, int);
    using NativeTickFn =
        void(__thiscall*)(uint8_t*, void*);

    static constexpr uint32_t kForwardAction = 0;
    static constexpr uint32_t kPrimaryAction = 9;
    static constexpr uint32_t kLockAction = 10;
    static constexpr uint64_t kCycleMs = 900;
    static constexpr uint64_t kForwardHeldMs = 180;
    static constexpr uint64_t kPrimaryHeldMs = 360;

    static constexpr size_t kManagerLocalPlayer = 0x88;
    static constexpr size_t kUnitObjectId = 0x04;
    static constexpr size_t kUnitNode = 0x08;
    static constexpr size_t kUnitHp = 0x38;
    static constexpr size_t kUnitMoveFsm = 0x9C8;
    static constexpr size_t kUnitAttackFsm = 0x9CC;
    static constexpr size_t kUnitControlContext = 0xDC0;
    static constexpr size_t kUnitPilotId = 0x2B68;
    static constexpr size_t kUnitPlayerControlEnabled = 0x31E9;
    static constexpr size_t kMoveOwner = 0x21C;
    static constexpr size_t kNodePosition = 0x88;
    static constexpr size_t kUnitTerrainArea = 0x3148;   // left x, right x, top z, bottom z, then u8 on

    // Special attack (BattlePlayerSpecial).
    enum class SpecialPhase : uint8_t { Charge, Select, Fire, Done };
    enum class SpecialStep : uint8_t { Control, Wait, Fail };
    static constexpr size_t kUnitDefinition = 0x9C4;
    static constexpr size_t kUnitSpecialGauge = 0xBC8;
    static constexpr int32_t kSpecialGaugeFull = 30000;
    static constexpr int32_t kSpecialSlot = 3;
    static constexpr uint32_t kSpecialSelectAction = 14u + kSpecialSlot;
    static constexpr size_t kDefinitionSpecialWeapon = 0x249 + kSpecialSlot * 4;
    static constexpr size_t kAttackSelectedSlot = 0x240;
    static constexpr size_t kAttackSelectedWeapon = 0x248;
    static constexpr size_t kWeaponAttackStyle = 0x69;
    static constexpr size_t kWeaponRangeType = 0x6A;
    static constexpr size_t kWeaponDamageSize = 0x6C;
    static constexpr size_t kWeaponRangeSize = 0x72;
    static constexpr uint64_t kSpecialSelectMs = 6000;
    static constexpr uint64_t kSpecialFireMs = 20000;
    static constexpr uint32_t kSpecialAttempts = 10;
    // Target gate: the special weapon's own RangeSize, at least kSpecialMinReach;
    // after kSpecialRelaxMs without a target in reach, twice that.
    static constexpr float kSpecialMinReach = 300.0f;
    static constexpr float kSpecialDefaultReach = 1000.0f;
    static constexpr uint64_t kSpecialRelaxMs = 60000;
    static constexpr uint64_t kSpecialReportMs = 10000;

    void** unitManager_ = nullptr;
    ObjectIdFromPilotFn objectIdFromPilot_ = nullptr;
    FindObjectFn findObject_ = nullptr;
    InputBusinessFn moveInput_ = nullptr;
    InputBusinessFn fireInput_ = nullptr;
    InputBusinessFn lockInput_ = nullptr;
    WeaponInputFn weaponInput_ = nullptr;

    bool enabled_ = false;
    bool continuousPrimary_ = false;
    bool lockOn_ = false;
    int requestedWeaponSlot_ = -1;
    bool weaponReported_ = false;
    bool ammoBaselineValid_ = false;
    bool ammoSpent_ = false;
    bool sampledAmmoReported_ = false;
    int16_t previousAmmo_ = 0;
    uint32_t sampledUnit_ = 0, sampledWeapon_ = 0;
    bool sampledAlternate_ = false;
    uint8_t* sampledAttack_ = nullptr;
    bool controlsActive_ = false;
    bool forwardHeld_ = false;
    bool primaryHeld_ = false;
    bool baselineValid_ = false;
    bool inputObserved_ = false;
    bool movementObserved_ = false;
    bool attackObserved_ = false;
    bool movementReported_ = false;
    bool attackReported_ = false;
    bool finished_ = false;
    uint32_t battleSequence_ = 0;
    uint32_t boundObjectId_ = 0;
    uint8_t* boundUnit_ = nullptr;
    uint64_t controlStartedAt_ = 0;
    uint32_t bursts_ = 0;
    uint32_t consumedMoveQueries_ = 0;
    uint32_t consumedFireQueries_ = 0;
    uint32_t dispatches_ = 0;
    float baseline_[3] = {};
    // Probe time: each case's context restarts elapsed_ms, and a battle can be
    // polled from more than one case (oc.special, then pvp.return).
    bool clockStarted_ = false;
    uint64_t clock_ = 0;
    uint64_t lastElapsed_ = 0;

    bool special_ = false;
    SpecialEventsFn specialEvents_ = nullptr;
    SpecialPhase specialPhase_ = SpecialPhase::Charge;
    uint64_t specialPhaseAt_ = 0;
    uint32_t specialAttempts_ = 0;
    uint32_t specialSelectInputs_ = 0;
    uint32_t specialEventsBefore_ = 0;
    uint32_t specialEventsAfter_ = 0;
    int32_t specialGauge_ = 0;
    int32_t specialGaugeMax_ = 0;
    int32_t specialGaugeAtSelect_ = 0;
    uint32_t specialUnitData_ = 0;
    uint32_t specialWeapon_ = 0;
    uint8_t specialFamily_ = 0;
    int32_t specialDamageSize_ = 0;
    bool specialBound_ = false;
    bool specialObserved_ = false;
    uint32_t specialFills_ = 0;
    float specialReach_ = 0.0f;
    float specialDistance_ = -1.0f;
    uint32_t specialTargetPilot_ = 0;
    uint64_t specialWaitAt_ = 0;
    uint64_t specialReportAt_ = 0;
    bool specialWaiting_ = false;
    bool specialRelaxed_ = false;
    // The PvP snapshot of the current Poll (enemy positions); null for missions.
    const SdgoPvpAiClientSnapshotV1* pvpSnapshot_ = nullptr;

    void Release(ScoreboardActions& input)
    {
        input.HoldGameplay(kForwardAction, false);
        input.HoldGameplay(kPrimaryAction, false);
        input.HoldGameplay(kLockAction, false);
        if (requestedWeaponSlot_ >= 0)
            input.HoldGameplay(14u + requestedWeaponSlot_, false);
        if (special_)
            input.HoldGameplay(kSpecialSelectAction, false);
        forwardHeld_ = false;
        primaryHeld_ = false;
        controlsActive_ = false;
        baselineValid_ = false;
        boundObjectId_ = 0;
        boundUnit_ = nullptr;
        weaponReported_ = false;
        ammoBaselineValid_ = false;
        sampledAmmoReported_ = false;
    }

    void BeginBattle(
        uint32_t sequence, ScoreboardActions& input)
    {
        Release(input);
        battleSequence_ = sequence;
        inputObserved_ = false;
        movementObserved_ = false;
        attackObserved_ = false;
        movementReported_ = false;
        attackReported_ = false;
        finished_ = false;
        ammoSpent_ = false;
        bursts_ = 0;
        consumedMoveQueries_ = 0;
        consumedFireQueries_ = 0;
        dispatches_ = 0;
        specialPhase_ = SpecialPhase::Charge;
        specialPhaseAt_ = 0;
        specialAttempts_ = 0;
        specialSelectInputs_ = 0;
        specialEventsBefore_ = 0;
        specialEventsAfter_ = 0;
        specialGauge_ = 0;
        specialGaugeMax_ = 0;
        specialGaugeAtSelect_ = 0;
        specialUnitData_ = 0;
        specialWeapon_ = 0;
        specialFamily_ = 0;
        specialDamageSize_ = 0;
        specialBound_ = false;
        specialObserved_ = false;
        specialFills_ = 0;
        specialReach_ = 0.0f;
        specialDistance_ = -1.0f;
        specialTargetPilot_ = 0;
        specialWaitAt_ = 0;
        specialReportAt_ = 0;
        specialWaiting_ = false;
        specialRelaxed_ = false;
    }

    static bool MeleeFamily(uint8_t rangeType)
    {
        return rangeType == 1 || rangeType == 8;
    }

    // Monotonic probe time in milliseconds. It equals elapsed_ms while one case
    // polls the probe; a later case's restarted elapsed_ms continues it.
    uint64_t Now(const SdgoGameTestContextV1* context)
    {
        const uint64_t elapsed = context->elapsed_ms;
        if (!clockStarted_) {
            clockStarted_ = true;
            clock_ = elapsed;
        } else {
            clock_ += elapsed >= lastElapsed_ ? elapsed - lastElapsed_ : elapsed;
        }
        lastElapsed_ = elapsed;
        return clock_;
    }

    // The special weapon's RangeSize (+0x72) from the live weapon bank, at
    // least kSpecialMinReach; kSpecialDefaultReach while the bank is unreadable.
    float SpecialReach(uint8_t* unit)
    {
        if (specialReach_ > 0.0f)
            return specialReach_;
        battle_ai::native::Capabilities caps{};
        if (!battle_ai::native::ReadCapabilities(unit, caps))
            return kSpecialDefaultReach;
        const auto& weapon = caps.weapons[kSpecialSlot];
        if (!weapon.id || weapon.attackStyle != 2)
            return kSpecialDefaultReach;
        specialReach_ = weapon.range > kSpecialMinReach ? weapon.range : kSpecialMinReach;
        return specialReach_;
    }

    // Horizontal distance from the local scene node to the nearest living enemy
    // AI of the current PvP snapshot (its live scene node when readable).
    bool NearestEnemy(void* manager, uint8_t* unit, float& distance, uint32_t& pilot) const
    {
        using namespace battle_ai::native::detail;
        const SdgoPvpAiClientSnapshotV1* snapshot = pvpSnapshot_;
        uint8_t* node = nullptr;
        float x = 0.0f, z = 0.0f;
        if (!snapshot || !manager || !findObject_ ||
            (snapshot->local_team != 1 && snapshot->local_team != 2) ||
            !ReadPointer(unit, kUnitNode, node) ||
            !Read(node, kNodePosition, x) || !Read(node, kNodePosition + 8, z) ||
            !std::isfinite(x) || !std::isfinite(z))
            return false;
        bool found = false;
        for (uint32_t i = 0; i < snapshot->bot_count && i < 12; ++i) {
            const auto& bot = snapshot->bots[i];
            if (bot.team != 3 - snapshot->local_team || !bot.valid_registered_object ||
                !bot.position_valid || !bot.object_id)
                continue;
            uint8_t* enemy = findObject_(manager, bot.object_id);
            int32_t hp = 0;
            if (!enemy || !Read(enemy, kUnitHp, hp) || hp <= 0)
                continue;
            float ex = bot.position_x, ez = bot.position_z;
            uint8_t* enemyNode = nullptr;
            float liveX = 0.0f, liveZ = 0.0f;
            if (ReadPointer(enemy, kUnitNode, enemyNode) &&
                Read(enemyNode, kNodePosition, liveX) && Read(enemyNode, kNodePosition + 8, liveZ) &&
                std::isfinite(liveX) && std::isfinite(liveZ)) {
                ex = liveX;
                ez = liveZ;
            }
            const float dx = ex - x, dz = ez - z;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (!std::isfinite(d))
                continue;
            if (!found || d < distance) {
                distance = d;
                pilot = bot.pilot_id;
                found = true;
            }
        }
        return found;
    }

    // One tick of the special attack. Control: run the ordinary control this
    // tick (waiting for a target, or firing the bound special). Wait: this tick
    // only supplied the mapped special-slot selection. Fail: the case fails.
    SpecialStep PollSpecial(
        const SdgoGameTestContextV1* context, uint64_t now,
        void* manager, uint8_t* unit, uint8_t* attack,
        NativeTickFn dispatchAttack, void* nativeInput,
        ScoreboardActions& input)
    {
        using namespace battle_ai::native::detail;
        int32_t gauge = 0;
        if (!Read(unit, kUnitSpecialGauge, gauge))
            return SpecialStep::Control;
        specialGauge_ = gauge;
        if (gauge > specialGaugeMax_)
            specialGaugeMax_ = gauge;
        char text[256] = {};

        if (specialPhase_ == SpecialPhase::Charge) {
            if (specialAttempts_ >= kSpecialAttempts)
                return SpecialStep::Control;
            if (!specialWaiting_) {
                specialWaiting_ = true;
                specialRelaxed_ = false;
                specialWaitAt_ = now;
                specialReportAt_ = now;
            }
            const float reach = SpecialReach(unit) * (specialRelaxed_ ? 2.0f : 1.0f);
            if (pvpSnapshot_) {
                float distance = -1.0f;
                uint32_t pilot = 0;
                const bool seen = NearestEnemy(manager, unit, distance, pilot);
                specialDistance_ = seen ? distance : -1.0f;
                specialTargetPilot_ = seen ? pilot : 0;
                if (!seen || distance > reach) {
                    if (!specialRelaxed_ && now - specialWaitAt_ >= kSpecialRelaxMs) {
                        specialRelaxed_ = true;
                        sprintf_s(text,
                            "native player special: no enemy within reach=%.0f for %llu ms; the reach doubles for this attempt",
                            reach, static_cast<unsigned long long>(now - specialWaitAt_));
                        context->Observe(text);
                    }
                    if (now - specialReportAt_ >= kSpecialReportMs) {
                        specialReportAt_ = now;
                        sprintf_s(text,
                            "native player special waits for an enemy within reach=%.0f nearest=%.0f pilot=%08X sp=%d attempts=%u",
                            reach, specialDistance_, specialTargetPilot_, gauge, specialAttempts_);
                        context->Observe(text);
                    }
                    return SpecialStep::Control;
                }
            }
            uint32_t events = 0;
            if (!specialEvents_ || !specialEvents_(&events)) {
                context->Assert(0, "the native special probe can read the OC client's local special computations");
                Release(input);
                return SpecialStep::Fail;
            }
            // GameTest fixture: fill the local gauge for the native client gates
            // (slot-3 selection and special admission).
            if (gauge < kSpecialGaugeFull) {
                const int32_t full = kSpecialGaugeFull;
                std::memcpy(unit + kUnitSpecialGauge, &full, sizeof(full));
                ++specialFills_;
            }
            specialEventsBefore_ = events;
            specialGaugeAtSelect_ = gauge;
            specialWaiting_ = false;
            ++specialAttempts_;
            specialPhase_ = SpecialPhase::Select;
            specialPhaseAt_ = now;
            sprintf_s(text,
                "native player special attempt=%u target=%08X distance=%.0f reach=%.0f: fixture SP %d -> %d, "
                "mapped action 17 selects the special slot (events=%u)",
                specialAttempts_, specialTargetPilot_, specialDistance_, reach, gauge,
                gauge < kSpecialGaugeFull ? kSpecialGaugeFull : gauge, events);
            context->Observe(text);
            if (gauge < kSpecialGaugeFull)
                gauge = kSpecialGaugeFull;
        }

        if (specialPhase_ == SpecialPhase::Select) {
            int32_t slot = -1;
            uint8_t* record = nullptr;
            uint8_t* definition = nullptr;
            uint32_t unitData = 0, expected = 0, bound = 0;
            uint8_t style = 0, family = 0;
            int32_t damageSize = 0;
            const bool committed =
                Read(attack, kAttackSelectedSlot, slot) && slot == kSpecialSlot &&
                ReadPointer(attack, kAttackSelectedWeapon, record) &&
                ReadPointer(unit, kUnitDefinition, definition) &&
                Read(definition, 0, unitData) &&
                Read(definition, kDefinitionSpecialWeapon, expected) && expected != 0 &&
                Read(record, 0, bound) && bound == expected &&
                Read(record, kWeaponAttackStyle, style) &&
                Read(record, kWeaponRangeType, family) &&
                Read(record, kWeaponDamageSize, damageSize);
            if (committed) {
                input.HoldGameplay(kSpecialSelectAction, false);
                if (style != 2) {
                    sprintf_s(text, "native special slot weapon=%u attack_style=%u", bound, unsigned(style));
                    context->Observe(text);
                    context->Assert(0, "the native special slot binds an AttackStyle-2 (fatal) weapon");
                    Release(input);
                    return SpecialStep::Fail;
                }
                specialBound_ = true;
                specialUnitData_ = unitData;
                specialWeapon_ = bound;
                specialFamily_ = family;
                specialDamageSize_ = damageSize;
                specialPhase_ = SpecialPhase::Fire;
                specialPhaseAt_ = now;
                // The first firing tick then supplies a fresh primary down edge.
                input.HoldGameplay(kPrimaryAction, false);
                primaryHeld_ = false;
                sprintf_s(text,
                    "native player special slot bound unit_data=%u slot=3 weapon=%u range_type=%u damage_size=%d sp=%d inputs=%u",
                    unitData, bound, unsigned(family), damageSize, gauge, specialSelectInputs_);
                context->Observe(text);
                return SpecialStep::Control;
            }
            if (now - specialPhaseAt_ >= kSpecialSelectMs ||
                gauge < kSpecialGaugeFull) {
                input.HoldGameplay(kSpecialSelectAction, false);
                sprintf_s(text,
                    "native player special slot not bound attempt=%u sp=%d slot=%d after %llu ms; charging again",
                    specialAttempts_, gauge, slot,
                    static_cast<unsigned long long>(now - specialPhaseAt_));
                context->Observe(text);
                specialPhase_ = SpecialPhase::Charge;
                return SpecialStep::Control;
            }
            // The original consumer reads the down edge, not the held level.
            // It queues the selection; the attack FSM dispatch commits it.
            input.HoldGameplay(kForwardAction, false);
            input.HoldGameplay(kPrimaryAction, false);
            forwardHeld_ = false;
            primaryHeld_ = false;
            input.HoldGameplay(kSpecialSelectAction, false);
            input.HoldGameplay(kSpecialSelectAction, true);
            weaponInput_(unit, nativeInput, 0);
            dispatchAttack(attack, unit + kUnitControlContext);
            ++specialSelectInputs_;
            ++dispatches_;
            return SpecialStep::Wait;
        }

        if (specialPhase_ == SpecialPhase::Fire) {
            uint32_t events = 0;
            if (!specialEvents_ || !specialEvents_(&events)) {
                context->Assert(0, "the native special probe can read the OC client's local special computations");
                Release(input);
                return SpecialStep::Fail;
            }
            if (events > specialEventsBefore_) {
                specialObserved_ = true;
                specialEventsAfter_ = events;
                specialPhase_ = SpecialPhase::Done;
                sprintf_s(text,
                    "native player special computed attempt=%u weapon=%u range_type=%u events=%u->%u sp_at_select=%d sp=%d fire_ms=%llu",
                    specialAttempts_, specialWeapon_, unsigned(specialFamily_),
                    specialEventsBefore_, events, specialGaugeAtSelect_, gauge,
                    static_cast<unsigned long long>(now - specialPhaseAt_));
                context->Observe(text);
                return SpecialStep::Control;
            }
            if (now - specialPhaseAt_ >= kSpecialFireMs) {
                int32_t slot = -1;
                Read(attack, kAttackSelectedSlot, slot);
                sprintf_s(text,
                    "native player special attempt=%u fired for %llu ms without a local special computation (sp=%d slot=%d); charging again",
                    specialAttempts_,
                    static_cast<unsigned long long>(now - specialPhaseAt_),
                    gauge, slot);
                context->Observe(text);
                specialPhase_ = SpecialPhase::Charge;
            }
        }
        return SpecialStep::Control;
    }

    void BeginControl(
        uint8_t* unit, uint32_t objectId, uint64_t elapsed,
        const float* position, ScoreboardActions& input)
    {
        input.HoldGameplay(kForwardAction, false);
        input.HoldGameplay(kPrimaryAction, false);
        controlsActive_ = true;
        forwardHeld_ = false;
        primaryHeld_ = false;
        boundUnit_ = unit;
        boundObjectId_ = objectId;
        controlStartedAt_ = elapsed;
        baseline_[0] = position[0];
        baseline_[1] = position[1];
        baseline_[2] = position[2];
        baselineValid_ =
            std::isfinite(position[0]) &&
            std::isfinite(position[1]) &&
            std::isfinite(position[2]);
    }

public:
    bool Install(const SdgoApi* api, SdgoMod* mod)
    {
        if (!api || !api->SettingInt || !api->Symbol)
            return false;
        const int setting =
            api->SettingInt(mod, "BattlePlayerCombat", 0);
        const int continuous =
            api->SettingInt(
                mod,
                "BattlePlayerCombatContinuous",
                0);
        if ((setting != 0 && setting != 1) ||
            (continuous != 0 && continuous != 1))
            return false;
        enabled_ = setting != 0;
        continuousPrimary_ = continuous != 0;
        const int lock = api->SettingInt(mod, "BattlePlayerSecondary", 0);
        if (lock != 0 && lock != 1) return false;
        lockOn_ = lock != 0;
        requestedWeaponSlot_ = api->SettingInt(mod, "BattlePlayerWeaponSlot", -1);
        if (requestedWeaponSlot_ < -1 || requestedWeaponSlot_ > 2)
            return false;
        // The special slot (3) is reached only through the special mode, which
        // charges with the current weapon and needs the combat input itself.
        const int special = api->SettingInt(mod, "BattlePlayerSpecial", 0);
        if ((special != 0 && special != 1) ||
            (special != 0 && (!enabled_ || requestedWeaponSlot_ >= 0)))
            return false;
        special_ = special != 0;
        // the unit manager is also needed by ReadLocalArea (read-only) when combat input is off
        unitManager_ = static_cast<void**>(
            api->Symbol(mod, "game.unit_manager"));
        if (!enabled_)
            return true;

        objectIdFromPilot_ =
            reinterpret_cast<ObjectIdFromPilotFn>(
                api->Symbol(
                    mod, "um.object_id_from_pilot"));
        findObject_ =
            reinterpret_cast<FindObjectFn>(
                api->Symbol(mod, "um.find_object"));
        moveInput_ =
            reinterpret_cast<InputBusinessFn>(
                api->Symbol(
                    mod, "test.apsalus_move_input"));
        fireInput_ =
            reinterpret_cast<InputBusinessFn>(
                api->Symbol(
                    mod, "test.apsalus_primary_input"));
        if (lockOn_ || special_) {
            lockInput_ = reinterpret_cast<InputBusinessFn>(
                api->Symbol(mod, "test.player_secondary_input"));
            if (!lockInput_) return false;
        }
        if (requestedWeaponSlot_ >= 0 || special_) {
            weaponInput_ = reinterpret_cast<WeaponInputFn>(
                api->Symbol(mod, "test.player_weapon_input"));
            if (!weaponInput_) return false;
        }
        return unitManager_ && objectIdFromPilot_ &&
               findObject_ && moveInput_ && fireInput_;
    }

    void SetSpecialEventSource(SpecialEventsFn source) { specialEvents_ = source; }
    bool SpecialEnabled() const { return enabled_ && special_; }
    bool SpecialObserved() const { return specialObserved_; }
    uint32_t SpecialWeapon() const { return specialWeapon_; }
    uint8_t SpecialFamily() const { return specialFamily_; }
    int32_t SpecialDamageSize() const { return specialDamageSize_; }
    int32_t SpecialGauge() const { return specialGauge_; }
    uint32_t SpecialAttempts() const { return specialAttempts_; }
    uint32_t SpecialFills() const { return specialFills_; }
    float SpecialDistance() const { return specialDistance_; }

    bool Poll(
        const SdgoGameTestContextV1* context,
        const SdgoPvpAiClientSnapshotV1& snapshot,
        const SdgoPvpAiClientStartStatusV1& start,
        ScoreboardActions& input)
    {
        const bool released =
            snapshot.size == sizeof(snapshot) &&
            snapshot.version ==
                SDGO_PVP_AI_CLIENT_OBSERVER_VERSION &&
            start.size == sizeof(start) &&
            start.version ==
                SDGO_PVP_AI_CLIENT_OBSERVER_VERSION &&
            snapshot.native_app_state == 22 &&
            snapshot.profile_pilot_id != 0 &&
            snapshot.received_battle_start_count != 0 &&
            start.battle_start_sequence ==
                snapshot.received_battle_start_count &&
            start.player_control_enabled != 0;
        pvpSnapshot_ = &snapshot;
        const bool result = PollReleased(
            context,
            snapshot.native_app_state,
            snapshot.profile_pilot_id,
            snapshot.received_battle_start_count,
            released,
            "battle",
            input);
        pvpSnapshot_ = nullptr;
        return result;
    }

    bool PollMission(
        const SdgoGameTestContextV1* context,
        const SdgoMissionAiClientSnapshotV1& snapshot,
        uint32_t expectedGeneration,
        uint32_t expectedRoom,
        uint32_t nativeAppState,
        uint32_t profilePilotId,
        ScoreboardActions& input)
    {
        const bool released =
            snapshot.size == sizeof(snapshot) &&
            snapshot.version ==
                SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION &&
            snapshot.active != 0 &&
            snapshot.battle_generation != 0 &&
            snapshot.battle_generation ==
                expectedGeneration &&
            snapshot.room_object != 0 &&
            snapshot.room_object == expectedRoom &&
            nativeAppState == 22 &&
            profilePilotId != 0;
        return PollReleased(
            context,
            nativeAppState,
            profilePilotId,
            snapshot.battle_generation,
            released,
            "mission",
            input);
    }

private:
    bool PollReleased(
        const SdgoGameTestContextV1* context,
        uint32_t nativeAppState,
        uint32_t profilePilotId,
        uint32_t sequence,
        bool released,
        const char* scope,
        ScoreboardActions& input)
    {
        if (!enabled_)
            return true;
        if (!context)
            return false;
        const uint64_t now = Now(context);
        // Result handling may still report app state 22 for several updates.
        // Finish ends this battle's input lease even before that state changes.
        if (finished_ && battleSequence_ == sequence) {
            Release(input);
            return true;
        }

        if (!released) {
            if (context->tick % 300 == 0) {
                char text[192];
                sprintf_s(
                    text,
                    "native fire probe waits for released %s: app=%u sequence=%u pilot=%u",
                    scope, nativeAppState, sequence,
                    profilePilotId);
                context->Observe(text);
            }
            Release(input);
            return true;
        }
        if (battleSequence_ != sequence) {
            BeginBattle(sequence, input);
        }

        void* manager =
            unitManager_ ? *unitManager_ : nullptr;
        void* nativeInput = input.NativeInput();
        if (!manager || !nativeInput) {
            if (context->tick % 300 == 0)
                context->Observe("native fire probe waits for the real local manager and observed native input object");
            Release(input);
            return true;
        }

        const uint32_t objectId =
            objectIdFromPilot_(
                manager, profilePilotId);
        uint8_t* unit =
            objectId ? findObject_(manager, objectId) : nullptr;
        if (!unit) {
            Release(input);
            return true;
        }

        uint8_t* localPlayer =
            *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(manager) +
                kManagerLocalPlayer);
        uint8_t* node =
            *reinterpret_cast<uint8_t**>(
                unit + kUnitNode);
        uint8_t* move =
            *reinterpret_cast<uint8_t**>(
                unit + kUnitMoveFsm);
        uint8_t* attack =
            *reinterpret_cast<uint8_t**>(
                unit + kUnitAttackFsm);
        const bool boundAliveController =
            localPlayer == unit &&
            *reinterpret_cast<uint32_t*>(
                unit + kUnitObjectId) == objectId &&
            *reinterpret_cast<uint32_t*>(
                unit + kUnitPilotId) ==
                profilePilotId &&
            *reinterpret_cast<int32_t*>(
                unit + kUnitHp) > 0 &&
            unit[kUnitPlayerControlEnabled] != 0 &&
            node && move && attack &&
            *reinterpret_cast<uint8_t**>(
                move + kMoveOwner) == unit;
        if (!boundAliveController) {
            if (context->tick % 300 == 0)
                context->Observe("native fire probe waits for the authenticated living local controller");
            Release(input);
            return true;
        }

        void** moveVtable =
            *reinterpret_cast<void***>(move);
        void** attackVtable =
            *reinterpret_cast<void***>(attack);
        if (!moveVtable || !attackVtable ||
            !moveVtable[1] || !attackVtable[1]) {
            Release(input);
            return true;
        }

        const auto dispatchMove =
            reinterpret_cast<NativeTickFn>(
                moveVtable[1]);
        const auto dispatchAttack =
            reinterpret_cast<NativeTickFn>(
                attackVtable[1]);

        if (special_) {
            const SpecialStep step = PollSpecial(
                context, now, manager, unit, attack, dispatchAttack,
                nativeInput, input);
            if (step == SpecialStep::Fail)
                return false;
            if (step == SpecialStep::Wait)
                return true;
        }
        const bool specialFire =
            special_ && specialPhase_ == SpecialPhase::Fire;

        if (requestedWeaponSlot_ >= 0) {
            battle_ai::native::Capabilities caps{};
            if (!battle_ai::native::ReadCapabilities(unit, caps)) {
                // A fresh local player's attack bank may not be coherent until
                // the original weapon-selection work has actually run. Feed
                // its mapped selection and dispatch that real queued FSM work
                // before waiting for the read-only acknowledgement.
                input.HoldGameplay(kForwardAction, false);
                input.HoldGameplay(kPrimaryAction, false);
                input.HoldGameplay(14u + requestedWeaponSlot_, false);
                input.HoldGameplay(14u + requestedWeaponSlot_, true);
                weaponInput_(unit, nativeInput, 0);
                reinterpret_cast<NativeTickFn>(attackVtable[1])(
                    attack, unit + kUnitControlContext);
                if (context->tick % 300 == 0)
                    context->Observe("native fire probe waits for a coherent live form and weapon-bank observation");
                Release(input);
                return true;
            }
            const auto& weapon = caps.weapons[requestedWeaponSlot_];
            if (!weapon.valid || !weapon.ordinary ||
                weapon.kind == battle_ai::native::WeaponKind::Melee) {
                context->Assert(0, "the requested native fire-test slot must contain an ordinary ranged weapon");
                Release(input);
                return false;
            }
            // CPlayer's real input consumer clears DD0 to -4 without a weapon
            // selection down edge. Supply mapped key-down pulses and invoke
            // that consumer until the original FSM commits the requested slot.
            // A queued 0x48 message alone can be overwritten before that commit.
            if (caps.selectedSlot != requestedWeaponSlot_) {
                input.HoldGameplay(kForwardAction, false);
                input.HoldGameplay(kPrimaryAction, false);
                // The native consumer queries the down edge, not the held
                // level. Repeat a mapped key-down until binding is observed.
                input.HoldGameplay(14u + requestedWeaponSlot_, false);
                input.HoldGameplay(14u + requestedWeaponSlot_, true);
                weaponInput_(unit, nativeInput, 0);
                if (context->tick % 300 == 0)
                    context->Observe("native fire probe supplied mapped weapon-selection input; actual binding is still pending");
                return true;
            }
            input.HoldGameplay(14u + requestedWeaponSlot_, false);
            // ReadCapabilities also verifies the committed slot's bound record
            // against its live form/bank. A requested slot alone is insufficient.
            if (!weaponReported_) {
                char text[160];
                sprintf_s(text, "native player ranged weapon bound unit_data=%u slot=%d weapon=%u range_type=%u",
                          caps.unitId, requestedWeaponSlot_, weapon.id, unsigned(weapon.rangeType));
                context->Observe(text);
                weaponReported_ = true;
            }
            const size_t bank = caps.alternate ? 0x56C : 0x24C;
            uint8_t* liveAttack = nullptr;
            uint8_t* record = nullptr;
            uint8_t alternate = 0;
            uint32_t boundId = 0;
            int32_t slot = -1;
            int16_t ammo = 0;
            using namespace battle_ai::native::detail;
            if (!ReadPointer(unit, kUnitAttackFsm, liveAttack) || liveAttack != attack ||
                !Read(unit, 0xE34, alternate) || alternate != unsigned(caps.alternate) ||
                !Read(liveAttack, 0x240, slot) || slot != requestedWeaponSlot_ ||
                !ReadPointer(liveAttack, 0x248, record) || !Read(record, 0, boundId) || boundId != weapon.id ||
                !Read(liveAttack, bank + requestedWeaponSlot_ * 0xC8, ammo)) {
                Release(input);
                return true;
            }
            const bool sameWeapon = sampledUnit_ == caps.unitId && sampledWeapon_ == weapon.id &&
                                    sampledAlternate_ == caps.alternate && sampledAttack_ == liveAttack;
            if (!sameWeapon) sampledAmmoReported_ = false;
            if (ammoBaselineValid_ && sameWeapon && ammo >= 0 && ammo < previousAmmo_) {
                ammoSpent_ = true;
                if (!sampledAmmoReported_) {
                    char text[192];
                    sprintf_s(text,"native player ammunition consumed unit_data=%u slot=%d weapon=%u before=%d after=%d",
                              caps.unitId,requestedWeaponSlot_,weapon.id,previousAmmo_,ammo);
                    context->Observe(text);
                    sampledAmmoReported_ = true;
                }
            }
            previousAmmo_ = ammo;
            ammoBaselineValid_ = true;
            sampledUnit_ = caps.unitId; sampledWeapon_ = weapon.id;
            sampledAlternate_ = caps.alternate; sampledAttack_ = liveAttack;
        }

        const float* position =
            reinterpret_cast<const float*>(
                node + kNodePosition);
        if (!controlsActive_ || boundUnit_ != unit ||
            boundObjectId_ != objectId) {
            BeginControl(
                unit, objectId, now,
                position, input);
        }

        const uint64_t controlledFor =
            now - controlStartedAt_;
        // The bound special fires in pulses from its binding, so every cycle
        // offers the native admission a fresh primary down edge.
        const bool forward =
            !specialFire && controlledFor < kForwardHeldMs;
        const bool primary =
            specialFire
                ? (now - specialPhaseAt_) % kCycleMs <
                      kPrimaryHeldMs
                : controlledFor >= kForwardHeldMs &&
                      (continuousPrimary_ ||
                       (controlledFor - kForwardHeldMs) %
                               kCycleMs <
                           kPrimaryHeldMs);
        if (primary && !primaryHeld_)
            ++bursts_;
        input.HoldGameplay(kForwardAction, forward);
        input.HoldGameplay(kPrimaryAction, primary);
        // Supply the mapped secondary action to its original consumer.
        // Target choice, aim and all control-state changes stay native.
        // Reacquire ordinary targets with real down edges; a melee special
        // retains its native held lock while capturing the target in reach.
        const bool lock =
            (lockOn_ && controlledFor % kCycleMs < kPrimaryHeldMs) ||
            (specialFire && MeleeFamily(specialFamily_));
        input.HoldGameplay(kLockAction, lock);
        if (lock) lockInput_(unit, nativeInput);
        forwardHeld_ = forward;
        primaryHeld_ = primary;

        const uint32_t moveBefore =
            input.GameplayQueries(kForwardAction);
        const uint32_t fireBefore =
            input.GameplayQueries(kPrimaryAction);
        moveInput_(unit, nativeInput);
        fireInput_(unit, nativeInput);
        const uint32_t moveAfter =
            input.GameplayQueries(kForwardAction);
        const uint32_t fireAfter =
            input.GameplayQueries(kPrimaryAction);
        consumedMoveQueries_ += moveAfter - moveBefore;
        consumedFireQueries_ += fireAfter - fireBefore;
        inputObserved_ =
            inputObserved_ ||
            (consumedMoveQueries_ != 0 &&
             consumedFireQueries_ != 0);

        dispatchMove(
            move, unit + kUnitControlContext);
        dispatchAttack(
            attack, unit + kUnitControlContext);
        ++dispatches_;

        if (baselineValid_) {
            const float dx =
                position[0] - baseline_[0];
            const float dy =
                position[1] - baseline_[1];
            const float dz =
                position[2] - baseline_[2];
            movementObserved_ =
                movementObserved_ ||
                (std::isfinite(dx) &&
                 std::isfinite(dy) &&
                 std::isfinite(dz) &&
                 dx * dx + dy * dy + dz * dz > 1.0f &&
                 consumedMoveQueries_ != 0);
        }
        const uintptr_t state =
            reinterpret_cast<uintptr_t>(
                *reinterpret_cast<void**>(
                    attack + 4));
        const uintptr_t first =
            reinterpret_cast<uintptr_t>(
                attack + 0x20);
        attackObserved_ =
            attackObserved_ ||
            (state > first &&
             state < first + 0x200 &&
             consumedFireQueries_ != 0);

        if (movementObserved_ && !movementReported_) {
            context->Observe(
                "native player combat consumed mapped forward input and changed the real local scene position");
            movementReported_ = true;
        }
        if (attackObserved_ && !attackReported_) {
            context->Observe(
                "native player combat consumed mapped primary input and entered a real local attack state");
            attackReported_ = true;
        }
        return true;
    }

public:
    // Read-only view of the local unit for imported-mission observation: its scene position (x, y, z) and the
    // movable area the client keeps it in (SetTerrainRect: left x, right x, top z, bottom z) with the on flag.
    bool ReadLocalArea(float position[3], float area[4], uint8_t& enabled) const
    {
        using namespace battle_ai::native::detail;
        const uint8_t* manager = nullptr;
        if (!unitManager_ || !Read(reinterpret_cast<const uint8_t*>(unitManager_), 0, manager) || !manager)
            return false;
        uint8_t* unit = nullptr;
        uint8_t* node = nullptr;
        if (!ReadPointer(manager, kManagerLocalPlayer, unit) || !ReadPointer(unit, kUnitNode, node))
            return false;
        for (int i = 0; i < 3; ++i)
            if (!Read(node, kNodePosition + 4 * i, position[i])) return false;
        for (int i = 0; i < 4; ++i)
            if (!Read(unit, kUnitTerrainArea + 4 * i, area[i])) return false;
        return Read(unit, kUnitTerrainArea + 0x10, enabled);
    }

    // Test-only: move the local player's scene node to (x, z), height kept (imported mission area
    // barrier review). The client's own area clamp still applies afterwards.
    bool PlaceLocalPlayer(float x, float z) const
    {
        using namespace battle_ai::native::detail;
        const uint8_t* manager = nullptr;
        if (!unitManager_ || !Read(reinterpret_cast<const uint8_t*>(unitManager_), 0, manager) || !manager)
            return false;
        uint8_t* unit = nullptr;
        uint8_t* node = nullptr;
        if (!ReadPointer(manager, kManagerLocalPlayer, unit) || !ReadPointer(unit, kUnitNode, node))
            return false;
        return WriteNodePosition(node, x, z);
    }

    static bool WriteNodePosition(uint8_t* node, float x, float z)
    {
        __try {
            *reinterpret_cast<float*>(node + kNodePosition) = x;
            *reinterpret_cast<float*>(node + kNodePosition + 8) = z;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void Finish(
        const SdgoGameTestContextV1* context,
        ScoreboardActions& input)
    {
        Release(input);
        if (!enabled_ || finished_ || !context)
            return;
        finished_ = true;

        char text[256] = {};
        sprintf_s(
            text,
            "native player combat sequence=%u bursts=%u "
            "move_queries=%u fire_queries=%u dispatches=%u "
            "moved=%u attacked=%u continuous_primary=%u",
            battleSequence_, bursts_,
            consumedMoveQueries_, consumedFireQueries_,
            dispatches_, movementObserved_ ? 1u : 0u,
            attackObserved_ ? 1u : 0u,
            continuousPrimary_ ? 1u : 0u);
        context->Observe(text);
        context->Assert(
            inputObserved_ &&
                consumedMoveQueries_ != 0 &&
                consumedFireQueries_ != 0,
            "the original native input consumers read mapped forward and primary actions from the real input object");
        context->Assert(
            movementObserved_,
            "mapped forward input and native FSM dispatch moved the real local scene node");
        context->Assert(
            attackObserved_,
            "mapped primary input and native FSM dispatch entered a real local attack state");
        if (requestedWeaponSlot_ >= 0)
            context->Assert(ammoSpent_, "the acknowledged ordinary ranged weapon consumed its real native ammunition");
        if (special_) {
            char report[384] = {};
            sprintf_s(
                report,
                "native player special sequence=%u attempts=%u fixture_fills=%u select_inputs=%u bound=%u "
                "unit_data=%u weapon=%u range_type=%u damage_size=%d reach=%.0f last_distance=%.0f "
                "sp_max=%d sp_at_select=%d events=%u->%u computed=%u",
                battleSequence_, specialAttempts_, specialFills_, specialSelectInputs_,
                specialBound_ ? 1u : 0u, specialUnitData_, specialWeapon_,
                unsigned(specialFamily_), specialDamageSize_, specialReach_, specialDistance_,
                specialGaugeMax_, specialGaugeAtSelect_, specialEventsBefore_, specialEventsAfter_,
                specialObserved_ ? 1u : 0u);
            context->Observe(report);
            context->Assert(
                specialBound_,
                "with the SP gauge full, mapped action 17 selected the special slot through the original consumer and the native FSM bound its AttackStyle-2 weapon");
            context->Assert(
                specialObserved_,
                "mapped primary input fired the bound special and the OC client recorded the local special computation");
        }
    }
};
