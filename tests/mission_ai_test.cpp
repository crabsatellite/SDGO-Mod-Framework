#include "ai_test_support.h"
#include "../mods/mission_ai/mission_ai.cpp"
#include <array>

namespace fixture {
using ai_test::Put;
std::array<uint8_t, 0x640> mode{};
std::array<uint8_t, 0x640> derivedMode{};
std::array<uint8_t, 0x80> room{};
std::array<uint8_t, 12 * kRecordSize> humans{}, ais{};
std::array<std::array<uint8_t, 0x3300>, 12> units{};
std::array<std::array<uint8_t, 0x180>, 12> npcDefinitions{};
std::array<std::array<uint8_t, 0x100>, 12> playerSlots{};
std::array<std::wstring, 12> slotNames{};
std::array<uint8_t, 0x40> start{};
std::array<uint8_t, 0x500> app{};
std::array<uint8_t, 0x120> service{};
std::array<ModeVirtualFn, 8> vtable{};
int manager = 0;
void* um = &manager;
int created = 0, remote = 0, assigned = 0, calculated = 0, initialized = 0;
bool observer = false;
std::vector<int> slots;
std::vector<uint32_t> beginGenerations;
struct ProfileCall { uint8_t* unit; int flags; uint32_t generation; };
std::vector<ProfileCall> profileCalls;
std::vector<std::string> profileEvents;
int aceNameCalls = 0;
bool appendSuccess = true;
int lineCtors = 0, lineDtors = 0, appendCalls = 0;
std::wstring appendedText;

uint8_t* __cdecl App() { return app.data(); }
bool __cdecl Network() { return true; }
bool __fastcall Observer(void*, void*) { return observer; }
int __fastcall Virtual(uint8_t*, void*) { return 1; }
uint8_t* __fastcall Start(void*, void*, int, int) { return start.data(); }
void WriteNpc(int index, uint32_t npcId, uint32_t unitId, const wchar_t* name) {
    auto& npc = npcDefinitions[index]; npc.fill(0);
    Put(npc.data(), 0, npcId);
    std::memcpy(npc.data() + 5, name, (wcslen(name) + 1) * sizeof(wchar_t));
    Put(npc.data(), 0x69, unitId);
}
uint8_t* __fastcall Create(uint8_t*, void*, uint32_t npcId, float, float, float, int team, float, float, float) {
    if (created >= 12) return nullptr;
    const int index = created;
    auto* unit = units[index].data();
    Put(unit, kUnitObjectId, 1001u + index);
    Put(unit, kUnitTeam, static_cast<uint32_t>(team));
    Put(unit, kUnitNpcData, npcDefinitions[index].data());
    Put(unit, kUnitNpcDataId, ai_test::Get<uint32_t>(npcDefinitions[index].data(), 0x69));
    CHECK(ai_test::Get<uint32_t>(npcDefinitions[index].data(), 0) == npcId);
    ++created;
    return unit;
}
uint8_t* __fastcall Remote(uint8_t* m, void*, uint32_t npc, float x, float y, float z, int team, float dx, float dy, float dz) {
    ++remote; return Create(m, nullptr, npc, x, y, z, team, dx, dy, dz);
}
void __fastcall Info(void*, void*, int slot, int, uint32_t, uint32_t, uint8_t*) {
    CHECK(slot >= 0 && slot < 12); slots.push_back(slot); ++assigned;
}
uint8_t* __fastcall Player(void*, void*, int slot) {
    return slot >= 0 && slot < 12 ? playerSlots[slot].data() : nullptr;
}
void __fastcall Assign(void* target, void*, const wchar_t* text, size_t length) {
    for (int slot = 0; slot < 12; ++slot) {
        if (target == playerSlots[slot].data() + kSlotName) {
            slotNames[slot].assign(text, length);
            return;
        }
    }
    CHECK(false);
}
void __fastcall Add(void*, void*, uint8_t*) {}
void __fastcall Calculate(void*, void*) { ++calculated; }
void* __fastcall Find(void*, void*, uint32_t object) {
    for (int i = 0; i < created; ++i)
        if (ai_test::Get<uint32_t>(units[i].data(), kUnitObjectId) == object) return units[i].data();
    return nullptr;
}
void __fastcall Init(uint8_t*, void*) { ++initialized; }
void __fastcall Update(void*, void*) {}
int __cdecl MatchTick(uint8_t*) { return 1; }
int __cdecl BeginProfiles(uint32_t generation) {
    beginGenerations.push_back(generation); profileEvents.push_back("begin"); return 1;
}
void __cdecl InitProfile(uint8_t* unit, int flags, uint32_t generation) {
    profileCalls.push_back({unit, flags, generation}); profileEvents.push_back("unit");
}
int __cdecl AceName(uint32_t unitId, wchar_t* out, uint32_t capacity) {
    ++aceNameCalls;
    if (unitId != 70001 || !out || capacity < 4) return 0;
    wcscpy_s(out, capacity, L"王牌甲"); return 1;
}
void __fastcall LineCtor(void*, void*) { ++lineCtors; }
void __fastcall LineDtor(void*, void*) { ++lineDtors; }
void* __fastcall NativeString(void* self, void*, const wchar_t* text, size_t length) {
    appendedText.assign(text, length); return self;
}
void __fastcall Append(void*, void*, const void* record) {
    ++appendCalls;
    const auto* bytes = static_cast<const uint8_t*>(record);
    CHECK(ai_test::Get<uint32_t>(bytes, 0x54) == 1000);
    CHECK(ai_test::Get<uint32_t>(bytes, 0x58) == 0x70D9FF);
    CHECK(ai_test::Get<uint16_t>(bytes, 0x64) == 10);
    if (appendSuccess)
        Put(service.data(), 0x98, ai_test::Get<uint32_t>(service.data(), 0x98) + 1);
}
uint32_t AcePilotId(uint32_t nativeId) {
    battle_ai::ace::Outcome outcome{};
    outcome.ace = true;
    outcome.base = battle_ai::Difficulty::Medium;
    outcome.effective = battle_ai::Difficulty::Hard;
    outcome.hpPercent = 100;
    outcome.attackPercent = 100;
    uint32_t encoded = 0;
    CHECK(battle_ai::ace::EncodePilotId(nativeId, outcome, &encoded));
    return encoded;
}
void SetAceRecord(int index, int team = 1) {
    auto* record = ais.data() + index * kRecordSize;
    Put(record, kRecordServerId, AcePilotId(0x80000010u + index));
    record[kRecordTeam] = static_cast<uint8_t>(team);
}
void Reset(int h, int a, bool master) {
    mode.fill(0); derivedMode.fill(0); room.fill(0); humans.fill(0); ais.fill(0);
    units = {}; npcDefinitions = {}; playerSlots = {}; slotNames = {};
    app.fill(0); service.fill(0); slots.clear(); beginGenerations.clear(); profileCalls.clear(); profileEvents.clear();
    created = remote = assigned = calculated = initialized = 0; observer = false; aceNameCalls = 0;
    appendSuccess = true; lineCtors = lineDtors = appendCalls = 0; appendedText.clear();
    g_api = &ai_test::api;
    vtable.fill(nullptr); vtable[kModeIsNetworkSlot] = reinterpret_cast<ModeVirtualFn>(&Virtual);
    Put(mode.data(), 0, vtable.data()); Put(mode.data(), kModeRoom, room.data());
    Put(derivedMode.data(), 0, static_cast<void*>(nullptr));
    mode[kModeRoomMaster] = master ? 1 : 0;
    Put(room.data(), kRoomHumansBegin, humans.data()); Put(room.data(), kRoomHumansEnd, humans.data() + h * kRecordSize);
    Put(room.data(), kRoomAiBegin, ais.data()); Put(room.data(), kRoomAiEnd, ais.data() + a * kRecordSize);
    for (int i = 0; i < a; ++i) {
        auto* record = ais.data() + i * kRecordSize;
        Put(record, kRecordServerId, 0x80000010u + i);
        Put(record, kRecordNpcData, 5001u + i);
        record[kRecordTeam] = 1;
        WriteNpc(i, 5001u + i, 70001u + i, i == 0 ? L"机体一" : L"机体二");
    }
    g_game = {};
    g_game.unitManager = &um; g_game.isNetwork = &Network;
    g_game.isObserver = reinterpret_cast<IsObserverFn>(&Observer);
    g_game.findStartPoint = reinterpret_cast<FindStartPointFn>(&Start);
    g_game.createNpc = reinterpret_cast<CreateNpcFn>(&Create); g_game.createRemoteNpc = reinterpret_cast<CreateNpcFn>(&Remote);
    g_game.setPlayerInfo = reinterpret_cast<SetPlayerInfoFn>(&Info);
    g_game.playerSlot = reinterpret_cast<PlayerSlotFn>(&Player);
    g_game.assign = reinterpret_cast<WStringAssignFn>(&Assign);
    g_game.addAiUnit = reinterpret_cast<AddAiUnitFn>(&Add); g_game.calculateTeam = reinterpret_cast<CalculateTeamFn>(&Calculate);
    g_game.findObject = reinterpret_cast<FindObjectFn>(&Find);
    g_game.originalInit = reinterpret_cast<ModeInitFn>(&Init); g_game.missionVtable = vtable.data();
    Put(app.data(), 0x4A4, service.data()); Put(service.data(), 0x0C, mode.data());
    Put(service.data(), 0x3C, uint32_t{2}); Put(service.data(), 0x94, reinterpret_cast<void*>(1));
    g_gameApp = &App;
    g_originalUpdate = reinterpret_cast<MissionUpdateFn>(&Update);
    g_matchAiMissionTick = nullptr;
    g_battleAiModule = nullptr; g_battleAiBeginBattle = &BeginProfiles; g_battleAiInitializeUnit = &InitProfile;
    g_aceAiModule = nullptr; g_acePilotName = &AceName;
    g_lineCtor = reinterpret_cast<NativeStepFn>(&LineCtor);
    g_lineDtor = reinterpret_cast<NativeStepFn>(&LineDtor);
    g_nativeStringCtor = reinterpret_cast<NativeStringCtorFn>(&NativeString);
    g_appendLine = reinterpret_cast<AppendLineFn>(&Append);
    g_nextProfileGeneration = 0; g_profileBattleBegun = false;
    g_missionAcePending = false; g_missionAceAnnounced = false; g_missionReadyReleased = false;
    g_pendingRespawnObject = 0; g_observedWingmen.clear(); g_observedMode = nullptr;
    g_wingmen.clear(); g_localWingmen.clear(); g_disabled = false;
}
}

