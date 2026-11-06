#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <map>
#include "form_chain_runtime.h"

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
using ModelDelete=void(__thiscall*)(void*,unsigned);

struct Chain { std::uint32_t form=0;const std::uint8_t* record=nullptr;void* model=nullptr; };

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
        if(!target||target!=Read<std::uint32_t>(current,kRecordTransform)||target==Read<std::uint32_t>(alternate,0))return false;
        hop->current=Read<std::uint32_t>(current,0);hop->target=target;hop->unit=unit;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
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
                if(chain.model&&chain.form!=hop.target){ // A different third form is impossible for a two-hop chain; refuse rather than guess.
                    LeaveCriticalSection(&g_lock);InterlockedExchange(&g_lastRefusal,2);InterlockedIncrement64(&g_refused);
                    g_swap(unit,edx);return;
                }
                if(!chain.model){
                    g_modelCreate(unit,record,&model);
                    g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: model for %lu created at %p",static_cast<unsigned long>(hop.target),model);
                    if(model){chain.form=hop.target;chain.record=record;chain.model=model;InterlockedIncrement64(&g_modelsCreated);}
                } else model=chain.model;
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

// Before the original destructor frees the deck pair, put the deck-owned models back
// into the live pair and release the chain model the deck does not know about.
bool RestorePair(std::uint8_t* unit,Chain* chain){
    __try{
        auto* current=Read<std::uint8_t*>(unit,kCurrentModel);auto* alternate=Read<std::uint8_t*>(unit,kAlternateModel);
        if(current!=chain->model&&alternate!=chain->model)return true; // The deck pair is already live (respawn restored it).
        const auto* begin=Read<const std::uint8_t*>(unit,kDeckBegin);
        const auto index=Read<std::int32_t>(unit,kDeckIndex);
        if(!begin||index<0||index>15)return false;
        // The deck entry is validated by content, not by a vector bound: its alternate
        // record/model must be the live pair member the chain did not replace, and the
        // chain's own form is never a deck form.
        const auto* entry=begin+static_cast<std::size_t>(index)*kDeckEntryBytes;
        const auto* deckRecord=Read<const std::uint8_t*>(entry,0);auto* deckModel=Read<std::uint8_t*>(entry,4);
        const auto* deckAlternateRecord=Read<const std::uint8_t*>(entry,8);auto* deckAlternateModel=Read<std::uint8_t*>(entry,12);
        if(!deckRecord||!deckModel||!deckAlternateRecord||!deckAlternateModel)return false;
        const auto* currentRecord=Read<const std::uint8_t*>(unit,kCurrentRecord);const auto* alternateRecord=Read<const std::uint8_t*>(unit,kAlternateRecord);
        const auto* survivingRecord=current==chain->model?alternateRecord:currentRecord;
        auto* survivingModel=current==chain->model?alternate:current;
        if(survivingRecord!=deckAlternateRecord||survivingModel!=deckAlternateModel||
           deckModel==chain->model||deckAlternateModel==chain->model||Read<std::uint32_t>(deckRecord,0)==chain->form)return false;
        // The surviving deck model is the one the chain did not replace.
        const bool keepCurrent=(current==deckModel||current==deckAlternateModel);
        auto* other=keepCurrent?current:alternate;
        const bool otherIsBase=other==deckModel;
        Write(unit,kCurrentModel,otherIsBase?deckAlternateModel:deckModel);Write(unit,kAlternateModel,other);
        Write(unit,kCurrentRecord,otherIsBase?deckAlternateRecord:deckRecord);Write(unit,kAlternateRecord,otherIsBase?deckRecord:deckAlternateRecord);
        Write<std::uint8_t>(unit,kTransform,otherIsBase?1:0);
        Write(unit,8,Read<std::uint8_t*>(unit,kCurrentModel)); // Scene node follows the current model, as in the native swap.
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

void ReleaseModel(void* model){
    __try{
        auto** table=*reinterpret_cast<void***>(model);
        reinterpret_cast<ModelDelete>(table[0])(model,1);
        InterlockedIncrement64(&g_modelsReleased);
    }__except(EXCEPTION_EXECUTE_HANDLER){}
}

void __fastcall OnDestroy(std::uint8_t* unit,void* edx){
    Chain chain;bool known=false;
    EnterCriticalSection(&g_lock);
    const auto found=g_chains->find(unit);
    if(found!=g_chains->end()){chain=found->second;g_chains->erase(found);known=true;}
    LeaveCriticalSection(&g_lock);
    // The deck pair goes back into the live pair before the original destructor so it
    // frees exactly the deck-owned models. The chain model is released only after the
    // destructor returns: weapon/effect objects built from the alternate pair can still
    // reference it while the unit tears them down.
    bool release=false;
    if(known&&chain.model){
        release=RestorePair(unit,&chain);
        if(release)InterlockedIncrement64(&g_unitsRestored);
        else g_api->Log(g_mod,SDGO_LOG_WARN,"unit form chain: deck pair could not be restored before destruction; chain model %p retained",chain.model);
    }
    g_destroy(unit,edx);
    if(release){
        ReleaseModel(chain.model);
        g_api->Log(g_mod,SDGO_LOG_INFO,"unit form chain: deck pair restored; chain model %p released after destruction",chain.model);
    }
}
} // namespace

bool Attach(const SdgoApi* api,SdgoMod* mod,const unit_catalog::Catalog& catalog,GridData data,GridFind find){
    if(!api->SettingInt(mod,"FormChains",1))return true;
    g_api=api;g_mod=mod;g_catalog=&catalog;g_data=data;g_find=find;
    g_weaponTables=reinterpret_cast<WeaponTables>(api->Symbol(mod,"unit_catalog.unit_weapon_tables"));
    g_modelCreate=reinterpret_cast<ModelCreate>(api->Symbol(mod,"unit_catalog.unit_model_create"));
    // Chains are optional: a client whose symbols differ keeps the catalog without them.
    if(!g_weaponTables||!g_modelCreate){api->Log(mod,SDGO_LOG_WARN,"unit form chain symbols differ; third-form chains disabled");return true;}
    InitializeCriticalSection(&g_lock);
    g_chains=new std::map<std::uint8_t*,Chain>();
    if(!api->HookCode(mod,"unit_catalog.unit_transform_swap",reinterpret_cast<void*>(&OnSwap),reinterpret_cast<void**>(&g_swap))||
       !api->HookCode(mod,"unit_catalog.unit_destroy",reinterpret_cast<void*>(&OnDestroy),reinterpret_cast<void**>(&g_destroy))){
        api->Log(mod,SDGO_LOG_ERROR,"unit form chain hooks were refused");return false;
    }
    g_installed=true;
    api->Log(mod,SDGO_LOG_INFO,"unit form chains: third-form rebase installed");
    return true;
}
} // namespace sdgo::unit_form_chain

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
