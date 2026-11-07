#pragma once
#include "cache_sync.h"

namespace room_random_client {
using RoomInfoFn=uint8_t*(__thiscall*)(uint8_t*);
using ManagerUpdateFn=void(__thiscall*)(uint8_t*);
using UserHasStateFn=uint8_t(__thiscall*)(uint8_t*,int,int,int);
using ChangeRoomViewFn=int(__thiscall*)(uint8_t*);
struct Bindings {
    uint8_t** profile=nullptr;
    uint8_t** app=nullptr;
    const void* waitRoomVtable=nullptr;
    const void* changeRoomViewVtable=nullptr;
    RoomInfoFn roomInfo=nullptr;
    ManagerUpdateFn managerUpdate=nullptr;
    UserHasStateFn userHasState=nullptr;
    ChangeRoomViewFn changeRoomView=nullptr;
    SlotAccess normal,mission;
};
// A legacy CAccountData+EC publication can survive a scene transition. The
// active 3D manager owns the current CBaseRoomData; its verified native getter
// takes precedence over that older publication.
// Never retain the manager/model pointer across scene transitions.
inline Context ResolveModel(const Bindings& b,uint8_t* model,
                            uint8_t* manager=nullptr) {
    Context c;
    c.profile=b.profile ? *b.profile : nullptr;
    if(!c.profile)return c;
    c.local=Read<uint32_t>(c.profile,0x138);
    auto* vtable=model ? Read<uint8_t*>(model,0) : nullptr;
    if(!vtable || !b.roomInfo || Read<RoomInfoFn>(vtable,0x1C)!=b.roomInfo)return c;
    const auto ownerGetter=Read<SlotAccess::Owner>(vtable,0x20);
    const auto& slots=ownerGetter==b.normal.owner ? b.normal : b.mission;
    if(!ownerGetter || ownerGetter!=slots.owner || Read<SlotAccess::Get>(vtable,0x88)!=slots.active ||
       Read<SlotAccess::Get>(vtable,0x90)!=slots.ship || Read<SlotAccess::Set>(vtable,0x84)!=slots.setActive ||
       Read<SlotAccess::Set>(vtable,0x8C)!=slots.setShip) return c;
    if(manager) {
        auto* managerVtable=Read<uint8_t*>(manager,0);
        if(!managerVtable || !b.managerUpdate || !b.userHasState ||
           Read<ManagerUpdateFn>(managerVtable,0x10)!=b.managerUpdate ||
           Read<UserHasStateFn>(managerVtable,0x1C)!=b.userHasState ||
           Read<uint8_t*>(manager,0x38)!=model)return c;
    }
    c.room=b.roomInfo(model);c.source=2;c.model=model;c.slots=slots;
    c.manager=manager;return c;
}
inline Context Resolve(const Bindings& b) {
    Context c;
    c.profile=b.profile ? *b.profile : nullptr;
    if(!c.profile)return c;
    c.local=Read<uint32_t>(c.profile,0x138);
    auto* app=b.app ? *b.app : nullptr;
    if(app && Read<uint32_t>(app,0x4A0)==18) {
        auto* owner=Read<uint8_t*>(app,0x4A4);
        if(!owner || Read<const void*>(owner,0)!=b.waitRoomVtable)return c;
        auto* manager=Read<uint8_t*>(owner,4);
        auto* managerVtable=manager ? Read<uint8_t*>(manager,0) : nullptr;
        if(!managerVtable || !b.managerUpdate || !b.userHasState ||
           Read<ManagerUpdateFn>(managerVtable,0x10)!=b.managerUpdate ||
           Read<UserHasStateFn>(managerVtable,0x1C)!=b.userHasState)return c;
        return ResolveModel(b,Read<uint8_t*>(manager,0x38),manager);
    }
    c.room=Read<uint8_t*>(c.profile,0xEC);
    if(c.room)c.source=1;
    return c;
}

inline bool HumanStartTasksSettled(const Bindings& b,const Context& c,
                                   const uint8_t* packet) {
    if(c.source!=2)return true;
    if(!packet || !c.manager || !b.userHasState ||
       Read<uint16_t>(packet,0)!=0x06C3)return false;
    const int count=Read<int>(c.manager,0x6C);
    auto** users=Read<uint8_t**>(c.manager,0x70);
    if(count<=0 || count>static_cast<int>(kSlots) || !users)return false;
    bool human=false;
    for(size_t slot=0;slot<kSlots;++slot) {
        const auto* entry=packet+0x20+slot*0x10F;
        const uint32_t owner=Read<uint32_t>(entry,0);
        if(!owner || (owner&0x80000000u))continue;
        human=true;
        // 0861/098B enqueue E/F, then the local player's handler enqueues C.
        // Once 06C3 is held, that C request is obsolete and the process hook
        // retires it through the native scheduler. The root D start state runs
        // before per-player queues on the shipped manager tick, so all three
        // must leave the queues before the original parser is resumed.
        if(static_cast<int>(slot)>=count || !users[slot] ||
           c.Owner(slot)!=owner ||
           b.userHasState(c.manager,static_cast<int>(slot),0x0C,0) ||
           b.userHasState(c.manager,static_cast<int>(slot),0x0E,0) ||
           b.userHasState(c.manager,static_cast<int>(slot),0x0F,0))
            return false;
    }
    return human;
}

// State 0x0C is the local player's ChangeRoomView request. The shipped
// per-user manager owns both its external state queue and a private payload
// queue inside the state object. Resolve only the exact live local service;
// a room title or a matching address alone is not authority to retire it.
inline uint8_t* CurrentHumanRoomViewState(const Bindings& b,const Context& c,
                                          const uint8_t* packet) {
    if(c.source!=2 || !packet || !c.manager || !c.local ||
       !b.userHasState || !b.changeRoomView || !b.changeRoomViewVtable ||
       Read<uint16_t>(packet,0)!=0x06C3)return nullptr;
    const int count=Read<int>(c.manager,0x6C);
    auto** users=Read<uint8_t**>(c.manager,0x70);
    if(count<=0 || count>static_cast<int>(kSlots) || !users)return nullptr;
    int localSlot=-1;
    for(size_t slot=0;slot<kSlots;++slot) {
        const uint32_t owner=Read<uint32_t>(packet+0x20+slot*0x10F,0);
        if(owner!=c.local)continue;
        if(localSlot!=-1)return nullptr;
        localSlot=static_cast<int>(slot);
    }
    if(localSlot<0 || localSlot>=count || !users[localSlot] ||
       c.Owner(static_cast<size_t>(localSlot))!=c.local ||
       !b.userHasState(c.manager,localSlot,0x0C,0))return nullptr;
    auto** states=Read<uint8_t**>(users[localSlot],0x0C);
    auto* state=states ? states[0x0C] : nullptr;
    if(!state || Read<const void*>(state,0)!=b.changeRoomViewVtable ||
       Read<int>(state,4)!=0x0C || Read<int>(state,8)!=1 ||
       Read<ChangeRoomViewFn>(
           static_cast<const uint8_t*>(b.changeRoomViewVtable),8)!=
               b.changeRoomView)
        return nullptr;
    return state;
}

// Mirror 0x00735E59..0x00735E6B before returning zero to the native scheduler.
// Its normal DeleteState path then invokes state+0x28, removes exactly one
// private payload node, and frees the two-byte payload now owned at +0x20.
inline bool PrimeRoomViewPayloadForNativeCleanup(uint8_t* state) {
    if(!state || Read<uint8_t*>(state,0x20) ||
       Read<uint32_t>(state,0x18)==0)return false;
    auto* sentinel=Read<uint8_t*>(state,0x14);
    auto* first=sentinel ? Read<uint8_t*>(sentinel,0) : nullptr;
    auto* payload=(first && first!=sentinel) ?
        Read<uint8_t*>(first,8) : nullptr;
    if(!payload)return false;
    std::memcpy(state+0x20,&payload,sizeof(payload));
    return true;
}
}
