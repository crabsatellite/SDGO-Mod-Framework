#include <windows.h>
#include <intrin.h>
#include "native_form_gate.h"
#include "sdgo/native_unit_form_composition.h"
#include "sdgo/native_unit_form_commit.h"

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
alignas(8) volatile LONG64 rebindAttempts=0,rebound=0;
volatile LONG lastRoot=0,lastCurrent=0,lastTarget=0,lastStage=0,lastFault=0,lastCommit=0;
bool installed=false,composerReady=false;
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
NativeBindings composer;
volatile LONG compositionBusy=0;
struct CompositionLease {
    const bool held=InterlockedCompareExchange(&compositionBusy,1,0)==0;
    ~CompositionLease(){if(held)InterlockedExchange(&compositionBusy,0);}
};
bool CopySlot(const void* address,Slot* output){
    __try{std::memcpy(output->data(),address,output->size());return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// A graph-allowed target absent from the cached native banks is composed as a
// fresh bank and written over the inactive bank, replacing the retired form.
// The original handler then accepts it as a cached transform. The retired
// form's ammo/cooldown state is discarded; the active bank, HP, skills and
// inventory identity are untouched.
bool ComposeInactive(const NativeTransformContext& context,const HumanUnitCaptureSnapshot& snapshot){
    // The native callbacks' cross-room reentrancy is not established. Refuse
    // a concurrent composition rather than wait while holding room locks.
    CompositionLease lease;
    if(!lease.held)return false;
    InterlockedIncrement64(&rebindAttempts);
    InterlockedExchange(&lastRoot,context.root);InterlockedExchange(&lastCurrent,context.current);
    InterlockedExchange(&lastTarget,context.target);InterlockedExchange(&lastStage,1);
    InterlockedExchange(&lastFault,0);InterlockedExchange(&lastCommit,0);
    if(context.active>1||context.forms[context.active]!=context.current||context.target==context.forms[0]||
       context.target==context.forms[1]||context.target==context.current||!expected->count(context.target))return false;
    InterlockedExchange(&lastStage,2);Slot before{},staged{};
    if(!CopySlot(context.slot,&before))return false;
    Bank targetBank{};std::uint32_t fault=0;
    const auto composed=ComposeNativeBank(composer,snapshot.unit.composition,context.target,targetBank,fault);
    InterlockedExchange(&lastFault,fault);InterlockedExchange(&lastStage,3);
    if(composed!=CompositionResult::composed)return false;
    const auto stagedResult=StageInactive(before,context.root,context.current,targetBank,staged);
    if(stagedResult==StageResult::already_cached){InterlockedExchange(&lastStage,7);return true;}
    if(stagedResult!=StageResult::prepared)return false;
    InterlockedExchange(&lastStage,4);
    NativeTransformContext current;
    const auto index=(reinterpret_cast<std::uintptr_t>(context.player)-reinterpret_cast<std::uintptr_t>(context.room)-0x3ce4)/0x1f54;
    if(!ReadNativeTransformContext(context.room,static_cast<std::uint32_t>(index),context.slot,context.target,current)||
       CheckUnitForm(*graphs,*expected,current,snapshot,lookup).decision!=PreflightDecision::native_uncached||
       !capture->Current(snapshot))return false;
    InterlockedExchange(&lastStage,5);
    // A matching record at a new address must not leave the freshly composed
    // target with an old borrowed pointer. The supported registry remains a
    // process-lifetime table; this is not a concurrent catalog-reload protocol.
    if(lookup.find(context.target)!=reinterpret_cast<const void*>(Read32(targetBank.bytes.data()+8)))return false;
    InterlockedExchange(&lastStage,6);
    const auto commit=CommitInactiveBank(const_cast<void*>(context.slot),before,staged);
    InterlockedExchange(&lastCommit,static_cast<LONG>(commit));
    if(commit==IdentityCommit::unrecoverable){InterlockedExchange(&lastStage,9);RaiseFailFastException(nullptr,nullptr,0);}
    if(commit!=IdentityCommit::committed){InterlockedExchange(&lastStage,8);return false;}
    InterlockedIncrement64(&rebound);InterlockedExchange(&lastStage,7);return true;
}
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
                            // An uncached but allowed edge is composed into the inactive bank
                            // first; when that is not possible the original handler keeps
                            // its own uncached refusal, so nothing beyond the original happens.
                            if(policy.decision==PreflightDecision::native_uncached&&composerReady)ComposeInactive(context,snapshot);
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
    // Composing uncached graph targets needs the composer symbols; without them the gate
    // still runs its preflight and the original handler keeps refusing uncached forms.
    const auto symbol=[&](const char* name,std::uint32_t rva){auto* value=api->Symbol(mod,name);return value==base+rva?value:nullptr;};
    composer.unit_registry=registry;composer.find=lookup.query;composer.map_hash=hash;composer.map_compare=compare;
    composer.weapon_registry=symbol("unit_form.weapon_registry",0x42aa2c);
    composer.stats=reinterpret_cast<NativeBindings::Stats>(symbol("unit_form.stats",0xd6790));
    composer.weapons=reinterpret_cast<NativeBindings::Weapons>(symbol("unit_form.weapons",0xd8280));
    composer.boost=reinterpret_cast<NativeBindings::Boost>(symbol("unit_form.boost",0x79120));
    composer.deck_boost=reinterpret_cast<NativeBindings::DeckBoost>(symbol("unit_form.deck_boost",0x79170));
    composer.reset=reinterpret_cast<NativeBindings::Reset>(symbol("unit_form.reset",0x7a9f0));
    composer.mode_singleton=reinterpret_cast<void*const*>(symbol("unit_form.mode_singleton",0x42ee70));
    composerReady=composer.weapon_registry&&composer.stats&&composer.weapons&&composer.boost&&composer.deck_boost&&composer.reset&&composer.mode_singleton;
    if(!composerReady)api->Log(mod,SDGO_LOG_WARN,"unit form composer symbols differ; third-form chains stay refused by the original handler");
    if(!api->HookSite(mod,"unit_form.gate",reinterpret_cast<void*>(&Gate),&original)){
        error="native form gate installation was refused";return false;}
    installed=true;return true;
}
SdgoUnitFormGateStateV1 ReadFormGateState(){
    SdgoUnitFormGateStateV1 state{};state.size=sizeof(state);state.version=1;state.installed=installed?1u:0u;
    state.expected_definitions=expected?static_cast<std::uint32_t>(expected->size()):0;
    state.calls=Counter(&calls);state.unmanaged=Counter(&unmanaged);state.cached_allowed=Counter(&cached);
    state.uncached_directed=Counter(&uncached);state.rejected=Counter(&denied);state.stale_capture=Counter(&stale);
    state.definition_checks=Counter(&checks);state.normalization_observations=Counter(&normalized);state.actor_writes=Counter(&rebound);return state;
}
SdgoUnitFormRebindStateV1 ReadFormRebindState(){
    // compiled_for_testing reports whether native bank composition is available.
    SdgoUnitFormRebindStateV1 state{};state.size=sizeof(state);state.version=1;state.compiled_for_testing=composerReady?1u:0u;
    const auto word=[](volatile LONG* value){return static_cast<std::uint32_t>(InterlockedCompareExchange(value,0,0));};
    state.last_root=word(&lastRoot);state.last_current=word(&lastCurrent);state.last_target=word(&lastTarget);
    state.last_stage=word(&lastStage);state.last_native_fault=word(&lastFault);state.last_commit_result=word(&lastCommit);
    state.attempts=Counter(&rebindAttempts);state.committed=Counter(&rebound);return state;
}
} // namespace unit_catalog_server
