// Unshipped read-only observer. Gameplay/navigation belong to GameTest's native
// cases; this worker only records the product's real sprite-player snapshots.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/skill_atlas_state.h"
#include "sdgo/unit_program_state.h"

namespace {
std::wstring outputPath;
std::wstring battleOutputPath;
std::wstring programOutputPath;
char runId[80]={};
void Text(FILE* file,const char* text){
    std::fputc('"',file);for(;*text;++text)std::fputc(*text=='\\'?'/':*text,file);std::fputc('"',file);
}
DWORD WINAPI Observe(void*){
    const auto start=GetTickCount64();HMODULE module=nullptr;SdgoReadSkillAtlasStateV1Fn read=nullptr;
    SdgoReadSkillAtlasBattleStateV1Fn battleRead=nullptr;
    SdgoGetUnitProgramCallStateV1 programRead=nullptr;
    SdgoGetUnitProgramStateV1 programStateRead=nullptr;
    SdgoSkillAtlasStateV1 previous={};
    SdgoSkillAtlasBattleStateV1 previousBattle={};
    std::vector<SdgoUnitProgramCallStateV1> previousPrograms;
    while(GetTickCount64()-start<600000){
        if(!module&&GetModuleHandleExW(0,L"unit_catalog.dll",&module)){
            read=reinterpret_cast<SdgoReadSkillAtlasStateV1Fn>(GetProcAddress(module,"SdgoReadSkillAtlasStateV1"));
            battleRead=reinterpret_cast<SdgoReadSkillAtlasBattleStateV1Fn>(GetProcAddress(module,"SdgoReadSkillAtlasBattleStateV1"));
            programRead=reinterpret_cast<SdgoGetUnitProgramCallStateV1>(GetProcAddress(module,"SdgoReadUnitProgramCallStateV1"));
            programStateRead=reinterpret_cast<SdgoGetUnitProgramStateV1>(GetProcAddress(module,"SdgoReadUnitProgramStateV1"));
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
        if(programRead&&programStateRead){
            SdgoUnitProgramStateV1 base={};base.size=sizeof(base);base.version=1;
            if(programStateRead(&base)&&base.active){
                for(unsigned i=0;i<base.export_count&&i<4096;++i){
                    SdgoUnitProgramCallStateV1 call={};call.size=sizeof(call);call.version=1;
                    if(!programRead(i,&call)||!call.calls)continue;
                    if(previousPrograms.size()<base.export_count)previousPrograms.resize(base.export_count);
                    if(!std::memcmp(&call,&previousPrograms[i],sizeof(call)))continue;
                    FILE* file=nullptr;if(_wfopen_s(&file,programOutputPath.c_str(),L"ab")||!file)return 3;
                    std::fprintf(file,"{\"run_id\":\"%s\",\"pid\":%lu,\"bundle_sha256\":\"%s\",\"available\":%lu,\"index\":%lu,\"total\":%lu,\"vm\":\"0x%08lX\",\"function\":\"0x%08lX\",\"name\":\"%s\",\"calls\":%lu,\"succeeded\":%lu,\"errors\":%lu,\"last_status\":%ld,\"last_argc\":%ld,\"first_caller\":\"0x%08lX\",\"last_caller\":\"0x%08lX\",\"first_thread\":%lu,\"last_thread\":%lu,\"root_thread\":%lu,\"first_qpc\":%llu,\"last_qpc\":%llu}\n",
                        runId,GetCurrentProcessId(),base.bundle_sha256,static_cast<DWORD>(call.available),static_cast<DWORD>(call.index),static_cast<DWORD>(call.total),
                        static_cast<DWORD>(call.vm),static_cast<DWORD>(call.function_identity),call.name,static_cast<DWORD>(call.calls),static_cast<DWORD>(call.succeeded),static_cast<DWORD>(call.errors),
                        static_cast<LONG>(call.last_status),static_cast<LONG>(call.last_argc),static_cast<DWORD>(call.first_caller),static_cast<DWORD>(call.last_caller),
                        static_cast<DWORD>(call.first_thread),static_cast<DWORD>(call.last_thread),static_cast<DWORD>(base.root_thread_id),
                        static_cast<unsigned long long>(call.first_qpc),static_cast<unsigned long long>(call.last_qpc));
                    std::fclose(file);previousPrograms[i]=call;
                }
            }
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
    programOutputPath=std::wstring(api->ModDirectory(mod))+L"\\..\\..\\gametest\\unit-program-invocations.jsonl";
    HANDLE worker=CreateThread(nullptr,0,Observe,nullptr,0,nullptr);if(!worker)return 0;CloseHandle(worker);return 1;
}
