// Unshipped polling of the real form-gate consumer; no hook or observer sink.
#include <windows.h>
#include <share.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_form_gate_state.h"
namespace {
SdgoReadUnitFormGateStateV1Fn readState=nullptr;
char run[80]{};std::wstring output;
DWORD WINAPI Pump(void*){
    FILE* file=_wfsopen(output.c_str(),L"wb",_SH_DENYNO);if(!file)return 1;
    std::fprintf(file,"{\"event\":\"probe_ready\",\"run_id\":\"%s\",\"pid\":%lu}\n",run,GetCurrentProcessId());std::fflush(file);
    SdgoUnitFormGateStateV1 previous{};
    for(;;){
        SdgoUnitFormGateStateV1 state{};state.size=sizeof(state);state.version=1;
        if(readState(&state)&&std::memcmp(&state,&previous,sizeof(state))){
            std::fprintf(file,"{\"event\":\"gate_state\",\"run_id\":\"%s\",\"pid\":%lu,\"installed\":%u,\"expected_definitions\":%u,\"calls\":%llu,\"unmanaged\":%llu,\"cached_allowed\":%llu,\"uncached_directed\":%llu,\"rejected\":%llu,\"stale_capture\":%llu,\"definition_checks\":%llu,\"normalization_observations\":%llu,\"actor_writes\":%llu}\n",
                run,GetCurrentProcessId(),state.installed,state.expected_definitions,state.calls,state.unmanaged,state.cached_allowed,
                state.uncached_directed,state.rejected,state.stale_capture,state.definition_checks,state.normalization_observations,state.actor_writes);
            previous=state;std::fflush(file);
        }
        Sleep(20);
    }
}
}
SDGO_MOD_ENTRY {
    if(!api||api->version!=1)return 0;
    api->SettingString(mod,"RunId","",run,sizeof(run));if(std::strlen(run)!=36)return 0;
    for(const auto* p=run;*p;++p)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||*p=='-'))return 0;
    const auto module=GetModuleHandleW(L"unit_catalog_server.dll");if(!module)return 0;
    readState=reinterpret_cast<SdgoReadUnitFormGateStateV1Fn>(GetProcAddress(module,"SdgoReadUnitFormGateStateV1"));if(!readState)return 0;
    output=std::wstring(api->ModDirectory(mod))+L"\\gate-"+std::wstring(run,run+36)+L".jsonl";
    if(GetFileAttributesW(output.c_str())!=INVALID_FILE_ATTRIBUTES)return 0;
    HANDLE thread=CreateThread(nullptr,0,&Pump,nullptr,0,nullptr);if(!thread)return 0;CloseHandle(thread);return 1;
}
