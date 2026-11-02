#pragma once

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sdgo/game_test.h"
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
}

// Opt-in passive observation of the original client death handlers. The hooks
// copy packet identity and current unit state, then call the native handlers
// with the original arguments. They never write HP, position, input or packets.
class NativeDeathProbe
{
    enum class Kind : uint8_t
    {
        Ordinary,
        MapDamage
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
    };

    struct Event
    {
        uint64_t qpc = 0;
        uint32_t threadId = 0;
        Kind kind = Kind::Ordinary;
        Identity killer = {};
        Identity victim = {};
        uint8_t nativeVictimCount = 0;
        uint8_t recordIndex = 0;
        uint8_t raw4 = 0;
        uint8_t raw9 = 0;
        uint8_t raw10 = 0;
        uint8_t raw11 = 0;
        uint8_t raw12 = 0;
        uint8_t samePilot = 0;
        uint8_t sameObject = 0;
    };

    using HandlerFn = int(__thiscall*)(void*, uint8_t*);
    using ObjectIdFromPilotFn = int(__thiscall*)(void*, uint32_t);
    using FindObjectFn = void*(__thiscall*)(void*, int);

    static constexpr unsigned kQueueCapacity = 256;
    static constexpr size_t kUnitNode = 0x08;
    static constexpr size_t kUnitTeam = 0x20;
    static constexpr size_t kUnitHp = 0x38;
    static constexpr size_t kUnitMaxHp = 0x3C;
    static constexpr size_t kNodePosition = 0x88;
    static constexpr size_t kUnitNpcDataId = 0x3238;

    inline static NativeDeathProbe* active_ = nullptr;
    void** unitManager_ = nullptr;
    ObjectIdFromPilotFn objectIdFromPilot_ = nullptr;
    FindObjectFn findObject_ = nullptr;
    HandlerFn deadOriginal_ = nullptr;
    HandlerFn mapDamageOriginal_ = nullptr;
    CRITICAL_SECTION queueLock_ = {};
    Event queue_[kQueueCapacity] = {};
    unsigned queueRead_ = 0;
    unsigned queueWrite_ = 0;
    unsigned queueCount_ = 0;
    bool enabled_ = false;
    volatile LONG dropped_ = 0;
    volatile LONG ordinaryVictims_ = 0;
    volatile LONG mapDamagePackets_ = 0;
    volatile LONG parseFailures_ = 0;
    volatile LONG truncatedPackets_ = 0;
    volatile LONG samePilot_ = 0;
    volatile LONG sameObject_ = 0;

    Identity CaptureIdentity(uint32_t pilotId) const
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
            identity.objectId = static_cast<uint32_t>(objectId);
            const uint8_t* unit = static_cast<const uint8_t*>(
                findObject_(manager, objectId));
            if (!unit)
                return identity;
            identity.npcId =
                *reinterpret_cast<const uint32_t*>(
                    unit + kUnitNpcDataId);
            identity.hp =
                *reinterpret_cast<const int32_t*>(unit + kUnitHp);
            identity.maxHp =
                *reinterpret_cast<const int32_t*>(unit + kUnitMaxHp);
            identity.team =
                *reinterpret_cast<const int32_t*>(unit + kUnitTeam);
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
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return identity;
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

    void Emit(
        const SdgoGameTestContextV1* context,
        const Event& event) const
    {
        char text[640] = {};
        const char* kind =
            event.kind == Kind::Ordinary
                ? "ordinary_death"
                : "map_damage";
        sprintf_s(
            text,
            "native death cause=%s qpc=%llu thread=%u "
            "killer_pilot=%u killer_object=%u killer_npc=%u killer_team=%d "
            "victim_pilot=%u victim_object=%u victim_npc=%u victim_team=%d "
            "victim_hp=%d/%d victim_pos=%s(%.3f,%.3f,%.3f) "
            "same_pilot=%u same_object=%u victim_record=%u/%u "
            "raw4=%u raw9=%u raw10=%u raw11=%u raw12=%u",
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
            event.samePilot,
            event.sameObject,
            static_cast<unsigned>(event.recordIndex),
            static_cast<unsigned>(event.nativeVictimCount),
            static_cast<unsigned>(event.raw4),
            static_cast<unsigned>(event.raw9),
            static_cast<unsigned>(event.raw10),
            static_cast<unsigned>(event.raw11),
            static_cast<unsigned>(event.raw12));
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
        if (!unitManager_ || !objectIdFromPilot_ || !findObject_)
            return false;
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
                reinterpret_cast<void**>(&mapDamageOriginal_));
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
        char text[288] = {};
        sprintf_s(
            text,
            "native death summary ordinary_victims=%ld "
            "map_damage_packets=%ld parse_failures=%ld "
            "truncated_packets=%ld same_pilot=%ld same_object=%ld "
            "dropped=%ld",
            ordinary, map, parseFailures, truncatedPackets,
            samePilot, sameObject, dropped);
        context->Observe(text);
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
