#include <windows.h>
#include <intrin.h>
#include <array>
#include <cstdio>
#include "sdgo/sdgo_api.h"
#include "sdgo/random_loadout.h"
#include "sdgo/room_titles_current.h"
#include "cache_sync.h"
#include "context.h"
#include "pending_start.h"

namespace {
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
room_random_client::Bindings g_bindings;
void* g_original=nullptr;
const DWORD g_defaultMxcsr=0x1F80;
room_random_client::CacheSync g_sync;
room_random_client::PendingStart g_pending;
CRITICAL_SECTION g_lock;
struct alignas(16) NativeFpState {
    unsigned char bytes[512];
};
// The shipped converter is a static four-stack-argument function (RET 10h).
// ECX is volatile scratch, not a CBaseRoomData receiver.
using FillFn=void(__stdcall*)(uint8_t*,const uint8_t*,const uint8_t*,uint8_t);
FillFn g_fill=nullptr;
using BeginStartFn=void(__thiscall*)(void*,const uint8_t*);
BeginStartFn g_beginStart=nullptr;
room_random_client::ManagerUpdateFn g_managerUpdate=nullptr;
room_random_client::ChangeRoomViewFn g_changeRoomView=nullptr;

bool ParserMatches(void* parser,const room_random_client::Context& context) {
    if(!parser || !context.model)return false;
    auto* owner=room_random_client::Read<uint8_t*>(static_cast<uint8_t*>(parser),0x0C);
    return owner &&
        room_random_client::Read<uint8_t*>(owner,0x14)==context.model &&
        (!context.manager ||
         room_random_client::Read<uint8_t*>(
             static_cast<uint8_t*>(parser),0x10)==context.manager);
}
bool RandomRoom(const room_random_client::Context& context) {
    if(!context.room)return false;
    std::array<char,SDGO_ROOM_TITLE_MAX_UTF8_BYTES+1> title{};
    if(g_api && g_api->ModDirectory) {
        const auto length=sdgo::CurrentRoomTitle(
            g_api->ModDirectory(g_mod),title.data(),static_cast<uint32_t>(title.size()));
        if(length)return sdgo::random_loadout::Command(title.data(),length,g_api);
    }
    const auto length=room_random_client::Read<uint8_t>(context.room,0x79);
    return length<=SDGO_ROOM_TITLE_LEGACY_BYTES &&
        sdgo::random_loadout::Command(
            reinterpret_cast<const char*>(context.room+0x7A),length,g_api);
}
struct PacketWork {
    room_random_client::Result result;
    uint16_t opcode=0;
    const uint8_t* packet=nullptr;
    bool orderedStart=false;
};
PacketWork GuardedPacket(BYTE* frame) {
    PacketWork work;
    __try {
        work.opcode=static_cast<uint16_t>(room_random_client::Read<uint32_t>(frame,0x14));
        work.packet=room_random_client::Read<const uint8_t*>(frame,0x18);
        const auto context=room_random_client::Resolve(g_bindings);
        if(g_pending.Active() &&
           (!g_pending.Revalidate(context) || !ParserMatches(g_pending.Parser(),context)))
            g_pending.Clear();
        if(work.opcode!=0x0861 && work.opcode!=0x098B && work.opcode!=0x06C3)
            return work;
        // Repeated delivery of the exact held start belongs to that same
        // generation. Keep any fresh records already collected for it.
        if(work.opcode==0x06C3 && work.packet && g_pending.Active() &&
           std::memcmp(g_pending.Packet(),work.packet,
                       room_random_client::kStartPacketSize)==0)
            return work;
        work.orderedStart=work.opcode==0x06C3 &&
            context.model && RandomRoom(context);
        work.result=g_sync.Process(
            context,work.opcode,work.packet,work.orderedStart);
        // A new genuine start supersedes any older held parser generation. Its
        // own parser hook below will either forward it or retain a fresh copy.
        if(work.opcode==0x06C3) {
            g_pending.Clear();
        }
        return work;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_pending.Clear();work.result.reason=5;return work;
    }
}
void __stdcall OnPacket(BYTE* frame) {
    const DWORD error=GetLastError();
    EnterCriticalSection(&g_lock);
    const auto work=GuardedPacket(frame);
    LeaveCriticalSection(&g_lock);
    const auto& result=work.result;
    const auto opcode=work.opcode;
    if(result.applied) g_api->Log(g_mod,SDGO_LOG_INFO,"native %04X applied immediately: %u human loadout record(s), unit %lu, cache source %u",
        opcode,result.applied,static_cast<unsigned long>(result.model),result.source);
    else if(result.reason && (opcode==0x0861 || opcode==0x098B || opcode==0x06C3))
        g_api->Log(g_mod,SDGO_LOG_WARN,"native %04X cache update skipped: context reason %u",opcode,result.reason);
    if(opcode==0x06C3 && (result.applied || result.mismatched))
        g_api->Log(g_mod,result.mismatched && !work.orderedStart ? SDGO_LOG_ERROR : SDGO_LOG_INFO,
            "battle start cache: %u refreshed, %u mismatch; server unit %lu, cached unit %lu",
            result.applied,result.mismatched,static_cast<unsigned long>(result.model),static_cast<unsigned long>(result.cached));
    SetLastError(error);
}
// Interior site +5 coexists with match_ai's function-entry hook: its trampoline
// resumes at this site. Preserve the full native context before forwarding.
__declspec(naked) void DispatchSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax,esp }
    __asm { sub esp,210h }
    __asm { and esp,0FFFFFFF0h }
    __asm { mov [esp+200h],eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { push ebp }
    __asm { call OnPacket }
    __asm { fxrstor [esp] }
    __asm { mov esp,[esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_original] }
}
room_random_client::Construction GuardedConstruction(const uint8_t* source,
                                                     const uint8_t* entry,uint8_t deck) {
    __try {
        auto result=g_sync.PrepareConstruction(room_random_client::Resolve(g_bindings),source,entry,deck);
        if(result.reason==1 && g_sync.ObservedModel()) {
            // The static converter's ECX is not a room receiver. Revalidate
            // the manager/model generation captured from this start packet;
            // stale or allocator-reused objects must never authorize a copy.
            result=g_sync.PrepareConstruction(
                room_random_client::ResolveModel(
                    g_bindings,g_sync.ObservedModel(),
                    g_sync.ObservedManager()),
                source,entry,deck);
        }
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        room_random_client::Construction r;r.reason=7;return r;
    }
}
void __stdcall FillHuman(uint8_t* output,const uint8_t* source,
                          const uint8_t* entry,uint8_t deck) {
    const DWORD error=GetLastError();
    EnterCriticalSection(&g_lock);
    const auto replacement=GuardedConstruction(source,entry,deck);
    LeaveCriticalSection(&g_lock);
    if(replacement.applied) g_api->Log(g_mod,SDGO_LOG_INFO,
        "human construction owner %lu: snapshot %lu -> %lu, complete %s record",
        static_cast<unsigned long>(replacement.owner),static_cast<unsigned long>(replacement.before),
        static_cast<unsigned long>(replacement.after),deck ? "unit/ship" : "unit");
    else if(replacement.reason) g_api->Log(g_mod,SDGO_LOG_WARN,
        "human construction owner %lu: source unchanged, validation reason %u",
        static_cast<unsigned long>(replacement.owner),replacement.reason);
    SetLastError(error);
    // Original runs once, outside the diagnostic SEH/lock. Its strings and unit
    // attributes are constructed normally from the matching complete source.
    g_fill(output,replacement.applied ? replacement.pilot.data() : source,entry,deck);
}
struct BeginDecision {
    bool forward=true;
    bool random=false;
    bool ready=false;
    bool held=false;
    unsigned reason=0;
};
BeginDecision GuardedBegin(void* self,const uint8_t* packet) {
    BeginDecision decision;
    __try {
        if(!packet || room_random_client::Read<uint16_t>(packet,0)!=0x06C3)
            return decision;
        const auto context=room_random_client::Resolve(g_bindings);
        decision.random=RandomRoom(context);
        if(!decision.random) {
            g_pending.Clear();
            return decision;
        }
        if(!context.model || !ParserMatches(self,context)) {
            g_pending.Clear();decision.reason=1;return decision;
        }
        if(g_sync.ReadyForStart(context,packet) &&
           room_random_client::HumanStartTasksSettled(
               g_bindings,context,packet)) {
            g_pending.Clear();decision.ready=true;return decision;
        }
        if(g_pending.Hold(context,self,packet)) {
            decision.forward=false;decision.held=true;return decision;
        }
        decision.reason=2;return decision;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_pending.Clear();decision.reason=3;return decision;
    }
}
void __fastcall BeginStart(void* self,void*,const uint8_t* packet) {
    const DWORD error=GetLastError();
    EnterCriticalSection(&g_lock);
    const auto decision=GuardedBegin(self,packet);
    LeaveCriticalSection(&g_lock);
    if(decision.held)
        g_api->Log(g_mod,SDGO_LOG_WARN,
            "random battle start held until every human loadout is complete and native room tasks settle");
    else if(decision.random && decision.ready)
        g_api->Log(g_mod,SDGO_LOG_INFO,
            "random battle start already has complete human loadout records");
    else if(decision.random && decision.reason)
        g_api->Log(g_mod,SDGO_LOG_ERROR,
            "random battle start could not bind the live parser generation; forwarding unchanged (reason %u)",
            decision.reason);
    SetLastError(error);
    if(decision.forward)g_beginStart(self,packet);
}
bool GuardedChangeRoomView(uint8_t* self) {
    __try {
        if(!g_pending.Active())return false;
        const auto context=room_random_client::Resolve(g_bindings);
        if(!g_pending.Revalidate(context) ||
           !ParserMatches(g_pending.Parser(),context))
            return false;
        if(room_random_client::CurrentHumanRoomViewState(
               g_bindings,context,g_pending.Packet())!=self)
            return false;
        return room_random_client::PrimeRoomViewPayloadForNativeCleanup(self);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_pending.Clear();return false;
    }
}
int __fastcall ChangeRoomView(uint8_t* self,void*) {
    const DWORD error=GetLastError();
    NativeFpState fp;
    _fxsave(fp.bytes);
    EnterCriticalSection(&g_lock);
    const bool cancel=GuardedChangeRoomView(self);
    LeaveCriticalSection(&g_lock);
    if(cancel)
        g_api->Log(g_mod,SDGO_LOG_INFO,
            "random battle start retired one obsolete local room-view request through native state cleanup");
    SetLastError(error);
    _fxrstor(fp.bytes);
    if(cancel)return 0;
    return g_changeRoomView(self);
}
room_random_client::PendingCall GuardedManagerUpdate(uint8_t* self) {
    room_random_client::PendingCall call;
    __try {
        const auto context=room_random_client::Resolve(g_bindings);
        if(!g_pending.Active())return call;
        if(!g_pending.Revalidate(context) ||
           !ParserMatches(g_pending.Parser(),context)) {
            g_pending.Clear();return call;
        }
        if(context.manager==self &&
           g_sync.ReadyForStart(context,g_pending.Packet()) &&
           room_random_client::HumanStartTasksSettled(
               g_bindings,context,g_pending.Packet()))
            call=g_pending.Take(context,g_pending.Parser());
        return call;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_pending.Clear();return call;
    }
}
void __fastcall ManagerUpdate(uint8_t* self,void*) {
    g_managerUpdate(self);
    NativeFpState fp;
    _fxsave(fp.bytes);
    const DWORD error=GetLastError();
    EnterCriticalSection(&g_lock);
    const auto call=GuardedManagerUpdate(self);
    LeaveCriticalSection(&g_lock);
    if(call) {
        g_api->Log(g_mod,SDGO_LOG_INFO,
            "random battle start resumed after every human native loadout task and obsolete local room-view cleanup completed");
        SetLastError(error);
        _fxrstor(fp.bytes);
        g_beginStart(call.parser,call.packet.data());
        return;
    }
    SetLastError(error);
    _fxrstor(fp.bytes);
}
}
SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    if(api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite) ||
       !api->HookSite || !api->HookCode || !api->ModDirectory) return 0;
    g_bindings.profile=reinterpret_cast<uint8_t**>(api->Symbol(mod,"room_random.client_profile"));
    g_bindings.app=reinterpret_cast<uint8_t**>(api->Symbol(mod,"room_random.client_app"));
    g_bindings.waitRoomVtable=api->Symbol(mod,"room_random.client_waitroom_vtable");
    g_bindings.changeRoomViewVtable=
        api->Symbol(mod,"room_random.client_change_room_view_vtable");
    g_bindings.roomInfo=reinterpret_cast<room_random_client::RoomInfoFn>(api->Symbol(mod,"room_random.client_room_info"));
    g_bindings.managerUpdate=reinterpret_cast<room_random_client::ManagerUpdateFn>(
        api->Symbol(mod,"room_random.client_manager_update"));
    g_bindings.userHasState=reinterpret_cast<room_random_client::UserHasStateFn>(
        api->Symbol(mod,"room_random.client_user_has_state"));
    g_bindings.changeRoomView=
        reinterpret_cast<room_random_client::ChangeRoomViewFn>(
            api->Symbol(mod,"room_random.client_change_room_view"));
    if(!g_bindings.profile || !g_bindings.app || !g_bindings.waitRoomVtable ||
       !g_bindings.changeRoomViewVtable || !g_bindings.changeRoomView ||
       !g_bindings.roomInfo || !g_bindings.managerUpdate ||
       !g_bindings.userHasState) return 0;
    auto bindSlots=[&](room_random_client::SlotAccess& s,const char* prefix) {
        char name[96];
        auto symbol=[&](const char* key) {sprintf_s(name,"room_random.client_%s_%s",prefix,key);return api->Symbol(mod,name);};
        s.owner=reinterpret_cast<room_random_client::SlotAccess::Owner>(symbol("owner"));
        s.active=reinterpret_cast<room_random_client::SlotAccess::Get>(symbol("unit"));
        s.ship=reinterpret_cast<room_random_client::SlotAccess::Get>(symbol("ship"));
        s.setActive=reinterpret_cast<room_random_client::SlotAccess::Set>(symbol("set_unit"));
        s.setShip=reinterpret_cast<room_random_client::SlotAccess::Set>(symbol("set_ship"));
        return s.owner && s.active && s.ship && s.setActive && s.setShip;
    };
    if(!bindSlots(g_bindings.normal,"normal") || !bindSlots(g_bindings.mission,"mission"))return 0;
    InitializeCriticalSection(&g_lock);
    if(!api->HookSite(mod,"room_random.client_dispatch",reinterpret_cast<void*>(&DispatchSite),&g_original)) return 0;
    if(!api->HookCode(mod,"room_random.client_fill_human",reinterpret_cast<void*>(&FillHuman),reinterpret_cast<void**>(&g_fill))) return 0;
    if(!api->HookCode(mod,"room_random.client_begin_start",reinterpret_cast<void*>(&BeginStart),reinterpret_cast<void**>(&g_beginStart))) return 0;
    if(!api->HookCode(mod,"room_random.client_manager_update",
                      reinterpret_cast<void*>(&ManagerUpdate),
                      reinterpret_cast<void**>(&g_managerUpdate))) return 0;
    if(!api->HookCode(mod,"room_random.client_change_room_view",
                      reinterpret_cast<void*>(&ChangeRoomView),
                      reinterpret_cast<void**>(&g_changeRoomView))) return 0;
    api->Log(mod,SDGO_LOG_INFO,
        "native random starts wait for complete human loadouts, retire obsolete local room-view requests through native cleanup, and execute the original parser exactly once");
    return 1;
}
