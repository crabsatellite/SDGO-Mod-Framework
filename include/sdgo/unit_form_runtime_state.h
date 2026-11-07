#pragma once
#include <stdint.h>
#pragma pack(push,4)
typedef struct SdgoUnitFormRuntimeStateV1 {
    uint32_t size,version;
    uint32_t capture_installed,observer_healthy;
    uint32_t rooms,players,pending;
    uint32_t reserved;
    uint64_t completed,rejected,live_transform_commits;
    uint32_t sample_root,sample_owner,sample_deck,sample_definition,sample_room,sample_player;
    uint64_t sample_serial,sample_room_epoch,sample_generation;
    char candidate_sha256[65],authority_sha256[65],server_fingerprint[65];
} SdgoUnitFormRuntimeStateV1;
#pragma pack(pop)
typedef int (__cdecl* SdgoReadUnitFormRuntimeStateV1Fn)(SdgoUnitFormRuntimeStateV1*);
#ifdef __cplusplus
static_assert(sizeof(SdgoUnitFormRuntimeStateV1)==300,"unit form runtime state ABI changed");
#endif
