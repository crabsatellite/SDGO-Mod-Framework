#include <windows.h>
#include <intrin.h>
#include "stage_runtime.h"
#include "sdgo/unit_stage_rules.h"

namespace sdgo::unit_stages {
namespace {
using Update=void(__thiscall*)(void*,void*,std::uint32_t,std::uint32_t,std::uint32_t);
using PlayerInput=void(__thiscall*)(void*,void*);
using Request=unsigned char(__thiscall*)(void*);
using Find=void*(__thiscall*)(void*,unsigned);
Update originalUpdate=nullptr;Request originalRequest=nullptr;Find find=nullptr;void** manager=nullptr;
PlayerInput originalPlayerInput=nullptr;
const SdgoApi* api=nullptr;SdgoMod* mod=nullptr;Policy policy;
bool installed=false;
void* actor=nullptr;std::uint32_t pendingSource=0,pendingTarget=0;
std::uint32_t observedStage=0;unsigned observedActive=2;
struct Snapshot {void* unit=nullptr;void* move=nullptr;std::uint32_t id=0,target=0,hp=0,maximum=0;bool skillActive=false;};
template<class T>T Read(const void* base,std::size_t offset){T result;std::memcpy(&result,static_cast<const unsigned char*>(base)+offset,sizeof(result));return result;}
void* Local(){
    __try{
        if(!manager||!*manager)return nullptr;
        auto* unit=Read<unsigned char*>(*manager,0x88);
        if(!unit||!unit[0x31e9])return nullptr;
        const auto id=Read<std::uint32_t>(unit,4);
        return id&&find(*manager,id)==unit?unit:nullptr;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
bool ReadActor(void* unit,std::uint32_t skill,Snapshot& state){
    __try{
        if(!unit)return false;
        const auto* record=Read<const unsigned char*>(unit,0x9c4);if(!record)return false;
        state.unit=unit;state.move=Read<void*>(unit,0x9c8);state.id=Read<std::uint32_t>(record,0);
        state.target=Read<std::uint32_t>(record,316);state.hp=Read<std::uint32_t>(unit,0x38);state.maximum=Read<std::uint32_t>(unit,0x3c);
        if(!state.move||Read<void*>(state.move,0x21c)!=unit||!state.maximum||!state.hp||state.hp>state.maximum)return false;
        for(const unsigned offset:{0xabcu,0xac0u})if(const auto* object=Read<const unsigned char*>(unit,offset)){
            const auto* definition=Read<const unsigned char*>(object,8);
            if(definition&&Read<std::uint32_t>(definition,0)==skill&&object[0x3c]==1)state.skillActive=true;
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Eligible(void* unit,Snapshot& state){
    Snapshot basic;if(!ReadActor(unit,0,basic))return false;
    const auto found=policy.rules.find(basic.id);if(found==policy.rules.end())return false;
    if(!ReadActor(unit,found->second.skill,state))return false;
    return state.target==found->second.target&&state.skillActive&&
           static_cast<std::uint64_t>(state.hp)*100<=static_cast<std::uint64_t>(state.maximum)*found->second.percent;
}
unsigned char __fastcall Gate(void* move,void*){
    const DWORD error=GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
    bool denied=false;
    void* unit=nullptr;__try{unit=Read<void*>(move,0x21c);}__except(EXCEPTION_EXECUTE_HANDLER){}
    if(unit==Local()){
        Snapshot state;ReadActor(unit,0,state);
        denied=policy.managed.count(state.id)&&!Eligible(unit,state);
    }
    _fxrstor(fp);SetLastError(error);
    if(denied)return 0;
    return originalRequest(move);
}
void Advance(void* unit){
    if(unit!=Local())return;
    Snapshot state;if(!ReadActor(unit,0,state)){if(actor==unit)Forget(unit);return;}
    if(actor!=unit){actor=unit;pendingSource=pendingTarget=0;}
    const auto tracked=policy.rules.find(state.id);
    if(tracked!=policy.rules.end()){
        Snapshot sample;
        if(ReadActor(unit,tracked->second.skill,sample)&&
           (observedStage!=sample.id||observedActive!=static_cast<unsigned>(sample.skillActive))){
            observedStage=sample.id;observedActive=sample.skillActive;
            api->Log(mod,SDGO_LOG_INFO,"unit stage skill state: unit=%lu skill=%lu hp=%lu/%lu active=%u",
                static_cast<unsigned long>(sample.id),static_cast<unsigned long>(tracked->second.skill),
                static_cast<unsigned long>(sample.hp),static_cast<unsigned long>(sample.maximum),observedActive);
        }
    }
    if(pendingTarget){
        if(state.id==pendingTarget){
            api->Log(mod,SDGO_LOG_INFO,"unit stage committed: %lu -> %lu hp=%lu/%lu (native form commit)",
                static_cast<unsigned long>(pendingSource),static_cast<unsigned long>(pendingTarget),
                static_cast<unsigned long>(state.hp),static_cast<unsigned long>(state.maximum));
            pendingSource=pendingTarget=0;
        }else if(state.id==pendingSource)return;
        else {pendingSource=pendingTarget=0;}
    }
    if(!Eligible(unit,state))return;
    if(originalRequest(state.move)){
        pendingSource=state.id;pendingTarget=state.target;
        const auto& rule=policy.rules.at(state.id);
        api->Log(mod,SDGO_LOG_INFO,"unit stage admitted: %lu -> %lu skill=%lu hp=%lu/%lu threshold=%lu active=1 (native request)",
            static_cast<unsigned long>(state.id),static_cast<unsigned long>(state.target),static_cast<unsigned long>(rule.skill),
            static_cast<unsigned long>(state.hp),static_cast<unsigned long>(state.maximum),static_cast<unsigned long>(rule.percent));
    }
}
void __fastcall Tick(void* skill,void*,void* unit,std::uint32_t a,std::uint32_t b,std::uint32_t c){
    originalUpdate(skill,unit,a,b,c);
    const DWORD error=GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
    Advance(unit);_fxrstor(fp);SetLastError(error);
}
void __fastcall InputTick(void* unit,void*,void* input){
    originalPlayerInput(unit,input);
    const DWORD error=GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
    Advance(unit);_fxrstor(fp);SetLastError(error);
}
std::string CatalogHash(const unit_catalog::Catalog& catalog){
    using namespace unit_catalog;std::vector<std::uint8_t> bytes(sizeof(Header));
    for(const auto& e:catalog.records){EntryHeader h={e.kind,e.id,static_cast<std::uint32_t>(e.bytes.size()),e.recordHash,0,0};
        const auto* p=reinterpret_cast<const std::uint8_t*>(&h);bytes.insert(bytes.end(),p,p+sizeof(h));bytes.insert(bytes.end(),e.bytes.begin(),e.bytes.end());}
    Header h={};std::memcpy(h.magic,"SDGOUN1\0",8);h.version=1;h.headerBytes=32;h.count=static_cast<std::uint32_t>(catalog.records.size());
    h.payloadBytes=static_cast<std::uint32_t>(bytes.size()-32);h.payloadHash=Hash32(bytes.data()+32,h.payloadBytes);std::memcpy(bytes.data(),&h,32);
    return scripts::Sha256(bytes);
}
}
void Forget(void* unit){if(actor==unit){actor=nullptr;pendingSource=pendingTarget=observedStage=0;observedActive=2;}}
bool Attach(const SdgoApi* input,SdgoMod* module,const unit_catalog::Catalog& catalog,const wchar_t* directory){
    api=input;mod=module;const std::wstring path=std::wstring(directory?directory:api->ModDirectory(mod))+L"\\stage-rules.bin";
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND)return true;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info={};LARGE_INTEGER size={};DWORD got=0;std::vector<std::uint8_t> bytes;
    bool ok=GetFileInformationByHandle(file,&info)&&GetFileSizeEx(file,&size)&&size.QuadPart>=80&&size.QuadPart<=80+128*16&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
    if(ok){bytes.resize(static_cast<std::size_t>(size.QuadPart));ok=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size();}
    CloseHandle(file);std::string error;
    Policy part;
    if(!ok||!part.Parse(bytes,CatalogHash(catalog),catalog,error)){api->Log(mod,SDGO_LOG_ERROR,"unit stage rules refused: %s",error.c_str());return false;}
    for(const auto& row:part.rules)if(policy.rules.count(row.first)){api->Log(mod,SDGO_LOG_ERROR,"unit stage rules cross content scopes");return false;}
    policy.rules.insert(part.rules.begin(),part.rules.end());policy.managed.insert(part.managed.begin(),part.managed.end());
    if(policy.rules.empty()||installed)return true;
    manager=reinterpret_cast<void**>(api->Symbol(mod,"game.unit_manager"));find=reinterpret_cast<Find>(api->Symbol(mod,"um.find_object"));
    if(!manager||!find)return false;
    installed=api->HookCode(mod,"unit_catalog.stage_skill_update",reinterpret_cast<void*>(&Tick),reinterpret_cast<void**>(&originalUpdate))&&
        api->HookCode(mod,"unit_catalog.stage_player_input",reinterpret_cast<void*>(&InputTick),reinterpret_cast<void**>(&originalPlayerInput))&&
        api->HookCode(mod,"combat.transform_request",reinterpret_cast<void*>(&Gate),reinterpret_cast<void**>(&originalRequest));
    return installed;
}
}
