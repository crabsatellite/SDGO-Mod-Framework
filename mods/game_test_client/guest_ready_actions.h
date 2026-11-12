#pragma once
#include <windows.h>
#include <cstdint>
#include "native_actions.h"

// The 3D Main Ready business queues the local player's Ready state. That
// player's original sender emits 05A6 and its original reply consumer applies
// 0999 through CBaseRoomData. No legacy CWaitRoom layout or UI input is used.
template<class NativeT=NativeActions>
class GuestReadyActionsT {
public:
    enum Result:uint32_t {None=0,Pending=1,Rejected=2,Accepted=3};
    struct Observation {
        uint32_t sequence=0,result=0,native_app_state=0,room_id=0;
        uint16_t reply_opcode=0;
        uint8_t local_slot=0,ready_flag=0,reply_detail=0,server_reply_observed=0;
        uint8_t original_dispatch_completed=0,post_dispatch_model_ready=0,native_business_ready=0,reserved=0;
    };
    bool Bind(const SdgoApi* api,SdgoMod* mod,NativeT* native) {
        api_=api;mod_=mod;native_=native;
        if(!api||!api->Symbol||!api->HookCode||!mod||!native)return false;
        app_=Symbol<AppFn>("game.app");master_=Symbol<MasterFn>("flow.is_room_master");
        missionMaster_=Symbol<MasterFn>("flow.mission_is_room_master");
        mainVtable_=Symbol<void*>("flow.normal_3d_main_state_vtable");
        missionMainVtable_=Symbol<void*>("flow.mission_3d_main_vtable");
        ownerVtable_=Symbol<void*>("flow.three_d_wait_room_vtable");
        playerReadyVtable_=Symbol<void*>("flow.guest_ready_command_vtable");
        roomInfo_=Symbol<RoomInfoFn>("flow.mission_room_info");
        localSlot_=Symbol<LocalSlotFn>("flow.guest_ready_local_slot");
        modelReady_=Symbol<ModelReadyFn>("flow.guest_ready_model_flag");
        business_=Symbol<BusinessFn>("flow.guest_ready_main_business");
        auto send=Symbol<BusinessFn>("flow.guest_ready_player_send");
        auto reply=Symbol<ReplyFn>("flow.guest_ready_player_reply");
        managers_[0]=Symbol<void*>("flow.normal_3d_manager_vtable");
        managers_[1]=Symbol<void*>("flow.ai_3d_manager_vtable");
        managers_[2]=Symbol<void*>("flow.battleship_3d_manager_vtable");
        managers_[3]=Symbol<void*>("flow.mission_3d_manager_vtable");
        if(!app_||!master_||!mainVtable_||!missionMainVtable_||!ownerVtable_||!playerReadyVtable_||!roomInfo_||
           !localSlot_||!modelReady_||!business_||!send||!reply||!managers_[0]||!managers_[1]||!managers_[2]||!managers_[3])return false;
        __try {if(Read<BusinessFn>(static_cast<uint8_t*>(playerReadyVtable_)+8)!=send)return false;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        if(active_&&active_!=this)return false;
        active_=this;
        if(!api->HookCode(mod,"flow.guest_ready_player_send",reinterpret_cast<void*>(&OnSend),reinterpret_cast<void**>(&send_))) {
            active_=nullptr;return false;
        }
        passiveSend_=send_;
        if(!api->HookCode(mod,"flow.guest_ready_player_reply",reinterpret_cast<void*>(&OnReply),reinterpret_cast<void**>(&reply_))) {
            active_=nullptr;return false;
        }
        passiveReply_=reply_;bound_=true;return true;
    }
    bool Ready(uint32_t expectedRoom) {
        if(!bound_||!expectedRoom||State().result==Pending)return false;
        __try {
            Context current{};
            if(!Resolve(current,expectedRoom)||current.ready(current.model,static_cast<int>(current.slot))!=0)return false;
            AcquireSRWLockExclusive(&lock_);
            expected_=current;command_=nullptr;
            observed_={};observed_.result=Pending;observed_.room_id=expectedRoom;
            observed_.native_app_state=18;observed_.local_slot=static_cast<uint8_t>(current.slot);
            ReleaseSRWLockExclusive(&lock_);
            // 00730F90 is Main's ordinary 0BE2 action, including its load gates.
            // It queues the real player state; send/ACK happen on later ticks.
            business_(current.main);return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){Reject();return false;}
    }
    Observation State() const {
        AcquireSRWLockShared(&lock_);const auto copy=observed_;ReleaseSRWLockShared(&lock_);return copy;
    }
    void Reset() {
        AcquireSRWLockExclusive(&lock_);observed_={};expected_={};command_=nullptr;ReleaseSRWLockExclusive(&lock_);
        InterlockedExchange(&stage_,0);InterlockedExchange(&flags_,0);
    }
    uint32_t Stage()const{return static_cast<uint32_t>(InterlockedCompareExchange(&stage_,0,0));}
    uint32_t Flags()const{return static_cast<uint32_t>(InterlockedCompareExchange(&flags_,0,0));}
private:
    using AppFn=uint8_t*(__cdecl*)();
    using MasterFn=bool(__stdcall*)(void*);
    using RoomInfoFn=uint8_t*(__thiscall*)(uint8_t*);
    using LocalSlotFn=int(__thiscall*)(uint8_t*);
    using ModelReadyFn=uint8_t(__thiscall*)(uint8_t*,int);
    using BusinessFn=void(__thiscall*)(void*);
    using ReplyFn=void(__thiscall*)(void*,const uint8_t*);
    struct Context {uint8_t *main=nullptr,*context=nullptr,*model=nullptr;uint32_t room=0,slot=0;ModelReadyFn ready=nullptr;};
    template<class T>T Symbol(const char* name){return reinterpret_cast<T>(api_->Symbol(mod_,name));}
    template<class T>static T Read(const void* address){return *static_cast<const T*>(address);}
    bool Resolve(Context& out,uint32_t room) const {
        InterlockedExchange(&stage_,1);InterlockedExchange(&flags_,0);
        auto* app=app_();if(!app||Read<uint32_t>(app+0x4A0)!=18)return false;
        InterlockedExchange(&stage_,2);
        auto* owner=Read<uint8_t*>(app+0x4A4);
        if(!owner||Read<void*>(owner)!=ownerVtable_)return false;
        auto* manager=Read<uint8_t*>(owner+4);if(!manager)return false;
        auto* kind=Read<void*>(manager);
        if(kind!=managers_[0]&&kind!=managers_[1]&&kind!=managers_[2]&&kind!=managers_[3])return false;
        const void* expectedMain=kind==managers_[3]?missionMainVtable_:mainVtable_;
        if(kind==managers_[3]&&!missionMaster_)return false;
        InterlockedExchange(&stage_,3);
        auto** states=Read<uint8_t**>(manager+0x0C);auto* main=states?states[0]:nullptr;
        auto* context=manager+0x24;
        if(!main||Read<void*>(main)!=expectedMain||Read<uint32_t>(main+8)!=2||
           Read<uint8_t*>(main+0x0C)!=context||Read<uint8_t*>(main+0x10)!=manager||
           !context[2]||context[0x24]==1||context[0x25]==1)return false;
        InterlockedExchange(&flags_,1);InterlockedExchange(&stage_,4);
        auto* model=Read<uint8_t*>(context+0x14);auto* vtable=model?Read<uint8_t*>(model):nullptr;
        if(!model||!vtable||Read<RoomInfoFn>(vtable+0x1C)!=roomInfo_)return false;
        const auto ready=Read<ModelReadyFn>(vtable+0x28);
        if(!ready||(kind!=managers_[3]&&ready!=modelReady_))return false;
        auto* info=roomInfo_(model);
        if(!info||Read<uint32_t>(info)!=room||(kind==managers_[3]?missionMaster_:master_)(info))return false;
        InterlockedExchange(&flags_,7);InterlockedExchange(&stage_,5);
        const int slot=localSlot_(model);if(slot<0||slot>=12)return false;
        if(ready(model,slot)>1)return false;
        out={main,context,model,room,static_cast<uint32_t>(slot),ready};
        InterlockedExchange(&flags_,15);InterlockedExchange(&stage_,6);return true;
    }
    bool Matches(void* self,const Context& expected) const {
        if(!self||Read<void*>(self)!=playerReadyVtable_||Read<uint32_t>(static_cast<uint8_t*>(self)+4)!=4)return false;
        auto* player=Read<uint8_t*>(static_cast<uint8_t*>(self)+0x0C);
        if(!player||Read<uint32_t>(player)!=expected.slot||Read<uint8_t*>(player+8)!=expected.context)return false;
        Context current{};
        return Resolve(current,expected.room)&&current.main==expected.main&&current.context==expected.context&&
               current.model==expected.model&&current.slot==expected.slot&&current.ready==expected.ready;
    }
    bool PendingContext(Context& copy,void*& command) const {
        AcquireSRWLockShared(&lock_);const bool pending=observed_.result==Pending;
        copy=expected_;command=command_;ReleaseSRWLockShared(&lock_);return pending;
    }
    void Reject() {
        AcquireSRWLockExclusive(&lock_);observed_.result=Rejected;++observed_.sequence;ReleaseSRWLockExclusive(&lock_);
    }
    static void __fastcall OnSend(void* self,void*) {
        auto* active=active_;
        if(!active){if(passiveSend_)passiveSend_(self);return;}
        Context expected{};void* command=nullptr;bool owned=false;
        __try {owned=active->PendingContext(expected,command)&&active->Matches(self,expected);}
        __except(EXCEPTION_EXECUTE_HANDLER){owned=false;}
        const auto before=active->native_->TransportState();active->send_(self);
        const auto after=active->native_->TransportState();
        if(!owned)return;
        if(after.send_sequence==before.send_sequence||after.opcode!=0x05A6||after.packet_size!=8||after.result==0){active->Reject();return;}
        AcquireSRWLockExclusive(&active->lock_);
        if(active->observed_.result==Pending&&active->expected_.model==expected.model) {
            active->command_=self;active->observed_.native_business_ready=1;++active->observed_.sequence;
        }
        ReleaseSRWLockExclusive(&active->lock_);
    }
    static void __fastcall OnReply(void* self,void*,const uint8_t* packet) {
        auto* active=active_;
        if(!active){if(passiveReply_)passiveReply_(self,packet);return;}
        Context expected{};void* command=nullptr;bool owned=false;uint8_t slot=0,flag=0,detail=0;
        __try {
            owned=packet&&active->PendingContext(expected,command)&&command==self&&
                active->State().native_business_ready&&active->Matches(self,expected);
            if(owned){slot=packet[6];flag=packet[7];detail=packet[8];owned=slot==expected.slot;}
        } __except(EXCEPTION_EXECUTE_HANDLER){owned=false;}
        active->reply_(self,packet);
        if(!owned)return;
        bool same=false,ready=false;
        __try {same=active->Matches(self,expected);ready=same&&expected.ready(expected.model,static_cast<int>(expected.slot))==1;}
        __except(EXCEPTION_EXECUTE_HANDLER){same=false;ready=false;}
        AcquireSRWLockExclusive(&active->lock_);
        if(active->observed_.result==Pending&&active->command_==self) {
            auto& s=active->observed_;s.reply_opcode=0x0999;s.ready_flag=flag;s.reply_detail=detail;
            s.server_reply_observed=s.original_dispatch_completed=1;s.post_dispatch_model_ready=ready?1:0;
            s.result=same&&flag==1&&ready?Accepted:Rejected;++s.sequence;
        }
        ReleaseSRWLockExclusive(&active->lock_);
    }
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;NativeT* native_=nullptr;
    AppFn app_=nullptr;MasterFn master_=nullptr,missionMaster_=nullptr;RoomInfoFn roomInfo_=nullptr;LocalSlotFn localSlot_=nullptr;ModelReadyFn modelReady_=nullptr;
    BusinessFn business_=nullptr,send_=nullptr;ReplyFn reply_=nullptr;
    void *mainVtable_=nullptr,*missionMainVtable_=nullptr,*ownerVtable_=nullptr,*playerReadyVtable_=nullptr,*managers_[4]{};
    bool bound_=false;mutable SRWLOCK lock_=SRWLOCK_INIT;Observation observed_{};Context expected_{};void* command_=nullptr;
    mutable volatile LONG stage_=0,flags_=0;
    inline static GuestReadyActionsT* active_=nullptr;
    inline static BusinessFn passiveSend_=nullptr;
    inline static ReplyFn passiveReply_=nullptr;
};
using GuestReadyActions=GuestReadyActionsT<>;
