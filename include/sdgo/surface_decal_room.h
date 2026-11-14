#pragma once
#include <cstdint>
#include <cstddef>

namespace sdgo::surface_decal {
// The supported client stores 32-bit pointers, including in offline fixtures.
// Resolve the live visual object's slot, never an identity guessed from skin,
// type or the condensed appearance (which contains no owned-unit serial).
struct RoomBindings {
    std::uint32_t service=0,managerUpdate=0,roomInfo=0;
    std::uint32_t normalOwner=0,missionOwner=0,normalUnit=0,missionUnit=0;
    std::uint32_t object=0,data=0;
    bool Valid() const {
        return service&&managerUpdate&&roomInfo&&normalOwner&&missionOwner&&
            normalUnit&&missionUnit&&object&&data;
    }
    template<class Read,class Get>
    std::uint32_t Resolve(std::uint32_t app,std::uint32_t model,Read read,Get get) const {
        if(!Valid()||!app||!model)return 0;
        const auto at=[&](std::uint32_t base,std::uint32_t offset=0){
            std::uint32_t value=0;
            if(!base||base>UINT32_MAX-offset||!read(base+offset,&value,sizeof value))return std::uint32_t{0};
            return value;
        };
        if(at(app,0x4a0)!=18)return 0;
        const auto owner=at(app,0x4a4);
        if(at(owner)!=service)return 0;
        const auto manager=at(owner,4),room=at(manager,0x38),visual=at(manager,0x40);
        if(at(at(manager),0x10)!=managerUpdate||at(at(room),0x1c)!=roomInfo)return 0;
        const auto ownerGetter=at(at(room),0x20),unitGetter=at(at(room),0x88);
        if(!((ownerGetter==normalOwner&&unitGetter==normalUnit)||
             (ownerGetter==missionOwner&&unitGetter==missionUnit)))return 0;
        const auto count=at(visual,8),slots=at(visual,0xc),roomCount=at(room,0x28);
        // The native visual pool reserves twelve objects even when the room
        // exposes only eight (team PvP) or four (mission) player slots.
        // Resolve only real room slots; spare visual objects have no owner.
        if(!slots||!roomCount||roomCount>count||count>12)return 0;
        for(std::uint32_t i=0;i<roomCount;++i){
            const auto entry=at(slots,i*4);
            if(!entry||entry>UINT32_MAX-0x2118||at(entry)!=object||at(entry,0x1c)!=data)continue;
            const auto first=entry+0x1c+0x17d8;
            if(model!=first&&model!=first+0x488)continue;
            const auto record=get(unitGetter,room,i);
            // An asynchronous loader must still belong to this live room.
            if(at(app,0x4a0)!=18||at(app,0x4a4)!=owner||at(owner,4)!=manager||
               at(manager,0x38)!=room||at(manager,0x40)!=visual||at(visual,0xc)!=slots||
               at(slots,i*4)!=entry)return 0;
            return record;
        }
        return 0;
    }
};
}
