#pragma once

#include <stdint.h>

#define SDGO_SCENE_LIFECYCLE_STATUS_EXPORT "SdgoSceneLifecycleGetStatusV1"
#define SDGO_SCENE_LIFECYCLE_STATUS_VERSION 1u

typedef struct SdgoSceneLifecycleStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t process_id;
    uint32_t reparent_calls;
    uint32_t allocation_attempts;
    uint32_t transfers;
    uint32_t same_owner;
    uint32_t from_unowned;
    uint32_t allocator_null;
    uint32_t capacity_errors;
} SdgoSceneLifecycleStatusV1;

typedef int(__cdecl* SdgoSceneLifecycleGetStatusV1Fn)(
    SdgoSceneLifecycleStatusV1* output);
