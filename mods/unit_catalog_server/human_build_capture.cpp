#include <windows.h>
#include "human_build_capture.h"
#include "sdgo/battle_human_unit_observer_provider.h"
#include <algorithm>
#include <vector>

namespace unit_catalog_server {
namespace {
using BuildFn=int(__thiscall*)(void*,void*,void*,void*,std::uint8_t,std::uint8_t);
using ReleaseFn=void(__thiscall*)(void*);
using DispatchFn=int(__thiscall*)(void*,int,void*,std::uint32_t*,std::uint32_t*);
using SpecialFn=int(__thiscall*)(void*,void*,std::uintptr_t);
using ValidateFn=int(__thiscall*)(void*,void*,int);
struct Attempt { std::vector<const void*> rooms;Attempt* parent=nullptr; };
thread_local Attempt* attempt=nullptr;
}
struct HumanBuildCapture::Impl {
    const SdgoApi* api=nullptr;SdgoMod* mod=nullptr;
    std::unique_ptr<sdgo::unit_forms::HumanUnitCapture> capture;
    sdgo::unit_forms::HumanUnitObserverProvider local;
    SdgoBattleRegisterHumanUnitObserverV1Fn attach=nullptr;
    SdgoBattleReadHumanUnitObserverStatusV1Fn status=nullptr;
    bool external=false,prepared=false,attached=false;
    HANDLE stop=nullptr,worker=nullptr;
    BuildFn build=nullptr;ReleaseFn release=nullptr;DispatchFn dispatch=nullptr;
    SpecialFn special=nullptr;ValidateFn validate=nullptr;
    static Impl* owner;
    static DWORD WINAPI Drain(void* user) {
        auto& self=*static_cast<Impl*>(user);
        try { while(WaitForSingleObject(self.stop,10)==WAIT_TIMEOUT)self.capture->status(); }
        catch(...) { self.capture->disable(); }
        return 0;
    }
    void StopWorker() {
        if(stop)SetEvent(stop);
        if(worker){WaitForSingleObject(worker,INFINITE);CloseHandle(worker);worker=nullptr;}
        if(stop){CloseHandle(stop);stop=nullptr;}
    }
    static void __cdecl Observe(const SdgoBattleHumanUnitEventV1* event,void* user) {
        if(event)static_cast<Impl*>(user)->capture->observe(*event);
    }
    void Publish(std::uint32_t type,std::uint64_t sequence,const void* room,const void* source=nullptr,
                 const void* player=nullptr,std::uint8_t rule=0,std::uint8_t mode=0,int result=0) {
        // No diagnostic/native call may leak FP or LastError into the original.
        const DWORD error=GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
        if(local.Publish(type,sequence,room,source,player,rule,mode,result))capture->disable();
        _fxrstor(fp);SetLastError(error);
    }
    void Room(const void* room) {
        if(!attempt)return;
        auto& rooms=attempt->rooms;
        try {
            if(std::find(rooms.begin(),rooms.end(),room)==rooms.end()) {
                rooms.push_back(room);Publish(SDGO_BATTLE_HUMAN_UNIT_ROOM_INVALIDATED,local.Next(),room);
            }
        } catch(...) { Publish(SDGO_BATTLE_HUMAN_UNIT_ROOM_INVALIDATED,local.Next(),room);throw; }
    }
    template<class Call> int Start(Call&& call) {
        Attempt frame;frame.parent=attempt;attempt=&frame;
        try {
            const auto result=call();
            for(const auto* room:frame.rooms)Publish(result?SDGO_BATTLE_HUMAN_UNIT_ROOM_INVALIDATED:SDGO_BATTLE_HUMAN_UNIT_START_ACCEPTED,local.Next(),room);
            attempt=frame.parent;return result;
        } catch(...) {
            for(const auto* room:frame.rooms)Publish(SDGO_BATTLE_HUMAN_UNIT_ROOM_INVALIDATED,local.Next(),room);
            attempt=frame.parent;throw;
        }
    }
    static int __fastcall Build(void* proc,void*,void* room,void* source,void* player,std::uint8_t rule,std::uint8_t mode) {
        auto& self=*owner;self.Room(room);const auto sequence=self.local.Next();
        self.Publish(SDGO_BATTLE_HUMAN_UNIT_BEFORE,sequence,room,source,player,rule,mode);
        const auto result=self.build(proc,room,source,player,rule,mode);
        self.Publish(SDGO_BATTLE_HUMAN_UNIT_AFTER,sequence,room,source,player,rule,mode,result);return result;
    }
    static void __fastcall Release(void* room,void*) {
        auto& self=*owner;self.release(room);self.Publish(SDGO_BATTLE_HUMAN_UNIT_ROOM_RELEASED,self.local.Next(),room);
    }
    static int __fastcall Dispatch(void* proc,void*,int session,void* packet,std::uint32_t* key,std::uint32_t* index) {
        return owner->Start([&]{return owner->dispatch(proc,session,packet,key,index);});
    }
    static int __fastcall Special(void* proc,void*,void* source,std::uintptr_t extra) {
        return owner->Start([&]{return owner->special(proc,source,extra);});
    }
    static int __fastcall Validate(void* proc,void*,void* room,int slot) { return owner->validate(proc,room,slot); }
};
HumanBuildCapture::Impl* HumanBuildCapture::Impl::owner=nullptr;
HumanBuildCapture::HumanBuildCapture():impl_(std::make_unique<Impl>()){}
HumanBuildCapture::~HumanBuildCapture(){impl_->StopWorker();if(Impl::owner==impl_.get())Impl::owner=nullptr;}
bool HumanBuildCapture::Prepare(const SdgoApi* api,SdgoMod* mod,std::shared_ptr<const sdgo::unit_forms::GraphBindings> graphs,std::string& error) {
    auto& self=*impl_;error.clear();if(self.prepared||!api||!graphs){error="human capture prerequisites differ";return false;}
    self.api=api;self.mod=mod;self.capture=std::make_unique<sdgo::unit_forms::HumanUnitCapture>(std::move(graphs));
    if(const auto provider=GetModuleHandleW(L"room_random.dll")) {
        self.external=true;
        self.attach=reinterpret_cast<SdgoBattleRegisterHumanUnitObserverV1Fn>(GetProcAddress(provider,SDGO_BATTLE_HUMAN_UNIT_OBSERVER_EXPORT));
        self.status=reinterpret_cast<SdgoBattleReadHumanUnitObserverStatusV1Fn>(GetProcAddress(provider,SDGO_BATTLE_HUMAN_UNIT_STATUS_EXPORT));
        SdgoBattleHumanUnitObserverStatusV1 state{};state.size=sizeof(state);state.version=1;
        if(!self.attach||!self.status||!self.status(&state)||state.registration_state!=0) {
            error="loaded human lifecycle provider is incompatible or occupied";return false;
        }
    } else {
        if(Impl::owner&&Impl::owner!=&self){error="local human lifecycle provider already exists";return false;}
        Impl::owner=&self;
        if(!api->HookCode(mod,"battle.start_dispatch",reinterpret_cast<void*>(&Impl::Dispatch),reinterpret_cast<void**>(&self.dispatch))||
           !api->HookCode(mod,"battle.start_special",reinterpret_cast<void*>(&Impl::Special),reinterpret_cast<void**>(&self.special))||
           !api->HookCode(mod,"battle.validate_unit",reinterpret_cast<void*>(&Impl::Validate),reinterpret_cast<void**>(&self.validate))||
           !api->HookCode(mod,"battle.build_human_unit",reinterpret_cast<void*>(&Impl::Build),reinterpret_cast<void**>(&self.build))||
           !api->HookCode(mod,"battle.release_pilots",reinterpret_cast<void*>(&Impl::Release),reinterpret_cast<void**>(&self.release))) {
            error="local human lifecycle hook installation was refused";return false;
        }
    }
    self.prepared=true;return true;
}
bool HumanBuildCapture::Attach() {
    auto& self=*impl_;if(!self.prepared||self.attached)return false;
    self.stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!self.stop)return false;
    self.worker=CreateThread(nullptr,0,&Impl::Drain,&self,0,nullptr);
    if(!self.worker){self.StopWorker();return false;}
    const SdgoBattleHumanUnitObserverSinkV1 sink{sizeof(sink),1,&Impl::Observe,&self};
    self.attached=(self.external?self.attach(&sink):self.local.Register(&sink))!=0;
    if(!self.attached)self.StopWorker();return self.attached;
}
bool HumanBuildCapture::Healthy()const {
    auto& self=*impl_;if(!self.attached||!self.capture->status().healthy)return false;
    SdgoBattleHumanUnitObserverStatusV1 status{};status.size=sizeof(status);status.version=1;
    const bool read=self.external?self.status(&status)!=0:self.local.ReadStatus(&status)!=0;
    return read&&status.registration_state==2;
}
sdgo::unit_forms::HumanUnitCaptureStatus HumanBuildCapture::Status()const {
    return impl_->capture?impl_->capture->status():sdgo::unit_forms::HumanUnitCaptureStatus{};
}
bool HumanBuildCapture::Inspect(sdgo::unit_forms::HumanUnitCaptureSnapshot& output)const {
    sdgo::unit_forms::HumanUnitCaptureSnapshot snapshot;
    if(!Healthy()||!impl_->capture->inspect(snapshot)||!Healthy()||!impl_->capture->current(snapshot))return false;
    output=std::move(snapshot);return true;
}
bool HumanBuildCapture::Snapshot(const void* room,const void* player,std::uint32_t deck,std::uint32_t root,std::uint64_t uid,
                                sdgo::unit_forms::HumanUnitCaptureSnapshot& result)const {
    sdgo::unit_forms::HumanUnitCaptureSnapshot snapshot;
    if(!Healthy()||!impl_->capture->snapshot(room,player,deck,root,uid,snapshot)||!Healthy()||!impl_->capture->current(snapshot))return false;
    result=std::move(snapshot);return true;
}
bool HumanBuildCapture::Current(const sdgo::unit_forms::HumanUnitCaptureSnapshot& snapshot)const {
    return Healthy()&&impl_->capture->current(snapshot);
}
} // namespace unit_catalog_server
