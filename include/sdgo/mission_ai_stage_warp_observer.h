// Read-only witness for mission rule-script PlayerWarp transitions.
//
// mission_ai publishes this snapshot only after the original PlayerWarp and
// the existing local-wingman mirror have both returned. The witness contains
// actual native unit poses read from the human slot and the same living local
// wingmen assigned to that player. It never accepts a destination vector from
// Lua and never writes a unit position.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SDGO_MISSION_AI_STAGE_WARP_OBSERVER_VERSION 1u
#define SDGO_MISSION_AI_STAGE_WARP_SNAPSHOT_EXPORT \
    "SdgoMissionAiClientGetStageWarpSnapshotV1"

#pragma pack(push, 8)

struct SdgoMissionAiStageWarpWingmanV1 {
    uint32_t pilot_id;
    uint32_t object_id;
    int32_t slot;
    float position_x;
    float position_y;
    float position_z;
    float distance_to_human;
    uint8_t alive;
    uint8_t local_control;
    uint8_t assigned_to_player;
    uint8_t position_valid;
    uint8_t matches_human_pose;
    uint8_t reserved[3];
};

struct SdgoMissionAiStageWarpSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t battle_generation;
    uint32_t mode_object;
    uint32_t room_mode;
    uint32_t warp_count;
    uint32_t last_player_index; // mission Lua index, one based
    uint32_t last_player_slot;  // native unit-manager slot, zero based
    uint32_t last_warp_tick;
    uint32_t human_object_id;
    uint32_t assigned_alive_local_count;
    uint32_t pose_valid_count;
    uint32_t mirrored_count;
    float human_position_x;
    float human_position_y;
    float human_position_z;
    float maximum_pose_distance;
    uint8_t active;
    uint8_t exact_plain_mission;
    uint8_t room_records_valid;
    uint8_t human_position_valid;
    uint8_t all_assigned_mirrored;
    uint8_t reserved[3];
    SdgoMissionAiStageWarpWingmanV1 wingmen[12];
};

#pragma pack(pop)

static_assert(sizeof(void*) == 4,
              "the mission stage-warp observer ABI is Win32-only");
static_assert(sizeof(SdgoMissionAiStageWarpWingmanV1) == 36,
              "unexpected mission stage-warp wingman layout");
static_assert(
    offsetof(SdgoMissionAiStageWarpWingmanV1, alive) == 28,
    "unexpected mission stage-warp wingman flag offset");
static_assert(sizeof(SdgoMissionAiStageWarpSnapshotV1) == 508,
              "unexpected mission stage-warp snapshot layout");
static_assert(
    offsetof(SdgoMissionAiStageWarpSnapshotV1, wingmen) == 76,
    "unexpected mission stage-warp array offset");

typedef int(__cdecl* SdgoMissionAiClientGetStageWarpSnapshotV1Fn)(
    SdgoMissionAiStageWarpSnapshotV1* snapshot);
