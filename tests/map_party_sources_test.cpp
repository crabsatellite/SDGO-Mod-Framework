// Exercise the actual Lua callback against bounded native-layout fixtures.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct lua_State { std::vector<double> values; };
enum { SDGO_LOG_INFO, SDGO_LOG_WARN };
struct Api { void (*Log)(void*, int, const char*, ...); };
void Log(void*, int, const char*, ...) {}
Api api{Log};
Api* g_api = &api;
void* g_mod = nullptr;
uint8_t fixtureApp[0x500]{}, fixtureService[0x20]{}, fixtureMode[0x1000]{}, fixtureRoom[0x100]{};
uint8_t records[12][0x344]{}, aiRecords[12][0x344]{};
uint8_t units[16][0x3300]{}, definitions[16][16]{};
void* fixtureDocument = &fixtureRoom;
void** g_unitManager = &fixtureDocument;
void* g_activeMissionMode = fixtureMode;
constexpr size_t kModeRoom=8, kRoomHumansBegin=0x1C, kRoomHumansEnd=0x20;
constexpr size_t kRoomLocalPlayer=0x68, kHumanRecordBytes=0x344;
uint32_t U32(const void* p) { uint32_t v; std::memcpy(&v,p,4); return v; }
template<class T> void Put(void* p, T value) { std::memcpy(p,&value,sizeof(value)); }
uint8_t* GameApp() { return fixtureApp; }
auto g_gameApp = GameApp;
uint32_t ObjectForPilot(void*, uint32_t pilot) {
    for(unsigned i=0;i<16;++i) if(U32(units[i]+0x2B68)==pilot) return U32(units[i]+4);
    return 0;
}
auto g_objectForPilot = ObjectForPilot;
uint8_t* FindObject(void*, uint32_t id) {
    for(unsigned i=0;i<16;++i) if(U32(units[i]+4)==id) return units[i];
    return nullptr;
}
auto g_findObject = FindObject;
void* Data() { return fixtureApp; }
auto g_data = Data;
bool Find(void*, const uint8_t** out, uint32_t id, unsigned, unsigned) {
    for(unsigned i=0;i<16;++i) if(id && U32(definitions[i])==id) { *out=definitions[i]; return true; }
    return false;
}
auto g_find = Find;
void PushNumber(lua_State* L,double value) { L->values.push_back(value); }
auto g_luaPushNumber = PushNumber;
#include "../mods/map_catalog/party_sources.inl"

void Check(bool ok,const char* message) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
void Unit(unsigned index, uint32_t pilot, uint32_t npc=0, int hp=100) {
    auto* unit=units[index];
    std::memset(unit,0,sizeof(units[index]));
    Put(unit+4,index+100u); Put(unit+0x2B68,pilot); Put(unit+0x20,1u); Put(unit+0x38,hp);
    Put(definitions[index],npc); Put(unit+0x320C,definitions[index]);
}
int main() {
    lua_State clock;
    const auto beforeClock = GetTickCount64();
    Check(LuaMissionClockMs(&clock) == 1 && clock.values.size() == 1,
          "the real mission clock callback returns exactly one Lua number");
    Check(clock.values[0] >= static_cast<double>(beforeClock) &&
          clock.values[0] <= static_cast<double>(GetTickCount64()),
          "party timing uses monotonic milliseconds independent of update count");
    Put(fixtureApp+0x4A0,22u); Put(fixtureApp+0x4A4,fixtureService); Put(fixtureService+0x0C,fixtureMode); Put(fixtureMode+8,fixtureRoom);
    Put(fixtureRoom+0x68,401u); Put(fixtureRoom+0x1C,records[0]); Put(fixtureRoom+0x20,records[1]); Put(records[0],401u);
    Put(fixtureRoom+0x3C,aiRecords[0]); Put(fixtureRoom+0x40,aiRecords[3]);
    Unit(0,401); Unit(1,0x80000001u,8101); Unit(2,0x80000002u,8102); Unit(3,0x80000003u,8103);
    for(unsigned i=0;i<3;++i) Put(aiRecords[i],0x80000001u+i);
    Unit(4,9000,9200); // A live enemy not present in either participant vector.
    lua_State L;
    Check(LuaPartySources(&L)==8,"one human and three separate fixtureRoom AI records are included");
    Check(L.values==std::vector<double>({100,0,101,8101,102,8102,103,8103}),
          "humans retain runtime appearance and wingmen retain NPC template identities");
    L.values.clear(); Put(units[2]+0x38,0);
    Check(LuaPartySources(&L)==6,"dead wingmen are excluded");
    L.values.clear(); Put(aiRecords[2],0x80000001u);
    Check(LuaPartySources(&L)==4,"duplicate native fixtureRoom records cannot create duplicate movie actors");
    L.values.clear(); Put(fixtureRoom+0x40,aiRecords[2]+1);
    Check(LuaPartySources(&L)==0 && L.values.empty(),"malformed AI vector fails before any Lua output");
    Put(fixtureRoom+0x3C,static_cast<uint8_t*>(nullptr)); Put(fixtureRoom+0x40,static_cast<uint8_t*>(nullptr));
    Check(LuaPartySources(&L)==2,"empty AI vector keeps the ordinary human-only movie available");
    L.values.clear(); Put(fixtureRoom+0x3C,aiRecords[0]); Put(fixtureRoom+0x40,aiRecords[8]);
    for(unsigned i=0;i<8;++i) { Unit(i+1,0x80000001u+i,8101+i); Put(aiRecords[i],0x80000001u+i); }
    Check(LuaPartySources(&L)==0 && L.values.empty(),"party capacity overflow is rejected atomically");
    std::puts("PASS actual party-source callback: human/AI separation, identity and bounded vectors");
}
