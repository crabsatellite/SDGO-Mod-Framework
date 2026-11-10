#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include <cassert>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <xmmintrin.h>
#include "sdgo/sdgo_api.h"
#include "sdgo/newcomer_protection_observer.h"
namespace {
void* ready=nullptr;void* hit=nullptr;void* partHit=nullptr;std::wstring directory;
__declspec(naked) void ReturnNative(){
    __asm {
        mov eax,1
        ret
    }
}
__declspec(naked) int Invoke(void*,void*){
    __asm {
        push ebp
        mov eax,[esp+8]
        mov ebp,[esp+12]
        call eax
        pop ebp
        ret
    }
}
int Hook(SdgoMod*,const char* name,void* entry,void** original){
    if(std::strcmp(name,"newcomer_protection.human_ready")==0)ready=entry;
    else if(std::strcmp(name,"newcomer_protection.normal_hit")==0)hit=entry;
    else if(std::strcmp(name,"newcomer_protection.part_hit")==0)partHit=entry;
    else return 0;
    *original=reinterpret_cast<void*>(&ReturnNative);return 1;
}
const wchar_t* Directory(SdgoMod*){return directory.c_str();}
void Log(SdgoMod*,int,const char*,...){}
template<class T> void Put(void* base,std::ptrdiff_t at,T value){std::memcpy(static_cast<std::uint8_t*>(base)+at,&value,sizeof(value));}
template<class T> T Get(void* base,std::ptrdiff_t at){T value{};std::memcpy(&value,static_cast<std::uint8_t*>(base)+at,sizeof(value));return value;}
}
int main(int argc,char** argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    assert(argc==2);
    const auto temp=std::filesystem::temp_directory_path()/std::filesystem::path(L"sdgo-newcomer-fixture-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(temp);directory=temp.wstring();
    const auto config=(temp/L"config.ini").wstring();
    assert(WritePrivateProfileStringW(L"settings",L"Enabled",L"1",config.c_str()));
    assert(WritePrivateProfileStringW(L"settings",L"ProtectedAccounts",L"7",config.c_str()));
    auto library=LoadLibraryW(std::filesystem::path(argv[1]).c_str());assert(library);
    using Entry=int(*)(const SdgoApi*,SdgoMod*);
    auto entry=reinterpret_cast<Entry>(GetProcAddress(library,"SdgoModLoad"));assert(entry);
    SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.host_key="BattleServer-548A891D-0046C000";
    api.host_base=reinterpret_cast<void*>(0x400000);
    api.HookSite=&Hook;api.ModDirectory=&Directory;api.Log=&Log;
    assert(entry(&api,reinterpret_cast<SdgoMod*>(1))&&ready&&hit&&partHit);
    std::vector<std::uint8_t> room(0x38000);
    auto* source=room.data()+0x533+0x48e;auto* player=room.data()+0x3ce4+0x1f54;
    auto* unit=player+0x340;
    room[5]=1;Put(source,0x429,std::uint32_t{7});Put(source,0x49,std::uint32_t{123});Put(player,0,std::uint32_t{7});
    Put(unit,0,std::uint32_t{123});Put(unit,4,std::uint32_t{10001});Put(unit,0x34,std::uint32_t{3999});
    Put(unit,0xe,std::uint16_t{1234});Put(unit,0x12,std::uint32_t{0x3f800000});
    std::array<std::uint8_t,0x1000> storage{};auto* frame=storage.data()+0x800;
    Put(frame,8,room.data());Put(frame,12,source);Put(frame,16,player);Put(frame,24,std::uint8_t{1});
    const auto csr=_mm_getcsr();SetLastError(73);assert(Invoke(ready,frame)==1);assert(GetLastError()==73&&_mm_getcsr()==csr);
    assert(Get<std::uint32_t>(unit,4)==20002&&Get<std::uint32_t>(unit,0x34)==7998);
    assert(Get<std::uint16_t>(unit,0xe)==1234&&Get<std::uint32_t>(unit,0x12)==0x3f800000);
    std::array<std::uint8_t,0x60> weapon{};Put(weapon.data(),0,std::uint32_t{12345});
    Put(frame,-0x480,room.data());Put(frame,0xc,std::uint32_t{1});Put(frame,0x10,player);Put(frame,0x14,weapon.data());
    Put(frame,0x1c,std::uint32_t{17});Put(frame,0x20,std::uint32_t{2000});Put(frame,0x30,std::uint32_t{13000});Put(frame,0x38,std::uint32_t{0});Put(frame,-4,std::uint32_t{2000});
    SetLastError(91);assert(Invoke(hit,frame)==1);assert(GetLastError()==91&&_mm_getcsr()==csr);
    assert(Get<std::uint32_t>(frame,0x20)==4000&&Get<std::uint32_t>(frame,0x30)==13000&&Get<std::uint32_t>(frame,-4)==2000);
    for(auto type:{1u,2u,3u,4u}){
        Put(frame,0x38,type);Put(frame,0x20,std::uint32_t{2000});Put(frame,0x30,std::uint32_t{19000});
        assert(Invoke(hit,frame)==1);assert(Get<std::uint32_t>(frame,0x20)==2000&&Get<std::uint32_t>(frame,0x30)==19000);
    }
    // Mission parts inherit the attack type only from their verified native caller.
    std::array<std::uint8_t,0x1000> partStorage{};auto* part=partStorage.data()+0x800;
    Put(frame,-0x15c,room.data());Put(frame,0x34,std::uint32_t{0});
    Put(part,0,frame);Put(part,4,std::uint32_t{0x4a0b79});Put(part,-0x14,room.data());
    Put(part,8,std::uint32_t{1});Put(part,0xc,player);Put(part,0x10,weapon.data());
    Put(part,0x18,std::uint32_t{17});Put(part,0x1c,std::uint32_t{2000});Put(part,0x2c,std::uint32_t{13000});
    assert(Invoke(partHit,part)==1&&Get<std::uint32_t>(part,0x1c)==4000&&Get<std::uint32_t>(part,0x2c)==13000);
    Put(frame,0x34,std::uint32_t{1});Put(part,0x1c,std::uint32_t{2000});
    assert(Invoke(partHit,part)==1&&Get<std::uint32_t>(part,0x1c)==2000);
    Put(frame,0x38,std::uint32_t{0});Put(player,0,std::uint32_t{17});Put(source,0x429,std::uint32_t{17});
    assert(Invoke(hit,frame)==1&&Get<std::uint32_t>(frame,0x20)==2000);
    auto recent=reinterpret_cast<SdgoNewcomerProtectionRecentV1Fn>(GetProcAddress(library,SDGO_NEWCOMER_PROTECTION_RECENT_EXPORT));
    auto hits=reinterpret_cast<SdgoNewcomerProtectionHitsV1Fn>(GetProcAddress(library,SDGO_NEWCOMER_PROTECTION_HITS_EXPORT));
    SdgoNewcomerProtectionReceiptV1 receipt{};SdgoNewcomerProtectionHitV1 damage{};
    assert(recent&&hits&&recent(&receipt,1)==1&&hits(&damage,1)==1);
    assert(receipt.maximum_after==20002&&damage.proposed_after==4000);
    // Native deck records are independent, including a zero-current-HP unit.
    room[5]=6;Put(frame,8,room.data());Put(frame,12,source);Put(frame,16,player);Put(frame,24,std::uint8_t{6});
    Put(source,0x429,std::uint32_t{7});Put(player,0,std::uint32_t{7});player[0x33c]=2;
    for(unsigned i=0;i<4;++i){
        auto* deck=player+0x340+i*0x658;
        Put(source,0x19e+i*0x99,std::uint32_t{123+i});Put(deck,0,std::uint32_t{123+i});
        Put(deck,4,std::uint32_t{10000+i});Put(deck,0x34,std::uint32_t{i==2?0u:5000+i});
        Put(deck,0x12,std::uint32_t{0x3fc00000+i});
    }
    assert(Invoke(ready,frame)==1);
    for(unsigned i=0;i<4;++i){
        auto* deck=player+0x340+i*0x658;
        assert(Get<std::uint32_t>(deck,4)==(10000+i)*2);
        assert(Get<std::uint32_t>(deck,0x34)==(i==2?0u:(5000+i)*2));
        assert(Get<std::uint32_t>(deck,0x12)==0x3fc00000+i);
    }
    Put(source,0x429,std::uint32_t{0x80000007});Put(player,0,std::uint32_t{0x80000007});
    assert(Invoke(ready,frame)==1&&Get<std::uint32_t>(unit,4)==20000);
    FreeLibrary(library);std::filesystem::remove_all(temp);
}
