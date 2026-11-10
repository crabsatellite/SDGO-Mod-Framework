// Development-only native fixture for the OC special GameTest. Never included in runtime releases.
// BattleServer admits a human pilot's special request (C2S 0x04BE, handler 0x4C3820) only with a
// full SP gauge: the int at room pilot +5 must reach 30000 at 0x4C3AEA. Charging it in combat takes
// about 27,300 damage dealt, which a short isolated battle does not provide, and the native debug
// "SP FULL" switch has no reader. At that check this fixture fills the gauge of the authenticated
// host pilot only, right before the native comparison, and records every fill for the client run.
// The comparison, the request's other checks, admission, SP cost, damage and notifications stay
// native; AI pilots never reach this instruction and other humans are left alone.
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include "sdgo/sdgo_api.h"

namespace {
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
void* resume=nullptr;
std::wstring output,runId;
uint32_t host=0,requests=0,fills=0;
int32_t lastBefore=-1;
constexpr int32_t kFullGauge=30000;
constexpr size_t kFramePilotId=0x328D8,kFramePilot=0x328DC,kPilotSp=5;

void WriteMarker() {
    const std::wstring text=L"[probe]\r\nrun_id="+runId+L"\r\nhost_pilot_id="+std::to_wstring(host)+
        L"\r\nrequests="+std::to_wstring(requests)+L"\r\nfills="+std::to_wstring(fills)+
        L"\r\nlast_sp_before="+std::to_wstring(lastBefore)+L"\r\n";
    const std::wstring temp=output+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    const wchar_t bom=0xFEFF;DWORD written=0;WriteFile(file,&bom,2,&written,nullptr);
    const DWORD bytes=static_cast<DWORD>(text.size()*sizeof(wchar_t));
    const bool complete=WriteFile(file,text.data(),bytes,&written,nullptr) && written==bytes;
    CloseHandle(file);
    if(complete)MoveFileExW(temp.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING);
}

// EBP is the native handler's frame: [ebp-0x328D8] the requesting pilot id, [ebp-0x328DC] its room
// pilot record, whose first dword repeats the id.
void __stdcall SpecialRequest(uint8_t* frame) {
    __try {
        uint32_t id=0;uint8_t* pilot=nullptr;
        std::memcpy(&id,frame-kFramePilotId,sizeof(id));
        std::memcpy(&pilot,frame-kFramePilot,sizeof(pilot));
        uint32_t recorded=0;
        if(!host || id!=host || !pilot)return;
        std::memcpy(&recorded,pilot,sizeof(recorded));
        if(recorded!=id)return;
        int32_t sp=0;std::memcpy(&sp,pilot+kPilotSp,sizeof(sp));
        ++requests;lastBefore=sp;
        if(sp<kFullGauge) {std::memcpy(pilot+kPilotSp,&kFullGauge,sizeof(kFullGauge));++fills;}
        WriteMarker();
        g_api->Log(g_mod,SDGO_LOG_INFO,"OC special fixture: host pilot %u special request %u, SP %d -> %d",
            id,requests,sp,sp<kFullGauge?kFullGauge:sp);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(naked) void SpecialGateSite() {__asm {
    pushfd
    pushad
    push ebp
    call SpecialRequest
    popad
    popfd
    jmp dword ptr [resume]
}}
}

SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    wchar_t exe[32768]{},env[64]{};GetModuleFileNameW(nullptr,exe,32768);
    auto root=std::filesystem::path(exe).parent_path();bool marked=false;
    for(int i=0;i<5 && !root.empty();++i,root=root.parent_path())
        if(std::filesystem::is_regular_file(root/L"TEST-ONLY.txt")){marked=true;break;}
    if(!marked || !GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",env,64))return 0;
    std::filesystem::path job=api->ModDirectory(mod);
    job=job.parent_path().parent_path()/L"gametest"/L"run.ini";
    wchar_t id[64]{},path[32768]{};
    GetPrivateProfileStringW(L"run",L"id",L"",id,64,job.c_str());
    if(wcscmp(id,env)!=0)return 0;
    runId=id;host=GetPrivateProfileIntW(L"observer",L"host_pilot_id",0,job.c_str());
    GetPrivateProfileStringW(L"oc_special_probe",L"output",L"",path,32768,job.c_str());
    output=path;
    if(!host || (host&0x80000000u) || output.empty() || std::filesystem::exists(output))return 0;
    // Every Battle host of the run loads this fixture; only the one holding the battle writes the marker.
    if(api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite) || !api->HookSite ||
       !api->HookSite(mod,"gametest.oc_special_sp_gate",reinterpret_cast<void*>(&SpecialGateSite),&resume))
        return 0;
    api->Log(mod,SDGO_LOG_INFO,"OC special fixture ready for host pilot %u",host);
    return 1;
}
