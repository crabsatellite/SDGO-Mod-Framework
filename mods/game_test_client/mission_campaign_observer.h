#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>
#include <intrin.h>

#include "sdgo/game_test.h"
#include "sdgo/map_catalog_format.h"
#include "sdgo/map_catalog_mission_observer.h"
#include "sdgo/mission_ai_client_observer.h"
#include "sdgo/sdgo_api.h"

#include "native_death_probe.h"

// Read-only evidence collector for the private Odessa native campaign. It
// enumerates canonical UnitManager lists, observes original 0570 death packets
// through NativeDeathProbe, and passively wraps the original 0905 mission-clear
// handler. Its only input is an optional one-shot raw Escape pulse returned to
// the source-verified phase-cinematic consumer at return address 00AD0060. It
// never writes HP, position, target, visibility, controller, FSM, inventory,
// packets, results, phase state, or Lua state.
class MissionCampaignObserver {
public:
    enum Mode : uint32_t {
        kOff = 0,
        kFullEight = 1,
        kDamageGate = 2,
    };

    struct MissionSuccessReceipt {
        uint32_t sequence = 0;
        uint32_t observedPhase = 0;
        uint32_t clearId = 0;
        uint32_t eventOrder = 0;
        uint32_t originalResult = 0;
        uint8_t count = 0;
        uint32_t ids[10] = {};
    };

private:
    using GridFind = bool(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
    using GridData = void*(__cdecl*)();
    using FindObject = uint8_t*(__thiscall*)(uint8_t*, uint32_t);
    using KeyPressed = bool(__cdecl*)(uint32_t);
    using MissionSuccessHandler = int(__stdcall*)(const uint8_t*);

    struct Node {
        Node* next;
        Node* previous;
        uint8_t* object;
    };

    struct Actor {
        uint32_t object = 0;
        uint32_t pilot = 0;
        uint32_t npc = 0;
        uint32_t target = 0;
        int32_t hp = 0;
        int32_t maxHp = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint8_t list = 0;
        uint8_t controlled = 0;
        uint8_t hurt = 0;
        uint8_t position = 0;
    };

    inline static MissionCampaignObserver* active_ = nullptr;
    static constexpr uintptr_t kPartySkipCaller = 0x00AD0060u;

    Mode mode_ = kOff;
    uint32_t targetNpc_ = 0;
    uint32_t guardNpc_ = 0;
    uint32_t partyPhase_ = 6;
    bool skipEnabled_ = false;
    uint32_t skipDelayMs_ = 2000;
    GridFind gridFind_ = nullptr;
    GridData gridData_ = nullptr;
    FindObject findObject_ = nullptr;
    KeyPressed keyPressedOriginal_ = nullptr;
    MissionSuccessHandler missionSuccessOriginal_ = nullptr;
    SdgoMapCatalogGetMissionSuccessV1Fn mapSuccessGetter_ = nullptr;
    uint32_t mapSuccessObservedSequence_ = 0;

    uint32_t mission_ = 0;
    uint32_t phase_ = 0;
    bool demo_ = false;
    volatile LONG hookPhase_ = 0;
    volatile LONG hookDemo_ = 0;
    std::vector<Actor> actors_;
    std::vector<Actor> previousActors_;
    uint64_t lastActorLogAt_ = 0;
    uint64_t actorReadFailureAt_ = 0;
    uint32_t actorReadFailures_ = 0;

    bool partyObserved_ = false;
    bool partyCleanupPending_ = false;
    bool partyCleanupObserved_ = false;
    uint64_t partyStartedAt_ = 0;
    uint64_t partyEndedAt_ = 0;
    uint64_t lastPartyMotionAt_ = 0;
    std::vector<uint32_t> partySources_;
    std::vector<uint32_t> partyBefore_;
    std::vector<uint32_t> partyTransientActors_;

    volatile LONG skipPulse_ = 0;
    volatile LONG skipConsumedCount_ = 0;
    uint32_t skipQueryBaseline_ = 0;
    bool skipArmed_ = false;
    bool skipConsumed_ = false;

    uint32_t targetObject_ = 0;
    uint32_t targetPilot_ = 0;
    bool targetAliveObserved_ = false;
    int32_t targetBaselineHp_ = 0;
    int32_t targetLastHp_ = 0;
    uint8_t targetLastHurt_ = 0;
    bool targetHurtStateTransitionObserved_ = false;
    bool targetDamageBeforeGuard_ = false;
    bool targetDamageAfterGuard_ = false;
    bool targetDeathObserved_ = false;
    uint32_t guardObject_ = 0;
    uint32_t guardPilot_ = 0;
    bool guardAliveObserved_ = false;
    int32_t guardBaselineHp_ = 0;
    int32_t guardLastHp_ = 0;
    bool guardDamageObserved_ = false;
    bool guardDeathObserved_ = false;
    bool guardDepartureObserved_ = false;
    bool gateTransitionReported_ = false;

    volatile LONG successWriteVersion_ = 0;
    volatile LONG successSequence_ = 0;
    volatile LONG successObservedPhase_ = 0;
    volatile LONG successClearId_ = 0;
    volatile LONG successEventOrder_ = 0;
    volatile LONG successOriginalResult_ = 0;
    volatile LONG successCount_ = 0;
    volatile LONG successIds_[10] = {};
    uint32_t observedSuccessSequence_ = 0;
    bool phaseEightSuccessObserved_ = false;

    uint32_t visualSequence_ = 0;
    char visualLabel_[64] = {};

    template <class T>
    static bool Read(const uint8_t* base, size_t offset, T& value) {
        if (!base) return false;
        __try {
            value = *reinterpret_cast<const T*>(base + offset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool SameIdentity(const Actor& left, const Actor& right) {
        return left.object == right.object;
    }

    static const Actor* FindObjectActor(const std::vector<Actor>& actors, uint32_t object) {
        const auto found = std::lower_bound(
            actors.begin(), actors.end(), object,
            [](const Actor& actor, uint32_t value) { return actor.object < value; });
        return found != actors.end() && found->object == object ? &*found : nullptr;
    }

    static const Actor* FindNpcActor(const std::vector<Actor>& actors, uint32_t npc) {
        const auto found = std::find_if(
            actors.begin(), actors.end(),
            [npc](const Actor& actor) { return actor.npc == npc; });
        return found != actors.end() ? &*found : nullptr;
    }

    static void SortUnique(std::vector<uint32_t>& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    static bool Contains(const std::vector<uint32_t>& values, uint32_t value) {
        return std::binary_search(values.begin(), values.end(), value);
    }

    bool NpcId(const uint8_t* unit, uint32_t& npc) const {
        npc = 0;
        uint32_t pilot = 0;
        const uint8_t* definition = nullptr;
        if (!unit || !gridFind_ || !gridData_ ||
            !Read(unit, 0x2B68, pilot) || !(pilot & 0x80000000u) ||
            !Read(unit, 0x320C, definition) || !definition ||
            !Read(definition, 0, npc) || !npc)
            return false;
        const uint8_t* native = nullptr;
        return gridFind_(gridData_(), &native, npc,
                         static_cast<int>(sdgo::map_catalog::kNpcTemplateType), 0) &&
               native == definition;
    }

    bool ReadActor(uint8_t* manager, uint8_t* unit, uint8_t list, Actor& actor) const {
        if (!manager || !unit || !findObject_) return false;
        uint8_t* node = nullptr;
        uint8_t* target = nullptr;
        actor = {};
        actor.list = list;
        if (!Read(unit, 0x04, actor.object) || !actor.object ||
            findObject_(manager, actor.object) != unit ||
            !Read(unit, 0x2B68, actor.pilot) ||
            !Read(unit, 0x38, actor.hp) ||
            !Read(unit, 0x3C, actor.maxHp) ||
            !Read(unit, 0x31E9, actor.controlled) ||
            !Read(unit, 0x9D8, actor.hurt) ||
            !Read(unit, 0x08, node))
            return false;
        if (node && Read(node, 0x88, actor.x) && Read(node, 0x8C, actor.y) &&
            Read(node, 0x90, actor.z) && std::isfinite(actor.x) &&
            std::isfinite(actor.y) && std::isfinite(actor.z))
            actor.position = 1;
        if (Read(unit, 0x3130, target) && target)
            Read(target, 0x04, actor.target);
        uint32_t npc = 0;
        if (NpcId(unit, npc)) actor.npc = npc;
        return true;
    }

    bool CollectList(uint8_t* manager, Node* head, uint32_t expected,
                     uint8_t list, std::vector<Actor>& output) const {
        if (!head || expected > 4096) return false;
        Node* node = nullptr;
        if (!Read(reinterpret_cast<const uint8_t*>(head), 0, node)) return false;
        uint32_t seen = 0;
        while (node && node != head && seen < expected) {
            Node* next = nullptr;
            uint8_t* unit = nullptr;
            if (!Read(reinterpret_cast<const uint8_t*>(node), 0, next) ||
                !Read(reinterpret_cast<const uint8_t*>(node), 8, unit) || !unit)
                return false;
            Actor actor{};
            if (!ReadActor(manager, unit, list, actor)) return false;
            output.push_back(actor);
            node = next;
            ++seen;
        }
        return node == head && seen == expected;
    }

    bool Collect(uint8_t* manager, std::vector<Actor>& output) const {
        output.clear();
        Node* players = nullptr;
        Node* npcs = nullptr;
        uint32_t playerCount = 0;
        uint32_t npcCount = 0;
        if (!manager || !Read(manager, 0x48, players) ||
            !Read(manager, 0x4C, playerCount) ||
            !Read(manager, 0x70, npcs) || !Read(manager, 0x74, npcCount) ||
            !CollectList(manager, players, playerCount, 1, output) ||
            !CollectList(manager, npcs, npcCount, 2, output)) {
            output.clear();
            return false;
        }
        std::sort(output.begin(), output.end(),
                  [](const Actor& left, const Actor& right) {
                      return left.object < right.object;
                  });
        return std::adjacent_find(output.begin(), output.end(), SameIdentity) == output.end();
    }

    void Visual(const char* label) {
        ++visualSequence_;
        strncpy_s(visualLabel_, label ? label : "campaign", _TRUNCATE);
    }

    static void ActorLine(const SdgoGameTestContextV1* context,
                          const char* prefix, uint32_t phase, const Actor& actor) {
        char text[320] = {};
        sprintf_s(text,
                  "%s phase=%u list=%c object=%08X pilot=%08X npc=%u hp=%d/%d hurt=%u control=%u target=%08X pos=%u:(%.1f,%.1f,%.1f)",
                  prefix, phase, actor.list == 1 ? 'P' : 'N', actor.object,
                  actor.pilot, actor.npc, actor.hp, actor.maxHp,
                  unsigned(actor.hurt), unsigned(actor.controlled), actor.target,
                  unsigned(actor.position), actor.x, actor.y, actor.z);
        context->Observe(text);
    }

    void LogActors(const SdgoGameTestContextV1* context, const char* prefix,
                   uint32_t phase) const {
        for (const auto& actor : actors_) ActorLine(context, prefix, phase, actor);
    }

    void BeginParty(const SdgoGameTestContextV1* context,
                    const SdgoMissionAiClientSnapshotV1& snapshot,
                    uint8_t* manager, uint64_t now, uint32_t phase) {
        partyObserved_ = true;
        partyStartedAt_ = now;
        partySources_.clear();
        partyBefore_.clear();
        partyTransientActors_.clear();
        uint8_t* local = nullptr;
        if (Read(manager, 0x88, local) && local) {
            uint32_t object = 0;
            if (Read(local, 0x04, object) && object) partySources_.push_back(object);
        }
        for (uint32_t i = 0; i < snapshot.ai_record_count && i < 12; ++i)
            if (snapshot.participants[i].object_id)
                partySources_.push_back(snapshot.participants[i].object_id);
        SortUnique(partySources_);
        for (const auto& actor : previousActors_) partyBefore_.push_back(actor.object);
        SortUnique(partyBefore_);
        char text[192] = {};
        sprintf_s(text,
                  "campaign party cinematic began phase=%u sources=%u preexisting=%u ai=%u",
                  phase, unsigned(partySources_.size()),
                  unsigned(partyBefore_.size()), snapshot.ai_record_count);
        context->Observe(text);
        for (uint32_t source : partySources_) {
            const Actor* actor = FindObjectActor(actors_, source);
            if (!actor) actor = FindObjectActor(previousActors_, source);
            if (actor) ActorLine(context, "campaign party source", phase, *actor);
        }
        Visual("party-begin");
    }

    void ObserveSkipConsumption(const SdgoGameTestContextV1* context) {
        const uint32_t consumed = static_cast<uint32_t>(
            InterlockedCompareExchange(&skipConsumedCount_, 0, 0));
        if (skipArmed_ && consumed > skipQueryBaseline_ && !skipConsumed_) {
            skipConsumed_ = true;
            if (context) {
                char text[160] = {};
                sprintf_s(text,
                          "campaign original phase-cinematic consumer read raw scan-code 1 queries=%u->%u",
                          skipQueryBaseline_, consumed);
                context->Observe(text);
            }
        }
    }

    void ObserveParty(const SdgoGameTestContextV1* context, uint64_t now) {
        for (const auto& actor : actors_) {
            if (!Contains(partySources_, actor.object) &&
                !Contains(partyBefore_, actor.object))
                partyTransientActors_.push_back(actor.object);
        }
        SortUnique(partyTransientActors_);
        if (now - lastPartyMotionAt_ >= 500) {
            lastPartyMotionAt_ = now;
            for (uint32_t transient : partyTransientActors_) {
                if (const Actor* actor = FindObjectActor(actors_, transient))
                    ActorLine(context, "campaign party transient actor", phase_, *actor);
            }
        }
        if (skipEnabled_ && !skipArmed_ && now - partyStartedAt_ >= skipDelayMs_) {
            skipQueryBaseline_ = static_cast<uint32_t>(
                InterlockedCompareExchange(&skipConsumedCount_, 0, 0));
            InterlockedExchange(&skipPulse_, 1);
            skipArmed_ = true;
            context->Observe(
                "campaign armed one raw Escape pulse for the source-verified native phase-cinematic query");
        }
        ObserveSkipConsumption(context);
    }

    void ObservePartyCleanup(const SdgoGameTestContextV1* context,
                             const SdgoMissionAiClientSnapshotV1& snapshot,
                             uint8_t* manager) {
        bool transientGone = !partyTransientActors_.empty();
        for (uint32_t transient : partyTransientActors_)
            transientGone = transientGone && !FindObjectActor(actors_, transient);
        bool sourcesPresent = !partySources_.empty();
        for (uint32_t source : partySources_)
            sourcesPresent = sourcesPresent && FindObjectActor(actors_, source);
        uint8_t* local = nullptr;
        uint32_t localObject = 0;
        uint8_t localControl = 0;
        const bool localRestored =
            Read(manager, 0x88, local) && local && Read(local, 0x04, localObject) &&
            Contains(partySources_, localObject) && Read(local, 0x31E9, localControl) &&
            localControl != 0;
        bool wingmenRestored = true;
        for (uint32_t i = 0; i < snapshot.ai_record_count && i < 12; ++i) {
            const auto& participant = snapshot.participants[i];
            wingmenRestored = wingmenRestored && participant.object_id &&
                Contains(partySources_, participant.object_id) && participant.mapping_valid &&
                participant.valid_registered_object && participant.ai_enabled;
        }
        if (!transientGone || !sourcesPresent || !localRestored || !wingmenRestored)
            return;
        partyCleanupPending_ = false;
        partyCleanupObserved_ = true;
        char text[256] = {};
        sprintf_s(text,
                  "campaign party cleanup retained original sources=%u removed_observed_transients=%u local_control=%u wingmen=%u duration_ms=%llu",
                  unsigned(partySources_.size()), unsigned(partyTransientActors_.size()),
                  unsigned(localControl), snapshot.ai_record_count,
                  static_cast<unsigned long long>(partyEndedAt_ - partyStartedAt_));
        context->Observe(text);
        Visual("party-cleanup");
    }

    void ObserveTargets(const SdgoGameTestContextV1* context,
                        const NativeDeathProbe& deaths) {
        if ((mode_ != kFullEight && mode_ != kDamageGate) ||
            phase_ != 8 || !targetNpc_) return;
        const bool guardedGate = mode_ == kDamageGate;
        const Actor* target = FindNpcActor(actors_, targetNpc_);
        const Actor* guard = guardNpc_ ? FindNpcActor(actors_, guardNpc_) : nullptr;

        if (guardNpc_ && guard && guard->hp > 0) {
            if (!guardAliveObserved_) {
                guardAliveObserved_ = true;
                guardObject_ = guard->object;
                guardPilot_ = guard->pilot;
                guardBaselineHp_ = guardLastHp_ = guard->hp;
                ActorLine(context, "campaign Gyan baseline", phase_, *guard);
                Visual("gate-guard-arrival");
            }
            if (guard->hp < guardLastHp_) {
                guardDamageObserved_ = true;
                char text[160] = {};
                sprintf_s(text,
                          "campaign Gyan authoritative HP decreased npc=%u before=%d after=%d",
                          guardNpc_, guardLastHp_, guard->hp);
                context->Observe(text);
                Visual("gate-guard-damage");
            }
            guardLastHp_ = guard->hp;
        }
        if (guardAliveObserved_ && guardPilot_ &&
            deaths.ObservedOrdinaryVictim(guardPilot_)) {
            if (!guardDeathObserved_) {
                guardDeathObserved_ = true;
                context->Observe(
                    "campaign native 0570 ordinary-death receipt identified the observed Gyan pilot");
                Visual("gate-guard-death");
            }
            if ((!guard || guard->hp <= 0) && !guardDepartureObserved_) {
                guardDepartureObserved_ = true;
                context->Observe(
                    "campaign Gyan was absent or nonliving only after its native ordinary-death receipt");
            }
        }

        if (target && target->hp > 0) {
            if (!targetAliveObserved_) {
                targetAliveObserved_ = true;
                targetObject_ = target->object;
                targetPilot_ = target->pilot;
                targetBaselineHp_ = targetLastHp_ = target->hp;
                targetLastHurt_ = target->hurt;
                ActorLine(context, "campaign protected target baseline", phase_, *target);
                Visual("gate-target-baseline");
            }
            if (target->hp < targetLastHp_) {
                if (!guardedGate || guardDeathObserved_)
                    targetDamageAfterGuard_ = true;
                else
                    targetDamageBeforeGuard_ = true;
                char text[192] = {};
                sprintf_s(text,
                          "campaign protected target authoritative HP decreased npc=%u before=%d after=%d guard_death=%u",
                          targetNpc_, targetLastHp_, target->hp,
                          guardDeathObserved_ ? 1u : 0u);
                context->Observe(text);
                Visual("gate-target-damage");
            }
            if (guardedGate && !guardAliveObserved_ &&
                target->hp == targetBaselineHp_ &&
                target->hurt == 1 && targetLastHurt_ != 1) {
                targetHurtStateTransitionObserved_ = true;
                context->Observe(
                    "campaign protected nuke changed from its recorded baseline into native state 1 while authoritative HP stayed unchanged before Gyan appeared; no visual flinch is inferred");
                Visual("gate-target-state1");
            }
            targetLastHp_ = target->hp;
            targetLastHurt_ = target->hurt;
        }
        if (targetAliveObserved_ && targetPilot_ &&
            deaths.ObservedOrdinaryVictim(targetPilot_) && !targetDeathObserved_) {
            targetDeathObserved_ = true;
            context->Observe(
                "campaign native 0570 ordinary-death receipt identified the observed nuke pilot");
            Visual("gate-target-death");
        }

        const bool complete = guardedGate
            ? targetHurtStateTransitionObserved_ && guardDeathObserved_ &&
                  guardDepartureObserved_ && targetDamageAfterGuard_ &&
                  targetDeathObserved_
            : targetDamageAfterGuard_ && targetDeathObserved_;
        if (complete && !gateTransitionReported_) {
            gateTransitionReported_ = true;
            context->Observe(guardedGate
                ? "campaign observed unchanged shielded nuke HP, genuine Gyan death, then authoritative nuke damage and genuine nuke death in phase 8"
                : "campaign observed direct authoritative nuke HP loss and genuine native nuke death in phase 8 without a prerequisite guard gate");
        }
    }

    static bool ParseMissionSuccess(const uint8_t* packet,
                                    MissionSuccessReceipt& receipt) {
        receipt = {};
        if (!packet) return false;
        __try {
            if (*reinterpret_cast<const uint16_t*>(packet) != 0x0905) return false;
            receipt.clearId = *reinterpret_cast<const uint32_t*>(packet + 6);
            receipt.eventOrder = *reinterpret_cast<const uint32_t*>(packet + 10);
            receipt.count = packet[14];
            if (receipt.count > 10) return false;
            for (uint32_t i = 0; i < receipt.count; ++i)
                receipt.ids[i] = *reinterpret_cast<const uint32_t*>(packet + 15 + i * 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            receipt = {};
            return false;
        }
        return true;
    }

    void PublishMissionSuccess(const MissionSuccessReceipt& receipt,
                               uint32_t originalResult) {
        InterlockedIncrement(&successWriteVersion_);
        InterlockedExchange(&successObservedPhase_, static_cast<LONG>(receipt.observedPhase));
        InterlockedExchange(&successClearId_, static_cast<LONG>(receipt.clearId));
        InterlockedExchange(&successEventOrder_, static_cast<LONG>(receipt.eventOrder));
        InterlockedExchange(&successOriginalResult_, static_cast<LONG>(originalResult));
        InterlockedExchange(&successCount_, static_cast<LONG>(receipt.count));
        for (uint32_t i = 0; i < 10; ++i)
            InterlockedExchange(&successIds_[i], static_cast<LONG>(receipt.ids[i]));
        InterlockedIncrement(&successSequence_);
        InterlockedIncrement(&successWriteVersion_);
    }

    MissionSuccessReceipt ReadMissionSuccess() const {
        MissionSuccessReceipt receipt{};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successWriteVersion_), 0, 0);
            if (before & 1) continue;
            receipt.sequence = static_cast<uint32_t>(InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successSequence_), 0, 0));
            receipt.observedPhase = static_cast<uint32_t>(InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successObservedPhase_), 0, 0));
            receipt.clearId = static_cast<uint32_t>(InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successClearId_), 0, 0));
            receipt.eventOrder = static_cast<uint32_t>(InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successEventOrder_), 0, 0));
            receipt.originalResult = static_cast<uint32_t>(InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successOriginalResult_), 0, 0));
            receipt.count = static_cast<uint8_t>(InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successCount_), 0, 0));
            for (uint32_t i = 0; i < 10; ++i)
                receipt.ids[i] = static_cast<uint32_t>(InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&successIds_[i]), 0, 0));
            const LONG after = InterlockedCompareExchange(
                const_cast<volatile LONG*>(&successWriteVersion_), 0, 0);
            if (before == after && !(after & 1)) return receipt;
        }
        return {};
    }

    void ObserveMissionSuccess(const SdgoGameTestContextV1* context) {
        const MissionSuccessReceipt receipt = ReadMissionSuccess();
        if (!receipt.sequence || receipt.sequence == observedSuccessSequence_) return;
        observedSuccessSequence_ = receipt.sequence;
        bool targetListed = false;
        for (uint32_t i = 0; i < receipt.count; ++i)
            targetListed = targetListed || receipt.ids[i] == targetPilot_;
        if (receipt.observedPhase == 8 && receipt.originalResult == 1) {
            phaseEightSuccessObserved_ = true;
        }
        char text[384] = {};
        sprintf_s(text,
                  "campaign native 0905 success receipt sequence=%u observed_phase=%u clear_id=%u event_order=%u count=%u original_result=%u target_pilot=%08X target_listed=%u ids=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X",
                  receipt.sequence, receipt.observedPhase, receipt.clearId,
                  receipt.eventOrder, unsigned(receipt.count), receipt.originalResult,
                  targetPilot_, targetListed ? 1u : 0u,
                  receipt.ids[0], receipt.ids[1], receipt.ids[2], receipt.ids[3],
                  receipt.ids[4], receipt.ids[5], receipt.ids[6], receipt.ids[7],
                  receipt.ids[8], receipt.ids[9]);
        context->Observe(text);
        Visual("mission-success-0905");
    }

    static bool __cdecl RawKeyPressed(uint32_t scan) {
        MissionCampaignObserver* const owner = active_;
        if (!owner || !owner->keyPressedOriginal_) return false;
        const DWORD incomingError = GetLastError();
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const uint32_t phase = static_cast<uint32_t>(
            InterlockedCompareExchange(&owner->hookPhase_, 0, 0));
        const bool demo = InterlockedCompareExchange(&owner->hookDemo_, 0, 0) != 0;
        if (scan == 1 && caller == kPartySkipCaller &&
            phase == owner->partyPhase_ && demo &&
            InterlockedCompareExchange(&owner->skipPulse_, 0, 1) == 1) {
            InterlockedIncrement(&owner->skipConsumedCount_);
            SetLastError(incomingError);
            return true;
        }
        SetLastError(incomingError);
        const bool result = owner->keyPressedOriginal_(scan);
        const DWORD originalError = GetLastError();
        SetLastError(originalError);
        return result;
    }

    void PollMapMissionSuccess() {
        if (!mapSuccessGetter_ || !mission_) return;
        SdgoMapMissionSuccessV1 status{};
        status.size = sizeof(status);
        status.version = SDGO_MAP_MISSION_SUCCESS_VERSION;
        if (!mapSuccessGetter_(&status) || !status.hook_active ||
            status.sequence == 0 || status.sequence == mapSuccessObservedSequence_)
            return;
        mapSuccessObservedSequence_ = status.sequence;
        if (status.mission_id != mission_ || !status.phase || status.phase > 8 ||
            status.count > 10)
            return;
        MissionSuccessReceipt receipt{};
        receipt.sequence = status.sequence;
        receipt.observedPhase = status.phase;
        receipt.clearId = status.clear_id;
        receipt.eventOrder = status.event_order;
        receipt.originalResult = status.original_result;
        receipt.count = static_cast<uint8_t>(status.count);
        for (uint32_t i = 0; i < status.count; ++i)
            receipt.ids[i] = status.pilot_ids[i];
        PublishMissionSuccess(receipt, status.original_result);
    }
    static int __stdcall MissionSuccess(const uint8_t* packet) {
        MissionCampaignObserver* const owner = active_;
        const DWORD incomingError = GetLastError();
        MissionSuccessReceipt receipt{};
        const bool parsed = owner && ParseMissionSuccess(packet, receipt);
        if (owner && parsed)
            receipt.observedPhase = static_cast<uint32_t>(
                InterlockedCompareExchange(&owner->hookPhase_, 0, 0));
        SetLastError(incomingError);
        const int result = owner && owner->missionSuccessOriginal_
            ? owner->missionSuccessOriginal_(packet) : 0;
        const DWORD originalError = GetLastError();
        if (owner && parsed) owner->PublishMissionSuccess(receipt, static_cast<uint32_t>(result));
        SetLastError(originalError);
        return result;
    }

