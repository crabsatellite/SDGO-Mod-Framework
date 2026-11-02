#pragma once

#include <cstdint>

#define SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION 1u
#define SDGO_FRIEND_UNIT_GIFT_CLIENT_SNAPSHOT_EXPORT \
    "SdgoFriendUnitGiftClientGetSnapshotV1"
#define SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_EXPORT \
    "SdgoFriendUnitGiftClientActionV1"

enum SdgoFriendUnitGiftClientActionV1 : std::uint32_t {
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_NONE = 0,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_FRIEND_CONTEXT = 1,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CLICK_GIFT = 2,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SELECT_UNIT = 3,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE = 4,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_PREVIEW = 5,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CONFIRM = 6,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CANCEL = 7,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_STORAGE_VIEW = 8,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_REFRESH_INBOX = 9,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_REFRESH_RETURNED = 10,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SELECT_GIFT = 11,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_ACCEPT = 12,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_RETURN_TO_SENDER = 13,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_TAKEBACK = 14,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_PROVENANCE = 15,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_QUERY_RECEIPT = 16,
};

enum SdgoFriendUnitGiftClientComposerPhaseV1 : std::uint32_t {
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_CLOSED = 0,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_SELECTING = 1,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_PREVIEWING = 2,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_CONFIRMING = 3,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_SENDING = 4,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_OUTCOME_UNKNOWN = 5,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_COMPLETE = 6,
};

enum SdgoFriendUnitGiftClientControlFlagsV1 : std::uint32_t {
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_FRIEND_CONTEXT = 1u << 0,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_GIFT_BUTTON = 1u << 1,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_COMPOSER = 1u << 2,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_COMBO = 1u << 3,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_NOTE = 1u << 4,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_PRIMARY = 1u << 5,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_CANCEL = 1u << 6,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_PAGE = 1u << 7,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_BUTTON = 1u << 8,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_ROWS = 1u << 9,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_ACTION = 1u << 10,
    SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_PROVENANCE = 1u << 11,
};

#pragma pack(push, 8)
struct SdgoFriendUnitGiftClientRowV1 {
    std::uint64_t gift_id;
    std::uint64_t unit_uid;
    std::uint64_t offered_unix_millis;
    std::uint32_t peer_account;
    std::uint32_t unit_number;
    std::uint32_t level;
    std::uint32_t state;
    std::uint32_t direction;
    std::uint16_t peer_name_code_units;
    std::uint16_t peer_name[20];
};

struct SdgoFriendUnitGiftClientActionRequestV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint64_t run_hash;
    std::uint32_t action;
    std::uint32_t receiver_id;
    std::uint64_t unit_uid;
    std::uint64_t gift_id;
    std::uint16_t note_code_units;
    std::uint16_t note[200];
};

struct SdgoFriendUnitGiftClientSnapshotV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint64_t run_hash;
    std::uint32_t process_id;
    std::uint32_t ui_thread_id;
    std::uint64_t session;
    std::uint32_t transport_state;
    std::uint32_t capabilities;
    std::uint64_t server_epoch;
    std::int32_t last_status;
    std::uint32_t pending_command;
    std::uint8_t pending_request_id[16];
    std::uint32_t ready;
    std::uint32_t mutation_uncertain;
    std::uint32_t storage_broker;
    std::uint32_t storage_fallback;
    std::uint32_t control_flags;
    std::uint32_t composer_phase;
    std::uint32_t action_sequence;
    std::uint32_t reply_sequence;
    std::uint32_t last_action;
    std::uint32_t last_accepted_operation;
    std::uint32_t last_reply_command;
    std::int32_t last_reply_status;
    std::uint8_t last_reply_request_id[16];
    std::uint64_t last_reply_gift_id;
    std::uint64_t last_reply_unit_uid;
    std::uint32_t selected_unit_number;
    std::uint16_t selected_unit_slot;
    std::uint16_t reserved16;
    std::uint64_t selected_unit_uid;
    std::uint64_t selected_gift_id;
    std::uint32_t page;
    std::uint32_t total_count;
    std::uint32_t row_count;
    SdgoFriendUnitGiftClientRowV1 rows[20];
    std::uint32_t provenance_present;
    std::uint16_t provenance_text_code_units;
    std::uint16_t provenance_text[512];
    std::uint32_t reserved[8];
};
#pragma pack(pop)

using SdgoFriendUnitGiftClientGetSnapshotV1Fn =
    int(__cdecl*)(SdgoFriendUnitGiftClientSnapshotV1* output);
using SdgoFriendUnitGiftClientActionV1Fn =
    int(__cdecl*)(const SdgoFriendUnitGiftClientActionRequestV1* request);
