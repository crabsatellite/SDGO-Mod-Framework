#include "skill_atlas_runtime.h"
#include "sdgo/skill_atlas_format.h"
#include "sdgo/skill_atlas_state.h"
#include "sdgo/native_script_registration.h"
#include "../../support/native_resource_overlay/native_resource_overlay.h"
#include <windows.h>
#include <cmath>

namespace sdgo::unit_skill_atlas {
namespace {
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
const unit_catalog::Catalog* g_catalog=nullptr;
GridData g_data=nullptr;GridFind g_find=nullptr;
skill_atlas::Bundle g_bundle;
using Clear=void(__thiscall*)(void*);
using Load=void(__thiscall*)(void*,const scripts::NativeWideString*,char,float,float);
using Frame=void(__thiscall*)(void*,std::uint32_t);
Clear g_clear=nullptr;Load g_load=nullptr;Frame g_frame=nullptr;
void* g_firstOriginal=nullptr;void* g_secondOriginal=nullptr;
void* g_battleOriginal=nullptr;void* g_resolvedOriginal=nullptr;void* g_battleContinue=nullptr;
std::set<std::wstring> g_paths;
SRWLOCK g_lock=SRWLOCK_INIT;
SdgoSkillAtlasStateV1 g_state={};
SdgoSkillAtlasBattleStateV1 g_battleState={};
struct Registers{std::uint32_t edi,esi,ebp,savedEsp,ebx,edx,ecx,eax,flags;};
alignas(16) const std::uint32_t kMxcsr=0x1f80;

bool ReadBytes(const std::wstring& path,std::vector<std::uint8_t>& bytes){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info={};LARGE_INTEGER extent={};
    bool ok=GetFileInformationByHandle(file,&info)&&GetFileSizeEx(file,&extent)&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&extent.QuadPart>=128&&extent.QuadPart<=128+8LL*1024*1024;
    if(ok){bytes.resize(static_cast<std::size_t>(extent.QuadPart));DWORD got=0;ok=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size();}
    CloseHandle(file);return ok;
}

std::string CatalogHash(const unit_catalog::Catalog& catalog){
    using namespace unit_catalog;std::vector<std::uint8_t> bytes(sizeof(Header));
    for(const auto& entry:catalog.records){EntryHeader header={entry.kind,entry.id,static_cast<std::uint32_t>(entry.bytes.size()),entry.recordHash,0,0};
        const auto* first=reinterpret_cast<const std::uint8_t*>(&header);bytes.insert(bytes.end(),first,first+sizeof(header));bytes.insert(bytes.end(),entry.bytes.begin(),entry.bytes.end());}
    Header header={};std::memcpy(header.magic,"SDGOUN1\0",8);header.version=1;header.headerBytes=32;
    header.count=static_cast<std::uint32_t>(catalog.records.size());header.payloadBytes=static_cast<std::uint32_t>(bytes.size()-32);
    header.payloadHash=Hash32(bytes.data()+32,header.payloadBytes);std::memcpy(bytes.data(),&header,32);return scripts::Sha256(bytes);
}

bool Context(Registers* r,unsigned slot,const std::uint8_t** record,void** widget,std::uint32_t* id,std::uint16_t* frame){
    __try{
        *record=reinterpret_cast<const std::uint8_t*>(slot?r->ebx:r->edi);
        *widget=*reinterpret_cast<void**>(r->esi+(slot?0x184:0x180));
        if(!*record||!*widget)return false;
        std::memcpy(id,*record,4);std::memcpy(frame,*record+426,2);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Same(const std::uint8_t* pointer,const std::vector<std::uint8_t>& bytes){
    __try{return !std::memcmp(pointer,bytes.data(),bytes.size());}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

const skill_atlas::Entry* Owner(const std::uint8_t* record,std::uint32_t id){
    const auto found=g_bundle.entries.find(id);
    if(found==g_bundle.entries.end())return nullptr;
    const std::uint8_t* actual=nullptr;void* grid=g_data();
    if(grid&&g_find(grid,&actual,id,13,0)&&actual==record&&Same(record,found->second.record))return &found->second;
    return nullptr;
}

bool Reload(void* widget,const wchar_t* path,std::size_t length,std::uint16_t requested,
            std::uint32_t expectedCount,SdgoSkillAtlasSlotV1* state){
    __try{
        scripts::NativeWideString name={};name.value.pointer=path;name.length=static_cast<std::uint32_t>(length);name.capacity=(std::max)(8u,name.length);
        g_clear(widget);g_load(widget,&name,0,0.0f,0.0f);
        auto** begin=*reinterpret_cast<void***>(static_cast<std::uint8_t*>(widget)+0x1a4);
        auto** end=*reinterpret_cast<void***>(static_cast<std::uint8_t*>(widget)+0x1a8);
        if(!begin||end!=begin+1||!begin[0])return false;
        auto* player=static_cast<std::uint8_t*>(*static_cast<void**>(begin[0]));
        if(!player)return false;
        std::uint16_t first=0,last=0,actual=0;
        std::memcpy(&first,player+0x11c,2);std::memcpy(&last,player+0x11e,2);
        if(expectedCount&&(first!=0||last+1u!=expectedCount))return false;
        g_frame(player,requested);std::memcpy(&actual,player+0x120,2);
        if(expectedCount&&actual!=requested)return false;
        state->player=reinterpret_cast<std::uint32_t>(player);state->frame_first=first;state->frame_last=last;state->actual_frame=actual;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

void Fail(){
    AcquireSRWLockExclusive(&g_lock);++g_state.failures;ReleaseSRWLockExclusive(&g_lock);
    g_api->Log(g_mod,SDGO_LOG_ERROR,"native skill atlas failed to load/read back its actual sprite player; closing candidate client");
    TerminateProcess(GetCurrentProcess(),0xe0c00115);ExitProcess(0xe0c00115);
}

void __cdecl Update(Registers* r,unsigned slot){
    const DWORD lastError=GetLastError();
    const std::uint8_t* record=nullptr;void* widget=nullptr;std::uint32_t id=0;std::uint16_t frame=0;
    if(!Context(r,slot,&record,&widget,&id,&frame)){SetLastError(lastError);return;}
    const skill_atlas::Entry* owned=nullptr;
    const auto found=g_bundle.entries.find(id);
    if(found!=g_bundle.entries.end()){
        const std::uint8_t* actual=nullptr;void* grid=g_data();
        if(grid&&g_find(grid,&actual,id,13,0)&&actual==record&&Same(record,found->second.record))owned=&found->second;
    }
    const std::wstring baseline=L"sprs\\0594.spr";
    const auto& path=owned?owned->sprite:baseline;
    SdgoSkillAtlasSlotV1 state={};state.skill_id=id;state.requested_frame=frame;
    if(!Reload(widget,path.c_str(),path.size(),frame,owned?owned->frameCount:0,&state))Fail();
    std::string narrow;for(const wchar_t c:path)narrow.push_back(static_cast<char>(c));
    std::memcpy(state.sprite,narrow.c_str(),narrow.size()+1);
    AcquireSRWLockExclusive(&g_lock);
    state.calls=g_state.slots[slot].calls+1;state.owner_loads=g_state.slots[slot].owner_loads+(owned?1:0);
    state.baseline_loads=g_state.slots[slot].baseline_loads+(owned?0:1);g_state.slots[slot]=state;
    ReleaseSRWLockExclusive(&g_lock);
    if(owned&&state.owner_loads<=4)g_api->Log(g_mod,SDGO_LOG_INFO,"native skill atlas slot=%u skill=%lu frame=%u path=%ls",slot+1,static_cast<DWORD>(id),frame,path.c_str());
    SetLastError(lastError);
}

#define ATLAS_THUNK(name,slot,original,handler) \
__declspec(naked) void name(){ \
    __asm{pushfd} __asm{pushad} __asm{mov eax,esp} __asm{sub esp,210h} __asm{and esp,0fffffff0h} \
    __asm{mov [esp+200h],eax} __asm{fxsave [esp]} __asm{fninit} __asm{ldmxcsr dword ptr [kMxcsr]} __asm{cld} \
    __asm{push slot} __asm{push eax} __asm{call handler} __asm{add esp,8} __asm{fxrstor [esp]} \
    __asm{mov esp,[esp+200h]} __asm{popad} __asm{popfd} __asm{jmp dword ptr [original]} \
}
ATLAS_THUNK(First,0,g_firstOriginal,Update)
ATLAS_THUNK(Second,1,g_secondOriginal,Update)

bool BattleRecord(Registers* r,const std::uint8_t** record,std::uint32_t* id,std::uint16_t* frame){
    __try{
        *record=*reinterpret_cast<const std::uint8_t**>(r->esi+8);
        if(!*record)return false;
        std::memcpy(id,*record,4);std::memcpy(frame,*record+426,2);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool BattleArguments(Registers* r,const wchar_t* path,std::uint32_t length){
    __try{
        // This thunk reserved twelve bytes after PUSHFD. Copy the original
        // flags before replacing its slot with the native string length.
        auto* scratch=reinterpret_cast<std::uint32_t*>(r->savedEsp);
        scratch[0]=scratch[3];scratch[2]=reinterpret_cast<std::uint32_t>(path);scratch[3]=length;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void __cdecl BeforeBattle(Registers* r){
    const DWORD lastError=GetLastError();
    const std::uint8_t* record=nullptr;std::uint32_t id=0;std::uint16_t frame=0;
    if(!BattleRecord(r,&record,&id,&frame))Fail();
    const auto* owner=Owner(record,id);
    if(g_bundle.entries.count(id)&&!owner)Fail();
    const wchar_t* path=owner?owner->sprite.c_str():L"sprs\\0594.spr";
    const auto length=owner?static_cast<std::uint32_t>(owner->sprite.size()):13u;
    if(!BattleArguments(r,path,length))Fail();
    AcquireSRWLockExclusive(&g_lock);++g_battleState.calls;
    if(owner)++g_battleState.owner_loads;else ++g_battleState.baseline_loads;
    ReleaseSRWLockExclusive(&g_lock);SetLastError(lastError);
}
__declspec(naked) void Battle(){
    __asm{pushfd} __asm{sub esp,12} __asm{pushad} __asm{mov eax,esp}
    __asm{sub esp,210h} __asm{and esp,0fffffff0h} __asm{mov [esp+200h],eax}
    __asm{fxsave [esp]} __asm{fninit} __asm{ldmxcsr dword ptr [kMxcsr]} __asm{cld}
    __asm{push eax} __asm{call BeforeBattle} __asm{add esp,4} __asm{fxrstor [esp]}
    __asm{mov esp,[esp+200h]} __asm{popad}
    __asm{push dword ptr [esp]} __asm{popfd} __asm{lea esp,[esp+8]}
    __asm{mov word ptr [ebp-80h],cx} __asm{jmp dword ptr [g_battleContinue]}
}
bool ReadResolvedTexture(Registers* r,char* output,wchar_t* path,float* uv){
    __try{
        const auto* name=reinterpret_cast<const scripts::NativeWideString*>(r->ebp-0x48);
        if(name->length<6||name->length>80||name->capacity<name->length)return false;
        const wchar_t* text=name->capacity>=8?name->value.pointer:name->value.small;
        for(std::uint32_t i=0;i<name->length;++i){
            wchar_t c=text[i];if(c==L'\\')c=L'/';if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';
            if(c<32||c>126)return false;path[i]=c==L'/'?L'\\':c;output[i]=static_cast<char>(c);
        }
        output[name->length]=0;path[name->length]=0;
        std::memcpy(uv,reinterpret_cast<void*>(r->ebp-0x220),16);
        for(unsigned i=0;i<4;++i)if(!std::isfinite(uv[i])||uv[i]<0||uv[i]>1)return false;
        return uv[2]>uv[0]&&uv[3]>uv[1];
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ResolvedTexture(Registers* r,char* output,float* uv){
    wchar_t path[81]={};
    if(!ReadResolvedTexture(r,output,path,uv))return false;
    const std::wstring name=path;
    return g_paths.count(name)&&name.compare(0,5,L"txrs\\")==0&&name.substr(name.size()-4)==L".txr";
}
void __cdecl AfterBattle(Registers* r,unsigned){
    const DWORD lastError=GetLastError();
    const std::uint8_t* record=nullptr;std::uint32_t id=0;std::uint16_t frame=0;
    if(!BattleRecord(r,&record,&id,&frame))Fail();
    const auto* owner=Owner(record,id);
    if(!owner){if(g_bundle.entries.count(id))Fail();SetLastError(lastError);return;}
    SdgoSkillAtlasBattleEntryV1 entry={};entry.skill_id=id;entry.frame=frame;
    if(!ResolvedTexture(r,entry.texture,entry.uv))Fail();
    for(std::size_t i=0;i<owner->sprite.size();++i)entry.sprite[i]=static_cast<char>(owner->sprite[i]);
    AcquireSRWLockExclusive(&g_lock);
    unsigned index=0;while(index<g_battleState.count&&g_battleState.entries[index].skill_id!=id)++index;
    if(index<16){entry.calls=g_battleState.entries[index].calls+1;g_battleState.entries[index]=entry;
        if(index==g_battleState.count)++g_battleState.count;}
    ReleaseSRWLockExclusive(&g_lock);
    g_api->Log(g_mod,SDGO_LOG_INFO,"native actor skill atlas skill=%lu frame=%u texture=%s",static_cast<DWORD>(id),frame,entry.texture);
    SetLastError(lastError);
}
ATLAS_THUNK(Resolved,0,g_resolvedOriginal,AfterBattle)
#undef ATLAS_THUNK

bool Body(const std::uint8_t* body,std::size_t first,std::size_t last,const char* expected,std::size_t gapFirst=0,std::size_t gapLast=0){
    std::vector<std::uint8_t> bytes(body+first,body+last);
    if(gapLast>gapFirst)bytes.erase(bytes.begin()+(gapFirst-first),bytes.begin()+(gapLast-first));
    return scripts::Sha256(bytes)==expected;
}
}

bool Attach(const SdgoApi* api,SdgoMod* mod,const unit_catalog::Catalog& catalog,GridData data,GridFind find,const std::vector<Scope>& scopes){
    g_api=api;g_mod=mod;g_catalog=&catalog;g_data=data;g_find=find;
    std::vector<std::uint8_t> identities;
    for(const auto& scope:scopes){
        const std::wstring file=scope.directory+L"\\skill-atlases.bin";
        if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND)continue;
        resource_overlay::BundleRegistration resources;std::string error;std::vector<std::uint8_t> bytes;
        std::set<std::wstring> paths;
        if(!scope.catalog||!resource_overlay::InspectUnitBundleFiles(scope.directory.c_str(),resources,error)){
            api->Log(mod,SDGO_LOG_ERROR,"skill atlas resource preflight failed: %s",error.c_str());return false;}
        for(const auto& entry:resources.entries){paths.insert(entry.first);g_paths.insert(entry.first);}
        skill_atlas::Bundle part;
        if(!ReadBytes(file,bytes)||!part.Parse(bytes,CatalogHash(*scope.catalog),resources.identity,paths,*scope.catalog,error)){
            api->Log(mod,SDGO_LOG_ERROR,"skill atlas sidecar rejected: %s",error.c_str());return false;}
        for(auto& entry:part.entries)if(!g_bundle.entries.emplace(entry.first,std::move(entry.second)).second){
            api->Log(mod,SDGO_LOG_ERROR,"skill atlas ID crosses content scopes");return false;}
        identities.insert(identities.end(),bytes.begin(),bytes.end());
    }
    if(g_bundle.entries.empty())return true;
    auto* body=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.ui_skill_body"));
    auto* first=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_first"));
    auto* second=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_second"));
    auto* load=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_load"));
    auto* clear=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_clear"));
    auto* frame=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_frame"));
    auto* battle=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_battle_body"));
    auto* before=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_battle_path"));
    auto* resolved=static_cast<const std::uint8_t*>(api->Symbol(mod,"unit_catalog.skill_atlas_battle_resolved"));
    if(!body||first!=body+0x3fd||second!=body+0x59d||!load||!clear||!frame||
        !Body(body,0,1529,"d9cc9addc93bbe25b39d3307a8878eb888968a3ccd69116dc2cdeb9d759a9883")||
        !Body(load,0,306,"ea6632ba6a5a6861054e39a56860654479c47681d13d296bfccdf45974a6f569",157,160)||
        !Body(clear,0,185,"527a08e91ca3cc4aedf5d216a6442c01eecc1790326d1bbd7317b36d8912e201")||
        !Body(frame,0,67,"667e38c6f299b18a37ffef9d678b4e9194853fa3f861dafe96c601db3828067a")||
        !battle||before!=battle+0x241||resolved!=battle+0x2a7||
        !Body(battle,0,1173,"99b2bd4e05b1be0c1d279e2c620b9eee3a19eb755a196fe314ce5f5b5abc0443")){
        api->Log(mod,SDGO_LOG_ERROR,"native skill atlas consumers differ from the supported profile");return false;}
    g_clear=reinterpret_cast<Clear>(const_cast<std::uint8_t*>(clear));g_load=reinterpret_cast<Load>(const_cast<std::uint8_t*>(load));g_frame=reinterpret_cast<Frame>(const_cast<std::uint8_t*>(frame));
    g_state.size=sizeof(g_state);g_state.version=1;const auto hash=scripts::Sha256(identities);std::memcpy(g_state.sidecar_sha256,hash.c_str(),65);
    g_battleState.size=sizeof(g_battleState);g_battleState.version=1;g_battleContinue=const_cast<std::uint8_t*>(before+11);
    return api->HookSite(mod,"unit_catalog.skill_atlas_first",reinterpret_cast<void*>(&First),&g_firstOriginal)&&
           api->HookSite(mod,"unit_catalog.skill_atlas_second",reinterpret_cast<void*>(&Second),&g_secondOriginal)&&
           api->HookSite(mod,"unit_catalog.skill_atlas_battle_path",reinterpret_cast<void*>(&Battle),&g_battleOriginal)&&
           api->HookSite(mod,"unit_catalog.skill_atlas_battle_resolved",reinterpret_cast<void*>(&Resolved),&g_resolvedOriginal);
}

int ReadState(SdgoSkillAtlasStateV1* output){
    if(!output||output->size!=sizeof(*output)||output->version!=1)return 0;
    AcquireSRWLockShared(&g_lock);*output=g_state;ReleaseSRWLockShared(&g_lock);return output->version==1;
}
int ReadBattleState(SdgoSkillAtlasBattleStateV1* output){
    if(!output||output->size!=sizeof(*output)||output->version!=1)return 0;
    AcquireSRWLockShared(&g_lock);*output=g_battleState;ReleaseSRWLockShared(&g_lock);return output->version==1;
}
}
extern "C" __declspec(dllexport) int __cdecl SdgoReadSkillAtlasStateV1(SdgoSkillAtlasStateV1* output){return sdgo::unit_skill_atlas::ReadState(output);}
extern "C" __declspec(dllexport) int __cdecl SdgoReadSkillAtlasBattleStateV1(SdgoSkillAtlasBattleStateV1* output){return sdgo::unit_skill_atlas::ReadBattleState(output);}
