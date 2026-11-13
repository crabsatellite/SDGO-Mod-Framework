#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <windows.h>

#include "sdgo/game_test.h"
#include "sdgo/sdgo_api.h"

#include "native_death_probe.h"

// Passive evidence for the shipped movement-hurt state. The three detours only
// record atomic lifecycle facts around their original callbacks. Poll performs
// all pointer validation, logging, receipt correlation and acceptance work on
// the GameTest update thread. No HP, FSM, controller, packet or input state is
// written by this observer.
class NativeHurtProbe {
public:
    struct LifecycleSnapshot {
        uint32_t entry_count = 0;
        uint32_t entry_return_count = 0;
        uint32_t update_count = 0;
        uint32_t update_return_count = 0;
        uint32_t exit_count = 0;
        uint32_t exit_return_count = 0;
        uint32_t entry_tick = 0;
        uint32_t update_tick = 0;
        uint32_t exit_tick = 0;
        uintptr_t entry_fsm = 0;
        uintptr_t update_fsm = 0;
        uintptr_t exit_fsm = 0;
        uintptr_t entry_owner = 0;
        uintptr_t update_owner = 0;
        uintptr_t exit_owner = 0;
    };

private:
    using HurtCallback = void(__thiscall*)(uint8_t*, void*);

    static constexpr size_t kManagerLocalPlayer = 0x88;
    static constexpr size_t kUnitObjectId = 0x04;
    static constexpr size_t kUnitHp = 0x38;
    static constexpr size_t kUnitSp = 0xBC8;
    static constexpr size_t kUnitPilotId = 0x2B68;
    static constexpr size_t kUnitControl = 0x31E9;
    static constexpr size_t kMovementOwner = 0x21C;
    static constexpr uint32_t kPairWindowMs = 2500;
    static constexpr size_t kCandidateCapacity = 4;

    struct DamageCandidate {
        NativeDeathProbe::MapDamageEvidence evidence{};
        int32_t sp = 0;
        bool valid = false;
    };

    inline static NativeHurtProbe* active_ = nullptr;

    bool enabled_ = false;
    void** unitManager_ = nullptr;
    HurtCallback entryOriginal_ = nullptr;
    HurtCallback updateOriginal_ = nullptr;
    HurtCallback exitOriginal_ = nullptr;

    volatile LONG entryCount_ = 0;
    volatile LONG entryReturnCount_ = 0;
    volatile LONG updateCount_ = 0;
    volatile LONG updateReturnCount_ = 0;
    volatile LONG exitCount_ = 0;
    volatile LONG exitReturnCount_ = 0;
    volatile LONG entryTick_ = 0;
    volatile LONG updateTick_ = 0;
    volatile LONG exitTick_ = 0;
    PVOID volatile expectedLocal_ = nullptr;
    PVOID volatile entryFsm_ = nullptr;
    PVOID volatile updateFsm_ = nullptr;
    PVOID volatile exitFsm_ = nullptr;
    PVOID volatile entryOwner_ = nullptr;
    PVOID volatile updateOwner_ = nullptr;
    PVOID volatile exitOwner_ = nullptr;

    uint32_t observedMapSequence_ = 0;
    DamageCandidate candidates_[kCandidateCapacity]{};
    size_t candidateWrite_ = 0;
    uint32_t exactDamageReceipts_ = 0;
    NativeDeathProbe::MapDamageEvidence acceptedEvidence_{};

    bool authoritativeDamageObserved_ = false;
    uint32_t damageTick_ = 0;
    int32_t damageBeforeHp_ = 0;
    int32_t damageAfterHp_ = 0;
    int32_t damageBeforeSp_ = 0;
    bool entryPaired_ = false;
    bool updatePaired_ = false;
    bool exitPaired_ = false;
    bool exitFireBaselineCaptured_ = false;
    uint32_t fireQueriesAtExit_ = 0;
    bool recoveryObserved_ = false;
    bool spPreserved_ = false;
    bool receiptLogged_ = false;
    bool lifecycleLogged_ = false;
    uint32_t captureTriggerSequence_ = 0;

