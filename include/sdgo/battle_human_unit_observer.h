#pragma once
#include <stddef.h>
#include <stdint.h>

#define SDGO_BATTLE_HUMAN_UNIT_OBSERVER_VERSION 1u
#define SDGO_BATTLE_HUMAN_UNIT_OBSERVER_EXPORT "SdgoBattleRegisterHumanUnitObserverV1"
#define SDGO_BATTLE_HUMAN_UNIT_STATUS_EXPORT "SdgoBattleReadHumanUnitObserverStatusV1"

enum SdgoBattleHumanUnitEventTypeV1 {
    SDGO_BATTLE_HUMAN_UNIT_BEFORE = 1,
    SDGO_BATTLE_HUMAN_UNIT_AFTER = 2,
    SDGO_BATTLE_HUMAN_UNIT_ROOM_RELEASED = 3,
    SDGO_BATTLE_HUMAN_UNIT_ROOM_INVALIDATED = 4, // Fresh start boundary or abandoned/rolled-back generation.
    SDGO_BATTLE_HUMAN_UNIT_START_ACCEPTED = 5, // Native start returned success and owner-side synchronization completed.
};

#pragma pack(push, 4)
typedef struct SdgoBattleHumanUnitEventV1 {
    uint32_t size, version, type;
    int32_t native_result; // Valid only for AFTER, including a native zero result.
    uint64_t sequence; // The same invocation token for BEFORE and AFTER.
    uint32_t thread_id;
    const void* room;
    const void* source; // Final source after any existing owner-side rewrite.
    const void* player;
    uint8_t rule, mode;
    uint16_t reserved;
} SdgoBattleHumanUnitEventV1;

// Synchronous read-only observation on the originating native server thread.
// Calls can run concurrently/interleave. BEFORE source is final/readable;
// player storage permits bounded raw preimage capture, not initialized-object
// use or dereferencing its nested pointers. AFTER player data is usable only on
// successful native_result. RELEASED/INVALIDATED carry room identity only and
// invalidate every pending/cached generation for that room.
// The first build of each start attempt is preceded by ROOM_INVALIDATED. A
// successful start then emits START_ACCEPTED (room identity only), which is a
// business-return boundary, not proof of network delivery or active gameplay.
// Borrowed pointers expire on return. Copy bounded data before returning; do
// not mutate the provider/native objects, reenter the provider or block here.
typedef void (__cdecl* SdgoBattleHumanUnitObserverCallbackV1)(const SdgoBattleHumanUnitEventV1*, void* user);
typedef struct SdgoBattleHumanUnitObserverSinkV1 {
    uint32_t size, version;
    SdgoBattleHumanUnitObserverCallbackV1 callback;
    void* user;
} SdgoBattleHumanUnitObserverSinkV1;
typedef struct SdgoBattleHumanUnitObserverStatusV1 {
    uint32_t size, version;
    uint32_t registration_state; // 0 empty, 1 registering, 2 registered, 3 fault-disabled.
    uint32_t reserved;
    uint64_t last_sequence; // Allocation watermark only, never delivery/completion proof.
} SdgoBattleHumanUnitObserverStatusV1;
#pragma pack(pop)

// One process-lifetime sink, registered as the consumer's final load action.
// A loaded provider with a missing/incompatible export must never be bypassed
// by installing another hook at its native address.
typedef int (__cdecl* SdgoBattleRegisterHumanUnitObserverV1Fn)(const SdgoBattleHumanUnitObserverSinkV1*);
// Cached actor data is unusable unless this remains registered (state 2).
// Consumers must fail closed when a callback fault disabled observations; a
// missing release/build event must never make stale pointer generations usable.
typedef int (__cdecl* SdgoBattleReadHumanUnitObserverStatusV1Fn)(SdgoBattleHumanUnitObserverStatusV1*);

#ifdef __cplusplus
static_assert(sizeof(void*) == 4, "human unit observer requires the supported x86 server");
static_assert(sizeof(SdgoBattleHumanUnitEventV1) == 44, "human unit observer event ABI changed");
static_assert(offsetof(SdgoBattleHumanUnitEventV1, sequence) == 16, "human unit observer sequence ABI changed");
static_assert(offsetof(SdgoBattleHumanUnitEventV1, room) == 28, "human unit observer pointers ABI changed");
static_assert(sizeof(SdgoBattleHumanUnitObserverSinkV1) == 16, "human unit observer sink ABI changed");
static_assert(sizeof(SdgoBattleHumanUnitObserverStatusV1) == 24, "human unit observer status ABI changed");
#endif
