#pragma once
#include <stdint.h>

// Read-only status for native acceptance observers; no mutation or test switches.
typedef struct SdgoNativeDataGuardStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t funnel_bounds_corrected;
    uint32_t missing_names;
    uint32_t missing_transform_metadata;
} SdgoNativeDataGuardStatusV1;
