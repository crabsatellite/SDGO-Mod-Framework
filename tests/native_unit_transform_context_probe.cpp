// Unshipped, read-only observation at the original authorized/locked transform
// boundary. The original handler and notification execute without modification.
#include <windows.h>
#include <intrin.h>
#include <share.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/native_unit_transform_context.h"
#include "sdgo/unit_form_runtime_state.h"
namespace {
struct Registers { std::uint32_t edi,esi,ebp,savedEsp,ebx,edx,ecx,eax,flags; };
struct Event {
    DWORD thread=0;
    bool frame=false,read=false,identity=false;
    unsigned index=0;
    sdgo::unit_forms::NativeTransformContext context;
    SdgoUnitFormRuntimeStateV1 capture{};
};
struct Cell { std::atomic<std::uint64_t> ready{0};Event event; };
constexpr std::size_t capacity=128;
std::array<Cell,capacity> queue;
std::atomic<std::uint64_t> head{0},tail{0};
std::atomic<bool> broken{false};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"native diagnostic queue must not lock");
void* original=nullptr;
SdgoReadUnitFormRuntimeStateV1Fn readState=nullptr;
char run[80]{};std::wstring output;
HANDLE stop=nullptr;
alignas(16) const std::uint32_t defaultMxcsr=0x1f80;
bool ReadFrame(const Registers* r,Event* event) {
    __try {
        const auto* room=*reinterpret_cast<const void*const*>(r->ebp-0x18);
        event->index=*reinterpret_cast<const std::uint32_t*>(r->ebp-0x14);
        const auto* slot=*reinterpret_cast<const void*const*>(r->ebp-0x328a8);
        const auto* packet=*reinterpret_cast<const std::uint8_t*const*>(r->ebp-0x328b0);
        const auto target=sdgo::unit_forms::Read32(packet+0xa);
        event->read=sdgo::unit_forms::ReadNativeTransformContext(room,event->index,slot,target,event->context);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __cdecl Observe(const Registers* registers) {
    const auto error=GetLastError();
    if(!broken.load(std::memory_order_acquire)) {
        const auto position=head.fetch_add(1,std::memory_order_acq_rel);
        if(position==UINT64_MAX||position-tail.load(std::memory_order_acquire)>=capacity)broken=true;
        else {
            auto& cell=queue[position%capacity];cell.event=Event{};auto& event=cell.event;
            event.thread=GetCurrentThreadId();event.frame=ReadFrame(registers,&event);
            event.capture.size=sizeof(event.capture);event.capture.version=1;
            if(event.frame&&event.read&&readState(&event.capture)) {
                const auto& context=event.context;const auto& state=event.capture;
                event.identity=state.capture_installed==1&&state.observer_healthy==1&&state.live_transform_commits==0&&
                    state.sample_root==context.root&&state.sample_owner==context.owner&&state.sample_deck==context.deck&&
                    state.sample_room==reinterpret_cast<std::uintptr_t>(context.room)&&
                    state.sample_player==reinterpret_cast<std::uintptr_t>(context.player)&&
                    state.sample_serial==context.serial&&state.sample_room_epoch&&state.sample_generation;
            }
            cell.ready.store(position+1,std::memory_order_release);
        }
    }
    SetLastError(error);
}
__declspec(naked) void Gate() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax,esp }
    __asm { sub esp,210h }
    __asm { and esp,0fffffff0h }
    __asm { mov [esp+200h],eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [defaultMxcsr] }
    __asm { cld }
    __asm { push eax }
    __asm { call Observe }
    __asm { add esp,4 }
    __asm { fxrstor [esp] }
    __asm { mov esp,[esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [original] }
}
DWORD WINAPI Pump(void*) {
    FILE* file=_wfsopen(output.c_str(),L"wb",_SH_DENYNO);if(!file){broken=true;return 1;}
    std::fprintf(file,"{\"event\":\"probe_ready\",\"run_id\":\"%s\",\"pid\":%lu,\"actor_writes\":false}\n",run,GetCurrentProcessId());std::fflush(file);
    while(WaitForSingleObject(stop,10)==WAIT_TIMEOUT) {
        auto position=tail.load(std::memory_order_relaxed);const auto limit=head.load(std::memory_order_acquire);
        while(position<limit) {
            auto& cell=queue[position%capacity];if(cell.ready.load(std::memory_order_acquire)!=position+1)break;
            const auto& event=cell.event;const auto& c=event.context;const auto& s=event.capture;
            std::fprintf(file,"{\"event\":\"locked_transform_context\",\"run_id\":\"%s\",\"pid\":%lu,\"thread\":%lu,\"frame\":%s,\"read\":%s,\"capture_identity\":%s,\"queue_healthy\":%s,\"human_slot\":%u,\"room\":%u,\"player\":%u,\"slot\":%u,\"owner\":%u,\"root\":%u,\"serial\":%llu,\"deck\":%u,\"mode\":%u,\"active\":%u,\"current\":%u,\"target\":%u,\"hp\":%u,\"forms\":[%u,%u],\"definitions\":[%u,%u],\"targets\":[%u,%u],\"room_epoch\":%llu,\"generation\":%llu,\"actor_writes\":false}\n",
                run,GetCurrentProcessId(),event.thread,event.frame?"true":"false",event.read?"true":"false",event.identity?"true":"false",broken.load()?"false":"true",event.index,
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(c.room)),static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(c.player)),static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(c.slot)),
                c.owner,c.root,c.serial,c.deck,c.mode,c.active,c.current,c.target,c.hp,c.forms[0],c.forms[1],
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(c.definitions[0])),static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(c.definitions[1])),c.targets[0],c.targets[1],s.sample_room_epoch,s.sample_generation);
            std::fflush(file);++position;tail.store(position,std::memory_order_release);
        }
    }
    std::fclose(file);return 0;
}
}
SDGO_MOD_ENTRY {
    if(!api||api->version!=1||api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite)||!api->HookSite||
       !api->host_key||std::strcmp(api->host_key,"BattleServer-548A891D-0046C000"))return 0;
    api->SettingString(mod,"RunId","",run,sizeof(run));if(std::strlen(run)!=36)return 0;
    for(const char* c=run;*c;++c)if(!((*c>='0'&&*c<='9')||(*c>='a'&&*c<='f')||*c=='-'))return 0;
    if(api->Symbol(mod,"probe.transform_gate")!=static_cast<const std::uint8_t*>(api->host_base)+0xcace0)return 0;
    const auto module=GetModuleHandleW(L"unit_catalog_server.dll");if(!module)return 0;
    readState=reinterpret_cast<SdgoReadUnitFormRuntimeStateV1Fn>(GetProcAddress(module,"SdgoReadUnitFormRuntimeStateV1"));if(!readState)return 0;
    output=std::wstring(api->ModDirectory(mod))+L"\\transform-"+std::wstring(run,run+36)+L".jsonl";
    if(GetFileAttributesW(output.c_str())!=INVALID_FILE_ATTRIBUTES)return 0;
    stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stop)return 0;
    HANDLE thread=CreateThread(nullptr,0,&Pump,nullptr,0,nullptr);if(!thread){CloseHandle(stop);stop=nullptr;return 0;}
    if(!api->HookSite(mod,"probe.transform_gate",reinterpret_cast<void*>(&Gate),&original)) {
        SetEvent(stop);WaitForSingleObject(thread,INFINITE);CloseHandle(thread);CloseHandle(stop);stop=nullptr;return 0;
    }
    CloseHandle(thread);return 1;
}
