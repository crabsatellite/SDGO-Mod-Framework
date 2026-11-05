#pragma once
#include <windows.h>
#include <cstdio>
#include "sdgo/game_test.h"
#include "sdgo/sdgo_api.h"
#include "unit_transform_packet_format.h"

namespace apsalus_probe {
// Observe the original reply/notification consumers without changing a packet,
// invoking a business action, or holding our short queue lock across a consumer.
class TransformPackets {
    using Handler=int(__thiscall*)(void*,unsigned char*);
    inline static TransformPackets* active_=nullptr;
    Handler reply_=nullptr,notice_=nullptr;
    volatile LONG lock_=0,dropped_=0;
    sdgo_test::transforms::Packet queue_[32]{};
    unsigned count_=0;
    bool enabled_=false;
    void Capture(unsigned char* raw,bool notice){
        sdgo_test::transforms::Packet record{};bool valid=false;
        __try {
            // Bounds are the minimum read by the original native consumers;
            // the packet remains owned and validated by their dispatcher.
            valid=notice?sdgo_test::transforms::DecodeNotice(raw,19,record):sdgo_test::transforms::DecodeReply(raw,8,record);
        } __except(EXCEPTION_EXECUTE_HANDLER) {return;}
        if(!valid)return;
        if(InterlockedCompareExchange(&lock_,1,0)!=0){InterlockedIncrement(&dropped_);return;}
        if(count_<32)queue_[count_++]=record;else InterlockedIncrement(&dropped_);
        InterlockedExchange(&lock_,0);
    }
    static int __fastcall Reply(void* self,void*,unsigned char* raw){
        const DWORD error=GetLastError();active_->Capture(raw,false);SetLastError(error);
        return active_->reply_(self,raw);
    }
    static int __fastcall Notice(void* self,void*,unsigned char* raw){
        const DWORD error=GetLastError();active_->Capture(raw,true);SetLastError(error);
        return active_->notice_(self,raw);
    }
public:
    bool Install(const SdgoApi* api,SdgoMod* mod){
        enabled_=api->SettingInt(mod,"UnitDirectedTransforms",0)!=0;
        if(!enabled_)return true;
        active_=this;
        return api->HookCode(mod,"test.unit_transform_reply",reinterpret_cast<void*>(&Reply),reinterpret_cast<void**>(&reply_))&&
            api->HookCode(mod,"test.unit_transform_notice",reinterpret_cast<void*>(&Notice),reinterpret_cast<void**>(&notice_));
    }
    void Observe(const SdgoGameTestContextV1* context){
        if(!enabled_||InterlockedCompareExchange(&lock_,1,0)!=0)return;
        sdgo_test::transforms::Packet local[32]{};const unsigned count=count_;
        for(unsigned i=0;i<count;++i)local[i]=queue_[i];count_=0;
        const LONG dropped=InterlockedExchange(&dropped_,0);InterlockedExchange(&lock_,0);
        for(unsigned i=0;i<count;++i){
            const auto& packet=local[i];char message[192];
            if(packet.kind==1)sprintf_s(message,"native transform reply opcode=%u error=%u",packet.opcode,packet.error);
            else sprintf_s(message,"native transform notice opcode=%u pilot=%u target=%u raw14=%u",packet.opcode,packet.pilot,packet.target,packet.raw14);
            context->Observe(message);
        }
        if(dropped){char message[96];sprintf_s(message,"native transform packet observations dropped=%ld",dropped);context->Observe(message);}
    }
};
inline TransformPackets packetObserver;
}
