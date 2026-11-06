#pragma once
#include <cstdint>
#include <cstring>
#include "sdgo/mission_behavior_profile.h"

namespace sdgo::mission_behavior {
// Current native World->Battle download record and MissionNpcNode prefix.
// The native AddMissionNpc copies exactly 0x2b bytes into a 0x34-byte node.
#pragma pack(push,1)
struct NativeNpcRow {
    std::uint32_t id, mission, difficulty, part, npc;
    std::int32_t x, y, z;
    std::uint16_t after_time, abs_time;
    std::uint32_t manual;
    std::uint8_t party, create_limit, zone;
};
#pragma pack(pop)
static_assert(sizeof(NativeNpcRow)==43);
static_assert(offsetof(NativeNpcRow,part)==0xc);
static_assert(offsetof(NativeNpcRow,npc)==0x10);
static_assert(offsetof(NativeNpcRow,party)==0x28);

inline NpcRow DecodeNative(const NativeNpcRow& n) {
    return {n.id,n.mission,n.difficulty,n.part,n.npc,n.x,n.y,n.z,
        n.after_time,n.abs_time,n.manual,n.party,n.create_limit,n.zone};
}
inline void ApplyNative(const NpcRow& row, std::uint32_t mask, NativeNpcRow& n) {
    if(mask&kChangeNpc)n.npc=row.npc;
    if(mask&kChangeX)n.x=row.x;
    if(mask&kChangeY)n.y=row.y;
    if(mask&kChangeZ)n.z=row.z;
    if(mask&kChangeDelay)n.after_time=static_cast<std::uint16_t>(row.after_time);
    if(mask&kChangeLimit)n.create_limit=static_cast<std::uint8_t>(row.create_limit);
}
} // namespace sdgo::mission_behavior
