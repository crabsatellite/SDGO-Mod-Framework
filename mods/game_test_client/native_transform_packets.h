#pragma once
#include <windows.h>
#include <cstdio>
#include "sdgo/game_test.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/match_ai_events.h"
#include "unit_transform_packet_format.h"

namespace apsalus_probe {
// Subscribe to the production dispatcher's read-only packet fanout. match_ai
// owns the transform notification hook; a second hook would be rejected.
class TransformPackets {
    volatile LONG lock_=0,dropped_=0;
    sdgo_test::transforms::Packet queue_[32]{};
    unsigned count_=0;
    bool enabled_=false;
    void Capture(const unsigned char* raw,bool notice){
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
    static void __cdecl Packet(uint16_t opcode,const uint8_t* raw,void* user){
        const DWORD error=GetLastError();
        if(opcode==0x0a43||opcode==0x0a44)
            static_cast<TransformPackets*>(user)->Capture(raw,opcode==0x0a44);
        SetLastError(error);
    }
public:
    bool Install(const SdgoApi* api,SdgoMod* mod){
        enabled_=api->SettingInt(mod,"UnitDirectedTransforms",0)!=0;
        if(!enabled_)return true;
        const HMODULE provider=GetModuleHandleW(L"match_ai.dll");
        const auto attach=provider?reinterpret_cast<SdgoMatchAiRegisterEventsV1Fn>(GetProcAddress(provider,SDGO_MATCH_AI_EVENTS_EXPORT)):nullptr;
        if(!attach){api->Log(mod,SDGO_LOG_ERROR,"native transform observation requires the match_ai packet fanout");return false;}
        const SdgoMatchAiEventsV1 sink{sizeof(sink),1,nullptr,&Packet,this};
        return attach(&sink)!=0;
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
