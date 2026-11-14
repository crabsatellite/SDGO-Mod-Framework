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

// Additive observer ABI for loading-time material and wait-room camera reads.
typedef struct SdgoNativeDataGuardStatusV5 {
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
    uint32_t unavailable_material_slots;
    uint32_t skipped_decal_slots;
    uint32_t rejected_camera_indices;
    uint32_t skipped_camera_updates;
} SdgoNativeDataGuardStatusV5;

// Additive observer for the native login inventory / pending-menu boundary.
typedef struct SdgoNativeDataGuardStatusV6 {
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
    uint32_t unavailable_material_slots;
    uint32_t skipped_decal_slots;
    uint32_t rejected_camera_indices;
    uint32_t skipped_camera_updates;
    uint32_t deferred_inventory_transition_calls;
    uint32_t resumed_inventory_transitions;
    uint32_t deferred_inventory_catalog_requests;
    uint32_t resumed_inventory_catalog_requests;
    uint32_t cancelled_inventory_catalog_requests;
} SdgoNativeDataGuardStatusV6;

// Independent read-only ABI: the existing native damage/message bank is bounded
// to twelve targets, including hits collected before the sorted actor pass.
typedef struct SdgoNativeBeamTargetGuardStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t capacity;
    uint32_t bounded_loops;
} SdgoNativeBeamTargetGuardStatusV1;
