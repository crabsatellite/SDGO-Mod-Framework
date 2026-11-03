#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../mods/unit_catalog/skill_presentation.h"

namespace {
using namespace sdgo::unit_skill_presentation;
unsigned checks=0,sets=0;
bool zeroPresent=false,secondPresent=true;
std::array<std::uint8_t,1132> skill{};
struct Widget {void** vtable;bool emptied;};
std::array<void*,35> table{};
Widget name{table.data(),false},description{table.data(),false};
std::array<std::uint8_t,0x170> icon{};
std::array<std::uint8_t,0x1b0> view{};
std::array<std::uint8_t,0x80> frame{};
sdgo::unit_catalog::Catalog owned;
SdgoApi sdk{};
void Check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);std::printf("PASS %s\n",message);}
template<class T> void Put(void* target,std::size_t offset,T value) {std::memcpy(static_cast<std::uint8_t*>(target)+offset,&value,sizeof(value));}
void Log(SdgoMod*,int,const char*,...) {SetLastError(0x9999);}
void* Data(){return &owned;}
bool __fastcall Find(void* grid,void*,const std::uint8_t** out,std::uint32_t id,int kind,char logging) {
    if(grid!=&owned || kind!=13 || logging)return false;
    *out=nullptr;
    if((!id && zeroPresent) || (id==79001 && secondPresent)){*out=skill.data();return true;}
    return false;
}
void __fastcall Empty(void* self,void*,const EmptyWideString* value) {
    auto* widget=static_cast<Widget*>(self);
    widget->emptied=value && value->length==0 && value->capacity==7 && value->data[0]==0;
    ++sets;
}
Registers Context(const std::uint8_t* record) {
    auto* base=frame.data()+0x70;Put(base-0x58,0,record);Put(base-4,0,std::int32_t{-1});
    Registers r{};r.ecx=reinterpret_cast<std::uint32_t>(record);r.esi=reinterpret_cast<std::uint32_t>(view.data());
    r.ebp=reinterpret_cast<std::uint32_t>(base);r.edx=0x55667788;return r;
}
}
int main(){
    try {
        sdk.size=sizeof(sdk);sdk.Log=Log;api=&sdk;mod=nullptr;catalog=&owned;data=Data;find=reinterpret_cast<GridFind>(&Find);
        sdgo::unit_catalog::Entry entry;entry.kind=2;entry.id=90001;entry.bytes.resize(4976);
        sdgo::unit_catalog::Put32(entry.bytes.data(),entry.id);sdgo::unit_catalog::Put32(entry.bytes.data()+579,79001);
        owned.records.push_back(entry);const auto originalRecord=owned.records[0].bytes;
        table[0x88/4]=reinterpret_cast<void*>(&Empty);Put(view.data(),0x19c,&name);Put(view.data(),0x1a8,&description);Put(view.data(),0x180,icon.data());icon[0x162]=1;
        auto r=Context(owned.records[0].bytes.data());const auto beforeRegisters=r;SetLastError(0x3344);
        Check(ContinueSecond(&r)==1,"a byte-identical owned record with an empty first slot continues to the existing second-slot block");
        Check(sets==2 && name.emptied && description.emptied && icon[0x162]==0,"native caption ABI clears only the missing first skill and hides its icon");
        Check(r.edx==beforeRegisters.ecx && r.ecx==beforeRegisters.ecx && r.esi==beforeRegisters.esi && r.ebp==beforeRegisters.ebp,
              "the continuation supplies exactly the native EDX unit pointer without changing the other captured registers");
        Check(GetLastError()==0x3344 && owned.records[0].bytes==originalRecord,"display repair preserves last-error and every unit/skill-slot byte");
        const auto oldSets=sets;
        auto altered=originalRecord;altered[105]=1;r=Context(altered.data());
        Check(ContinueSecond(&r)==0 && sets==oldSets && r.edx==0x55667788,"externally changed records retain their original renderer path");
        altered=originalRecord;sdgo::unit_catalog::Put32(altered.data(),90002);r=Context(altered.data());
        Check(ContinueSecond(&r)==0 && sets==oldSets,"an unowned native unit is never changed");
        r=Context(owned.records[0].bytes.data());Put(frame.data()+0x70-4,0,std::int32_t{5});
        Check(ContinueSecond(&r)==0 && sets==oldSets,"a different native exception-unwind state cannot enter the continuation");
        r=Context(owned.records[0].bytes.data());zeroPresent=true;
        Check(ContinueSecond(&r)==0 && sets==oldSets,"a real native kind-13 zero record keeps the original first-slot lookup");
        zeroPresent=false;secondPresent=false;
        Check(ContinueSecond(&r)==0 && sets==oldSets,"missing second-slot definitions are not manufactured by the renderer");
        secondPresent=true;sdgo::unit_catalog::Put32(owned.records[0].bytes.data()+575,79002);r=Context(owned.records[0].bytes.data());
        Check(ContinueSecond(&r)==0 && sets==oldSets,"normal two-skill records remain entirely native");
        std::printf("%u checks passed\n",checks);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
