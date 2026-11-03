// Offline Win32 ABI/state guards, not a substitute for native gameplay.
#include "../mods/game_test_client/guest_ready_actions.h"
#include <cstdio>
#include <cstring>
namespace {
static_assert(sizeof(void*)==4,"native Ready ABI is Win32");
int failures=0,sends=0,replies=0,businesses=0;bool host=false,queued=false,apply=true,rejectReplyHook=false;
alignas(4) uint8_t app[0x4A8]{},owner[8]{},manager[0x80]{},mainState[0x14]{},model[0x4B00]{},room[0x500]{},player[16]{},command[16]{};
void *ownerVtable[2]{},*managerVtable[3]{},*mainVtable[12]{},*modelVtable[32]{},*commandVtable[11]{};
uint8_t* states[1]{};
template<class T>void Put(void* p,size_t o,T value){memcpy(static_cast<uint8_t*>(p)+o,&value,sizeof(value));}
template<class T>T Get(const void* p,size_t o){T value{};memcpy(&value,static_cast<const uint8_t*>(p)+o,sizeof(value));return value;}
void Check(bool ok,const char* message){if(!ok){++failures;printf("FAIL %s\n",message);}}
struct Native {
    NativeActions::TransportObservation transport{};
    auto TransportState()const{return transport;}
} native;
using Driver=GuestReadyActionsT<Native>;
using SendHook=void(__fastcall*)(void*,void*);
using ReplyHook=void(__fastcall*)(void*,void*,const uint8_t*);
SendHook sendHook=nullptr;ReplyHook replyHook=nullptr;Driver* current=nullptr;
uint32_t duringOriginal=UINT32_MAX;uint16_t sendOpcode=0x05A6;
uint8_t* __cdecl App(){return app;}
bool __stdcall Master(void*){return host;}
uint8_t* __fastcall Info(uint8_t* p,void*){return p==model?room:nullptr;}
int __fastcall Slot(uint8_t* p,void*){return Get<int>(p,0x30);}
uint8_t __fastcall ReadyFlag(uint8_t* p,void*,int slot){
    if(slot<0||slot>=12||!Get<uint32_t>(p,0x1744+slot*0x48E))return 0;
    return p[0x174A+slot*0x48E];
}
void __fastcall Business(void* p,void*){Check(p==mainState,"Main Ready business receives the real Main object");++businesses;queued=true;}
void __fastcall Send(void*,void*){
    ++sends;++native.transport.send_sequence;native.transport.opcode=sendOpcode;native.transport.packet_size=8;native.transport.result=8;
}
void __fastcall Reply(void* p,void*,const uint8_t* packet){
    ++replies;duringOriginal=current?current->State().result:UINT32_MAX;
    const auto* context=Get<uint8_t*>(p,0x0C);
    if(apply&&packet&&Get<uint32_t>(context,0)==packet[6])model[0x174A+packet[6]*0x48E]=packet[7];
}
void* Symbol(SdgoMod*,const char* name){
    struct Entry{const char* name;void* value;};
    const Entry entries[]={
        {"game.app",reinterpret_cast<void*>(&App)},{"flow.is_room_master",reinterpret_cast<void*>(&Master)},
        {"flow.normal_3d_main_state_vtable",mainVtable},{"flow.three_d_wait_room_vtable",ownerVtable},
        {"flow.guest_ready_command_vtable",commandVtable},{"flow.mission_room_info",reinterpret_cast<void*>(&Info)},
        {"flow.guest_ready_local_slot",reinterpret_cast<void*>(&Slot)},{"flow.guest_ready_model_flag",reinterpret_cast<void*>(&ReadyFlag)},
        {"flow.guest_ready_main_business",reinterpret_cast<void*>(&Business)},{"flow.guest_ready_player_send",reinterpret_cast<void*>(&Send)},
        {"flow.guest_ready_player_reply",reinterpret_cast<void*>(&Reply)},
        {"flow.normal_3d_manager_vtable",managerVtable},{"flow.ai_3d_manager_vtable",managerVtable+1},
        {"flow.battleship_3d_manager_vtable",managerVtable+2}};
    for(const auto& entry:entries)if(!strcmp(name,entry.name))return entry.value;
    return nullptr;
}
int Hook(SdgoMod* mod,const char* name,void* detour,void** original){
    if(rejectReplyHook&&!strcmp(name,"flow.guest_ready_player_reply"))return 0;
    *original=Symbol(mod,name);if(!*original)return 0;
    if(!strcmp(name,"flow.guest_ready_player_send"))sendHook=reinterpret_cast<SendHook>(detour);
    if(!strcmp(name,"flow.guest_ready_player_reply"))replyHook=reinterpret_cast<ReplyHook>(detour);
    return 1;
}
void Reset(Driver& driver){
    driver.Reset();memset(app,0,sizeof(app));memset(manager,0,sizeof(manager));memset(mainState,0,sizeof(mainState));
    memset(model,0,sizeof(model));memset(room,0,sizeof(room));memset(player,0,sizeof(player));memset(command,0,sizeof(command));
    sends=replies=businesses=0;host=queued=false;apply=true;sendOpcode=0x05A6;native.transport={};
    Put<uint32_t>(app,0x4A0,18);Put(app,0x4A4,owner);Put(owner,0,ownerVtable);Put(owner,4,manager);
    Put(manager,0,managerVtable);Put(manager,0x0C,states);states[0]=mainState;
    Put(mainState,0,mainVtable);Put<uint32_t>(mainState,8,2);Put(mainState,0x0C,manager+0x24);Put(mainState,0x10,manager);
    manager[0x26]=1;Put(manager,0x38,model);Put(model,0,modelVtable);Put<int>(model,0x30,1);Put<uint32_t>(room,0,81);
    modelVtable[0x1C/4]=reinterpret_cast<void*>(&Info);modelVtable[0x28/4]=reinterpret_cast<void*>(&ReadyFlag);
    Put<uint32_t>(model,0x1744+0x48E,42);Put<uint32_t>(player,0,1);Put(player,8,manager+0x24);
    Put(command,0,commandVtable);Put<uint32_t>(command,4,4);Put(command,0x0C,player);current=&driver;
}
void TickSend(){Check(queued,"Main queued Ready before the player sender executes");queued=false;sendHook(command,nullptr);}
void Deliver(uint8_t slot=1,uint8_t flag=1){uint8_t packet[9]{};packet[6]=slot;packet[7]=flag;packet[8]=7;replyHook(command,nullptr,packet);}
}
int main(){
    SdgoApi api{};api.Symbol=&Symbol;api.HookCode=&Hook;
    commandVtable[2]=reinterpret_cast<void*>(&Send);
    Driver rejected;rejectReplyHook=true;
    Check(!rejected.Bind(&api,reinterpret_cast<SdgoMod*>(1),&native),"partial Ready bind fails closed");
    sendHook(command,nullptr);Check(sends==1,"a failed Ready bind leaves its sender pass-through");rejectReplyHook=false;
    Driver driver;Check(driver.Bind(&api,reinterpret_cast<SdgoMod*>(1),&native),"actual Main/player Ready contracts bind");
    Reset(driver);host=true;Check(!driver.Ready(81)&&businesses==0,"host cannot take guest Ready path");
    Reset(driver);Put<uint32_t>(mainState,8,1);Check(!driver.Ready(81),"Ready waits for native idle Main");
    Reset(driver);modelVtable[0x28/4]=nullptr;Check(!driver.Ready(81),"unrecognized model getter fails closed");
    Reset(driver);Put<int>(model,0x30,12);Check(!driver.Ready(81),"actual model slot bounds are checked");
    Reset(driver);model[0x174A+0x48E]=1;Check(!driver.Ready(81)&&businesses==0,"already-ready pilot is never toggled off");
    Reset(driver);Put<uint32_t>(manager,0x20,0xDEADBEEF);manager[0x27]=255;
    Check(driver.Ready(81)&&businesses==1&&!sends&&!driver.State().native_business_ready,"Main action only queues; unrelated manager words are not local-slot fields");
    Deliver();Check(driver.State().result==Driver::Pending,"reply and model alone cannot replace observed original request");
    Reset(driver);Check(driver.Ready(81),"native queue prepared");TickSend();
    Check(driver.State().native_business_ready&&driver.State().result==Driver::Pending,"original player sender and real transport precede acknowledgement");
    Deliver(2);Check(driver.State().result==Driver::Pending,"foreign slot acknowledgement is passed through without satisfying local Ready");
    Deliver();const auto accepted=driver.State();
    Check(duringOriginal==Driver::Pending&&accepted.result==Driver::Accepted&&accepted.reply_opcode==0x0999&&
          accepted.server_reply_observed&&accepted.original_dispatch_completed&&accepted.post_dispatch_model_ready,
          "matching Ready is published only after original reply consumer updates CBaseRoomData");
    Reset(driver);driver.Ready(81);sendOpcode=0x0621;TickSend();Check(driver.State().result==Driver::Rejected,"another outbound packet is not a Ready request");
    Reset(driver);driver.Ready(81);TickSend();apply=false;Deliver();Check(driver.State().result==Driver::Rejected,"successful reply without original model update fails");
    Reset(driver);driver.Ready(81);TickSend();Deliver(1,0);Check(driver.State().result==Driver::Rejected,"native unready reply is not Ready success");
    Reset(driver);driver.Ready(81);TickSend();Put<uint32_t>(room,0,82);Deliver();
    Check(replies==1&&driver.State().result!=Driver::Accepted,"changed native room cannot reuse a previous Ready acknowledgement");
    printf("Original 3D Ready offline: %d failures\n",failures);return failures?1:0;
}
