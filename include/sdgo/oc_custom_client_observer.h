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
#define SDGO_OC_WINDOW_INFO 0x80u       // uirs\0800.uir OC tab page (level track and ITEM box)

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
    // The OC tab page (0800) and the six-dimension chart of 0794.
    uint32_t page_hidden;          // the upgrade page dialog is hidden under 0800
    uint32_t info_levels;          // OVER CUSTOM level icons lit on 0800
    uint32_t owned_items;          // OC items the account holds (ITEM box source)
    int32_t owned_item_ids[3];     // the ITEM box slots
    uint32_t radar_frames;         // charts drawn on 0794
    float radar_base[6];           // native stat / cap, protocol stat order
    float radar_total[6];          // with the normal and Over points
    uint32_t record_slots_known;   // the active unit record was read
    uint16_t record_custom[4];     // its four custom part slots (UnitSkill) as the client holds it
    uint32_t upgrade_tab_selected; // the factory tab menu has the 经验值转换 slot selected
    uint32_t oc_title;             // the factory header shows 终极强化 for the OC tab
    uint32_t actions_hidden;       // factory page actions (SaveBtn, ReturnBtn) hidden on the OC tab
    uint32_t tab_clicks_accepted;  // native tab clicks the menu accepted (it drops some while busy)
    uint32_t item_models_shown;    // ITEM box slots showing their item's 3D model
    uint32_t item_models_loaded;   // item models loaded into the ITEM box so far
    // Room systems: the server's "room" mode lets each room choose OC or the
    // current custom skills (room title command !oc).
    uint32_t server_room_rule;     // mode reply: each room decides the system
    uint32_t room_known;           // the current room's system has been read
    uint32_t room_system;          // 0 current custom skills, 1 OC
    uint32_t mobility_room_skips;  // local stat setups left native because the room plays the current system
    uint32_t room_list_marks;      // room list rows marked with their room's system
    uint32_t title_marks;          // waiting-room system labels set
    int32_t title_mark_system;     // system of the label last shown: 0 current custom skills, 1 OC, -1 none
    // Whether the local battle unit carries the record's own custom skills:
    // the client unit (first 16 KB) is searched for the record's four
    // UnitSkill values at every local stat setup.
    uint32_t battle_skills_seen;    // local stat setups searched
    int32_t battle_skills_offset;   // where the record's skills were found, -1 absent
    uint16_t battle_skills[4];      // the record's custom skills that were searched for
    // The 体系 row of the PvP create window (room systems).
    uint32_t create_open;           // a PvP create window is open and ready
    uint32_t create_shown;          // create windows that showed the row
    uint32_t create_toggles;        // system changes made with the row's arrows
    int32_t create_system;          // system of the open window's room name: 0 current, 1 OC, -1 none
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
// a mouse click does. window: "tab", "entry", "over", "challenge", "confirm"
// or "create" (the open PvP create window: its 体系 row arrows and native buttons);
// control: the original control name (for example L"SpeedUpBtn").
using SdgoOcCustomClientGetSnapshotV2Fn = int(__cdecl*)(SdgoOcCustomClientSnapshotV2*);
using SdgoOcCustomClientActionV2Fn = int(__cdecl*)(const SdgoOcCustomClientActionV2*);
using SdgoOcCustomClientClickV2Fn = int(__cdecl*)(const char* window, const wchar_t* control);
