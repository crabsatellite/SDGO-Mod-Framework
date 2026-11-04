#pragma once

#include <cstddef>
#include <cstdint>

#define SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_VERSION 1
#define SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_EXPORT \
    "SdgoPvpAiClientGetForcedPadStatusV1"

#pragma pack(push, 8)

struct SdgoPvpAiClientForcedPadBotStatusV1 {
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

struct SdgoPvpAiClientForcedPadStatusV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t battle_start_sequence;
    std::uint32_t bot_count;
    std::uint32_t bridge_available;
    std::uint32_t reserved;
    SdgoPvpAiClientForcedPadBotStatusV1 bots[12];
};

#pragma pack(pop)

static_assert(sizeof(SdgoPvpAiClientForcedPadBotStatusV1) == 68,
              "unexpected forced-pad bot witness layout");
static_assert(offsetof(SdgoPvpAiClientForcedPadBotStatusV1, outstanding) == 36,
              "unexpected forced-pad outstanding offset");
static_assert(offsetof(SdgoPvpAiClientForcedPadBotStatusV1,
                       activation_position_x) == 56,
              "unexpected forced-pad position offset");
static_assert(sizeof(SdgoPvpAiClientForcedPadStatusV1) == 840,
              "unexpected forced-pad witness layout");

using SdgoPvpAiClientGetForcedPadStatusV1Fn =
    int(__cdecl*)(SdgoPvpAiClientForcedPadStatusV1* status);
