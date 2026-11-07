// Unshipped native composition experiment in disposable owned hosts only.
// The live initializer runs exactly once. The reduced composer never replays
// inventory/equipment initialization or the definition-normalizing selector.
// Global lookup callback purity remains outside this probe's acceptance.
#include <windows.h>
#include <intrin.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/script_registration.h"
#include "sdgo/native_unit_form_composition.h"

namespace {
constexpr std::size_t kSource=0x476,kPlayer=0x1f54,kSlot=0x658,kBank=0x2f0;
using Initialize=unsigned(__thiscall*)(void*,void*,const void*,void*,unsigned char,unsigned char);
Initialize original=nullptr;
sdgo::unit_forms::NativeBindings binding;
unsigned selectedRoot=0,targetId=0;
char runId[80]{};
std::wstring output;
volatile LONG observed=0;
struct Input {std::array<unsigned char,kSource> source{};std::array<unsigned char,kPlayer> player{};bool valid=false;};
bool Copy(const void* from,void* to,std::size_t bytes){
    __try{std::memcpy(to,from,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
unsigned Word(const unsigned char* bytes,std::size_t offset){unsigned value=0;std::memcpy(&value,bytes+offset,4);return value;}
struct View {
    unsigned root=0,bank0=0,bank1=0,hp=0,currentHp=0;
    unsigned weapons[2][5]{};bool valid=false;
};
View Inspect(const unsigned char* slot){
    View result;
    __try{
        std::memcpy(&result.root,slot,4);std::memcpy(&result.hp,slot+4,4);std::memcpy(&result.currentHp,slot+0x34,4);
        for(unsigned bank=0;bank<2;++bank){
            const auto* definition=*reinterpret_cast<const unsigned char*const*>(slot+0x6c+bank*kBank);
            unsigned id=0;if(definition)std::memcpy(&id,definition,4);
            if(bank)result.bank1=id;else result.bank0=id;
            for(unsigned weapon=0;weapon<5;++weapon){
                const auto* record=*reinterpret_cast<const unsigned char*const*>(slot+0x70+bank*kBank+weapon*0x94);
                if(record)std::memcpy(&result.weapons[bank][weapon],record,4);
            }
        }
        result.valid=true;
    }__except(EXCEPTION_EXECUTE_HANDLER){}
    return result;
}
void PrintView(FILE* file,const char* key,const View& view){
    std::fprintf(file,"\"%s\":{\"valid\":%s,\"root\":%u,\"bank0\":%u,\"bank1\":%u,\"hp\":%u,\"current_hp\":%u,\"weapons\":[",key,view.valid?"true":"false",view.root,view.bank0,view.bank1,view.hp,view.currentHp);
    for(unsigned bank=0;bank<2;++bank){if(bank)std::fputc(',',file);std::fputc('[',file);
        for(unsigned i=0;i<5;++i){if(i)std::fputc(',',file);std::fprintf(file,"%u",view.weapons[bank][i]);}std::fputc(']',file);}
    std::fputs("]}",file);
}
struct Scratch {
    std::array<unsigned char,32> before;
    std::array<unsigned char,kPlayer> player{};
    std::array<unsigned char,32> after;
    Scratch(){before.fill(0xa7);after.fill(0xa7);}
    bool intact()const{for(auto v:before)if(v!=0xa7)return false;for(auto v:after)if(v!=0xa7)return false;return true;}
};
struct Composed{Scratch scratch;unsigned result=0,fault=0;};
bool CaptureSeed(const Input* input,const unsigned char* live,unsigned char rule,unsigned char mode,
                 sdgo::unit_forms::CompositionSeed* seed,unsigned* fault){
    __try{
        const unsigned index=input->player[0x33c];
        const auto sourceAt=(mode==6||mode==7)?0x19e+index*0x99:0x49;
        seed->rule=rule;seed->mode=mode;seed->source_rank=input->source[0x1f];
        seed->part_deck=(mode==6||mode==7)?static_cast<unsigned char>(index+1):live[0x1d93];
        if(seed->part_deck<1||seed->part_deck>4)return false;
        std::memcpy(&seed->boost_bonus,live+0x1dac+(seed->part_deck-1)*0x68,4);
        std::memcpy(seed->serialized_entry.data(),input->source.data()+sourceAt,seed->serialized_entry.size());
        const auto* before=input->player.data()+0x340+index*kSlot;
        std::memcpy(seed->before.bytes.data(),before+0x64,kBank);
        std::memcpy(seed->before.stats.data(),before+0xe,12);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){*fault=GetExceptionCode();return false;}
}
unsigned Compose(const sdgo::unit_forms::CompositionSeed& seed,unsigned identity,unsigned char* scratch,unsigned* fault){
    sdgo::unit_forms::Bank bank;
    if(sdgo::unit_forms::ComposeNativeBank(binding,seed,identity,bank,*fault)!=sdgo::unit_forms::CompositionResult::composed)return 0;
    auto* slot=scratch+0x340;
    std::memcpy(slot+0x64,bank.bytes.data(),kBank);std::memcpy(slot+0xe,bank.stats.data(),12);
    return 1;
}
void Observe(const Input& input,const unsigned char* live,unsigned char rule,unsigned char mode,const unsigned char* preFp){
    const auto index=input.player[0x33c];if(index>=4)return;
    const auto slotAt=0x340+index*kSlot;
    if(slotAt+kSlot>input.player.size())return;
    std::array<unsigned char,kSlot> before{};if(!Copy(live+slotAt,before.data(),before.size())||Word(before.data(),0)!=selectedRoot)return;
    // Native mode6/7 uses four serialized unit entries; other modes use the
    // single equipped entry. The input is a copy, never the live source record.
    const auto sourceAt=(mode==6||mode==7)?0x19e+index*0x99:0x49;
    if(Word(input.source.data(),sourceAt)!=selectedRoot)return;
    if(InterlockedCompareExchange(&observed,1,0))return;
    Composed same,target;
    sdgo::unit_forms::CompositionSeed seed;unsigned seedFault=0;
    const bool captured=CaptureSeed(&input,live,rule,mode,&seed,&seedFault);
    bool preBankZero=captured;for(auto byte:seed.before.bytes)if(byte)preBankZero=false;
    _fxrstor(preFp);
    same.fault=seedFault;same.result=captured?Compose(seed,selectedRoot,same.scratch.player.data(),&same.fault):0;
    _fxrstor(preFp);
    target.fault=seedFault;target.result=captured?Compose(seed,targetId,target.scratch.player.data(),&target.fault):0;
    std::array<unsigned char,kSlot> after{};const bool readAfter=Copy(live+slotAt,after.data(),after.size());
    const auto* sameSlot=same.scratch.player.data()+0x340;const auto* targetSlot=target.scratch.player.data()+0x340;
    const auto liveView=Inspect(before.data()),sameView=Inspect(sameSlot),targetView=Inspect(targetSlot);
    sdgo::unit_forms::Slot staged{},slotImage{},targetImage{};
    std::memcpy(slotImage.data(),before.data(),before.size());std::memcpy(targetImage.data(),targetSlot,kSlot);
    sdgo::unit_forms::Bank replacement;sdgo::unit_forms::Capture(targetImage,0,replacement);
    const auto active=Word(before.data(),0x60);
    const auto current=active<2?Word(before.data(),0x64+active*kBank):0;
    const bool inactiveStaged=target.result&&sdgo::unit_forms::StageInactive(slotImage,selectedRoot,current,replacement,staged)==sdgo::unit_forms::StageResult::prepared;
    bool preserved=inactiveStaged;
    for(unsigned i=0;preserved&&i<kSlot;++i){
        const auto bankAt=0x64+(active^1)*kBank,statsAt=0xe+(active^1)*12;
        if(!((i>=bankAt&&i<bankAt+kBank)||(i>=statsAt&&i<statsAt+12))&&slotImage[i]!=staged[i])preserved=false;
    }
    std::vector<unsigned> differences;
    for(unsigned i=0;i<kSlot;++i)if(((i>=0xe&&i<0x1a)||(i>=0x64&&i<0x64+kBank))&&before[i]!=sameSlot[i])differences.push_back(i);
    FILE* file=nullptr;
    if(!_wfopen_s(&file,output.c_str(),L"ab")&&file){
        std::fprintf(file,"{\"run_id\":\"%s\",\"composer\":\"fresh-guarded-sdk-bank-methods-r4\",\"bank_only_output\":true,\"pid\":%lu,\"thread\":%lu,\"rule\":%u,\"mode\":%u,\"slot_index\":%u,\"part_deck\":%u,\"pre_bank_zero\":%s,\"same_return\":%u,\"target_return\":%u,\"same_fault\":%u,\"target_fault\":%u,\"canaries_intact\":%s,\"live_bytes_unchanged\":%s,\"identity_bank_and_stats_equal\":%s,\"identity_diff_offsets\":[",
            runId,GetCurrentProcessId(),GetCurrentThreadId(),rule,mode,index,seed.part_deck,preBankZero?"true":"false",same.result,target.result,same.fault,target.fault,
            same.result&&target.result&&same.scratch.intact()&&target.scratch.intact()?"true":"false",readAfter&&after==before?"true":"false",same.result&&differences.empty()?"true":"false");
        for(std::size_t i=0;i<differences.size();++i){if(i)std::fputc(',',file);std::fprintf(file,"%u",differences[i]);}
        std::fputs("],",file);PrintView(file,"live",liveView);std::fputc(',',file);PrintView(file,"same",sameView);std::fputc(',',file);PrintView(file,"target",targetView);
        const auto digest=sdgo::scripts::Sha256(std::vector<unsigned char>(targetSlot,targetSlot+kSlot));
        std::fprintf(file,",\"target_slot_sha256\":\"%s\",\"inactive_bank_staged\":%s,\"staging_preserved_other_bytes\":%s,\"global_noninterference_proved\":false,\"live_switch_performed\":false,\"gameplay_accepted\":false}\n",digest.c_str(),inactiveStaged?"true":"false",preserved?"true":"false");std::fclose(file);
    }
}
unsigned __fastcall InitializeHook(void* context,void*,void* room,const void* source,void* player,unsigned char rule,unsigned char mode){
    const DWORD incomingError=GetLastError();alignas(16) unsigned char preFp[512];_fxsave(preFp);
    Input input;
    if(!InterlockedCompareExchange(&observed,0,0))input.valid=Copy(source,input.source.data(),input.source.size())&&Copy(player,input.player.data(),input.player.size());
    _fxrstor(preFp);SetLastError(incomingError);
    const auto result=original(context,room,source,player,rule,mode);
    const DWORD error=GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
    if(result&&input.valid){try{Observe(input,static_cast<unsigned char*>(player),rule,mode,preFp);}catch(...){}}
    _fxrstor(fp);SetLastError(error);return result;
}
}
SDGO_MOD_ENTRY {
    if(!api||api->version!=1)return 0;
    const int root=api->SettingInt(mod,"RootUnit",0),target=api->SettingInt(mod,"TargetUnit",0);
    if(root<10001||root>30000||target<10001||target>30000)return 0;
    selectedRoot=static_cast<unsigned>(root);targetId=static_cast<unsigned>(target);
    using Native=sdgo::unit_forms::NativeBindings;
    binding.unit_registry=api->Symbol(mod,"probe.unit_registry");binding.weapon_registry=api->Symbol(mod,"probe.weapon_registry");
    binding.find=reinterpret_cast<Native::Find>(api->Symbol(mod,"probe.find_unit"));
    binding.stats=reinterpret_cast<Native::Stats>(api->Symbol(mod,"probe.unit_stats"));
    binding.weapons=reinterpret_cast<Native::Weapons>(api->Symbol(mod,"probe.unit_weapons"));
    binding.boost=reinterpret_cast<Native::Boost>(api->Symbol(mod,"probe.player_sp"));
    binding.deck_boost=reinterpret_cast<Native::DeckBoost>(api->Symbol(mod,"probe.deck_sp"));
    binding.reset=reinterpret_cast<Native::Reset>(api->Symbol(mod,"probe.weapon_reset"));
    binding.map_hash=api->Symbol(mod,"probe.map_hash");binding.map_compare=api->Symbol(mod,"probe.map_compare");
    binding.mode_singleton=reinterpret_cast<void*const*>(api->Symbol(mod,"probe.mode_singleton"));
    if(!binding.unit_registry||!binding.weapon_registry||!binding.find||!binding.stats||!binding.weapons||
       !binding.boost||!binding.deck_boost||!binding.reset||!binding.map_hash||!binding.map_compare||!binding.mode_singleton)return 0;
    api->SettingString(mod,"RunId","",runId,sizeof(runId));if(std::strlen(runId)!=36)return 0;
    for(const char* p=runId;*p;++p)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||*p=='-'))return 0;
    output=std::wstring(api->ModDirectory(mod))+L"\\composition.jsonl";
    return api->HookCode(mod,"probe.unit_slot_initialize",reinterpret_cast<void*>(&InitializeHook),reinterpret_cast<void**>(&original));
}
