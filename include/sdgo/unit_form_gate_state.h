#pragma once
#include <stdint.h>
#pragma pack(push,4)
// Diagnostics only: independent monotonic counters, not an actor permission
// token or transactional snapshot. No source/player blobs or pointers exported.
typedef struct SdgoUnitFormGateStateV1 {
    uint32_t size,version,installed,expected_definitions;
    uint64_t calls,unmanaged,cached_allowed,uncached_directed,rejected;
    uint64_t stale_capture,definition_checks,normalization_observations;
    uint64_t actor_writes; // By this gate, not the original cached-transform routine.
} SdgoUnitFormGateStateV1;
#pragma pack(pop)
typedef int (__cdecl* SdgoReadUnitFormGateStateV1Fn)(SdgoUnitFormGateStateV1*);
#ifdef __cplusplus
static_assert(sizeof(SdgoUnitFormGateStateV1)==88,"unit form gate state ABI changed");
#endif

#pragma pack(push,4)
typedef struct SdgoUnitFormRebindStateV1 {
    uint32_t size,version,compiled_for_testing;
    uint32_t last_root,last_current,last_target,last_stage,last_native_fault,last_commit_result,reserved;
    uint64_t attempts,committed;
} SdgoUnitFormRebindStateV1;
#pragma pack(pop)
typedef int (__cdecl* SdgoReadUnitFormRebindStateV1Fn)(SdgoUnitFormRebindStateV1*);
#ifdef __cplusplus
static_assert(sizeof(SdgoUnitFormRebindStateV1)==56,"unit form rebind state ABI changed");
#endif
