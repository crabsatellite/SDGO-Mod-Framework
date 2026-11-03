#pragma once

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sdgo/battle_ai_observer.h"
#include "sdgo/game_test.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "sdgo/sdgo_api.h"

namespace sdgo_game_test_death
{
    constexpr unsigned kMaximumVictims = 12;

    struct DeadVictim
    {
        uint32_t pilotId = 0;
        uint8_t raw4 = 0;
        uint8_t raw9 = 0;
        uint8_t raw10 = 0;
    };

    struct DeadPacket
    {
        uint32_t killerPilotId = 0;
        uint8_t nativeVictimCount = 0;
        uint8_t capturedVictimCount = 0;
        uint8_t truncated = 0;
        DeadVictim victims[kMaximumVictims] = {};
    };

    struct MapDamagePacket
    {
        uint32_t victimPilotId = 0;
        uint8_t raw10 = 0;
        uint8_t raw11 = 0;
        uint8_t raw12 = 0;
    };

    inline bool ParseDeadPacket(const uint8_t* packet, DeadPacket* output)
    {
        if (!packet || !output)
            return false;
        DeadPacket parsed = {};
        __try {
            parsed.killerPilotId =
                *reinterpret_cast<const uint32_t*>(packet + 0x06);
            parsed.nativeVictimCount = packet[0x0B];
            parsed.capturedVictimCount =
                parsed.nativeVictimCount < kMaximumVictims
                    ? parsed.nativeVictimCount
                    : static_cast<uint8_t>(kMaximumVictims);
            parsed.truncated =
                parsed.nativeVictimCount > kMaximumVictims ? 1 : 0;
            for (unsigned i = 0; i < parsed.capturedVictimCount; ++i) {
                const uint8_t* record = packet + 0x0C + i * 0x0B;
                parsed.victims[i].pilotId =
                    *reinterpret_cast<const uint32_t*>(record);
                parsed.victims[i].raw4 = record[0x04];
                parsed.victims[i].raw9 = record[0x09];
                parsed.victims[i].raw10 = record[0x0A];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        *output = parsed;
        return true;
    }

    inline bool ParseMapDamagePacket(
        const uint8_t* packet, MapDamagePacket* output)
    {
        if (!packet || !output)
            return false;
        MapDamagePacket parsed = {};
        __try {
            parsed.victimPilotId =
                *reinterpret_cast<const uint32_t*>(packet + 0x06);
            parsed.raw10 = packet[0x0A];
            parsed.raw11 = packet[0x0B];
            parsed.raw12 = packet[0x0C];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        *output = parsed;
        return true;
    }

    struct RegisteredAi
    {
        uint8_t registered = 0;
        uint8_t aiEnabled = 0;
        uint8_t localControl = 0;
        uint8_t remoteCopy = 0;
        uint8_t masterNpc = 0;
    };

    inline RegisteredAi FindRegisteredAi(
        const SdgoPvpAiClientSnapshotV1& snapshot,
        uint32_t pilotId, uint32_t objectId)
    {
        RegisteredAi result = {};
        if (snapshot.size != sizeof(snapshot) ||
            snapshot.version !=
                SDGO_PVP_AI_CLIENT_OBSERVER_VERSION)
            return result;
        const uint32_t count =
            snapshot.bot_count < _countof(snapshot.bots)
                ? snapshot.bot_count
                : static_cast<uint32_t>(
                      _countof(snapshot.bots));
        for (uint32_t i = 0; i < count; ++i) {
            const auto& bot = snapshot.bots[i];
            if (!bot.valid_registered_object)
                continue;
            const bool pilotMatches =
                pilotId != 0 && bot.pilot_id == pilotId;
            const bool objectMatches =
                objectId != 0 && bot.object_id == objectId;
            if (!pilotMatches && !objectMatches)
                continue;
            result.registered = 1;
            result.aiEnabled = bot.ai_enabled;
            result.localControl = bot.local_control;
            result.remoteCopy = bot.remote_copy;
            result.masterNpc = bot.master_npc;
            return result;
        }
        return result;
    }
}

// Opt-in passive observation of the original client death handlers. The hooks
// copy packet identity and current unit state, then call the native handlers
// with the original arguments. They never write HP, position, input or packets.
class NativeDeathProbe
{
    enum class Kind : uint8_t
    {
        Ordinary,
        MapDamage,
        MovementMapRequest,
        EventMapRequest
    };

    struct Identity
    {
        uint32_t pilotId = 0;
        uint32_t objectId = 0;
        uint32_t npcId = 0;
        int32_t hp = 0;
        int32_t maxHp = 0;
        int32_t team = 0;
        float x = 0;
        float y = 0;
        float z = 0;
        uint8_t positionValid = 0;
        uint8_t registeredAi = 0;
        uint8_t aiEnabled = 0;
        uint8_t localControl = 0;
        uint8_t remoteCopy = 0;
        uint8_t masterNpc = 0;
        uint8_t playerControlEnabled = 0;
        uint8_t battleSnapshotValid = 0;
        uint32_t hurtState = 0;
        uint32_t controlBlocked = 0;
        uint32_t movementState = 0;
        uint32_t movementPending = 0;
        uint32_t attackState = 0;
        uint32_t attackPending = 0;
    };

    struct Event
    {
        uint64_t qpc = 0;
        uint32_t threadId = 0;
        Kind kind = Kind::Ordinary;
        Identity killer = {};
        Identity victim = {};
        Identity after = {};
        uint8_t aliveRelocation = 0;
        uint8_t nativeVictimCount = 0;
        uint8_t recordIndex = 0;
        uint8_t raw4 = 0;
        uint8_t raw9 = 0;
        uint8_t raw10 = 0;
        uint8_t raw11 = 0;
        uint8_t raw12 = 0;
        uint8_t samePilot = 0;
        uint8_t sameObject = 0;
        uint8_t senderOnceBefore = 0;
        uint8_t senderOnceAfter = 0;
        uint8_t networkCountersValid = 0;
        uint8_t sendConfirmed = 0;
        uint16_t sourceOpcode = 0;
        uint16_t reserved = 0;
        uint32_t networkSendDelta = 0;
        uint32_t networkByteDelta = 0;
    };

    using HandlerFn = int(__thiscall*)(void*, uint8_t*);
    using MovementMapSenderFn =
        void(__thiscall*)(uint8_t*, void*);
    using EventMapSenderFn =
        int(__thiscall*)(void*, uint8_t*);
    using ObjectIdFromPilotFn = int(__thiscall*)(void*, uint32_t);
    using FindObjectFn = void*(__thiscall*)(void*, int);
    using NetworkFn = uint8_t*(__cdecl*)();

    static constexpr unsigned kQueueCapacity = 256;
    static constexpr size_t kUnitNode = 0x08;
    static constexpr size_t kUnitTeam = 0x20;
    static constexpr size_t kUnitHp = 0x38;
    static constexpr size_t kUnitMaxHp = 0x3C;
    static constexpr size_t kNodePosition = 0x88;
    static constexpr size_t kUnitMoveFsm = 0x9C8;
    static constexpr size_t kUnitAttackFsm = 0x9CC;
    static constexpr size_t kUnitPilotId = 0x2B68;
    static constexpr size_t kUnitPlayerControlEnabled = 0x31E9;
    static constexpr size_t kUnitNpcDataId = 0x3238;
    static constexpr size_t kMoveOwner = 0x21C;
    static constexpr size_t kMovementMapSent = 0x218;
    static constexpr size_t kNetworkSendCount = 0x58;
    static constexpr size_t kNetworkSendBytes = 0x5C;

    inline static NativeDeathProbe* active_ = nullptr;
    void** unitManager_ = nullptr;
    ObjectIdFromPilotFn objectIdFromPilot_ = nullptr;
    FindObjectFn findObject_ = nullptr;
    NetworkFn network_ = nullptr;
    SdgoPvpAiClientGetSnapshotV1Fn getPvpSnapshot_ = nullptr;
    SdgoBattleAiGetUnitSnapshotV1Fn getBattleSnapshot_ = nullptr;
    HandlerFn deadOriginal_ = nullptr;
    HandlerFn mapDamageOriginal_ = nullptr;
    MovementMapSenderFn movementMapSenderOriginal_ = nullptr;
    EventMapSenderFn eventMapSenderOriginal_ = nullptr;
    CRITICAL_SECTION queueLock_ = {};
    Event queue_[kQueueCapacity] = {};
    unsigned queueRead_ = 0;
    unsigned queueWrite_ = 0;
    unsigned queueCount_ = 0;
    bool enabled_ = false;
    bool requireTeleport_ = false;
    volatile LONG aliveTeleportRelocations_ = 0;
    volatile LONG dropped_ = 0;
    volatile LONG ordinaryVictims_ = 0;
    volatile LONG mapDamagePackets_ = 0;
    volatile LONG parseFailures_ = 0;
    volatile LONG truncatedPackets_ = 0;
    volatile LONG samePilot_ = 0;
    volatile LONG sameObject_ = 0;
    volatile LONG registeredAiMapDamagePackets_ = 0;
    volatile LONG movementMapRequestCandidates_ = 0;
    volatile LONG movementMapRequests_ = 0;
    volatile LONG registeredAiMovementMapRequests_ = 0;
    volatile LONG eventMapRequestCandidates_ = 0;
    volatile LONG eventMapRequests_ = 0;
    volatile LONG registeredAiEventMapRequests_ = 0;

    struct NetworkCounters
    {
        uint32_t sends = 0;
        uint32_t bytes = 0;
        uint8_t valid = 0;
    };

    static uint32_t FsmStateOffset(
        const uint8_t* fsm, size_t field)
    {
        if (!fsm)
            return 0;
        const uint8_t* state =
            *reinterpret_cast<const uint8_t* const*>(
                fsm + field);
        return state >= fsm && state < fsm + 0x210
            ? static_cast<uint32_t>(state - fsm)
            : 0;
    }

    void ResolveObservers()
    {
        if (!getPvpSnapshot_) {
            HMODULE matchAi = GetModuleHandleW(L"match_ai.dll");
            if (!matchAi)
                matchAi = GetModuleHandleW(nullptr);
            if (matchAi)
                getPvpSnapshot_ =
                    reinterpret_cast<
                        SdgoPvpAiClientGetSnapshotV1Fn>(
                        GetProcAddress(
                            matchAi,
                            SDGO_PVP_AI_CLIENT_SNAPSHOT_EXPORT));
        }
        if (!getBattleSnapshot_) {
            HMODULE battleAi =
                GetModuleHandleW(L"battle_ai.dll");
            if (!battleAi)
                battleAi = GetModuleHandleW(nullptr);
            if (battleAi)
                getBattleSnapshot_ =
                    reinterpret_cast<
                        SdgoBattleAiGetUnitSnapshotV1Fn>(
                        GetProcAddress(
                            battleAi,
                            SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        }
    }

    Identity CaptureUnitIdentity(
        const uint8_t* unit, uint32_t pilotId,
        uint32_t objectId)
    {
        Identity identity = {};
        identity.pilotId = pilotId;
        identity.objectId = objectId;
        __try {
            if (!unit)
                return identity;
            if (!identity.pilotId)
                identity.pilotId =
                    *reinterpret_cast<const uint32_t*>(
                        unit + kUnitPilotId);
            if (!identity.objectId)
                identity.objectId =
                    *reinterpret_cast<const uint32_t*>(
                        unit + 0x04);
            identity.npcId =
                *reinterpret_cast<const uint32_t*>(
                    unit + kUnitNpcDataId);
            identity.hp =
                *reinterpret_cast<const int32_t*>(unit + kUnitHp);
            identity.maxHp =
                *reinterpret_cast<const int32_t*>(unit + kUnitMaxHp);
            identity.team =
                *reinterpret_cast<const int32_t*>(unit + kUnitTeam);
            identity.playerControlEnabled =
                unit[kUnitPlayerControlEnabled] != 0 ? 1 : 0;
            const uint8_t* movement =
                *reinterpret_cast<const uint8_t* const*>(
                    unit + kUnitMoveFsm);
            const uint8_t* attack =
                *reinterpret_cast<const uint8_t* const*>(
                    unit + kUnitAttackFsm);
            identity.movementState =
                FsmStateOffset(movement, 0x04);
            identity.movementPending =
                FsmStateOffset(movement, 0x08);
            identity.attackState =
                FsmStateOffset(attack, 0x04);
            identity.attackPending =
                FsmStateOffset(attack, 0x08);
            const uint8_t* node =
                *reinterpret_cast<const uint8_t* const*>(
                    unit + kUnitNode);
            if (node) {
                const float* position =
                    reinterpret_cast<const float*>(
                        node + kNodePosition);
                if (std::isfinite(position[0]) &&
                    std::isfinite(position[1]) &&
                    std::isfinite(position[2])) {
                    identity.positionValid = 1;
                    identity.x = position[0];
                    identity.y = position[1];
                    identity.z = position[2];
                }
            }
            ResolveObservers();
            if (getPvpSnapshot_) {
                SdgoPvpAiClientSnapshotV1 snapshot = {};
                snapshot.size = sizeof(snapshot);
                snapshot.version =
                    SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
                if (getPvpSnapshot_(&snapshot)) {
                    const auto ai =
                        sdgo_game_test_death::
                            FindRegisteredAi(
                                snapshot,
                                identity.pilotId,
                                identity.objectId);
                    identity.registeredAi = ai.registered;
                    identity.aiEnabled = ai.aiEnabled;
                    identity.localControl = ai.localControl;
                    identity.remoteCopy = ai.remoteCopy;
                    identity.masterNpc = ai.masterNpc;
                }
            }
            if (getBattleSnapshot_ && identity.objectId) {
                SdgoBattleAiUnitSnapshotV1 snapshot = {};
                snapshot.size = sizeof(snapshot);
                snapshot.version = 1;
                if (getBattleSnapshot_(
                        identity.objectId, &snapshot)) {
                    identity.battleSnapshotValid = 1;
                    identity.hurtState =
                        snapshot.hurt_state;
                    identity.controlBlocked =
                        snapshot.control_blocked;
                    identity.aiEnabled =
                        snapshot.ai_enabled != 0 ? 1 : 0;
                    identity.movementState =
                        snapshot.movement_state;
                    identity.movementPending =
                        snapshot.movement_pending;
                    identity.attackState =
                        snapshot.attack_state;
                    identity.attackPending =
                        snapshot.attack_pending;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return identity;
    }

    Identity CaptureIdentity(uint32_t pilotId)
    {
        Identity identity = {};
        identity.pilotId = pilotId;
        __try {
            void* manager =
                unitManager_ ? *unitManager_ : nullptr;
            if (!manager || !pilotId)
                return identity;
            const int objectId =
                objectIdFromPilot_(manager, pilotId);
            if (objectId <= 0)
                return identity;
            const uint8_t* unit =
                static_cast<const uint8_t*>(
                    findObject_(manager, objectId));
            return CaptureUnitIdentity(
                unit, pilotId,
                static_cast<uint32_t>(objectId));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return identity;
        }
    }

    NetworkCounters CaptureNetworkCounters() const
    {
        NetworkCounters result = {};
        __try {
            uint8_t* network =
                network_ ? network_() : nullptr;
            if (!network)
                return result;
            result.sends =
                *reinterpret_cast<const uint32_t*>(
                    network + kNetworkSendCount);
            result.bytes =
                *reinterpret_cast<const uint32_t*>(
                    network + kNetworkSendBytes);
            result.valid = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return result;
    }

    static void ApplyNetworkDelta(
        Event& event, const NetworkCounters& before,
        const NetworkCounters& after)
    {
        if (!before.valid || !after.valid)
            return;
        event.networkCountersValid = 1;
        event.networkSendDelta =
            after.sends - before.sends;
        event.networkByteDelta =
            after.bytes - before.bytes;
        event.sendConfirmed =
            event.networkSendDelta == 1 &&
                    event.networkByteDelta == 0x0E
                ? 1
                : 0;
    }

    void Queue(Event event)
    {
        LARGE_INTEGER qpc = {};
        QueryPerformanceCounter(&qpc);
        event.qpc = static_cast<uint64_t>(qpc.QuadPart);
        event.threadId = GetCurrentThreadId();
        event.samePilot =
            event.killer.pilotId != 0 &&
                    event.killer.pilotId == event.victim.pilotId
                ? 1
                : 0;
        event.sameObject =
            event.killer.objectId != 0 &&
                    event.killer.objectId == event.victim.objectId
                ? 1
                : 0;
        if (event.samePilot)
            InterlockedIncrement(&samePilot_);
        if (event.sameObject)
            InterlockedIncrement(&sameObject_);
        if (!TryEnterCriticalSection(&queueLock_)) {
            InterlockedIncrement(&dropped_);
            return;
        }
        if (queueCount_ == kQueueCapacity) {
            InterlockedIncrement(&dropped_);
        } else {
            queue_[queueWrite_] = event;
            queueWrite_ = (queueWrite_ + 1) % kQueueCapacity;
            ++queueCount_;
        }
        LeaveCriticalSection(&queueLock_);
    }

    void CaptureDead(const uint8_t* packet)
    {
        sdgo_game_test_death::DeadPacket parsed = {};
        if (!sdgo_game_test_death::ParseDeadPacket(
                packet, &parsed)) {
            InterlockedIncrement(&parseFailures_);
            return;
        }
        if (parsed.truncated)
            InterlockedIncrement(&truncatedPackets_);
        const Identity killer =
            CaptureIdentity(parsed.killerPilotId);
        for (unsigned i = 0;
             i < parsed.capturedVictimCount; ++i) {
            Event event = {};
            event.kind = Kind::Ordinary;
            event.killer = killer;
            event.victim =
                CaptureIdentity(parsed.victims[i].pilotId);
            event.nativeVictimCount =
                parsed.nativeVictimCount;
            event.recordIndex = static_cast<uint8_t>(i);
            event.raw4 = parsed.victims[i].raw4;
            event.raw9 = parsed.victims[i].raw9;
            event.raw10 = parsed.victims[i].raw10;
            Queue(event);
            InterlockedIncrement(&ordinaryVictims_);
        }
    }

    void CaptureMapDamage(const uint8_t* packet)
    {
        sdgo_game_test_death::MapDamagePacket parsed = {};
        if (!sdgo_game_test_death::ParseMapDamagePacket(
                packet, &parsed)) {
            InterlockedIncrement(&parseFailures_);
            return;
        }
        Event event = {};
        event.kind = Kind::MapDamage;
        event.victim = CaptureIdentity(parsed.victimPilotId);
        event.raw10 = parsed.raw10;
        event.raw11 = parsed.raw11;
        event.raw12 = parsed.raw12;
        Queue(event);
        InterlockedIncrement(&mapDamagePackets_);
        if (event.victim.registeredAi)
            InterlockedIncrement(
                &registeredAiMapDamagePackets_);
    }

    Event CaptureMovementMapRequest(
        const uint8_t* movement)
    {
        Event event = {};
        event.kind = Kind::MovementMapRequest;
        __try {
            event.senderOnceBefore =
                movement[kMovementMapSent];
            const uint8_t* unit =
                *reinterpret_cast<
                    const uint8_t* const*>(
                    movement + kMoveOwner);
            event.victim =
                CaptureUnitIdentity(unit, 0, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return event;
    }

    Event CaptureEventMapRequest(
        const uint8_t* packet)
    {
        Event event = {};
        event.kind = Kind::EventMapRequest;
        __try {
            if (packet) {
                event.sourceOpcode =
                    *reinterpret_cast<
                        const uint16_t*>(packet);
                const uint32_t pilotId =
                    *reinterpret_cast<
                        const uint32_t*>(
                        packet + 0x06);
                event.victim =
                    CaptureIdentity(pilotId);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&parseFailures_);
        }
        return event;
    }

    unsigned Drain(Event* output, unsigned capacity)
    {
        EnterCriticalSection(&queueLock_);
        unsigned count = 0;
        while (count < capacity && queueCount_ != 0) {
            output[count++] = queue_[queueRead_];
            queueRead_ = (queueRead_ + 1) % kQueueCapacity;
            --queueCount_;
        }
        LeaveCriticalSection(&queueLock_);
        return count;
    }

    static int __fastcall DeadHandler(
        void* self, void*, uint8_t* packet)
    {
        const DWORD incomingError = GetLastError();
        active_->CaptureDead(packet);
        SetLastError(incomingError);
        const int result =
            active_->deadOriginal_(self, packet);
        const DWORD originalError = GetLastError();
        SetLastError(originalError);
        return result;
    }

    static int __fastcall MapDamageHandler(
        void* self, void*, uint8_t* packet)
    {
        const DWORD incomingError = GetLastError();
        active_->CaptureMapDamage(packet);
        SetLastError(incomingError);
        const int result =
            active_->mapDamageOriginal_(self, packet);
        const DWORD originalError = GetLastError();
        SetLastError(originalError);
        return result;
    }

    static void __fastcall MovementMapSender(
        uint8_t* movement, void*, void* control)
    {
        NativeDeathProbe* probe = active_;
        const DWORD incomingError = GetLastError();
        Event event =
            probe->CaptureMovementMapRequest(movement);
        if (event.senderOnceBefore != 0) {
            SetLastError(incomingError);
            probe->movementMapSenderOriginal_(
                movement, control);
            return;
        }
        const NetworkCounters before =
            probe->CaptureNetworkCounters();
        InterlockedIncrement(
            &probe->movementMapRequestCandidates_);
        SetLastError(incomingError);
        probe->movementMapSenderOriginal_(
            movement, control);
        const DWORD originalError = GetLastError();
        __try {
            event.senderOnceAfter =
                movement[kMovementMapSent];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const NetworkCounters after =
            probe->CaptureNetworkCounters();
        ApplyNetworkDelta(event, before, after);
        if (event.sendConfirmed) {
            InterlockedIncrement(
                &probe->movementMapRequests_);
            if (event.victim.registeredAi)
                InterlockedIncrement(
                    &probe->
                        registeredAiMovementMapRequests_);
        }
        probe->Queue(event);
        SetLastError(originalError);
    }

    static int __fastcall EventMapSender(
        void* self, void*, uint8_t* packet)
    {
        NativeDeathProbe* probe = active_;
        const DWORD incomingError = GetLastError();
        Event event =
            probe->CaptureEventMapRequest(packet);
        const NetworkCounters before =
            probe->CaptureNetworkCounters();
        InterlockedIncrement(
            &probe->eventMapRequestCandidates_);
        SetLastError(incomingError);
        const int result =
            probe->eventMapSenderOriginal_(
                self, packet);
        const DWORD originalError = GetLastError();
        const NetworkCounters after =
            probe->CaptureNetworkCounters();
        ApplyNetworkDelta(event, before, after);
        event.after = probe->CaptureIdentity(event.victim.pilotId);
        const Identity& start = event.victim;
        const Identity& end = event.after;
        event.aliveRelocation =
            event.sourceOpcode == 0x07D4 &&
            start.registeredAi && start.aiEnabled && start.localControl &&
            !start.remoteCopy && start.masterNpc &&
            start.pilotId == end.pilotId && start.objectId != 0 &&
            start.objectId == end.objectId && start.hp > 0 && end.hp == start.hp &&
            start.positionValid && end.positionValid &&
            (std::fabs(start.x - end.x) > 1.0f ||
             std::fabs(start.y - end.y) > 1.0f ||
             std::fabs(start.z - end.z) > 1.0f) &&
            event.networkCountersValid && event.networkSendDelta == 0;
        if (event.aliveRelocation)
            InterlockedIncrement(&probe->aliveTeleportRelocations_);
        if (event.sendConfirmed) {
            InterlockedIncrement(
                &probe->eventMapRequests_);
            if (event.victim.registeredAi)
                InterlockedIncrement(
                    &probe->
                        registeredAiEventMapRequests_);
        }
        probe->Queue(event);
        SetLastError(originalError);
        return result;
    }

    void Emit(
        const SdgoGameTestContextV1* context,
        const Event& event) const
    {
        char text[1408] = {};
        const char* kind = "unknown";
        switch (event.kind) {
        case Kind::Ordinary:
            kind = "ordinary_death";
            break;
        case Kind::MapDamage:
            kind = "map_damage";
            break;
        case Kind::MovementMapRequest:
            kind = "map_request_movement";
            break;
        case Kind::EventMapRequest:
            kind = "map_request_event";
            break;
        }
        sprintf_s(
            text,
            "native death cause=%s qpc=%llu thread=%u "
            "killer_pilot=%u killer_object=%u killer_npc=%u killer_team=%d "
            "victim_pilot=%u victim_object=%u victim_npc=%u victim_team=%d "
            "victim_hp=%d/%d victim_pos=%s(%.3f,%.3f,%.3f) "
            "victim_registered_ai=%u victim_ai_enabled=%u "
            "victim_local_control=%u victim_remote_copy=%u "
            "victim_master_npc=%u victim_player_control=%u "
            "victim_battle_snapshot=%u victim_hurt=%u "
            "victim_control_blocked=%u "
            "victim_movement=%X/%X victim_attack=%X/%X "
            "same_pilot=%u same_object=%u victim_record=%u/%u "
            "raw4=%u raw9=%u raw10=%u raw11=%u raw12=%u "
            "trigger_opcode=0x%04X sender_once=%u/%u "
            "network_counters=%u network_send_delta=%u "
            "network_byte_delta=%u send_0492=%u "
            "post_object=%u post_hp=%d post_pos=%s(%.3f,%.3f,%.3f) alive_relocation=%u",
            kind,
            static_cast<unsigned long long>(event.qpc),
            event.threadId,
            event.killer.pilotId,
            event.killer.objectId,
            event.killer.npcId,
            event.killer.team,
            event.victim.pilotId,
            event.victim.objectId,
            event.victim.npcId,
            event.victim.team,
            event.victim.hp,
            event.victim.maxHp,
            event.victim.positionValid ? "" : "invalid",
            event.victim.x,
            event.victim.y,
            event.victim.z,
            static_cast<unsigned>(
                event.victim.registeredAi),
            static_cast<unsigned>(
                event.victim.aiEnabled),
            static_cast<unsigned>(
                event.victim.localControl),
            static_cast<unsigned>(
                event.victim.remoteCopy),
            static_cast<unsigned>(
                event.victim.masterNpc),
            static_cast<unsigned>(
                event.victim.playerControlEnabled),
            static_cast<unsigned>(
                event.victim.battleSnapshotValid),
            event.victim.hurtState,
            event.victim.controlBlocked,
            event.victim.movementState,
            event.victim.movementPending,
            event.victim.attackState,
            event.victim.attackPending,
            event.samePilot,
            event.sameObject,
            static_cast<unsigned>(event.recordIndex),
            static_cast<unsigned>(event.nativeVictimCount),
            static_cast<unsigned>(event.raw4),
            static_cast<unsigned>(event.raw9),
            static_cast<unsigned>(event.raw10),
            static_cast<unsigned>(event.raw11),
            static_cast<unsigned>(event.raw12),
            static_cast<unsigned>(event.sourceOpcode),
            static_cast<unsigned>(
                event.senderOnceBefore),
            static_cast<unsigned>(
                event.senderOnceAfter),
            static_cast<unsigned>(
                event.networkCountersValid),
            event.networkSendDelta,
            event.networkByteDelta,
            static_cast<unsigned>(
                event.sendConfirmed),
            event.after.objectId, event.after.hp,
            event.after.positionValid ? "" : "invalid",
            event.after.x, event.after.y, event.after.z,
            static_cast<unsigned>(event.aliveRelocation));
        context->Observe(text);
    }

public:
    bool Install(const SdgoApi* api, SdgoMod* mod)
    {
        if (!api || !api->SettingInt || !api->Symbol ||
            !api->HookCode)
            return false;
        const int setting =
            api->SettingInt(mod, "BattleAiDeaths", 0);
        if (setting != 0 && setting != 1)
            return false;
        enabled_ = setting != 0;
        const int teleport = api->SettingInt(mod, "BattleAiTeleport", 0);
        if ((teleport != 0 && teleport != 1) || (teleport && !enabled_))
            return false;
        requireTeleport_ = teleport != 0;
        if (!enabled_)
            return true;
        unitManager_ =
            static_cast<void**>(
                api->Symbol(mod, "game.unit_manager"));
        objectIdFromPilot_ =
            reinterpret_cast<ObjectIdFromPilotFn>(
                api->Symbol(mod, "um.object_id_from_pilot"));
        findObject_ =
            reinterpret_cast<FindObjectFn>(
                api->Symbol(mod, "um.find_object"));
        network_ =
            reinterpret_cast<NetworkFn>(
                api->Symbol(mod, "flow.network"));
        if (!unitManager_ || !objectIdFromPilot_ ||
            !findObject_ || !network_)
            return false;
        ResolveObservers();
        InitializeCriticalSection(&queueLock_);
        active_ = this;
        return
            api->HookCode(
                mod, "game.dead_unit_handler",
                reinterpret_cast<void*>(&DeadHandler),
                reinterpret_cast<void**>(&deadOriginal_)) &&
            api->HookCode(
                mod, "game.map_damage_handler",
                reinterpret_cast<void*>(&MapDamageHandler),
                reinterpret_cast<void**>(
                    &mapDamageOriginal_)) &&
            api->HookCode(
                mod, "game.map_death_movement_sender",
                reinterpret_cast<void*>(
                    &MovementMapSender),
                reinterpret_cast<void**>(
                    &movementMapSenderOriginal_)) &&
            api->HookCode(
                mod, "game.map_death_event_sender",
                reinterpret_cast<void*>(&EventMapSender),
                reinterpret_cast<void**>(
                    &eventMapSenderOriginal_));
    }

    bool Observe(const SdgoGameTestContextV1* context)
    {
        if (!enabled_)
            return true;
        Event events[32] = {};
        unsigned count = 0;
        while ((count = Drain(events, _countof(events))) != 0)
            for (unsigned i = 0; i < count; ++i)
                Emit(context, events[i]);
        return true;
    }

    void Finish(const SdgoGameTestContextV1* context)
    {
        if (!enabled_)
            return;
        Observe(context);
        const LONG dropped =
            InterlockedCompareExchange(&dropped_, 0, 0);
        const LONG ordinary =
            InterlockedCompareExchange(
                &ordinaryVictims_, 0, 0);
        const LONG map =
            InterlockedCompareExchange(
                &mapDamagePackets_, 0, 0);
        const LONG parseFailures =
            InterlockedCompareExchange(
                &parseFailures_, 0, 0);
        const LONG truncatedPackets =
            InterlockedCompareExchange(
                &truncatedPackets_, 0, 0);
        const LONG samePilot =
            InterlockedCompareExchange(&samePilot_, 0, 0);
        const LONG sameObject =
            InterlockedCompareExchange(&sameObject_, 0, 0);
        const LONG registeredAiMap =
            InterlockedCompareExchange(
                &registeredAiMapDamagePackets_, 0, 0);
        const LONG movementCandidates =
            InterlockedCompareExchange(
                &movementMapRequestCandidates_, 0, 0);
        const LONG movementRequests =
            InterlockedCompareExchange(
                &movementMapRequests_, 0, 0);
        const LONG registeredAiMovementRequests =
            InterlockedCompareExchange(
                &registeredAiMovementMapRequests_, 0, 0);
        const LONG eventCandidates =
            InterlockedCompareExchange(
                &eventMapRequestCandidates_, 0, 0);
        const LONG eventRequests =
            InterlockedCompareExchange(
                &eventMapRequests_, 0, 0);
        const LONG registeredAiEventRequests =
            InterlockedCompareExchange(
                &registeredAiEventMapRequests_, 0, 0);
        char text[512] = {};
        sprintf_s(
            text,
            "native death summary ordinary_victims=%ld "
            "map_damage_packets=%ld registered_ai_map_damage=%ld "
            "movement_0492_candidates=%ld movement_0492_sent=%ld "
            "registered_ai_movement_0492=%ld "
            "event_0492_candidates=%ld event_0492_sent=%ld "
            "registered_ai_event_0492=%ld parse_failures=%ld "
            "truncated_packets=%ld same_pilot=%ld same_object=%ld "
            "dropped=%ld",
            ordinary, map, registeredAiMap,
            movementCandidates, movementRequests,
            registeredAiMovementRequests,
            eventCandidates, eventRequests,
            registeredAiEventRequests,
            parseFailures, truncatedPackets,
            samePilot, sameObject, dropped);
        context->Observe(text);
        char teleportText[128] = {};
        const LONG relocations = InterlockedCompareExchange(&aliveTeleportRelocations_, 0, 0);
        sprintf_s(teleportText, "native teleport summary alive_relocations=%ld required=%u",
                  relocations, requireTeleport_ ? 1u : 0u);
        context->Observe(teleportText);
        if (requireTeleport_) {
            context->Assert(relocations > 0,
                "registered local AI teleported to a different position with HP preserved and no network send");
            context->Assert(registeredAiEventRequests == 0,
                "AI teleport notifications never sent the native 0492 destruction request");
        }
        context->Assert(
            dropped == 0,
            "native death observation retained every captured packet");
        context->Assert(
            parseFailures == 0,
            "native death observation parsed every delivered packet");
        context->Assert(
            truncatedPackets == 0,
            "native death observation retained every victim record");
        context->Assert(
            samePilot == 0 && sameObject == 0,
            "no ordinary native death identified the same pilot or object "
            "as both killer and victim");
    }
};
