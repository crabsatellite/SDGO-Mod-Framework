#pragma once
#include <stdint.h>

#define SDGO_UI_LOCALIZATION_SNAPSHOT_EXPORT "SdgoUiLocalizationGetSnapshotV1"
typedef struct SdgoUiLocalizationSnapshotV1 {
    uint32_t size,version,installed,phrase_count;
    uint64_t distinct_phrases,control_items,system_lookups,system_replaced,guarded_passthrough,invalid_inputs;
    uint32_t last_phrase,last_caller_rva,trace_enabled,system_phrase_count;
} SdgoUiLocalizationSnapshotV1;
typedef int (__cdecl* SdgoUiLocalizationGetSnapshotV1Fn)(SdgoUiLocalizationSnapshotV1*);
#ifdef __cplusplus
static_assert(sizeof(SdgoUiLocalizationSnapshotV1)==80,"UI localization observer ABI");
#endif
