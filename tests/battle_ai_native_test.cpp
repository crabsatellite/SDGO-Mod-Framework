// Real core + real battle_ai DLL + official Lua 5.0 VM. Game entry points are
// private fixtures: no game, account, network socket, or existing process used.
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string>
#include <vector>
#include <thread>
#include "sdgo/sdgo_api.h"
#include "sdgo/battle_ai_observer.h"
#include "../mods/battle_ai/native_protection.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "battle_ai_native_world.h"
namespace {
static_assert(sizeof(SdgoBattleAiUnitSnapshotV1)==208);
static_assert(sizeof(SdgoBattleAiMissionBossSpecialV1)==112);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,ranged_attempts)==168);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,ranged_nonzero_returns)==176);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,mobile_primary_dispatches)==184);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,mobile_defensive_dispatches)==192);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,backward_primary_dispatches)==200);
lua_State* g_L = nullptr;
int g_failures = 0;
int g_expectedDispatchResult = 123;
bool g_skipNativeDispatch=false;
std::wstring g_root;
void WaitNativeTicks(ULONGLONG milliseconds) {
    // The hooks use GetTickCount64, whose clock can advance in 15.6 ms steps.
    // A wall-clock Sleep(20/25) can leave their 20 ms sample gate unopened.
    const auto started=GetTickCount64();
    while(GetTickCount64()-started<milliseconds)Sleep(1);
}
void Check(bool ok,const char* what) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",what); if(!ok) ++g_failures;
}
bool Eval(const char* s) {
    int top=lua_gettop(g_L);
    bool ok=luaL_loadbuffer(g_L,s,strlen(s),"=native fixture")==0 && lua_pcall(g_L,0,0,0)==0;
    if(!ok) {std::printf("LUA ERROR %s\n",lua_tostring(g_L,-1));++g_failures;}
    lua_settop(g_L,top); return ok;
}
double GlobalNumber(const char* name) {
    lua_getglobal(g_L,name);
    const double value=lua_tonumber(g_L,-1);
    lua_pop(g_L,1);
    return value;
}
lua_State* __cdecl State(lua_State* value) { if(value) g_L=value; return g_L; }
int QueueFixtureSwing(lua_State*) {
    auto fsm=native_world::fsms[0].data();
    native_world::Put(fsm,0x240,int32_t(0));native_world::Put(fsm,0x248,native_world::weapons[0].data());
    native_world::Put(fsm,8,fsm+0x5C);
    return 0;
}
struct Unit {
    unsigned char bytes[0x3300] = {};
    __declspec(noinline) bool Think() {
        if(!bytes[0x326C]) return true;
        // Model the independent shared physics effect before the NPC-only
        // pause check. The enhancement must leave this query and fuel loss.
        if(reinterpret_cast<native_world::Callbacks*>(this)->EffectActive(5))
            native_world::Encode(bytes,0xBE1,0xBE5,0xBF5,0.0f);
        if(native_world::NpcBoostPauseQuery(bytes)) {
            reinterpret_cast<native_world::Callbacks*>(this)->StopMotor();
            reinterpret_cast<native_world::Callbacks*>(this)->AttackIntent(0);return true;
        }
        if(!g_skipNativeDispatch) {
            int result=native_world::Dispatch("VSAI_Gunfighter_01_Update",bytes);
            Check(result==g_expectedDispatchResult,"native dispatcher callback preserves its return value");
        }
        reinterpret_cast<native_world::Callbacks*>(this)->Intent();
        if(*reinterpret_cast<uint8_t**>(bytes+8)) reinterpret_cast<native_world::Callbacks*>(this)->FacingUpdate();
        reinterpret_cast<native_world::Callbacks*>(this)->ConsumeWeaponInput();
        return true;
    }
    __declspec(noinline) void Init(void* data) { bytes[0x100]=static_cast<unsigned char>(reinterpret_cast<uintptr_t>(data)); }
};
template<typename T> void* Address(T method) {
    static_assert(sizeof(T)==sizeof(void*),"single inheritance fixture ABI");
    void* p; memcpy(&p,&method,sizeof(p)); return p;
}
void Symbol(FILE* f,const char* name,void* addr,bool bad=false) {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f,"[%s]\nkind=code\nrva=0x%08lX\nexpect=",name,static_cast<unsigned long>(reinterpret_cast<uintptr_t>(addr)-base));
    auto p=static_cast<unsigned char*>(addr);
    for(int i=0;i<8;++i) std::fprintf(f,"%02X ",bad?0:p[i]);
    std::fprintf(f,"\n\n");
}
void Symbols(bool bad) {
    auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    wchar_t name[128];
    swprintf_s(name,L"\\sdgo\\symbols\\battle_ai_native_test-%08lX-%08lX.ini",
               nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    FILE* f=nullptr; _wfopen_s(&f,(g_root+name).c_str(),L"wb");
    if(!f) { Check(false,"open fixture symbols"); return; }
    std::fprintf(f,"[host]\nexe=battle_ai_native_test.exe\ntimestamp=0x%08lX\nsize_of_image=0x%08lX\n\n",
                 nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    Symbol(f,"npc.think",Address(&Unit::Think),bad); Symbol(f,"npc.init_ai",Address(&Unit::Init));
    Symbol(f,"lua_tinker.state",reinterpret_cast<void*>(&State));
    Symbol(f,"lua.gettop",reinterpret_cast<void*>(&lua_gettop));
    Symbol(f,"lua.settop",reinterpret_cast<void*>(&lua_settop));
    Symbol(f,"lua.tostring",reinterpret_cast<void*>(&lua_tostring));
    Symbol(f,"lua.pushnumber",reinterpret_cast<void*>(&lua_pushnumber));
    Symbol(f,"lua.pushstring",reinterpret_cast<void*>(&lua_pushstring));
    Symbol(f,"lua.pushcclosure",reinterpret_cast<void*>(&lua_pushcclosure));
    Symbol(f,"lua.gettable",reinterpret_cast<void*>(&lua_gettable));
    Symbol(f,"lua.settable",reinterpret_cast<void*>(&lua_settable));
    Symbol(f,"lua.pcall",reinterpret_cast<void*>(&lua_pcall));
    Symbol(f,"lua.load",reinterpret_cast<void*>(&lua_load));
    Symbol(f,"lua.type",reinterpret_cast<void*>(&lua_type));
    Symbol(f,"lua.tonumber",reinterpret_cast<void*>(&lua_tonumber));
    Symbol(f,"lua.newtable",reinterpret_cast<void*>(&lua_newtable));
    native_world::Symbols(f,Symbol);
    fclose(f);
}
void Config(const char* value) {
    FILE* f=nullptr;
    _wfopen_s(&f,(g_root+L"\\sdgo\\mods\\battle_ai\\config.ini").c_str(),L"wb");
    if(f) { fputs(value,f); fclose(f); }
}
bool FileContains(const std::wstring& path,const char* needle) {
    HANDLE file=CreateFileW(
        path.c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file,&size)||size.QuadPart<0||
       size.QuadPart>8*1024*1024) {
        CloseHandle(file);return false;
    }
    std::vector<char> text(static_cast<size_t>(size.QuadPart)+1,0);
    DWORD read=0;
    const bool ok=text.size()==1||
        ReadFile(file,text.data(),static_cast<DWORD>(size.QuadPart),&read,nullptr)!=0;
    CloseHandle(file);
    if(!ok)return false;
    text[read]=0;
    return strstr(text.data(),needle)!=nullptr;
}
}
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    bool disabled=argc>1 && !wcscmp(argv[1],L"disabled");
    bool bad=argc>1 && !wcscmp(argv[1],L"bad-signature");
    bool missing=argc>1 && !wcscmp(argv[1],L"missing-script");
    bool generic=argc>1 && !wcscmp(argv[1],L"generic");
    bool easy=argc>1 && !wcscmp(argv[1],L"easy");
    bool configIndependence=argc>1 && !wcscmp(argv[1],L"config-independence");
    bool projectileLead=argc>1 && !wcscmp(argv[1],L"projectile-lead");
    bool forms=argc>1 && !wcscmp(argv[1],L"forms");
    bool medium=argc>1 && (!wcscmp(argv[1],L"medium")||!wcscmp(argv[1],L"enabled"));
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr,exe,MAX_PATH); *wcsrchr(exe,L'\\')=0; g_root=exe;
    Symbols(bad);
    g_L=lua_open(); luaopen_base(g_L); lua_settop(g_L,0); luaopen_string(g_L); lua_settop(g_L,0);
    native_world::state=g_L;
    Check(Eval(R"(
      UNIT_MOVE_EVENT_MOVE=2;UNIT_MOVE_EVENT_DASH=3
      UNIT_MOVE_DIR_FORWARD=1;UNIT_MOVE_DIR_BACKWARD=2;UNIT_MOVE_DIR_LEFT=3;UNIT_MOVE_DIR_RIGHT=4
      calls=0; moves=0
      function VSAI_Gunfighter_01_Update(n) calls=calls+1; return 123 end
      function CheckObejctAlive(id) return true end
      function GetDistance(a,b) return 0 end
      function DocumentFind(id) return nil end
      npc={GetNPCID=function() return 3604001 end,GetID=function() return 20 end,
        GetHp=function() return 1000 end,CheckHurt=function() return false end,
        GetNowPos=function() return {} end,GetLuaStep=function() return 3 end,
        GetAttackTarget=function() return 0 end,GetMoveTargetFlag=function() return false end}
      _ACT_VSAI_GF01_NONE=3
    )"),"fixture Lua bindings initialize");
    HMODULE core=LoadLibraryW((g_root+L"\\sdgo\\core.dll").c_str());
    auto init=core?reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit")):nullptr;
    Check(init!=nullptr,"real core export present");
    if(!init) return 1;
    SdgoBoot boot={sizeof(boot),"battle_ai_native_test",GetModuleHandleW(nullptr)};
    Check(init(&boot)!=0,"real core initialization");
    Unit unit; unit.bytes[0x326C]=1; *reinterpret_cast<uint32_t*>(unit.bytes+0x3238)=3604001;
    if(generic||easy||medium||missing||configIndependence||projectileLead||forms) {
        native_world::Initialize(unit.bytes,g_L);
        native_world::Put(native_world::fsms[0].data(),0x200,uint32_t(2));
        native_world::enemy2[0xDCD]=0;
        native_world::shotReturn=0;
        uint32_t zeroShotResult=reinterpret_cast<native_world::Callbacks*>(native_world::fsms[2].data())->Shot(111,2,3);
        Check(zeroShotResult==0,"ranged shot hook preserves an original native zero return");
        native_world::shotReturn=0xBEEFF00D;
        uint32_t shotResult=reinterpret_cast<native_world::Callbacks*>(native_world::fsms[2].data())->Shot(111,2,3);
        Check(shotResult==0xBEEFF00D,"ranged shot hook forwards the original 32-bit returned object value");
        Check(*reinterpret_cast<uint32_t*>(native_world::fsms[2].data()+0x100)==116,"ranged shot observation forwards exact original arguments");
    }
    lua_pushnumber(g_L,987);
    unit.Init(reinterpret_cast<void*>(1)); unit.Think();
    const auto diagnosticSnapshot=
        reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(
            GetProcAddress(
                GetModuleHandleW(L"battle_ai.dll"),
                SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
    Check(lua_gettop(g_L)==1 && lua_tonumber(g_L,1)==987,"adapter preserves incoming Lua stack");
    if(forms) {
        auto movement=native_world::movements[0].data();
        auto attack=native_world::fsms[0].data();
        auto definition=*reinterpret_cast<uint8_t**>(unit.bytes+0x9C4);
        std::array<uint8_t,0x300> parked{};
        std::array<uint8_t,0x100> secondary{};
        native_world::Put(unit.bytes,0xAB0,parked.data());
        native_world::Put(movement,0x21C,unit.bytes);native_world::Put(movement,0x224,attack);
        native_world::Put(unit.bytes,0x9D4,secondary.data());
        native_world::Put(movement,0x220,secondary.data());
        native_world::Put(secondary.data(),4,secondary.data()+0x20);
        attack[0xB38]=1;unit.bytes[0xDCD]=0;unit.bytes[0x312A]=0;
        native_world::Put(definition,0x13C,uint32_t(888777));
        native_world::Put(parked.data(),0,uint32_t(888777));parked[0x140]=0;parked[0x247]=1;
        native_world::Put(parked.data(),0x13C,*reinterpret_cast<uint32_t*>(definition));
        native_world::Put(parked.data(),0x249,uint32_t(102));
        native_world::Put(attack,0x56C,int16_t(7));native_world::Put(attack,0x56C+8,int16_t(0));
        native_world::Put(attack,0x56C+0xC,native_world::weapons[1].data());
        Check(Eval("wantForm=1; SDGO_BattleAI_GenericUpdate=function(n) formSample=SDGO_BattleAI_Observe(); if wantForm==1 then formQueued=SDGO_BattleAI_RequestTransform() end; return 123 end"),
              "form bridge probe uses the real DLL observation and request exports");
        native_world::Put(unit.bytes,0x9D8,uint32_t(6));unit.Think();
        Check(Eval("assert(formSample.otherForm.allowed==0 and formQueued==0)")&&
              *reinterpret_cast<uint32_t*>(movement+0x2F0)==0&&*reinterpret_cast<uint32_t*>(movement+8)==0,
              "a just-respawned Neutral unit retains its recovery controller instead of entering transform D4");
        native_world::Put(unit.bytes,0x9D8,uint32_t(1));
        unit.Think();
        Check(Eval("assert(formSample.otherForm.unitId==888777 and formSample.otherForm.allowed==1 and formSample.otherForm.weapons[1].ammoReady==1 and formQueued==1)"),
              "another form's real bank is serialized and its original request can queue");
        Check(*reinterpret_cast<uint32_t*>(movement+0x2F0)==1&&
              *reinterpret_cast<uint8_t**>(movement+8)==movement+0xD4&&
              *reinterpret_cast<uint8_t**>(unit.bytes+0x9C4)==definition&&unit.bytes[0xE34]==0,
              "the adapter calls the original request receiver without changing either form definition or the bank flag");
        Check(Eval("wantForm=0"),"form probe releases its request");unit.Think();
        Check(Eval("assert(formSample.valid==1 and formSample.transforming==1 and formSample.weapons==nil)"),
              "a queued native transformation is observed as controller ownership rather than a broken capability sample");
        // Fixture commits a simulated native handoff. This validates adapter
        // reads only; actual native/network transformation is a separate gate.
        native_world::Put(movement,8,uint32_t(0));
        native_world::Put(unit.bytes,0x9C4,parked.data());native_world::Put(unit.bytes,0xAB0,definition);
        unit.bytes[0xE34]=1;native_world::Put(attack,0x240,int32_t(0));
        native_world::Put(attack,0x248,native_world::weapons[1].data());unit.Think();
        Check(Eval("assert(formSample.unitId==888777 and formSample.alternate==1 and formSample.selectedSlot==0 and formSample.meleeRole==0 and formSample.weapons[1].kind=='projectile')"),
              "a committed ranged-only form uses its actual weapon bank and replaces the previous combat role");
        movement[0x2F4]=1;WaitNativeTicks(510);
        Check(Eval("wantForm=1"),"form rejection probe requests through the same export");unit.Think();
        Check(Eval("assert(formQueued==0 and formSample.valid==1 and not formSample.transforming)"),
              "a refused native request is not acknowledged as a transformation");
        Check(*reinterpret_cast<uint32_t*>(movement+0x2F0)==2&&*reinterpret_cast<uint32_t*>(movement+8)==0,
              "native refusal leaves movement ownership free instead of fabricating a pending form");
        std::printf("NATIVE FORM BRIDGE CHECKS %s\n",g_failures?"FAILED":"PASSED");
        return g_failures?1:0;
    }
    if(configIndependence) {
        Check(Eval(
            "assert(SDGO_BattleAI_ConfigProbe.LogDecisions==1 and "
            "SDGO_BattleAI_ConfigProbe.TerrainRayAudit==0)"),
            "decision logging does not enable the independent terrain-ray audit");
        Config(
            "[settings]\nDifficulty=Hard\n"
            "LogDecisions=0\nTerrainRayAudit=1\n");
        Sleep(2100);
        unit.Think();
        Check(Eval(
            "assert(SDGO_BattleAI_ConfigProbe.LogDecisions==0 and "
            "SDGO_BattleAI_ConfigProbe.TerrainRayAudit==1)"),
            "terrain-ray audit can be enabled without decision logging");
        std::printf("NATIVE ADAPTER CHECKS %s\n",g_failures?"FAILED":"PASSED");
        return g_failures?1:0;
    }
    if(projectileLead) {
        Check(Eval(
            "SDGO_BattleAI_GenericUpdate=function(n) "
            "SDGO_BattleAI_Observe(); n:SetAttackTarget(2); "
            "SDGO_BattleAI_AimTarget(2); return 123 end"),
            "isolated projectile-lead fixture binds the moving target");
        WaitNativeTicks(40);
        unit.Think();
        WaitNativeTicks(40);
        native_world::Put(native_world::nodes[2].data(),0x90,140.0f);
        native_world::Put(
            native_world::scenes[2].data(),0xBC,
            native_world::Aabb{{1360,50,100},{1440,150,180}});
        unit.Think();
        native_world::Ray3 leadRay{};
        reinterpret_cast<native_world::Callbacks*>(
            native_world::fsms[0].data())->Ray(&leadRay);
        std::printf(
            "DIAG projectile lead direction=(%.6f,%.6f,%.6f)\n",
            leadRay.direction.x,leadRay.direction.y,leadRay.direction.z);
        Check(
            leadRay.direction.z>0.15f,
            "projectile ray leads a laterally moving observed target using native projectile speed");
        std::printf("NATIVE ADAPTER CHECKS %s\n",g_failures?"FAILED":"PASSED");
        return g_failures?1:0;
    }
    if(generic||medium) {
        Check(Eval(R"(
          fixtureCadenceCalls=0;fixtureCadenceMs=-1
          fixtureMeleeGateCalls=0;fixtureMeleeGateValue=-1;fixtureMeleeGateCancel=-1
          fixtureNativeCadence=SDGO_BattleAI_Cadence
          fixtureNativeMeleeGate=SDGO_BattleAI_MeleeGate
          SDGO_BattleAI_Cadence=function(ms)
            fixtureCadenceCalls=fixtureCadenceCalls+1
            fixtureCadenceMs=ms
            return fixtureNativeCadence(ms)
          end
          SDGO_BattleAI_MeleeGate=function(value,cancel)
            fixtureMeleeGateCalls=fixtureMeleeGateCalls+1
            fixtureMeleeGateValue=value
            fixtureMeleeGateCancel=cancel
            return fixtureNativeMeleeGate(value,cancel)
          end
        )"),"fixture records native cadence and melee-gate requests for failure diagnostics");
        Check(unit.bytes[0x3291]!=0,"enhanced PvP grants native automatic-jump permission without changing movement stats");
        const auto snapshot=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(
            GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        Check(snapshot!=nullptr,"the optional native behavior observer has a stable export");
        if(snapshot) {
            SdgoBattleAiUnitSnapshotV1 state{};state.size=sizeof(state);state.version=1;
            const auto stops=native_world::stopMotorCalls,off=native_world::message41Calls;
            Check(snapshot(20,&state)==1&&state.object_id==20&&state.hp==1000&&state.ai_enabled,
                  "read-only behavior snapshots identify the actual initialized native AI");
            Check(native_world::stopMotorCalls==stops&&native_world::message41Calls==off,
                  "reading behavior never sends an attack-off or motor-stop message");
            state.size=sizeof(state)-1;Check(snapshot(20,&state)==0,"short behavior observer buffers are rejected");
            state.size=sizeof(state);state.version=2;Check(snapshot(20,&state)==0,"unknown behavior observer versions are rejected");
        }
        auto fsm=native_world::fsms[0].data();
        native_world::Put(fsm,0x240,int32_t(0));native_world::Put(fsm,0x248,native_world::weapons[0].data());
        native_world::Put(unit.bytes,0xDD0,int32_t(1));
        int prior=native_world::switchRequests;
        Eval("npc:ChangeWeapon(1)");
        Check(native_world::switchRequests==prior&&*reinterpret_cast<int*>(fsm+0x240)==0,
              "stock NPC hint shortcut can suppress a switch that never actually committed");
        Eval("savedWeaponProbe=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) weaponProbeResult=SDGO_BattleAI_RequestWeapon(1); return 123 end");
        const auto selections=native_world::nativeSelections;
        native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,uint32_t(0));
        unit.Think();
        Check(native_world::switchRequests==prior+1&&native_world::nativeSelections==selections+1&&native_world::requestedSlot==1&&*reinterpret_cast<int*>(fsm+0x240)==1,
              "a free controller uses the original non-forced selection transaction rather than mistaking an NPC hint for binding");
        native_world::deferSwitch=true;
        native_world::Put(fsm,0x240,int32_t(0));native_world::Put(fsm,0x248,native_world::weapons[0].data());
        unit.Think();
        Check(*reinterpret_cast<int*>(fsm+0x240)==0,
              "queued switch never fabricates acknowledgement when the native receiver defers it");
        native_world::Put(fsm,4,fsm+0x34);const auto activeSelections=native_world::nativeSelections;
        unit.Think();
        Check(native_world::nativeSelections==activeSelections&&native_world::switchRequests==prior+3&&
              *reinterpret_cast<int*>(fsm+0x240)==0,
              "an active native action keeps only a pending hint and cannot be interrupted by a forced selection");
        native_world::Put(fsm,4,fsm+0x20);
        Check(Eval("assert(SDGO_BattleAI_RequestWeapon(1)==0)"),"weapon bridge rejects calls outside the current owned AI callback");
        native_world::deferSwitch=false;
        native_world::Put(fsm,0x240,int32_t(1));native_world::Put(fsm,0x248,native_world::weapons[1].data());
        Eval("SDGO_BattleAI_GenericUpdate=savedWeaponProbe");
        Eval("savedReleaseProbe=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) releaseProbe=SDGO_BattleAI_ReleaseHeldIntent(); return 123 end");
        native_world::Put(fsm,4,fsm+0x98);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(Eval("assert(releaseProbe==1)")&&unit.bytes[0x312A]==0&&
              *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0&&
              *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x98&&
              *reinterpret_cast<uint8_t**>(fsm+8)==nullptr,
              "held-input bridge releases an active special without replacing its native FSM state");
        native_world::Put(fsm,4,fsm+0x34);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(Eval("assert(releaseProbe==1)")&&unit.bytes[0x312A]==0&&
              *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0&&
              *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x34&&
              *reinterpret_cast<uint8_t**>(fsm+8)==nullptr,
              "held-input bridge stops ranged continuation without queueing Neutral");
        native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,fsm+0x98);
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(Eval("assert(releaseProbe==1)")&&unit.bytes[0x312A]==0&&
              *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0&&
              *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x20&&
              *reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x98,
              "held-input bridge preserves a pending special admission");
        native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,fsm+0x34);
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(Eval("assert(releaseProbe==1)")&&unit.bytes[0x312A]==0&&
              *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0&&
              *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x20&&
              *reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x34,
              "held-input bridge preserves a pending ranged admission");
        native_world::weapons[3][0x69]=2;
        native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
        native_world::Put(fsm,0x248,native_world::weapons[3].data());
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(Eval("assert(releaseProbe==1)")&&unit.bytes[0x312A]==0&&
              *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0&&
              *reinterpret_cast<uint8_t**>(fsm+8)==nullptr,
              "held-input bridge can cancel an unadmitted selected special without a state transition");
        native_world::Put(fsm,0x248,native_world::weapons[1].data());
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(Eval("assert(releaseProbe==0)")&&unit.bytes[0x312A]==1&&
              (medium||
               *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0x101)&&
              *reinterpret_cast<uint8_t**>(fsm+8)==nullptr,
              "held-input bridge refuses an ordinary neutral action it does not own while Medium pacing remains independent");
        Check(Eval("assert(SDGO_BattleAI_ReleaseHeldIntent()==0)"),
              "held-input bridge rejects calls outside the current owned AI callback");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) n:SetAttackOnOff(false); return 123 end");
        native_world::Put(fsm,4,fsm+0x98);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
        Check(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x20,
              "fixture models native SetAttackOnOff(false) queueing Neutral during an active special");
        native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
        native_world::Put(fsm,0x240,int32_t(1));native_world::Put(fsm,0x248,native_world::weapons[1].data());
        unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
        Eval("SDGO_BattleAI_GenericUpdate=savedReleaseProbe");
    }
    if(generic||medium) {
        Eval("motionProbe=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe(); n:SetMoveTargetPos_Dir(2,400,2,false); n:SetMoveType(1); n:SetAttackOnOff(true); return 123 end");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,1000});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,-1});
        native_world::Put(unit.bytes,0x3078,native_world::Vec3{0,0,-400});
        unit.Think();
        const auto protectedFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(protectedFacing.z>0.99f&&unit.bytes[0x30DC]!=0&&reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078)->z==-400,
              "Medium/Hard backward movement retains its waypoint and front facing despite a reversed path yaw cache");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(1));
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{0,0,-3000});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        unit.Think();
        Check(reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f,
              "a far idle opponent cannot force side-on defence during an existing front engagement");
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{0,0,-1000});
        native_world::Put(native_world::fsms[1].data(),0x200,uint32_t(20));
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        lua_pushcfunction(g_L,QueueFixtureSwing);lua_setglobal(g_L,"FixtureQueueSwing");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe(); n:SetAttackTarget(2); FixtureQueueSwing(); return 123 end");
        const int beforeMeleeTurn=native_world::rotations;unit.Think();
        Check(native_world::rotations==beforeMeleeTurn&&reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f,
              "a melee action queued after observation retains native target-facing instead of defence turning its swing away");
        native_world::Put(native_world::fsms[0].data(),8,uint32_t(0));
        native_world::Put(native_world::fsms[0].data(),0x240,int32_t(1));
        native_world::Put(native_world::fsms[0].data(),0x248,native_world::weapons[1].data());
        native_world::Put(native_world::fsms[1].data(),0x200,uint32_t(0));
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{1000,0,0});
        Eval("SDGO_BattleAI_GenericUpdate=motionProbe");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{1400,0,100});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        unit.bytes[0x30DC]=0;
    }
    if(easy||medium) {
        if(medium) {
            auto pacedFsm=native_world::fsms[0].data();
            native_world::acceptOrdinaryAdmission=true;
            reinterpret_cast<native_world::Callbacks*>(pacedFsm)->AttackAdmission();
            native_world::acceptOrdinaryAdmission=false;
            native_world::Put(pacedFsm,4,pacedFsm+0x20);native_world::Put(pacedFsm,8,uint32_t(0));
        }
        native_world::Put(unit.bytes,0x31F4,77);unit.Think();
        if(easy) {
            Check(Eval("assert(calls==2 and SDGO_BattleAI_Universal.states[20]==nil and SDGO_BattleAI.states[20]==nil)"),
                  "Easy keeps original decisions without generic or legacy tactics");
            Check(native_world::lastWait==77 && native_world::jumps==0 && native_world::moves==0,
                  "Easy leaves native cadence and locomotion unchanged");
        } else {
            Check(Eval("assert(calls==1 and SDGO_BattleAI_Universal.states[20].target==2 and SDGO_BattleAI.states[20]==nil)"),
                  "Medium uses continuous generic decisions and never reruns stock idle");
            lua_getglobal(g_L,"fixtureCadenceCalls");
            const int cadenceCalls=static_cast<int>(lua_tonumber(g_L,-1));
            lua_pop(g_L,1);
            lua_getglobal(g_L,"fixtureCadenceMs");
            const double cadenceMs=lua_tonumber(g_L,-1);
            lua_pop(g_L,1);
            const bool pacedWindow=native_world::lastWait==0&&
                                   unit.bytes[0xDCD]==0&&
                                   native_world::positiveJumps==0&&
                                   cadenceCalls>0;
            if(!pacedWindow) {
                SdgoBattleAiUnitSnapshotV1 state{};
                state.size=sizeof(state);state.version=1;
                const int haveSnapshot=
                    diagnosticSnapshot
                        ?diagnosticSnapshot(20,&state):0;
                std::printf(
                    "DIAG medium-ready lastWait=%d dcd=%u jumps=%d "
                    "positiveJumps=%d lastJumpSeconds=%.3f "
                    "cadenceCalls=%d cadenceMs=%.0f ownsIntentInferred=%u "
                    "nextPulseExported=0 now=%llu "
                    "snapshot=%d attackState=%u attackPending=%u\n",
                    native_world::lastWait,
                    static_cast<unsigned>(unit.bytes[0xDCD]),
                    native_world::jumps,native_world::positiveJumps,
                    native_world::lastJumpSeconds,
                    cadenceCalls,cadenceMs,
                    cadenceCalls>0?1u:0u,
                    static_cast<unsigned long long>(GetTickCount64()),
                    haveSnapshot,state.attack_state,
                    state.attack_pending);
            }
            Check(pacedWindow,
                  "Medium clears native NPC idle wait and paces an admitted ordinary attack without Hard air tactics");
        }
        native_world::Ray3 profileRay;
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
        if(easy) {
            Check(profileRay.direction.x==0 && profileRay.direction.z==1 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "Easy leaves original aiming ray and NPC error untouched");
        } else {
            Check(profileRay.direction.x>0.9f && *reinterpret_cast<float*>(unit.bytes+0x3204)==0,
                  "Medium corrects native shot aim with continuous decisions");
            using InitializeFn=void(__cdecl*)(uint8_t*,int,uint32_t);
            auto initializeUnit=reinterpret_cast<InitializeFn>(GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),"SdgoBattleAiInitializeUnit"));
            Check(initializeUnit!=nullptr,"optional match_ai native loadout endpoint is exported by its stable name");
            if(initializeUnit) {
                native_world::Put(unit.bytes,0xBC8,int32_t(18000));initializeUnit(unit.bytes,0x101,1);
                Check(*reinterpret_cast<int*>(unit.bytes+0xB0C)==12&&native_world::lastPart==0&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,
                      "Medium first spawn applies native level12/no part and clears pooled SP to the authoritative match start");
                int priorParts=native_world::partApplications;initializeUnit(unit.bytes,1,1);
                Check(native_world::partApplications==priorParts,"duplicate spawn/deck generation never reconstructs a client part");
                native_world::Put(unit.bytes,0xBC8,int32_t(12000));initializeUnit(unit.bytes,0x101,1);
                Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==12000,"duplicate initial-spawn delivery cannot erase earned SP");
                initializeUnit(unit.bytes,2,2);
                Check(*reinterpret_cast<int*>(unit.bytes+0xB0C)==13&&native_world::lastPart==75002,
                      "Hard applies native EX13 and a full-width legal mobility part for a mixed ranged loadout");
                Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==12000,"respawn/deck generation preserves the match SP resource");
                native_world::Put(unit.bytes,0xBC8,int32_t(0));
                native_world::Put(unit.bytes,0xB0C,int32_t(1));priorParts=native_world::partApplications;initializeUnit(unit.bytes,0,3);
                Check(*reinterpret_cast<int*>(unit.bytes+0xB0C)==1&&native_world::partApplications==priorParts,
                      "Easy spawn keeps the native level and optional part unchanged");
                uint8_t range1=native_world::weapons[1][0x6A],range2=native_world::weapons[2][0x6A];
                native_world::weapons[1][0x6A]=native_world::weapons[2][0x6A]=1;initializeUnit(unit.bytes,2,4);
                Check(native_world::lastPart==75003,"native melee-only weapon properties select the full-width fighting part on the client");
                native_world::weapons[1][0x6A]=range1;native_world::weapons[2][0x6A]=range2;
            }
            auto freshDefenceLifecycle=[&](uintptr_t spawn) {
                auto attackFsm=native_world::fsms[0].data();
                auto movementFsm=native_world::movements[0].data();
                const auto position=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x88);
                native_world::Put(attackFsm,4,attackFsm+0x20);native_world::Put(attackFsm,8,uint32_t(0));
                native_world::Put(movementFsm,4,movementFsm+0x20);native_world::Put(movementFsm,8,uint32_t(0));
                native_world::Put(unit.bytes,0x3078,position);
                unit.bytes[0x30DC]=0;unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
                unit.Init(reinterpret_cast<void*>(spawn));
            };
            native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
            native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
            native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,-500});
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
            native_world::Put(unit.bytes,0x3090,native_world::Vec3{-1,0,0});
            freshDefenceLifecycle(6);
            Sleep(90);unit.Think();
            auto facing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
            Check(facing.z>0.95f&&std::abs(facing.x)>0.01f,"Medium rear defence requests a bounded native scene turn even when old desired cache points elsewhere");
            for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
            facing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
            Check(facing.z<-0.98f,"Medium quickly faces an enemy attacking from behind over visible native frames");
            native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
            native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(1));
            native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{0,0,1000});
            native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,-1000});
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
            freshDefenceLifecycle(7);
            int beforeDefenceMoves=native_world::moves;Sleep(90);unit.Think();
            Check(native_world::moves>beforeDefenceMoves&&std::abs(reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078)->x)>1,
                  "Medium proposes a native lateral route to reduce front/rear crossfire");
            native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{1000,0,0});
            native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{1400,0,100});
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});Sleep(90);unit.Think();
            auto fsm=native_world::fsms[0].data();auto special=native_world::weapons[3].data();
            memcpy(special,native_world::weapons[1].data(),native_world::weapons[3].size());
            native_world::Put(special,0,uint32_t(104));special[0x69]=2;
            native_world::Put(native_world::definitions[0].data(),0x249+3*4,uint32_t(104));
            native_world::Put(fsm,0x24C+3*0xC8,int16_t(1));native_world::Put(fsm,0x258+3*0xC8,special);
            using RequestFn=void(__thiscall*)(uint8_t*);
            RequestFn volatile nativeRequest=reinterpret_cast<RequestFn>(native_world::Address(&native_world::Callbacks::AttackAdmission));
            auto requestSpecial=[&] {
                native_world::Put(fsm,0x240,int32_t(3));native_world::Put(fsm,0x248,special);
                nativeRequest(fsm);
            };
            int beforeRequests=native_world::attackAdmissions;
            nativeRequest(fsm);unit.Think();
            Check(native_world::attackAdmissions==beforeRequests+1&&*reinterpret_cast<int*>(fsm+0x240)==1,
                  "shared attack admission forwards ordinary shots even without SP and Medium does not select special");
            ++beforeRequests;
            native_world::Put(fsm,0x248,special);requestSpecial();
            Check(native_world::attackAdmissions==beforeRequests,"final attack admission rejects the special branch without player SP");
            native_world::Put(fsm,0x240,int32_t(1));
            native_world::Put(fsm,0x248,native_world::weapons[1].data());
            auto pressureMotor=native_world::movements[0].data();
            native_world::Put(pressureMotor,4,pressureMotor+0xE8);
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));unit.Think();
            Check(*reinterpret_cast<int*>(fsm+0x240)==3&&unit.bytes[0x312A]==0,
                  "Medium uses full SP for a player-permitted hurt counter through the native weapon-change API");
            unit.Think();Check(unit.bytes[0x312A]!=0,"Medium waits for special selection commit then requests native attack");
            native_world::Put(pressureMotor,4,pressureMotor+0x20);
            requestSpecial();Check(native_world::attackAdmissions==beforeRequests+1,"legal native special request forwards to original authorization path");
            native_world::Put(unit.bytes,0x2B68,uint32_t(0x80000014));
            native_world::Put(native_world::enemy1.data(),0x2B68,uint32_t(0x80000001));
            uint8_t accepted[0x18]={};
            native_world::Put(accepted,6,uint32_t(0x80000014));native_world::Put(accepted,0xA,uint32_t(104));native_world::Put(accepted,0x12,int32_t(20000));
            using NoticeFn=int(__thiscall*)(uint8_t*,const uint8_t*);
            NoticeFn volatile notify=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::SpecialNotice));
            const auto missionBossSpecial=
                reinterpret_cast<SdgoBattleAiGetMissionBossSpecialV1Fn>(
                    GetProcAddress(
                        GetModuleHandleW(L"battle_ai.dll"),
                        SDGO_BATTLE_AI_MISSION_BOSS_SPECIAL_EXPORT));
            Check(
                missionBossSpecial!=nullptr,
                "the passive mission Boss special observer has a stable export");
            SdgoBattleAiMissionBossSpecialV1 bossBefore{};
            bossBefore.size=sizeof(bossBefore);bossBefore.version=1;
            Check(
                missionBossSpecial && missionBossSpecial(&bossBefore)==1,
                "the passive mission Boss observer exposes a readable empty baseline");
            if(missionBossSpecial) {
                auto invalid=bossBefore;
                invalid.size=sizeof(invalid)-1;
                Check(
                    missionBossSpecial(&invalid)==0,
                    "the mission Boss observer rejects short ABI buffers");
                invalid=bossBefore;invalid.version=2;
                Check(
                    missionBossSpecial(&invalid)==0,
                    "the mission Boss observer rejects unknown ABI versions");
                Check(
                    missionBossSpecial(
                        reinterpret_cast<SdgoBattleAiMissionBossSpecialV1*>(1))==0,
                    "the mission Boss observer rejects an unreadable caller buffer");
            }
            const auto savedMissionNpc=
                *reinterpret_cast<uint32_t*>(unit.bytes+0x3238);
            const auto savedMissionHp=
                *reinterpret_cast<int32_t*>(unit.bytes+0x38);
            const auto savedMissionMaxHp=
                *reinterpret_cast<int32_t*>(unit.bytes+0x3C);
            const auto savedMissionAppState=
                *reinterpret_cast<uint32_t*>(native_world::app.data()+0x4A0);
            native_world::Put(unit.bytes,0x3238,uint32_t(85054));
            native_world::Put(unit.bytes,0x38,int32_t(700));
            native_world::Put(unit.bytes,0x3C,int32_t(1000));
            native_world::Put(native_world::app.data(),0x4A0,uint32_t(22));
            int noticeReturn=notify(native_world::document.data(),accepted);
            SdgoBattleAiMissionBossSpecialV1 bossAfter{};
            bossAfter.size=sizeof(bossAfter);bossAfter.version=1;
            Check(
                missionBossSpecial && missionBossSpecial(&bossAfter)==1 &&
                bossAfter.sequence>bossBefore.sequence &&
                bossAfter.battle_epoch &&
                bossAfter.event_tick_ms &&
                bossAfter.mission_object==
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                        native_world::document.data())) &&
                bossAfter.unit==
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                        unit.bytes)) &&
                bossAfter.pilot_id==0x80000014 &&
                bossAfter.object_id==20 &&
                bossAfter.npc_data_id==85054 &&
                bossAfter.weapon_id==104 &&
                bossAfter.native_app_state==22 &&
                bossAfter.hp_before==700 &&
                bossAfter.max_hp_before==1000 &&
                bossAfter.hp_after==700 &&
                bossAfter.max_hp_after==1000 &&
                bossAfter.state_before_valid &&
                bossAfter.state_after_valid &&
                bossAfter.original_result==0x51A7 &&
                bossAfter.original_returned,
                "a genuine original special notice publishes the exact live M1 Boss identity, 70-percent HP and before/after state");
            native_world::Put(unit.bytes,0x3238,savedMissionNpc);
            native_world::Put(unit.bytes,0x38,savedMissionHp);
            native_world::Put(unit.bytes,0x3C,savedMissionMaxHp);
            native_world::Put(native_world::app.data(),0x4A0,savedMissionAppState);
            unit.Think();
            Check(noticeReturn==0x51A7&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==20000,
                  "matched server special acceptance preserves native return and applies authoritative SP through native command18");
            Eval("savedAcceptedSpecial=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) staleAckRelease=SDGO_BattleAI_ReleaseHeldIntent(); return 123 end");
            native_world::Put(fsm,4,fsm+0x5C);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
            native_world::Put(fsm,0x240,int32_t(1));native_world::Put(fsm,0x248,native_world::weapons[1].data());
            unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));unit.Think();
            Check(Eval("assert(staleAckRelease==0)")&&unit.bytes[0x312A]==1&&
                  *reinterpret_cast<uint16_t*>(unit.bytes+0xDCD)==0x101&&
                  *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x5C&&
                  *reinterpret_cast<uint8_t**>(fsm+8)==nullptr,
                  "a historical special acknowledgement cannot release a later ordinary melee combo");
            Eval("SDGO_BattleAI_GenericUpdate=savedAcceptedSpecial");
            native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
            native_world::Put(unit.bytes,0xBC8,int32_t(15000));notify(native_world::document.data(),accepted);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==15000,"duplicate special notice without a pending request cannot refund NPC SP");
            uint8_t terminal[0x28]={};native_world::Put(terminal,0,uint16_t(606));
            native_world::Put(terminal,6,uint32_t(0x80000014));native_world::Put(terminal,0xE,int32_t(0));
            native_world::Put(terminal,0x12,uint32_t(0x80000001));
            NoticeFn volatile meleeNotice=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::MeleeNotice));
            int terminalResult=meleeNotice(native_world::document.data(),terminal);unit.Think();
            Check(terminalResult==0x51B1&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"actual melee completion606 applies authoritative remaining SP through native command18");
            Check(*reinterpret_cast<int*>(fsm+0x240)==1,
                  "authoritative completion immediately restores the ordinary weapon without waiting for a missed busy-state sample");
            native_world::Put(terminal,0xE,int32_t(20000));meleeNotice(native_world::document.data(),terminal);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"duplicate terminal notification cannot refund a finished special");
            auto completionResult=native_world::specialResults[0].data();
            uint32_t completionAction=0x7000;
            auto stageCompletion=[&](uint32_t damage) {
                native_world::Put(unit.bytes,0xBC8,int32_t(30000));requestSpecial();
                notify(native_world::document.data(),accepted);unit.Think();
                native_world::Put(fsm,4,fsm+0xAC);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
                native_world::Put(fsm,0x224,++completionAction);fsm[0x9B3]=1;fsm[0x9B5]=0;
                native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
                native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(1));
                native_world::Put(native_world::enemy1.data(),0x9D8,uint32_t(4));
                native_world::Put(native_world::enemy1.data(),0xEB8,uint32_t(20));
                memset(completionResult,0,0x24);
                native_world::Put(completionResult,0x10,damage);
                native_world::Put(completionResult,0x14,uint32_t(1));
                native_world::Put(completionResult,0x18,int32_t(1000));
                native_world::Put(completionResult,0x1C,int32_t(0));
                native_world::Put(terminal,0xE,int32_t(0));
                native_world::Put(terminal,0x24,damage);
                meleeNotice(native_world::document.data(),terminal);unit.Think();
            };
            native_world::specialCompletionCalls=0;
            native_world::Put(fsm,4,fsm+0xAC);native_world::Put(fsm,0x224,++completionAction);
            fsm[0x9B3]=1;fsm[0x9B5]=0;memset(completionResult,0,0x24);
            native_world::Put(completionResult,0x14,uint32_t(1));
            native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
            native_world::Put(native_world::enemy1.data(),0x9D8,uint32_t(4));
            native_world::Put(native_world::enemy1.data(),0xEB8,uint32_t(20));
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0&&native_world::specialCompletionCalls==0,
                  "zero damage without a matched terminal receipt retains the native JBE exit");
            stageCompletion(0);
            const unsigned priorMxcsr=_mm_getcsr(),expectedMxcsr=(priorMxcsr&~0x6000u)|0x4000u;
            _mm_setcsr(expectedMxcsr);
            double fpValue=123.5,fpRestored=0;
            __asm { fld qword ptr [fpValue] }
            int repaired=native_world::InvokeSpecialCompletionGate(fsm);
            __asm { fstp qword ptr [fpRestored] }
            Check(repaired==1&&native_world::specialCompletionCalls==1&&native_world::specialCompletionDamage==0&&
                  fsm[0x9B5]==1&&*reinterpret_cast<uint32_t*>(native_world::enemy1.data()+0x9D8)==2&&
                  *reinterpret_cast<uint32_t*>(native_world::enemy1.data()+0xEB8)==0,
                  "matched zero-damage 606 crosses the native JBE and 9B5 branch boundary into fixture completion");
            Check(native_world::specialCompletionOwner==reinterpret_cast<uintptr_t>(unit.bytes)&&
                  native_world::specialCompletionResult==reinterpret_cast<uintptr_t>(completionResult)&&
                  native_world::specialCompletionFsm==reinterpret_cast<uintptr_t>(fsm)&&
                  native_world::specialCompletionEdx==0x12345678&&native_world::specialCompletionEdi==0x23456789&&
                  native_world::specialCompletionEbp==0x3456789A&&fpRestored==fpValue&&_mm_getcsr()==expectedMxcsr,
                  "completion branch HookSite preserves live registers plus x87 and SIMD state");
            _mm_setcsr(priorMxcsr);
            fsm[0x9B5]=0;native_world::Put(native_world::enemy1.data(),0x9D8,uint32_t(4));
            native_world::Put(native_world::enemy1.data(),0xEB8,uint32_t(20));
            native_world::Put(completionResult,0x10,uint32_t(0));native_world::Put(completionResult,0x14,uint32_t(1));
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0&&native_world::specialCompletionCalls==1,
                  "the matched zero-damage receipt is consumed once");
            stageCompletion(0);native_world::Put(native_world::enemy1.data(),0xEB8,uint32_t(999));
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0,
                  "a zero-damage receipt cannot release a victim no longer captured by that caster");
            stageCompletion(0);native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
            native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0,
                  "a zero-damage receipt cannot run completion for a dead victim");
            stageCompletion(0);native_world::Put(fsm,0x224,++completionAction);
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0,
                  "a terminal receipt cannot outlive its bound special action");
            stageCompletion(0);native_world::Put(completionResult,0x14,uint32_t(2));
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0,
                  "a terminal receipt cannot release an unmatched result victim");
            stageCompletion(0);
            auto replacementResult=native_world::specialResults[1].data();
            memset(replacementResult,0,0x24);
            native_world::Put(replacementResult,0x14,uint32_t(1));
            native_world::Put(fsm,0x9BC,replacementResult);
            const int beforeReplacement=native_world::specialCompletionCalls;
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0&&
                  native_world::specialCompletionCalls==beforeReplacement,
                  "a terminal receipt cannot follow a replacement result buffer");
            native_world::Put(fsm,0x9BC,completionResult);
            stageCompletion(5);
            const int beforePositive=native_world::specialCompletionCalls;
            Check(native_world::InvokeSpecialCompletionGate(fsm)==1&&
                  native_world::specialCompletionCalls==beforePositive+1&&native_world::specialCompletionDamage==5,
                  "positive damage retains the native branch boundary into fixture completion without a repair receipt");
            native_world::Put(completionResult,0x10,uint32_t(5));fsm[0x9B5]=1;
            Check(native_world::InvokeSpecialCompletionGate(fsm)==0&&native_world::specialCompletionCalls==beforePositive+1,
                  "the native 9B5 applied guard still rejects an already completed result");
            native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,static_cast<uint8_t*>(nullptr));
            fsm[0x9B3]=0;fsm[0x9B5]=0;memset(completionResult,0,0x24);
            native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
            native_world::Put(native_world::enemy1.data(),0x9D8,uint32_t(1));
            native_world::Put(native_world::enemy1.data(),0xEB8,uint32_t(0));
            const int gatedRequests=native_world::attackAdmissions;
            unit.bytes[0x31C4]=1;requestSpecial();unit.bytes[0x31C4]=0;
            Check(native_world::attackAdmissions==gatedRequests,"native special gate rejects explicit unit restriction");
            native_world::Put(native_world::movements[0].data(),0x2C0,int32_t(1));requestSpecial();
            Check(native_world::attackAdmissions==gatedRequests,"native special gate rejects controller override");
            native_world::Put(native_world::movements[0].data(),0x2C0,int32_t(0));
            native_world::Put(unit.bytes,0xBC8,int32_t(0));requestSpecial();
            Check(native_world::attackAdmissions==gatedRequests,"final native special gate enforces SP, restriction and controller permissions despite NPC overrides");
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));requestSpecial();notify(native_world::document.data(),accepted);unit.Think();
            NoticeFn volatile rangedNotice=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::RangedNotice));
            native_world::Put(terminal,0xA,int32_t(0));
            int offThreadResult=0;
            std::thread noticeWorker([&]{offThreadResult=rangedNotice(native_world::document.data(),terminal);});noticeWorker.join();
            Check(offThreadResult==0x51B2&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==20000,"off-thread ranged completion forwards original and queues SP without touching game-thread actor state");
            unit.Think();Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"game thread drains ranged completion789 authoritative SP before the next decision");
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));requestSpecial();notify(native_world::document.data(),accepted);unit.Think();
            NoticeFn volatile spState=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::SpStateNotice));
            uint8_t state[0xF0]={};native_world::Put(state,0,uint16_t(0x779));native_world::Put(state,6,uint32_t(0x80000014));
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));native_world::Put(state,0xF,uint32_t(104));
            int priorDamageNotices=native_world::ordinaryNotices;int oldHp=*reinterpret_cast<int*>(unit.bytes+0x38);
            int stateResult=spState(native_world::document.data(),state);unit.Think();
            Check(stateResult==1&&native_world::ordinaryNotices==priorDamageNotices&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==0&&*reinterpret_cast<int*>(unit.bytes+0x38)==oldHp,
                  "fixed-size zero-victim SP state updates use native command18 without damage, hurt, effects or attack processing");
            state[0xA]=0;native_world::Put(state,0xB,int32_t(1000));int ordinaryResult=spState(native_world::document.data(),state);
            Check(ordinaryResult==0x51D0&&native_world::ordinaryNotices==priorDamageNotices+1,
                  "ordinary0779 notification preserves its original return and processing");
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));
            std::thread stateWorker([&]{spState(native_world::document.data(),state);});stateWorker.join();
            state[0xA]=0;native_world::Put(state,0xB,int32_t(4000));
            std::thread gainWorker([&]{spState(native_world::document.data(),state);});gainWorker.join();
            unit.Think();Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==4000,
                  "ordered off-thread state and ordinary damage snapshots cannot overwrite a newer earned SP gain");
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));
            std::thread oldState([&]{spState(native_world::document.data(),state);});oldState.join();
            state[0xA]=0;native_world::Put(state,0xB,int32_t(5000));spState(native_world::document.data(),state);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==5000,"older worker state cannot overwrite a newer main-thread ordinary SP gain");
            native_world::Put(state,0xB,int32_t(6000));
            std::thread oldGain([&]{spState(native_world::document.data(),state);});oldGain.join();
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));spState(native_world::document.data(),state);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"older worker gain cannot refund a newer main-thread terminal SP state");
            uint8_t lateOrdinary[0xF0]={};memcpy(lateOrdinary,state,sizeof(state));lateOrdinary[0xA]=0;native_world::Put(lateOrdinary,0xB,int32_t(7000));
            native_world::ordinaryEntered=false;native_world::releaseOrdinary=false;native_world::stallOrdinary=true;
            std::thread lateGain([&]{spState(native_world::document.data(),lateOrdinary);});
            for(int i=0;i<1000&&!native_world::ordinaryEntered;++i) Sleep(1);
            Check(native_world::ordinaryEntered,"controlled old ordinary handler has entered before the newer state");
            spState(native_world::document.data(),state);unit.Think();
            native_world::releaseOrdinary=true;lateGain.join();native_world::stallOrdinary=false;
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==7000,"controlled legacy worker finishes its old native SP write after the newer state");
            unit.Think();Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,
                  "late old native writes are corrected to the last authoritative sequence before AI decisions");
            std::thread oldBeforeSkill([&]{spState(native_world::document.data(),state);});oldBeforeSkill.join();
            uint8_t skillPacket[40]={};native_world::Put(skillPacket,0,uint16_t(0x6AA));native_world::Put(skillPacket,6,uint32_t(0x80000014));native_world::Put(skillPacket,0x1F,int32_t(9000));
            NoticeFn volatile skillNotice=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::SkillNotice));
            int skillResult=skillNotice(native_world::document.data(),skillPacket);unit.Think();
            Check(skillResult==0x51D1&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==9000,
                  "native skill-state SP snapshots share the same sequence and cannot be rolled back by an older queued clear");
            native_world::Put(pressureMotor,4,pressureMotor+0xE8);
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));unit.Think();unit.Think();
            native_world::Put(pressureMotor,4,pressureMotor+0x20);
            native_world::deferSwitch=true;
            Config("[settings]\nDifficulty=Easy\n");Sleep(2100);unit.Think();
            Check(native_world::requestedSlot==1&&*reinterpret_cast<int*>(fsm+0x240)==3,
                  "profile disable retains a pending restore while the native weapon switch is deferred");
            native_world::deferSwitch=false;int priorSwitches=native_world::switchRequests;
            const int priorSelections=native_world::nativeSelections;unit.Think();
            Check(native_world::nativeSelections==priorSelections+1&&*reinterpret_cast<int*>(fsm+0x240)==1,
                  "Easy handoff retries the previous weapon until the native selection commits");
            priorSwitches=native_world::switchRequests;unit.Think();
            Check(native_world::switchRequests==priorSwitches,"confirmed ordinary slot releases pending restore without repeated commands");
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x==0 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "Medium to Easy reload restores original aim behavior");
            native_world::Put(unit.bytes,0xBC8,int32_t(0));
            Config("[settings]\n");native_world::SetRoomTitle(L"!ai3 hard");Sleep(2100);unit.Think();
            Check(Eval("assert(SDGO_BattleAI_Universal.difficulty==2 and SDGO_BattleAI_Universal.states[20].target==2)"),
                  "heap room title selects Hard through the real app/service/mode/room pointer chain");
            Config("[settings]\nDifficulty=Hard\n");native_world::SetRoomTitle(L"!ai easy");Sleep(2100);unit.Think();
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x==0&&profileRay.direction.z==1&&*reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "room difficulty overrides local config and hands Easy back to original aim");
            Config("[settings]\n");native_world::SetRoomTitle(L"!ai3",true);Sleep(2100);unit.Think();
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x>0.9f&&Eval("assert(SDGO_BattleAI_Universal.difficulty==1 and SDGO_BattleAI_Universal.states[20])"),
                  "unmarked inline room title returns to Medium with no manual client settings");
            native_world::SetRoomTitle(L"!ai hard");native_world::Put(native_world::room.data(),0x10,uint32_t(21));Sleep(2100);unit.Think();
            Check(Eval("assert(SDGO_BattleAI_Universal.difficulty==1 and SDGO_BattleAI_Universal.states[20])"),"malformed room title fails closed to the default profile");
        }
    } else if(generic) {
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2 and calls==1)"),"generic dispatch chooses real observed lower HP and initializes stock once");
        unit.Think();
        Check(unit.bytes[0xDCD]!=0,"native attack-intent hook has no additional interval between ready pulses");
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[2].data())->Shot(111,2,3);
        Sleep(130);unit.Think();
        Check(native_world::positiveJumps==1 && native_world::moves>0,"brief ranged shot event enables native air strafe without gauge refill");
        Check(unit.bytes[0xDCD]!=0 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0,"cadence resumes while NPC aiming error is removed");
        native_world::Put(native_world::fsms[0].data(),4,native_world::fsms[0].data()+0x34);
        native_world::Put(unit.bytes,0x31F4,77);unit.Think();
        Check(unit.bytes[0xDCD]!=0 && native_world::lastWait==77,"active charge/beam state retains native held input and extra timer");
        native_world::Put(native_world::fsms[0].data(),4,native_world::fsms[0].data()+0x20);
        native_world::Ray3 ray;
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&ray);
        Check(ray.origin.x==12 && ray.origin.y==100 && ray.origin.z==13 && ray.direction.x>0.9f,
              "native ranged-ray hook keeps original origin and corrects direction");
        native_world::Put(native_world::enemy1.data(),0x38,50);
        Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==1 and calls==1)"),"new lower HP is selected without rerunning stock initialization");
        native_world::Put(native_world::enemy1.data(),0x38,1000);
        native_world::blocked=true;Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==1)"),"fresh native collision query excludes lower HP behind an obstacle");
        native_world::blocked=false;Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),"next fresh query sees newly clear target without a two-second shared cache");
        native_world::Put(native_world::fsms[0].data(),0x200,uint32_t(0));unit.Think();
        Check(*reinterpret_cast<uint32_t*>(native_world::fsms[0].data()+0x200)==2,
              "a free native target cleared by a weapon handoff is rebound while the policy keeps its opponent");
        Eval("geometrySavedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) geometryObservation=SDGO_BattleAI_Observe(); return 123 end");
        unit.Think();
        Check(Eval("assert(geometryObservation.attackNeutral==1)"),
              "native observation reports a current and pending neutral attack FSM");
        auto geometryFsm=native_world::fsms[0].data();
        native_world::Put(geometryFsm,8,geometryFsm+0x98);unit.Think();
        Check(Eval("assert(geometryObservation.attackNeutral==0 and geometryObservation.specialBusy==1)"),
              "a pending native special state keeps profile ownership out of the neutral gate");
        native_world::Put(geometryFsm,8,uint32_t(0));
        auto sphereShape=native_world::targetShapes[1].data();
        auto obbShape=native_world::targetShapes[2].data();
        native_world::unitKinds[1]=7;native_world::unitKinds[2]=0x12;
        native_world::Put(native_world::nodes[1].data(),0xE0,sphereShape);
        native_world::Put(native_world::nodes[2].data(),0xD8,obbShape);
        native_world::Put(sphereShape,0x10,native_world::Vec3{321,75,-40});
        native_world::Put(obbShape,0x3C,native_world::Vec3{654,80,70});
        unit.Think();
        Check(Eval("local a,b; for _,t in ipairs(geometryObservation.targets) do if t.id==1 then a=t elseif t.id==2 then b=t end end; assert(a and b and a.pos.x>320.99 and a.pos.x<321.01 and a.pos.y>74.99 and a.pos.y<75.01 and a.pos.z>-40.01 and a.pos.z<-39.99 and b.pos.x>653.99 and b.pos.x<654.01 and b.pos.y>79.99 and b.pos.y<80.01 and b.pos.z>69.99 and b.pos.z<70.01 and a.distance==a.rangedDistance and a.meleeDistance>0)"),
              "native observation uses typed sphere and OBB centers instead of stale root or scene bounds");
        const uint8_t oldOpaqueRangeType=native_world::weapons[1][0x6A];
        native_world::weapons[1][0x6A]=4;
        native_world::Put(native_world::weapons[1].data(),0x72,uint32_t(30000));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0xEC4,int32_t(0));
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{50000,0,0});
        native_world::Put(sphereShape,0x10,native_world::Vec3{26000,100,13});
        unit.Think();
        Check(Eval("local a; for _,t in ipairs(geometryObservation.targets) do if t.id==1 then a=t end end; local w=geometryObservation.weapons[2]; assert(a and a.rangedDistance>24000 and a.rangedDistance<30000 and w.kind=='opaque' and w.ordinary==1 and w.attackStyle==1 and w.rangeType==4)"),
              "a legal opaque ordinary weapon retains native metadata and targets beyond the extra-perception cap");
        native_world::weapons[1][0x6A]=oldOpaqueRangeType;
        native_world::Put(native_world::weapons[1].data(),0x72,uint32_t(500));
        native_world::Put(sphereShape,0x10,native_world::Vec3{12000,0,0});
        unit.Think();
        Check(Eval("local t;for _,v in ipairs(geometryObservation.targets) do if v.id==1 then t=v end end;assert(t and t.rangedDistance>10000 and t.rangedVisible==1)"),
              "short-range loadouts still observe distant line-of-sight combatants for pursuit");
        native_world::Put(native_world::weapons[1].data(),0x72,uint32_t(2000));
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{250,0,0});
        native_world::Put(sphereShape,0x10,native_world::Vec3{250,0,0});
        auto selfSphere=native_world::targetShapes[0].data();
        native_world::Put(native_world::nodes[0].data(),0xE0,selfSphere);
        native_world::Put(selfSphere,0x10,native_world::Vec3{100,0,0});
        native_world::Configure(native_world::sameTeamNpc.data(),4,4,2,1000,100,0);
        native_world::Put(native_world::wall.data(),0x0C,native_world::sameTeamNpc.data());
        native_world::blocked=true;native_world::forceObbBlock=true;
        unit.Think();
        Check(Eval("local a; for _,t in ipairs(geometryObservation.targets) do if t.id==1 then a=t end end; assert(a and a.rangedVisible==1 and a.meleeVisible==0 and a.meleeDistance>149 and a.meleeDistance<151 and a.rangedDistance>a.meleeDistance)")&&
              native_world::obbCalls>0&&std::abs(native_world::lastObbRay.origin.x-100)<0.01f&&
              std::abs(native_world::lastObbRay.origin.y)<0.01f&&std::abs(native_world::lastObbRay.origin.z)<0.01f,
              "ranged sight ignores a friendly body while strict melee uses the native self-sphere origin");
        native_world::Put(native_world::wall.data(),0x0C,native_world::enemy2.data());
        unit.Think();
        Check(Eval("local a; for _,t in ipairs(geometryObservation.targets) do if t.id==1 then a=t end end; assert(a and a.rangedVisible==0 and a.meleeVisible==0)"),
              "an enemy collision body blocks both ranged and melee visibility");
        native_world::Put(native_world::wall.data(),0x0C,native_world::sameTeamNpc.data());
        native_world::forceObbBlock=false;native_world::terrainWall=true;native_world::terrainCalls=0;
        unit.Think();
        Check(Eval("local a; for _,t in ipairs(geometryObservation.targets) do if t.id==1 then a=t end end; assert(a and a.rangedVisible==0 and a.meleeVisible==0)")&&native_world::terrainCalls>=2,
              "terrain blocks both visibility modes even when its backpointer names a friendly object");
        native_world::blocked=native_world::terrainWall=native_world::forceObbBlock=false;
        native_world::Put(native_world::wall.data(),0x0C,static_cast<uint8_t*>(nullptr));
        native_world::Put(native_world::nodes[0].data(),0xE0,static_cast<uint8_t*>(nullptr));
        native_world::Put(native_world::nodes[1].data(),0xE0,static_cast<uint8_t*>(nullptr));
        native_world::Put(native_world::nodes[2].data(),0xD8,static_cast<uint8_t*>(nullptr));
        native_world::unitKinds[1]=native_world::unitKinds[2]=0;
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{1000,0,0});
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{1400,0,100});
        native_world::Put(native_world::scenes[1].data(),0xBC,native_world::Aabb{{960,50,-40},{1040,150,40}});
        native_world::Put(native_world::scenes[2].data(),0xBC,native_world::Aabb{{1360,50,60},{1440,150,140}});
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(100));
        native_world::Put(native_world::enemy2.data(),0xEC4,int32_t(1));
        Eval("SDGO_BattleAI_GenericUpdate=geometrySavedGeneric");
        native_world::weapons[1][0x6E9]=1; // opaque initializer: static reader must not guess speed
        std::array<uint8_t,0xC00> projectile={};std::array<uint8_t,0x300> projectileNode={};
        std::array<void*,8> projectileVtable={};projectileVtable[0x18/sizeof(void*)]=native_world::Address(&native_world::Callbacks::Projectile);
        native_world::Put(projectile.data(),0,projectileVtable.data());
        native_world::Put(projectile.data(),8,projectileNode.data());
        native_world::Put(projectile.data(),0x9C4,unit.bytes);
        native_world::Put(projectile.data(),0x9C8,native_world::weapons[1].data());
        for(int i=0;i<3;++i) {
            bool result=reinterpret_cast<native_world::Callbacks*>(projectile.data())->Projectile(reinterpret_cast<void*>(0x1234));
            Check(result && *reinterpret_cast<uintptr_t*>(projectile.data()+0x100)==0x1234,"projectile observer preserves AL result and opaque argument");
        }
        Check(Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.weapons[2].projectileSpeed>2699 and o.weapons[2].projectileSpeed<2701); return 123 end"),"measured-speed probe installs");
        unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        memcpy(native_world::fsms[0].data()+0x56C,native_world::fsms[0].data()+0x24C,4*0xC8);
        unit.bytes[0xE34]=1;
        for(int i=0;i<3;++i) reinterpret_cast<native_world::Callbacks*>(projectile.data())->Projectile(nullptr);
        Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.valid==1 and o.weapons[2].projectileSpeed==0); return 123 end");
        unit.Think();
        Check(true,"old-form projectile samples are not learned as the new form's speed");
        unit.bytes[0xE34]=0;Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Check(Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.weapons[2].projectileSpeed==0); return 123 end"),"nonlinear-speed probe installs");
        projectile[0xA38]=1;
        reinterpret_cast<native_world::Callbacks*>(projectile.data())->Projectile(nullptr);
        unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        projectile[0xA38]=0;projectileVtable[0x18/sizeof(void*)]=native_world::Address(&native_world::Callbacks::Homing);
        native_world::Put(native_world::definitions[0].data(),0,uint32_t(987650));
        for(int i=0;i<3;++i) reinterpret_cast<native_world::Callbacks*>(projectile.data())->Homing(nullptr);
        Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.weapons[2].projectileSpeed==0); return 123 end");
        unit.Think();
        Check(true,"constant-speed homing wrapper is not misclassified as a straight projectile");
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        native_world::weapons[1][0x6E9]=0;
        // The prior live-controller cases can leave a short recovery/defence
        // lease. Allocator and short-step checks below require a free synthetic
        // controller; native-action ownership is checked separately afterwards.
        Sleep(260);
        auto routeFsm=native_world::fsms[0].data(),routeMotor=native_world::movements[0].data();
        native_world::Put(routeFsm,4,routeFsm+0x20);native_world::Put(routeFsm,8,uint32_t(0));
        native_world::Put(routeMotor,4,routeMotor+0x20);native_world::Put(routeMotor,8,uint32_t(0));
        Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_CancelRoute(); return 123 end");unit.Think();
        native_world::Put(unit.bytes,0x3078,native_world::Vec3{400,0,0});
        Eval("SDGO_BattleAI_GenericUpdate=function(n) n:SetMoveTargetPos_Dir(2,400,2,false); directRecorded=SDGO_BattleAI_DirectIntent(2,2,1); return 123 end");unit.Think();
        Check(Eval("assert(directRecorded==1)"),"relative native movement records its actual active goal transaction");
        const int acceptedDirectMoves=native_world::moves;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) directKept=SDGO_BattleAI_DirectIntent(2,2,0); return 123 end");unit.Think();unit.Think();
        Check(Eval("assert(directKept==1)")&&native_world::moves==acceptedDirectMoves,
              "an unchanged active native direct goal is retained without clearing its command list again");
        const int directStops=native_world::stopMotorCalls;
        native_world::Put(unit.bytes,0x3078,native_world::Vec3{500,0,0});unit.Think();
        Check(Eval("assert(directKept==0)")&&native_world::stopMotorCalls==directStops,
              "a goal replaced by another native owner is not claimed or stopped by the direct cache");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) directRecorded=SDGO_BattleAI_DirectIntent(2,2,1); return 123 end");unit.Think();
        unit.bytes[0x30DC]=0;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) directKept=SDGO_BattleAI_DirectIntent(2,2,0); return 123 end");unit.Think();
        Check(Eval("assert(directKept==0)"),"native completion releases the direct transaction for a fresh segment");
        native_world::Put(routeFsm,8,routeFsm+0x5C);
        Eval("SDGO_BattleAI_GenericUpdate=function(n) directAdmission=SDGO_BattleAI_DirectIntent(2,2,2); return 123 end");unit.Think();
        Check(Eval("assert(directAdmission==0)"),"relative movement admission refuses a pending native attack before any goal submission");
        native_world::Put(routeFsm,8,uint32_t(0));unit.Think();
        Check(Eval("assert(directAdmission==1)"),"a neutral native controller admits the next relative movement command");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_CancelRoute(); return 123 end");unit.Think();
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(native_world::wall.data(),0x0C,static_cast<uint8_t*>(nullptr));
        native_world::unknownBackwardScene=true;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) safeDirectionResult=SDGO_BattleAI_SafeDirection(2,400,1,2,3,4); return 123 end");unit.Think();
        Check(Eval("assert(safeDirectionResult==3)"),
              "unavailable geometry in the preferred direction does not skip independent safe lateral checks");
        native_world::unknownBackwardScene=false;
        native_world::Put(native_world::wall.data(),0x0C,native_world::enemy1.data());
        native_world::enclosedStep=true;native_world::blocked=true;
        native_world::Put(native_world::wall.data(),0x0C,static_cast<uint8_t*>(nullptr));
        native_world::Put(native_world::wall.data(),0xBC,native_world::Aabb{{-500,0,-500},{500,300,500}});
        const auto shortStepPosition=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x88);
        const auto shortStepSphere=*reinterpret_cast<native_world::Vec3*>(native_world::bodySpheres[0].data()+0x10);
        // The enclosing planes are centered at zero. Earlier ray-origin tests
        // move the root without moving the synthetic body sphere with it.
        native_world::Put(native_world::nodes[0].data(),0x88,native_world::Vec3{0,0,0});
        native_world::Put(native_world::bodySpheres[0].data(),0x10,native_world::Vec3{0,100,0});
        Eval("SDGO_BattleAI_GenericUpdate=function(n) shortDirection,shortDistance=SDGO_BattleAI_SafeStep(2,350,1,2,3,4); return 123 end");unit.Think();
        lua_getglobal(g_L,"shortDirection");const int shortDirection=static_cast<int>(lua_tonumber(g_L,-1));lua_pop(g_L,1);
        lua_getglobal(g_L,"shortDistance");const float shortDistance=static_cast<float>(lua_tonumber(g_L,-1));lua_pop(g_L,1);
        // Native defence can turn before this callback. Verify the endpoint
        // against the actual enclosing planes, including a diagonal heading.
        const auto shortForward=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        const native_world::Vec3 shortRight={shortForward.z,0,-shortForward.x};
        const auto shortVector=shortDirection==1?shortForward:shortDirection==2?native_world::Vec3{-shortForward.x,0,-shortForward.z}:
            shortDirection==3?native_world::Vec3{-shortRight.x,0,-shortRight.z}:shortRight;
        Check(shortDirection>=1&&shortDirection<=4&&shortDistance>50&&shortDistance<350&&
              std::abs(shortVector.x*shortDistance)<200&&std::abs(shortVector.z*shortDistance)<200,
              "solid-bounded space admits a separately verified shorter native step instead of refusing every direction");
        native_world::Put(native_world::nodes[0].data(),0x88,shortStepPosition);
        native_world::Put(native_world::bodySpheres[0].data(),0x10,shortStepSphere);
        native_world::enclosedStep=false;native_world::blocked=false;
        native_world::blocked=true;native_world::forwardExit=true;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) forwardEscape=SDGO_BattleAI_SafeDirection(2,350,1,2,3,4); return 123 end");unit.Think();
        Check(Eval("assert(forwardEscape==1,tostring(forwardEscape))"),"a forward-only exit remains available when retreat and both lateral directions are blocked");
        native_world::forwardExit=false;native_world::blocked=false;
        native_world::Put(native_world::wall.data(),0x0C,native_world::enemy1.data());
        native_world::Put(unit.bytes,0x3078,native_world::Vec3{NAN,NAN,NAN});unit.bytes[0x30DC]=0;
        Check(Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.valid==1 and o.goal==nil and o.goalKnown==0); return 123 end"),
              "inactive-native-goal observation probe installs");
        unit.Think();
        Check(std::isnan(reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078)->x),
              "unused nonfinite native steering storage is represented as absent without writing a fabricated goal");
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        // Invoke the real C callback during a generic dispatch, while its actor
        // context is valid, to exercise route ownership and native deallocation.
        Check(Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(0); SDGO_BattleAI_CancelRoute(); assert(SDGO_BattleAI_Route(1,1)==1); return 123 end"),"route probe installs");
        unit.Think();
        Check(native_world::frees>0,"native path buffer copied and released through the EXE allocator");
        const int handoffStops=native_world::stopMotorCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_CancelRoute(1); return 123 end");unit.Think();
        Check(native_world::stopMotorCalls==handoffStops&&unit.bytes[0x30DC]!=0,
              "a planned goal handoff releases adapter ownership without queuing native MoveStop ahead of its replacement");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_RouteMotion(3)==1); assert(SDGO_BattleAI_Route(1,1)==1); return 123 end");
        unit.Think();
        Check(native_world::lastGoalMode==1&&native_world::lastMoveType==3,
              "route goal keeps its native default while locomotion uses the separate native dash setter");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_RouteMotion(2)==1); return 123 end");
        unit.Think();
        Check(native_world::lastMoveType==2,"an active route can release dash without generating another route");
        Check(Eval("assert(SDGO_BattleAI_RouteMotion(3)==0)"),"route locomotion rejects calls outside an owned AI callback");
        native_world::Put(native_world::nodes[0].data(),0x88,native_world::Vec3{0,37,0});
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==1); return 123 end");
        unit.Think();const int heightGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) return 123 end");unit.Think();
        Check(native_world::goalCalls==heightGoals,
              "native steering Y differs from the endpoint without resubmitting an unchanged path every frame");
        native_world::Put(native_world::nodes[0].data(),0x88,native_world::Vec3{0,0,0});
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Check(Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==1); assert(SDGO_BattleAI_Route(999,1)==0); return 123 end"),"failed-repath probe installs");
        unit.Think();
        Check(unit.bytes[0x30DC]==0,"failed repath clears the old route and does not reissue its previous motor goal");
        native_world::pathMode=3;int beforeFree=native_world::frees;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==0); return 123 end");
        unit.Think();
        Check(native_world::frees==beforeFree+1 && native_world::protectedPath==nullptr && unit.bytes[0x30DC]==0,
              "unreadable native path copy still releases allocation and cancels route");
        native_world::pathMode=0;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==1); SDGO_BattleAI_CancelRoute(); return 123 end");
        unit.Think();Check(unit.bytes[0x30DC]==0,"cancelling an owned route releases its motor goal");
        auto advanceFsm=native_world::fsms[0].data();
        auto advanceMovement=native_world::movements[0].data();
        native_world::Put(advanceFsm,4,advanceFsm+0x20);native_world::Put(advanceFsm,8,uint32_t(0));
        native_world::Put(advanceMovement,4,advanceMovement+0x20);native_world::Put(advanceMovement,8,uint32_t(0));
        native_world::Put(native_world::nodes[0].data(),0x88,native_world::Vec3{0,37,0});
        int beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) advanceResult=SDGO_BattleAI_AdvancePoint(1000,999,0,250,0); return 123 end");
        unit.Think();
        auto advanceGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(Eval("assert(advanceResult==1)")&&native_world::goalCalls==beforeGoals+1&&native_world::lastGoalMode==1&&
              std::abs(advanceGoal.x-250)<0.01f&&std::abs(advanceGoal.y-37)<0.01f&&std::abs(advanceGoal.z)<0.01f&&unit.bytes[0x30DC],
              "native short-step bridge clamps horizontal pursuit and preserves the current height");
        unit.bytes[0x30DC]=0;beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) return 123 end");unit.Think();
        Check(native_world::goalCalls==beforeGoals+1&&unit.bytes[0x30DC],
              "a stopped motor with the same cached waypoint is rearmed");
        beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) advanceResult=SDGO_BattleAI_AdvancePoint(0,-500,1000,900,0); return 123 end");
        unit.Think();advanceGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(Eval("assert(advanceResult==1)")&&native_world::goalCalls==beforeGoals+1&&
              std::abs(advanceGoal.x)<0.01f&&std::abs(advanceGoal.y-37)<0.01f&&std::abs(advanceGoal.z-400)<0.01f,
              "native short-step bridge caps its maximum step at 400");
        beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) advanceResult=SDGO_BattleAI_AdvancePoint(1000,0,0,1,0); return 123 end");
        unit.Think();advanceGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(Eval("assert(advanceResult==1)")&&native_world::goalCalls==beforeGoals+1&&
              std::abs(advanceGoal.x-100)<0.01f&&std::abs(advanceGoal.y-37)<0.01f&&std::abs(advanceGoal.z)<0.01f,
              "native short-step bridge raises its minimum step limit to 100");
        native_world::weapons[1][0x8F]=0;native_world::Put(advanceFsm,4,advanceFsm+0x34);
        unit.bytes[0x30DC]=0;beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) advanceResult=SDGO_BattleAI_AdvancePoint(600,0,0,200,1); return 123 end");
        unit.Think();
        Check(Eval("assert(advanceResult==0)")&&native_world::goalCalls==beforeGoals,
              "stationary-only ranged fire retains motor ownership");
        native_world::weapons[1][0x8F]=1;beforeGoals=native_world::goalCalls;unit.Think();
        Check(Eval("assert(advanceResult==1)")&&native_world::goalCalls==beforeGoals+1,
              "a legal moving-shot window permits the bounded pursuit step");
        native_world::Put(advanceFsm,4,advanceFsm+0x48);beforeGoals=native_world::goalCalls;unit.Think();
        Check(Eval("assert(advanceResult==0)")&&native_world::goalCalls==beforeGoals,
              "active melee retains motor ownership from the short-step bridge");
        native_world::Put(advanceFsm,4,advanceFsm+0x98);beforeGoals=native_world::goalCalls;unit.Think();
        Check(Eval("assert(advanceResult==0)")&&native_world::goalCalls==beforeGoals,
              "active special retains motor ownership from the short-step bridge");
        native_world::Put(advanceFsm,4,advanceFsm+0x20);native_world::Put(advanceMovement,4,advanceMovement+0xE8);
        beforeGoals=native_world::goalCalls;unit.Think();
        Check(Eval("assert(advanceResult==0)")&&native_world::goalCalls==beforeGoals,
              "hurt movement state retains motor ownership from the short-step bridge");
        native_world::Put(advanceMovement,4,advanceMovement+0x20);beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(1); advanceResult=SDGO_BattleAI_AdvancePoint(600,0,0,200,0); return 123 end");
        unit.Think();
        Check(Eval("assert(advanceResult==0)")&&native_world::goalCalls==beforeGoals,
              "wall-recovery lease has priority over a new pursuit step");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(1); advanceResult=SDGO_BattleAI_AdvancePoint(600,0,0,200,0); return 123 end");
        Sleep(260);beforeGoals=native_world::goalCalls;unit.Think();
        Check(Eval("assert(advanceResult==1)")&&native_world::goalCalls==beforeGoals+1,
              "reasserting recovery does not renew an expired motor lease or erase the pending pursuit");
        unit.bytes[0x30DC]=0;beforeGoals=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(1); return 123 end");unit.Think();
        Check(native_world::goalCalls==beforeGoals+1,
              "a repeated recovery state preserves the newly planned path after its entry lease expires");
        Check(Eval("assert(SDGO_BattleAI_AdvancePoint(1,2,3,200,0)==0)"),
              "short-step bridge rejects calls outside the current owned AI callback");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(0); SDGO_BattleAI_CancelRoute(); return 123 end");
        unit.Think();
        unit.bytes[0x30DC]=0;
        native_world::Put(native_world::nodes[0].data(),0x88,native_world::Vec3{0,0,0});
        native_world::Put(advanceFsm,4,advanceFsm+0x20);native_world::weapons[1][0x8F]=1;
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Eval("beforeRayFaultCalls=calls;beforeRayFaults=SDGO_BattleAI_Universal.faults");
        native_world::nullRay=true;unit.Think();native_world::nullRay=false;
        Check(Eval("assert(calls==beforeRayFaultCalls and SDGO_BattleAI_Universal.faults==beforeRayFaults+1)"),
              "an unavailable native ray reports an enhanced-policy failure without running a second brain");
        native_world::Ray3 offThreadRay={};
        std::thread worker([&] {
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&offThreadRay);
            reinterpret_cast<native_world::Callbacks*>(unit.bytes)->Intent();
        });worker.join();
        Check(offThreadRay.direction.x==0 && offThreadRay.direction.z==1,"off-thread native ray retains original behavior without shared policy maps");
        Eval("priorGenericGeneration=SDGO_BattleAI_Universal.states[20].generation");
        std::thread initWorker([&] {unit.Init(reinterpret_cast<void*>(3));});initWorker.join();
        Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].generation>priorGenericGeneration)"),"off-thread spawn reset is queued and applied by the established game thread");
        Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) sample=SDGO_BattleAI_Observe(); return 123 end");
        auto fsm=native_world::fsms[0].data();auto callbacks=reinterpret_cast<native_world::Callbacks*>(fsm);
        native_world::Put(fsm,4,fsm+0x34);native_world::Put(fsm,8,uint32_t(0));native_world::Put(fsm,0x224,uint32_t(7001));
        native_world::Put(fsm,0x248,native_world::weapons[1].data());
        unit.Think();Check(Eval("assert(sample.shots==0)"),"raw projectile/muzzle calls are not primary attack-cycle fires");
        native_world::shotReturn=0;
        callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shotAttempts==2 and sample.shots==2 and sample.shotNonzeroReturns==0)"),
              "zero-return native families retain the established primary receipt and expose no nonzero-return witness");
        native_world::shotReturn=0xBEEFF00D;
        callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shotAttempts==4 and sample.shots==4 and sample.shotNonzeroReturns==2)"),
              "native receipt scope counts primary kinds 1 and 8, excludes chained kind 7, and observes stable nonzero returns separately");
        native_world::replaceShotAction=true;callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shotAttempts==6 and sample.shots==6 and sample.shotNonzeroReturns==3)"),
              "an action replaced by the original call keeps receipt compatibility without crediting a stale nonzero-return witness");
        native_world::replaceShotWeapon=true;callbacks->Receipts(nullptr);
        Check(*reinterpret_cast<uint8_t**>(fsm+0x248)==native_world::weapons[2].data(),
              "weapon replacement fixture changes the selected record during the original call");
        // The completion guard has already observed the changed pointer. Restore
        // the fixture's selected-slot/record pairing before Lua capability reads.
        native_world::Put(fsm,0x248,native_world::weapons[1].data());
        unit.Think();
        Check(Eval("assert(sample.shotAttempts==8 and sample.shots==8 and sample.shotNonzeroReturns==4)"),
              "a weapon replaced by the original call keeps receipt compatibility without crediting a stale nonzero-return witness");
        native_world::Put(fsm,0x224,uint32_t(0));
        callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shotAttempts==10 and sample.shots==10 and sample.shotNonzeroReturns==4)"),
              "an action-zero primary receipt preserves established shot semantics without manufacturing an identity witness");
        const auto shotSnapshot=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(
            GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        SdgoBattleAiUnitSnapshotV1 shotState{};shotState.size=sizeof(shotState);shotState.version=1;
        Check(shotSnapshot&&shotSnapshot(20,&shotState)==1&&
              shotState.ranged_attempts==10&&shotState.ranged_shots==10&&
              shotState.ranged_nonzero_returns==4,
              "native behavior snapshot exposes primary receipts and stable nonzero returns as separate telemetry");
        native_world::Put(fsm,0x224,uint32_t(7001));
        native_world::Put(fsm,0x248,native_world::weapons[1].data());
        native_world::Put(fsm,4,fsm+0x20);callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shotAttempts==10 and sample.shots==10 and sample.shotNonzeroReturns==4)"),
              "non-ranged state cannot manufacture a counted primary receipt or nonzero-return witness");
        native_world::coverWall=true;auto beforeCoverMoves=native_world::moves;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) coverSample=SDGO_BattleAI_Cover(2,1400,100,0); coverRepeat=SDGO_BattleAI_Cover(2,1400,100,0); count80=0; for i=1,1000 do count80=count80+SDGO_BattleAI_Chance(80) end; return 123 end");
        int maxCoverQueries=0;
        for(int frame=0;frame<6;++frame) {int previousQueries=native_world::queries;unit.Think();maxCoverQueries=(std::max)(maxCoverQueries,native_world::queries-previousQueries);}
        Check(maxCoverQueries<=46&&Eval("assert(coverRepeat.pending==1)"),
              "cover geometry is bounded per Think and repeated same-frame calls do not repeat the search");
        Check(Eval("assert(coverSample.valid==1 and coverSample.exposure<=3 and coverSample.peek.z>0)"),"real native bridge samples a minimally exposed fireable cover edge using private collision queries");
        Check(native_world::moves==beforeCoverMoves&&Eval("assert(count80>=750 and count80<=850)"),"cover sampling is read-only and mod-owned probability does not touch native RNG");
        native_world::coverWall=false;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) sample=SDGO_BattleAI_Observe(); return 123 end");
        native_world::Put(fsm,4,fsm+0x5C);native_world::Put(fsm,8,fsm+0x20);unit.Think();
        Check(Eval("assert(sample.meleeSwinging==1 and sample.rangedBusy==0)"),"actual swing survives a pending neutral observation");
        native_world::Put(fsm,8,uint32_t(0));native_world::Put(fsm,0x240,int32_t(0));
        native_world::Put(fsm,0x248,native_world::weapons[0].data());
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_MeleeGate(1); return 123 end");unit.Think();
        uint8_t input[0x20]={};input[0xD]=1;callbacks->SwingUpdate(input);
        Check(native_world::swingInput==0&&input[0xD]==1,"melee continuation gate preserves native update and restores event input afterward");
        native_world::Put(fsm,4,fsm+0x48);native_world::Put(fsm,8,uint32_t(0));
        native_world::Put(fsm,0x200,uint32_t(2));unit.bytes[0x312A]=1;unit.bytes[0xDCD]=1;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_MeleeGate(1,1); return 123 end");unit.Think();
        const int beforeTrackingCancel=native_world::trackUpdates;
        callbacks->TrackingUpdate(input);
        Check(*reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x48&&
              *reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x20&&
              !unit.bytes[0x312A]&&!unit.bytes[0xDCD]&&native_world::trackUpdates==beforeTrackingCancel,
              "confirmed ranged response releases held knife tracking through the existing native Neutral transition");
        native_world::Put(fsm,8,fsm+0x5C);unit.bytes[0x312A]=1;unit.bytes[0xDCD]=1;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_MeleeGate(1,1); return 123 end");unit.Think();
        callbacks->TrackingUpdate(input);
        Check(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C&&unit.bytes[0x312A]&&unit.bytes[0xDCD]&&
              native_world::trackUpdates==beforeTrackingCancel+1,
              "ranged response never overwrites an already queued native swing or clears its committed input");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_MeleeGate(0); sample=SDGO_BattleAI_Observe(); return 123 end");
        native_world::Put(fsm,4,fsm+0x5C);native_world::Put(fsm,8,uint32_t(0));
        native_world::Put(fsm,0x200,uint32_t(2));native_world::Put(fsm,0x224,uint32_t(1234));
        uint8_t message[0x50]={};native_world::Put(message,0,uint16_t(0xC));native_world::Put(message,0x14,int32_t(100));native_world::Put(message,0x3C,uint32_t(20));
        bool delivered=native_world::EmitContact(native_world::document.data(),2,message,0);unit.Think();
        Check(delivered&&Eval("assert(sample.contactTarget==2)"),"only the exact immediate-contact caller records delivered native melee contact");
        reinterpret_cast<native_world::Callbacks*>(native_world::document.data())->Message(1,message,0);unit.Think();
        Check(Eval("assert(sample.contactTarget==2)"),"another hurt-message caller cannot be mistaken for immediate melee contact");
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{100,0,0});
        native_world::Put(fsm,4,fsm+0x48);native_world::Put(fsm,0x968,int32_t(0));
        auto beforeRotations=native_world::rotations,beforeUpdates=native_world::trackUpdates;
        callbacks->TrackingEntry(input);callbacks->TrackingUpdate(input);WaitNativeTicks(20);callbacks->TrackingUpdate(input);
        Check(native_world::rotations==beforeRotations+1&&native_world::trackUpdates==beforeUpdates&&
              *reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C,
              "selected opener cues only the NPC and skips tracking dash through the native queued swing");
        native_world::Put(fsm,4,fsm+0x5C);native_world::Put(fsm,8,uint32_t(0));callbacks->SwingEntry(input);
        Check(native_world::faces>0&&*reinterpret_cast<float*>(unit.bytes+0xD98)==0&&native_world::swingEntries>0,
              "actual swing Entry restores native target facing and runs original windup");
        fsm[0x9B1]=1;native_world::Put(fsm,4,fsm+0x48);beforeUpdates=native_world::trackUpdates;
        callbacks->TrackingEntry(input);callbacks->TrackingUpdate(input);
        Check(native_world::trackUpdates==beforeUpdates+1,"unsupported parallel tracking path falls back to original update");
        fsm[0x9B1]=0;
        auto movement=native_world::movements[0].data();native_world::Put(movement,4,movement+0xE8);
        beforeUpdates=native_world::trackUpdates;callbacks->TrackingEntry(input);callbacks->TrackingUpdate(input);unit.Think();
        Check(native_world::trackUpdates==beforeUpdates+1&&Eval("assert(sample.hurt==1)"),"pure MovementFSM hurt blocks quick swing without consuming CheckHurt latch");
        native_world::Put(movement,4,movement+0x20);native_world::Put(fsm,4,fsm+0x20);
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(100));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,150});
        auto enemyFsm=native_world::fsms[2].data();native_world::Put(enemyFsm,4,enemyFsm+0x5C);native_world::Put(enemyFsm,0x200,uint32_t(20));
        native_world::Put(enemyFsm,0x240,int32_t(0));native_world::Put(enemyFsm,0x248,native_world::weapons[0].data());
        native_world::Put(unit.bytes,0x2B70,500.0f);native_world::Put(unit.bytes,0x2B74,500.0f);
        native_world::Put(native_world::definitions[0].data(),0x145,int32_t(500));native_world::definitions[0][0x149]=0;
        native_world::definitions[0][0x14E]=4;native_world::definitions[0][0x14F]=4;
        native_world::Put(movement,4,movement+0xE8);unit.bytes[0x30DC]=0;
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        Sleep(16);unit.Think();auto shieldFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(shieldFacing.z>0.95f&&std::abs(shieldFacing.x)>0.01f&&unit.bytes[0x30DC]==0,
              "Hard begins a smooth back-shield turn during native hurt without forcing a movement escape");
        for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
        shieldFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(shieldFacing.z<-0.98f,"Hard presents an intact rear shield to the melee attacker");
        native_world::Put(unit.bytes,0x2B70,0.0f);Sleep(16);unit.Think();
        auto brokenFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(brokenFacing.z>-0.999f&&brokenFacing.z<0,"shield break immediately starts a bounded turn back toward front defence");
        for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
        brokenFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(brokenFacing.z>0.98f,"broken rear shield never leaves the bot intentionally exposing its back");
        native_world::blocked=true;native_world::Put(unit.bytes,0x2B70,500.0f);native_world::definitions[0][0x149]=2;
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        native_world::Put(message,0x3C,uint32_t(2));message[0x19]=1;message[0x1A]=2;
        native_world::EmitContact(native_world::document.data(),20,message,0);
        const auto hitTypeStarted=GetTickCount64();
        const int hitTypeRotations=native_world::rotations;
        for(int i=0;i<16;++i) {
            native_world::EmitContact(native_world::document.data(),20,message,0);
            WaitNativeTicks(16);
            unit.Think();
        }
        shieldFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        const bool hitTypeFacing=shieldFacing.z<-0.98f;
        if(!hitTypeFacing) {
            std::printf("DIAG hit-type elapsed=%llu rotations=%d->%d facing=(%.3f,%.3f,%.3f) message=(range:%u hit:%u) shield=(%.1f/%.1f type:%u mode:%u) obbDir=(%.3f,%.3f,%.3f)\n",
                static_cast<unsigned long long>(GetTickCount64()-hitTypeStarted),hitTypeRotations,native_world::rotations,
                shieldFacing.x,shieldFacing.y,shieldFacing.z,
                static_cast<unsigned>(message[0x19]),static_cast<unsigned>(message[0x1A]),
                *reinterpret_cast<float*>(unit.bytes+0x2B70),*reinterpret_cast<float*>(unit.bytes+0x2B74),
                static_cast<unsigned>(native_world::definitions[0][0x149]),
                static_cast<unsigned>(native_world::definitions[0][0x14E+*reinterpret_cast<int*>(fsm+0x240)]),
                native_world::lastObbRay.direction.x,native_world::lastObbRay.direction.y,native_world::lastObbRay.direction.z);
            Eval("local t=nil; for _,v in ipairs(sample.targets or {}) do if v.id==2 then t=v end end; print(string.format('DIAG hit-type observation attacker=%s hurt=%s targetVisible=%s ranged=%s melee=%s targetHit=%s known=%s', tostring(sample.meleeAttacker),tostring(sample.hurt),tostring(t and t.visible),tostring(t and t.rangedVisible),tostring(t and t.meleeVisible),tostring(t and t.hitType),tostring(t and t.hitKnown)))");
        }
        Check(hitTypeFacing,"a recent delivered attacker uses message HitType at +1A, not adjacent RangeType +19, when visibility is lost");
        native_world::blocked=false;native_world::definitions[0][0x149]=0;
        for(uint8_t sideMode:{uint8_t(2),uint8_t(1)}) {
            native_world::definitions[0][0x14E]=sideMode;native_world::definitions[0][0x14F]=sideMode;
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
            for(int i=0;i<8;++i) {Sleep(16);unit.Think();}
            auto sideFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
            Check(sideMode==2?sideFacing.x<-0.98f:sideFacing.x>0.98f,
                  sideMode==2?"class-3 shield presents its numeric side with forward -X to attacker +Z":"class-4 shield uses the opposite native yaw sign with forward +X");
        }
        native_world::Put(movement,4,movement+0x20);
        native_world::Put(unit.bytes,0x2B70,500.0f);
        native_world::definitions[0][0x14E]=native_world::definitions[0][0x14F]=4;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe(); n:SetMoveTargetPos_Dir(2,400,2,false); n:SetMoveType(1); return 123 end");
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,-1});
        unit.Think();
        Check(reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f&&unit.bytes[0x30DC]!=0,
              "Hard legal retreat faces the attacker instead of turning its intact rear shield and back into the movement");
        native_world::Put(unit.bytes,0x2B70,0.0f);
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Config("[settings]\nGenericMode=1\nEnabled=0\n");Sleep(2100);unit.Think();
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&ray);
        Check(unit.bytes[0x312A]==0 && unit.bytes[0x30DC]==0 && ray.direction.z==1 && ray.direction.x==0,
              "disabling generic mode releases owned motor and shooting and retains stock ray");
    } else if(missing) {
        Check(Eval("assert(SDGO_BattleAI==nil and calls==0)"),"missing enhanced policy scripts are explicit errors instead of switching to native AI");
        Check(FileContains(
                  g_root+L"\\sdgo\\logs\\sdgo.log",
                  "cannot read AI policy scripts"),
              "first-load missing policy remains visible in the native Mod log");
        const auto delayed=
            g_root+L"\\sdgo\\mods\\battle_ai\\battle_ai.delayed.lua";
        const auto live=
            g_root+L"\\sdgo\\mods\\battle_ai\\battle_ai.lua";
        Check(CopyFileW(delayed.c_str(),live.c_str(),FALSE)!=0,
              "fixture makes the delayed policy available without restarting the Mod");
        Sleep(2100);unit.Think();
        Check(Eval(
                  "fixtureDelayedPolicy=SDGO_BattleAI and 1 or 0; "
                  "fixtureDelayedLegacyState=SDGO_BattleAI and "
                  "SDGO_BattleAI.states[20] and 1 or 0; "
                  "fixtureDelayedUniversalState=SDGO_BattleAI_Universal and "
                  "SDGO_BattleAI_Universal.states[20] and 1 or 0; "
                  "fixtureDelayedFaults=SDGO_BattleAI and SDGO_BattleAI.faults or -1; "
                  "fixtureDelayedCalls=calls"),
              "fixture reads the delayed policy state after the next file check");
        std::printf(
            "DIAG missing-recovery policy=%.0f legacyState=%.0f "
            "universalState=%.0f faults=%.0f calls=%.0f\n",
            GlobalNumber("fixtureDelayedPolicy"),
            GlobalNumber("fixtureDelayedLegacyState"),
            GlobalNumber("fixtureDelayedUniversalState"),
            GlobalNumber("fixtureDelayedFaults"),
            GlobalNumber("fixtureDelayedCalls"));
        Check(GlobalNumber("fixtureDelayedPolicy")==1&&
              GlobalNumber("fixtureDelayedUniversalState")==1&&
              GlobalNumber("fixtureDelayedFaults")==0&&
              GlobalNumber("fixtureDelayedCalls")==1,
              "a first-load missing policy becomes ready with exactly its one policy-owned native initialization");
        Check(Eval(
                  "savedDelayedGeneric=SDGO_BattleAI_GenericUpdate; "
                  "savedDelayedAfter=SDGO_BattleAI.after; "
                  "assert(savedDelayedGeneric and savedDelayedAfter)"),
              "the recovered first policy exposes known working callbacks before hot reload");
        FILE* invalid=nullptr;
        _wfopen_s(&invalid,live.c_str(),L"ab");
        if(invalid) {
            fputs("\nthis is invalid lua [",invalid);
            fclose(invalid);
        }
        Check(invalid!=nullptr,
              "fixture makes the recovered policy invalid for the next hot reload");
        Sleep(2100);unit.Think();
        Check(Eval(
                  "fixtureReloadGenericSame="
                  "(SDGO_BattleAI_GenericUpdate==savedDelayedGeneric) and 1 or 0; "
                  "fixtureReloadAfterSame="
                  "(SDGO_BattleAI.after==savedDelayedAfter) and 1 or 0; "
                  "fixtureReloadUniversalState=SDGO_BattleAI_Universal and "
                  "SDGO_BattleAI_Universal.states[20] and 1 or 0; "
                  "fixtureReloadFaults=SDGO_BattleAI.faults; "
                  "fixtureReloadCalls=calls"),
              "fixture reads the retained policy after a rejected hot reload");
        std::printf(
            "DIAG missing-reload genericSame=%.0f afterSame=%.0f "
            "universalState=%.0f faults=%.0f calls=%.0f\n",
            GlobalNumber("fixtureReloadGenericSame"),
            GlobalNumber("fixtureReloadAfterSame"),
            GlobalNumber("fixtureReloadUniversalState"),
            GlobalNumber("fixtureReloadFaults"),
            GlobalNumber("fixtureReloadCalls"));
        Check(GlobalNumber("fixtureReloadGenericSame")==1&&
              GlobalNumber("fixtureReloadAfterSame")==1&&
              GlobalNumber("fixtureReloadUniversalState")==1&&
              GlobalNumber("fixtureReloadFaults")==0&&
              GlobalNumber("fixtureReloadCalls")==1,
              "a failed hot reload retains the already valid policy without another native fallback");
        Check(FileContains(
                  g_root+L"\\sdgo\\logs\\sdgo.log",
                  "Lua reload rejected; previous policy retained"),
              "the rejected hot reload remains visible in the native Mod log");
    } else if(disabled||bad) {
        Check(Eval("assert(SDGO_BattleAI==nil and calls==1)"),"disabled or rejected mod leaves native AI intact");
    } else {
        Check(Eval("assert(SDGO_BattleAI and SDGO_BattleAI.states[20] and SDGO_BattleAI.faults==0); firstGeneration=SDGO_BattleAI.states[20].generation"),"DLL injects policy on first native Think");
        unit.Init(reinterpret_cast<void*>(2)); unit.Think();
        Check(Eval("assert(SDGO_BattleAI.states[20].generation>firstGeneration and calls==2)"),"native respawn hook changes per-NPC generation");
        unit.bytes[0x2B65]=1; Eval("SDGO_BattleAI.states={}"); unit.Think();
        Check(Eval("assert(SDGO_BattleAI.states[20]==nil and calls==3)"),"remote native flag rejects enhanced decisions");
        unit.bytes[0x2B65]=0; *reinterpret_cast<uint32_t*>(unit.bytes+0x3238)=100;
        unit.Think();
        Check(Eval("assert(SDGO_BattleAI.states[20]==nil and calls==4)"),"unrelated native NPC id is excluded");
        *reinterpret_cast<uint32_t*>(unit.bytes+0x3238)=3604001;
        Config("[settings]\nGenericMode=0\nEnabled=0\nDecisionMs=invalid\n"); Sleep(2100); unit.Think();
        Check(Eval("assert(calls==5 and SDGO_BattleAI.faults==0)"),"separate config.ini reloads and invalid value is bounded");
        Config("[settings]\nGenericMode=0\nEnabled=1\n"); Sleep(2100); unit.Think();
        Check(Eval("assert(calls==6 and SDGO_BattleAI.states[20])"),"configuration can re-enable policy");
        Eval("function VSAI_Gunfighter_01_Update(n) calls=calls+1; return 123 end");
        Sleep(2100); unit.Think();
        Check(Eval("assert(calls==7 and VSAI_Gunfighter_01_Update==SDGO_BattleAI.wrappers.VSAI_Gunfighter_01_Update)"),"late native callback replacement is recaptured once");
        Eval("savedPolicy=SDGO_BattleAI.after");
        FILE* f=nullptr;
        _wfopen_s(&f,(g_root+L"\\sdgo\\mods\\battle_ai\\battle_ai.lua").c_str(),L"ab");
        if(f) { fputs("\nthis is invalid lua [",f); fclose(f); }
        Sleep(2100); unit.Think();
        Check(Eval("assert(calls==8 and SDGO_BattleAI.after==savedPolicy and SDGO_BattleAI.faults==0)"),"syntax error retains last working policy");
    }

    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);
        Config(generic?"[settings]\nDifficulty=Hard\nCoverChance=0\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        native_world::Configure(native_world::remoteHuman.data(),3,3,1,200,1200,150);
        native_world::remoteHuman[0x2B65]=1;native_world::Put(native_world::remoteHuman.data(),0x3238,uint32_t(0));
        native_world::Configure(native_world::sameTeamNpc.data(),4,4,2,1,600,50);
        native_world::remoteHead={&native_world::remoteEntry,&native_world::remoteEntry,nullptr};
        native_world::remoteEntry={&native_world::remoteHead,&native_world::remoteHead,native_world::remoteHuman.data()};
        native_world::friendlyHead={&native_world::friendlyEntry,&native_world::friendlyEntry,nullptr};
        native_world::friendlyEntry={&native_world::friendlyHead,&native_world::friendlyHead,native_world::sameTeamNpc.data()};
        native_world::Put(native_world::document.data(),0x70,&native_world::remoteHead);
        native_world::Put(native_world::document.data(),0x7C,&native_world::friendlyHead);
        native_world::Put(native_world::enemy1.data(),0x3238,uint32_t(0));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(50));
        unit.Init(reinterpret_cast<void*>(4));Sleep(2100);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "enemy AI selects the opposing team's AI while excluding its own lower-HP AI teammate");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "authoritative knockdown protection excludes the previous target from attack selection");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        auto protectedMovement=native_world::movements[2].data();native_world::Put(protectedMovement,8,protectedMovement+0x110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "a pending native get-up phase remains protected before authoritative state synchronization");
        native_world::Put(protectedMovement,8,uint32_t(0));native_world::Put(protectedMovement,4,protectedMovement+0xE8);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "ordinary hit-stun immediately becomes a legal target after the protected phase clears");
        native_world::Put(protectedMovement,4,protectedMovement+0x20);
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(0));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "the target's native permission state excludes an unattackable AI without excluding remote humans");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(4));native_world::Put(native_world::enemy2.data(),0xEB8,uint32_t(20));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "native restricted permission can admit an AI for this specific attacker");
        native_world::Put(native_world::enemy2.data(),0xEB8,uint32_t(999));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "an AI restricted to another attacker does not bypass native targetability");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0xEC4,int32_t(0));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "enemy AI selects the remote human teammate instead of being limited to the local human");
        native_world::Put(native_world::remoteHuman.data(),0x38,int32_t(0));
        native_world::Put(native_world::remoteHuman.data(),0xEC4,int32_t(0));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==1)"),
              "local human remains a normal eligible opponent after other opposing targets die");
        native_world::Put(unit.bytes,0x20,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(50));native_world::Put(native_world::enemy2.data(),0xEC4,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0x20,int32_t(2));
        native_world::Put(native_world::sameTeamNpc.data(),0x20,int32_t(1));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "host-side allied AI attacks the opposing AI and never the host or its own AI teammate");
        auto protectionFsm=native_world::fsms[0].data();
        Check(Eval(
                  "savedProtectionHookPolicy=SDGO_BattleAI_GenericUpdate; "
                  "protectionGateBaseline=0; "
                  "SDGO_BattleAI_GenericUpdate=function(n) "
                  "SDGO_BattleAI_MeleeGate(0,0); "
                  "protectionGateBaseline=protectionGateBaseline+1; "
                  "return 123 end"),
              "fixture installs an explicit unsuppressed policy baseline before live protection changes");
        unit.Think();
        Check(Eval(
                  "assert(protectionGateBaseline==1); "
                  "SDGO_BattleAI_GenericUpdate=savedProtectionHookPolicy"),
              "live protection hooks start after a confirmed unsuppressed policy frame");
        native_world::Put(protectionFsm,0x240,int32_t(0));native_world::Put(protectionFsm,0x248,native_world::weapons[0].data());
        native_world::Put(protectionFsm,0x200,uint32_t(2));native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));
        auto sourceCallbacks=reinterpret_cast<native_world::Callbacks*>(protectionFsm);
        const int beforeProtectedAdmission=native_world::attackAdmissions;sourceCallbacks->AttackAdmission();
        Check(native_world::attackAdmissions==beforeProtectedAdmission,
              "native melee admission rejects protection even if it changed after the last Lua decision");
        native_world::Put(protectionFsm,4,protectionFsm+0x48);native_world::Put(protectionFsm,8,uint32_t(0));
        uint8_t trackingInput[0x20]={};trackingInput[0xD]=1;
        const int beforeProtectedTracking=native_world::trackUpdates;sourceCallbacks->TrackingUpdate(trackingInput);
        Check(*reinterpret_cast<uint8_t**>(protectionFsm+4)==protectionFsm+0x48&&
              *reinterpret_cast<uint8_t**>(protectionFsm+8)==protectionFsm+0x20&&native_world::trackUpdates==beforeProtectedTracking,
              "protected-target knife tracking queues a normal release instead of chasing or snapping an active swing");
        native_world::Put(protectionFsm,8,uint32_t(0));native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        sourceCallbacks->TrackingUpdate(trackingInput);
        Check(native_world::trackUpdates==beforeProtectedTracking+1,
              "ordinary tracking resumes immediately when target protection clears");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));native_world::Put(protectionFsm,8,protectionFsm+0x5C);
        sourceCallbacks->TrackingUpdate(trackingInput);
        Check(*reinterpret_cast<uint8_t**>(protectionFsm+8)==protectionFsm+0x5C&&native_world::trackUpdates==beforeProtectedTracking+2,
              "protection cancellation never overwrites an already queued native action");
        native_world::Put(protectionFsm,4,protectionFsm+0x5C);uint8_t protectedInput[0x20]={};protectedInput[0xD]=1;
        native_world::swingInput=-1;native_world::lastCallbackSelf=nullptr;
        sourceCallbacks->SwingUpdate(protectedInput);
        Check(native_world::swingInput==0&&protectedInput[0xD]==1&&
              native_world::lastCallbackSelf==protectionFsm,
              "native continuation suppresses another slash against a protected target without corrupting shared input");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));sourceCallbacks->AttackAdmission();
        native_world::swingInput=-1;native_world::lastCallbackSelf=nullptr;
        sourceCallbacks->SwingUpdate(protectedInput);
        const bool ordinaryHurt=
            native_world::attackAdmissions==
                beforeProtectedAdmission+1&&
            native_world::swingInput==1&&
            native_world::lastCallbackSelf==protectionFsm;
        if(!ordinaryHurt) {
            lua_getglobal(g_L,"fixtureMeleeGateCalls");
            const int meleeGateCalls=
                static_cast<int>(lua_tonumber(g_L,-1));
            lua_pop(g_L,1);
            lua_getglobal(g_L,"fixtureMeleeGateValue");
            const int meleeGateValue=
                static_cast<int>(lua_tonumber(g_L,-1));
            lua_pop(g_L,1);
            lua_getglobal(g_L,"fixtureMeleeGateCancel");
            const int meleeGateCancel=
                static_cast<int>(lua_tonumber(g_L,-1));
            lua_pop(g_L,1);
            const auto protection=
                battle_ai::native::ReadDamageProtection(
                    native_world::enemy2.data());
            SdgoBattleAiUnitSnapshotV1 state{};
            state.size=sizeof(state);state.version=1;
            const int haveSnapshot=
                diagnosticSnapshot
                    ?diagnosticSnapshot(20,&state):0;
            std::printf(
                "DIAG ordinary-hurt admissions=%d expected=%d "
                "swingInput=%d eventInput=%u callbackSelf=%p expectedSelf=%p "
                "meleeGateCalls=%d "
                "suppressMeleeInferred=%d cancelMeleeInferred=%d "
                "protectionKnown=%u protectionActive=%u snapshot=%d "
                "attackState=%u attackPending=%u\n",
                native_world::attackAdmissions,
                beforeProtectedAdmission+1,
                native_world::swingInput,
                static_cast<unsigned>(protectedInput[0xD]),
                native_world::lastCallbackSelf,protectionFsm,
                meleeGateCalls,meleeGateValue,meleeGateCancel,
                protection.known?1u:0u,
                protection.active?1u:0u,
                haveSnapshot,state.attack_state,
                state.attack_pending);
        }
        Check(ordinaryHurt,
              "ordinary hurt resumes legal native melee admission and continuation immediately");
        native_world::Put(protectionFsm,4,protectionFsm+0x20);native_world::Put(protectionFsm,8,uint32_t(0));native_world::Put(protectionFsm,0x240,int32_t(1));
        native_world::Put(protectionFsm,0x248,native_world::weapons[1].data());
        native_world::Put(native_world::document.data(),0x70,static_cast<native_world::ListNode*>(nullptr));
        native_world::Put(native_world::document.data(),0x7C,static_cast<native_world::ListNode*>(nullptr));
    }
    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);
        Config(generic?"[settings]\nDifficulty=Hard\nCoverChance=0\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{321,77,-45});
        native_world::unitKinds[2]=0x15;

        std::array<uint8_t,0x3300> staleAlias{};
        native_world::Configure(staleAlias.data(),4,2,1,777,9000,9000);
        native_world::Put(staleAlias.data(),0x9D8,int32_t(2));
        native_world::Put(staleAlias.data(),0xEB4,uint8_t(1));
        native_world::unitKinds[4]=0x15;
        native_world::ListNode staleHead{},staleEntry{},canonicalHead{},canonicalEntry{};
        staleHead={&staleEntry,&staleEntry,nullptr};
        staleEntry={&staleHead,&staleHead,staleAlias.data()};
        canonicalHead={&canonicalEntry,&canonicalEntry,nullptr};
        canonicalEntry={&canonicalHead,&canonicalHead,native_world::enemy2.data()};
        native_world::Put(native_world::document.data(),0x48,&staleHead);
        native_world::Put(native_world::document.data(),0x70,&canonicalHead);
        native_world::Put(native_world::document.data(),0x7C,static_cast<native_world::ListNode*>(nullptr));

        // Let the documented configuration reload finish before installing a
        // probe callback; otherwise a slow host can reload over the callback.
        WaitNativeTicks(2100);unit.Init(reinterpret_cast<void*>(6));unit.Think();
        Eval("canonicalIdentityPolicy=SDGO_BattleAI_GenericUpdate; "
             "SDGO_BattleAI_GenericUpdate=function(n) canonicalIdentityObservation=SDGO_BattleAI_Observe(); return 123 end");
        unit.Think();
        Check(Eval(
                  "local t=nil; for _,candidate in ipairs(canonicalIdentityObservation.targets) do "
                  "if candidate.id==2 then t=candidate end end; "
                  "assert(t and t.hp==1 and t.damageProtected==0 and t.attackable==1 and "
                  "t.pos.x==321 and t.pos.y==77 and t.pos.z==-45)"),
              "same-id stale list alias cannot override the live canonical target observation");
        Eval("SDGO_BattleAI_GenericUpdate=canonicalIdentityPolicy; SDGO_BattleAI_Universal.states={}");
        unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
        unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)")&&unit.bytes[0x312A],
              "canonical low-HP target is attackable on its first legal policy frame");
    }
    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);
        Config(generic?"[settings]\nDifficulty=Hard\nCoverChance=0\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0xEC4,int32_t(7));
        native_world::enemy2[0x326C]=1;
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{450,25,0});
        native_world::unitKinds[2]=0x15;
        Check(reinterpret_cast<native_world::Callbacks*>(native_world::enemy2.data())->CanAttack(unit.bytes)&&
              *reinterpret_cast<int32_t*>(native_world::enemy2.data()+0xEC4)!=0&&
              native_world::enemy2[0x326C]==1,
              "zero raw HP fixture remains native-attackable with an active lifecycle and controller");

        Eval("zeroHpPolicy=SDGO_BattleAI_GenericUpdate; "
             "SDGO_BattleAI_GenericUpdate=function(n) zeroHpObservation=SDGO_BattleAI_Observe(); return 123 end");
        unit.Init(reinterpret_cast<void*>(7));unit.Think();
        Check(Eval(
                  "local t=nil; for _,candidate in ipairs(zeroHpObservation.targets) do "
                  "if candidate.id==2 then t=candidate end end; "
                  "assert(t and t.hp==0 and t.alive==1 and t.damageProtected==0 and t.attackable==1 and "
                  "t.pos.x==450 and t.pos.y==25 and t.pos.z==0)"),
              "native-live zero raw HP target remains present in the authoritative observation");
        Eval("SDGO_BattleAI_GenericUpdate=zeroHpPolicy; SDGO_BattleAI_Universal.states={}");
        unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
        unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)")&&unit.bytes[0x312A],
              "native-live zero raw HP target remains attackable on its first legal policy frame");

        native_world::Put(native_world::enemy2.data(),0x38,int32_t(500));
        native_world::Put(native_world::enemy2.data(),0xEC4,int32_t(0));
        Eval("SDGO_BattleAI_GenericUpdate=function(n) deadLifecycleObservation=SDGO_BattleAI_Observe(); return 123 end");
        unit.Think();
        Check(Eval(
                  "for _,candidate in ipairs(deadLifecycleObservation.targets) do "
                  "assert(candidate.id~=2) end"),
              "native-dead lifecycle excludes a positive raw HP object from observation");
        Eval("SDGO_BattleAI_GenericUpdate=zeroHpPolicy; SDGO_BattleAI_Universal.states={}");
        unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
        unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==nil)")&&!unit.bytes[0x312A],
              "native-dead lifecycle cannot remain an attack target despite positive raw HP");
    }
    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);
        Config(generic?"[settings]\nDifficulty=Hard\nCoverChance=0\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,150});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,-1});
        unit.Init(reinterpret_cast<void*>(5));Sleep(2100);unit.Think();
        Check(!unit.bytes[0x312A]&&!unit.bytes[0xDCD]&&unit.bytes[0x30DC]&&
              reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f,
              "a sole knocked-down enemy triggers non-firing repositioning while the actor keeps its front toward the get-up target");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));unit.Think();
        Check(unit.bytes[0x312A]&&unit.bytes[0xDCD],
              "the first fully cleared get-up observation resumes a natively ready shot without an extra timer");
    }
    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);native_world::directPathFixture=true;native_world::pathMode=0;
        Config(generic?"[settings]\nDifficulty=Hard\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        unit.Init(reinterpret_cast<void*>(3));Sleep(2100);unit.Think();
        Eval("blockedVictimPolicy=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) blockedVictimRoute=SDGO_BattleAI_Route(2,1); return 123 end");
        unit.Think();
        Check(Eval("assert(blockedVictimRoute==1)")&&unit.bytes[0x30DC],
              "victim-control fixture begins with a mod-owned native movement route");
        Eval("SDGO_BattleAI_GenericUpdate=blockedVictimPolicy");
        auto victimFsm=native_world::fsms[0].data();
        auto victimMovement=native_world::movements[0].data();
        native_world::Put(victimFsm,4,victimFsm+0x20);native_world::Put(victimFsm,8,uint32_t(0));
        native_world::Put(victimMovement,4,victimMovement+0xE8);native_world::Put(victimMovement,8,victimMovement+0xE8);
        unit.bytes[0x312A]=1;native_world::Put(unit.bytes,0xDCD,uint16_t(0x101));
        const int victimStops=native_world::stopMotorCalls;
        const int victimAttackOff=native_world::message41Calls;
        const int victimGoals=native_world::goalCalls;
        g_expectedDispatchResult=0;
        for(int frame=0;frame<3;++frame) unit.Think();
        g_expectedDispatchResult=123;
        Check(native_world::stopMotorCalls==victimStops&&native_world::message41Calls==victimAttackOff&&
              native_world::goalCalls==victimGoals&&unit.bytes[0x312A]&&
              *reinterpret_cast<uint8_t**>(victimMovement+4)==victimMovement+0xE8&&
              *reinterpret_cast<uint8_t**>(victimMovement+8)==victimMovement+0xE8,
              "sustained native victim control clears its mod route without MoveStop or command41 interference");
        // This fixture does not execute the game's hurt release. Model that
        // authoritative handoff explicitly, then require output on its first
        // legal controller frame rather than accepting a fixture-held input.
        native_world::Put(victimMovement,4,victimMovement+0x20);native_world::Put(victimMovement,8,uint32_t(0));
        unit.bytes[0x30DC]=0;unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
        unit.Think();
        Check(unit.bytes[0x312A]&&*reinterpret_cast<uint8_t**>(victimMovement+4)==victimMovement+0x20&&
              *reinterpret_cast<uint8_t**>(victimMovement+8)==nullptr,
              "the fixture's authoritative victim release permits ready native output on the first legal frame");

        native_world::Initialize(unit.bytes,g_L);native_world::directPathFixture=true;native_world::pathMode=0;
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        unit.Init(reinterpret_cast<void*>(3));unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=function(n) defenceObservation=SDGO_BattleAI_Observe();return 123 end");
        auto location=[&] {return *reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x88);};
        auto setPosition=[&](native_world::Vec3 p){native_world::Put(native_world::nodes[0].data(),0x88,p);};
        auto crossfire=[&] {
            auto p=location();
            native_world::Put(native_world::nodes[1].data(),0x88,p+native_world::Vec3{0,0,-5000});
            native_world::Put(native_world::nodes[2].data(),0x88,p+native_world::Vec3{0,0,5000});
            uint8_t hit[0x50]={};native_world::Put(hit,0,uint16_t(0xC));native_world::Put(hit,0x14,int32_t(1));
            for(uint32_t attacker:{1u,2u}) {native_world::Put(hit,0x3C,attacker);native_world::EmitContact(native_world::document.data(),20,hit,0);}
        };
        crossfire();const auto handoffAnchor=location();const double radius=generic?1200:480;
        native_world::pathMode=4;const int priorPaths=native_world::pathCalls;unit.Think();
        const auto alternateGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(unit.bytes[0x30DC]&&alternateGoal.x<handoffAnchor.x&&native_world::pathCalls-priorPaths<=3,
              "a blocked best escape point tries a bounded reachable alternative instead of giving up on movement");
        crossfire();unit.Think();
        Check(Eval("assert(defenceObservation.routeActive==1 and defenceObservation.defenceMoving==1)"),
              "crossfire handoff fixture starts from an observed adapter-owned defensive route");
        auto defenceFsm=native_world::fsms[0].data();
        auto defenceMovement=native_world::movements[0].data();
        native_world::weapons[1][0x8F]=0;
        native_world::Put(defenceFsm,4,defenceFsm+0x20);native_world::Put(defenceFsm,8,defenceFsm+0x34);
        native_world::Put(defenceMovement,4,defenceMovement+0x20);native_world::Put(defenceMovement,8,uint32_t(0));
        unit.bytes[0x312A]=1;
        const int fixedGoals=native_world::goalCalls;
        const int fixedStops=native_world::stopMotorCalls;
        crossfire();unit.Think();
        Check(*reinterpret_cast<uint8_t**>(defenceFsm+4)==defenceFsm+0x20&&
              *reinterpret_cast<uint8_t**>(defenceFsm+8)==defenceFsm+0x34&&
              *reinterpret_cast<uint8_t**>(defenceMovement+4)==defenceMovement+0x20&&
              *reinterpret_cast<uint8_t**>(defenceMovement+8)==nullptr&&
              unit.bytes[0x312A]&&native_world::goalCalls==fixedGoals&&
              native_world::stopMotorCalls==fixedStops,
              "a pending stationary shot takes defensive-route ownership without rewriting either native FSM");
        crossfire();unit.Think();
        Check(Eval("assert(defenceObservation.routeActive==0)")&&native_world::goalCalls==fixedGoals,
              "stationary-only ranged ownership drops the adapter route instead of reissuing its movement goal");
        native_world::Put(defenceFsm,4,defenceFsm+0x20);native_world::Put(defenceFsm,8,uint32_t(0));
        unit.bytes[0x312A]=0;
        const int resumePaths=native_world::pathCalls;
        crossfire();unit.Think();
        Check(native_world::pathCalls>resumePaths&&
              *reinterpret_cast<uint8_t**>(defenceFsm+4)==defenceFsm+0x20&&
              *reinterpret_cast<uint8_t**>(defenceFsm+8)==nullptr&&
              *reinterpret_cast<uint8_t**>(defenceMovement+4)==defenceMovement+0x20&&
              *reinterpret_cast<uint8_t**>(defenceMovement+8)==nullptr,
              "native neutral handoff reloads the defensive route without mutating movement or attack state");
        crossfire();unit.Think();
        Check(Eval("assert(defenceObservation.routeActive==1 and defenceObservation.defenceMoving==1)"),
              "the resumed neutral defensive route is observable before ranged movement admission");
        native_world::weapons[1][0x8F]=1;
        native_world::Put(defenceFsm,4,defenceFsm+0x34);native_world::Put(defenceFsm,8,defenceFsm+0x34);
        unit.bytes[0x312A]=1;
        const int movingGoals=native_world::goalCalls;
        crossfire();unit.Think();crossfire();unit.Think();
        Check(Eval("assert(defenceObservation.routeActive==1 and defenceObservation.defenceMoving==1)")&&
              native_world::goalCalls==movingGoals&&unit.bytes[0x312A]&&
              *reinterpret_cast<uint8_t**>(defenceFsm+4)==defenceFsm+0x34&&
              *reinterpret_cast<uint8_t**>(defenceFsm+8)==defenceFsm+0x34&&
              *reinterpret_cast<uint8_t**>(defenceMovement+4)==defenceMovement+0x20&&
              *reinterpret_cast<uint8_t**>(defenceMovement+8)==nullptr,
              "current-and-pending moving-shot state retains the owned defensive route and native controls");
        const auto dispatchSnapshot=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(
            GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        auto readDispatches=[&] {
            SdgoBattleAiUnitSnapshotV1 state{};state.size=sizeof(state);state.version=1;
            Check(dispatchSnapshot&&dispatchSnapshot(20,&state)==1,
                  "native dispatch witness snapshot is readable on the game thread");
            return state;
        };
        native_world::Put(defenceFsm,0x200,uint32_t(1));
        crossfire();
        WaitNativeTicks(25);unit.Think();
        auto movingPosition=location();setPosition(movingPosition+native_world::Vec3{0,0,20});
        WaitNativeTicks(25);unit.Think();
        const auto beforeMobileDispatch=readDispatches();
        reinterpret_cast<native_world::Callbacks*>(defenceFsm)->Receipts(nullptr);
        const auto mobileDispatch=readDispatches();
        Check(mobileDispatch.mobile_primary_dispatches==beforeMobileDispatch.mobile_primary_dispatches+2&&
              mobileDispatch.mobile_defensive_dispatches==beforeMobileDispatch.mobile_defensive_dispatches+2&&
              mobileDispatch.backward_primary_dispatches==beforeMobileDispatch.backward_primary_dispatches+2,
              "post-ammo primary dispatches record a bound moving-shot gun with live motor, real defensive motion and physical retreat");
        native_world::Put(defenceMovement,4,defenceMovement+0xD4);
        movingPosition=location();setPosition(movingPosition+native_world::Vec3{0,0,20});
        WaitNativeTicks(25);unit.Think();
        reinterpret_cast<native_world::Callbacks*>(defenceFsm)->Receipts(nullptr);
        auto rejectedDispatch=readDispatches();
        Check(rejectedDispatch.mobile_primary_dispatches==mobileDispatch.mobile_primary_dispatches&&
              rejectedDispatch.mobile_defensive_dispatches==mobileDispatch.mobile_defensive_dispatches&&
              rejectedDispatch.backward_primary_dispatches==mobileDispatch.backward_primary_dispatches,
              "blocked native control cannot manufacture a mobile primary dispatch witness");
        native_world::Put(defenceMovement,4,defenceMovement+0x20);
        movingPosition=location();setPosition(movingPosition+native_world::Vec3{0,0,20});
        WaitNativeTicks(25);unit.Think();unit.bytes[0x30DC]=0;
        reinterpret_cast<native_world::Callbacks*>(defenceFsm)->Receipts(nullptr);
        rejectedDispatch=readDispatches();
        Check(rejectedDispatch.mobile_primary_dispatches==mobileDispatch.mobile_primary_dispatches&&
              rejectedDispatch.mobile_defensive_dispatches==mobileDispatch.mobile_defensive_dispatches&&
              rejectedDispatch.backward_primary_dispatches==mobileDispatch.backward_primary_dispatches,
              "a stopped native motor cannot manufacture a mobile primary dispatch witness");
        unit.bytes[0x30DC]=1;
        WaitNativeTicks(25);unit.Think();
        reinterpret_cast<native_world::Callbacks*>(defenceFsm)->Receipts(nullptr);
        rejectedDispatch=readDispatches();
        Check(rejectedDispatch.mobile_primary_dispatches==mobileDispatch.mobile_primary_dispatches&&
              rejectedDispatch.mobile_defensive_dispatches==mobileDispatch.mobile_defensive_dispatches&&
              rejectedDispatch.backward_primary_dispatches==mobileDispatch.backward_primary_dispatches,
              "a motor flag without fresh physical displacement cannot manufacture a mobile primary dispatch witness");
        native_world::Initialize(unit.bytes,g_L);native_world::directPathFixture=true;native_world::pathMode=4;
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        unit.Init(reinterpret_cast<void*>(31));unit.Think();
        defenceFsm=native_world::fsms[0].data();
        defenceMovement=native_world::movements[0].data();
        native_world::Put(defenceFsm,4,defenceFsm+0x20);native_world::Put(defenceFsm,8,uint32_t(0));
        native_world::Put(defenceFsm,0x240,int32_t(1));native_world::Put(defenceFsm,0x248,native_world::weapons[1].data());
        native_world::weapons[1][0x8F]=1;native_world::Put(unit.bytes,0xDD0,int32_t(1));
        native_world::Put(defenceMovement,4,defenceMovement+0x20);native_world::Put(defenceMovement,8,uint32_t(0));
        setPosition({0,0,0});native_world::Put(unit.bytes,0x3078,native_world::Vec3{0,0,0});unit.bytes[0x30DC]=0;
        native_world::Put(defenceFsm,4,defenceFsm+0x20);native_world::Put(defenceFsm,8,uint32_t(0));
        unit.bytes[0x312A]=0;native_world::Put(unit.bytes,0xDCD,uint16_t(0));
        const auto anchor=location();
        const int radiusStartPaths=native_world::pathCalls;
        crossfire();unit.Think();
        auto radiusStartGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(unit.bytes[0x30DC]&&native_world::pathCalls>radiusStartPaths&&
              battle_ai::Length(radiusStartGoal-anchor)>25&&battle_ai::Length(radiusStartGoal-anchor)<=radius+0.1,
              "the independent continuous-crossfire fixture starts a bounded native defensive route");
        crossfire();unit.Think();
        radiusStartGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(Eval("assert(defenceObservation.routeActive==1 and defenceObservation.defenceMoving==1)"),
              "the independent continuous-crossfire route is observable as adapter-owned on its next policy frame");
        const int goalsBeforeDefence=native_world::goalCalls;
        Eval("savedDefenceProbe=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) advanceResult=SDGO_BattleAI_AdvancePoint(1000,0,0,200,1); return 123 end");
        unit.Think();
        Check(Eval("assert(advanceResult==0)")&&native_world::goalCalls==goalsBeforeDefence,
              "front/rear defensive route retains motor ownership from a pursuit step");
        Eval("SDGO_BattleAI_GenericUpdate=savedDefenceProbe");
        if(unit.bytes[0x30DC]) setPosition(radiusStartGoal);
        native_world::pathMode=0;
        bool bounded=true;double furthest=0;
        for(int step=0;step<12;++step) {
            unit.Think();
            if(unit.bytes[0x30DC]) {
                auto goal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
                double distance=battle_ai::Length(goal-anchor);furthest=(std::max)(furthest,distance);
                bounded=bounded&&distance<=radius+0.1;setPosition(goal);
            }
        }
        if(!(bounded&&furthest>radius-1))
            std::printf("DIAG continuous-crossfire bounded=%d furthest=%.3f radius=%.3f anchor=(%.3f,%.3f,%.3f) location=(%.3f,%.3f,%.3f) motor=%u\n",
                bounded?1:0,furthest,radius,anchor.x,anchor.y,anchor.z,
                location().x,location().y,location().z,static_cast<unsigned>(unit.bytes[0x30DC]));
        Check(bounded&&furthest>radius-1,
              "a continuous crossfire episode reaches but never exceeds its Medium/Hard defensive destination radius");
        const auto nextAnchor=location();
        native_world::Put(native_world::nodes[1].data(),0x88,nextAnchor+native_world::Vec3{0,0,1000});
        native_world::Put(native_world::nodes[2].data(),0x88,nextAnchor+native_world::Vec3{10,0,1100});
        Sleep(1050);unit.Think();crossfire();unit.Think();
        const auto newGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(unit.bytes[0x30DC]&&battle_ai::Length(newGoal-nextAnchor)>25&&battle_ai::Length(newGoal-nextAnchor)<=radius+0.1,
              "a pressure-free interval expires the old anchor so a new crossfire episode can move again");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy1.data(),0xEC4,int32_t(0));setPosition(newGoal);
        const int beforePointRoute=native_world::pathCalls;unit.Think();
        Check(unit.bytes[0x30DC]&&native_world::pathCalls>beforePointRoute,
              "an explicit defensive destination remains navigable without a living threat object");
        crossfire();
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        Eval("SDGO_BattleAI_GenericUpdate=function(n) local before=SDGO_BattleAI_Observe(); SDGO_BattleAI_RecoveryMode(1); SDGO_BattleAI_CancelRoute(); n:SetMoveTargetPos_Dir(3,350,3,false); n:SetMoveType(1); n:SetJumpTime(0); n:SetAttackOnOff(true); local after=SDGO_BattleAI_Observe(); assert(after.movementRecovery==1 and after.defenceMoving==0); return 123 end");
        auto recoveryGoal=location()+native_world::Vec3{177,0,0};native_world::Put(unit.bytes,0x3078,recoveryGoal);
        const int routesBeforeRecovery=native_world::pathCalls,turnsBeforeRecovery=native_world::rotations;
        Sleep(16);unit.Think();
        const auto retainedGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(native_world::pathCalls==routesBeforeRecovery&&battle_ai::Length(retainedGoal-recoveryGoal)<0.01&&unit.bytes[0x30DC],
              "a wall-recovery motor lease prevents defence from restoring the blocked route in the same Think");
        Check(native_world::rotations>turnsBeforeRecovery,
              "wall recovery keeps bounded front-facing defence while it owns the movement goal");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(0); local o=SDGO_BattleAI_Observe(); assert(o.movementRecovery==0); return 123 end");
        unit.Think();
        native_world::Initialize(unit.bytes,g_L);native_world::directPathFixture=true;native_world::pathMode=0;
        Config(generic?"[settings]\nDifficulty=Hard\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        unit.Init(reinterpret_cast<void*>(4));Sleep(2100);unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe();return 123 end");
        auto stalePosition=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x88);
        native_world::Put(native_world::nodes[1].data(),0x88,stalePosition+native_world::Vec3{0,0,-5000});
        native_world::Put(native_world::nodes[2].data(),0x88,stalePosition+native_world::Vec3{0,0,5000});
        uint8_t staleHit[0x50]={};native_world::Put(staleHit,0,uint16_t(0xC));native_world::Put(staleHit,0x14,int32_t(1));
        for(uint32_t attacker:{1u,2u}) {
            native_world::Put(staleHit,0x3C,attacker);
            native_world::EmitContact(native_world::document.data(),20,staleHit,0);
        }
        unit.Think();
        Check(unit.bytes[0x30DC]!=0,"stale-defence fixture first owns a native movement route");
        Sleep(1050);
        stalePosition=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x88);
        native_world::Put(native_world::nodes[1].data(),0x88,stalePosition+native_world::Vec3{0,0,1000});
        native_world::Put(native_world::nodes[2].data(),0x88,stalePosition+native_world::Vec3{10,0,1100});
        auto staleFsm=native_world::fsms[0].data();
        native_world::Put(staleFsm,4,staleFsm+0x48);
        unit.Think();
        native_world::Put(staleFsm,4,staleFsm+0x20);
        const int goalsBeforeStaleDefence=native_world::goalCalls;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) advanceResult=SDGO_BattleAI_AdvancePoint(1000,0,0,200,0); return 123 end");
        unit.Think();
        Check(Eval("assert(advanceResult==1)")&&native_world::goalCalls==goalsBeforeStaleDefence+1,
              "an empty obsolete defence route releases motor ownership for pursuit");
        std::array<uint8_t,0x200> idleControl{};
        native_world::Put(idleControl.data(),4,idleControl.data()+0x20);
        native_world::Put(unit.bytes,0x9D4,idleControl.data());unit.bytes[0x30F0]='V';
        native_world::Put(staleFsm,4,staleFsm+0x20);native_world::Put(staleFsm,8,uint32_t(0));
        native_world::Put(defenceMovement,4,defenceMovement+0x20);native_world::Put(defenceMovement,8,uint32_t(0));
        unit.bytes[0x312A]=0;unit.bytes[0x30DC]=0;
        Eval("idlePolicyCalls=0; SDGO_BattleAI_GenericUpdate=function(n) idlePolicyCalls=idlePolicyCalls+1; local o=SDGO_BattleAI_Observe(); lastIdleBoost=o.boostFraction; assert(o.valid==1); return 123 end");
        unit.Think();g_skipNativeDispatch=true;unit.Think();unit.Think();
        Check(Eval("assert(idlePolicyCalls==3)"),"an established enhanced policy keeps receiving one decision on free native frames without dispatch_ai");
        g_skipNativeDispatch=false;unit.Think();
        Check(Eval("assert(idlePolicyCalls==4)"),"a native dispatch already made during Think is not followed by a duplicate policy decision");
        g_skipNativeDispatch=true;
        native_world::Put(staleFsm,8,staleFsm+0x5C);unit.Think();native_world::Put(staleFsm,8,uint32_t(0));
        native_world::Put(defenceMovement,4,defenceMovement+0xE8);unit.Think();
        native_world::Put(defenceMovement,4,defenceMovement+0x124);unit.Think();
        native_world::Put(defenceMovement,4,defenceMovement+0x20);
        native_world::Put(defenceMovement,8,defenceMovement+0x124);unit.Think();native_world::Put(defenceMovement,8,uint32_t(0));
        native_world::Put(idleControl.data(),4,idleControl.data()+0x98);unit.Think();
        native_world::Put(idleControl.data(),4,idleControl.data()+0x20);
        native_world::Put(idleControl.data(),8,idleControl.data()+0x98);unit.Think();native_world::Put(idleControl.data(),8,uint32_t(0));
        unit.bytes[0x30F0]=0;unit.Think();unit.bytes[0x30F0]='V';
        Check(Eval("assert(idlePolicyCalls==4)"),"continuous neutral decisions preserve pending attacks and native hurt, map, control and script admission gates");
        native_world::Put(unit.bytes,0xE90,uint32_t(0x10));
        Check(reinterpret_cast<native_world::Callbacks*>(&unit)->EffectActive(5)&&native_world::NpcBoostPauseQuery(unit.bytes),
              "effect queries outside the owned NPC Think keep the real Boost Down status");
        const int boostPauseStops=native_world::stopMotorCalls;
        unit.Think();
        Check(Eval("assert(idlePolicyCalls==5 and lastIdleBoost==0)")&&*reinterpret_cast<uint32_t*>(unit.bytes+0xE90)==0x10&&
              native_world::stopMotorCalls==boostPauseStops,
              "Boost Down keeps its status bit and native zero fuel while the enhanced controller continues a free frame");
        native_world::Put(unit.bytes,0xE90,uint32_t(0));native_world::Put(unit.bytes,0xEA0,uint32_t(0x10));unit.Think();
        Check(Eval("assert(idlePolicyCalls==6)"),"pending Boost Down does not impose the stock NPC-only decision pause");
        native_world::Put(unit.bytes,0xEA0,uint32_t(0));
        native_world::Put(unit.bytes,0xE90,uint32_t(0x20));unit.Think();native_world::Put(unit.bytes,0xE90,uint32_t(0));
        Check(Eval("assert(idlePolicyCalls==7)"),"the native one-based effect-5 gate does not block an unrelated effect bit");
        unit.Init(reinterpret_cast<void*>(5));unit.Think();
        Check(Eval("assert(idlePolicyCalls==7)"),"a new generation waits for its first native policy callback before continuous neutral decisions begin");
        g_skipNativeDispatch=false;
        native_world::Initialize(unit.bytes,g_L);
        native_world::Put(native_world::document.data(),0x580,int32_t(0));
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");unit.Init(reinterpret_cast<void*>(6));
        Eval("forceGeometryRoute=0; SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.valid==1); assert(SDGO_BattleAI_Route(1,forceGeometryRoute,400,0,0)==1); return 123 end");
        unit.Think();const int cachedGeometryGoals=native_world::goalCalls;
        unit.Think();
        Check(native_world::goalCalls==cachedGeometryGoals,"an unchanged completed geometry plan retains its issued native goal");
        Eval("forceGeometryRoute=1");unit.Think();
        Check(native_world::goalCalls>cachedGeometryGoals,"an explicit recovery repath rebuilds the completed geometry plan instead of recycling the blocked waypoint");
        if(medium) {
            native_world::Initialize(unit.bytes,g_L);
            native_world::SetRoomTitle(L"!ai medium");
            auto fsm=native_world::fsms[0].data();
            native_world::Put(fsm,4,fsm+0x20);
            native_world::Put(fsm,8,uint32_t(0));
            native_world::Put(fsm,0x200,uint32_t(2));
            native_world::Put(fsm,0x240,int32_t(1));
            native_world::Put(fsm,0x248,native_world::weapons[1].data());
            Check(Eval(
                "SDGO_BattleAI_GenericUpdate=function(n) "
                "n:SetAttackOnOff(true); SDGO_BattleAI_Cadence(0); return 123 end"),
                "isolated Medium pacing fixture continuously requests a ready ordinary attack");
            unit.Init(reinterpret_cast<void*>(7));
            unit.Think();
            Check(unit.bytes[0xDCD]!=0,
                  "Medium pacing admits the first ordinary request of a fresh generation");
            native_world::acceptOrdinaryAdmission=false;
            reinterpret_cast<native_world::Callbacks*>(fsm)->AttackAdmission();
            unit.Think();
            Check(unit.bytes[0xDCD]!=0,
                  "a native refusal adds no artificial interval before the next legal attack attempt");
            native_world::acceptOrdinaryAdmission=true;
            reinterpret_cast<native_world::Callbacks*>(fsm)->AttackAdmission();
            Check(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x34,
                  "only the original controller queues the successfully admitted ranged action");
            native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,uint32_t(0));
            unit.Think();
            Check(unit.bytes[0xDCD]==0,
                  "Medium pacing suppresses a fresh request inside 600ms of native admission");
            WaitNativeTicks(620);
            unit.Think();
            Check(unit.bytes[0xDCD]!=0,
                  "Medium pacing resumes the ordinary request after the published 600ms interval");
            const auto rangeType=native_world::weapons[1][0x6A];
            native_world::weapons[1][0x6A]=2;
            reinterpret_cast<native_world::Callbacks*>(fsm)->AttackAdmission();
            native_world::Put(fsm,4,fsm+0x20);native_world::Put(fsm,8,uint32_t(0));unit.Think();
            Check(unit.bytes[0xDCD]==0,
                  "ordinary range-type2 shots use attack style instead of the range field for pacing");
            native_world::weapons[1][0x6A]=rangeType;native_world::acceptOrdinaryAdmission=false;
        }
    }
    std::printf("NATIVE ADAPTER CHECKS %s\n",g_failures?"FAILED":"PASSED");
    // Mods live for this process; do not unload an installed trampoline.
    return g_failures?1:0;
}
