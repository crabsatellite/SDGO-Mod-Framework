#pragma once
#include <stdint.h>
#define SDGO_HANGAR_PREVIEW_GUARD_STATUS_EXPORT "SdgoHangarPreviewGuardGetStatusV1"
struct SdgoHangarPreviewGuardStatusV1 {
    uint32_t size,version;
    uint32_t rows,previews,loaded,fallbacks,discarded_roots,discarded_models,lock_releases;
    uint32_t missing_vertex,failed_vertex_lock,missing_index,failed_index_lock,failed_model_load;
    uint32_t last_unit,last_failure;
    uint32_t vertex24_failures,vertex32_failures;
    uint32_t closed_sources,active_preview,active_parser;
    uint32_t active_failure,active_unit,recovered_previews,last_loaded_unit;
    uint32_t last_failed_slot;
};
typedef int(__cdecl* SdgoHangarPreviewGuardGetStatusV1Fn)(SdgoHangarPreviewGuardStatusV1*);
