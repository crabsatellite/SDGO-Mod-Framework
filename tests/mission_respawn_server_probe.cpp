// Development-only native combat fixture. Never included in runtime releases.
// An authenticated GameTest client creates the mission normally. This probe
// uses the shipped damage/kill path on one real AI, then observes production
// respawn ticks and packets. It never edits life quotas or death counters.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include "sdgo/sdgo_api.h"

namespace {
const SdgoApi* g_api=nullptr; SdgoMod* g_mod=nullptr;
using DamageFn=uint32_t(__thiscall*)(uint8_t*,uint32_t,uint32_t,uint8_t*,uint8_t*,uint32_t,uint32_t,uint32_t,uint16_t,uint8_t,uint32_t,uint8_t,uint32_t);
DamageFn damage=nullptr;
std::wstring output,runId;
uint32_t host=0; bool infinite=false,finished=false;
uint8_t* selected=nullptr;
uint32_t pilotId=0,limit=0,kills=0,respawns=0;
int targetSlot=-1,hostSlot=-1;
DWORD began=0,lastAction=0;
bool down=false;
bool primaryComplete=false;
uint32_t primaryDeaths=0,primaryRespawns=0,humanDeaths=0,aliveAiBeforeFailure=0;
constexpr size_t kPilots=0x3CE4,kStride=0x1F54;
template<class T>T Read(uint8_t* p,size_t at){T v;memcpy(&v,p+at,sizeof(v));return v;}
uint8_t* Pilot(uint8_t* room,int slot){return room+kPilots+slot*kStride;}
uint8_t* Unit(uint8_t* pilot){return pilot+0x340+pilot[0x33C]*0x658;}
void WriteMarker(const wchar_t* status,const wchar_t* reason,uint32_t deaths) {
    const std::wstring text=L"[probe]\r\nstatus="+std::wstring(status)+
        L"\r\nrun_id="+runId+L"\r\npilot="+std::to_wstring(pilotId)+L"\r\nlife_limit="+std::to_wstring(limit)+
        L"\r\ndeaths="+std::to_wstring(primaryComplete?primaryDeaths:deaths)+L"\r\nrespawns="+std::to_wstring(primaryComplete?primaryRespawns:respawns)+
        L"\r\nhuman_deaths="+std::to_wstring(humanDeaths)+L"\r\nai_alive_before_failure="+std::to_wstring(aliveAiBeforeFailure)+
        L"\r\nnative_human_failure="+std::to_wstring(finished && wcscmp(status,L"pass")==0?1:0)+
        L"\r\npolicy="+(infinite?L"infinite":L"player")+L"\r\nreason="+reason+L"\r\n";
    const std::wstring temp=output+L".tmp";
    HANDLE f=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;
    const wchar_t bom=0xFEFF;DWORD written=0;WriteFile(f,&bom,2,&written,nullptr);
    WriteFile(f,text.data(),static_cast<DWORD>(text.size()*sizeof(wchar_t)),&written,nullptr);
    FlushFileBuffers(f);CloseHandle(f);MoveFileExW(temp.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH|MOVEFILE_REPLACE_EXISTING);
}
void Finish(bool passed,const wchar_t* reason,uint32_t deaths) {
    if(finished)return;finished=true;
    WriteMarker(passed?L"pass":L"fail",reason,deaths);
    g_api->Log(g_mod,passed?SDGO_LOG_INFO:SDGO_LOG_ERROR,"native mission respawn probe %s: deaths=%u quota=%u respawns=%u",passed?"passed":"failed",deaths,limit,respawns);
}
void PrimaryComplete(const wchar_t* reason,uint32_t deaths) {
    primaryComplete=true;primaryDeaths=deaths;primaryRespawns=respawns;lastAction=GetTickCount();
    WriteMarker(L"basic",reason,deaths);
}
bool ClientAcknowledged() {
    const std::wstring ack=(std::filesystem::path(output).parent_path()/L"mission-respawn-client-ack.ini").wstring();
    wchar_t id[64]{};GetPrivateProfileStringW(L"ack",L"run_id",L"",id,64,ack.c_str());
    return runId==id;
}
bool Lethal(uint8_t* room,uint32_t now,int from,int to) {
    uint8_t* attacker=Pilot(room,from);uint8_t* victim=Pilot(room,to);
    const uint8_t team=attacker[4];
    __try {
        attacker[4]=victim[4]==1?2:1;
        for(int hit=0;hit<8 && Read<uint32_t>(Unit(victim),0x34)>0;++hit)
            damage(room,now,static_cast<uint32_t>(from),attacker,nullptr,Read<uint32_t>(victim,9),
                Read<uint32_t>(victim,0),13000,0,0,0,0,0);
    } __finally {attacker[4]=team;}
    return Read<uint32_t>(Unit(victim),0x34)==0;
}
void HumanFailureTick(uint8_t* room,uint32_t now,DWORD clock) {
    if(!ClientAcknowledged())return;
    uint8_t* human=Pilot(room,hostSlot);
    if(Read<uint32_t>(human,0)!=host || human[0x33C]>=4) {Finish(false,L"human identity changed before exhaustion proof",primaryDeaths);return;}
    if(Read<uint32_t>(Unit(human),0x34)==0 || clock-lastAction<1000)return;
    int attacker=-1;aliveAiBeforeFailure=0;
    for(int slot=0;slot<12;++slot) {
        auto p=Pilot(room,slot);
        if((Read<uint32_t>(p,0)&0x80000000u) && p[0x33C]<4 && Read<uint32_t>(Unit(p),0x34)>0) {++aliveAiBeforeFailure;attacker=slot;}
    }
    if(attacker<0) {Finish(false,L"no living native AI for human exhaustion fixture",primaryDeaths);return;}
    if(!Lethal(room,now,attacker,hostSlot))return;
    lastAction=clock;humanDeaths=Read<uint32_t>(human,0x1A);
    if(humanDeaths<limit)return;
    const bool failedImmediately=Read<uint32_t>(room,0x316A4)==6 && Read<uint32_t>(room,0x316A8)==0x13DF;
    bool nativeGateClosed=failedImmediately;
    if(failedImmediately) {
        auto survivor=Pilot(room,attacker);const auto hp=Read<uint32_t>(Unit(survivor),0x34);
        const auto deaths=Read<uint32_t>(survivor,0x1A);
        Lethal(room,now,hostSlot,attacker);
        nativeGateClosed=Read<uint32_t>(Unit(survivor),0x34)==hp && Read<uint32_t>(survivor,0x1A)==deaths;
    }
    Finish(failedImmediately && nativeGateClosed,failedImmediately && nativeGateClosed?L"last human life immediately triggers native failure and its existing damage gate despite living AI":L"native human exhaustion failure or damage gate was not immediate",primaryDeaths);
}
void Tick(uint8_t* room,uint32_t now) {
    if(finished || !room || (room[5]!=5 && room[5]!=8) || Read<uint32_t>(room,0x4CE)!=host)return;
    const DWORD clock=GetTickCount();
    if(!selected) {
        for(int slot=0;slot<12;++slot) {
            uint8_t* p=Pilot(room,slot);const auto id=Read<uint32_t>(p,0);
            if(id==host)hostSlot=slot;
            if((id&0x80000000u) && targetSlot<0 && p[0x33C]<4 && Read<uint32_t>(Unit(p),0x34)>0) {targetSlot=slot;pilotId=id;}
        }
        if(hostSlot<0 || targetSlot<0)return;
        uint8_t* rules=Read<uint8_t*>(room,0x316A0);if(!rules)return;
        limit=Read<uint32_t>(rules,0x28);selected=room;began=lastAction=clock;
        if(limit>12) {Finish(false,L"native mission quota outside bounded probe range",0);return;}
    }
    if(room!=selected)return;
    if(clock-began>330000) {Finish(false,L"native respawn probe timed out",0);return;}
    if(primaryComplete) {HumanFailureTick(room,now,clock);return;}
    auto victim=Pilot(room,targetSlot),attacker=Pilot(room,hostSlot);
    const auto deaths=Read<uint32_t>(victim,0x1A);
    if(Read<uint32_t>(victim,0)!=pilotId || Read<uint32_t>(attacker,0)!=host || victim[0x33C]>=4) {
        Finish(false,L"native participant identity changed",deaths);return;
    }
    const auto hp=Read<uint32_t>(Unit(victim),0x34);
    if(down && hp>0) {
        if(!infinite && deaths>=limit) {Finish(false,L"player-policy AI revived after exhausting its quota",deaths);return;}
        ++respawns;down=false;lastAction=clock;
        g_api->Log(g_mod,SDGO_LOG_INFO,"probe observed native revival %u after death %u",respawns,deaths);
        if(infinite && deaths>=limit+2) {PrimaryComplete(L"native revivals exceed player quota",deaths);return;}
    }
    if(down) {
        if(!infinite && deaths>=limit && clock-lastAction>=15000)PrimaryComplete(L"native player quota exhausted and wingman stays down",deaths);
        return;
    }
    if(clock-began<20000 || clock-lastAction<1000)return;
    if(hp==0) {down=true;lastAction=clock;return;}
    // Make the real authenticated human an opposing attacker for this isolated
    // damage call only. The original team byte is restored even on exception.
    // All victim HP, death stats, deadlines and broadcasts are native effects.
    if(Lethal(room,now,hostSlot,targetSlot)) {
        ++kills;down=true;lastAction=clock;
        if(Read<uint32_t>(victim,0x1A)!=deaths+1) {Finish(false,L"native lethal damage did not increment death counter once",Read<uint32_t>(victim,0x1A));return;}
        g_api->Log(g_mod,SDGO_LOG_INFO,"probe native lethal damage %u, original team restored, quota=%u",kills,limit);
        g_api->Log(g_mod,SDGO_LOG_INFO,"probe human native state id=%u deaths=%u hp=%u phase=%u reason=%u",
            Read<uint32_t>(attacker,0),Read<uint32_t>(attacker,0x1A),Read<uint32_t>(Unit(attacker),0x34),
            Read<uint32_t>(room,0x316A4),Read<uint32_t>(room,0x316A8));
    }
}
void __stdcall GuardedTick(uint8_t* room,uint32_t now) {
    __try {Tick(room,now);} __except(EXCEPTION_EXECUTE_HANDLER) {Finish(false,L"native probe exception",0);}
}
void* resume=nullptr;
__declspec(naked) void TickSite() {__asm {
    pushfd
    pushad
    push dword ptr [ebp+8]
    push dword ptr [ebp-20h]
    call GuardedTick
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
    GetPrivateProfileStringW(L"mission_respawn_probe",L"output",L"",path,32768,job.c_str());
    output=path;infinite=GetPrivateProfileIntW(L"mission_respawn_probe",L"infinite",0,job.c_str())==1;
    if(!host || output.empty() || std::filesystem::exists(output))return 0;
    damage=reinterpret_cast<DamageFn>(api->Symbol(mod,"items.accepted_damage"));
    if(!damage)return 0;
    return api->HookCode(mod,"gametest.mission_respawn_tick",reinterpret_cast<void*>(&TickSite),&resume);
}
