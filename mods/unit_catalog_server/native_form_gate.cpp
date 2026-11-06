#include <windows.h>
#include <intrin.h>
#include "native_form_gate.h"

namespace unit_catalog_server {
namespace {
using namespace sdgo::unit_forms;
struct Registers {std::uint32_t edi,esi,ebp,savedEsp,ebx,edx,ecx,eax,flags;};
struct Frame {const void* room=nullptr;const void* slot=nullptr;std::uint32_t index=0,target=0,root=0,owner=0;};
std::shared_ptr<const GraphBindings> graphs;
const std::map<std::uint32_t,BattleDefinition>* expected=nullptr;
const HumanBuildCapture* capture=nullptr;
void* original=nullptr;void* rejected=nullptr;
alignas(8) volatile LONG64 calls=0,unmanaged=0,cached=0,uncached=0,denied=0,stale=0,checks=0,normalized=0;
bool installed=false;
alignas(16) const std::uint32_t defaultMxcsr=0x1f80;
class Lookup final:public UnitDefinitionLookup {
public:
    using Find=void*(__thiscall*)(void*,std::uint32_t);
    void* registry=nullptr;Find query=nullptr;const void* hash=nullptr;const void* compare=nullptr;
    bool ready()const {
        const auto* map=static_cast<const std::uint8_t*>(registry)+0x18;
        const auto buckets=Read32(map+0x1c);
        return Read32(map+8)==4&&Read32(map+0x14)==reinterpret_cast<std::uintptr_t>(hash)&&
            Read32(map+0x18)==reinterpret_cast<std::uintptr_t>(compare)&&buckets&&buckets<=1048576&&Read32(map+0x20);
    }
    const void* find(std::uint32_t id)const override {
        __try{if(!ready())return nullptr;const auto* result=query(registry,id);return ready()?result:nullptr;}
        __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
} lookup;
bool ReadFrame(const Registers* r,Frame* frame) {
    __try {
        frame->room=*reinterpret_cast<const void*const*>(r->ebp-0x18);
        frame->index=*reinterpret_cast<const std::uint32_t*>(r->ebp-0x14);
        frame->slot=*reinterpret_cast<const void*const*>(r->ebp-0x328a8);
        const auto* packet=*reinterpret_cast<const std::uint8_t*const*>(r->ebp-0x328b0);
        if(!frame->room||!frame->slot||frame->index>=12||!packet)return false;
        frame->root=Read32(static_cast<const std::uint8_t*>(frame->slot));
        frame->owner=Read32(static_cast<const std::uint8_t*>(frame->room)+0x3ce4+frame->index*0x1f54);
        frame->target=Read32(packet+0xa);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
int __cdecl Evaluate(const Registers* registers) {
    const DWORD error=GetLastError();int result=0;InterlockedIncrement64(&calls);
    try {
        Frame frame;
        // Unknown/unmanaged/NPC contexts gain no extra capability: retain the
        // original handler. An identified managed human fails closed below.
        if(!ReadFrame(registers,&frame)||!graphs->hasEntry(frame.root)||(frame.owner&0x80000000u))InterlockedIncrement64(&unmanaged);
        else {
            result=1;NativeTransformContext context;
            if(ReadNativeTransformContext(frame.room,frame.index,frame.slot,frame.target,context)){
                HumanUnitCaptureSnapshot snapshot;
                if(capture->Snapshot(context.room,context.player,context.deck,context.root,context.serial,snapshot)){
                    InterlockedIncrement64(&checks);const auto policy=CheckUnitForm(*graphs,*expected,context,snapshot,lookup);
                    InterlockedAdd64(&normalized,policy.normalizations);
                    if(policy.decision==PreflightDecision::native_cached||policy.decision==PreflightDecision::native_uncached){
                        if(capture->Current(snapshot)){
                            InterlockedIncrement64(policy.decision==PreflightDecision::native_cached?&cached:&uncached);
                            result=0;
                        }else InterlockedIncrement64(&stale);
                    }
                }else InterlockedIncrement64(&stale);
            }
            if(result)InterlockedIncrement64(&denied);
        }
    }catch(...){result=1;InterlockedIncrement64(&denied);}
    SetLastError(error);return result;
}
__declspec(naked) void Gate(){
    __asm{pushfd}
    __asm{pushad}
    __asm{mov eax,esp}
    __asm{sub esp,210h}
    __asm{and esp,0fffffff0h}
    __asm{mov [esp+200h],eax}
    __asm{fxsave [esp]}
    __asm{fninit}
    __asm{ldmxcsr dword ptr [defaultMxcsr]}
    __asm{cld}
    __asm{push eax}
    __asm{call Evaluate}
    __asm{add esp,4}
    __asm{test eax,eax}
    __asm{jnz refused}
    __asm{fxrstor [esp]}
    __asm{mov esp,[esp+200h]}
    __asm{popad}
    __asm{popfd}
    __asm{jmp dword ptr [original]}
refused:
    __asm{fxrstor [esp]}
    __asm{mov esp,[esp+200h]}
    __asm{popad}
    __asm{popfd}
    // Let the original function release its lock and unwind its own SEH.
    __asm{jmp dword ptr [rejected]}
}
std::uint64_t Counter(volatile LONG64* value){return static_cast<std::uint64_t>(InterlockedCompareExchange64(value,0,0));}
}
bool InstallFormGate(const SdgoApi* api,SdgoMod* mod,std::shared_ptr<const GraphBindings> bindings,
                     const std::map<std::uint32_t,BattleDefinition>* definitions,
                     const HumanBuildCapture* lifecycle,std::string& error) {
    error.clear();
    if(installed||!api||api->version!=1||api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite)||!api->HookSite||
       !api->Symbol||!api->host_base||!api->host_key||std::strcmp(api->host_key,"BattleServer-548A891D-0046C000")||
       !bindings||!definitions||definitions->empty()||!lifecycle){error="native form gate prerequisites differ";return false;}
    const auto* base=static_cast<const std::uint8_t*>(api->host_base);
    auto* gate=api->Symbol(mod,"unit_form.gate");auto* failure=api->Symbol(mod,"unit_form.reject");
    auto* registry=api->Symbol(mod,"unit_form.registry");auto* find=api->Symbol(mod,"unit_form.find");
    auto* hash=api->Symbol(mod,"unit_form.hash");auto* compare=api->Symbol(mod,"unit_form.compare");
    if(gate!=base+0xcace0||failure!=base+0xcaf12||registry!=base+0x42aab8||find!=base+0x64ed0||
       hash!=base+0x321b30||compare!=base+0x321ae0){
        error="native form gate symbol boundaries differ";return false;}
    graphs=std::move(bindings);expected=definitions;capture=lifecycle;rejected=failure;lookup.registry=registry;lookup.query=reinterpret_cast<Lookup::Find>(find);lookup.hash=hash;lookup.compare=compare;
    if(!api->HookSite(mod,"unit_form.gate",reinterpret_cast<void*>(&Gate),&original)){
        error="native form gate installation was refused";return false;}
    installed=true;return true;
}
SdgoUnitFormGateStateV1 ReadFormGateState(){
    SdgoUnitFormGateStateV1 state{};state.size=sizeof(state);state.version=1;state.installed=installed?1u:0u;
    state.expected_definitions=expected?static_cast<std::uint32_t>(expected->size()):0;
    state.calls=Counter(&calls);state.unmanaged=Counter(&unmanaged);state.cached_allowed=Counter(&cached);
    state.uncached_directed=Counter(&uncached);state.rejected=Counter(&denied);state.stale_capture=Counter(&stale);
    state.definition_checks=Counter(&checks);state.normalization_observations=Counter(&normalized);return state;
}
} // namespace unit_catalog_server
