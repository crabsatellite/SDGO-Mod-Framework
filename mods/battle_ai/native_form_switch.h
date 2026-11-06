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

// Read the same form definitions, bank counters and admission restrictions as
// native TransformRequest 0x006D8B90. Request/Entry/Release remain native code.
inline FormTransition ReadFormTransition(std::uint8_t* unit) noexcept {
    using namespace detail;
    FormTransition result{};
    std::uint8_t *move=nullptr,*attack=nullptr,*current=nullptr,*parked=nullptr,*owner=nullptr,*boundAttack=nullptr;
    std::uint8_t kind=0,attackBlocked=1;
    std::uint32_t state=0,pending=0,mode=0,parkedId=0;
    if(!ReadPointer(unit,0x9C8,move)||!ReadPointer(unit,0x9CC,attack)||
       !ReadPointer(unit,kActiveUnitDefinition,current)||
       !ReadPointer(move,0x21C,owner)||owner!=unit||
       !ReadPointer(move,0x224,boundAttack)||boundAttack!=attack||
       !Read(move,4,state)||!Read(move,8,pending)||!Read(move,0x2C0,mode)||
       !Read(current,kUnitId,result.currentId)||!Read(current,kTransformKind,kind))return result;
    result.known=true;
    result.busy=StateIs(move,state,0xD4)||StateIs(move,pending,0xD4)||mode!=0;
    if(!kind||!Read(current,kTransformPeerId,result.targetId)||!result.targetId||
       !ReadPointer(unit,kParkedUnitDefinition,parked)||parked==current||
       !Read(parked,kUnitId,parkedId)||parkedId!=result.targetId||
       !Read(parked,0x140,result.condition))return result;
    result.available=true;
    if(result.busy||!Read(attack,0x9B2,attackBlocked)||attackBlocked)return result;
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
