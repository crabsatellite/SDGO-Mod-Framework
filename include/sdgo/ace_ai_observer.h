// Read-only Ace AI receipts for isolated native acceptance.
//
// These ABIs report state already produced by the authoritative server or the
// normal client. They never select an Ace, create a unit, append a feed line,
// change HP, or drive a state transition. Client exports are called on the
// GOnline main thread after the native update. Server exports accept the
// process-local room cookie already emitted by the PvP GameTest observer.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define SDGO_ACE_AI_OBSERVER_VERSION 1u
#define SDGO_ACE_AI_MATCH_CLIENT_EXPORT "SdgoMatchAiGetAceSnapshotV1"
#define SDGO_ACE_AI_MISSION_CLIENT_EXPORT "SdgoMissionAiGetAceSnapshotV1"
#define SDGO_ACE_AI_PVP_SERVER_EXPORT "SdgoPvpAiGetAceSnapshotV1"
#define SDGO_ACE_AI_MISSION_SERVER_EXPORT "SdgoCoopAiGetAceSnapshotV1"
#define SDGO_ACE_AI_PVP_SERVER_OBSERVER_EXPORT "SdgoPvpAiRegisterAceObserverV1"
#define SDGO_ACE_AI_MISSION_SERVER_OBSERVER_EXPORT "SdgoCoopAiRegisterAceObserverV1"
#define SDGO_ACE_AI_PVP_ANNOUNCEMENT_EXPORT "SdgoAceAiGetAnnouncementSnapshotV1"
#define SDGO_ACE_AI_MISSION_ANNOUNCEMENT_EXPORT "SdgoMissionAiGetAnnouncementSnapshotV1"
#define SDGO_ACE_AI_MATCH_PLAYBACK_CONTEXT_EXPORT "SdgoMatchAiGetMissionPlaybackContextV1"

enum SdgoAceAiAnnouncementSourceV1 : uint32_t {
    SDGO_ACE_AI_ANNOUNCEMENT_PVP = 1,
    SDGO_ACE_AI_ANNOUNCEMENT_MISSION = 2,
};

enum SdgoAceAiServerSourceV1 : uint8_t {
    SDGO_ACE_AI_SERVER_PVP = 1,
    SDGO_ACE_AI_SERVER_MISSION = 2,
};

#pragma pack(push, 4)

struct SdgoAceAiClientUnitV1 {
    uint32_t pilot_id;
    uint32_t native_pilot_id;
    uint32_t object_id;
    uint32_t npc_id;
    uint32_t machine_id;
    uint32_t generation;
    uint32_t current_hp;
    uint32_t max_hp;
    int32_t slot;
    int32_t form;
    uint8_t base_difficulty;
    uint8_t effective_difficulty;
    uint8_t ace;
    uint8_t team;
    uint8_t colour;
    uint8_t active;
    uint8_t name_matches;
    uint8_t slot_name_valid;
    wchar_t expected_name[65];
    wchar_t slot_name[65];
};

struct SdgoAceAiPvpClientSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t battle_sequence;
    uint32_t unit_count;
    SdgoAceAiClientUnitV1 units[12];
};

struct SdgoAceAiMissionClientSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t battle_generation;
    uint32_t unit_count;
    uint32_t playback_sequence;
    uint32_t playback_object_id;
    uint32_t playback_generation;
    uint32_t playback_pilot_id;
    uint32_t playback_machine_id;
    int32_t playback_slot;
    uint32_t playback_colour;
    uint32_t playback_native_result;
    uint8_t playback_name_refreshed;
    uint8_t reserved[3];
    SdgoAceAiClientUnitV1 units[12];
};

// For the PvP getter, input colour 1 or 2 selects that team's latest
// successfully appended native feed receipt. Any other input colour selects
// the latest successful receipt across both teams. The mission getter ignores
// the input colour because its battle has a single authored announcement.
struct SdgoAceAiAnnouncementSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t sequence;
    uint32_t source;
    uint32_t colour;
    uint32_t feed_count_before;
    uint32_t feed_count_after;
    uint32_t successful_count;
    wchar_t text[32];
};

// Thread-local context exposed by match_ai only while it invokes the existing
// mission respawn-complete callback. It identifies the exact native
// SetSlotColour -> PlaybackUnit completion without extending the lifecycle ABI.
struct SdgoAceAiMissionPlaybackContextV1 {
    uint32_t size;
    uint32_t version;
    uint32_t object_id;
    uint32_t colour;
    uint32_t native_result;
    uint32_t active;
};

