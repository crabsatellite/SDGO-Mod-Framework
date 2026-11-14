#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <map>
#include <vector>
#include <utility>
#include "sdgo/unit_form_models.h"
#include "sdgo/unit_transform_recovery.h"
#include "form_chain_runtime.h"
#include "stage_runtime.h"

// Static PE export for authoring compatibility checks; readers do not execute
// the DLL. Old single-extra-model runtimes must not publish general graphs.
extern "C" __declspec(dllexport) const struct {
    char magic[8];std::uint32_t size,version,capacity,cached,reuse,cleanup;
} SdgoUnitDirectedFormsCapabilityV1={{'S','D','G','O','D','I','R','1'},32,1,64,
    static_cast<std::uint32_t>(sdgo::unit_form_models::kMaxCachedModels),1,1};

namespace sdgo::unit_form_chain {
namespace {
// Supported original client layout (GOnline-54BE1568-00C97000), see docs/unit-import.md.
constexpr std::size_t kCurrentModel=0x34,kCurrentRecord=0x9c4,kMoveController=0x9c8,kAttackController=0x9cc;
constexpr std::size_t kAlternateRecord=0xab0,kAlternateModel=0xab4,kTransform=0xe34;
constexpr std::size_t kAlternateStatA=0x2d00,kAlternateStatB=0x2d02,kAlternateFatalCost=0x2d04;
constexpr std::size_t kDeckBegin=0x2ed0,kDeckEnd=0x2ed4,kDeckIndex=0x2ee0,kDeckEntryBytes=0x60c;
constexpr std::size_t kControllerTarget=0x2b0;
constexpr std::size_t kRecordTransform=0x13c,kRecordFatalWeapon=0x255,kRecordStatA=0x22c,kRecordStatB=0x22e;
constexpr std::size_t kWeaponCost=0x6c;
constexpr std::uint32_t kUnitKind=2,kWeaponKind=10;

using Swap=void(__fastcall*)(std::uint8_t*,void*);
using Destroy=void(__fastcall*)(std::uint8_t*,void*);
using WeaponTables=void(__thiscall*)(void*);
// CUnit::CreateModel(this, record, &model): thiscall on the battle unit (it reads this+0xD4).
using ModelCreate=void(__thiscall*)(void*,const std::uint8_t*,void**);

struct CachedModel { const std::uint8_t* record=nullptr;void* model=nullptr; };
struct Chain { std::map<std::uint32_t,CachedModel> models; };

const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
const unit_catalog::Catalog* g_catalog=nullptr;
GridData g_data=nullptr;GridFind g_find=nullptr;
Swap g_swap=nullptr;Destroy g_destroy=nullptr;
WeaponTables g_weaponTables=nullptr;ModelCreate g_modelCreate=nullptr;
CRITICAL_SECTION g_lock;
std::map<std::uint8_t*,Chain>* g_chains=nullptr; // Allocated at attach; never destroyed with the process.
bool g_installed=false;
alignas(8) volatile LONG64 g_swapsSeen=0,g_rebased=0,g_refused=0,g_modelsCreated=0,g_modelsReleased=0,g_unitsRestored=0,g_normalized=0;
volatile LONG g_lastUnit=0,g_lastCurrent=0,g_lastTarget=0,g_lastRefusal=0;

template<class T> T Read(const std::uint8_t* base,std::size_t offset){T value;std::memcpy(&value,base+offset,sizeof(value));return value;}
template<class T> void Write(std::uint8_t* base,std::size_t offset,T value){std::memcpy(base+offset,&value,sizeof(value));}

const std::uint8_t* Record(std::uint32_t id,std::uint32_t kind){
    const std::uint8_t* record=nullptr;
    if(!id||!g_find(g_data(),&record,id,static_cast<int>(kind),0))return nullptr;
    return record;
}

struct Hop { std::uint32_t current=0,target=0;const std::uint8_t* record=nullptr;std::uint8_t* unit=nullptr; };

// SEH-guarded reads of the live actor; no C++ objects with destructors inside.
bool ReadHop(std::uint8_t* unit,Hop* hop){
    __try{
        // Any Trans state: after the entry hop the pair form is the base form (Trans=0)
        // while the alternate slot still holds the entry form, so the hop is keyed on
        // the controller's target alone.
        const auto* current=Read<const std::uint8_t*>(unit,kCurrentRecord);
        const auto* alternate=Read<const std::uint8_t*>(unit,kAlternateRecord);
        const auto* controller=Read<const std::uint8_t*>(unit,kMoveController);
        if(!current||!alternate||!controller)return false;
        const auto target=Read<std::uint32_t>(controller,kControllerTarget);
        if(!target||target==Read<std::uint32_t>(current,0)||target!=Read<std::uint32_t>(current,kRecordTransform)||target==Read<std::uint32_t>(alternate,0))return false;
        hop->current=Read<std::uint32_t>(current,0);hop->target=target;hop->unit=unit;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

struct DeckSnapshot {
    unit_form_models::Pair pairs[unit_form_models::kMaxDeckEntries]{};
    std::size_t count=0,selected=0;std::uint8_t* entry=nullptr;
    void* current=nullptr;void* alternate=nullptr;
};

bool ReadDeck(std::uint8_t* unit,DeckSnapshot* output){
    __try{
        auto* begin=Read<std::uint8_t*>(unit,kDeckBegin);auto* end=Read<std::uint8_t*>(unit,kDeckEnd);
        const auto first=reinterpret_cast<std::uintptr_t>(begin),last=reinterpret_cast<std::uintptr_t>(end);
        const auto index=Read<std::int32_t>(unit,kDeckIndex);
        if(!begin||last<first||(last-first)%kDeckEntryBytes||index<0)return false;
        const auto count=(last-first)/kDeckEntryBytes;
        if(!count||count>unit_form_models::kMaxDeckEntries||static_cast<std::size_t>(index)>=count)return false;
        output->count=count;output->selected=static_cast<std::size_t>(index);
        output->entry=begin+output->selected*kDeckEntryBytes;
        output->current=Read<void*>(unit,kCurrentModel);output->alternate=Read<void*>(unit,kAlternateModel);
        for(std::size_t i=0;i<count;++i){
            output->pairs[i].base=Read<std::uintptr_t>(begin+i*kDeckEntryBytes,4);
            output->pairs[i].alternate=Read<std::uintptr_t>(begin+i*kDeckEntryBytes,12);
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

// A cycle may revisit either original form after several hops. Borrow its
// existing deck-owned model rather than create a second owner for that form.
void* NativeModel(std::uint8_t* unit,std::uint32_t identity){
    DeckSnapshot snapshot;
    if(!ReadDeck(unit,&snapshot))return nullptr;
    __try{
        for(const auto offset:{0u,8u}){
            const auto* record=Read<const std::uint8_t*>(snapshot.entry,offset);
            if(record&&Read<std::uint32_t>(record,0)==identity)return Read<void*>(snapshot.entry,offset+4);
        }
        return nullptr;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}

bool ApplyHop(std::uint8_t* unit,const std::uint8_t* record,void* model){
    __try{
        Write(unit,kAlternateRecord,record);
        Write(unit,kAlternateModel,model);
        Write<std::uint8_t>(unit,kTransform,0); // The native swap toggles it back to 1: B is A's alternate.
        Write(unit,kAlternateStatA,Read<std::uint16_t>(record,kRecordStatA));
        Write(unit,kAlternateStatB,Read<std::uint16_t>(record,kRecordStatB));
        if(const auto* fatal=Record(Read<std::uint32_t>(record,kRecordFatalWeapon),kWeaponKind))
            Write(unit,kAlternateFatalCost,static_cast<std::int32_t>(static_cast<float>(Read<std::int32_t>(fatal,kWeaponCost))));
        if(auto* attack=Read<void*>(unit,kAttackController))g_weaponTables(attack);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

// After the entry hop (E -> A) the native pair is (A, E) with Trans=1, which makes the
// next request take the MA -> MS path: it plays change_ms on A and waits for its
// animation event, but the pair's transform animations live on B. Treat A as the
// base form instead: Trans=0 with the weapon tables rebuilt as (A, E), so A -> B
// takes the immediate MS -> MA path and B -> A plays change_ms on B.
bool NormalizeAfterEntryHop(std::uint8_t* unit){
    __try{
        if(!Read<std::uint8_t>(unit,kTransform))return false;
        const auto* current=Read<const std::uint8_t*>(unit,kCurrentRecord);
        const auto* alternate=Read<const std::uint8_t*>(unit,kAlternateRecord);
        if(!current||!alternate)return false;
        const auto next=Read<std::uint32_t>(current,kRecordTransform);
        if(!next||next==Read<std::uint32_t>(alternate,0)||next==Read<std::uint32_t>(current,0))return false;
        if(!g_catalog->Contains(Read<std::uint32_t>(current,0),kUnitKind))return false;
        Write<std::uint8_t>(unit,kTransform,0);
        if(auto* attack=Read<void*>(unit,kAttackController))g_weaponTables(attack);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

bool CommittedChainTarget(const std::uint8_t* unit){
    __try{
        const auto* current=Read<const std::uint8_t*>(unit,kCurrentRecord);
        const auto* alternate=Read<const std::uint8_t*>(unit,kAlternateRecord);
        const auto* move=Read<const std::uint8_t*>(unit,kMoveController);
        if(!current||!alternate||!move||Read<const void*>(move,0x21c)!=unit||Read<std::int32_t>(unit,0x38)<=0)return false;
        const auto id=Read<std::uint32_t>(current,0),peer=Read<std::uint32_t>(alternate,0);
        return id!=peer&&Read<std::uint32_t>(move,kControllerTarget)==id&&
            Read<std::uint32_t>(alternate,kRecordTransform)==id&&
            g_catalog->Contains(id,kUnitKind)&&g_catalog->Contains(peer,kUnitKind)&&
            Record(id,kUnitKind)==current&&Record(peer,kUnitKind)==alternate;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

void __fastcall OnSwap(std::uint8_t* unit,void* edx){
    InterlockedIncrement64(&g_swapsSeen);
    Hop hop;
    if(ReadHop(unit,&hop)){
        InterlockedExchange(&g_lastUnit,static_cast<LONG>(hop.current));InterlockedExchange(&g_lastCurrent,static_cast<LONG>(hop.current));
        InterlockedExchange(&g_lastTarget,static_cast<LONG>(hop.target));
        // Only imported chain forms are rebased; a stock pair keeps the original behaviour.
        if(!g_catalog->Contains(hop.target,kUnitKind)){InterlockedExchange(&g_lastRefusal,3);InterlockedIncrement64(&g_refused);}
        else{
            const auto* record=Record(hop.target,kUnitKind);
            void* model=nullptr;
            g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: hop %lu -> %lu unit=%p record=%p controller=%p",
                       static_cast<unsigned long>(hop.current),static_cast<unsigned long>(hop.target),unit,record,
                       Read<void*>(unit,kAttackController));
            if(!record){InterlockedExchange(&g_lastRefusal,1);InterlockedIncrement64(&g_refused);}
            else{
                EnterCriticalSection(&g_lock);
                auto& chain=(*g_chains)[unit];
                const auto cached=chain.models.find(hop.target);
                if(cached!=chain.models.end()){
                    if(cached->second.record==record)model=cached->second.model;
                }else model=NativeModel(unit,hop.target);
                if(!model&&cached==chain.models.end()&&chain.models.size()<unit_form_models::kMaxCachedModels){
                    g_modelCreate(unit,record,&model);
                    g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: model for %lu created at %p",static_cast<unsigned long>(hop.target),model);
                    if(model){chain.models.emplace(hop.target,CachedModel{record,model});InterlockedIncrement64(&g_modelsCreated);}
                }
                LeaveCriticalSection(&g_lock);
                if(!model){InterlockedExchange(&g_lastRefusal,2);InterlockedIncrement64(&g_refused);}
                else if(ApplyHop(unit,record,model)){
                    InterlockedExchange(&g_lastRefusal,0);InterlockedIncrement64(&g_rebased);
                    g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: %lu -> %lu rebased the alternate pair; swapping",
                               static_cast<unsigned long>(hop.current),static_cast<unsigned long>(hop.target));
                }
            }
        }
    }
    g_swap(unit,edx);
    g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: swap returned unit=%p current=%p alternate=%p trans=%u",unit,
               Read<void*>(unit,kCurrentRecord),Read<void*>(unit,kAlternateRecord),static_cast<unsigned>(Read<std::uint8_t>(unit,kTransform)));
    if(NormalizeAfterEntryHop(unit)){
        InterlockedIncrement64(&g_normalized);
        g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: entry hop landed on a pair form; the pair form is now the base form");
    }
}

bool HandOverPair(std::uint8_t* unit,const DeckSnapshot* snapshot){
    __try{
        const auto& pair=snapshot->pairs[snapshot->selected];
        if(Read<void*>(unit,kCurrentModel)!=snapshot->current||Read<void*>(unit,kAlternateModel)!=snapshot->alternate||
           Read<std::uintptr_t>(snapshot->entry,4)!=pair.base||Read<std::uintptr_t>(snapshot->entry,12)!=pair.alternate)return false;
        Write(snapshot->entry,4,snapshot->current);Write(snapshot->entry,12,snapshot->alternate);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

void __fastcall OnDestroy(std::uint8_t* unit,void* edx){
    sdgo::unit_stages::Forget(unit);
    Chain chain;bool known=false;
    EnterCriticalSection(&g_lock);
    const auto found=g_chains->find(unit);
    if(found!=g_chains->end()){chain=std::move(found->second);g_chains->erase(found);known=true;}
    LeaveCriticalSection(&g_lock);
    unit_form_models::Cleanup cleanup;
    DeckSnapshot snapshot;
    if(known&&ReadDeck(unit,&snapshot)){
        std::vector<unit_form_models::Pair> deck(snapshot.pairs,snapshot.pairs+snapshot.count);
        std::vector<std::uintptr_t> cached;
        for(const auto& entry:chain.models)cached.push_back(reinterpret_cast<std::uintptr_t>(entry.second.model));
        cleanup=unit_form_models::Plan(deck,snapshot.selected,
            {reinterpret_cast<std::uintptr_t>(snapshot.current),reinterpret_cast<std::uintptr_t>(snapshot.alternate)},cached);
        if(cleanup.result==unit_form_models::Result::handed_over&&!HandOverPair(unit,&snapshot))cleanup={};
        if(cleanup.result==unit_form_models::Result::handed_over){
            InterlockedIncrement64(&g_unitsRestored);
            g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: live pair %p/%p handed to the original destructor; detached=%u",
                snapshot.current,snapshot.alternate,static_cast<unsigned>(cleanup.detached.size()));
        }
    }
    if(known&&cleanup.result==unit_form_models::Result::refused)
            g_api->Log(g_mod,SDGO_LOG_WARN,"unit form chain: deck entry did not match the live pair; destructor left unchanged");
    g_destroy(unit,edx);
    for(auto address:cleanup.detached){
        auto* detached=reinterpret_cast<void*>(address);
        // Native model deleting destructor(flag=1). Its cleanup reads model
        // fields and scene parents, not the expired unit back-reference.
        // Keep exceptions observable rather than hiding an ownership fault.
        using DeleteModel=void(__thiscall*)(void*,unsigned);
        const auto table=*static_cast<void***>(detached);
        reinterpret_cast<DeleteModel>(table[0])(detached,1);
        InterlockedIncrement64(&g_modelsReleased);
        g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: detached model %p released after native unit cleanup",detached);
    }
}
} // namespace

bool Attach(const SdgoApi* api,SdgoMod* mod,const unit_catalog::Catalog& catalog,GridData data,GridFind find){
    if(!api->SettingInt(mod,"FormChains",1))return true;
    g_api=api;g_mod=mod;g_catalog=&catalog;g_data=data;g_find=find;
    g_weaponTables=reinterpret_cast<WeaponTables>(api->Symbol(mod,"unit_catalog.unit_weapon_tables"));
    g_modelCreate=reinterpret_cast<ModelCreate>(api->Symbol(mod,"unit_catalog.unit_model_create"));
    // Chains are optional: a client whose symbols differ keeps the catalog without them.
    if(!g_weaponTables||!g_modelCreate){api->Log(mod,SDGO_LOG_WARN,"unit form chain symbols differ; directed chains disabled");return true;}
    InitializeCriticalSection(&g_lock);
    g_chains=new std::map<std::uint8_t*,Chain>();
    if(!api->HookCode(mod,"unit_catalog.unit_transform_swap",reinterpret_cast<void*>(&OnSwap),reinterpret_cast<void**>(&g_swap))||
       !api->HookCode(mod,"unit_catalog.unit_destroy",reinterpret_cast<void*>(&OnDestroy),reinterpret_cast<void**>(&g_destroy))){
        api->Log(mod,SDGO_LOG_ERROR,"unit form chain hooks were refused");return false;
    }
    g_installed=true;
    api->Log(mod,SDGO_LOG_INFO,"unit form chains: directed model cache and lifecycle rebase installed");
    return true;
}
} // namespace sdgo::unit_form_chain

extern "C" __declspec(dllexport) int __cdecl SdgoIsImportedTransformRecoveryV1(const void* actor){
    using namespace sdgo::unit_form_chain;
    if(!actor||!g_installed||!g_chains)return 0;
    auto* unit=const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(actor));
    EnterCriticalSection(&g_lock);
    const auto found=g_chains->find(unit);
    const bool owned=found!=g_chains->end()&&!found->second.models.empty();
    LeaveCriticalSection(&g_lock);
    return owned&&CommittedChainTarget(unit)?1:0;
}

extern "C" __declspec(dllexport) int __cdecl SdgoReadUnitFormChainStateV1(SdgoUnitFormChainStateV1* output){
    using namespace sdgo::unit_form_chain;
    if(!output||output->size<sizeof(*output))return 0;
    const auto counter=[](volatile LONG64* value){return static_cast<std::uint64_t>(InterlockedCompareExchange64(value,0,0));};
    const auto word=[](volatile LONG* value){return static_cast<std::uint32_t>(InterlockedCompareExchange(value,0,0));};
    SdgoUnitFormChainStateV1 state{};state.size=sizeof(state);state.version=1;state.installed=g_installed?1u:0u;
    state.swaps_seen=counter(&g_swapsSeen);state.rebased=counter(&g_rebased);state.refused=counter(&g_refused);
    state.models_created=counter(&g_modelsCreated);state.models_released=counter(&g_modelsReleased);state.units_restored=counter(&g_unitsRestored);
    state.reserved=static_cast<uint32_t>(counter(&g_normalized));
    state.last_unit_id=word(&g_lastUnit);state.last_current=word(&g_lastCurrent);state.last_target=word(&g_lastTarget);state.last_refusal=word(&g_lastRefusal);
    std::memcpy(output,&state,sizeof(state));return 1;
}
