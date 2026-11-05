#include <windows.h>
#include <filesystem>
#include "sdgo/native_skill_identity.h"
#include "sdgo/skill_identity_format.h"
#include "sdgo/unit_skill_runtime_state.h"

namespace {
using namespace sdgo::skill_identity;
RuntimeInput g_input;
SdgoUnitSkillRuntimeStateV1 g_state={};
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

extern "C" __declspec(dllexport) int __cdecl SdgoReadUnitSkillRuntimeStateV1(SdgoUnitSkillRuntimeStateV1* state){
    if(!state||state->size!=sizeof(*state)||state->version!=1||!g_state.installed)return 0;
    *state=g_state;const auto native=sdgo::skill_identity::ReadNativeStatus();
    state->enable_aliases=native.enableAliases;state->disable_aliases=native.disableAliases;state->rejected=native.rejected;return 1;
}

SDGO_MOD_ENTRY {
    if(!api||api->version!=1||!api->SettingInt(mod,"Enabled",1))return 0;
    const auto root=std::filesystem::path(api->ModDirectory(mod));
    const auto sdgo=root.parent_path().parent_path();
    std::vector<std::uint8_t> candidate,authority,host;
    wchar_t image[MAX_PATH]={};
    if(!GetModuleFileNameW(nullptr,image,MAX_PATH)||!Read(image,host,64u*1024u*1024u)||
       sdgo::scripts::Sha256(host)!="f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4"){
        api->Log(mod,SDGO_LOG_ERROR,"unit skill bindings require the supported original server image");return 0;}
    if(!Read(root/L"skill-identities.bin",candidate,64+format::kMaxBytes)||
       !Read(sdgo/L"install-state"/L"unit-skill-authority.bin",authority,128+format::kMaxBytes+400000)){
        api->Log(mod,SDGO_LOG_ERROR,"unit skill bindings require matching content and an independent server installation receipt");return 0;}
    std::string error;
    if(!g_input.Parse(candidate,authority,error)){
        api->Log(mod,SDGO_LOG_ERROR,"unit skill installation rejected: %s",error.c_str());return 0;}
    g_lookup.registry=api->Symbol(mod,"loadout.skill_registry");
    g_lookup.query=reinterpret_cast<NativeLookup::Find>(api->Symbol(mod,"loadout.find_skill"));
    if(!g_lookup.registry||!g_lookup.query)return 0;
    auto bindings=Bindings::Build(g_input.candidates,g_input.authority,error);
    if(!bindings||!InstallNative(api,mod,std::move(bindings),&g_lookup,error)){
        api->Log(mod,SDGO_LOG_ERROR,"unit skill native binding rejected: %s",error.c_str());return 0;}
    g_state.size=sizeof(g_state);g_state.version=1;
    for(const auto& owner:g_input.candidates)g_state.bindings+=static_cast<std::uint32_t>(owner.skills.size());
    const auto digest=sdgo::scripts::Sha256(authority);
    std::memcpy(g_state.candidate_sha256,g_input.candidateSha256.c_str(),65);
    std::memcpy(g_state.authority_sha256,digest.c_str(),65);
    std::memcpy(g_state.server_fingerprint,g_input.serverFingerprint.c_str(),65);g_state.installed=1;
    api->Log(mod,SDGO_LOG_INFO,"unit skill bindings installed: %lu mappings; server fingerprint %s",static_cast<DWORD>(g_state.bindings),g_state.server_fingerprint);
    return 1;
}