struct SdgoAceAiServerUnitV1 {
    uint32_t pilot_id;
    uint32_t native_pilot_id;
    uint32_t npc_id;
    uint32_t machine_id;
    uint32_t generation;
    uint32_t current_hp;
    uint32_t max_hp;
    uint32_t original_max_hp;
    uint32_t hp_percent;
    uint32_t attack_percent;
    int32_t slot;
    int32_t form;
    uint8_t base_difficulty;
    uint8_t effective_difficulty;
    uint8_t ace;
    uint8_t team;
    uint8_t colour;
    uint8_t active;
    uint8_t reserved[2];
};

struct SdgoAceAiDamageDecisionV1 {
    uint32_t sequence;
    uint32_t room_cookie;
    uint32_t generation;
    uint32_t attacker_pilot_id;
    uint32_t native_pilot_id;
    uint32_t attack_type;
    uint32_t proposed_damage;
    uint32_t adjusted_damage;
    uint32_t native_result;
    uint32_t attack_percent;
    uint8_t source;
    uint8_t ace;
    uint8_t policy_matched;
    uint8_t reserved;
};

struct SdgoAceAiServerSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t room_cookie;
    uint32_t mode;
    uint32_t generation;
    uint32_t unit_count;
    SdgoAceAiServerUnitV1 units[12];
    SdgoAceAiDamageDecisionV1 damage;
};

// Providers invoke this synchronously on the BattleServer thread after copying
// a completed setup, damage, or respawn/tick snapshot. The pointer is valid
// only for the call. Consumers must copy the fixed bytes and return without
// blocking, dereferencing room state, or calling back into the provider.
typedef void(__cdecl* SdgoAceAiServerObserverCallbackV1)(
    const SdgoAceAiServerSnapshotV1* snapshot,
    void* user);

struct SdgoAceAiServerObserverSinkV1 {
    uint32_t size;
    uint32_t version;
    SdgoAceAiServerObserverCallbackV1 callback;
    void* user;
};

#pragma pack(pop)

static_assert(sizeof(void*) == 4, "the Ace observer ABI is Win32-only");
static_assert(sizeof(wchar_t) == 2, "the Ace observer ABI requires Windows wchar_t");
static_assert(sizeof(SdgoAceAiClientUnitV1) == 308, "unexpected Ace client unit layout");
static_assert(sizeof(SdgoAceAiPvpClientSnapshotV1) == 3712, "unexpected Ace PvP client layout");
static_assert(sizeof(SdgoAceAiMissionClientSnapshotV1) == 3748, "unexpected Ace mission client layout");
static_assert(sizeof(SdgoAceAiAnnouncementSnapshotV1) == 96, "unexpected Ace announcement layout");
static_assert(sizeof(SdgoAceAiMissionPlaybackContextV1) == 24, "unexpected Ace mission playback context layout");
static_assert(sizeof(SdgoAceAiServerUnitV1) == 56, "unexpected Ace server unit layout");
static_assert(sizeof(SdgoAceAiDamageDecisionV1) == 44, "unexpected Ace damage layout");
static_assert(sizeof(SdgoAceAiServerSnapshotV1) == 740, "unexpected Ace server layout");
static_assert(sizeof(SdgoAceAiServerObserverSinkV1) == 16, "unexpected Ace server observer sink layout");

typedef int(__cdecl* SdgoAceAiGetPvpClientSnapshotV1Fn)(SdgoAceAiPvpClientSnapshotV1*);
typedef int(__cdecl* SdgoAceAiGetMissionClientSnapshotV1Fn)(SdgoAceAiMissionClientSnapshotV1*);
typedef int(__cdecl* SdgoAceAiGetAnnouncementSnapshotV1Fn)(SdgoAceAiAnnouncementSnapshotV1*);
typedef int(__cdecl* SdgoAceAiGetMissionPlaybackContextV1Fn)(SdgoAceAiMissionPlaybackContextV1*);
typedef int(__cdecl* SdgoAceAiGetServerSnapshotV1Fn)(SdgoAceAiServerSnapshotV1*);
typedef int(__cdecl* SdgoAceAiRegisterServerObserverV1Fn)(
    const SdgoAceAiServerObserverSinkV1*);
