// Unshipped read-only observation around original server dispatchers. Product
// unit_catalog_server owns the selector adaptation; this probe never invokes
// dispatchers, changes definitions or edits actor flags.
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_skill_runtime_state.h"

namespace {
using Dispatch=std::uint32_t(__thiscall*)(void*,const void*,void*);
Dispatch g_enable=nullptr,g_disable=nullptr;
std::set<std::uint32_t> g_ids;
std::wstring g_output;
char g_run[80]={};
CRITICAL_SECTION g_lock;
struct Snapshot{std::uint32_t id=0;std::uint8_t flag=0;bool valid=false;};
Snapshot Read(const void* record,void* actor){
    Snapshot state;
    __try{
        state.id=*static_cast<const std::uint32_t*>(record);
        state.flag=*(static_cast<const std::uint8_t*>(actor)+0x1cde);state.valid=true;
    }__except(EXCEPTION_EXECUTE_HANDLER){}
    return state;
}
void Observe(bool enable,void* context,const void* record,void* actor,const Snapshot& before,std::uint32_t result){
    const DWORD error=GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
    const auto after=Read(record,actor);
    if(before.valid&&g_ids.count(before.id)){
        SdgoUnitSkillRuntimeStateV1 installed={};installed.size=sizeof(installed);installed.version=1;
        HMODULE module=GetModuleHandleW(L"unit_catalog_server.dll");
        const auto read=module?reinterpret_cast<SdgoReadUnitSkillRuntimeStateV1Fn>(GetProcAddress(module,"SdgoReadUnitSkillRuntimeStateV1")):nullptr;
        const bool available=read&&read(&installed);
        EnterCriticalSection(&g_lock);FILE* file=nullptr;
        if(!_wfopen_s(&file,g_output.c_str(),L"ab")&&file){
            LARGE_INTEGER now={};QueryPerformanceCounter(&now);
            std::fprintf(file,"{\"run_id\":\"%s\",\"pid\":%lu,\"thread\":%lu,\"qpc\":%lld,\"enable\":%s,\"context\":\"0x%08lx\",\"definition\":\"0x%08lx\",\"actor\":\"0x%08lx\",\"id_before\":%lu,\"id_after\":%lu,\"flag_before\":%u,\"flag_after\":%u,\"read_after_valid\":%s,\"native_return\":%lu,\"product_available\":%s,\"candidate_sha256\":\"%s\",\"authority_sha256\":\"%s\",\"server_fingerprint\":\"%s\",\"enable_aliases\":%lu,\"disable_aliases\":%lu,\"rejected\":%lu}\n",
                g_run,GetCurrentProcessId(),GetCurrentThreadId(),now.QuadPart,enable?"true":"false",
                reinterpret_cast<DWORD>(context),reinterpret_cast<DWORD>(record),reinterpret_cast<DWORD>(actor),
                static_cast<DWORD>(before.id),static_cast<DWORD>(after.id),before.flag,after.flag,after.valid?"true":"false",static_cast<DWORD>(result),available?"true":"false",
                installed.candidate_sha256,installed.authority_sha256,installed.server_fingerprint,
                static_cast<DWORD>(installed.enable_aliases),static_cast<DWORD>(installed.disable_aliases),static_cast<DWORD>(installed.rejected));
            std::fclose(file);
        }
        LeaveCriticalSection(&g_lock);
    }
    _fxrstor(fp);SetLastError(error);
}
std::uint32_t __fastcall Enable(void* context,void*,const void* record,void* actor){
    const auto before=Read(record,actor);const auto result=g_enable(context,record,actor);Observe(true,context,record,actor,before,result);return result;
}
std::uint32_t __fastcall Disable(void* context,void*,const void* record,void* actor){
    const auto before=Read(record,actor);const auto result=g_disable(context,record,actor);Observe(false,context,record,actor,before,result);return result;
}
}
SDGO_MOD_ENTRY {
    if(!api||api->version!=1)return 0;
    api->SettingString(mod,"RunId","",g_run,sizeof(g_run));if(std::strlen(g_run)!=36)return 0;
    for(const char* p=g_run;*p;++p)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||*p=='-'))return 0;
    char ids[1024]={};api->SettingString(mod,"SkillIds","",ids,sizeof(ids));const char* cursor=ids;
    while(*cursor){std::uint32_t id=0;bool digit=false;
        while(*cursor>='0'&&*cursor<='9'){digit=true;id=id*10+static_cast<unsigned>(*cursor++-'0');if(id>9999999)return 0;}
        if(!digit||!id||g_ids.size()>=64||!g_ids.insert(id).second)return 0;
        if(!*cursor)break;if(*cursor++!=','||!*cursor)return 0;
    }
    if(g_ids.empty())return 0;
    InitializeCriticalSection(&g_lock);g_output=std::wstring(api->ModDirectory(mod))+L"\\selector-observations.jsonl";
    return api->HookCode(mod,"probe.skill_enable",reinterpret_cast<void*>(&Enable),reinterpret_cast<void**>(&g_enable))&&
           api->HookCode(mod,"probe.skill_disable",reinterpret_cast<void*>(&Disable),reinterpret_cast<void**>(&g_disable));
}
