#include "ai_test_support.h"
#include "../mods/coop_ai/coop_ai.cpp"
#include <algorithm>
#include <array>
#include <map>

namespace fixture {
using ai_test::Put;
std::map<std::string, int> settings;
int Setting(SdgoMod*, const char* key, int def) { auto i = settings.find(key); return i == settings.end() ? def : i->second; }
uint32_t StringSetting(SdgoMod*, const char*, const char*, char* value, uint32_t size) {
    strcpy_s(value, size, "100,101"); return 7;
}
std::vector<uint8_t> room(0x33000), lobby(0x100), lobbyProc(0x60), npcList(0x40);
std::array<uint8_t, 16> sentinel{}, first{}, second{};
std::array<uint8_t, 0x40> missionRules{};
uint8_t* listPointer = nullptr;
uint32_t npc1 = 100, nextId = 0x80000010u;
int added = 0, released = 0, started = 0;
int respawns = 0, refreshes = 0, stateRespawns = 0;
uint32_t lifeLimit = 3;
bool revive = true, throws = false;
int __fastcall Respawn(uint8_t*, void*, uint32_t now, uint8_t* pilot, uint32_t, uint32_t) {
    ++respawns;
    if (throws) RaiseException(0xE0000001, 0, 0, nullptr);
    if (now < ai_test::Get<uint32_t>(pilot, 0x34)) return 0;
    if (!InfiniteMissionPilot(pilot) && ai_test::Get<uint32_t>(pilot, 0x1A) >= lifeLimit) return 0;
    if (revive) Put(pilot + kPilotUnits, 0x34, uint32_t{9000});
    return 1;
}
void __fastcall Refresh(uint8_t*, void*, int, uint8_t*, int, int) { ++refreshes; }
int __fastcall StateRespawn(uint8_t*, void*) { ++stateRespawns; return 1; }
void __fastcall Part(uint8_t*, void*) {}
void __fastcall Id(uint8_t*, void*, uint32_t) {}
void __fastcall Room(uint8_t*, void*, uint8_t*) {}
void __fastcall Mode(uint8_t*, void*, uint8_t, uint8_t) {}
int __fastcall Build(uint8_t*, void*, uint8_t* pilot, uint8_t* unit, uint32_t npc) {
    Put(pilot, 0, nextId++); Put(unit, 0, npc); return 1;
}
int __fastcall Setup(void*, void*, uint8_t*, uint8_t*, uint8_t, uint8_t) { ++added; return 1; }
int __fastcall Map(void*, void*, const uint32_t*, int* index) { *index = 1; return 1; }
uint8_t* __fastcall Lobby(void*, void*, int, int) { return lobby.data(); }
void __fastcall Release(void*, void*, int) { ++released; }
void __fastcall Start(void*, void*, uint8_t*, uint32_t, int, uint32_t, uint32_t) { ++started; }
void Reset() {
    std::fill(room.begin(), room.end(), uint8_t{}); std::fill(lobby.begin(), lobby.end(), uint8_t{});
    g_settings = Settings{}; g_settings.levels[2] = {100, 101}; g_random = 123;
    g_api = &ai_test::api; ai_test::api.SettingInt = &Setting; ai_test::api.SettingString = &StringSetting;
    g_buildAiPilot = reinterpret_cast<BuildAiPilotFn>(&Build); g_setupAiPilot = reinterpret_cast<SetupAiPilotFn>(&Setup);
    g_pilotReset = reinterpret_cast<PilotResetFn>(&Part); g_pilotStateId = reinterpret_cast<PilotStateIdFn>(&Id);
    g_pilotStateRoom = reinterpret_cast<PilotStateRoomFn>(&Room); g_pilotStateMode = reinterpret_cast<PilotStateModeFn>(&Mode);
    g_lobbyMapFind = reinterpret_cast<MapFindFn>(&Map); g_lobbyRoom = reinterpret_cast<LobbyRoomFn>(&Lobby);
    g_lobbyRelease = reinterpret_cast<LobbyReleaseFn>(&Release); g_startNotify = reinterpret_cast<StartNotifyFn>(&Start);
    g_lobbyProc = lobbyProc.data(); g_aiNpcList = &listPointer;
    room[kRoomMode] = 5; SlotPilotId(room.data(), 0) = 7;
    missionRules.fill(0); Put(room.data(), 0x316A0, missionRules.data()); Put(missionRules.data(), 0x28, uint32_t{3});
    Put(room.data()+kPilots, 0, uint32_t{7}); Put(room.data()+kPilots+kPilotUnits, 0x34, uint32_t{9000});
    lobby[kLobbyTitleLength] = 3; std::memcpy(lobby.data() + kLobbyTitle, "!ai", 3);
    added = released = started = 0; settings.clear();
    g_lives.clear(); g_infinitePilot = nullptr;
    g_respawnPilot = reinterpret_cast<RespawnPilotFn>(&Respawn);
    g_respawnRefresh = reinterpret_cast<RespawnRefreshFn>(&Refresh);
    g_pilotStateRespawn = reinterpret_cast<PilotStateRespawnFn>(&StateRespawn);
    respawns = refreshes = stateRespawns = 0; lifeLimit = 3; revive = true; throws = false;
}
}

