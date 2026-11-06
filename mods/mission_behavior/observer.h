#pragma once

#include <cstdint>

#define SDGO_MISSION_BEHAVIOR_CLIENT_STATUS_EXPORT \
    "SdgoMissionBehaviorClientGetStatusV1"

enum SdgoMissionBehaviorClientProfileV1 : std::uint32_t {
    SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_UNAVAILABLE = 0,
    SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_OC = 1,
    SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_GCO = 2,
};

struct SdgoMissionBehaviorClientStatusV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t requested_profile;
    std::uint32_t profile_data_valid;
    std::uint32_t profile_data_flags;
    std::uint32_t hook_installed;
    std::uint32_t table_generation;
    std::uint32_t observed_records;
    std::uint32_t applied_records;
    std::uint32_t restored_records;
    std::uint32_t unchanged_records;
    std::uint32_t conflict_records;
    std::uint32_t active_profile;
    std::uint32_t last_npc_id;
};

static_assert(
    sizeof(SdgoMissionBehaviorClientStatusV1) == 56,
    "unexpected mission behavior client status ABI");

using SdgoMissionBehaviorClientGetStatusV1Fn =
    int(__cdecl*)(SdgoMissionBehaviorClientStatusV1* output);
