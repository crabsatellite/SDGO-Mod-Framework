#pragma once
#include <stdint.h>

// Read-only observation of the original native mission-success handler. This
// does not execute a packet, advance a mission, or grant cleanup authority.
#define SDGO_MAP_MISSION_SUCCESS_VERSION 1
#define SDGO_MAP_MISSION_SUCCESS_EXPORT "SdgoMapCatalogGetMissionSuccessV1"

struct SdgoMapMissionSuccessV1 {
    uint32_t size;
    uint32_t version;
    uint32_t hook_active;
    uint32_t sequence;
    uint32_t mission_id;
    uint32_t phase;
    uint32_t clear_id;
    uint32_t event_order;
    uint32_t original_result;
    uint32_t count;
    uint32_t pilot_ids[10];
};
static_assert(sizeof(SdgoMapMissionSuccessV1)==80,"unexpected mission-success observation ABI");
typedef int(__cdecl* SdgoMapCatalogGetMissionSuccessV1Fn)(SdgoMapMissionSuccessV1* value);

#define SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_VERSION 1
#define SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_EXPORT "SdgoMapCatalogGetLocalBoundaryReceiptV1"
struct SdgoMapLocalBoundaryReceiptV1 {
    uint32_t size;
    uint32_t version;
    uint32_t hook_active;
    uint32_t sequence;
    uint32_t tick;
    uint32_t mission_id;
    uint32_t phase;
    uint32_t wire_pilot;
    uint32_t pilot_id;
    uint32_t object_id;
    uint32_t npc_id;
    uint32_t notice_hp;
    int32_t before_hp;
    int32_t after_hp;
    int32_t request_before_hp;
    int32_t maximum_hp;
    uint32_t feedback_pending;
};
static_assert(sizeof(SdgoMapLocalBoundaryReceiptV1)==68,"unexpected local boundary receipt ABI");
typedef int(__cdecl* SdgoMapCatalogGetLocalBoundaryReceiptV1Fn)(SdgoMapLocalBoundaryReceiptV1* value);