__declspec(naked) int NativeDefeatResult() { __asm {
    mov eax, 10h
    adc eax, 0
    mov esp, ebp
    pop ebp
    ret
}}
__declspec(naked) int UnlimitedLifeResult() { __asm {
    mov eax, 20h
    adc eax, 0
    mov esp, ebp
    pop ebp
    ret
}}
__declspec(naked) int HumanDefeatResult() { __asm {
    mov eax, 30h
    adc eax, 0
    mov esp, ebp
    pop ebp
    ret
}}
__declspec(naked) int CheckMissionDefeatGate(uint8_t*) { __asm {
    push ebp
    mov ebp, esp
    sub esp, 480h
    mov eax, [ebp+8]
    mov [ebp-480h], eax
    stc
    jmp MissionDefeatGate
}}

int main() {
    using namespace fixture;
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data() + kLobbyMapLock));
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(npcList.data() + 0x18));
    Put(npcList.data(), 4, first.data()); Put(npcList.data(), 8, sentinel.data()); Put<int>(npcList.data(), 12, 2);
    Put(first.data(), 8, second.data()); Put(second.data(), 0, &npc1);
    // The native list advances once before reading each payload.
    Put(second.data(), 8, sentinel.data()); listPointer = npcList.data();
    Reset();
    SCENARIO("co-op command semantics");
    CHECK(!ParseTitle("mission").enabled);
    auto p = ParseTitle("!AI3 LV4"); CHECK(p.enabled && p.count == 3 && p.level == 4);
    p = ParseTitle("!ai"); CHECK(p.enabled && p.count == 0 && p.level == 2);
    g_settings.requireRoomCommand = 0; CHECK(ParseTitle("mission").enabled);
    p = ParseTitle("!ai4294967296"); CHECK(p.count == kSlots);
    g_settings.requireRoomCommand=1;
    for(const char* title:{"!rand !ai3 lv4","!ai3 lv4 !rand","!random !AI3 LV4"}) {
        p=ParseTitle(title);CHECK(p.enabled && p.count==3 && p.level==4);
    }
    p=ParseTitle("!ai !rand lv5");CHECK(p.enabled && p.count==0 && p.level==2);
    p=ParseTitle("!ai2 !future lv5");CHECK(p.enabled && p.count==2 && p.level==2);
    p=ParseTitle("!rand !ai0");CHECK(p.enabled && p.count==0);
    for(const char* title:{"!aim","prefix!ai3","!ai3x","!ai!rand"}) CHECK(!ParseTitle(title).enabled);
    auto parserApi=ai_test::api;parserApi.FindRoomCommand=&sdgo::room_commands::Parse;
    g_api=&parserApi;p=ParseTitle("!rand !AI3 LV4");CHECK(p.enabled && p.count==3 && p.level==4);
    g_api=&ai_test::api;
    SCENARIO("mission respawn policy is scoped to the AI argument span and defaults to player");
    CHECK(!ParseTitle("!ai3 lv4").infinite);
    CHECK(ParseTitle("!AI3 INF LV4").infinite);
    CHECK(ParseTitle("!ai3 infinite").infinite);
    CHECK(!ParseTitle("!ai3 inf player").infinite);
    CHECK(ParseTitle("!ai3 player inf").infinite);
    CHECK(!ParseTitle("!ai3 !rand inf").infinite);
    CHECK(!ParseTitle("inf !ai3").infinite);
    CHECK(!ParseTitle("!ai3 infinity").infinite);
    CHECK(ParseIds("4294967296,100") == (std::vector<uint32_t>{100}));
    SCENARIO("invalid configuration cannot index outside level pools");
    settings["DefaultLevel"] = 99; settings["SquadSize"] = 999; settings["DefaultCount"] = -1;
    LoadSettings(); CHECK(g_settings.defaultLevel == 2); CHECK(g_settings.squadSize == 4); CHECK(g_settings.defaultCount == 0);
    settings["DefaultLevel"] = 5; settings["SquadSize"] = 12; settings["DefaultCount"] = 2;
    LoadSettings(); CHECK(g_settings.defaultLevel == 5 && g_settings.squadSize == 12 && g_settings.defaultCount == 2);
    SCENARIO("wingmen fill only free squad slots and join the human team");
    Reset(); CHECK(AiNpcIds() == (std::vector<uint32_t>{100}));
    StartNotify(nullptr, nullptr, room.data(), 0, 0, 0, 0);
    CHECK(added == 3 && started == 1 && released == 1);
    for (int i = 1; i <= 3; ++i) {
        CHECK(SlotPilotId(room.data(), i) & 0x80000000u);
        CHECK(room[kSlotInfo + i * kSlotInfoStride + kSlotInfoTeamA] == 1);
    }
    StartNotify(nullptr, nullptr, room.data(), 0, 0, 0, 0);
    CHECK(added == 3 && g_lives.size() == 1 && g_lives[room.data()].pilots[1] == SlotPilotId(room.data(), 1));
    AddWingmen(room.data()); CHECK(added == 3);
    SCENARIO("combined titles still add wingmen and preserve the human slot");
    for(const char* title:{"!rand !ai2","!ai2 !rand"}) {
        Reset();
        const size_t length=std::strlen(title);lobby[kLobbyTitleLength]=static_cast<uint8_t>(length);
        std::memcpy(lobby.data()+kLobbyTitle,title,length);
        StartNotify(nullptr,nullptr,room.data(),0,0,0,0);
        CHECK(added==2 && started==1 && SlotPilotId(room.data(),0)==7);
    }
    SCENARIO("explicit count and no-command mode isolation");
    Reset(); lobby[kLobbyTitleLength] = 4; std::memcpy(lobby.data() + kLobbyTitle, "!ai1", 4);
    AddWingmen(room.data()); CHECK(added == 1);
    Reset(); lobby[kLobbyTitleLength] = 4; std::memcpy(lobby.data() + kLobbyTitle, "test", 4);
    AddWingmen(room.data()); CHECK(added == 0);
    Reset(); room[kRoomMode] = 1; AddWingmen(room.data()); CHECK(added == 0);
    Reset(); g_settings.levels[2] = {999}; AddWingmen(room.data()); CHECK(added == 0);
    SCENARIO("default wingmen use the native mission quota and respawn delay without changing humans");
    Reset(); AddWingmen(room.data());
    g_missionDefeatOriginal=reinterpret_cast<void*>(&NativeDefeatResult);
    g_missionDefeatContinue=reinterpret_cast<void*>(&UnlimitedLifeResult);
    g_missionDefeatExhausted=reinterpret_cast<void*>(&HumanDefeatResult);
    CHECK(HumanMissionState(room.data())==1 && CheckMissionDefeatGate(room.data())==0x21);
    auto pilot = room.data() + kPilots + kPilotStride;
    Put(pilot, 0x1A, uint32_t{1}); Put(pilot, 0x34, uint32_t{100});
    const auto human = std::vector<uint8_t>(room.begin() + kPilots, room.begin() + kPilots + kPilotStride);
    MissionRespawnTick(room.data(), 99); CHECK(refreshes == 2 && ai_test::Get<uint32_t>(pilot + kPilotUnits, 0x34) == 0);
    MissionRespawnTick(room.data(), 100); CHECK(refreshes == 3 && stateRespawns == 3);
    Put(pilot + kPilotUnits, 0x34, uint32_t{0}); Put(pilot, 0x1A, lifeLimit);
    MissionRespawnTick(room.data(), 101); CHECK(refreshes == 3 && ai_test::Get<uint32_t>(pilot, 0x1A) == lifeLimit);
    CHECK(std::equal(human.begin(), human.end(), room.begin() + kPilots));
    SCENARIO("infinite wingmen respawn beyond the quota while retaining native death statistics");
    Reset(); const char* infinite = "!ai1 inf"; lobby[kLobbyTitleLength] = 8;
    std::memcpy(lobby.data() + kLobbyTitle, infinite, 8); AddWingmen(room.data());
    pilot = room.data() + kPilots + kPilotStride;
    CHECK(HumanMissionState(room.data())==1 && CheckMissionDefeatGate(room.data())==0x21);
    for (uint32_t deaths : {3u, 4u, 1000u}) {
        Put(pilot, 0x1A, deaths); Put(pilot + kPilotUnits, 0x34, uint32_t{0});
        MissionRespawnTick(room.data(), 100);
        CHECK(ai_test::Get<uint32_t>(pilot + kPilotUnits, 0x34) == 9000);
        CHECK(ai_test::Get<uint32_t>(pilot, 0x1A) == deaths && !g_infinitePilot);
    }
    CHECK(refreshes == 3 && stateRespawns == 3);
    revive = false; Put(pilot + kPilotUnits, 0x34, uint32_t{0});
    MissionRespawnTick(room.data(), 101); CHECK(refreshes == 3);
    throws = true; MissionRespawnTick(room.data(), 102); CHECK(!g_infinitePilot && refreshes == 3);
    throws = false; revive = true; SlotPilotId(room.data(), 1)++;
    CHECK(HumanMissionState(room.data())==-1 && CheckMissionDefeatGate(room.data())==0x11);
    const int prior = respawns; MissionRespawnTick(room.data(), 103); CHECK(respawns == prior);
    SlotPilotId(room.data(), 1)--; room[kRoomMode] = 1;
    MissionRespawnTick(room.data(), 104); CHECK(respawns == prior);
    room[kRoomMode] = 5; MissionEnded(room.data());
    MissionRespawnTick(room.data(), 105); CHECK(respawns == prior && g_lives.empty());
    CHECK(SlotPilotId(room.data(), 1) == 0 && SlotPilotId(room.data(), 0) == 7);
    SCENARIO("AI revival timing is independent; only human life quota controls mission defeat");
    Reset(); AddWingmen(room.data());
    auto humanPilot=room.data()+kPilots;
    Put(humanPilot+kPilotUnits,0x34,uint32_t{0}); Put(humanPilot,0x1A,uint32_t{1});
    CHECK(HumanMissionState(room.data())==1 && CheckMissionDefeatGate(room.data())==0x21);
    MissionRespawnTick(room.data(),100); CHECK(respawns==3 && refreshes==3);
    Put(humanPilot+kPilotUnits,0x34,uint32_t{9000});
    MissionRespawnTick(room.data(),101); CHECK(respawns==3 && refreshes==3);
    Put(humanPilot+kPilotUnits,0x34,uint32_t{0}); Put(humanPilot,0x1A,uint32_t{3});
    CHECK(HumanMissionState(room.data())==0 && CheckMissionDefeatGate(room.data())==0x31);
    g_lives[room.data()].infinite=true;
    CHECK(HumanMissionState(room.data())==0 && CheckMissionDefeatGate(room.data())==0x31);
    CHECK(ai_test::Get<uint32_t>(room.data()+kPilots+kPilotStride+kPilotUnits,0x34)==9000);
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data() + kLobbyMapLock));
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(npcList.data() + 0x18));
    return ai_test::Finish();
}
