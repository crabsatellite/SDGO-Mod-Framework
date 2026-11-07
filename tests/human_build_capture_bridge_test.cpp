#include <windows.h>
#include "sdgo/human_unit_capture.h"
#include "sdgo/sdgo_api.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <map>
#include <string>

namespace fixture {
bool present=false,missing=false;unsigned registration=0,attaches=0;
SdgoBattleHumanUnitObserverSinkV1 sink{};
std::map<std::string,void*> hooks;
HMODULE WINAPI Module(LPCWSTR) { return present?reinterpret_cast<HMODULE>(1):nullptr; }
int __cdecl Register(const SdgoBattleHumanUnitObserverSinkV1* value) { ++attaches;if(registration)return 0;sink=*value;registration=2;return 1; }
int __cdecl Status(SdgoBattleHumanUnitObserverStatusV1* status) { status->registration_state=registration;status->reserved=0;return 1; }
FARPROC WINAPI Export(HMODULE,LPCSTR name) {
    if(missing)return nullptr;
    return std::strcmp(name,SDGO_BATTLE_HUMAN_UNIT_OBSERVER_EXPORT)==0?reinterpret_cast<FARPROC>(&Register):reinterpret_cast<FARPROC>(&Status);
}
}
#define GetModuleHandleW fixture::Module
#define GetProcAddress fixture::Export
#include "../mods/unit_catalog_server/human_build_capture.cpp"
#undef GetModuleHandleW
#undef GetProcAddress

namespace fixture {
std::array<std::uint8_t,0x476> source{};
std::array<std::uint8_t,0x1f54> player{};
std::array<std::uint8_t,64> definition{};
char room=0;
bool failStart=false;
unsigned builds=0,releases=0,validations=0;
void Put(std::uint8_t* ptr,unsigned at,std::uint32_t value) { std::memcpy(ptr+at,&value,4); }
void Reset(bool provider) {
    present=provider;missing=false;registration=attaches=builds=releases=validations=0;hooks.clear();sink={};failStart=false;
    source.fill(0);player.fill(0);definition.fill(0);Put(source.data(),0x49,15001);std::uint64_t uid=123;std::memcpy(source.data()+0x51,&uid,8);
    Put(source.data(),0x429,99);Put(player.data(),0,99);player[0x1d93]=1;Put(definition.data(),0,15001);
}
int __fastcall OriginalBuild(void*,void*,void*,void*,void*,std::uint8_t,std::uint8_t) {
    ++builds;Put(player.data(),0x340,15001);Put(player.data(),0x3ac,reinterpret_cast<std::uintptr_t>(definition.data()));return 1;
}
void __fastcall OriginalRelease(void*,void*) { ++releases; }
int __fastcall OriginalValidate(void*,void*,void*,int) { ++validations;return 0; }
int __fastcall OriginalDispatch(void* proc,void*,int,void*,std::uint32_t*,std::uint32_t*) {
    using Validate=int(__fastcall*)(void*,void*,void*,int);
    using Build=int(__fastcall*)(void*,void*,void*,void*,void*,std::uint8_t,std::uint8_t);
    assert(reinterpret_cast<Validate>(hooks.at("battle.validate_unit"))(proc,nullptr,&room,0)==0);
    assert(reinterpret_cast<Build>(hooks.at("battle.build_human_unit"))(proc,nullptr,&room,source.data(),player.data(),2,1)==1);
    return failStart?1:0;
}
int __fastcall OriginalSpecial(void* proc,void*,void*,std::uintptr_t) { return OriginalDispatch(proc,nullptr,0,nullptr,nullptr,nullptr); }
int Hook(SdgoMod*,const char* name,void* detour,void** original) {
    hooks[name]=detour;
    if(std::strcmp(name,"battle.build_human_unit")==0)*original=reinterpret_cast<void*>(&OriginalBuild);
    else if(std::strcmp(name,"battle.release_pilots")==0)*original=reinterpret_cast<void*>(&OriginalRelease);
    else if(std::strcmp(name,"battle.validate_unit")==0)*original=reinterpret_cast<void*>(&OriginalValidate);
    else if(std::strcmp(name,"battle.start_dispatch")==0)*original=reinterpret_cast<void*>(&OriginalDispatch);
    else if(std::strcmp(name,"battle.start_special")==0)*original=reinterpret_cast<void*>(&OriginalSpecial);
    else return 0;
    return 1;
}
class Authority final:public sdgo::unit_forms::GraphAuthority {
public:
    std::string revision(const std::string&)const override{return std::string(64,'a');}
    bool entry(const std::string&,std::uint32_t root)const override{return root==15001;}
    bool unit(const std::string&,const sdgo::unit_forms::GraphNode& node)const override{return node.id==15001;}
};
std::shared_ptr<const sdgo::unit_forms::GraphBindings> Graph() {
    using namespace sdgo::unit_forms;Authority authority;std::string error;
    return GraphBindings::Build({{"Example",std::string(64,'a'),{15001},{{15001,0,NodeOrigin::owned,std::string(64,'b'),std::string(64,'c')}}}},authority,error);
}
void Emit(unsigned type,unsigned sequence,int result=0) {
    SdgoBattleHumanUnitEventV1 event{};event.size=sizeof(event);event.version=1;event.type=type;event.thread_id=GetCurrentThreadId();event.sequence=sequence;event.room=&room;
    if(type<3){event.source=source.data();event.player=player.data();event.rule=2;event.mode=1;event.native_result=result;}
    sink.callback(&event,sink.user);
}
}

