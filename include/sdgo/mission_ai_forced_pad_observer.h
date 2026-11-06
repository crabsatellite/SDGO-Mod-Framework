// Read-only witness for authored ForceMove volumes used by co-op mission
// wingmen. This is separate from the PvP match_ai witness because the two
// participant lifecycles and mode ownership checks are independent.
#pragma once

#include <cstddef>
#include <cstdint>

#define SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_VERSION 1
#define SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_EXPORT \
    "SdgoMissionAiClientGetForcedPadStatusV1"
#define SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION 2
#define SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_V2_EXPORT \
    "SdgoMissionAiClientGetForcedPadStatusV2"

#pragma pack(push, 8)

struct SdgoMissionAiClientForcedPadBotStatusV1 {
    std::uint32_t pilot_id;
    std::uint32_t object_id;
    std::uint32_t generation;
    std::uint32_t request_count;
    std::uint32_t notice_count;
    std::uint32_t displacement_count;
    std::uint32_t timeout_count;
    std::uint32_t send_failure_count;
    std::uint32_t ownership_loss_count;
    std::uint32_t outstanding;
    std::uint32_t outstanding_generation;
    std::uint32_t last_result;
    std::uint32_t last_clear_reason;
    std::uint32_t activation_position_valid;
    float activation_position_x;
    float activation_position_y;
    float activation_position_z;
};

struct SdgoMissionAiClientForcedPadStatusV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t mode_object;
    std::uint32_t bot_count;
    std::uint32_t bridge_available;
    std::uint32_t tick_count;
    SdgoMissionAiClientForcedPadBotStatusV1 bots[12];
};

// Passive request witness for exact authored-pad correlation.  The latest
// successfully submitted payload remains available after ACK/clear; consumers
// pair request_count with notice_count and exact bot generation.  pad_object_id
// is best effort and must be ignored unless pad_object_valid is set.
struct SdgoMissionAiClientForcedPadBotStatusV2 {
    std::uint32_t pilot_id;
    std::uint32_t object_id;
    std::uint32_t generation;
    std::uint32_t request_count;
    std::uint32_t notice_count;
    std::uint32_t outstanding;
    std::uint32_t outstanding_generation;
    std::uint32_t last_result;
    std::uint32_t last_clear_reason;
    std::uint32_t request_tick;
    std::uint32_t payload_valid;
    std::uint32_t pad_object_valid;
    std::uint32_t pad_object_id;
    std::uint32_t request_pilot_id;
    float speed;
    float duration_updates;
    float direction_x;
    float direction_y;
    float direction_z;
};

struct SdgoMissionAiClientForcedPadStatusV2 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t mode_object;
    std::uint32_t bot_count;
    std::uint32_t bridge_available;
    std::uint32_t tick_count;
    SdgoMissionAiClientForcedPadBotStatusV2 bots[12];
};

#pragma pack(pop)

static_assert(sizeof(SdgoMissionAiClientForcedPadBotStatusV1) == 68,
              "unexpected mission forced-pad bot witness layout");
static_assert(
    offsetof(SdgoMissionAiClientForcedPadBotStatusV1, outstanding) == 36,
    "unexpected mission forced-pad outstanding offset");
static_assert(
    offsetof(SdgoMissionAiClientForcedPadBotStatusV1,
             activation_position_x) == 56,
    "unexpected mission forced-pad position offset");
static_assert(sizeof(SdgoMissionAiClientForcedPadStatusV1) == 840,
              "unexpected mission forced-pad witness layout");
static_assert(sizeof(SdgoMissionAiClientForcedPadBotStatusV2) == 76,
              "unexpected mission forced-pad V2 bot witness layout");
static_assert(
    offsetof(
        SdgoMissionAiClientForcedPadBotStatusV2,
        speed) == 56,
    "unexpected mission forced-pad V2 payload offset");
static_assert(sizeof(SdgoMissionAiClientForcedPadStatusV2) == 936,
              "unexpected mission forced-pad V2 witness layout");

using SdgoMissionAiClientGetForcedPadStatusV1Fn =
    int(__cdecl*)(SdgoMissionAiClientForcedPadStatusV1* status);
using SdgoMissionAiClientGetForcedPadStatusV2Fn =
    int(__cdecl*)(SdgoMissionAiClientForcedPadStatusV2* status);
