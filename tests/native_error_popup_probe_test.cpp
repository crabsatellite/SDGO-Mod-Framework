#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include "../mods/game_test_client/native_error_popup_probe.h"

namespace {
using Popup=uint32_t(__cdecl*)(uint16_t,int32_t,uint8_t);
Popup captured=nullptr;
unsigned calls=0,logs=0,failures=0;
uint16_t lastOpcode=0;
int32_t lastError=0;
uint8_t lastFlag=0;
DWORD originalEntry=0;
void Check(bool value,const char* reason){if(!value){++failures;std::printf("FAIL %s\n",reason);}}
uint32_t __cdecl Original(uint16_t opcode,int32_t error,uint8_t flag){
    ++calls;lastOpcode=opcode;lastError=error;lastFlag=flag;originalEntry=GetLastError();
    Check(logs==calls,"the observation is persisted before the modal call");
    SetLastError(987);return 0xA5100000u+calls;
}
void Log(SdgoMod*,int,const char*,...){++logs;SetLastError(555);}
int Hook(SdgoMod*,const char* name,void* detour,void** original){
    Check(std::strcmp(name,"test.native_error_popup")==0,"verified popup symbol selected");
    captured=reinterpret_cast<Popup>(detour);*original=reinterpret_cast<void*>(&Original);return 1;
}
}
int main(){
    SdgoApi api{};api.size=sizeof(api);api.Log=&Log;api.HookCode=&Hook;
    NativeErrorPopupProbe probe;
    Check(probe.Install(&api,reinterpret_cast<SdgoMod*>(1)),"observer installs");
    for(unsigned i=0;i<3;++i){
        SetLastError(42);
        const auto result=captured(0x0880,1,7);
        Check(result==0xA5100001u+i,"native uint32 result is returned unchanged");
        Check(GetLastError()==987,"native post-call LastError is returned unchanged");
        Check(originalEntry==42,"logging preserves native entry LastError");
    }
    Check(calls==3&&logs==3&&probe.Sequence()==3,"repeated identical native errors are never deduplicated");
    Check(lastOpcode==0x0880&&lastError==1&&lastFlag==7,"all three native arguments pass through unchanged");
    std::array<NativeErrorPopupProbe::Observation,4> events{};
    Check(probe.CopyAfter(0,events.data(),static_cast<uint32_t>(events.size()))==3,"every invocation is observable");
    for(unsigned i=0;i<3;++i){
        Check(events[i].sequence==i+1&&events[i].opcode==0x0880&&events[i].error==1&&events[i].flag==7,
              "retained events preserve native identity and order");
        Check(events[i].entry_last_error==42&&events[i].caller&&events[i].frame_count,
              "retained events include the original context and stack");
    }
    std::printf("NATIVE ERROR POPUP OBSERVER: %u failure(s)\n",failures);return failures?1:0;
}
