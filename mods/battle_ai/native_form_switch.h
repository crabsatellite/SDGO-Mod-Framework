#pragma once
#include "native_capabilities.h"

namespace battle_ai::native {

struct FormTransition {
    bool known=false,available=false,allowed=false,busy=false;
    std::uint32_t currentId=0,targetId=0;
    std::uint8_t condition=0;
};

inline bool TransformOwnsMotor(std::uint8_t* unit) noexcept {
    using namespace detail;
    std::uint8_t *move=nullptr,*owner=nullptr;
    std::uint32_t current=0,pending=0,mode=0;
    return ReadPointer(unit,0x9C8,move)&&ReadPointer(move,0x21C,owner)&&owner==unit&&
        Read(move,4,current)&&Read(move,8,pending)&&Read(move,0x2C0,mode)&&
        (StateIs(move,current,0xD4)||StateIs(move,pending,0xD4)||mode!=0);
}

// Original input admission 0x006D7260 precedes TransformRequest. Its state
// queries use the pending state when present (0x00BE38F0), unlike a conservative
// current-or-pending action-ownership check. This is read-only.
inline bool TransformInputAllowed(std::uint8_t* move,std::uint8_t* attack) noexcept {
    using namespace detail;
    std::uint8_t* secondary=nullptr;
    std::uint8_t cancelOpen=0;
    std::uint32_t mc=0,mp=0,ac=0,ap=0,sc=0,sp=0;
    if(!ReadPointer(move,0x220,secondary)||!Read(attack,0xB38,cancelOpen)||!cancelOpen||
       !Read(move,4,mc)||!Read(move,8,mp)||!Read(attack,4,ac)||!Read(attack,8,ap)||
       !Read(secondary,4,sc)||!Read(secondary,8,sp))return false;
    const auto movement=mp?mp:mc,weapon=ap?ap:ac,effect=sp?sp:sc;
    if(StateIs(secondary,effect,0x34)||StateIs(secondary,effect,0x5C)||
       StateIs(move,movement,0xFC)||StateIs(move,movement,0x110))return false;
    for(const auto offset:{0x34u,0x98u,0xC0u,0x48u,0x5Cu})
        if(StateIs(attack,weapon,offset))return false;
    return true;
}

// The peer definition and its condition, not bytes inside the model pathname,
// define this directed transition. Native request/Entry/Release own the commit.
inline FormTransition ReadFormTransition(std::uint8_t* unit) noexcept {
    using namespace detail;
    FormTransition result{};
    std::uint8_t *move=nullptr,*attack=nullptr,*current=nullptr,*parked=nullptr,*owner=nullptr,*boundAttack=nullptr;
    std::uint8_t attackBlocked=1,currentCondition=0,attackInput=1;
    std::uint32_t state=0,pending=0,mode=0,parkedId=0,hurtState=0;
    if(!ReadPointer(unit,0x9C8,move)||!ReadPointer(unit,0x9CC,attack)||
       !ReadPointer(unit,kActiveUnitDefinition,current)||
       !ReadPointer(move,0x21C,owner)||owner!=unit||
       !ReadPointer(move,0x224,boundAttack)||boundAttack!=attack||
       !Read(move,4,state)||!Read(move,8,pending)||!Read(move,0x2C0,mode)||
       !Read(current,kUnitId,result.currentId))return result;
    result.known=true;
    result.busy=StateIs(move,state,0xD4)||StateIs(move,pending,0xD4)||mode!=0;
    if(!Read(current,kTransformPeerId,result.targetId)||!result.targetId||
       !ReadPointer(unit,kParkedUnitDefinition,parked)||parked==current||
       !Read(parked,kUnitId,parkedId)||parkedId!=result.targetId||
       !Read(parked,0x140,result.condition))return result;
    result.available=true;
    // Original physical R admission (0x0067C940) checks the CURRENT form's
    // outgoing edge and excludes automatic-only condition3 forms. A parked
    // reciprocal record does not invent a return edge for a one-way form.
    // Native Msg1D takes its respawn branch at 0x006894FC instead of entering
    // D4: that state's Update does not run the state6 recovery path. Keep the
    // original respawn controller until it has restored the unit's state.
    if(result.busy||!Read(unit,0x9D8,hurtState)||hurtState==6||
       !Read(current,0x140,currentCondition)||currentCondition==3||
       !Read(unit,0xDCD,attackInput)||attackInput||!TransformInputAllowed(move,attack)||
       !Read(attack,0x9B2,attackBlocked)||attackBlocked)return result;
    result.allowed=true;
    switch(result.condition) {
    case 1: {
        std::int32_t hp=0,maximum=0;
        result.allowed=Read(unit,0x38,hp)&&Read(unit,0x3C,maximum)&&maximum>0&&hp>=0&&
            (std::int64_t(hp)*100/maximum)<40;
        break;
    }
    case 2: case 4: case 5: case 6: {
        const auto bank=(result.condition==5||result.condition==6)?kAlternateBank:kStandardBank;
        const auto slot=bank+2*kSlotStride;
        std::int16_t ammo=0,maximum=0;
        std::uint8_t* record=nullptr;
        if(!Read(attack,slot+kSlotAmmo,ammo)) {result.allowed=false;break;}
        if(result.condition==4||result.condition==6) result.allowed=ammo>3;
        else result.allowed=ReadPointer(attack,slot+kSlotWeapon,record)&&Read(record,kMaxAmmo,maximum)&&ammo==maximum;
        break;
    }
    // Native condition3, zero and values outside1..6 impose no extra rule.
    default: break;
    }
    return result;
}

} // namespace battle_ai::native
