#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum RoomStylePreviewDecisionV1 {
    ROOM_STYLE_PREVIEW_UNOBSERVED = 0,
    ROOM_STYLE_PREVIEW_PASSTHROUGH = 1,
    ROOM_STYLE_PREVIEW_REWRITTEN = 2
};

enum RoomStylePreviewRejectV1 {
    ROOM_STYLE_PREVIEW_REJECT_NONE = 0,
    ROOM_STYLE_PREVIEW_REJECT_DISABLED = 1,
    ROOM_STYLE_PREVIEW_REJECT_NOT_APP18 = 2,
    ROOM_STYLE_PREVIEW_REJECT_UNKNOWN_CALLER = 3,
    ROOM_STYLE_PREVIEW_REJECT_UNREADABLE_PAYLOAD = 4,
    ROOM_STYLE_PREVIEW_REJECT_WRONG_FACTORY = 5,
    ROOM_STYLE_PREVIEW_REJECT_INVALID_ID = 6,
    ROOM_STYLE_PREVIEW_REJECT_NO_PROFILE = 7,
    ROOM_STYLE_PREVIEW_REJECT_NATIVE_IDENTITY_MISMATCH = 8,
    /* Kept as an ABI/source alias for the first prototype observer. */
    ROOM_STYLE_PREVIEW_REJECT_PROFILE_MISMATCH =
        ROOM_STYLE_PREVIEW_REJECT_NATIVE_IDENTITY_MISMATCH,
    ROOM_STYLE_PREVIEW_REJECT_ALREADY_USED = 9,
    ROOM_STYLE_PREVIEW_REJECT_WRITE_FAILED = 10
};

/*
 * Fixed-width, process-local snapshot for the isolated GameTest observer.
 * `ready` is one only when the current native app, unique shipped manager,
 * manager room id and manager map all match the rewritten transition.
 * `refresh_count` is intentionally passive in this prototype. The existing
 * transport observer owns refresh observation and can compare its count with
 * `expected_refresh_opcode` (APP7=0x0943, APP16=0x08A5).
 */
typedef struct RoomStylePreviewSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t requested_target;
    uint32_t actual_app;
    uint32_t room_id;
    uint32_t map_id;
    uint32_t transitions;
    uint32_t ready;
    uint32_t refresh_count;
    uint32_t expected_refresh_opcode;
    uint32_t last_decision;
    uint32_t reject_reason;
    uint32_t factory_type;
    uint32_t source_app;
    uint32_t caller_rva;
    uint32_t payload_address;
    uint32_t profile_room_id;
    uint32_t profile_map_id;
    uint32_t manager_found;
    uint32_t manager_ids_match;
    uint32_t tail_crc32;
    uint32_t reserved[5]; /* authority room, original source, source/shape/authority matches */
} RoomStylePreviewSnapshotV1;

typedef int(__cdecl* SdgoRoomStylePreviewGetSnapshotV1Fn)(
    RoomStylePreviewSnapshotV1* out,
    uint32_t out_size);

#ifdef __cplusplus
}
#endif
