#pragma once
#include <cstdint>
#include <cstring>
#include "sdgo/pvp_team_plan.h"

namespace match_ai_host {
using RoomInfo = uint8_t*(__thiscall*)(uint8_t*);
struct Bindings {
    RoomInfo roomInfo=nullptr;
    void* hostGetter=nullptr;

    uint32_t Select(uint8_t* model,uint32_t candidate) const {
        if(!(candidate&0x80000000u) || !model || !roomInfo || !hostGetter)return candidate;
        auto* vtable=*reinterpret_cast<uint8_t**>(model);
        if(!vtable || *reinterpret_cast<RoomInfo*>(vtable+0x1C)!=roomInfo ||
           *reinterpret_cast<void**>(vtable+0x70)!=hostGetter)return candidate;
        auto* room=roomInfo(model);
        if(!room)return candidate;
        switch(room[5]) {
        case 1: case 3: case 6: case 7: case 9: break;
        default:return candidate;
        }
        std::array<uint32_t,sdgo::pvp_teams::kStorageSlots> pilots{};
        for(size_t i=0;i<pilots.size();++i)
            std::memcpy(&pilots[i],room+0x533+i*0x48E+0x429,sizeof(pilots[i]));
        uint32_t previous=0,headerHost=0;
        // Prefer the transaction's cached owner while that human is still in
        // the roster, then the packed header, then another remaining human.
        std::memcpy(&previous,model+0x24,sizeof(previous));
        std::memcpy(&headerHost,room+0x4CE,sizeof(headerHost));
        previous=sdgo::pvp_teams::SelectHumanHost(previous,headerHost,pilots);
        return sdgo::pvp_teams::SelectHumanHost(candidate,previous,pilots);
    }
};
}
