// GameTest observer for the OC client panel. Exported by oc_custom_client.dll.
#pragma once

#include <cstdint>

#define SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION 1u

struct SdgoOcCustomClientSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t panel_attached;   // the factory upgrade page holds the panel
    uint32_t transport_ready;  // gift carrier usable for OC requests
    uint32_t server_oc_mode;   // mode reply said oc
    uint32_t profile_present;
    uint32_t busy;
    uint32_t request_count;    // requests sent since load
    uint32_t reply_count;      // replies received since load
    int32_t last_status;       // WireStatus of the last reply or failure
    int32_t last_ret_val;      // procedure result of the last reply
    uint32_t last_command;
    uint64_t unit_uid;
    uint32_t unit_number;
    uint32_t oc_level;
    uint32_t native_level;
    uint32_t revision;
    uint32_t normal_points_left;
    uint32_t over_points_left;
    uint8_t normal[6];
    uint8_t over[6];
    uint8_t pending_normal[6];
    uint8_t pending_over[6];
    int32_t base[6];
    int32_t normal_gain[6];
    int32_t over_gain[6];
    uint32_t over_mode;
    uint32_t carrier_present;  // the page record carries an OC payload
    int32_t carrier_delta[6];
    uint32_t last_upgrade_success;
    uint32_t last_reset_gp;
};

// Drives the same handlers the panel buttons use. action values:
//   1 refresh, 2 plus(stat), 3 minus(stat), 4 apply, 5 reset cost, 6 reset,
//   7 challenge, 8 toggle over mode, 9 select unit by uid (argument64).
struct SdgoOcCustomClientActionV1 {
    uint32_t size;
    uint32_t version;
    uint32_t action;
    uint32_t stat;
    uint64_t argument64;
};

using SdgoOcCustomClientGetSnapshotV1Fn = int(__cdecl*)(SdgoOcCustomClientSnapshotV1*);
using SdgoOcCustomClientActionV1Fn = int(__cdecl*)(const SdgoOcCustomClientActionV1*);
