// Unshipped diagnostic of the actual imported-unit server consumer. Reads only
// its exported process-local status, without registering another observer sink.
#include <windows.h>
#include <share.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_form_runtime_state.h"
namespace {
SdgoReadUnitFormRuntimeStateV1Fn readState=nullptr;
std::wstring output;
char run[80]{};
DWORD WINAPI Pump(void*) {
    FILE* file=_wfsopen(output.c_str(),L"wb",_SH_DENYNO);if(!file)return 1;
    std::fprintf(file,"{\"event\":\"probe_ready\",\"run_id\":\"%s\",\"pid\":%lu,\"actor_writes\":false}\n",run,GetCurrentProcessId());std::fflush(file);
    SdgoUnitFormRuntimeStateV1 previous{};
    for(;;) {
        SdgoUnitFormRuntimeStateV1 state{};state.size=sizeof(state);state.version=1;
        if(readState(&state)&&std::memcmp(&state,&previous,sizeof(state))) {
            std::fprintf(file,"{\"event\":\"capture_state\",\"run_id\":\"%s\",\"pid\":%lu,\"installed\":%u,\"healthy\":%u,\"rooms\":%u,\"players\":%u,\"pending\":%u,\"completed\":%llu,\"rejected\":%llu,\"live_commits\":%llu,\"root\":%u,\"owner\":%u,\"deck\":%u,\"definition\":%u,\"room\":%u,\"player\":%u,\"serial\":%llu,\"room_epoch\":%llu,\"generation\":%llu,\"candidate\":\"%s\",\"authority\":\"%s\",\"server_fingerprint\":\"%s\"}\n",
                run,GetCurrentProcessId(),state.capture_installed,state.observer_healthy,state.rooms,state.players,state.pending,
                state.completed,state.rejected,state.live_transform_commits,state.sample_root,state.sample_owner,state.sample_deck,
                state.sample_definition,state.sample_room,state.sample_player,state.sample_serial,state.sample_room_epoch,state.sample_generation,
                state.candidate_sha256,state.authority_sha256,state.server_fingerprint);
            previous=state;std::fflush(file);
        }
        Sleep(20);
    }
}
}
SDGO_MOD_ENTRY {
    if(!api||api->version!=1)return 0;
    api->SettingString(mod,"RunId","",run,sizeof(run));if(std::strlen(run)!=36)return 0;
    for(const char* c=run;*c;++c)if(!((*c>='0'&&*c<='9')||(*c>='a'&&*c<='f')||*c=='-'))return 0;
    const auto module=GetModuleHandleW(L"unit_catalog_server.dll");if(!module)return 0;
    readState=reinterpret_cast<SdgoReadUnitFormRuntimeStateV1Fn>(GetProcAddress(module,"SdgoReadUnitFormRuntimeStateV1"));if(!readState)return 0;
    output=std::wstring(api->ModDirectory(mod))+L"\\capture-"+std::wstring(run,run+36)+L".jsonl";
    if(GetFileAttributesW(output.c_str())!=INVALID_FILE_ATTRIBUTES)return 0;
    HANDLE thread=CreateThread(nullptr,0,&Pump,nullptr,0,nullptr);if(!thread)return 0;CloseHandle(thread);return 1;
}