int main() {
    using namespace fixture;
    g_api = &ai_test::api;
    SCENARIO("configuration values remain data when converted to Lua");
    CHECK(Trim(" \t hi\r ") == "hi"); CHECK(Trim(" \t\r").empty());
    CHECK(IsInteger("-42") && IsInteger("0") && !IsInteger("-") && !IsInteger("1e9"));
    CHECK(!IsInteger("1; error('injected')") && !IsInteger("9999999999"));
    CHECK(IntegerList("1, -2, 3") == "{ 1, -2, 3 }");
    CHECK(IntegerList("1,,3").empty() && IntegerList("1,2,").empty());
    CHECK(LuaValue("-42") == "-42"); CHECK(LuaValue("1,2") == "{ 1, 2 }");
    CHECK(LuaValue("1,boom()") == "\"1,boom()\"");
    CHECK(LuaValue("a\"\\b\n") == "\"a\\\"\\\\b\"");
    CHECK(LuaValue("") == "\"\"");
    SCENARIO("INI settings become escaped Lua fields, invalid keys are ignored");
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, dir) > 0);
    CHECK(GetTempFileNameW(dir, L"sdt", 0, file) != 0);
    g_manifestPathW = file;
    CHECK(WritePrivateProfileStringW(L"settings", L"EnemiesTargetWingmen", L"1", file) != 0);
    CHECK(WritePrivateProfileStringW(L"settings", L"Label", L"x\"y\\z", file) != 0);
    CHECK(WritePrivateProfileStringW(L"settings", L"bad-key", L"1", file) != 0);
    CHECK(BuildConfig());
    CHECK(g_configChunk.find("EnemiesTargetWingmen = 1") != std::string::npos);
    CHECK(g_configChunk.find("Label = \"x\\\"y\\\\z\"") != std::string::npos);
    CHECK(g_configChunk.find("bad-key") == std::string::npos);
    CHECK(DeleteFileW(file) != 0);

    SCENARIO("mission master runs AI, other clients create remote copies");
    Reset(1, 3, true); MissionInit(mode.data(), nullptr);
    CHECK(initialized == 1 && created == 3 && remote == 0 && calculated == 1);
    CHECK(g_wingmen == (std::vector<uint32_t>{1001, 1002, 1003}));
    CHECK(g_localWingmen == g_wingmen);
    CHECK(slotNames[1] == L"机体一");
    SCENARIO("mission respawn mappings retain only this mission's exact wingman objects");
    CHECK(MissionObjectForPilot(um, 0x80000010u) == 1001);
    CHECK(MissionSlotForObject(um, 1001) == 1);
    CHECK(KeepDeadMissionUnit(units[0].data()) == 1);
    CHECK(KeepDeadMissionUnit(units[5].data()) == 0);
    CHECK(MissionObjectForPilot(um, 123) == 0);
    CHECK(MissionSlotForObject(nullptr, 1001) == -1);
    Put(service.data(), 0x0C, static_cast<uint8_t*>(nullptr));
    CHECK(MissionSlotForObject(um, 1001) == -1 && !KeepDeadMissionUnit(units[0].data()));
    CHECK(slots == (std::vector<int>{1, 2, 3}));
    Reset(1, 2, false); MissionInit(mode.data(), nullptr);
    CHECK(remote == 2 && g_localWingmen.empty());
    Reset(1, 2, true); observer = true; MissionInit(mode.data(), nullptr);
    CHECK(remote == 2 && g_localWingmen.empty());

    SCENARIO("mission slot limit and derived mode isolation");
    Reset(11, 3, true); MissionInit(mode.data(), nullptr);
    CHECK(created == 1 && assigned == 1 && g_wingmen.size() == 1);
    Reset(1, 3, true); g_game.missionVtable = nullptr; MissionInit(mode.data(), nullptr);
    CHECK(initialized == 1 && created == 0);

    SCENARIO("friendly Ace uses machine id for name and promoted profile from first life through respawn");
    Reset(1, 1, true); SetAceRecord(0); MissionInit(mode.data(), nullptr);
    CHECK(g_observedWingmen.size() == 1 && g_observedWingmen[0].ace);
    CHECK(g_observedWingmen[0].unitDataId == 70001);
    CHECK(ai_test::Get<uint32_t>(npcDefinitions[0].data(), 0) == 5001);
    CHECK(slotNames[1] == L"王牌甲" && aceNameCalls == 1);
    CHECK(profileEvents == (std::vector<std::string>{"begin", "unit"}));
    CHECK(beginGenerations.size() == 1 && profileCalls.size() == 1);
    CHECK(profileCalls[0].unit == units[0].data());
    CHECK(profileCalls[0].flags == (static_cast<int>(battle_ai::Difficulty::Hard) | 0x100));
    CHECK(profileCalls[0].generation != beginGenerations[0]);
    const uint32_t pilot = g_observedWingmen[0].pilotId;
    CHECK(MissionObjectForPilot(um, pilot) == 1001);
    slotNames[1] = L"机体一";
    MissionRespawnComplete(um, 1001);
    CHECK(slotNames[1] == L"王牌甲" && aceNameCalls == 2);
    CHECK(profileCalls.size() == 2);
    CHECK(profileCalls[1].flags == static_cast<int>(battle_ai::Difficulty::Hard));
    CHECK(profileCalls[1].generation != profileCalls[0].generation);
    Put(units[0].data(), kObjectHp, uint32_t{0});
    g_disabled = true;
    MissionUpdate(mode.data(), nullptr);
    CHECK(profileCalls.size() == 2 && slotNames[1] == L"王牌甲");

    SCENARIO("missing name provider retries only in the active exact mission");
    Reset(1, 1, true); SetAceRecord(0); g_acePilotName = nullptr; MissionInit(mode.data(), nullptr);
    CHECK(slotNames[1] == L"机体一" && !g_observedWingmen[0].aceNameApplied);
    g_acePilotName = &AceName; g_disabled = true;
    Put(service.data(), 0x0C, derivedMode.data());
    MissionUpdate(derivedMode.data(), nullptr);
    CHECK(slotNames[1] == L"机体一" && aceNameCalls == 0);
    Put(service.data(), 0x0C, mode.data());
    MissionUpdate(mode.data(), nullptr);
    CHECK(slotNames[1] == L"王牌甲" && aceNameCalls == 1);

    SCENARIO("hostile AI never consumes Ace metadata, pilot names, or announcement state");
    Reset(1, 1, true); SetAceRecord(0, 2); MissionInit(mode.data(), nullptr);
    CHECK(g_observedWingmen.size() == 1 && !g_observedWingmen[0].ace);
    CHECK(g_observedWingmen[0].effectiveDifficulty == battle_ai::Difficulty::Medium);
    CHECK(slotNames[1] == L"机体一" && aceNameCalls == 0 && !g_missionAcePending);
    CHECK(profileCalls.size() == 1 && profileCalls[0].flags ==
          (static_cast<int>(battle_ai::Difficulty::Medium) | 0x100));

    SCENARIO("fixed Ace announcement retries dependencies and append failure, then sends once at genuine ready state");
    Reset(1, 1, true); SetAceRecord(0); MissionInit(mode.data(), nullptr);
    CHECK(g_missionAcePending && !g_missionAceAnnounced);
    CHECK(MissionReady(mode.data()) == 0 && appendCalls == 0); // state 2
    Put(service.data(), 0x3C, uint32_t{3});
    g_matchAiMissionTick = &MatchTick; g_disabled = true;
    MissionUpdate(mode.data(), nullptr);
    CHECK(!g_missionReadyReleased && appendCalls == 0);
    g_appendLine = nullptr;
    CHECK(MissionReady(mode.data()) == 0 && !g_missionAceAnnounced);
    g_appendLine = reinterpret_cast<AppendLineFn>(&Append); appendSuccess = false;
    CHECK(MissionReady(mode.data()) == 0 && appendCalls == 1 && lineCtors == 1 && lineDtors == 1);
    CHECK(!g_missionAceAnnounced && g_missionAcePending);
    appendSuccess = true;
    CHECK(MissionReady(mode.data()) == 1);
    CHECK(appendedText == L"我方出现了王牌驾驶员！");
    CHECK(g_missionAceAnnounced && !g_missionAcePending && appendCalls == 2);
    CHECK(lineCtors == 2 && lineDtors == 2);
    CHECK(MissionReady(mode.data()) == 1 && appendCalls == 2);

    SCENARIO("every local mission audience receives the same fixed message once per new battle");
    Reset(1, 1, false); SetAceRecord(0); MissionInit(mode.data(), nullptr);
    Put(service.data(), 0x3C, uint32_t{3});
    CHECK(MissionReady(mode.data()) == 1 && appendedText == L"我方出现了王牌驾驶员！");
    CHECK(appendCalls == 1);
    Reset(1, 1, true); SetAceRecord(0); MissionInit(mode.data(), nullptr);
    Put(service.data(), 0x3C, uint32_t{3});
    CHECK(MissionReady(mode.data()) == 1 && appendCalls == 1);

    return ai_test::Finish();
}