public:
    bool Configure(Mode mode, uint32_t targetNpc, uint32_t guardNpc,
                   uint32_t partyPhase, bool skipEnabled, uint32_t skipDelayMs) {
        if (mode > kDamageGate || partyPhase > 8 || skipDelayMs > 10000)
            return false;
        if (mode == kDamageGate && (!targetNpc || !guardNpc || targetNpc == guardNpc))
            return false;
        if (mode == kFullEight &&
            (!targetNpc || (guardNpc && targetNpc == guardNpc) || !partyPhase))
            return false;
        if (mode == kOff && (targetNpc || guardNpc))
            return false;
        if (skipEnabled && (mode != kFullEight || !partyPhase))
            return false;
        mode_ = mode;
        targetNpc_ = targetNpc;
        guardNpc_ = guardNpc;
        partyPhase_ = partyPhase;
        skipEnabled_ = skipEnabled;
        skipDelayMs_ = skipDelayMs;
        return true;
    }

    bool Install(const SdgoApi* api, SdgoMod* mod) {
        if (mode_ == kOff) return true;
        if (!api || !api->Symbol || !api->HookCode || active_) return false;
        gridFind_ = reinterpret_cast<GridFind>(api->Symbol(mod, "grid.find"));
        gridData_ = reinterpret_cast<GridData>(api->Symbol(mod, "grid.data"));
        findObject_ = reinterpret_cast<FindObject>(api->Symbol(mod, "um.find_object"));
        if (!gridFind_ || !gridData_ || !findObject_) return false;
        active_ = this;
        const HMODULE mapModule = GetModuleHandleW(L"map_catalog.dll");
        mapSuccessGetter_ = mapModule
            ? reinterpret_cast<SdgoMapCatalogGetMissionSuccessV1Fn>(
                  GetProcAddress(mapModule, SDGO_MAP_MISSION_SUCCESS_EXPORT))
            : nullptr;
        bool mapOwnsSuccessHook = false;
        if (mapSuccessGetter_) {
            SdgoMapMissionSuccessV1 status{};
            status.size = sizeof(status);
            status.version = SDGO_MAP_MISSION_SUCCESS_VERSION;
            mapOwnsSuccessHook = mapSuccessGetter_(&status) && status.hook_active != 0;
            if (mapOwnsSuccessHook) mapSuccessObservedSequence_ = status.sequence;
        }
        if (!mapOwnsSuccessHook &&
            (!api->HookCode(mod, "campaign.mission_success_0905",
                            reinterpret_cast<void*>(&MissionSuccess),
                            reinterpret_cast<void**>(&missionSuccessOriginal_)) ||
             !missionSuccessOriginal_)) {
            active_ = nullptr;
            return false;
        }
        if (!mapOwnsSuccessHook) mapSuccessGetter_ = nullptr;
        if (skipEnabled_ &&
            (!api->HookCode(mod, "title_test.key_pressed",
                            reinterpret_cast<void*>(&RawKeyPressed),
                            reinterpret_cast<void**>(&keyPressedOriginal_)) ||
             !keyPressedOriginal_)) {
            active_ = nullptr;
            return false;
        }
        return true;
    }

    bool Enabled() const { return mode_ != kOff; }
    Mode CampaignMode() const { return mode_; }
    uint32_t CurrentMissionId() const { return mission_; }
    uint32_t CurrentPhase() const { return phase_; }
    uint32_t VisualSequence() const { return visualSequence_; }
    const char* VisualLabel() const { return visualLabel_; }
    bool FastCaptureActive() const {
        return (mode_ == kFullEight || mode_ == kDamageGate) &&
               ((phase_ == 8 && !targetDeathObserved_) ||
                (partyPhase_ && phase_ == partyPhase_ && demo_));
    }
    MissionSuccessReceipt SuccessReceipt() const { return ReadMissionSuccess(); }

    bool Poll(const SdgoGameTestContextV1* context,
              const SdgoMissionAiClientSnapshotV1& snapshot,
              uint32_t nativeAppState, uint32_t profilePilotId,
              const NativeDeathProbe& deaths) {
        if (mode_ == kOff) return true;
        if (!context || nativeAppState != 22 || !profilePilotId || !snapshot.active ||
            snapshot.size != sizeof(snapshot) ||
            snapshot.version != SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION ||
            !snapshot.mode_object || !snapshot.unit_manager_object)
            return true;
        auto* mode = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(snapshot.mode_object));
        auto* manager = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(snapshot.unit_manager_object));
        const uint8_t* record = nullptr;
        uint32_t mission = 0;
        uint32_t phase = 0;
        uint8_t demo = 0;
        const uint64_t now = GetTickCount64();
        const bool modeRead = Read(mode, 0xE8, record) && record &&
            Read(record, 0, mission) && mission != 0;
        const bool phaseRead = Read(mode, 0x620, phase);
        const bool demoRead = Read(mode, 0x104, demo);
        // The opening script runs before EventOrder 1. Native DemoPlay can
        // therefore legitimately retain phase 0 until the opening finishes.
        // Keep validating the complete lists during that opening as well.
        const bool phaseValid = phaseRead && demoRead && phase <= 8 &&
            (phase != 0 || demo != 0);
        const bool listsRead = modeRead && phaseValid && Collect(manager, actors_);
        if (!modeRead || !phaseValid || !listsRead) {
            ++actorReadFailures_;
            if (!actorReadFailureAt_) {
                actorReadFailureAt_ = now;
                char text[256]{};
                sprintf_s(text,
                    "campaign observer read deferred mode_ok=%u mission=%u phase_ok=%u phase=%u demo_ok=%u demo=%u lists_ok=%u manager=%p",
                    modeRead ? 1u : 0u, mission, phaseRead ? 1u : 0u, phase,
                    demoRead ? 1u : 0u, unsigned(demo), listsRead ? 1u : 0u, manager);
                context->Observe(text);
            }
            if (now - actorReadFailureAt_ < 2000) return true;
            context->Assert(0,
                "campaign observer recovers complete canonical UnitManager actor lists within two seconds");
            return false;
        }
        actorReadFailureAt_ = 0;
        actorReadFailures_ = 0;

        const bool changedPhase = mission_ != mission || phase_ != phase;
        const bool changedDemo = demo_ != (demo != 0);
        if (changedPhase || changedDemo || now - lastActorLogAt_ >= 10000) {
            char text[192] = {};
            sprintf_s(text,
                      "campaign native state mission=%u phase=%u demo=%u actors=%u players=%u ai=%u",
                      mission, phase, demo ? 1u : 0u, unsigned(actors_.size()),
                      snapshot.unit_manager_player_count, snapshot.ai_record_count);
            context->Observe(text);
            lastActorLogAt_ = now;
            if (changedPhase) LogActors(context, "campaign actor census", phase);
        }

        InterlockedExchange(&hookPhase_, static_cast<LONG>(phase));
        InterlockedExchange(&hookDemo_, demo ? 1 : 0);
        const bool enteringParty = partyPhase_ && phase == partyPhase_ && demo &&
                                   (!partyObserved_ || phase_ != phase || !demo_);
        mission_ = mission;
        phase_ = phase;
        if (enteringParty) BeginParty(context, snapshot, manager, now, phase);
        if (partyObserved_ && phase == partyPhase_ && demo)
            ObserveParty(context, now);
        if (partyObserved_ && demo_ && !demo && phase == partyPhase_) {
            partyEndedAt_ = now;
            partyCleanupPending_ = true;
            InterlockedExchange(&skipPulse_, 0);
            ObserveSkipConsumption(context);
            char text[160] = {};
            sprintf_s(text, "campaign party cinematic ended duration_ms=%llu skip_consumed=%u",
                      static_cast<unsigned long long>(partyEndedAt_ - partyStartedAt_),
                      skipConsumed_ ? 1u : 0u);
            context->Observe(text);
        }
        demo_ = demo != 0;
        if (partyCleanupPending_)
            ObservePartyCleanup(context, snapshot, manager);

        ObserveTargets(context, deaths);
        PollMapMissionSuccess();
        ObserveMissionSuccess(context);
        previousActors_ = actors_;
        return true;
    }

    void Finish(const SdgoGameTestContextV1* context) {
        if (mode_ == kOff || !context) return;
        InterlockedExchange(&skipPulse_, 0);
        ObserveSkipConsumption(context);
        PollMapMissionSuccess();
        ObserveMissionSuccess(context);
        if (mode_ == kFullEight) {
            context->Assert(partyObserved_,
                            "the full campaign entered the native phase-6 party cinematic");
            context->Assert(!partyTransientActors_.empty(),
                            "the phase-6 party cinematic created at least one native transient actor absent before the demo");
            context->Assert(partyCleanupObserved_,
                            "the party cleanup removed every observed transient actor while retaining original sources and restoring local control and wingman mappings");
            context->Assert(phaseEightSuccessObserved_,
                            "the full campaign reached an original native 0905 success handler while the observer was in phase 8");
            const uint64_t partyDuration = partyEndedAt_ > partyStartedAt_
                ? partyEndedAt_ - partyStartedAt_ : 0;
            if (skipEnabled_) {
                context->Assert(skipConsumed_,
                                "the source-verified native phase-cinematic consumer read the one-shot raw Escape input");
                context->Assert(partyDuration && partyDuration < 4000,
                                "the consumed native Escape input ended the authored five-second party cinematic early");
            } else {
                context->Assert(partyDuration >= 4000 && partyDuration <= 7500,
                                "the normal authored party cinematic completed its five-second interval");
            }
            context->Assert(targetAliveObserved_ && targetBaselineHp_ > 0,
                            "the full campaign observed the real phase-8 nuke actor and authoritative HP baseline");
            context->Assert(targetDamageAfterGuard_,
                            "ordinary native fire directly decreased the phase-8 nuke actor authoritative HP without a prerequisite guard gate");
            context->Assert(targetDeathObserved_,
                            "a genuine native ordinary-death receipt identified the directly attacked nuke actor");
        }
        if (mode_ == kDamageGate) {
            context->Assert(targetAliveObserved_ && targetBaselineHp_ == 50000,
                            "the focused gate run observed the real 50,000-HP nuke actor");
            context->Assert(targetHurtStateTransitionObserved_ && !targetDamageBeforeGuard_,
                            "ordinary fire produced a target-side native state-1 transition from the recorded baseline while authoritative nuke HP stayed unchanged before Gyan death; presentation remains a separate visual check");
            context->Assert(guardAliveObserved_ && guardBaselineHp_ > 0 &&
                                guardDeathObserved_ && guardDepartureObserved_,
                            "the delayed real Gyan was observed alive and then identified by a genuine native ordinary-death receipt before departure");
            context->Assert(targetDamageAfterGuard_ && targetDeathObserved_,
                            "after genuine Gyan death, authoritative nuke HP decreased and a genuine native ordinary-death receipt identified the nuke");
            context->Assert(phaseEightSuccessObserved_,
                            "the original client completed a valid native 0905 success handler while the observer was in phase 8");
        }
    }
};
