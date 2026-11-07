#include <windows.h>
#include "sdgo/native_unit_transform_context.h"
#include <cstring>

namespace sdgo::unit_forms {
namespace {
static_assert(sizeof(void*) == 4, "native transform context requires the supported x86 host");
std::uint32_t Word(const std::uint8_t* at) { std::uint32_t value;std::memcpy(&value,at,4);return value; }
std::uint64_t Serial(const std::uint8_t* at) { std::uint64_t value;std::memcpy(&value,at,8);return value; }
bool Unit(std::uint32_t value) { return value >= 10001 && value <= 30000; }
bool Read(const void* address,std::uint32_t index,const void* expected,std::uint32_t target,NativeTransformContext* out) {
    __try {
        if(!address||!expected||index>=12||!Unit(target))return false;
        const auto* room=static_cast<const std::uint8_t*>(address);
        if(!Word(room+0x21))return false;
        const auto* player=room+0x3ce4+index*0x1f54;
        const auto* source=room+0x533+index*0x48e;
        const auto deck=player[0x33c];if(deck>=4)return false;
        const auto* slot=player+0x340+deck*0x658;if(slot!=expected)return false;
        const auto mode=room[5];const bool decks=mode==6||mode==7;
        const auto* entry=source+(decks?0x19e+deck*0x99:0x49);
        out->room=room;out->source=source;out->player=player;out->slot=slot;
        out->owner=Word(player);out->root=Word(slot);out->deck=deck;out->mode=mode;
        out->active=Word(slot+0x60);out->hp=Word(slot+0x34);out->target=target;out->serial=Serial(entry+8);
        if(!out->owner||(out->owner&0x80000000u)||out->owner!=Word(source+0x429)||
           !Unit(out->root)||out->root!=Word(entry)||!out->serial||!out->hp||out->active>1)return false;
        for(unsigned i=0;i<2;++i) {
            const auto* bank=slot+0x64+i*0x2f0;
            const auto* definition=*reinterpret_cast<const std::uint8_t*const*>(bank+8);
            if(!definition)return false;
            out->definitions[i]=definition;out->forms[i]=Word(definition);out->targets[i]=Word(definition+0xc);
            if(!Unit(out->forms[i])||out->forms[i]!=Word(bank)||
               (out->targets[i]&&!Unit(out->targets[i])))return false;
        }
        out->current=out->forms[out->active];return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
bool ReadNativeTransformContext(const void* room,std::uint32_t index,const void* expected,std::uint32_t target,
                                NativeTransformContext& output) noexcept {
    NativeTransformContext context;
    if(!Read(room,index,expected,target,&context))return false;
    output=context;return true;
}
bool MatchesCapture(const NativeTransformContext& context,const HumanUnitCaptureSnapshot& snapshot) noexcept {
    return context.room&&context.source&&context.player&&context.slot&&snapshot.roomEpoch&&snapshot.generation&&
        reinterpret_cast<std::uintptr_t>(context.room)==snapshot.room&&
        reinterpret_cast<std::uintptr_t>(context.player)==snapshot.player&&
        context.owner==snapshot.unit.owner&&context.root==snapshot.unit.root&&context.deck==snapshot.unit.deck&&
        context.serial==snapshot.unit.serial&&context.mode==snapshot.unit.composition.mode;
}
} // namespace sdgo::unit_forms
