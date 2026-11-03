#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace sdgo::pvp_teams {
// Counts always describe additional AI pilots, relative to the host's final
// team. Callers supply the humans after native assignment or cooperative grouping.
constexpr int kFillSide=-1;
constexpr int kFillRoom=-2;
constexpr int kStorageSlots=12;
enum class Error { None, InvalidCapacity, MissingHost, InvalidCount, RoomFull, TeamFull, EmptyOpponent };
struct Request {int allies=kFillSide,enemies=kFillSide;};
struct Room {
    int capacity=8;
    int perTeamCapacity=8;
    uint8_t hostTeam=0;
    std::array<int,3> humans{},ai{};
};
struct Plan {
    Error error=Error::None;
    uint8_t allyTeam=0,enemyTeam=0;
    int allies=0,enemies=0;
};
// Room ownership is separate from the MVP and result. Inputs are actual room
// slot identities; the caller still performs the native ownership transaction.
inline uint32_t SelectHumanHost(uint32_t nativeCandidate,uint32_t previousHost,
                               const std::array<uint32_t,kStorageSlots>& slots) {
    const auto humanPresent=[&](uint32_t pilot) {
        return pilot && !(pilot&0x80000000u) && std::find(slots.begin(),slots.end(),pilot)!=slots.end();
    };
    if(humanPresent(nativeCandidate))return nativeCandidate;
    if(humanPresent(previousHost))return previousHost;
    for(const auto pilot:slots)if(humanPresent(pilot))return pilot;
    return 0;
}
inline Plan Resolve(Request request,const Room& room) {
    Plan plan;
    if(room.capacity<2 || room.capacity>kStorageSlots || room.perTeamCapacity<1 ||
       room.perTeamCapacity>kStorageSlots) {plan.error=Error::InvalidCapacity;return plan;}
    if(room.hostTeam!=1 && room.hostTeam!=2) {plan.error=Error::MissingHost;return plan;}
    plan.allyTeam=room.hostTeam;plan.enemyTeam=static_cast<uint8_t>(3-room.hostTeam);
    int present[3]{};
    for(int side=1;side<=2;++side) {
        if(room.humans[side]<0 || room.ai[side]<0) {plan.error=Error::InvalidCount;return plan;}
        present[side]=room.humans[side]+room.ai[side];
        if(present[side]>room.perTeamCapacity) {plan.error=Error::TeamFull;return plan;}
    }
    const int free=room.capacity-present[1]-present[2];
    if(free<0) {plan.error=Error::RoomFull;return plan;}
    if(room.humans[plan.allyTeam]<1) {plan.error=Error::MissingHost;return plan;}
    const auto valid=[](int value){return value>=kFillRoom && value<=kStorageSlots;};
    if(!valid(request.allies) || !valid(request.enemies)) {plan.error=Error::InvalidCount;return plan;}
    if(request.allies==kFillRoom && request.enemies==kFillRoom)
        request={kFillSide,kFillSide};
    const int allyPresent=present[plan.allyTeam],enemyPresent=present[plan.enemyTeam];
    const int allyTarget=(room.capacity+1)/2;
    const int enemyTarget=room.capacity/2;
    plan.allies=request.allies>=0 ? request.allies:0;
    plan.enemies=request.enemies>=0 ? request.enemies:0;
    if(plan.allies+plan.enemies>free) {plan.error=Error::RoomFull;return plan;}
    int vacancies=free-plan.allies-plan.enemies;
    while(vacancies>0) {
        const bool allyFill=request.allies==kFillSide && allyPresent+plan.allies<allyTarget;
        const bool enemyFill=request.enemies==kFillSide && enemyPresent+plan.enemies<enemyTarget;
        if(!allyFill && !enemyFill)break;
        if(allyFill && (!enemyFill || allyPresent+plan.allies<=enemyPresent+plan.enemies))++plan.allies;
        else ++plan.enemies;
        --vacancies;
    }
    if(request.allies==kFillRoom)plan.allies=free-plan.enemies;
    if(request.enemies==kFillRoom)plan.enemies=free-plan.allies;
    if(allyPresent+plan.allies>room.perTeamCapacity || enemyPresent+plan.enemies>room.perTeamCapacity) {
        plan.error=Error::TeamFull;return plan;
    }
    if(enemyPresent+plan.enemies==0 && (request.allies!=0 || request.enemies!=0))plan.error=Error::EmptyOpponent;
    return plan;
}
} // namespace sdgo::pvp_teams
