#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include "sdgo/sdgo_api.h"
#include "sdgo/friend_unit_gift_protocol.h"
#include <cassert>
#include <cstring>
#include <iostream>
namespace fixture {
int master=1,rgbOn=1,decalOn=1,failRgb=0,failDecal=0,rgbLoads=0,decalLoads=0,aborts=0,registrations=0,dispatches=0;
unsigned first=0,last=0;
sdgo::friend_unit_gift::ExtensionHandlerFn registered=nullptr;
int Setting(SdgoMod*,const char* key,int fallback){return !std::strcmp(key,"Enabled")?master:!std::strcmp(key,"RgbEnabled")?rgbOn:!std::strcmp(key,"DecalsEnabled")?decalOn:fallback;}
void Log(SdgoMod*,int,const char*,...){}
std::int32_t __cdecl Handler(void*,std::uint32_t,std::uint16_t,const std::uint8_t*,std::uint32_t,void*,void*,std::uint8_t*,std::uint32_t,std::uint32_t*){++dispatches;return 7;}
int __cdecl Register(std::uint16_t a,std::uint16_t b,sdgo::friend_unit_gift::ExtensionHandlerFn h,void*){++registrations;first=a;last=b;registered=h;return 1;}
HMODULE WINAPI Module(LPCWSTR){return reinterpret_cast<HMODULE>(1);}
FARPROC WINAPI Proc(HMODULE,LPCSTR){return reinterpret_cast<FARPROC>(&Register);}
void Reset(){master=1;failRgb=failDecal=rgbLoads=decalLoads=aborts=registrations=dispatches=0;registered=nullptr;}
}
#ifdef APPEARANCE_SERVER
#define GetModuleHandleW fixture::Module
#define GetProcAddress fixture::Proc
#include "../mods/unit_appearance_server/unit_appearance_server.cpp"
#undef GetModuleHandleW
#undef GetProcAddress
ExtensionHandlerFn PrepareRgb(const SdgoApi*,SdgoMod*){++fixture::rgbLoads;return fixture::failRgb?nullptr:&fixture::Handler;}
ExtensionHandlerFn PrepareDecals(const SdgoApi*,SdgoMod*){++fixture::decalLoads;return fixture::failDecal?nullptr:&fixture::Handler;}
#else
#include "../mods/unit_appearance_client/unit_appearance_client.cpp"
int LoadRgb(const SdgoApi*,SdgoMod*){++fixture::rgbLoads;return !fixture::failRgb;}
int LoadDecals(const SdgoApi*,SdgoMod*){++fixture::decalLoads;return !fixture::failDecal;}
void AbortRgbLoad(){++fixture::aborts;}
void AbortDecalsLoad(){++fixture::aborts;}
#endif
int main(){
    using namespace fixture;SdgoApi api{};api.SettingInt=&Setting;api.Log=&Log;auto* mod=reinterpret_cast<SdgoMod*>(1);
    for(int r=0;r<2;++r)for(int d=0;d<2;++d){
        Reset();rgbOn=r;decalOn=d;
#ifdef APPEARANCE_SERVER
        rgb=decals=nullptr;
#endif
        assert(SdgoModLoad(&api,mod)==1);assert(rgbLoads==r&&decalLoads==d);
#ifdef APPEARANCE_SERVER
        assert(registrations==(r||d));
        if(r||d){
            assert(first==(d?0x1020u:0x1030u));assert(last==(r?0x1031u:0x1021u));
            std::uint32_t bytes=123;
            for(unsigned command:{0x1020u,0x1021u,0x1022u,0x102fu,0x1030u,0x1031u}){
                const bool active=(d&&(command==0x1020||command==0x1021))||(r&&(command==0x1030||command==0x1031));
                assert((registered(nullptr,1,static_cast<std::uint16_t>(command),nullptr,0,nullptr,nullptr,nullptr,0,&bytes)==7)==active);
                assert(bytes==0);
            }
        }
#endif
    }
    for(int failure=0;failure<2;++failure){Reset();rgbOn=decalOn=1;failRgb=failure==0;failDecal=failure==1;assert(!SdgoModLoad(&api,mod));
#ifdef APPEARANCE_SERVER
        assert(registrations==0); // No callback survives a failed preparation.
#else
        assert(aborts==2); // Both thread-message hooks are released before unload.
#endif
    }
    Reset();master=0;assert(!SdgoModLoad(&api,mod));assert(!rgbLoads&&!decalLoads&&!registrations);
    std::cout<<"Appearance switches, dispatch and failed-load cleanup passed\n";
}
