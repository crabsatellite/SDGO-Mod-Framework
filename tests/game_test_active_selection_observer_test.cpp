#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../mods/game_test_client/active_selection_observer.h"
namespace {
uint8_t profile[0x800]{},inventory[0x40]{},brief[0x19]{};
uint8_t* profilePointer=profile;uint8_t* inventoryPointer=inventory;
int failures=0,calls=0;
template<class T> void Put(uint8_t* p,size_t at,T value){std::memcpy(p+at,&value,sizeof(value));}
void Check(bool value,const char* message){if(!value){++failures;std::printf("FAIL %s\n",message);}}
void __fastcall Brief(void* self,void*,void* out,uint16_t slot){
    Check(self==inventory && slot==4,"getter receives exact native inventory and ushort slot");
    ++calls;std::memcpy(out,brief,sizeof(brief));
}
void* Symbol(SdgoMod*,const char* name){
    if(!std::strcmp(name,"flow.profile_pointer"))return &profilePointer;
    if(!std::strcmp(name,"flow.unit_info_pointer"))return &inventoryPointer;
    if(!std::strcmp(name,"flow.unit_brief_by_slot"))return reinterpret_cast<void*>(&Brief);
    return nullptr;
}
}
int main(){
    SdgoApi api{};api.Symbol=&Symbol;ActiveSelectionObserver observer;
    Check(observer.Bind(&api,reinterpret_cast<SdgoMod*>(1)),"binding");
    Put(profile,0x138,uint32_t{101});Put(profile,0x1A9,uint32_t{22097});Put(profile,0x1AD,uint16_t{4});
    Put(profile,0x1AF,uint16_t{0xFFFF}); // flags adjacent to the native ushort slot
    Put(profile,0x1B1,UINT64_C(0x1234567800000042));
    Put(inventory,0x0C,int32_t{1200});inventory[0x20]=1;
    Put(brief,0,uint32_t{22097});Put(brief,4,UINT64_C(0x1234567800000042));Put(brief,12,uint16_t{4});
    const auto beforeProfile=profilePointer;uint8_t saved[sizeof(profile)];std::memcpy(saved,profile,sizeof(saved));
    ActiveSelectionObserver::Snapshot baseline{},after{};
    Check(observer.Read(101,&baseline) && baseline.inventory_matches && baseline.slot==4 &&
          baseline.uid==UINT64_C(0x1234567800000042),"64-bit identity and packed slot are read faithfully");
    Put(profile,0x1A9,uint32_t{15019});
    Check(observer.Read(101,&after) && !after.inventory_matches && !baseline.SameSelection(after),
          "a stale temporary profile is observable, not mislabeled unavailable or restored");
    Put(profile,0x1A9,uint32_t{22097});
    Check(observer.Read(101,&after) && after.inventory_matches && baseline.SameSelection(after),"genuine restored identity matches baseline");
    int before=calls;Check(!observer.Read(999,&after) && calls==before,"foreign profile is never consumed");
    inventory[0x20]=0;Check(!observer.Read(101,&after) && calls==before,"incomplete native inventory waits without getter");inventory[0x20]=1;
    profilePointer=nullptr;Check(!observer.Read(101,&after),"missing native account");profilePointer=beforeProfile;
    Put(inventory,0x0C,int32_t{2});Check(!observer.Read(101,&after),"out-of-capacity active slot");Put(inventory,0x0C,int32_t{1200});
    Check(!std::memcmp(saved,profile,sizeof(saved)),"observation never changes native account bytes");
    std::printf("active selection observer: %d failures\n",failures);return failures?1:0;
}
