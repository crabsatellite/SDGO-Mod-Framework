#pragma once
#include <cstdint>

// Optional capsule-provider bridge for imported native type-85 catalog rows.
// Applies both configured capsule sources and optional collection hints. Keeping
// the existing V1 export lets unit imports pick up later capsule publications.
// The caller owns the writable 614-byte row; no pointer is retained.
using SdgoCollectionAcquisitionApplyV1Fn = int(__cdecl*)(void*, uint32_t);
#define SDGO_COLLECTION_ACQUISITION_APPLY_EXPORT "SdgoCollectionAcquisitionApplyV1"
