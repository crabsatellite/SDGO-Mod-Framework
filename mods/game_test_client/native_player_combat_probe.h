#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "sdgo/game_test.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "sdgo/sdgo_api.h"

#include "scoreboard_actions.h"
#include "../battle_ai/native_capabilities.h"

// Opt-in native player input for an isolated AI battle GameTest. The probe
// resolves the authenticated pilot's real local unit, supplies mapped forward
// and primary-fire actions to the original input consumers, and dispatches the
// resulting native FSM work through the unit's real control context. It never
// writes HP, SP, position, controller flags or FSM state.
class NativePlayerCombatProbe
{
    using ObjectIdFromPilotFn =
        uint32_t(__thiscall*)(void*, uint32_t);
    using FindObjectFn =
        uint8_t*(__thiscall*)(void*, uint32_t);
    // Both consumers and both FSM dispatch slots take one stack argument and
    // return with RET 4. Their native owner is supplied through ECX.
    using InputBusinessFn =
        void(__thiscall*)(uint8_t*, void*);
    using NativeTickFn =
        void(__thiscall*)(uint8_t*, void*);

    static constexpr uint32_t kForwardAction = 0;
    static constexpr uint32_t kPrimaryAction = 9;
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

    void** unitManager_ = nullptr;
    ObjectIdFromPilotFn objectIdFromPilot_ = nullptr;
    FindObjectFn findObject_ = nullptr;
    InputBusinessFn moveInput_ = nullptr;
    InputBusinessFn fireInput_ = nullptr;

    bool enabled_ = false;
    bool continuousPrimary_ = false;
    int requestedWeaponSlot_ = -1;
    bool weaponRequestSent_ = false;
    bool weaponReported_ = false;
    uint64_t weaponRequestAt_ = 0;
    bool ammoBaselineValid_ = false;
    bool ammoSpent_ = false;
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

    void Release(ScoreboardActions& input)
    {
        input.HoldGameplay(kForwardAction, false);
        input.HoldGameplay(kPrimaryAction, false);
        forwardHeld_ = false;
        primaryHeld_ = false;
        controlsActive_ = false;
        baselineValid_ = false;
        boundObjectId_ = 0;
        boundUnit_ = nullptr;
        weaponRequestSent_ = false;
        weaponReported_ = false;
        ammoBaselineValid_ = false;
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
        requestedWeaponSlot_ = api->SettingInt(mod, "BattlePlayerWeaponSlot", -1);
        if (requestedWeaponSlot_ < -1 || requestedWeaponSlot_ > 2)
            return false;
        if (!enabled_)
            return true;

        unitManager_ = static_cast<void**>(
            api->Symbol(mod, "game.unit_manager"));
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
        return unitManager_ && objectIdFromPilot_ &&
               findObject_ && moveInput_ && fireInput_;
    }

    bool Poll(
        const SdgoGameTestContextV1* context,
        const SdgoPvpAiClientSnapshotV1& snapshot,
        const SdgoPvpAiClientStartStatusV1& start,
        ScoreboardActions& input)
    {
        if (!enabled_)
            return true;
        if (!context)
            return false;

        const bool genuineReleasedBattle =
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
        if (!genuineReleasedBattle) {
            Release(input);
            return true;
        }
        if (battleSequence_ !=
            snapshot.received_battle_start_count) {
            BeginBattle(
                snapshot.received_battle_start_count, input);
        }

        void* manager =
            unitManager_ ? *unitManager_ : nullptr;
        void* nativeInput = input.NativeInput();
        if (!manager || !nativeInput) {
            Release(input);
            return true;
        }

        const uint32_t objectId =
            objectIdFromPilot_(
                manager, snapshot.profile_pilot_id);
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
                snapshot.profile_pilot_id &&
            *reinterpret_cast<int32_t*>(
                unit + kUnitHp) > 0 &&
            unit[kUnitPlayerControlEnabled] != 0 &&
            node && move && attack &&
            *reinterpret_cast<uint8_t**>(
                move + kMoveOwner) == unit;
        if (!boundAliveController) {
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

        if (requestedWeaponSlot_ >= 0) {
            battle_ai::native::Capabilities caps{};
            if (!battle_ai::native::ReadCapabilities(unit, caps)) {
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
            // The common CPlayer receiver handles the same native 0x48 request
            // as CUnit/CNPC. It only queues the desired slot; actual permission,
            // binding, ammo and cooldown remain owned by the original game.
            if (caps.selectedSlot != requestedWeaponSlot_) {
                input.HoldGameplay(kForwardAction, false);
                input.HoldGameplay(kPrimaryAction, false);
                if (!weaponRequestSent_ || context->elapsed_ms - weaponRequestAt_ >= 250) {
                    auto table = *reinterpret_cast<void***>(unit);
                    if (!table || !table[7]) { Release(input); return false; }
                    alignas(4) uint8_t message[0x14]{};
                    *reinterpret_cast<uint16_t*>(message) = 0x48;
                    message[0x10] = static_cast<uint8_t>(requestedWeaponSlot_);
                    using Receive = void(__thiscall*)(uint8_t*, const uint8_t*);
                    reinterpret_cast<Receive>(table[7])(unit, message);
                    weaponRequestSent_ = true;
                    weaponRequestAt_ = context->elapsed_ms;
                }
                return true;
            }
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
            if (ammoBaselineValid_ && sameWeapon && ammo >= 0 && ammo < previousAmmo_) ammoSpent_ = true;
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
                unit, objectId, context->elapsed_ms,
                position, input);
        }

        const uint64_t controlledFor =
            context->elapsed_ms - controlStartedAt_;
        const bool forward =
            controlledFor < kForwardHeldMs;
        const bool primary =
            controlledFor >= kForwardHeldMs &&
            (continuousPrimary_ ||
             (controlledFor - kForwardHeldMs) %
                     kCycleMs <
                 kPrimaryHeldMs);
        if (primary && !primaryHeld_)
            ++bursts_;
        input.HoldGameplay(kForwardAction, forward);
        input.HoldGameplay(kPrimaryAction, primary);
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

        const auto dispatchMove =
            reinterpret_cast<NativeTickFn>(
                moveVtable[1]);
        const auto dispatchAttack =
            reinterpret_cast<NativeTickFn>(
                attackVtable[1]);
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
    }
};