    template <typename T>
    static bool Read(const uint8_t* base, size_t offset, T& value) {
        if (!base) return false;
        __try {
            value = *reinterpret_cast<const T*>(base + offset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static uint8_t* MovementOwner(uint8_t* fsm) {
        uint8_t* owner = nullptr;
        if (fsm) {
            __try {
                owner = *reinterpret_cast<uint8_t**>(fsm + kMovementOwner);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                owner = nullptr;
            }
        }
        return owner;
    }

    static bool RecordBefore(NativeHurtProbe* observer,
                             volatile LONG& count, volatile LONG& tick,
                             PVOID volatile& fsmSlot, PVOID volatile& ownerSlot,
                             uint8_t* fsm) {
        uint8_t* movementOwner = MovementOwner(fsm);
        void* expected = InterlockedCompareExchangePointer(
            &observer->expectedLocal_, nullptr, nullptr);
        if (!expected || movementOwner != expected) return false;
        InterlockedExchangePointer(&fsmSlot, fsm);
        InterlockedExchangePointer(&ownerSlot, movementOwner);
        InterlockedExchange(&tick, static_cast<LONG>(GetTickCount()));
        InterlockedIncrement(&count);
        return true;
    }

    static void __fastcall HurtEntry(uint8_t* fsm, void*, void* context) {
        const DWORD incomingError = GetLastError();
        NativeHurtProbe* owner = active_;
        const bool recorded = owner &&
            RecordBefore(owner, owner->entryCount_, owner->entryTick_,
                         owner->entryFsm_, owner->entryOwner_, fsm);
        SetLastError(incomingError);
        if (owner && owner->entryOriginal_) owner->entryOriginal_(fsm, context);
        const DWORD originalError = GetLastError();
        if (recorded) InterlockedIncrement(&owner->entryReturnCount_);
        SetLastError(originalError);
    }

    static void __fastcall HurtUpdate(uint8_t* fsm, void*, void* context) {
        const DWORD incomingError = GetLastError();
        NativeHurtProbe* owner = active_;
        const bool recorded = owner &&
            RecordBefore(owner, owner->updateCount_, owner->updateTick_,
                         owner->updateFsm_, owner->updateOwner_, fsm);
        SetLastError(incomingError);
        if (owner && owner->updateOriginal_) owner->updateOriginal_(fsm, context);
        const DWORD originalError = GetLastError();
        if (recorded) InterlockedIncrement(&owner->updateReturnCount_);
        SetLastError(originalError);
    }

    static void __fastcall HurtExit(uint8_t* fsm, void*, void* context) {
        const DWORD incomingError = GetLastError();
        NativeHurtProbe* owner = active_;
        const bool recorded = owner &&
            RecordBefore(owner, owner->exitCount_, owner->exitTick_,
                         owner->exitFsm_, owner->exitOwner_, fsm);
        SetLastError(incomingError);
        if (owner && owner->exitOriginal_) owner->exitOriginal_(fsm, context);
        const DWORD originalError = GetLastError();
        if (recorded) InterlockedIncrement(&owner->exitReturnCount_);
        SetLastError(originalError);
    }

    static bool CausalWithin(uint32_t before, uint32_t after) {
        return static_cast<uint32_t>(after - before) <= kPairWindowMs;
    }

    static bool ExactLocalTarget(
        const NativeDeathProbe::MapDamageEvidence& evidence,
        uint32_t object, uint32_t localPilot, uint32_t profilePilot) {
        return object && localPilot && profilePilot && localPilot == profilePilot &&
               evidence.objectId == object &&
               evidence.targetPilotId == profilePilot;
    }

    static bool LifecycleMatches(const LifecycleSnapshot& lifecycle,
                                 const DamageCandidate& candidate,
                                 const uint8_t* local) {
        const uintptr_t owner = reinterpret_cast<uintptr_t>(local);
        return candidate.valid && lifecycle.entry_count && lifecycle.update_count &&
               lifecycle.exit_count &&
               lifecycle.entry_return_count >= lifecycle.entry_count &&
               lifecycle.update_return_count >= lifecycle.update_count &&
               lifecycle.exit_return_count >= lifecycle.exit_count &&
               lifecycle.entry_owner == owner && lifecycle.update_owner == owner &&
               lifecycle.exit_owner == owner && lifecycle.entry_fsm &&
               lifecycle.entry_fsm == lifecycle.update_fsm &&
               lifecycle.entry_fsm == lifecycle.exit_fsm &&
               CausalWithin(candidate.evidence.tick, lifecycle.entry_tick) &&
               CausalWithin(lifecycle.entry_tick, lifecycle.update_tick) &&
               CausalWithin(lifecycle.update_tick, lifecycle.exit_tick) &&
               CausalWithin(candidate.evidence.tick, lifecycle.exit_tick);
    }

public:
    bool Configure(bool enabled) {
        if (active_) return false;
        enabled_ = enabled;
        return true;
    }

    bool Install(const SdgoApi* api, SdgoMod* mod) {
        if (!enabled_) return true;
        if (!api || !api->Symbol || !api->HookCode || active_) return false;
        unitManager_ = static_cast<void**>(api->Symbol(mod, "game.unit_manager"));
        if (!unitManager_) return false;
        active_ = this;
        if (!api->HookCode(mod, "test.movement_hurt_entry",
                           reinterpret_cast<void*>(&HurtEntry),
                           reinterpret_cast<void**>(&entryOriginal_)) ||
            !api->HookCode(mod, "test.movement_hurt_update",
                           reinterpret_cast<void*>(&HurtUpdate),
                           reinterpret_cast<void**>(&updateOriginal_)) ||
            !api->HookCode(mod, "test.movement_hurt_exit",
                           reinterpret_cast<void*>(&HurtExit),
                           reinterpret_cast<void**>(&exitOriginal_)) ||
            !entryOriginal_ || !updateOriginal_ || !exitOriginal_) {
            active_ = nullptr;
            return false;
        }
        return true;
    }

    bool Enabled() const { return enabled_; }

    LifecycleSnapshot Snapshot() {
        LifecycleSnapshot value{};
        value.entry_count = static_cast<uint32_t>(InterlockedCompareExchange(&entryCount_, 0, 0));
        value.entry_return_count = static_cast<uint32_t>(InterlockedCompareExchange(&entryReturnCount_, 0, 0));
        value.update_count = static_cast<uint32_t>(InterlockedCompareExchange(&updateCount_, 0, 0));
        value.update_return_count = static_cast<uint32_t>(InterlockedCompareExchange(&updateReturnCount_, 0, 0));
        value.exit_count = static_cast<uint32_t>(InterlockedCompareExchange(&exitCount_, 0, 0));
        value.exit_return_count = static_cast<uint32_t>(InterlockedCompareExchange(&exitReturnCount_, 0, 0));
        value.entry_tick = static_cast<uint32_t>(InterlockedCompareExchange(&entryTick_, 0, 0));
        value.update_tick = static_cast<uint32_t>(InterlockedCompareExchange(&updateTick_, 0, 0));
        value.exit_tick = static_cast<uint32_t>(InterlockedCompareExchange(&exitTick_, 0, 0));
        value.entry_fsm = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(&entryFsm_, nullptr, nullptr));
        value.update_fsm = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(&updateFsm_, nullptr, nullptr));
        value.exit_fsm = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(&exitFsm_, nullptr, nullptr));
        value.entry_owner = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(&entryOwner_, nullptr, nullptr));
        value.update_owner = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(&updateOwner_, nullptr, nullptr));
        value.exit_owner = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(&exitOwner_, nullptr, nullptr));
        return value;
    }

    uint32_t CaptureTriggerSequence() const { return captureTriggerSequence_; }

    bool Poll(const SdgoGameTestContextV1* context,
              const NativeDeathProbe& deaths, uint32_t fireQueries,
              uint32_t profilePilotId, uint32_t expectedMission,
              uint32_t expectedPhase) {
        if (!enabled_) return true;
        if (!context || !unitManager_) return false;
        uint8_t* manager = nullptr;
        uint8_t* local = nullptr;
        uint32_t object = 0;
        uint32_t pilot = 0;
        int32_t hp = 0;
        int32_t sp = 0;
        uint8_t control = 0;
        if (!Read(reinterpret_cast<const uint8_t*>(unitManager_), 0, manager) ||
            !manager || !Read(manager, kManagerLocalPlayer, local) || !local ||
            !Read(local, kUnitObjectId, object) || !object ||
            !Read(local, kUnitPilotId, pilot) || !pilot ||
            !Read(local, kUnitHp, hp) || !Read(local, kUnitSp, sp) ||
            !Read(local, kUnitControl, control))
            return true;

        InterlockedExchangePointer(&expectedLocal_, local);
        const uint32_t nowTick = GetTickCount();
        NativeDeathProbe::MapDamageEvidence evidence{};
        if (deaths.LatestMapDamage(evidence) &&
            evidence.sequence != observedMapSequence_) {
            observedMapSequence_ = evidence.sequence;
            const bool exact =
                ExactLocalTarget(evidence, object, pilot, profilePilotId) &&
                expectedMission && expectedPhase &&
                evidence.missionId == expectedMission &&
                evidence.phase == expectedPhase && evidence.feedbackPending == 1 &&
                evidence.tick && CausalWithin(evidence.tick, nowTick) &&
                evidence.beforeHp > static_cast<int32_t>(evidence.noticeHealth) &&
                evidence.afterHp == static_cast<int32_t>(evidence.noticeHealth);
            if (exact) {
                ++exactDamageReceipts_;
                if (!captureTriggerSequence_) captureTriggerSequence_ = evidence.sequence;
                DamageCandidate& candidate = candidates_[candidateWrite_];
                candidate.evidence = evidence;
                candidate.sp = sp;
                candidate.valid = true;
                candidateWrite_ = (candidateWrite_ + 1) % kCandidateCapacity;
            }
        }

        const LifecycleSnapshot lifecycle = Snapshot();
        if (!authoritativeDamageObserved_) {
            for (const DamageCandidate& candidate : candidates_) {
                if (!LifecycleMatches(lifecycle, candidate, local)) continue;
                authoritativeDamageObserved_ = true;
                acceptedEvidence_ = candidate.evidence;
                damageTick_ = candidate.evidence.tick;
                damageBeforeHp_ = candidate.evidence.beforeHp;
                damageAfterHp_ = candidate.evidence.afterHp;
                damageBeforeSp_ = candidate.sp;
                entryPaired_ = true;
                updatePaired_ = true;
                exitPaired_ = true;
                exitFireBaselineCaptured_ = true;
                fireQueriesAtExit_ = fireQueries;
                break;
            }
        } else if (exitFireBaselineCaptured_ && control &&
                   fireQueries > fireQueriesAtExit_) {
            recoveryObserved_ = true;
            spPreserved_ = sp >= damageBeforeSp_;
        }

        if (authoritativeDamageObserved_ && !receiptLogged_) {
            char text[384]{};
            sprintf_s(text,
                      "native boundary hurt authoritative0793 sequence=%u mission=%u phase=%u local_object=%08X pilot=%08X hp=%d->%d notice=%u tick=%u",
                      acceptedEvidence_.sequence, acceptedEvidence_.missionId,
                      acceptedEvidence_.phase, object, pilot, damageBeforeHp_,
                      damageAfterHp_, acceptedEvidence_.noticeHealth, damageTick_);
            context->Observe(text);
            receiptLogged_ = true;
        }
        if (entryPaired_ && updatePaired_ && exitPaired_ && !lifecycleLogged_) {
            char text[320]{};
            sprintf_s(text,
                      "native movement-hurt lifecycle causal entry=%u update=%u exit=%u ticks=%u/%u/%u fsm=%p owner=%p",
                      lifecycle.entry_count, lifecycle.update_count,
                      lifecycle.exit_count, lifecycle.entry_tick,
                      lifecycle.update_tick, lifecycle.exit_tick,
                      reinterpret_cast<void*>(lifecycle.entry_fsm), local);
            context->Observe(text);
            lifecycleLogged_ = true;
        }

        (void)hp;
        return true;
    }

    void Finish(const SdgoGameTestContextV1* context) {
        if (!enabled_ || !context) return;
        context->Assert(authoritativeDamageObserved_ &&
                            damageBeforeHp_ > damageAfterHp_,
                        "a fresh authoritative 0793 receipt causally matched the exact local player's native hurt lifecycle");
        context->Assert(exactDamageReceipts_ >= 1,
                        "the local wall-contact cost has at least one distinct authoritative receipt, each sequence observed once");
        context->Assert(entryPaired_ && updatePaired_ && exitPaired_,
                        "the same local movement FSM completed native hurt Entry, Update and Exit after the wall-damage receipt");
        context->Assert(recoveryObserved_,
                        "native local control recovered after hurt Exit and the ordinary primary-fire consumer continued querying input");
        context->Assert(spPreserved_,
                        "the real wall-hurt lifecycle completed without an unexpected local SP reset");
    }
};
