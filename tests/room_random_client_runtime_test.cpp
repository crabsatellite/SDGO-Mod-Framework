#include <windows.h>
#include <float.h>
#include <xmmintrin.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "../mods/room_random/loadout_sync.h"
#include "../mods/room_random_client/cache_sync.h"
#include "../mods/room_random_client/context.h"
#include "room_random_native_copy.h"

namespace {
int failures=0,startedModel=0,factoryCalls=0,fillCalls=0;
int startParserCalls=0,startParserConstructions=0;
uint32_t nativeNoticeRoute=0;
uint8_t nativeParserEpoch=0;
std::array<uint8_t,0x5DA8> profile{};
std::array<uint8_t,0x4000> room{};
std::array<uint8_t,0x4C0> app{};
std::array<uint8_t,0x10> owner{};
std::array<uint8_t,0x80> manager{},model{},ownerVtable{};
std::array<uint8_t,0x20> managerVtable{};
std::array<uint8_t,0xA0> modelVtable{};
std::array<uint8_t,0x20> startParser{},startParserOwner{};
std::array<uint8_t,0x4000> missionRoom{};
std::array<std::array<uint8_t,0x20>,12> userManagers{};
std::array<uint8_t*,12> userManagerPointers{};
std::array<std::array<uint8_t*,0x19>,12> userStateServices{};
std::array<std::vector<int>,12> userStates{};
std::array<void*,11> roomViewVtable{};
uint8_t* profilePointer=profile.data();
uint8_t* appPointer=app.data();
bool starting=true;
bool threeD=false;
bool asyncNativeQueues=false;
bool skipNativeRoomViewQueue=false;
bool autoAckRoomView=true;
bool rootStartQueued=false;
bool rootRanWithViewPending=false;
bool managerStateProbe=false;
bool roomViewStateProbe=false;
int rootStateRuns=0,roomViewRuns=0,roomViewRequests=0,managerTicks=0;
constexpr DWORD kManagerUpdateLastError=0x5A17;
constexpr DWORD kRoomViewLastError=0x5A18;
std::array<uint8_t,0xCD4> queuedStart{};
std::array<std::array<uint8_t,0x48E>,12> frozen{};
std::array<uint8_t,0x48E> lastConstruction{};
std::array<uint8_t,0x344> lastOutput{};
std::array<int,12> startedModels{};
std::unique_ptr<room_random_native_test::ClientCopies> nativeCopies;
template<class T> void Put(uint8_t* p,size_t n,T v){std::memcpy(p+n,&v,sizeof(v));}
struct RoomViewNode {
    RoomViewNode* next=nullptr;
    RoomViewNode* previous=nullptr;
    uint8_t* payload=nullptr;
};
struct RoomViewService {
    alignas(16) std::array<uint8_t,0x24> object{};
    RoomViewNode sentinel{};
    std::array<RoomViewNode,8> nodes{};
    std::array<std::array<uint8_t,2>,8> payloads{};
    size_t issued=0;
    int frees=0;
    void Reset() {
        object={};sentinel={};nodes={};payloads={};issued=0;frees=0;
        sentinel.next=&sentinel;sentinel.previous=&sentinel;
        Put<void*>(object.data(),0,roomViewVtable.data());
        Put<int>(object.data(),4,0x0C);
        Put<RoomViewNode*>(object.data(),0x14,&sentinel);
    }
    bool Enqueue(uint16_t view) {
        if(issued>=nodes.size())return false;
        auto& node=nodes[issued];auto& payload=payloads[issued++];
        Put<uint16_t>(payload.data(),0,view);
        node.payload=payload.data();node.next=&sentinel;
        node.previous=sentinel.previous;
        sentinel.previous->next=&node;sentinel.previous=&node;
        Put<uint32_t>(object.data(),0x18,
            room_random_client::Read<uint32_t>(object.data(),0x18)+1);
        return true;
    }
    bool Cleanup() {
        if(!room_random_client::Read<uint32_t>(object.data(),0x18) ||
           sentinel.next==&sentinel)return false;
        auto* node=sentinel.next;
        auto* owned=room_random_client::Read<uint8_t*>(object.data(),0x20);
        if(!owned || owned!=node->payload)return false;
        sentinel.next=node->next;node->next->previous=&sentinel;
        Put<uint32_t>(object.data(),0x18,
            room_random_client::Read<uint32_t>(object.data(),0x18)-1);
        Put<uint8_t*>(object.data(),0x20,nullptr);
        Put<int>(object.data(),8,0);
        ++frees;return true;
    }
};
std::array<RoomViewService,12> roomViewServices{};
void Check(bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;}
unsigned short ReadX87Control() {
    unsigned short control=0;
    __asm fnstcw control
    return control;
}
void WriteX87Control(unsigned short control) {
    __asm fldcw control
}
__declspec(noinline) void* __cdecl NativeFactory(int type,uint32_t modelId) {
    ++factoryCalls;return type==15 ? reinterpret_cast<void*>(static_cast<uintptr_t>(modelId)+0x1000) : nullptr;
}
__declspec(noinline) void __stdcall NativeFill(uint8_t* output,
    const uint8_t* source,const uint8_t*,uint8_t deck) {
    ++fillCalls;std::memcpy(lastConstruction.data(),source,lastConstruction.size());
    for(size_t i=0;i<(deck ? 4u : 1u);++i) {
        auto* dst=output+0xB4*i;
        if(nativeCopies)nativeCopies->Project(dst,source+0x99*i,deck);
        else {
            const auto* unit=source+(deck ? 0x19E : 0x49)+0x99*i;
            Put<uint32_t>(dst,0x44,room_random_client::Read<uint32_t>(unit,0));
            std::memcpy(dst+0x48,unit+0x10,4);std::memcpy(dst+0x4C,unit+0x14,8);
        }
    }
}
__declspec(naked) void __stdcall StaticFillWithUnrelatedEcx(uint8_t*,const uint8_t*,const uint8_t*,uint8_t) {
    __asm {
        push ebp
        mov ebp,esp
        push dword ptr [ebp+14h]
        push dword ptr [ebp+10h]
        push dword ptr [ebp+0Ch]
        push dword ptr [ebp+08h]
        mov ecx,12345678h
        call NativeFill
        pop ebp
        ret 10h
    }
}
void Construct(const uint8_t* entry,const uint8_t* source,uint8_t deck) {
    std::array<uint8_t,0x344> output{};
    StaticFillWithUnrelatedEcx(output.data(),source,entry,deck);lastOutput=output;
    startedModel=static_cast<int>(room_random_client::Read<uint32_t>(output.data(),0x44));
    NativeFactory(15,static_cast<uint32_t>(startedModel));
}
void SetTitle(const char* title) {
    std::memset(room.data()+0x7A,0,20);
    const auto length=(std::min)(std::strlen(title),size_t{20});
    room[0x79]=static_cast<uint8_t>(length);
    std::memcpy(room.data()+0x7A,title,length);
}
bool HasQueuedState(int state) {
    for(const auto& queue:userStates)
        if(std::find(queue.begin(),queue.end(),state)!=queue.end())return true;
    return false;
}
void ExecuteQueuedStart(const uint8_t* packet) {
    if(threeD) {
        Put<uint32_t>(app.data(),0x4A0,22);
        const auto deck=room[5]==6 || room[5]==7 ? uint8_t{1} : uint8_t{0};
        for(size_t slot=0;slot<12;++slot) {
            const auto* entry=packet+0x20+slot*0x10F;
            const auto pilot=room_random_client::Read<uint32_t>(entry,0);
            if(!pilot || (pilot&0x80000000u))continue;
            Construct(entry,frozen[slot].data(),deck);
            startedModels[slot]=startedModel;++startParserConstructions;
        }
    } else {
        startedModel=static_cast<int>(room_random_client::Read<uint32_t>(room.data(),0x57C));
        NativeFactory(15,static_cast<uint32_t>(startedModel));
    }
}
__declspec(noinline) void __fastcall FakeStartParser(void*,void*,const uint8_t* packet) {
    ++startParserCalls;
    nativeParserEpoch=packet[room_random_sync::kStartEpochOffset];
    if(asyncNativeQueues) {
        std::memcpy(queuedStart.data(),packet,queuedStart.size());
        rootStartQueued=true;
        return;
    }
    ExecuteQueuedStart(packet);
}
__declspec(noinline) uint8_t __fastcall NativeUserHasState(
    uint8_t*,void*,int slot,int state,int remove) {
    if(slot<0 || slot>=static_cast<int>(userStates.size()))return 0;
    auto& queue=userStates[static_cast<size_t>(slot)];
    const auto found=std::find(queue.begin(),queue.end(),state);
    if(found==queue.end())return 0;
    if(remove)queue.erase(found);
    return 1;
}
__declspec(noinline) int __fastcall NativeChangeRoomView(uint8_t* self,void*) {
    const auto count=room_random_client::Read<uint32_t>(self,0x18);
    auto* sentinel=room_random_client::Read<RoomViewNode*>(self,0x14);
    auto* payload=(count && sentinel && sentinel->next!=sentinel) ?
        sentinel->next->payload : nullptr;
    Put<uint8_t*>(self,0x20,payload);
    if(!payload) {
        Put<int>(self,8,0);
        return 0;
    }
    ++roomViewRequests;
    Put<int>(self,8,2);
    if(roomViewStateProbe) {
        WriteX87Control(static_cast<unsigned short>(
            (ReadX87Control()&~0x0C00u)|0x0800u));
        _mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_UP);
        SetLastError(kRoomViewLastError);
    }
    return 2;
}
void EnqueueUserState(size_t slot,int state,uint16_t view=1) {
    userStates[slot].push_back(state);
    if(state==0x0C)
        Check(roomViewServices[slot].Enqueue(view),
              "fixture room-view payload queue has capacity");
}
void ResetUserQueues() {
    for(size_t slot=0;slot<userStates.size();++slot) {
        userStates[slot].clear();
        roomViewServices[slot].Reset();
        userStateServices[slot][0x0C]=roomViewServices[slot].object.data();
    }
}
__declspec(noinline) void __fastcall NativeManagerUpdate(uint8_t*,void*) {
    ++managerTicks;
    // Shipped GOnline 0x00752AD0 advances the root room state before it
    // advances one state for each per-player manager. This fixture models
    // that ordering; it is not a native game execution.
    if(rootStartQueued) {
        rootRanWithViewPending=rootRanWithViewPending||HasQueuedState(0x0C);
        rootStartQueued=false;++rootStateRuns;
        ExecuteQueuedStart(queuedStart.data());
    }
    for(size_t slot=0;slot<userStates.size();++slot) {
        auto& queue=userStates[slot];
        if(queue.empty())continue;
        const int state=queue.front();
        if(state!=0x0C) {
            queue.erase(queue.begin());
            continue;
        }
        auto& service=roomViewServices[slot];
        int result=room_random_client::Read<int>(service.object.data(),8);
        if(result==0) {
            Put<int>(service.object.data(),8,1);
            result=1;
        } else if(result==1) {
            auto* vtable=room_random_client::Read<uint8_t*>(
                service.object.data(),0);
            auto process=room_random_client::Read<
                room_random_client::ChangeRoomViewFn>(vtable,8);
            result=process(service.object.data());
            if(result && autoAckRoomView) {
                Put<int>(service.object.data(),8,0);
                result=0;
            }
        }
        if(result==0) {
            const bool cleaned=service.Cleanup();
            Check(cleaned,"native scheduler cleanup owns one room-view payload");
            queue.erase(queue.begin());
            ++roomViewRuns;
        }
    }
    if(managerStateProbe) {
        WriteX87Control(static_cast<unsigned short>(
            (ReadX87Control()&~0x0C00u)|0x0400u));
        _mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_TOWARD_ZERO);
        SetLastError(kManagerUpdateLastError);
    }
}
__declspec(noinline) uint8_t* __fastcall NativeRoomInfo(uint8_t* object,void*) {
    return object==model.data() ? room.data() : nullptr;
}
#define SLOT_METHODS(Name,Storage,Base,Count) \
__declspec(noinline) uint32_t __fastcall Name##Owner(uint8_t*,void*,int slot) {return slot>=0 && slot<Count ? room_random_client::Read<uint32_t>(Storage.data(),Base+slot*0x48E+0x429) : 0;} \
__declspec(noinline) uint8_t* __fastcall Name##Unit(uint8_t* self,void*,int slot) {return Name##Owner(self,nullptr,slot) ? Storage.data()+Base+slot*0x48E+0x49 : nullptr;} \
__declspec(noinline) uint8_t* __fastcall Name##Ship(uint8_t* self,void*,int slot) {return Name##Owner(self,nullptr,slot) ? Storage.data()+Base+slot*0x48E+0x146 : nullptr;} \
__declspec(noinline) void __fastcall Name##SetUnit(uint8_t* self,void*,int slot,const uint8_t* data) {auto* p=Name##Unit(self,nullptr,slot);if(p)std::memcpy(p,data,0x99);} \
__declspec(noinline) void __fastcall Name##SetShip(uint8_t* self,void*,int slot,const uint8_t* data) {auto* p=Name##Ship(self,nullptr,slot);if(p)std::memcpy(p,data,0x2BC);}
SLOT_METHODS(Normal,room,0x533,12)
SLOT_METHODS(Mission,missionRoom,0x5C6,4)
#undef SLOT_METHODS
void __stdcall QueuedHandler(uint32_t opcode,const uint8_t* packet) {
    // Native 3D room notifications are commands. Starting prevents their cache
    // operation from completing before the start command constructs the mode.
    if(asyncNativeQueues && (opcode==0x0861 || opcode==0x098B)) {
        const auto slot=room_random_client::Read<uint32_t>(packet,10);
        if(slot<userStates.size()) {
            EnqueueUserState(slot,opcode==0x0861 ? 0x0E : 0x0F);
            if(room_random_client::Read<uint32_t>(packet,6)==
               room_random_client::Read<uint32_t>(profile.data(),0x138) &&
               !skipNativeRoomViewQueue)
                EnqueueUserState(slot,0x0C);
        }
    }
    if(opcode==0x0861 && !starting)
        std::memcpy(room.data()+0x57C,packet+0x12,0x99);
    if(opcode==0x0861 || opcode==0x098B)
        nativeNoticeRoute=room_random_client::Read<uint32_t>(packet,14);
    if(opcode==0x06C3)
        FakeStartParser(startParser.data(),nullptr,packet);
}
__declspec(naked) int NativeDispatch() {
    __asm {
        push ebp
        mov ebp,esp
        push esi
        push edi
        mov edi,ecx
        mov eax,[edi+44h]
        mov esi,[eax]
        push dword ptr [ebp+18h]
        push dword ptr [ebp+14h]
        call QueuedHandler
        pop edi
        pop esi
        mov esp,ebp
        pop ebp
        ret 14h
    }
}
using DispatchFn=int(__thiscall*)(void*,int,int,int,int,void*);
void Symbol(FILE* f,const char* name,const void* address,const char* kind) {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f,"[%s]\nkind=%s\nrva=0x%08lX\n",name,kind,static_cast<unsigned long>(reinterpret_cast<uintptr_t>(address)-base));
    if(std::strcmp(kind,"data")) {
        auto bytes=static_cast<const uint8_t*>(address);std::fprintf(f,"expect=");
        for(int i=0;i<7;++i)std::fprintf(f,"%02X ",bytes[i]);std::fprintf(f,"\n");
    }
    std::fprintf(f,"\n");
}
void Symbols() {
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);wchar_t file[160];
    swprintf_s(file,L"sdgo/symbols/room_random_client_runtime_test-%08lX-%08lX.ini",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    FILE* f=nullptr;_wfopen_s(&f,file,L"wb");if(!f){Check(false,"fixture symbols created");return;}
    std::fprintf(f,"[host]\ntimestamp=0x%08lX\nsize_of_image=0x%08lX\n\n",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    Symbol(f,"test.dispatch.entry",reinterpret_cast<void*>(&NativeDispatch),"code");
    Symbol(f,"room_random.client_dispatch",reinterpret_cast<const uint8_t*>(&NativeDispatch)+5,"site");
    Symbol(f,"room_random.client_profile",&profilePointer,"data");
    Symbol(f,"room_random.client_fill_human",reinterpret_cast<void*>(&NativeFill),"code");
    Symbol(f,"room_random.client_begin_start",reinterpret_cast<void*>(&FakeStartParser),"code");
    Symbol(f,"room_random.client_app",&appPointer,"data");
    Symbol(f,"room_random.client_waitroom_vtable",ownerVtable.data(),"data");
    Symbol(f,"room_random.client_room_info",reinterpret_cast<void*>(&NativeRoomInfo),"code");
    Symbol(f,"room_random.client_manager_update",reinterpret_cast<void*>(&NativeManagerUpdate),"code");
    Symbol(f,"room_random.client_user_has_state",reinterpret_cast<void*>(&NativeUserHasState),"code");
    Symbol(f,"room_random.client_change_room_view",reinterpret_cast<void*>(&NativeChangeRoomView),"code");
    Symbol(f,"room_random.client_change_room_view_vtable",roomViewVtable.data(),"data");
    Symbol(f,"room_random.client_normal_owner",reinterpret_cast<void*>(&NormalOwner),"code");
    Symbol(f,"room_random.client_normal_unit",reinterpret_cast<void*>(&NormalUnit),"code");
    Symbol(f,"room_random.client_normal_ship",reinterpret_cast<void*>(&NormalShip),"code");
    Symbol(f,"room_random.client_normal_set_unit",reinterpret_cast<void*>(&NormalSetUnit),"code");
    Symbol(f,"room_random.client_normal_set_ship",reinterpret_cast<void*>(&NormalSetShip),"code");
    Symbol(f,"room_random.client_mission_owner",reinterpret_cast<void*>(&MissionOwner),"code");
    Symbol(f,"room_random.client_mission_unit",reinterpret_cast<void*>(&MissionUnit),"code");
    Symbol(f,"room_random.client_mission_ship",reinterpret_cast<void*>(&MissionShip),"code");
    Symbol(f,"room_random.client_mission_set_unit",reinterpret_cast<void*>(&MissionSetUnit),"code");
    Symbol(f,"room_random.client_mission_set_ship",reinterpret_cast<void*>(&MissionSetShip),"code");
    fclose(f);
}
template<size_t N>
void TagNotice(std::array<uint8_t,N>& packet,uint16_t roomIndex,uint8_t epoch) {
    Put<uint32_t>(packet.data(),14,room_random_sync::TaggedRoute(roomIndex,epoch));
}
void TagStart(std::array<uint8_t,0xCD4>& packet,uint8_t epoch) {
    packet[room_random_sync::kStartEpochOffset]=epoch;
}
}
int wmain(int argc,wchar_t** argv) {
    const bool enabled=argc>=2 && !std::wcscmp(argv[1],L"enabled");
    if(argc>=3) {
        try {nativeCopies=std::make_unique<room_random_native_test::ClientCopies>(argv[2]);}
        catch(const std::exception& e){std::printf("native fixture: %s\n",e.what());return 2;}
        std::printf("Using authenticated GOnline human-source/model/attribute instruction spans\n");
    }
    roomViewVtable[2]=reinterpret_cast<void*>(&NativeChangeRoomView);
    for(size_t slot=0;slot<userManagerPointers.size();++slot) {
        userManagerPointers[slot]=userManagers[slot].data();
        roomViewServices[slot].Reset();
        userStateServices[slot][0x0C]=roomViewServices[slot].object.data();
        Put<uint8_t**>(userManagers[slot].data(),0x0C,
                       userStateServices[slot].data());
    }
    Put<uint8_t*>(profile.data(),0xEC,room.data());Put<uint32_t>(profile.data(),0x138,1001);
    Put<uint32_t>(room.data(),0,7);room[4]=2;room[5]=8;
    Put<uint32_t>(room.data(),0x95C,1001);Put<uint32_t>(room.data(),0x57C,15019);
    std::array<uint8_t,0x99> unit{};Put<uint32_t>(unit.data(),0,11032);Put<uint64_t>(unit.data(),8,0x123456789ABCULL);
    auto notice=room_random_sync::Packet(
        0x0861,1001,0,room_random_sync::TaggedRoute(7,1),unit);
    std::array<uint8_t,0xCD4> start{};Put<uint16_t>(start.data(),0,0x06C3);
    TagStart(start,1);
    Put<uint32_t>(start.data(),0x20,1001);Put<uint32_t>(start.data(),0x2E,11032);
    std::array<uint8_t,0x50> dispatcher{};void* head=nullptr;Put<void*>(dispatcher.data(),0x44,&head);
    Put<uint8_t*>(startParser.data(),0x0C,startParserOwner.data());
    Put<uint8_t*>(startParserOwner.data(),0x14,model.data());
    auto dispatch=reinterpret_cast<DispatchFn>(&NativeDispatch);
    TagNotice(notice,7,1);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    Check(startedModel==15019,"baseline reproduces stale model while native start is pending");
    Symbols();HMODULE core=LoadLibraryW(L"sdgo/core.dll");Check(core!=nullptr,"real core loads");if(!core)return 2;
    auto init=reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit"));
    SdgoBoot boot{sizeof(SdgoBoot),"room_random_client_runtime_test",GetModuleHandleW(nullptr)};
    Check(init && init(&boot),"real core initializes both entry and interior hook owners");
    startedModel=0;factoryCalls=0;nativeNoticeRoute=0;nativeParserEpoch=0;
    SetTitle("fixture !rand");
    TagNotice(notice,7,1);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    Check(startedModel==(enabled?11032:15019),"start uses the authoritative model only when synchronization is enabled");
    Check(factoryCalls==1,"native factory still executes once");
    Check(nativeNoticeRoute==(enabled ? 7u :
              room_random_sync::TaggedRoute(7,1)),
          "enabled hook strips the private epoch signature before the native notice handler");
    if(nativeParserEpoch!=(enabled ? 0u : 1u))
        std::printf("parser epoch observed: %u\n",
                    static_cast<unsigned>(nativeParserEpoch));
    Check(nativeParserEpoch==(enabled ? 0u : 1u),
          "enabled hook passes a canonical zero reserved byte to the original parser");
    Put<uint32_t>(frozen[0].data(),0x429,1001);Put<uint32_t>(frozen[0].data(),0x49,15019);
    Construct(start.data()+0x20,frozen[0].data(),0);
    Check(startedModel==(enabled?11032:15019),"the static converter's unrelated ECX cannot replace the verified lobby packet context");
    if(enabled) {
        Check(room_random_client::Read<uint64_t>(profile.data(),0x1B1)==0x123456789ABCULL,"own profile receives the complete 64-bit inventory identity");
        Put<uint32_t>(unit.data(),0,15019);Put<uint64_t>(unit.data(),8,777);
        auto restore=room_random_sync::Packet(0x0861,1001,0,7,unit);
        dispatch(dispatcher.data(),0,0,0,0x0861,restore.data());
        Check(room_random_client::Read<uint32_t>(room.data(),0x57C)==15019,"restore notice updates the room cache without a UI queue tick");
    }
    Put<uint8_t*>(profile.data(),0xEC,nullptr);
    Put<uint32_t>(app.data(),0x4A0,18);Put<uint8_t*>(app.data(),0x4A4,owner.data());
    Put<uint8_t*>(owner.data(),0,ownerVtable.data());Put<uint8_t*>(owner.data(),4,manager.data());
    Put<uint8_t*>(manager.data(),0,managerVtable.data());
    Put<void*>(managerVtable.data(),0x10,reinterpret_cast<void*>(&NativeManagerUpdate));
    Put<void*>(managerVtable.data(),0x1C,reinterpret_cast<void*>(&NativeUserHasState));
    Put<int>(manager.data(),0x6C,static_cast<int>(userManagerPointers.size()));
    Put<uint8_t**>(manager.data(),0x70,userManagerPointers.data());
    Put<uint8_t*>(manager.data(),0x38,model.data());Put<uint8_t*>(model.data(),0,modelVtable.data());
    Put<void*>(modelVtable.data(),0x1C,reinterpret_cast<void*>(&NativeRoomInfo));
    Put<void*>(modelVtable.data(),0x20,reinterpret_cast<void*>(&MissionOwner));
    Put<void*>(modelVtable.data(),0x84,reinterpret_cast<void*>(&MissionSetUnit));
    Put<void*>(modelVtable.data(),0x88,reinterpret_cast<void*>(&MissionUnit));
    Put<void*>(modelVtable.data(),0x8C,reinterpret_cast<void*>(&MissionSetShip));
    Put<void*>(modelVtable.data(),0x90,reinterpret_cast<void*>(&MissionShip));
    Put<uint8_t*>(startParser.data(),0x10,manager.data());
    Put<uint32_t>(missionRoom.data(),0x9EF,1001);Put<uint32_t>(missionRoom.data(),0x60F,15019);threeD=true;
    SetTitle("fixture !rand");
    for(size_t i=0;i<frozen[0].size();++i)frozen[0][i]=static_cast<uint8_t>(i*13);
    Put<uint32_t>(frozen[0].data(),0x429,1001);Put<uint32_t>(frozen[0].data(),0x49,15019);
    const auto original=frozen[0];
    Put<uint32_t>(room.data(),0x57C,15019);startedModel=0;
    fillCalls=0;
    TagStart(start,1);TagNotice(notice,7,1);
    dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    reinterpret_cast<room_random_client::ManagerUpdateFn>(&NativeManagerUpdate)(manager.data());
    Check(startedModel==(enabled?11032:15019),"real DLL corrects the frozen 3D mission source after the scene transition");
    Check(fillCalls==1,"native human converter executes exactly once");
    auto expected=original;
    if(enabled)std::memcpy(expected.data()+0x49,notice.data()+0x12,0x99);
    Check(lastConstruction==expected && frozen[0]==original,"complete construction input changes without mutating the frozen source");
    Check(room_random_client::Read<uint8_t*>(profile.data(),0xEC)==nullptr,"3D synchronization never fabricates a legacy room publication");
    Check(room_random_client::Read<uint32_t>(room.data(),0x57C)==15019,"3D mission updates never write the unrelated normal-layout row");
    const auto verifiedVtable=modelVtable;
    Put<void*>(modelVtable.data(),0x1C,nullptr);
    Construct(start.data()+0x20,frozen[0].data(),0);
    Check(lastConstruction==original,"a captured model address must still pass its complete live native vtable before reuse");
    modelVtable=verifiedVtable;
    // Switch to the normal-room native interface; final conversion is shared.
    Put<void*>(modelVtable.data(),0x20,reinterpret_cast<void*>(&NormalOwner));
    Put<void*>(modelVtable.data(),0x84,reinterpret_cast<void*>(&NormalSetUnit));
    Put<void*>(modelVtable.data(),0x88,reinterpret_cast<void*>(&NormalUnit));
    Put<void*>(modelVtable.data(),0x8C,reinterpret_cast<void*>(&NormalSetShip));
    Put<void*>(modelVtable.data(),0x90,reinterpret_cast<void*>(&NormalShip));
    Put<uint32_t>(app.data(),0x4A0,18);room[5]=1;startedModel=0;
    TagStart(start,2);TagNotice(notice,7,2);
    dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    reinterpret_cast<room_random_client::ManagerUpdateFn>(&NativeManagerUpdate)(manager.data());
    Check(startedModel==(enabled?11032:15019),"ordinary PvP also uses the corrected complete construction source");
    // The native 3D manager is authoritative even if an older legacy room
    // publication remains non-null. A matching owner in that stale buffer must
    // not make it writable during a 3D transition.
    auto staleLegacy=room;
    Put<uint32_t>(staleLegacy.data(),0x57C,15019);
    const auto staleBefore=staleLegacy;
    Put<uint32_t>(app.data(),0x4A0,18);
    Put<uint8_t*>(profile.data(),0xEC,staleLegacy.data());
    Put<uint32_t>(room.data(),0x57C,15019);
    TagNotice(notice,7,2);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    Check(room_random_client::Read<uint32_t>(room.data(),0x57C)==(enabled?11032u:15019u),
          "active native 3D model takes precedence over a non-null legacy publication");
    Check(staleLegacy==staleBefore,"a retained legacy publication is never written by 3D synchronization");
    const auto liveModelVtable=modelVtable;
    Put<void*>(modelVtable.data(),0x1C,nullptr);
    const auto activeBefore=room;
    TagNotice(notice,7,2);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    Check(room==activeBefore&&staleLegacy==staleBefore,
          "an invalid active 3D model cannot fall back to a stale writable legacy room");
    modelVtable=liveModelVtable;
    Put<uint8_t*>(profile.data(),0xEC,nullptr);
    // Deterministic model of the shipped 0x00752AD0 ordering:
    // network dispatch has already queued per-player E then C, while the root
    // D start state runs before one per-player state on every manager tick.
    // The disabled case represents the unguarded/0.1.13 ordering. This is a
    // fixture proof of the race window; the native campaign proves occurrence.
    asyncNativeQueues=true;starting=false;threeD=true;
    ResetUserQueues();
    rootStartQueued=false;rootRanWithViewPending=false;
    rootStateRuns=0;roomViewRuns=0;roomViewRequests=0;managerTicks=0;
    Put<uint32_t>(app.data(),0x4A0,18);Put<uint32_t>(room.data(),0,39);
    room[5]=8;SetTitle("queue !rand");
    Put<uint32_t>(room.data(),0x95C,1001);Put<uint32_t>(room.data(),0x57C,15019);
    Put<uint32_t>(frozen[0].data(),0x429,1001);Put<uint32_t>(frozen[0].data(),0x49,15019);
    startParserCalls=0;startParserConstructions=0;startedModel=0;
    TagStart(start,3);TagNotice(notice,39,3);
    dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    Check(userStates[0].size()==2 && userStates[0][0]==0x0E &&
          userStates[0][1]==0x0C,
          "fixture queues the native loadout task before its local room-view request");
    Check(startParserCalls==(enabled?0:1),
          "guarded random start is retained while native E/C work is pending");
    auto tick=reinterpret_cast<room_random_client::ManagerUpdateFn>(&NativeManagerUpdate);
    const DWORD savedError=GetLastError();
    const unsigned int savedMxcsr=_mm_getcsr();
    const unsigned short savedControl=ReadX87Control();
    managerStateProbe=true;
    tick(manager.data());
    const DWORD observedError=GetLastError();
    const unsigned int observedMxcsr=_mm_getcsr();
    const unsigned short observedControl=ReadX87Control();
    managerStateProbe=false;
    _mm_setcsr(savedMxcsr);
    WriteX87Control(savedControl);
    SetLastError(savedError);
    const bool managerStatePreserved=
        observedError==kManagerUpdateLastError &&
        (observedMxcsr&_MM_ROUND_MASK)==_MM_ROUND_TOWARD_ZERO &&
        (observedControl&0x0C00u)==0x0400u;
    if(!managerStatePreserved)
        std::printf("manager state: error=%08lX mxcsr=%08X control=%08X\n",
                    static_cast<unsigned long>(observedError),
                    observedMxcsr,observedControl);
    Check(managerStatePreserved,
          "manager gate preserves the original update LastError and x87/SSE state");
    Check(rootStateRuns==(enabled?0:1) &&
          rootRanWithViewPending==(enabled?false:true),
          "unguarded root D executes before C; guarded root D remains closed");
    tick(manager.data());
    Check(roomViewRuns==0 && startParserCalls==(enabled?0:1),
          "native C first enters its process stage without releasing the held start");
    const DWORD roomViewEntryError=0x5A19;
    WriteX87Control(static_cast<unsigned short>(
        (ReadX87Control()&~0x0C00u)|0x0C00u));
    _mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_DOWN);
    SetLastError(roomViewEntryError);
    roomViewStateProbe=true;
    tick(manager.data());
    roomViewStateProbe=false;
    const DWORD roomViewObservedError=GetLastError();
    const unsigned int roomViewObservedMxcsr=_mm_getcsr();
    const unsigned short roomViewObservedControl=ReadX87Control();
    _mm_setcsr(savedMxcsr);
    WriteX87Control(savedControl);
    SetLastError(savedError);
    const bool roomViewStateCorrect=enabled ?
        (roomViewObservedError==roomViewEntryError &&
         (roomViewObservedMxcsr&_MM_ROUND_MASK)==_MM_ROUND_DOWN &&
         (roomViewObservedControl&0x0C00u)==0x0C00u) :
        (roomViewObservedError==kRoomViewLastError &&
         (roomViewObservedMxcsr&_MM_ROUND_MASK)==_MM_ROUND_UP &&
         (roomViewObservedControl&0x0C00u)==0x0800u);
    Check(roomViewStateCorrect,
          "cancel preserves entry state while forwarding preserves native room-view effects");
    Check(roomViewRuns==1 &&
          roomViewServices[0].frees==1 &&
          roomViewRequests==(enabled?0:1) &&
          startParserCalls==1 &&
          rootStartQueued==(enabled?true:false),
          "held random start retires obsolete C through one native cleanup without sending 0972");
    tick(manager.data());
    Check(rootStateRuns==1 &&
          rootRanWithViewPending==(enabled?false:true),
          "only the guarded root D executes with no pending ChangeRoomView task");
    ResetUserQueues();
    roomViewRuns=0;roomViewRequests=0;autoAckRoomView=false;
    EnqueueUserState(0,0x0C,3);
    Put<int>(roomViewServices[0].object.data(),8,1);
    tick(manager.data());
    Check(roomViewRequests==1 && roomViewRuns==0 &&
          userStates[0].size()==1 &&
          room_random_client::Read<uint8_t*>(
              roomViewServices[0].object.data(),0x20)==
                  roomViewServices[0].payloads[0].data(),
          "ordinary waiting-room C forwards to the original 0972 process");
    Check(roomViewServices[0].Cleanup(),
          "fixture acknowledges the forwarded ordinary room-view request");
    userStates[0].clear();autoAckRoomView=true;
    if(enabled) {
        // A held start may outlive more than one obsolete local C request.
        // Each one must use the native payload cleanup exactly once, while
        // incomplete human records continue to keep the parser closed.
        ResetUserQueues();rootStartQueued=false;
        roomViewRuns=0;roomViewRequests=0;startParserCalls=0;
        Put<uint32_t>(app.data(),0x4A0,18);
        Put<uint32_t>(room.data(),0,44);room[5]=8;SetTitle("multi !rand");
        Put<uint32_t>(room.data(),0x95C,1001);
        Put<uint32_t>(room.data(),0x57C,15019);
        TagStart(start,4);
        dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
        Check(startParserCalls==0,
              "missing human record holds the random start before C cleanup");
        TagNotice(notice,44,4);
        dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
        TagNotice(notice,44,4);
        dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
        for(int i=0;i<6;++i)tick(manager.data());
        Check(roomViewServices[0].frees==2 &&
              roomViewRequests==0 && startParserCalls==1 &&
              rootStartQueued,
              "each tagged duplicate retires its own C and the held parser resumes once after both E/C pairs settle");
        tick(manager.data());

        // The shipped notice handler can skip enqueueing local state C when
        // its native flag 4 is set. The tagged-notice cleanup permit belongs
        // only to the matching held start and must expire when that start is
        // released, or a later ordinary waiting-room C would be cancelled.
        ResetUserQueues();rootStartQueued=false;
        roomViewRuns=0;roomViewRequests=0;startParserCalls=0;
        Put<uint32_t>(app.data(),0x4A0,18);
        Put<uint32_t>(room.data(),0,49);room[5]=8;SetTitle("skip C !rand");
        Put<uint32_t>(room.data(),0x95C,1001);
        Put<uint32_t>(room.data(),0x57C,15019);
        TagNotice(notice,49,13);
        TagStart(start,13);
        skipNativeRoomViewQueue=true;
        dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
        dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
        Check(userStates[0].size()==1 && userStates[0][0]==0x0E &&
              startParserCalls==0,
              "native flag-4 fixture queues E without a synthetic local C");
        tick(manager.data());
        Check(startParserCalls==1 && rootStartQueued &&
              userStates[0].empty(),
              "matching start resumes after the only native E task settles");
        skipNativeRoomViewQueue=false;
        autoAckRoomView=false;
        EnqueueUserState(0,0x0C,13);
        for(int i=0;i<2;++i)tick(manager.data());
        Check(roomViewRequests==1 && roomViewRuns==0 &&
              userStates[0].size()==1,
              "released epoch cannot cancel a later ordinary waiting-room C");
        Check(roomViewServices[0].Cleanup(),
              "fixture acknowledges the post-start ordinary room-view request");
        userStates[0].clear();autoAckRoomView=true;

        // Two ordered starts may choose the same model. The second 06C3 must
        // fence the first round's complete record so its old serial/attributes
        // cannot authorize construction before this round's fresh 0861.
        ResetUserQueues();rootStartQueued=false;
        roomViewRuns=0;roomViewRequests=0;startParserCalls=0;
        startParserConstructions=0;startedModels={};
        Put<uint32_t>(app.data(),0x4A0,18);
        Put<uint32_t>(room.data(),0,47);room[5]=8;SetTitle("same !rand");
        Put<uint32_t>(room.data(),0x95C,1001);
        Put<uint32_t>(room.data(),0x57C,11032);
        Put<uint32_t>(frozen[0].data(),0x429,1001);
        Put<uint32_t>(frozen[0].data(),0x49,11032);
        auto sameStart=start;
        Put<uint32_t>(sameStart.data(),0x20,1001);
        Put<uint32_t>(sameStart.data(),0x2E,11032);
        TagStart(sameStart,5);
        std::array<uint8_t,0x99> firstRoundUnit{};
        Put<uint32_t>(firstRoundUnit.data(),0,11032);
        Put<uint64_t>(firstRoundUnit.data(),8,0x111111111111ULL);
        Put<uint32_t>(firstRoundUnit.data(),0x14,0xA1A1A1A1);
        auto firstRoundNotice=room_random_sync::Packet(
            0x0861,1001,0,room_random_sync::TaggedRoute(47,5),firstRoundUnit);
        dispatch(dispatcher.data(),0,0,0,0x06C3,sameStart.data());
        dispatch(dispatcher.data(),0,0,0,0x0861,firstRoundNotice.data());
        for(int i=0;i<3;++i)tick(manager.data());
        Check(startParserCalls==1 && rootStartQueued,
              "first identical-model round waits for its ordered fresh record");
        tick(manager.data());
        Check(room_random_client::Read<uint64_t>(
                  lastConstruction.data(),0x49+8)==0x111111111111ULL &&
              room_random_client::Read<uint32_t>(
                  lastConstruction.data(),0x49+0x14)==0xA1A1A1A1,
              "first round constructs from its complete serial and attributes");

        ResetUserQueues();rootStartQueued=false;
        roomViewRuns=0;roomViewRequests=0;startParserCalls=0;
        startParserConstructions=0;startedModels={};
        Put<uint32_t>(app.data(),0x4A0,18);
        TagStart(sameStart,6);
        dispatch(dispatcher.data(),0,0,0,0x06C3,sameStart.data());
        for(int i=0;i<2;++i)tick(manager.data());
        Check(startParserCalls==0 && roomViewServices[0].frees==0,
              "next epoch rejects the prior complete record without consuming an unrelated C request");
        std::array<uint8_t,0x99> secondRoundUnit{};
        Put<uint32_t>(secondRoundUnit.data(),0,11032);
        Put<uint64_t>(secondRoundUnit.data(),8,0x222222222222ULL);
        Put<uint32_t>(secondRoundUnit.data(),0x14,0xB2B2B2B2);
        auto secondRoundNotice=room_random_sync::Packet(
            0x0861,1001,0,room_random_sync::TaggedRoute(47,6),secondRoundUnit);
        dispatch(dispatcher.data(),0,0,0,0x0861,secondRoundNotice.data());
        for(int i=0;i<3;++i)tick(manager.data());
        Check(startParserCalls==1 && rootStartQueued,
              "second identical-model parser opens only after its fresh post-start record and native tasks");
        tick(manager.data());
        Check(room_random_client::Read<uint64_t>(
                  lastConstruction.data(),0x49+8)==0x222222222222ULL &&
              room_random_client::Read<uint32_t>(
                  lastConstruction.data(),0x49+0x14)==0xB2B2B2B2,
              "second round uses the fresh serial and attributes instead of the prior matching model");

        // A byte-identical retransmission of the held start is the same
        // generation. It must retain fresh records already received after the
        // first 06C3 while the remaining humans and native C/E work complete.
        ResetUserQueues();rootStartQueued=false;
        roomViewRuns=0;roomViewRequests=0;startParserCalls=0;
        startParserConstructions=0;startedModels={};
        Put<uint32_t>(app.data(),0x4A0,18);
        Put<uint32_t>(room.data(),0,48);room[5]=8;SetTitle("duplicate !rand");
        std::array<uint8_t,0xCD4> duplicateStart{};
        Put<uint16_t>(duplicateStart.data(),0,0x06C3);
        TagStart(duplicateStart,7);
        std::array<std::array<uint8_t,0x99>,2> duplicateUnits{};
        std::array<std::array<uint8_t,room_random_sync::kPayload+
            room_random_sync::kUnitSize>,2> duplicateNotices{};
        for(size_t slot=0;slot<2;++slot) {
            const auto ownerId=static_cast<uint32_t>(1001+slot);
            const auto modelId=static_cast<uint32_t>(12001+slot);
            Put<uint32_t>(room.data(),0x533+slot*0x48E+0x429,ownerId);
            Put<uint32_t>(room.data(),0x533+slot*0x48E+0x49,
                          static_cast<uint32_t>(13001+slot));
            frozen[slot]={};
            Put<uint32_t>(frozen[slot].data(),0x429,ownerId);
            Put<uint32_t>(frozen[slot].data(),0x49,
                          static_cast<uint32_t>(13001+slot));
            Put<uint32_t>(duplicateStart.data(),0x20+slot*0x10F,ownerId);
            Put<uint32_t>(duplicateStart.data(),0x2E+slot*0x10F,modelId);
            Put<uint32_t>(duplicateUnits[slot].data(),0,modelId);
            Put<uint64_t>(duplicateUnits[slot].data(),8,
                          0x333333333333ULL+slot);
            Put<uint32_t>(duplicateUnits[slot].data(),0x14,
                          static_cast<uint32_t>(0xC3C3C3C3+slot));
            duplicateNotices[slot]=room_random_sync::Packet(
                0x0861,ownerId,static_cast<uint32_t>(slot),
                room_random_sync::TaggedRoute(48,7),
                duplicateUnits[slot]);
        }
        dispatch(dispatcher.data(),0,0,0,0x06C3,duplicateStart.data());
        dispatch(dispatcher.data(),0,0,0,0x0861,duplicateNotices[0].data());
        dispatch(dispatcher.data(),0,0,0,0x06C3,duplicateStart.data());
        dispatch(dispatcher.data(),0,0,0,0x0861,duplicateNotices[1].data());
        for(int i=0;i<3;++i)tick(manager.data());
        Check(startParserCalls==1 && rootStartQueued &&
              roomViewServices[0].frees==1,
              "identical held 06C3 retains earlier fresh records and resumes once after C/E cleanup");
        tick(manager.data());
        Check(startParserCalls==1 && startParserConstructions==2 &&
              startedModels[0]==12001 && startedModels[1]==12002,
              "duplicate start constructs both humans exactly once");
        Check(room_random_client::Read<uint64_t>(
                  profile.data(),0x1A9+8)==0x333333333333ULL &&
              room_random_client::Read<uint32_t>(
                  profile.data(),0x1A9+0x14)==0xC3C3C3C3,
              "local record received before duplicate 06C3 retains its fresh serial and attributes");
        Check(room_random_client::Read<uint64_t>(
                  lastConstruction.data(),0x49+8)==0x333333333334ULL &&
              room_random_client::Read<uint32_t>(
                  lastConstruction.data(),0x49+0x14)==0xC3C3C3C4,
              "record received after duplicate 06C3 supplies fresh construction serial and attributes");
        Put<uint32_t>(room.data(),0x533+0x48E+0x429,0);

        // A state from a superseded room generation is ordinary native work.
        // Revalidation clears the held token and forwards C unchanged.
        ResetUserQueues();rootStartQueued=false;
        roomViewRuns=0;roomViewRequests=0;startParserCalls=0;
        Put<uint32_t>(app.data(),0x4A0,18);
        Put<uint32_t>(room.data(),0,45);Put<uint32_t>(room.data(),0x57C,15019);
        TagStart(start,8);
        dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
        EnqueueUserState(0,0x0C,4);
        Put<int>(roomViewServices[0].object.data(),8,1);
        Put<uint32_t>(room.data(),0,46);
        autoAckRoomView=false;
        tick(manager.data());
        Check(roomViewRequests==1 && roomViewRuns==0 &&
              userStates[0].size()==1 && startParserCalls==0,
              "stale room generation passes its C request to the original process");
        Check(roomViewServices[0].Cleanup(),
              "fixture acknowledges the stale-generation forwarded request");
        userStates[0].clear();autoAckRoomView=true;
    }
    asyncNativeQueues=false;starting=true;
    ResetUserQueues();
    rootStartQueued=false;
    Put<uint32_t>(app.data(),0x4A0,18);
    // A real four-human random trace can deliver the last full record after
    // 06C3. The production parser must be held and then invoked once by the
    // manager tick after the final matching native notice and C/E/F work.
    threeD=true;Put<uint32_t>(app.data(),0x4A0,18);
    Put<uint32_t>(room.data(),0,40);room[5]=8;
    SetTitle("1v1 !rand");
    std::array<std::array<uint8_t,0x99>,4> lateUnits{};
    std::array<std::array<uint8_t,room_random_sync::kPayload+room_random_sync::kUnitSize>,4> lateNotices{};
    std::array<uint8_t,0xCD4> lateStart{};Put<uint16_t>(lateStart.data(),0,0x06C3);
    TagStart(lateStart,9);
    for(size_t slot=0;slot<4;++slot) {
        const auto pilot=static_cast<uint32_t>(4101+slot);
        const auto oldModel=static_cast<uint32_t>(17001+slot);
        const auto newModel=static_cast<uint32_t>(14001+slot);
        Put<uint32_t>(room.data(),0x533+slot*0x48E+0x429,pilot);
        Put<uint32_t>(room.data(),0x533+slot*0x48E+0x49,oldModel);
        Put<uint32_t>(lateUnits[slot].data(),0,newModel);
        Put<uint64_t>(lateUnits[slot].data(),8,0x600000001ULL+slot);
        lateNotices[slot]=room_random_sync::Packet(
            0x0861,pilot,static_cast<uint32_t>(slot),
            room_random_sync::TaggedRoute(40,9),lateUnits[slot]);
        Put<uint32_t>(lateStart.data(),0x20+slot*0x10F,pilot);
        Put<uint32_t>(lateStart.data(),0x2E+slot*0x10F,newModel);
        frozen[slot]={};
        Put<uint32_t>(frozen[slot].data(),0x429,pilot);
        Put<uint32_t>(frozen[slot].data(),0x49,oldModel);
    }
    startParserCalls=0;startParserConstructions=0;startedModels={};
    dispatch(dispatcher.data(),0,0,0,0x06C3,lateStart.data());
    for(size_t slot=0;slot<3;++slot)
        dispatch(dispatcher.data(),0,0,0,0x0861,lateNotices[slot].data());
    Check(startParserCalls==(enabled?0:1),
          "random start is held only while the final human record is missing");
    auto rejectedLate=lateNotices[3];Put<uint32_t>(rejectedLate.data(),6,9999);
    dispatch(dispatcher.data(),0,0,0,0x0861,rejectedLate.data());
    auto rangedLate=lateNotices[3];Put<uint32_t>(rangedLate.data(),10,12);
    dispatch(dispatcher.data(),0,0,0,0x0861,rangedLate.data());
    TagNotice(lateNotices[0],40,9);
    dispatch(dispatcher.data(),0,0,0,0x0861,lateNotices[0].data());
    Check(startParserCalls==(enabled?0:1),
          "rejected and duplicate notices cannot release an incomplete start");
    auto replacementStart=lateStart;replacementStart[0x20+5]=0x33;
    TagStart(replacementStart,10);
    dispatch(dispatcher.data(),0,0,0,0x06C3,replacementStart.data());
    Check(startParserCalls==(enabled?0:2),
          "a replacement 06C3 supersedes the older held parser generation");
    for(size_t slot=0;slot<4;++slot) {
        TagNotice(lateNotices[slot],40,10);
        dispatch(dispatcher.data(),0,0,0,0x0861,lateNotices[slot].data());
    }
    Check(startParserCalls==(enabled?0:2),
          "the last notice cannot resume a parser from inside packet dispatch");
    tick(manager.data());
    Check(startParserCalls==(enabled?1:2) &&
          startParserConstructions==(enabled?4:8),
          "the latest original parser runs once after all four humans are complete");
    for(size_t slot=0;slot<4;++slot) {
        Check(startedModels[slot]==(enabled?static_cast<int>(14001+slot):static_cast<int>(17001+slot)),
              "resumed parser constructs every human from one coherent generation");
    }
    // A held parser belongs to one room/model generation. Once that identity
    // changes, a later record must not replay the stale packet or parser.
    Put<uint32_t>(app.data(),0x4A0,18);Put<uint32_t>(room.data(),0,41);
    for(size_t slot=0;slot<3;++slot) {
        TagNotice(lateNotices[slot],41,11);
        dispatch(dispatcher.data(),0,0,0,0x0861,lateNotices[slot].data());
    }
    auto generationStart=replacementStart;
    TagStart(generationStart,11);
    dispatch(dispatcher.data(),0,0,0,0x06C3,generationStart.data());
    const auto beforeGeneration=startParserCalls;
    Put<uint32_t>(room.data(),0,42);
    std::array<uint8_t,2> irrelevant{};Put<uint16_t>(irrelevant.data(),0,0x7777);
    dispatch(dispatcher.data(),0,0,0,0x7777,irrelevant.data());
    TagNotice(lateNotices[3],42,11);
    dispatch(dispatcher.data(),0,0,0,0x0861,lateNotices[3].data());
    Check(startParserCalls==beforeGeneration,
          "room generation change discards a held parser instead of replaying it");
    // Full ship and every native model/attribute projection survive the hook.
    threeD=true;Put<uint32_t>(room.data(),0,43);room[5]=7;Put<uint32_t>(app.data(),0x4A0,18);
    SetTitle("deck !random");
    frozen[0]=original;
    Put<uint32_t>(room.data(),0x95C,1001);Put<uint32_t>(room.data(),0x57C,15019);
    std::array<uint8_t,0x2BC> ship{};
    for(size_t i=0;i<4;++i) {
        auto id=static_cast<uint32_t>(11032+i);
        Put<uint32_t>(ship.data(),0x58+i*0x99,id);
        Put<uint32_t>(ship.data(),0x58+i*0x99+0x14,static_cast<uint32_t>(0xB0000000+i));
        Put<uint64_t>(ship.data(),0x34+i*8,0x100000001ULL+i);
        Put<uint64_t>(ship.data(),0x60+i*0x99,0x100000001ULL+i);
        Put<uint32_t>(start.data(),0x2E+i*0x32,id);
    }
    Put<uint64_t>(ship.data(),0x1C,0x100000001ULL);
    auto deck=room_random_sync::Packet(
        0x098B,1001,0,room_random_sync::TaggedRoute(43,12),ship);
    TagStart(start,12);TagNotice(notice,43,12);
    startParserCalls=0;startParserConstructions=0;startedModels={};
    dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    dispatch(dispatcher.data(),0,0,0,0x098B,deck.data());
    Check(startParserCalls==(enabled?0:1),
          "deck start waits when the matching unit record is still late");
    TagNotice(notice,43,12);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    Check(startParserCalls==(enabled?0:1),
          "late unit completion does not re-enter the parser from dispatch");
    tick(manager.data());
    Check(startParserCalls==1 && startParserConstructions==1,
          "late unit record releases the original deck parser exactly once");
    expected=original;
    if(enabled){std::memcpy(expected.data()+0x49,notice.data()+0x12,0x99);std::memcpy(expected.data()+0x146,ship.data(),ship.size());}
    Check(lastConstruction==expected && frozen[0]==original,"real DLL passes the complete ship with original pilot metadata and immutable source");
    if(enabled)for(size_t i=0;i<4;++i) {
        Check(room_random_client::Read<uint32_t>(lastOutput.data(),0x44+i*0xB4)==11032+i,"native converter projects each deck model from the supplied full source");
        Check(room_random_client::Read<uint32_t>(lastOutput.data(),0x4C+i*0xB4)==0xB0000000+i,"native converter preserves the original full-width per-unit field");
    }
    auto ai=start;Put<uint32_t>(ai.data(),0x20,0x80000001u);Construct(ai.data()+0x20,frozen[0].data(),0);
    Check(lastConstruction==original,"AI-tagged source passes through unchanged");
    Put<uint32_t>(app.data(),0x4A0,18);
    auto changedNotice=notice;TagNotice(changedNotice,43,12);changedNotice[0x12+8]^=1;
    dispatch(dispatcher.data(),0,0,0,0x0861,changedNotice.data());Construct(start.data()+0x20,frozen[0].data(),1);
    Check(lastConstruction==original,"a later loadout notice invalidates previous construction authority");
    std::printf("%s CLIENT START-STATE RUNTIME CHECKS\n",failures?"FAILED":"PASSED");return failures?1:0;
}
