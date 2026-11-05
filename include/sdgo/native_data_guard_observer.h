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

// Additive observer ABI. V1 remains exported for existing GameTest clients.
typedef struct SdgoNativeDataGuardStatusV2 {
    uint32_t size;
    uint32_t version;
    uint32_t funnel_bounds_corrected;
    uint32_t missing_names;
    uint32_t missing_transform_metadata;
    uint32_t missing_information_records;
    uint32_t preserved_exception_contexts;
} SdgoNativeDataGuardStatusV2;

// Additive observer ABI for the unit-management deferred-message lifecycle.
typedef struct SdgoNativeDataGuardStatusV3 {
    uint32_t size;
    uint32_t version;
    uint32_t funnel_bounds_corrected;
    uint32_t missing_names;
    uint32_t missing_transform_metadata;
    uint32_t missing_information_records;
    uint32_t preserved_exception_contexts;
    uint32_t deferred_message_copies;
    uint32_t deferred_message_replacements;
    uint32_t deferred_message_copy_failures;
    uint32_t deferred_message_completions;
    uint32_t outstanding_deferred_messages;
} SdgoNativeDataGuardStatusV3;

// Additive observer ABI for whole-page base-information preflight.
typedef struct SdgoNativeDataGuardStatusV4 {
    uint32_t size;
    uint32_t version;
    uint32_t funnel_bounds_corrected;
    uint32_t missing_names;
    uint32_t missing_transform_metadata;
    uint32_t missing_information_records;
    uint32_t preserved_exception_contexts;
    uint32_t deferred_message_copies;
    uint32_t deferred_message_replacements;
    uint32_t deferred_message_copy_failures;
    uint32_t deferred_message_completions;
    uint32_t outstanding_deferred_messages;
    uint32_t missing_base_information_records;
} SdgoNativeDataGuardStatusV4;
