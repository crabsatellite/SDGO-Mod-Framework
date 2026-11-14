#pragma once
// Optional read-only client query. It recognizes a live, catalog-owned chain
// that has already committed its controller's latched target. It neither
// authorizes another edge nor changes the actor or its animation state.
#define SDGO_UNIT_TRANSFORM_RECOVERY_EXPORT "SdgoIsImportedTransformRecoveryV1"
typedef int (__cdecl* SdgoIsImportedTransformRecoveryV1Fn)(const void* unit);
