// GameTest observer for the OC client Mod. Exported by oc_custom_client.dll.
#pragma once

#include <cstdint>

#define SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION 2u

// Window bits of SdgoOcCustomClientSnapshotV2::windows.
#define SDGO_OC_WINDOW_ENTRY 0x01u      // OVER CUSTOM entry shown on the upgrade page
#define SDGO_OC_WINDOW_ENTRY_LIT 0x02u  // the entry is the glowing (usable) button
#define SDGO_OC_WINDOW_OVER 0x04u       // uirs\0794.uir allocation window
#define SDGO_OC_WINDOW_CHALLENGE 0x08u  // uirs\0813.uir challenge window
#define SDGO_OC_WINDOW_CONFIRM 0x10u    // uirs\0819.uir confirmation popup
#define SDGO_OC_WINDOW_TAB 0x20u        // 终极强化 tab restored in the factory tab bar
#define SDGO_OC_WINDOW_TAB_ACTIVE 0x40u // the upgrade page shows the OC tab content

struct SdgoOcCustomClientSnapshotV2 {
    uint32_t size;
    uint32_t version;
    uint32_t windows_loaded;   // the original OC layouts are loaded and bound on the page
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
    int32_t base[6];
    int32_t normal_gain[6];
    int32_t over_gain[6];
    uint32_t carrier_present;  // the active record carries an OC payload
    int32_t carrier_delta[6];
    uint32_t last_upgrade_success;
    uint32_t last_reset_gp;
    uint32_t windows;          // SDGO_OC_WINDOW_* bits currently shown
    uint32_t confirm_text_id;  // client text id shown in the confirmation popup (0 none)
    uint32_t challenge_items;  // OC items listed in the challenge window
    int32_t challenge_item_ids[10];
    uint32_t challenge_selected; // bit per listed item that is checked
    uint32_t hidden_dialogs;     // native dialogs (factory inventory) hidden under open OC windows
    // Battle: the local unit's mobility after the OC gains (stat setup hook).
    uint32_t mobility_applied;   // number of local stat setups that received gains
    uint32_t mobility_unit_number;
    int32_t mobility_dash_base;    // DashLevel of the unit data
    int32_t mobility_dash_before;  // unit +0xB64 as the native setup left it (with its own bonus)
    int32_t mobility_dash;         // unit +0xB64 after the OC gain
    int32_t mobility_motive_base;  // MotivePower of the unit data
    int32_t mobility_motive_before;
    int32_t mobility_motive;       // unit +0xB6A after the OC gain
    int32_t mobility_speed_gain;   // carried gains that were added
    int32_t mobility_agility_gain;
};

// Actions: 1 refresh; 2 save one more normal point on stat (direct request
// without the windows, GameTest preparation); 9 select unit by uid
// (argument64, without the page).
struct SdgoOcCustomClientActionV2 {
    uint32_t size;
    uint32_t version;
    uint32_t action;
    uint32_t stat;
    uint64_t argument64;
};

// SdgoOcCustomClientClickV2 delivers a real UI event for an original control
// to the page service's OnUIEvent (button 0x101, check box 0x401), exactly as
// a mouse click does. window: "tab", "entry", "over", "challenge" or "confirm";
// control: the original control name (for example L"SpeedUpBtn").
using SdgoOcCustomClientGetSnapshotV2Fn = int(__cdecl*)(SdgoOcCustomClientSnapshotV2*);
using SdgoOcCustomClientActionV2Fn = int(__cdecl*)(const SdgoOcCustomClientActionV2*);
using SdgoOcCustomClientClickV2Fn = int(__cdecl*)(const char* window, const wchar_t* control);
