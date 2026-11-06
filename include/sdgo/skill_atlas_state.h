#pragma once
#include <cstdint>

struct SdgoSkillAtlasSlotV1 {
    std::uint32_t calls,owner_loads,baseline_loads,skill_id,requested_frame,actual_frame;
    std::uint32_t player,frame_first,frame_last;
    char sprite[81];
};
struct SdgoSkillAtlasStateV1 {
    std::uint32_t size,version,failures;
    SdgoSkillAtlasSlotV1 slots[2];
    char sidecar_sha256[65];
};
using SdgoReadSkillAtlasStateV1Fn=int(__cdecl*)(SdgoSkillAtlasStateV1*);

// The original actor initializer's resolved texture/UV, separate from detail
// widgets. These observations do not by themselves prove the final HUD draw.
struct SdgoSkillAtlasBattleEntryV1 {
    std::uint32_t skill_id,frame,calls;
    char sprite[81],texture[81];
    float uv[4];
};
struct SdgoSkillAtlasBattleStateV1 {
    std::uint32_t size,version,calls,owner_loads,baseline_loads,failures,count;
    SdgoSkillAtlasBattleEntryV1 entries[16];
};
using SdgoReadSkillAtlasBattleStateV1Fn=int(__cdecl*)(SdgoSkillAtlasBattleStateV1*);
