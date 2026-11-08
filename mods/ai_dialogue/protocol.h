#pragma once
#include <stdint.h>
#include <wchar.h>

namespace sdgo::ai_dialogue {
constexpr unsigned kParticipants=24;
#pragma pack(push,1)
struct Participant {
    uint32_t pilot,object,unit,team,ai,alive,target,weaponSlot;
    uint32_t deaths,lifeIndex;
    int32_t stockPoints;
    uint32_t lifeCost,respawnState,targetedBy;
    wchar_t name[32],unitName[64];
};
struct Request {
    char magic[8];uint32_t size,pid;uint64_t session;
    uint32_t sequence,battle;uint64_t created;
    uint32_t kind,local,speaker,target,killer,victim,focus,weapon,weaponSlot,count;
    uint32_t mode,elapsedSeconds,limitedLives;
    Participant roster[kParticipants];
};
struct Reply {char magic[8];uint64_t session;uint32_t sequence,battle,length;};
#pragma pack(pop)
static_assert(sizeof(wchar_t)==2,"native dialogue uses Windows UTF-16");
static_assert(sizeof(Participant)==248,"dialogue participant ABI");
static_assert(sizeof(Request)==6044,"dialogue request ABI");
static_assert(sizeof(Reply)==28,"dialogue reply ABI");
}
