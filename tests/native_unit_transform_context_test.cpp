#include <windows.h>
#include "sdgo/native_unit_transform_context.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>
#include <array>
#include <cstring>
using namespace sdgo::unit_forms;
namespace {
void Put(void* at,std::uint32_t value) { std::memcpy(at,&value,4); }
struct Fixture {
    std::vector<std::uint8_t> room=std::vector<std::uint8_t>(0x32880);
    std::array<std::array<std::uint8_t,0x100>,2> definitions{};
    unsigned index=3,deck=2;
    std::uint64_t serial=0x1234567800000042ull;
    std::uint8_t* player() { return room.data()+0x3ce4+index*0x1f54; }
    std::uint8_t* source() { return room.data()+0x533+index*0x48e; }
    std::uint8_t* slot() { return player()+0x340+deck*0x658; }
    std::uint8_t* entry() { return source()+(room[5]==6||room[5]==7?0x19e+deck*0x99:0x49); }
    Fixture(unsigned mode=1) {
        room[5]=static_cast<std::uint8_t>(mode);Put(room.data()+0x21,1);
        Put(player(),7);Put(source()+0x429,7);player()[0x33c]=static_cast<std::uint8_t>(deck);
        Put(entry(),15001);std::memcpy(entry()+8,&serial,8);Put(slot(),15001);Put(slot()+0x34,99);Put(slot()+0x60,1);
        for(unsigned i=0;i<2;++i) {
            Put(definitions[i].data(),15001+i);Put(definitions[i].data()+0xc,15002-i);
            Put(slot()+0x64+i*0x2f0,15001+i);
            Put(slot()+0x6c+i*0x2f0,reinterpret_cast<std::uintptr_t>(definitions[i].data()));
        }
    }
    bool read(NativeTransformContext& output) { return ReadNativeTransformContext(room.data(),index,slot(),15001,output); }
    HumanUnitCaptureSnapshot captured() {
        HumanUnitCaptureSnapshot s;s.room=reinterpret_cast<std::uintptr_t>(room.data());s.player=reinterpret_cast<std::uintptr_t>(player());
        s.roomEpoch=1;s.generation=2;s.unit.root=15001;s.unit.owner=7;s.unit.deck=deck;s.unit.serial=serial;
        s.unit.definition=definitions[0].data();s.unit.composition.mode=room[5];return s;
    }
};
}
int main() {
    for(unsigned mode:{1,6,7}) {
        Fixture f(mode);const auto before=f.room;NativeTransformContext context;
        assert(f.read(context)&&f.room==before&&context.current==15002&&context.target==15001&&context.active==1);
        assert(context.mode==mode&&context.serial==f.serial&&context.definitions[0]==f.definitions[0].data());
        auto snapshot=f.captured();assert(MatchesCapture(context,snapshot));
        // A prologue need not remain cached; identity comes from owned source.
        Put(f.slot()+0x64,15003);Put(f.definitions[0].data(),15003);
        assert(f.read(context)&&MatchesCapture(context,snapshot));
        auto altered=snapshot;altered.unit.serial^=0x100000000ull;assert(!MatchesCapture(context,altered));
        altered=snapshot;++altered.unit.owner;assert(!MatchesCapture(context,altered));
        altered=snapshot;++altered.unit.root;assert(!MatchesCapture(context,altered));
        altered=snapshot;++altered.unit.deck;assert(!MatchesCapture(context,altered));
        altered=snapshot;altered.roomEpoch=0;assert(!MatchesCapture(context,altered));
        altered=snapshot;altered.unit.composition.mode=99;assert(!MatchesCapture(context,altered));
    }
    Fixture f;NativeTransformContext output;output.root=123;
    auto reject=[&] { assert(!f.read(output)&&output.root==123); };
    Put(f.source()+0x429,8);reject();Put(f.source()+0x429,7);
    Put(f.player(),0x80000007u);reject();Put(f.player(),7);
    Put(f.entry(),15002);reject();Put(f.entry(),15001);
    std::memset(f.entry()+8,0,8);reject();std::memcpy(f.entry()+8,&f.serial,8);
    Put(f.slot()+0x34,0);reject();Put(f.slot()+0x34,1);
    Put(f.slot()+0x60,2);reject();Put(f.slot()+0x60,1);
    Put(f.slot()+0x64,15009);reject();Put(f.slot()+0x64,15001);
    Put(f.slot()+0x6c,1);reject();Put(f.slot()+0x6c,reinterpret_cast<std::uintptr_t>(f.definitions[0].data()));
    Put(f.slot()+0x35c,0);reject();Put(f.slot()+0x35c,reinterpret_cast<std::uintptr_t>(f.definitions[1].data()));
    Put(f.definitions[0].data()+0xc,30001);reject();Put(f.definitions[0].data()+0xc,15002);
    assert(!ReadNativeTransformContext(f.room.data(),12,f.slot(),15001,output)&&output.root==123);
    assert(!ReadNativeTransformContext(f.room.data(),f.index,f.slot()+1,15001,output)&&output.root==123);
    assert(!ReadNativeTransformContext(f.room.data(),f.index,f.slot(),30001,output)&&output.root==123);
    assert(!ReadNativeTransformContext(reinterpret_cast<const void*>(1),0,f.slot(),15001,output)&&output.root==123);
    assert(f.read(output));
}
