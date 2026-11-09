// Observer for the OC Battle Mod's room-system decisions. Exported by
// oc_custom_battle.dll for tests and tools.
#pragma once

#include <cstdint>

#define SDGO_OC_CUSTOM_BATTLE_DECISIONS_EXPORT "SdgoOcCustomBattleRecentDecisionsV1"
#define SDGO_OC_CUSTOM_BATTLE_DECISION_TITLE_BYTES 64u

// One stat build that a room decided in "room" mode (carrier_room_rule).
struct SdgoOcCustomBattleDecisionV1 {
    uint32_t size;               // sizeof(SdgoOcCustomBattleDecisionV1)
    uint32_t sequence;           // 1, 2, ... in build order
    uint32_t room_key;           // the native room's key (room +0)
    uint8_t system;              // 0 G改 2.0 (current custom skills), 1 OC, 2 G改 1.0
    uint8_t form;                // 0..3
    uint8_t oc_applied;          // the OC deltas were added to this build
    uint8_t g1_profile;          // the record carried a G改 1.0 loadout
    uint64_t uid;                // owned unit UID of the record
    uint16_t native_skills[4];   // custom skills the native builder copied from the record
    uint16_t battle_skills[4];   // custom skills the battle unit plays after the room's decision
    char title[SDGO_OC_CUSTOM_BATTLE_DECISION_TITLE_BYTES]; // room title (UTF-8, truncated, NUL-terminated)
};

// Copies up to capacity of the most recent decisions (oldest first) and
// returns how many were copied. The Mod keeps the last 32.
typedef uint32_t(__cdecl* SdgoOcCustomBattleRecentDecisionsV1Fn)(SdgoOcCustomBattleDecisionV1* out, uint32_t capacity);