int main() {
    using namespace fixture;using unit_catalog_server::HumanBuildCapture;
    SdgoApi api{};api.version=1;api.HookCode=&Hook;std::string error;
    Reset(true);
    { HumanBuildCapture bridge;assert(bridge.Prepare(&api,nullptr,Graph(),error)&&hooks.empty());assert(bridge.Attach()&&attaches==1&&bridge.Healthy());
      Emit(4,1);Emit(1,2);OriginalBuild(nullptr,nullptr,nullptr,nullptr,nullptr,2,1);Emit(2,2,1);Emit(5,3);
      sdgo::unit_forms::HumanUnitCaptureSnapshot snapshot;assert(bridge.Snapshot(&room,player.data(),0,15001,123,snapshot)&&bridge.Current(snapshot));
      Emit(4,4);assert(!bridge.Current(snapshot));
      registration=3;assert(!bridge.Snapshot(&room,player.data(),0,15001,123,snapshot));sink={}; }
    Reset(true);missing=true;
    { HumanBuildCapture bridge;assert(!bridge.Prepare(&api,nullptr,Graph(),error)&&hooks.empty()&&attaches==0); }
    Reset(true);registration=2;
    { HumanBuildCapture bridge;assert(!bridge.Prepare(&api,nullptr,Graph(),error)&&hooks.empty()&&attaches==0); }
    Reset(false);
    { HumanBuildCapture bridge;assert(bridge.Prepare(&api,nullptr,Graph(),error)&&hooks.size()==5&&bridge.Attach());
      using Dispatch=int(__fastcall*)(void*,void*,int,void*,std::uint32_t*,std::uint32_t*);
      using Release=void(__fastcall*)(void*,void*);
      const auto dispatch=reinterpret_cast<Dispatch>(hooks.at("battle.start_dispatch"));assert(dispatch(nullptr,nullptr,0,nullptr,nullptr,nullptr)==0);
      sdgo::unit_forms::HumanUnitCaptureSnapshot snapshot;assert(bridge.Snapshot(&room,player.data(),0,15001,123,snapshot)&&builds==1&&validations==1);
      failStart=true;assert(dispatch(nullptr,nullptr,0,nullptr,nullptr,nullptr)==1);assert(!bridge.Snapshot(&room,player.data(),0,15001,123,snapshot));
      reinterpret_cast<Release>(hooks.at("battle.release_pilots"))(&room,nullptr);assert(releases==1); }
    return 0;
}
