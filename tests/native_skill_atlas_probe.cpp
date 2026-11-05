// Unshipped read-only observer. Gameplay/navigation belong to GameTest's native
// cases; this worker only records the product's real sprite-player snapshots.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/skill_atlas_state.h"

namespace {
std::wstring outputPath;
std::wstring battleOutputPath;
char runId[80]={};
void Text(FILE* file,const char* text){
    std::fputc('"',file);for(;*text;++text)std::fputc(*text=='\\'?'/':*text,file);std::fputc('"',file);
}
DWORD WINAPI Observe(void*){
    const auto start=GetTickCount64();HMODULE module=nullptr;SdgoReadSkillAtlasStateV1Fn read=nullptr;
    SdgoReadSkillAtlasBattleStateV1Fn battleRead=nullptr;
    SdgoSkillAtlasStateV1 previous={};
    SdgoSkillAtlasBattleStateV1 previousBattle={};
    while(GetTickCount64()-start<600000){
        if(!module&&GetModuleHandleExW(0,L"unit_catalog.dll",&module)){
            read=reinterpret_cast<SdgoReadSkillAtlasStateV1Fn>(GetProcAddress(module,"SdgoReadSkillAtlasStateV1"));
            battleRead=reinterpret_cast<SdgoReadSkillAtlasBattleStateV1Fn>(GetProcAddress(module,"SdgoReadSkillAtlasBattleStateV1"));
        }
        SdgoSkillAtlasStateV1 state={};state.size=sizeof(state);state.version=1;
        if(read&&read(&state)&&(state.slots[0].calls||state.slots[1].calls)&&std::memcmp(&state,&previous,sizeof(state))){
            FILE* file=nullptr;
            if(_wfopen_s(&file,outputPath.c_str(),L"ab")||!file)return 1;
            std::fprintf(file,"{\"run_id\":\"%s\",\"pid\":%lu,\"failures\":%lu,\"sidecar_sha256\":\"%s\",\"slots\":[",runId,GetCurrentProcessId(),static_cast<DWORD>(state.failures),state.sidecar_sha256);
            for(unsigned i=0;i<2;++i){const auto& slot=state.slots[i];if(i)std::fputc(',',file);
                std::fprintf(file,"{\"skill\":%lu,\"calls\":%lu,\"owner_loads\":%lu,\"baseline_loads\":%lu,\"frame\":%lu,\"actual_frame\":%lu,\"frame_first\":%lu,\"frame_last\":%lu,\"player\":\"0x%08lX\",\"sprite\":\"",
                    static_cast<DWORD>(slot.skill_id),static_cast<DWORD>(slot.calls),static_cast<DWORD>(slot.owner_loads),static_cast<DWORD>(slot.baseline_loads),
                    static_cast<DWORD>(slot.requested_frame),static_cast<DWORD>(slot.actual_frame),static_cast<DWORD>(slot.frame_first),static_cast<DWORD>(slot.frame_last),static_cast<DWORD>(slot.player));
                for(const char* p=slot.sprite;*p;++p)std::fputc(*p=='\\'?'/':*p,file);
                std::fputs("\"}",file);
            }
            std::fputs("]}\n",file);std::fclose(file);previous=state;
        }
        SdgoSkillAtlasBattleStateV1 battle={};battle.size=sizeof(battle);battle.version=1;
        if(battleRead&&battleRead(&battle)&&battle.calls&&std::memcmp(&battle,&previousBattle,sizeof(battle))){
            FILE* file=nullptr;if(_wfopen_s(&file,battleOutputPath.c_str(),L"ab")||!file)return 2;
            std::fprintf(file,"{\"run_id\":\"%s\",\"pid\":%lu,\"calls\":%lu,\"owner_loads\":%lu,\"baseline_loads\":%lu,\"failures\":%lu,\"entries\":[",
                runId,GetCurrentProcessId(),static_cast<DWORD>(battle.calls),static_cast<DWORD>(battle.owner_loads),static_cast<DWORD>(battle.baseline_loads),static_cast<DWORD>(battle.failures));
            for(unsigned i=0;i<battle.count&&i<16;++i){const auto& entry=battle.entries[i];if(i)std::fputc(',',file);
                std::fprintf(file,"{\"skill\":%lu,\"frame\":%lu,\"calls\":%lu,\"sprite\":",static_cast<DWORD>(entry.skill_id),static_cast<DWORD>(entry.frame),static_cast<DWORD>(entry.calls));
                Text(file,entry.sprite);std::fputs(",\"texture\":",file);Text(file,entry.texture);
                std::fprintf(file,",\"uv\":[%.9g,%.9g,%.9g,%.9g]}",entry.uv[0],entry.uv[1],entry.uv[2],entry.uv[3]);
            }
            std::fputs("]}\n",file);std::fclose(file);previousBattle=battle;
        }
        Sleep(100);
    }
    if(module)FreeLibrary(module);return 0;
}
}
SDGO_MOD_ENTRY{
    if(!api||api->version!=1||GetEnvironmentVariableA("SDGO_GAME_TEST_RUN",runId,sizeof(runId))!=36)return 0;
    outputPath=std::wstring(api->ModDirectory(mod))+L"\\..\\..\\gametest\\skill-atlas-observations.jsonl";
    battleOutputPath=std::wstring(api->ModDirectory(mod))+L"\\..\\..\\gametest\\skill-atlas-battle-observations.jsonl";
    HANDLE worker=CreateThread(nullptr,0,Observe,nullptr,0,nullptr);if(!worker)return 0;CloseHandle(worker);return 1;
}
