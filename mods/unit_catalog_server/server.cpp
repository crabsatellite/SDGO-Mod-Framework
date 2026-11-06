#include <windows.h>
#include <filesystem>
#include "sdgo/native_skill_identity.h"
#include "sdgo/skill_identity_format.h"
#include "sdgo/unit_skill_runtime_state.h"
#include "sdgo/unit_form_graph_format.h"
#include "sdgo/unit_form_runtime_state.h"
#include "human_build_capture.h"
#include "native_form_gate.h"

namespace {
using namespace sdgo::skill_identity;
RuntimeInput g_input;
SdgoUnitSkillRuntimeStateV1 g_state={};
sdgo::unit_forms::GraphRuntimeInput g_graphs;
std::unique_ptr<unit_catalog_server::HumanBuildCapture> g_capture;
SdgoUnitFormRuntimeStateV1 g_formState={};
class NativeLookup final:public Lookup {
public:
    using Find=void(__thiscall*)(void*,std::uint32_t,void**);
    void* registry=nullptr;Find query=nullptr;
    const void* find(std::uint32_t id)const override{
        __try{
            void* record=nullptr;query(registry,id,&record);
            return record&&*static_cast<const std::uint32_t*>(record)==id?record:nullptr;
        }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
} g_lookup;
bool Read(const std::filesystem::path& path,std::vector<std::uint8_t>& bytes,std::size_t maximum){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size={};BY_HANDLE_FILE_INFORMATION info={};
    bool valid=GetFileSizeEx(file,&size)&&GetFileInformationByHandle(file,&info)&&size.QuadPart>0&&
        static_cast<std::uint64_t>(size.QuadPart)<=maximum&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
    if(valid){bytes.resize(static_cast<std::size_t>(size.QuadPart));DWORD read=0;
        valid=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size();}
    CloseHandle(file);return valid;
}

}

extern "C" __declspec(dllexport) int __cdecl SdgoReadUnitFormGateStateV1(SdgoUnitFormGateStateV1* state){
    if(!state||state->size!=sizeof(*state)||state->version!=1)return 0;
    *state=unit_catalog_server::ReadFormGateState();return state->installed?1:0;
}

extern "C" __declspec(dllexport) int __cdecl SdgoReadUnitFormRuntimeStateV1(SdgoUnitFormRuntimeStateV1* state){
    if(!state||state->size!=sizeof(*state)||state->version!=1||!g_formState.capture_installed||!g_capture)return 0;
    *state=g_formState;const auto status=g_capture->Status();
    state->observer_healthy=g_capture->Healthy()?1u:0u;state->rooms=status.rooms;state->players=status.players;state->pending=status.pending;
    state->completed=status.completed;state->rejected=status.rejected;
    sdgo::unit_forms::HumanUnitCaptureSnapshot sample;
    if(g_capture->Inspect(sample)){
        state->sample_root=sample.unit.root;state->sample_owner=sample.unit.owner;state->sample_deck=sample.unit.deck;
        state->sample_definition=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(sample.unit.definition));
        state->sample_room=static_cast<std::uint32_t>(sample.room);state->sample_player=static_cast<std::uint32_t>(sample.player);
        state->sample_serial=sample.unit.serial;state->sample_room_epoch=sample.roomEpoch;state->sample_generation=sample.generation;
    }
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoReadUnitSkillRuntimeStateV1(SdgoUnitSkillRuntimeStateV1* state){
    if(!state||state->size!=sizeof(*state)||state->version!=1||!g_state.installed)return 0;
    *state=g_state;const auto native=sdgo::skill_identity::ReadNativeStatus();
    state->enable_aliases=native.enableAliases;state->disable_aliases=native.disableAliases;state->rejected=native.rejected;return 1;
}

SDGO_MOD_ENTRY {
    if(!api||api->version!=1||!api->SettingInt(mod,"Enabled",1))return 0;
    const auto root=std::filesystem::path(api->ModDirectory(mod));
    const auto sdgo=root.parent_path().parent_path();
    std::vector<std::uint8_t> candidate,authority,host,graphCandidate,graphAuthority;
    wchar_t image[MAX_PATH]={};
    if(!GetModuleFileNameW(nullptr,image,MAX_PATH)||!Read(image,host,64u*1024u*1024u)||
       sdgo::scripts::Sha256(host)!="f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4"){
        api->Log(mod,SDGO_LOG_ERROR,"unit server runtime requires the supported original server image");return 0;}
    std::string error;
    const auto exists=[](const std::filesystem::path& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;};
    const auto skillPath=root/L"skill-identities.bin",skillAuthority=sdgo/L"install-state"/L"unit-skill-authority.bin";
    const auto graphPath=root/L"form-graphs.bin",graphReceipt=sdgo/L"install-state"/L"unit-form-authority.bin";
    const bool skills=exists(skillPath)||exists(skillAuthority),graphs=exists(graphPath)||exists(graphReceipt);
    if(!skills&&!graphs){api->Log(mod,SDGO_LOG_ERROR,"unit server runtime needs paired content and independent installation authority");return 0;}
    std::shared_ptr<const Bindings> bindings;
    if(skills){
        if(!Read(skillPath,candidate,64+format::kMaxBytes)||!Read(skillAuthority,authority,128+format::kMaxBytes+400000)||!g_input.Parse(candidate,authority,error)){
            api->Log(mod,SDGO_LOG_ERROR,"unit skill installation rejected: %s",error.c_str());return 0;}
        g_lookup.registry=api->Symbol(mod,"loadout.skill_registry");
        g_lookup.query=reinterpret_cast<NativeLookup::Find>(api->Symbol(mod,"loadout.find_skill"));
        if(!g_lookup.registry||!g_lookup.query)return 0;
        bindings=Bindings::Build(g_input.candidates,g_input.authority,error);if(!bindings)return 0;
    }
    if(graphs){
        if(!Read(graphPath,graphCandidate,64+sdgo::unit_forms::graph_format::kMaxBytes)||
           !Read(graphReceipt,graphAuthority,sdgo::unit_forms::graph_format::kMaxAuthorityBytes)||!g_graphs.Parse(graphCandidate,graphAuthority,error)){
            api->Log(mod,SDGO_LOG_ERROR,"unit form installation rejected: %s",error.c_str());return 0;}
        g_capture=std::make_unique<unit_catalog_server::HumanBuildCapture>();
        if(!g_capture->Prepare(api,mod,g_graphs.bindings,error)){
            api->Log(mod,SDGO_LOG_ERROR,"unit form lifecycle capture rejected: %s",error.c_str());return 0;}
        g_formState.size=sizeof(g_formState);g_formState.version=1;g_formState.capture_installed=1;
        std::memcpy(g_formState.candidate_sha256,g_graphs.candidateSha256.c_str(),65);
        const auto digest=sdgo::scripts::Sha256(graphAuthority);std::memcpy(g_formState.authority_sha256,digest.c_str(),65);
        std::memcpy(g_formState.server_fingerprint,g_graphs.serverFingerprint.c_str(),65);
    }
    if(skills&&!InstallNative(api,mod,std::move(bindings),&g_lookup,error)){
        api->Log(mod,SDGO_LOG_ERROR,"unit skill native binding rejected: %s",error.c_str());return 0;}
    if(graphs&&!g_graphs.nativeDefinitions.empty()&&!unit_catalog_server::InstallFormGate(
            api,mod,g_graphs.bindings,&g_graphs.nativeDefinitions,g_capture.get(),error)){
        api->Log(mod,SDGO_LOG_ERROR,"unit form native preflight rejected: %s",error.c_str());return 0;}
    if(skills){
    g_state.size=sizeof(g_state);g_state.version=1;
    for(const auto& owner:g_input.candidates)g_state.bindings+=static_cast<std::uint32_t>(owner.skills.size());
    const auto digest=sdgo::scripts::Sha256(authority);
    std::memcpy(g_state.candidate_sha256,g_input.candidateSha256.c_str(),65);
    std::memcpy(g_state.authority_sha256,digest.c_str(),65);
    std::memcpy(g_state.server_fingerprint,g_input.serverFingerprint.c_str(),65);g_state.installed=1;
    api->Log(mod,SDGO_LOG_INFO,"unit skill bindings installed: %lu mappings; server fingerprint %s",static_cast<DWORD>(g_state.bindings),g_state.server_fingerprint);
    }
    if(graphs)api->Log(mod,SDGO_LOG_INFO,"unit form lifecycle/preflight prepared; Mod-authored form rebinding remains inactive");
    // A registered provider retains raw callback/user pointers for this process.
    // No fallible work or logging may follow this final external registration.
    return graphs?(g_capture->Attach()?1:0):1;
}
