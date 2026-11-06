#pragma once
#include <stdint.h>

// Read-only diagnostics for the selected process-local task profile. No control
// or test-injection operation is exposed by this ABI.
typedef struct SdgoMissionBehaviorServerSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t profile;  // 1 OC, 2 current GCO
    uint32_t state;    // 0 awaiting tables, 1 OC active, 2 GCO ready, 3 rejected, 4 rollback failure
    uint32_t entries;
    uint32_t applied;
    uint32_t baseline_matches;
    uint32_t payload_crc32;
    uint32_t native_rows_crc32;
    uint32_t ready_epochs;
    uint32_t conflict_id;
    uint32_t reserved;
} SdgoMissionBehaviorServerSnapshotV1;

#define SDGO_MISSION_BEHAVIOR_SERVER_SNAPSHOT_EXPORT \
    "SdgoMissionBehaviorServerObserveV1"
typedef int(__cdecl* SdgoMissionBehaviorServerObserveV1Fn)(
    SdgoMissionBehaviorServerSnapshotV1* output,
    uint32_t bytes);

typedef struct SdgoMissionBehaviorServerRowV1 {
    uint32_t id;
    uint32_t mission;
    uint32_t difficulty;
    uint32_t part;
    uint32_t npc;
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t after_time;
    uint32_t abs_time;
    uint32_t manual;
    uint32_t party;
    uint32_t create_limit;
    uint32_t zone;
} SdgoMissionBehaviorServerRowV1;

#define SDGO_MISSION_BEHAVIOR_SERVER_READ_ROW_EXPORT \
    "SdgoMissionBehaviorServerReadRowV1"
typedef int(__cdecl* SdgoMissionBehaviorServerReadRowV1Fn)(
    uint32_t id,
    SdgoMissionBehaviorServerRowV1* output,
    uint32_t bytes);

// Passive receipt published only after the original BattleServer mission-NPC
// creation function returns success. The row is copied before the original
// call, so no native table pointer escapes through this ABI.
typedef struct SdgoMissionBehaviorServerCreationV1 {
    uint32_t size;
    uint32_t version;
    uint64_t sequence;
    uint32_t profile;          // 1 OC, 2 current GCO
    uint32_t original_result;  // original SetMissionCreateNpc return value
    uint32_t tracked;          // row exists in the owner profile
    uint32_t profile_match;    // consumed row matches the selected profile
    uint32_t mission_npc_id;
    uint32_t mission_id;
    uint32_t difficulty;
    uint32_t part;
    uint32_t npc_id;
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t after_time;
    uint32_t abs_time;
    uint32_t manual_id;
    uint32_t party;
    uint32_t create_limit;
    uint32_t zone;
    uint32_t native_pilot_id;
    uint32_t native_unit_hp;
} SdgoMissionBehaviorServerCreationV1;

#define SDGO_MISSION_BEHAVIOR_SERVER_CREATION_EXPORT \
    "SdgoMissionBehaviorServerGetCreationV1"
typedef int(__cdecl* SdgoMissionBehaviorServerGetCreationV1Fn)(
    SdgoMissionBehaviorServerCreationV1* output,
    uint32_t bytes);

#ifdef __cplusplus
static_assert(
    sizeof(SdgoMissionBehaviorServerSnapshotV1) == 48,
    "unexpected mission behavior server snapshot ABI");
static_assert(
    sizeof(SdgoMissionBehaviorServerRowV1) == 56,
    "unexpected mission behavior server row ABI");
static_assert(
    sizeof(SdgoMissionBehaviorServerCreationV1) == 96,
    "unexpected mission behavior server creation observer ABI");
#endif
