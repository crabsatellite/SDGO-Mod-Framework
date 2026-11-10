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
uint32_t extraFormNpcId = 0, extraFormUnitId = 0;
uint8_t builtForm = 0;
uint32_t builtMaximum = 15000, builtCurrent = 0;
std::vector<uint32_t> aceEligibleIds = {10001, 10002};
std::vector<uint32_t> aceEligibilityInputs;
int aceEligibilityCalls = 0;
int added = 0, released = 0, started = 0;
int respawns = 0, refreshes = 0, stateRespawns = 0;
uint32_t proposedDamageSeen = 0, attackTypeSeen = 0;
uint32_t lifeLimit = 3;
bool revive = true, throws = false;
uint32_t respawnMaximum = 0, respawnCurrent = 9000;
int __fastcall Respawn(uint8_t*, void*, uint32_t now, uint8_t* pilot, uint32_t, uint32_t) {
    ++respawns;
    if (throws) RaiseException(0xE0000001, 0, 0, nullptr);
    if (now < ai_test::Get<uint32_t>(pilot, 0x34)) return 0;
    if (!InfiniteMissionPilot(pilot) && ai_test::Get<uint32_t>(pilot, 0x1A) >= lifeLimit) return 0;
    if (revive) {
        auto* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
        if (respawnMaximum) Put(unit, kUnitMaxHp, respawnMaximum);
        Put(unit, kUnitHp, respawnCurrent);
    }
    return 1;
}
void __fastcall Refresh(uint8_t*, void*, int, uint8_t*, int, int) { ++refreshes; }
int __fastcall StateRespawn(uint8_t*, void*) { ++stateRespawns; return 1; }
void __fastcall Part(uint8_t*, void*) {}
void __fastcall Id(uint8_t*, void*, uint32_t) {}
void __fastcall Room(uint8_t*, void*, uint8_t*) {}
void __fastcall Mode(uint8_t*, void*, uint8_t, uint8_t) {}
int __fastcall Build(uint8_t*, void*, uint8_t* pilot, uint8_t* unit, uint32_t npc) {
    Put(pilot, 0, nextId++);
    Put(unit, 0, npc);                         // native NPC id
    Put(unit, kUnitDataId, uint32_t{10001});   // machine/unit data id
    Put(unit, kUnitMaxHp, builtMaximum); Put(unit, kUnitHp, builtCurrent);
    pilot[kPilotForm] = builtForm;
    if (extraFormNpcId) {
        auto extra = pilot + kPilotUnits + kUnitStride;
        Put(extra, 0, extraFormNpcId);
        Put(extra, kUnitDataId, extraFormUnitId);
        Put(extra, kUnitMaxHp, builtMaximum); Put(extra, kUnitHp, builtCurrent);
    }
    return 1;
}
int __cdecl AceEligible(uint32_t unitId) {
    ++aceEligibilityCalls; aceEligibilityInputs.push_back(unitId);
    return std::find(aceEligibleIds.begin(), aceEligibleIds.end(), unitId) != aceEligibleIds.end() ? 1 : 0;
}
int __fastcall Setup(void*, void*, uint8_t*, uint8_t*, uint8_t, uint8_t) { ++added; return 1; }
int __fastcall Map(void*, void*, const uint32_t*, int* index) { *index = 1; return 1; }
uint8_t* __fastcall Lobby(void*, void*, int, int) { return lobby.data(); }
void __fastcall Release(void*, void*, int) { ++released; }
void __fastcall Start(void*, void*, uint8_t*, uint32_t, int, uint32_t, uint32_t) { ++started; }
uint32_t __fastcall Damage(
    uint8_t*, void*, uint32_t, uint32_t, uint8_t*, uint8_t*, uint32_t,
    uint32_t, uint32_t proposedDamage, uint16_t, uint8_t, uint32_t,
    uint8_t, uint32_t attackType) {
    proposedDamageSeen = proposedDamage;
    attackTypeSeen = attackType;
    return proposedDamage + 7;
}
void Reset() {
    std::fill(room.begin(), room.end(), uint8_t{}); std::fill(lobby.begin(), lobby.end(), uint8_t{});
    g_settings = Settings{}; g_settings.levels[2] = {100, 101}; g_random = 123;
    nextId = 0x80000010u;
    g_aceSettings = battle_ai::ace::Settings{}; g_aceSettings.enabled = false;
    g_reloadAceSettingsAtBattleEntry = false;
    g_aceServerModule = nullptr; g_aceUnitEligible = &AceEligible;
    aceEligibleIds = {10001, 10002}; aceEligibilityInputs.clear(); aceEligibilityCalls = 0;
    extraFormNpcId = extraFormUnitId = 0; builtForm = 0; builtMaximum = 15000; builtCurrent = 0;
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
    g_pendingAceDamage = {};
    InterlockedExchange(&g_aceDamageSequence, 0);
    g_respawnPilot = reinterpret_cast<RespawnPilotFn>(&Respawn);
    g_respawnRefresh = reinterpret_cast<RespawnRefreshFn>(&Refresh);
    g_pilotStateRespawn = reinterpret_cast<PilotStateRespawnFn>(&StateRespawn);
    g_damage = reinterpret_cast<DamageFn>(&Damage);
    respawns = refreshes = stateRespawns = 0; lifeLimit = 3; revive = true; throws = false;
    proposedDamageSeen = attackTypeSeen = 0;
    respawnMaximum = 0; respawnCurrent = 9000;
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

    SCENARIO("mission Ace selection is friendly-only, directory-gated and stable across duplicate start");
    Reset(); const char* one = "!ai1 hard"; lobby[kLobbyTitleLength] = 9;
    std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2] = {100, battle_ai::Difficulty::Hard, 120, 150};
    builtCurrent = 10000;
    StartNotify(nullptr, nullptr, room.data(), 0, 0, 0, 0);
    auto acePilot = room.data() + kPilots + kPilotStride;
    const uint32_t aceId = SlotPilotId(room.data(), 1);
    CHECK((aceId & (battle_ai::ace::kAiPilotBit | battle_ai::ace::kAcePilotBit)) ==
          (battle_ai::ace::kAiPilotBit | battle_ai::ace::kAcePilotBit));
    CHECK(g_lives[room.data()].ace[1].outcome.ace);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitHp) == 12000);
    SdgoAceAiServerSnapshotV1 missionSnapshot{};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1);
    CHECK(missionSnapshot.mode == 5 && missionSnapshot.generation != 0 &&
          missionSnapshot.unit_count == 1);
    CHECK(missionSnapshot.units[0].pilot_id == aceId &&
          missionSnapshot.units[0].native_pilot_id ==
              ai_test::Get<uint32_t>(acePilot, 0) &&
          missionSnapshot.units[0].npc_id == 100 &&
          missionSnapshot.units[0].machine_id == 10001 &&
          missionSnapshot.units[0].slot == 1 &&
          missionSnapshot.units[0].form == 0 &&
          missionSnapshot.units[0].base_difficulty == 2 &&
          missionSnapshot.units[0].effective_difficulty == 2 &&
          missionSnapshot.units[0].ace == 1 &&
          missionSnapshot.units[0].hp_percent == 120 &&
          missionSnapshot.units[0].attack_percent == 150 &&
          missionSnapshot.units[0].current_hp == 12000 &&
          missionSnapshot.units[0].max_hp == 18000 &&
          missionSnapshot.units[0].active == 1);
    missionSnapshot.size = sizeof(missionSnapshot) - 1;
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 0);
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION + 1;
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 0);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = 0x1234;
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 0);
    CHECK(SdgoCoopAiGetAceSnapshotV1(
              reinterpret_cast<SdgoAceAiServerSnapshotV1*>(1)) == 0);
    CHECK(MissionAcceptedDamage(room.data(), 1, acePilot, 200, 0) == 300);
    CHECK(MissionAcceptedDamage(room.data(), 1, acePilot, 200, 1) == 200);
    CHECK(MissionAcceptedDamage(room.data(), 0, room.data() + kPilots, 200, 0) == 200);
    auto copiedPilot = std::vector<uint8_t>(acePilot, acePilot + kPilotStride);
    CHECK(MissionAcceptedDamage(room.data(), 1, copiedPilot.data(), 200, 0) == 200);
    acePilot[0x04] = 2;
    CHECK(MissionAcceptedDamage(room.data(), 1, acePilot, 200, 0) == 200);
    acePilot[0x04] = kHumanTeam;
    uint8_t weapon[0x40]{};
    CHECK(AcceptedDamage(room.data(), nullptr, 10, 1, acePilot, weapon, 0,
                         0, 200, 0, 0, 0, 0, 0) == 307);
    CHECK(proposedDamageSeen == 300 && attackTypeSeen == 0);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1);
    CHECK(missionSnapshot.damage.sequence != 0 &&
          missionSnapshot.damage.source == SDGO_ACE_AI_SERVER_MISSION &&
          missionSnapshot.damage.attacker_pilot_id == aceId &&
          missionSnapshot.damage.native_pilot_id ==
              ai_test::Get<uint32_t>(acePilot, 0) &&
          missionSnapshot.damage.attack_type == 0 &&
          missionSnapshot.damage.proposed_damage == 200 &&
          missionSnapshot.damage.adjusted_damage == 300 &&
          missionSnapshot.damage.native_result == 307 &&
          missionSnapshot.damage.attack_percent == 150 &&
          missionSnapshot.damage.ace == 1 &&
          missionSnapshot.damage.policy_matched == 1);
    const uint32_t ordinaryMissionReceipt = missionSnapshot.damage.sequence;
    CHECK(AcceptedDamage(room.data(), nullptr, 11, 1, acePilot, weapon, 0,
                         0, 200, 0, 0, 0, 0, 1) == 207);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1);
    CHECK(missionSnapshot.damage.sequence > ordinaryMissionReceipt &&
          missionSnapshot.damage.attack_type == 1 &&
          missionSnapshot.damage.proposed_damage == 200 &&
          missionSnapshot.damage.adjusted_damage == 200 &&
          missionSnapshot.damage.native_result == 207 &&
          missionSnapshot.damage.attack_percent == 150);
    const uint32_t specialMissionReceipt = missionSnapshot.damage.sequence;
    CHECK(AcceptedDamage(
              room.data(), nullptr, 12, 0, room.data() + kPilots, weapon,
              0, 0, 200, 0, 0, 0, 0, 0) == 207);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1 &&
          missionSnapshot.damage.sequence == specialMissionReceipt);
    CHECK(SdgoCoopAiCompleteAceDamageV1(
              room.data(), 1, acePilot, 200, 300, 0, 307) == 0);
    const unsigned randomAfterFirst = g_random;
    StartNotify(nullptr, nullptr, room.data(), 0, 0, 0, 0);
    CHECK(SlotPilotId(room.data(), 1) == aceId);
    CHECK(g_random == randomAfterFirst);
    CHECK(added == 1);

    SCENARIO("zero chance and any missing nonempty form fail closed to ordinary identity");
    Reset(); lobby[kLobbyTitleLength] = 9; std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true; g_aceSettings.byDifficulty[2].chancePercent = 0;
    AddWingmen(room.data());
    CHECK((SlotPilotId(room.data(), 1) & battle_ai::ace::kAcePilotBit) == 0);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1 &&
          missionSnapshot.unit_count == 1 &&
          missionSnapshot.units[0].ace == 0 &&
          missionSnapshot.units[0].base_difficulty == 2 &&
          missionSnapshot.units[0].effective_difficulty == 2 &&
          missionSnapshot.units[0].hp_percent == 100 &&
          missionSnapshot.units[0].attack_percent == 100);
    Reset(); lobby[kLobbyTitleLength] = 9; std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true; g_aceSettings.byDifficulty[2].chancePercent = 100;
    extraFormNpcId = 101; extraFormUnitId = 999; AddWingmen(room.data());
    CHECK((SlotPilotId(room.data(), 1) & battle_ai::ace::kAcePilotBit) == 0);
    CHECK(aceEligibilityCalls == 2);
    CHECK(aceEligibilityInputs == (std::vector<uint32_t>{10001, 999}));
    Reset(); lobby[kLobbyTitleLength] = 9; std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2] = {100, battle_ai::Difficulty::Hard, 120, 100};
    extraFormNpcId = 101; extraFormUnitId = 10002; builtForm = 1; builtCurrent = 10000;
    AddWingmen(room.data());
    auto selectedPilot = room.data() + kPilots + kPilotStride;
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitMaxHp) == 15000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitHp) == 10000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits + kUnitStride, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits + kUnitStride, kUnitHp) == 12000);
    selectedPilot[kPilotForm] = 0;
    Put(selectedPilot + kPilotUnits, kUnitHp, uint32_t{0});
    respawnMaximum = 15000; respawnCurrent = 9000;
    MissionRespawnTick(room.data(), 100);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitHp) == 10800);

    SCENARIO("deck form0 to form1 to form0 applies Ace HP once per form without compounding");
    Reset(); lobby[kLobbyTitleLength] = 9; std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2] = {100, battle_ai::Difficulty::Hard, 120, 100};
    extraFormNpcId = 101; extraFormUnitId = 10002; builtForm = 0; builtCurrent = 10000;
    AddWingmen(room.data()); selectedPilot = room.data() + kPilots + kPilotStride;
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits + kUnitStride, kUnitMaxHp) == 15000);
    selectedPilot[kPilotForm] = 1;
    Put(selectedPilot + kPilotUnits + kUnitStride, kUnitHp, uint32_t{0});
    respawnMaximum = 15000; respawnCurrent = 9000;
    MissionRespawnTick(room.data(), 100);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits + kUnitStride, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits + kUnitStride, kUnitHp) == 10800);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1 &&
          missionSnapshot.unit_count == 1 &&
          missionSnapshot.units[0].form == 1 &&
          missionSnapshot.units[0].npc_id == 101 &&
          missionSnapshot.units[0].machine_id == 10002 &&
          missionSnapshot.units[0].npc_id !=
              missionSnapshot.units[0].machine_id &&
          missionSnapshot.units[0].max_hp == 18000 &&
          missionSnapshot.units[0].current_hp == 10800);
    selectedPilot[kPilotForm] = 0;
    Put(selectedPilot + kPilotUnits, kUnitHp, uint32_t{0});
    respawnMaximum = 18000; respawnCurrent = 17000;
    MissionRespawnTick(room.data(), 101);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(selectedPilot + kPilotUnits, kUnitHp) == 17000);
    Reset(); lobby[kLobbyTitleLength] = 9; std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true; g_aceSettings.byDifficulty[2].chancePercent = 100;
    g_aceUnitEligible = nullptr; AddWingmen(room.data());
    CHECK(SlotPilotId(room.data(), 1) == 0x80000010u);

    SCENARIO("exact native respawn keeps Ace identity and scales only an original HP seed");
    Reset(); lobby[kLobbyTitleLength] = 9; std::memcpy(lobby.data() + kLobbyTitle, one, 9);
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2] = {100, battle_ai::Difficulty::Hard, 120, 150};
    builtCurrent = 10000;
    AddWingmen(room.data()); acePilot = room.data() + kPilots + kPilotStride;
    const uint32_t stableAce = SlotPilotId(room.data(), 1);
    Put(acePilot + kPilotUnits, kUnitHp, uint32_t{0});
    respawnMaximum = 15000; respawnCurrent = 9000;
    MissionRespawnTick(room.data(), 100);
    CHECK(SlotPilotId(room.data(), 1) == stableAce);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitHp) == 10800);
    Put(acePilot + kPilotUnits, kUnitHp, uint32_t{0});
    respawnMaximum = 18000; respawnCurrent = 17000;
    MissionRespawnTick(room.data(), 101);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitMaxHp) == 18000);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitHp) == 17000);
    Put(acePilot + kPilotUnits, kUnitHp, uint32_t{0});
    respawnMaximum = 16000; respawnCurrent = 9000;
    MissionRespawnTick(room.data(), 102);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitMaxHp) == 16000);
    CHECK(ai_test::Get<uint32_t>(acePilot + kPilotUnits, kUnitHp) == 9000);

    SCENARIO("mission end permits a fresh battle roll and slot generation without touching humans");
    const uint32_t priorGeneration = g_lives[room.data()].generation;
    MissionEnded(room.data());
    CHECK(SlotPilotId(room.data(), 0) == 7 && SlotPilotId(room.data(), 1) == 0);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 0);
    AddWingmen(room.data());
    CHECK(g_lives[room.data()].generation != 0 &&
          g_lives[room.data()].generation != priorGeneration);
    missionSnapshot = {};
    missionSnapshot.size = sizeof(missionSnapshot);
    missionSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    missionSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoCoopAiGetAceSnapshotV1(&missionSnapshot) == 1 &&
          missionSnapshot.generation == g_lives[room.data()].generation &&
          missionSnapshot.generation != priorGeneration);

    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data() + kLobbyMapLock));
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(npcList.data() + 0x18));
    return ai_test::Finish();
}
