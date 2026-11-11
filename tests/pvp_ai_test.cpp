#include "ai_test_support.h"
#include "../mods/pvp_ai/pvp_ai.cpp"
#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
#include <thread>

namespace fixture {
using ai_test::Put;
using ai_test::Get;
std::vector<uint8_t> room(0x33000);
uint32_t nextId = kAiPilotBit | 100;
uint32_t failBuild = 0;
uint32_t failSetup = 0;
int buildCalls = 0, setupCalls = 0, respawns = 0, refreshes = 0, stateRespawns = 0;
uint32_t stateIdSeen = 0, setupPilotIdSeen = 0, setupSlotIdSeen = 0;
uint32_t proposedDamageSeen = 0, attackTypeSeen = 0;
int nativeDamageCalls = 0;
int originals = 0;
int resultHostCalls=0,resultHostTransfer=-1;
std::vector<std::array<uint8_t, 0x1A>> leavePackets;
std::vector<bool> leaveOriginalReturned;
std::vector<bool> leaveAiStillPresent;
int replaceSlotAfterBroadcast = -1;
uint32_t replacementPilotId = 0;
void __fastcall NativeResultHost(uint8_t* value,void*,const int* order,const int*,int transfer,int count) {
    ++resultHostCalls;resultHostTransfer=transfer;
    if(transfer>0 && count>0 && order && order[0]>=0 && order[0]<kSlots)
        std::memcpy(value+0xA5,value+kSlotInfo+order[0]*kSlotInfoStride,kSlotInfoStride);
}
int nativeTeamCalls=0,nativeHumanCalls=0,nativeHumanError=0;
bool nativeAssignDuringSetup=false;
int nativeRandomSeen=-1;
bool revive = true;
bool aceUnitEligible = true;
std::set<uint32_t> aceIneligibleUnits;
std::vector<uint32_t> aceEligibilityChecks;
uint32_t MachineForNpc(uint32_t npc) { return 10000u + npc; }
int missionDamageCalls = 0;
uint32_t missionDamageSeen = 0;
uint32_t missionAttackTypeSeen = 0;
bool clearIdentityAtEnd = false;
bool originalReturnedBeforeObserver = false;
std::vector<SdgoPvpAiObserverEventV1> observerEvents;
std::vector<SdgoAceAiServerSnapshotV1> aceObserverEvents;
int aceObserverUserCookie = 42;
void __cdecl ObserveAce(const SdgoAceAiServerSnapshotV1* snapshot, void* user) {
    CHECK(user == &aceObserverUserCookie);
    CHECK(g_itemsLock.RecursionCount == 0);
    CHECK(snapshot && snapshot->size == sizeof(*snapshot));
    aceObserverEvents.push_back(*snapshot);
}
void __cdecl ThrowAceObserver(const SdgoAceAiServerSnapshotV1*, void*) {
    RaiseException(0xE0000042, 0, 0, nullptr);
}
std::vector<uint32_t> npcIds = {100, 101, 102, 103, 104};
std::vector<uint8_t> points = {4, 3, 2, 5, 0};
std::wstring modDirectory;
const wchar_t* ModDirectory(SdgoMod*) {return modDirectory.c_str();}

void Human(int slot, int team, int colour = 0) {
    SlotPilotId(room.data(), slot) = 10u + slot;
    if(!Get<uint32_t>(room.data(),kRoomHost))Put<uint32_t>(room.data(),kRoomHost,10u+slot);
    room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoTeamA] = static_cast<uint8_t>(team);
    room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoColour] = static_cast<uint8_t>(colour ? colour : team);
}
uint8_t* Pilot(int slot) { return room.data() + kPilots + slot * kPilotStride; }
uint8_t* Unit(int slot, int form = 0) { return Pilot(slot) + kPilotUnits + form * kUnitStride; }
int __fastcall NativeTeamAssignment(void*,void*,uint8_t* value) {
    ++nativeTeamCalls;nativeRandomSeen=value[8];return 37;
}
void Title(const char* title) {
    const auto n = (std::min)(std::strlen(title), kRoomTitleMax);
    room[kRoomTitleLength] = static_cast<uint8_t>(n);
    std::memcpy(room.data() + kRoomTitle, title, n);
}
int __fastcall Build(uint8_t*, void*, uint8_t* pilot, uint8_t* unit, uint32_t npc) {
    ++buildCalls;
    if (npc == failBuild) return 0;
    auto it = std::find(npcIds.begin(), npcIds.end(), npc);
    if (it == npcIds.end()) return 0;
    Put(pilot, 0, nextId++);
    Put(unit, 0, npc);
    Put(unit, kUnitMachineId, MachineForNpc(npc));
    unit[kUnitPoints] = points[static_cast<size_t>(it - npcIds.begin())];
    unit[kUnitGrade] = 3;
    Put<uint16_t>(unit, kUnitLifeCost, 500);
    Put<uint32_t>(unit, kUnitMaxHp, 15000);
    Put<uint32_t>(unit, kUnitHp, 15000);
    return 1;
}
int __fastcall Setup(void*, void*, uint8_t* info, uint8_t* pilot, uint8_t, uint8_t mode) {
    ++setupCalls;
    setupPilotIdSeen = Get<uint32_t>(pilot, 0);
    const ptrdiff_t slot = (info - (room.data() + kSlotInfo)) / kSlotInfoStride;
    setupSlotIdSeen = slot >= 0 && slot < kSlots ? SlotPilotId(room.data(), static_cast<int>(slot)) : 0;
    uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
    if (Get<uint32_t>(unit, 0) == failSetup) return 0;
    if (mode == 3 || mode == 14) {
        auto cost = Get<uint16_t>(unit, kUnitLifeCost);
        Put<uint16_t>(pilot, kPilotPower, static_cast<uint16_t>(1000 - cost));
        Put(pilot, kPilotLifeCost, cost);
    }
    return 1;
}
uint32_t __fastcall Damage(uint8_t*, void*, uint32_t, uint32_t,
                           uint8_t*, uint8_t*, uint32_t, uint32_t,
                           uint32_t proposedDamage, uint16_t, uint8_t,
                           uint32_t, uint8_t, uint32_t attackType) {
    ++nativeDamageCalls;
    proposedDamageSeen = proposedDamage;
    attackTypeSeen = attackType;
    return proposedDamage;
}
int __cdecl AceEligible(uint32_t unitId) {
    aceEligibilityChecks.push_back(unitId);
    return aceUnitEligible && !aceIneligibleUnits.count(unitId) ? 1 : 0;
}
void __cdecl MissionTick(uint8_t*, uint32_t) {}
void __cdecl MissionEnd(uint8_t*) {}
uint32_t __cdecl MissionDamage(uint8_t*, uint32_t, uint8_t*,
                               uint32_t proposedDamage, uint32_t attackType) {
    ++missionDamageCalls;
    missionDamageSeen = proposedDamage;
    missionAttackTypeSeen = attackType;
    return proposedDamage + 125;
}
void __fastcall ResetPart(uint8_t*, void*) {}
void __fastcall SetId(uint8_t*, void*, uint32_t id) { stateIdSeen = id; }
void __fastcall SetRoom(uint8_t*, void*, uint8_t*) {}
void __fastcall SetMode(uint8_t*, void*, uint8_t, uint8_t) {}
int __fastcall Respawn(uint8_t*, void*, uint32_t, uint8_t* pilot, uint32_t, uint32_t) {
    ++respawns;
    if (revive) Put<uint32_t>(pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride, kUnitHp, 9000);
    return revive ? 1 : 0;
}
void __fastcall Refresh(uint8_t*, void*, int, uint8_t*, int, int) { ++refreshes; }
int __fastcall StateRespawn(uint8_t*, void*) { ++stateRespawns; return 1; }
void __fastcall End(uint8_t* endRoom, void*, uint16_t, uint16_t, int) {
    ++originals;
    if (clearIdentityAtEnd) {
        Put(endRoom, kRoomHost, 0u);
        endRoom[kRoomMode] = 0;
        endRoom[kRoomTitleLength] = 0;
        memset(endRoom + kRoomTitle, 0, kRoomTitleMax);
    }
    originalReturnedBeforeObserver = true;
}
int __fastcall Broadcast(uint8_t* value, void*, uint8_t* packet, uint32_t length,
                         uint32_t* attemptedRecipients) {
    CHECK(length == sizeof(RoomLeavePacket));
    CHECK(Get<uint16_t>(packet, 0) == 0x1F47);
    const uint32_t slot = Get<uint32_t>(packet, 0x12);
    const uint32_t pilotId = Get<uint32_t>(packet, 0x16);
    CHECK(slot < static_cast<uint32_t>(kSlots));
    const bool stillPresent = slot < static_cast<uint32_t>(kSlots) &&
                              SlotPilotId(value, static_cast<int>(slot)) == pilotId &&
                              (pilotId & kAiPilotBit) != 0;
    leaveOriginalReturned.push_back(originalReturnedBeforeObserver);
    leaveAiStillPresent.push_back(stillPresent);
    std::array<uint8_t, 0x1A> saved{};
    std::memcpy(saved.data(), packet, saved.size());
    leavePackets.push_back(saved);
    *attemptedRecipients = Get<uint32_t>(value, kRoomPlayers);
    Put(packet, 2, 10u);
    if (static_cast<int>(slot) == replaceSlotAfterBroadcast) {
        SlotPilotId(value, static_cast<int>(slot)) = replacementPilotId;
        std::memset(value + kPilots + slot * kPilotStride, 0x5A, kPilotStride);
    }
    return 1;
}
void __cdecl Observe(const SdgoPvpAiObserverEventV1* event, void*) {
    CHECK(originalReturnedBeforeObserver);
    observerEvents.push_back(*event);
}

void Reset(int mode = 1) {
    std::fill(room.begin(), room.end(), uint8_t{});
    room[kRoomMode] = static_cast<uint8_t>(mode);
    room[kRoomCapacity] = 8;
    room[kTeamColour1]=1;room[kTeamColour1+1]=2;
    g_settings = Settings{};
    g_settings.gradeCost[3] = 455;
    g_aceSettings = battle_ai::ace::Settings{};
    g_aceSettings.enabled = false;
    g_reloadAceSettingsAtBattleEntry = false;
    g_itemController = {};
    std::memset(g_itemRooms, 0, sizeof(g_itemRooms));
    g_roomGeneration = 0;
    InterlockedExchange(&g_aceDamageSequence, 0);
    InterlockedExchangePointer(&g_aceObserverCallback, nullptr);
    InterlockedExchangePointer(&g_aceObserverUser, nullptr);
    InterlockedExchange(&g_aceObserverRegistration, 0);
    aceObserverEvents.clear();
    g_coopAceDamageModule = nullptr;
    g_completeMissionAceDamage = nullptr;
    nextId = kAiPilotBit | 100;
    g_settings.logRoom = 0;
    g_random = 1234567;
    nativeTeamCalls=nativeHumanCalls=nativeHumanError=0;
    nativeAssignDuringSetup=false;nativeRandomSeen=-1;
    g_assignHumanTeams=reinterpret_cast<AssignHumanTeamsFn>(&NativeTeamAssignment);
    g_api = &ai_test::api;
    g_mod = nullptr;
    g_battleAiServerModule = nullptr;
    g_applyServerLoadout = nullptr;
    g_aceServerModule = nullptr;
    g_aceUnitEligible = &AceEligible;
    g_battleAiServerPath.clear();
    g_loadoutBridgeFaultLogged = 0;
    g_buildAiPilot = reinterpret_cast<BuildAiPilotFn>(&Build);
    g_setupAiPilot = reinterpret_cast<SetupAiPilotFn>(&Setup);
    g_pilotReset = reinterpret_cast<PilotResetFn>(&ResetPart);
    g_pilotStateId = reinterpret_cast<PilotStateIdFn>(&SetId);
    g_pilotStateRoom = reinterpret_cast<PilotStateRoomFn>(&SetRoom);
    g_pilotStateMode = reinterpret_cast<PilotStateModeFn>(&SetMode);
    g_respawnPilot = reinterpret_cast<RespawnPilotFn>(&Respawn);
    g_respawnRefresh = reinterpret_cast<RespawnRefreshFn>(&Refresh);
    g_pilotStateRespawn = reinterpret_cast<PilotStateRespawnFn>(&StateRespawn);
    g_broadcast = reinterpret_cast<BroadcastFn>(&Broadcast);
    g_damage = reinterpret_cast<DamageFn>(&Damage);
    g_battleEnd = reinterpret_cast<BattleEndFn>(&End);
    g_resultHost = reinterpret_cast<ResultHostFn>(&NativeResultHost);
    resultHostCalls=0;resultHostTransfer=-1;
    std::memset(g_dead, 0, sizeof(g_dead));
    std::memset(g_watch, 0, sizeof(g_watch));
    for (auto& active : g_observerActiveRooms)
        InterlockedExchangePointer(&active, nullptr);
    InterlockedExchangePointer(&g_observerCallback, nullptr);
    InterlockedExchangePointer(&g_observerUser, nullptr);
    InterlockedExchange(&g_observerRegistration, 0);
    InterlockedExchange(&g_observerSequence, 0);
    InterlockedExchange(&g_observerFaultLogged, 0);
    buildCalls = setupCalls = respawns = refreshes = stateRespawns = originals = 0;
    stateIdSeen = setupPilotIdSeen = setupSlotIdSeen = 0;
    proposedDamageSeen = attackTypeSeen = 0;
    nativeDamageCalls = 0;
    failBuild = failSetup = 0;
    revive = true;
    aceUnitEligible = true;
    aceIneligibleUnits.clear();
    aceEligibilityChecks.clear();
    missionDamageCalls = 0;
    missionDamageSeen = missionAttackTypeSeen = 0;
    g_missionLifecycle = nullptr;
    clearIdentityAtEnd = false;
    originalReturnedBeforeObserver = false;
    observerEvents.clear();
    leavePackets.clear();
    leaveOriginalReturned.clear();
    leaveAiStillPresent.clear();
    replaceSlotAfterBroadcast = -1;
    replacementPilotId = 0;
}
int __fastcall NativeHumanSetup(void* proc,void*,uint8_t* value,uint32_t*,uint32_t*,uint8_t) {
    ++nativeHumanCalls;
    if(nativeAssignDuringSetup)AssignHumanTeams(proc,nullptr,value);
    return nativeHumanError;
}
int AddUsingNativePoolFixture(bool throughStart=false) {
    struct Node {uint32_t* data;void* reserved;Node* next;};
    alignas(4) uint8_t list[0x30]{};
    auto* lock=reinterpret_cast<CRITICAL_SECTION*>(list+0x18);InitializeCriticalSection(lock);
    std::vector<Node> nodes(npcIds.size()+2);
    for(size_t i=0;i+1<nodes.size();++i)nodes[i].next=&nodes[i+1];
    for(size_t i=0;i<npcIds.size();++i)nodes[i+1].data=&npcIds[i];
    Put(list,4,nodes.data());Put(list,8,&nodes.back());Put<int>(list,0xC,static_cast<int>(npcIds.size()));
    uint8_t* nativeList=list;g_aiNpcList=&nativeList;g_settings.levels[2]=npcIds;
    if(throughStart)g_setupHumans=reinterpret_cast<SetupHumansFn>(&NativeHumanSetup);
    const int result=throughStart ? SetupHumans(nullptr,nullptr,room.data(),nullptr,nullptr,0) : (AddPvpAi(room.data())?1:0);
    g_aiNpcList=nullptr;DeleteCriticalSection(lock);return result;
}
}

int wmain(int argc,wchar_t** argv) {
    using namespace fixture;
    CHECK(argc==2);
    if(argc!=2) return 2;
    // The production ModLoad initializes the new item lifecycle lock. This
    // fixture calls the included implementation directly instead of ModLoad.
    InitializeCriticalSection(&g_itemsLock);

    SCENARIO("mission lifecycle accepts V1 and V2 safely and V2 scales damage before the native call");
    Reset(5);
    SdgoMissionServerLifecycleV1 missionV1{
        sizeof(missionV1), 1, &MissionTick, &MissionEnd};
    CHECK(SdgoPvpAiRegisterMissionV1(&missionV1) == 1);
    uint8_t weapon[0x40]{};
    CHECK(AcceptedDamage(room.data(), nullptr, 1, 2, Pilot(2), weapon,
                         0, 10, 1000, 0, 0, 0, 0, 0) == 1000);
    CHECK(missionDamageCalls == 0 && proposedDamageSeen == 1000);
    g_missionLifecycle = nullptr;
    SdgoMissionServerLifecycleV2 missionV2{
        sizeof(missionV2), 2, &MissionTick, &MissionEnd, &MissionDamage};
    auto truncatedMissionV2 = missionV2;
    truncatedMissionV2.size = sizeof(SdgoMissionServerLifecycleV1);
    CHECK(SdgoPvpAiRegisterMissionV1(
              reinterpret_cast<const SdgoMissionServerLifecycleV1*>(
                  &truncatedMissionV2)) == 0);
    CHECK(SdgoPvpAiRegisterMissionV1(
              reinterpret_cast<const SdgoMissionServerLifecycleV1*>(
                  &missionV2)) == 1);
    CHECK(AcceptedDamage(room.data(), nullptr, 2, 3, Pilot(3), weapon,
                         0, 10, 1000, 0, 0, 0, 0, 0) == 1125);
    CHECK(missionDamageCalls == 1 && missionDamageSeen == 1000 &&
          missionAttackTypeSeen == 0 && proposedDamageSeen == 1125);

    SCENARIO("server-authored Ace marker and effective difficulty are fixed before native pilot registration");
    struct AceDifficultyCase {
        const char* title;
        battle_ai::Difficulty base;
        battle_ai::Difficulty effective;
    };
    for (const auto& value : {
            AceDifficultyCase{"!ai easy", battle_ai::Difficulty::Easy, battle_ai::Difficulty::Medium},
            AceDifficultyCase{"!ai medium", battle_ai::Difficulty::Medium, battle_ai::Difficulty::Hard},
            AceDifficultyCase{"!ai hard", battle_ai::Difficulty::Hard, battle_ai::Difficulty::Hard}}) {
        Reset();
        Title(value.title);
        CHECK(PrepareRoomItems(room.data(), value.base));
        g_aceSettings.enabled = true;
        g_aceSettings.byDifficulty[battle_ai::ace::Index(value.base)].chancePercent = 100;
        CHECK(AddAiPilot(room.data(), 6, 100, 2, 1));
        CHECK(aceEligibilityChecks ==
              std::vector<uint32_t>{MachineForNpc(100)});
        const uint32_t pilotId = SlotPilotId(room.data(), 6);
        const auto decoded = battle_ai::ace::DecodePilotId(pilotId, value.base);
        CHECK(decoded.ace && decoded.effective == value.effective);
        CHECK(stateIdSeen == pilotId && setupPilotIdSeen == pilotId && setupSlotIdSeen == pilotId);
        battle_ai::ace::Outcome stored{};
        CHECK(CopyAceOutcome(room.data(), 6, pilotId, &stored));
        CHECK(stored.ace && stored.base == value.base && stored.effective == value.effective);
        CHECK(Get<uint32_t>(Pilot(6), 0x05) ==
              (value.effective == battle_ai::Difficulty::Easy ? 30000u : 0u));
    }

    SCENARIO("zero chance and exhausted room tracking fail closed to an ordinary pilot");
    Reset();
    Title("!ai hard");
    CHECK(PrepareRoomItems(room.data(), battle_ai::Difficulty::Hard));
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2].chancePercent = 0;
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    CHECK((SlotPilotId(room.data(), 6) & battle_ai::ace::kAcePilotBit) == 0);
    ApplyAceStats(room.data(), 6);
    CHECK(Get<uint32_t>(Unit(6), kUnitMaxHp) == 15000);
    std::memset(weapon, 0, sizeof(weapon));
    CHECK(AcceptedDamage(room.data(), nullptr, 3, 6, Pilot(6), weapon, 0,
                         10, 500, 0, 0, 0, 0, 0) == 500);
    SdgoAceAiServerSnapshotV1 pvpSnapshot{};
    pvpSnapshot.size = sizeof(pvpSnapshot);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    pvpSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 1);
    CHECK(pvpSnapshot.mode == 1 && pvpSnapshot.generation != 0 &&
          pvpSnapshot.unit_count == 1);
    CHECK(pvpSnapshot.units[0].pilot_id == SlotPilotId(room.data(), 6) &&
          pvpSnapshot.units[0].native_pilot_id ==
              ai_test::Get<uint32_t>(Pilot(6), 0) &&
          pvpSnapshot.units[0].npc_id == 100 &&
          pvpSnapshot.units[0].machine_id == MachineForNpc(100) &&
          pvpSnapshot.units[0].slot == 6 && pvpSnapshot.units[0].form == 0 &&
          pvpSnapshot.units[0].base_difficulty == 2 &&
          pvpSnapshot.units[0].effective_difficulty == 2 &&
          pvpSnapshot.units[0].ace == 0 &&
          pvpSnapshot.units[0].hp_percent == 100 &&
          pvpSnapshot.units[0].attack_percent == 100 &&
          pvpSnapshot.units[0].active == 1);
    CHECK(pvpSnapshot.damage.sequence != 0 &&
          pvpSnapshot.damage.source == SDGO_ACE_AI_SERVER_PVP &&
          pvpSnapshot.damage.ace == 0 &&
          pvpSnapshot.damage.policy_matched == 1 &&
          pvpSnapshot.damage.proposed_damage == 500 &&
          pvpSnapshot.damage.adjusted_damage == 500 &&
          pvpSnapshot.damage.native_result == 500 &&
          pvpSnapshot.damage.attack_percent == 100);
    const uint32_t ordinaryReceipt = pvpSnapshot.damage.sequence;
    pvpSnapshot.size = sizeof(pvpSnapshot) - 1;
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 0);
    pvpSnapshot.size = sizeof(pvpSnapshot);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION + 1;
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 0);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    pvpSnapshot.room_cookie = 0x1234;
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 0);
    CHECK(SdgoPvpAiGetAceSnapshotV1(
              reinterpret_cast<SdgoAceAiServerSnapshotV1*>(1)) == 0);
    CHECK(ordinaryReceipt != 0);
    Reset();
    Title("!ai hard");
    for (size_t i = 0; i < std::size(g_itemRooms); ++i) {
        g_itemRooms[i].active = true;
        g_itemRooms[i].room = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(0x10000 + i * 0x100));
        g_itemRooms[i].roomIndex = static_cast<uint32_t>(i + 1);
    }
    CHECK(!PrepareRoomItems(room.data(), battle_ai::Difficulty::Hard));
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2].chancePercent = 100;
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    CHECK((SlotPilotId(room.data(), 6) & battle_ai::ace::kAcePilotBit) == 0);
    Reset();
    Title("!ai hard");
    CHECK(PrepareRoomItems(room.data(), battle_ai::Difficulty::Hard));
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2].chancePercent = 100;
    aceUnitEligible = false;
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    CHECK((SlotPilotId(room.data(), 6) & battle_ai::ace::kAcePilotBit) == 0);

    SCENARIO("a missing pilot entry on any usable deck unit disables Ace without rejecting the bot");
    Reset(7);
    Title("!ai hard");
    CHECK(PrepareRoomItems(room.data(), battle_ai::Difficulty::Hard));
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2].chancePercent = 100;
    aceIneligibleUnits.insert(MachineForNpc(101));
    bool deckCatalogComplete = true;
    const auto checkedPoints =
        PoolPoints(room.data(), npcIds, &deckCatalogComplete);
    CHECK(!deckCatalogComplete);
    CHECK(AddTeam(room.data(), 2, 1, npcIds, checkedPoints,
                  deckCatalogComplete) == 1);
    int deckBotSlot = -1;
    for (int slot = 0; slot < kSlots; ++slot)
        if (SlotPilotId(room.data(), slot) & kAiPilotBit) {
            deckBotSlot = slot;
            break;
        }
    CHECK(deckBotSlot >= 0);
    CHECK((SlotPilotId(room.data(), deckBotSlot) &
           battle_ai::ace::kAcePilotBit) == 0);

    SCENARIO("Hard Ace scales every deck form once, keeps identity across respawn, and only scales ordinary attacks");
    Reset(7);
    Title("!ai hard");
    CHECK(PrepareRoomItems(room.data(), battle_ai::Difficulty::Hard));
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2].chancePercent = 100;
    g_aceSettings.byDifficulty[2].hpPercent = 120;
    g_aceSettings.byDifficulty[2].attackPercent = 150;
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    const uint32_t hardAceId = SlotPilotId(room.data(), 6);
    BuildDeck(room.data(), 6, 100, npcIds, points);
    const int aceForms = Pilot(6)[kPilotTagUnits];
    CHECK(aceForms >= 2);
    // A malformed form must not consume the one-shot latch; once repaired,
    // the complete deck is scaled together.
    const uint32_t formOneMaximum = Get<uint32_t>(Unit(6, 1), kUnitMaxHp);
    Put<uint32_t>(Unit(6, 1), kUnitMaxHp, 0);
    ApplyAceStats(room.data(), 6);
    CHECK(Get<uint32_t>(Unit(6), kUnitMaxHp) == 15000);
    Put<uint32_t>(Unit(6, 1), kUnitMaxHp, formOneMaximum);
    ApplyAceStats(room.data(), 6);
    for (int form = 0; form < aceForms; ++form) {
        CHECK(Get<uint32_t>(Unit(6, form), kUnitMaxHp) == 18000);
        CHECK(Get<uint32_t>(Unit(6, form), kUnitHp) == 18000);
    }
    ApplyAceStats(room.data(), 6);
    for (int form = 0; form < aceForms; ++form)
        CHECK(Get<uint32_t>(Unit(6, form), kUnitMaxHp) == 18000);
    std::memset(weapon, 0, sizeof(weapon));
    CHECK(AcceptedDamage(room.data(), nullptr, 7, 6, Pilot(6), weapon, 0,
                         10, 1000, 0, 0, 0, 0, 0) == 1500);
    CHECK(proposedDamageSeen == 1500 && attackTypeSeen == 0);
    pvpSnapshot = {};
    pvpSnapshot.size = sizeof(pvpSnapshot);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    pvpSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 1);
    CHECK(pvpSnapshot.damage.source == SDGO_ACE_AI_SERVER_PVP &&
          pvpSnapshot.damage.ace == 1 &&
          pvpSnapshot.damage.policy_matched == 1 &&
          pvpSnapshot.damage.attack_type == 0 &&
          pvpSnapshot.damage.proposed_damage == 1000 &&
          pvpSnapshot.damage.adjusted_damage == 1500 &&
          pvpSnapshot.damage.native_result == 1500 &&
          pvpSnapshot.damage.attack_percent == 150);
    const uint32_t ordinaryAceReceipt = pvpSnapshot.damage.sequence;
    CHECK(AcceptedDamage(room.data(), nullptr, 8, 6, Pilot(6), weapon, 0,
                         10, 1000, 0, 0, 0, 0, 1) == 1000);
    CHECK(proposedDamageSeen == 1000 && attackTypeSeen == 1);
    pvpSnapshot = {};
    pvpSnapshot.size = sizeof(pvpSnapshot);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    pvpSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    Pilot(6)[kPilotForm] = 1;
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 1);
    CHECK(pvpSnapshot.damage.sequence > ordinaryAceReceipt &&
          pvpSnapshot.damage.attack_type == 1 &&
          pvpSnapshot.damage.proposed_damage == 1000 &&
          pvpSnapshot.damage.adjusted_damage == 1000 &&
          pvpSnapshot.damage.native_result == 1000 &&
          pvpSnapshot.damage.attack_percent == 150);
    CHECK(pvpSnapshot.unit_count == 1 &&
          pvpSnapshot.units[0].form == 1 &&
          pvpSnapshot.units[0].npc_id ==
              ai_test::Get<uint32_t>(Unit(6, 1), 0) &&
          pvpSnapshot.units[0].machine_id ==
              ai_test::Get<uint32_t>(Unit(6, 1), kUnitMachineId) &&
          pvpSnapshot.units[0].npc_id != pvpSnapshot.units[0].machine_id &&
          pvpSnapshot.units[0].current_hp == 18000 &&
          pvpSnapshot.units[0].max_hp == 18000 &&
          pvpSnapshot.units[0].hp_percent == 120 &&
          pvpSnapshot.units[0].attack_percent == 150 &&
          pvpSnapshot.units[0].ace == 1);
    Pilot(6)[kPilotForm] = 0;
    Put<uint32_t>(Unit(6), kUnitHp, 0);
    RespawnAiPilots(room.data(), 100);
    CHECK(SlotPilotId(room.data(), 6) == hardAceId);
    CHECK(Get<uint32_t>(Unit(6), kUnitMaxHp) == 18000);
    CHECK(Get<uint32_t>(Unit(6), kUnitHp) == 9000);
    battle_ai::ace::Outcome respawned{};
    CHECK(CopyAceOutcome(room.data(), 6, hardAceId, &respawned));
    CHECK(respawned.ace && respawned.hpPercent == 120 && respawned.attackPercent == 150);
    ApplyAceStats(room.data(), 6);
    CHECK(Get<uint32_t>(Unit(6), kUnitMaxHp) == 18000);
    pvpSnapshot = {};
    pvpSnapshot.size = sizeof(pvpSnapshot);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    pvpSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 1 &&
          pvpSnapshot.units[0].current_hp == 9000 &&
          pvpSnapshot.units[0].active == 1);
    EndRoomItems(room.data());
    pvpSnapshot = {};
    pvpSnapshot.size = sizeof(pvpSnapshot);
    pvpSnapshot.version = SDGO_ACE_AI_OBSERVER_VERSION;
    pvpSnapshot.room_cookie = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(room.data()));
    CHECK(SdgoPvpAiGetAceSnapshotV1(&pvpSnapshot) == 0);

    SCENARIO("optional post-build server bridge is absent-safe and covers initial and deck builds");
    namespace fs=std::filesystem;
    const fs::path bridgeRoot=fs::temp_directory_path()/
        (L"sdgo-pvp-ai-bridge-"+std::to_wstring(GetCurrentProcessId()));
    std::error_code removeError;
    fs::remove_all(bridgeRoot,removeError);
    const fs::path pvpDirectory=bridgeRoot/L"sdgo/mods/pvp_ai";
    const fs::path serverDirectory=bridgeRoot/L"sdgo/mods/battle_ai_server";
    fs::create_directories(pvpDirectory);
    fs::create_directories(serverDirectory);
    modDirectory=pvpDirectory.wstring();
    auto bridgeApi=ai_test::api;
    bridgeApi.ModDirectory=&ModDirectory;
    Reset();
    g_api=&bridgeApi;
    Title("!ai hard");
    CHECK(AddAiPilot(room.data(),6,100,2,2));
    CHECK(g_battleAiServerModule==nullptr&&g_applyServerLoadout==nullptr);
    const fs::path staged=serverDirectory/L"battle_ai_server.dll";
    fs::copy_file(argv[1],staged,fs::copy_options::overwrite_existing);
    HMODULE bridge=LoadLibraryW(staged.c_str());
    CHECK(bridge!=nullptr);
    using GetInt=int(__cdecl*)();
    using GetPointer=void*(__cdecl*)();
    auto calls=bridge?reinterpret_cast<GetInt>(GetProcAddress(bridge,"TestCalls")):nullptr;
    auto lastNpc=bridge?reinterpret_cast<GetInt>(GetProcAddress(bridge,"TestLastNpc")):nullptr;
    auto lastRoom=bridge?reinterpret_cast<GetPointer>(GetProcAddress(bridge,"TestLastRoom")):nullptr;
    auto lastPilot=bridge?reinterpret_cast<GetPointer>(GetProcAddress(bridge,"TestLastPilot")):nullptr;
    auto lastUnit=bridge?reinterpret_cast<GetPointer>(GetProcAddress(bridge,"TestLastUnit")):nullptr;
    CHECK(calls&&lastNpc&&lastRoom&&lastPilot&&lastUnit);
    Reset(7);
    g_api=&bridgeApi;
    Title("!ai hard");
    CHECK(AddAiPilot(room.data(),6,100,2,2));
    CHECK(calls&&calls()==1&&lastNpc&&lastNpc()==100&&lastRoom&&lastRoom()==room.data()&&
          lastPilot&&lastPilot()==Pilot(6)&&lastUnit&&lastUnit()==Unit(6)&&Unit(6)[0x30]==0xA5);
    auto poolPoints=PoolPoints(room.data(),npcIds);
    CHECK(poolPoints==points&&calls&&calls()==1+static_cast<int>(npcIds.size()));
    Put<uint16_t>(Pilot(6),kPilotPower,321);Put<uint16_t>(Pilot(6),kPilotLifeCost,654);
    BuildDeck(room.data(),6,100,npcIds,poolPoints);
    CHECK(calls&&calls()>1+static_cast<int>(npcIds.size()));
    for(int form=1;form<Pilot(6)[kPilotTagUnits];++form) CHECK(Unit(6,form)[0x30]==0xA5);
    const int callsBeforeRespawn=calls?calls():0;
    Pilot(6)[kPilotForm]=1;Put<uint32_t>(Unit(6,1),kUnitHp,0);
    RespawnAiPilots(room.data(),123);
    CHECK(Unit(6,1)[0x30]==0xA5&&Get<uint32_t>(Unit(6,1),kUnitHp)==9000&&
          calls&&calls()==callsBeforeRespawn);
    FreeLibrary(bridge);
    Reset();
    g_api=&bridgeApi;
    CHECK(AddAiPilot(room.data(),6,100,2,2));
    CHECK(g_battleAiServerModule==nullptr&&g_applyServerLoadout==nullptr);
    fs::remove_all(bridgeRoot,removeError);

    Reset();
    SCENARIO("room commands and bounded numbers");
    CHECK(!ParseTitle("normal room").enabled);
    auto p = ParseTitle("!AI e3 a2 LV4");
    CHECK(p.enabled && p.enemies == 3 && p.allies == 2 && p.level == 4);
    p = ParseTitle("!ai"); CHECK(p.enemies == kFill && p.allies == kFill);
    p = ParseTitle("!ai3 a1"); CHECK(p.enemies == 3 && p.allies == 1);
    p = ParseTitle("!ai0"); CHECK(p.enemies == 0 && p.allies == 0);
    p = ParseTitle("!ai e2x a3 lv9"); CHECK(!p.valid && p.enemies == 0 && p.allies == 3 && p.level == 2);
    p = ParseTitle("!ai4294967296"); CHECK(!p.valid);
    p = ParseTitle("!ai e99999999999999"); CHECK(!p.valid);
    p = ParseTitle("!ai a99999999999999"); CHECK(!p.valid);
    SCENARIO("PvP AI and random commands compose in either order with scoped arguments");
    for(const char* title:{"!rand !ai3 a1 lv4","!ai3 a1 lv4 !rand","!rand !ai e3 a1 lv4","!AI e3 a1 LV4 !RaNd"}) {
        p=ParseTitle(title);CHECK(p.enabled && p.enemies==3 && p.allies==1 && p.level==4);
    }
    for(const char* title:{"!rand !ai","!ai !rand lv5 e2 a2","!ai !future lv5 e2"}) {
        p=ParseTitle(title);CHECK(p.enabled && p.enemies==kFill && p.allies==kFill && p.level==2);
    }
    p=ParseTitle("!ai e2 !rand a3 lv5");CHECK(p.enemies==2 && p.allies==0 && p.level==2);
    p=ParseTitle("!rand !ai0");CHECK(p.enabled && p.enemies==0 && p.allies==0);
    for(const char* title:{"!aim","prefix!ai3","!ai3x","!ai!rand"}) CHECK(!ParseTitle(title).enabled);
    auto parserApi=ai_test::api;parserApi.FindRoomCommand=&sdgo::room_commands::Parse;
    g_api=&parserApi;
    p=ParseTitle("!rand !AI3 a1 LV4");CHECK(p.enabled && p.enemies==3 && p.allies==1 && p.level==4);
    g_api=&ai_test::api;
    CHECK(ParseIds("1, 2 3,,4294967295") == (std::vector<uint32_t>{1, 2, 3, UINT32_MAX}));
    CHECK(ParseIds("4294967296, 100") == (std::vector<uint32_t>{100}));
    CHECK(ParseIds("18446744073709551616100,101") == (std::vector<uint32_t>{101}));
    CHECK(ParseIds("").empty());

    SCENARIO("single human random teams avoid the native negative pattern index without a phantom player");
    for(uint8_t colour:{uint8_t{1},uint8_t{2}}) {
        Reset();Human(6,2,3-colour);Title("!ai e7 a0");room[8]=1;
        Put<uint32_t>(room.data(),kRoomPlayers,1);room[kTeamColour1]=colour;
        Put<uint32_t>(room.data(),0x3CCF,0x11223344);Put<uint32_t>(room.data(),0x3CD3,0x55667788);
        CHECK(AssignHumanTeams(nullptr,nullptr,room.data())==1 && nativeTeamCalls==0);
        CHECK(SlotTeam(room.data(),6)==1 && room[kSlotInfo+6*kSlotInfoStride+kSlotInfoColour]==colour);
        CHECK(Get<uint32_t>(room.data(),kRoomPlayers)==1 && SlotPilotId(room.data(),6)==16 && room[8]==1);
        CHECK(Get<uint32_t>(room.data(),kLastRandomHumanCount)==1);
        CHECK(Get<uint32_t>(room.data(),0x3CCF)==0x11223344 && Get<uint32_t>(room.data(),0x3CD3)==0x55667788);
    }
    for(int humans:{2,3,5}) {
        Reset();Title("!ai enemy");room[8]=1;Put<uint32_t>(room.data(),kRoomPlayers,static_cast<uint32_t>(humans));
        for(int i=0;i<humans;++i)Human(i,i%2+1);
        const auto unchanged=room;
        CHECK(AssignHumanTeams(nullptr,nullptr,room.data())==37 && nativeTeamCalls==1 && room==unchanged);
    }
    Reset();Human(0,2);Title("!ai enemy");Put<uint32_t>(room.data(),kRoomPlayers,1);
    CHECK(AssignHumanTeams(nullptr,nullptr,room.data())==37 && SlotTeam(room.data(),0)==2);
    for(const char* title:{"ordinary room","!ai0","!ai e0 a0"}) {
        Reset();Human(0,2);Title(title);room[8]=1;Put<uint32_t>(room.data(),kRoomPlayers,1);
        CHECK(AssignHumanTeams(nullptr,nullptr,room.data())==37 && nativeTeamCalls==1 && SlotTeam(room.data(),0)==2);
    }
    Reset();Human(0,1);Title("!ai enemy");room[8]=1;room[kTeamColour1]=0;Put<uint32_t>(room.data(),kRoomPlayers,1);
    CHECK(AddUsingNativePoolFixture(true)==0x0FBE && nativeTeamCalls==0 && nativeHumanCalls==0);

    SCENARIO("cooperative grouping and AI population are independent on either chosen host side");
    for(int mode:{1,3,6,7,9})for(uint8_t hostTeam:{uint8_t{1},uint8_t{2}})
      for(int humans=1;humans<=7;++humans)for(uint8_t randomTeams:{uint8_t{0},uint8_t{1}})
      for(bool fill:{false,true}) {
        // Population rules are mode-independent; exhaust counts in deathmatch,
        // then cover both policies, host sides and room settings in every mode.
        if(mode!=3 && humans!=3)continue;
        Reset(mode);Title(fill?"!ai coop fill":"!ai coop");
        room[8]=randomTeams;room[kTeamColour1]=2;room[kTeamColour1+1]=1;
        const int occupied[]={6,0,11,3,9,1,7};
        for(int i=0;i<humans;++i)Human(occupied[i],i ? (i%2+1):hostTeam);
        Put<uint32_t>(room.data(),kRoomPlayers,static_cast<uint32_t>(humans));
        Put<uint32_t>(room.data(),kLastRandomHumanCount,5);
        memset(room.data()+kRandomPattern,0xA5,kSlots);
        const auto before=room;
        nativeAssignDuringSetup=true;
        CHECK(AddUsingNativePoolFixture(true)==0);
        const auto grouped=ViewRoom(room.data());
        CHECK(grouped.hostTeam==hostTeam && grouped.humans[hostTeam]==humans && grouped.humans[3-hostTeam]==0);
        const int alliedAi=fill ? (std::max)(4-humans,0):0;
        CHECK(grouped.ai[hostTeam]==alliedAi && grouped.ai[3-hostTeam]==8-humans-alliedAi);
        CHECK(nativeTeamCalls==1 && nativeRandomSeen==0 && room[8]==randomTeams);
        CHECK(Get<uint32_t>(room.data(),kLastRandomHumanCount)==5 &&
              std::memcmp(room.data()+kRandomPattern,before.data()+kRandomPattern,kSlots)==0);
        for(int i=0;i<humans;++i) {
            const int slot=occupied[i];
            const auto* info=room.data()+kSlotInfo+slot*kSlotInfoStride;
            const auto* original=before.data()+kSlotInfo+slot*kSlotInfoStride;
            CHECK(SlotPilotId(room.data(),slot)==Get<uint32_t>(original,kSlotInfoPilotId));
            CHECK(info[kSlotInfoColour]==3-hostTeam);
            bool unchanged=true;
            for(size_t offset=0;offset<kSlotInfoStride;++offset)
                if(offset!=kSlotInfoTeamA && offset!=kSlotInfoColour && info[offset]!=original[offset])unchanged=false;
            CHECK(unchanged);
        }
    }
    SCENARIO("cooperative exact counts can leave vacancies and add optional AI teammates");
    for(const char* title:{"!ai coop e4 a0","!ai coop e4 a1"}) {
        Reset();Title(title);Human(0,2);Human(6,1);Human(11,1);
        Put<uint32_t>(room.data(),kRoomPlayers,3);nativeAssignDuringSetup=true;
        CHECK(AddUsingNativePoolFixture(true)==0);
        const auto view=ViewRoom(room.data());
        CHECK(view.humans[2]==3 && view.humans[1]==0 && view.ai[1]==4);
        CHECK(view.ai[2]==(std::strstr(title,"a1")?1:0));
    }
    SCENARIO("native random rooms can group real unassigned humans before native team assignment");
    for(int humans:{1,3,4,7})for(int hostTeam:{0,1,2}) {
        Reset();Title("!ai coop");room[8]=1;
        const int occupied[]={6,0,11,3,9,1,7};
        for(int i=0;i<humans;++i)Human(occupied[i],i?0:hostTeam);
        Put<uint32_t>(room.data(),kRoomPlayers,static_cast<uint32_t>(humans));
        nativeAssignDuringSetup=true;
        CHECK(AddUsingNativePoolFixture(true)==0);
        const int side=hostTeam ? hostTeam:1;
        const auto view=ViewRoom(room.data());
        CHECK(view.hostTeam==side && view.humans[side]==humans && view.humans[3-side]==0);
        CHECK(view.ai[side]==0 && view.ai[3-side]==8-humans && room[8]==1 && nativeRandomSeen==0);
    }
    SCENARIO("unassigned manual teams and malformed random identities remain invalid");
    for(int bad=0;bad<4;++bad) {
        Reset();Title("!ai coop");Human(0,0);Human(6,0);
        Put<uint32_t>(room.data(),kRoomPlayers,2);nativeAssignDuringSetup=true;
        if(bad)room[8]=1;
        if(bad==1)SlotPilotId(room.data(),6)=SlotPilotId(room.data(),0);
        if(bad==2)room[kSlotInfo+6*kSlotInfoStride+kSlotInfoTeamA]=3;
        if(bad==3)Put<uint32_t>(room.data(),kRoomHost,9999);
        const auto before=room;
        CHECK(AddUsingNativePoolFixture(true)==0x0FBE && room==before && nativeHumanCalls==0);
    }
    SCENARIO("invalid cooperative starts are rejected before native setup or changing human sides");
    for(int bad=0;bad<7;++bad) {
        Reset();Title("!ai coop");Human(0,1);Human(6,2);Human(11,2);
        Put<uint32_t>(room.data(),kRoomPlayers,3);nativeAssignDuringSetup=true;
        if(bad==0)Title("!ai coop e6");
        if(bad==1)Put<uint32_t>(room.data(),kRoomHost,9999);
        if(bad==2)room[kTeamColour1+1]=1;
        if(bad==3)room[kRoomType]=1;
        if(bad==4)Put<uint32_t>(room.data(),kRoomPlayers,2);
        if(bad==5)room[kRoomCapacity]=2;
        if(bad==6)for(int slot:{1,2,3,4,5})Human(slot,2);
        if(bad==6)Put<uint32_t>(room.data(),kRoomPlayers,8);
        const auto before=room;
        CHECK(AddUsingNativePoolFixture(true)==0x0FBE);
        CHECK(room==before && nativeHumanCalls==0 && nativeTeamCalls==0 && buildCalls==0);
    }
    SCENARIO("failed original human setup restores cooperative sides and permits a normal retry");
    Reset();Title("!ai coop");Human(0,2);Human(6,1);Human(11,1);room[8]=1;
    Put<uint32_t>(room.data(),kRoomPlayers,3);nativeAssignDuringSetup=true;nativeHumanError=0x0FBA;
    auto coopBefore=room;
    CHECK(AddUsingNativePoolFixture(true)==0x0FBA && room==coopBefore && nativeRandomSeen==0);
    nativeHumanError=0;
    CHECK(AddUsingNativePoolFixture(true)==0 && ViewRoom(room.data()).humans[2]==3 && room[8]==1);
    CHECK(GuardedRemove(room.data()));
    SCENARIO("cooperative failed AI population restores human sides for a retry");
    Reset();Title("!ai coop");Human(0,1);Human(6,2);Human(11,2);room[8]=1;
    Put<uint32_t>(room.data(),kRoomPlayers,3);nativeAssignDuringSetup=true;failBuild=100;
    const auto coopPool=npcIds;npcIds={100,101};coopBefore=room;
    CHECK(AddUsingNativePoolFixture(true)==0x0FBE);
    CHECK(leavePackets.empty());
    for(int slot:{0,6,11}) {
        CHECK(SlotTeam(room.data(),slot)==SlotTeam(coopBefore.data(),slot));
        CHECK(room[kSlotInfo+slot*kSlotInfoStride+kSlotInfoColour]==coopBefore[kSlotInfo+slot*kSlotInfoStride+kSlotInfoColour]);
    }
    CHECK(ViewRoom(room.data()).ai[1]==0 && ViewRoom(room.data()).ai[2]==0 && room[8]==1);
    failBuild=0;
    CHECK(AddUsingNativePoolFixture(true)==0 && ViewRoom(room.data()).humans[1]==3);
    CHECK(GuardedRemove(room.data()));npcIds=coopPool;
    SCENARIO("cooperative intent is recalculated after host side and room capacity change");
    Human(6,2);Put<uint32_t>(room.data(),kRoomHost,16);room[kRoomCapacity]=6;
    CHECK(AddUsingNativePoolFixture(true)==0);
    CHECK(ViewRoom(room.data()).humans[2]==3 && ViewRoom(room.data()).ai[1]==3 && ViewRoom(room.data()).ai[2]==0);

    SCENARIO("asymmetric host-relative AI populations preserve native human sides and occupied slots");
    for(int mode:{1,3,6,7,9})for(uint8_t hostTeam:{uint8_t{1},uint8_t{2}})for(int humans:{1,3,4})for(uint8_t randomTeams:{uint8_t{0},uint8_t{1}}) {
        Reset(mode);Title("!ai e* a0");
        room[8]=randomTeams; // Native randomization has already resolved the human sides.
        const int occupied[]={0,6,11,4};
        for(int i=0;i<humans;++i)Human(occupied[i],hostTeam,static_cast<int>(3-hostTeam));
        Put<uint32_t>(room.data(),kRoomPlayers,static_cast<uint32_t>(humans));
        std::vector<std::vector<uint8_t>> before;
        for(int i=0;i<humans;++i) {
            const auto* info=room.data()+kSlotInfo+occupied[i]*kSlotInfoStride;
            before.emplace_back(info,info+kSlotInfoStride);
        }
        const uint8_t enemyTeam=static_cast<uint8_t>(3-hostTeam);
        CHECK(AddUsingNativePoolFixture());
        const auto after=ViewRoom(room.data());
        CHECK(after.hostTeam==hostTeam && after.humans[hostTeam]==humans && after.humans[enemyTeam]==0);
        CHECK(after.ai[hostTeam]==0 && after.ai[enemyTeam]==8-humans);
        CHECK(room[8]==randomTeams);
        for(int i=0;i<humans;++i)CHECK(std::memcmp(before[i].data(),room.data()+kSlotInfo+occupied[i]*kSlotInfoStride,kSlotInfoStride)==0);
        RecountBattle(room.data());
        CHECK(room[kRoomTeamSize1]==(hostTeam==1 ? humans:8-humans));
        CHECK(room[kRoomTeamSize2]==(hostTeam==2 ? humans:8-humans));
        if(mode==3) {
            CHECK(Get<uint32_t>(room.data(),hostTeam==1?kRoomPower1:kRoomPower2)==static_cast<uint32_t>(humans*1000));
            CHECK(Get<uint32_t>(room.data(),enemyTeam==1?kRoomPower1:kRoomPower2)==static_cast<uint32_t>((8-humans)*1000));
        }
        if(mode==6) {
            const int expected=humans==1 ? 160:humans==3 ? 120:80;
            CHECK(Get<int32_t>(room.data(),kRoomGauge1)==expected && Get<int32_t>(room.data(),kRoomGauge2)==expected);
        }
    }
    Reset();

    SCENARIO("room fill and exact commands retain human choices on either host side");
    for(const char* title:{"!ai","!ai fill","!ai e* a*"})
      for(uint8_t hostTeam:{uint8_t{1},uint8_t{2}})
      for(int allies=1;allies<=4;++allies)for(int enemies=0;enemies<=4;++enemies) {
        Reset();Title(title);
        for(int i=0;i<allies;++i)Human(i,hostTeam);
        for(int i=0;i<enemies;++i)Human(6+i,3-hostTeam);
        auto original=room;
        CHECK(AddUsingNativePoolFixture());
        const auto filled=ViewRoom(room.data());
        CHECK(filled.hostTeam==hostTeam && filled.humans[hostTeam]==allies && filled.humans[3-hostTeam]==enemies);
        CHECK(filled.ai[hostTeam]==4-allies && filled.ai[3-hostTeam]==4-enemies);
        for(int i=0;i<kSlots;++i)if(SlotPilotId(original.data(),i))
            CHECK(std::memcmp(original.data()+kSlotInfo+i*kSlotInfoStride,room.data()+kSlotInfo+i*kSlotInfoStride,kSlotInfoStride)==0);
      }
    for(uint8_t hostTeam:{uint8_t{1},uint8_t{2}}) {
        Reset();Human(0,hostTeam);Human(6,3-hostTeam);Title("!ai e4 a2");
        CHECK(AddUsingNativePoolFixture());
        const auto exact=ViewRoom(room.data());
        CHECK(exact.ai[hostTeam]==2 && exact.ai[3-hostTeam]==4);
    }
    Reset();

    SCENARIO("invalid populations do not silently start with fewer AI and failed partial setup can retry");
    for(int error:{0x0FBE,0x0FBA}) {
        Reset();Human(0,1);Title("!ai enemy");nativeHumanError=error;
        const auto before=room;
        CHECK(AddUsingNativePoolFixture(true)==error && nativeHumanCalls==1 && buildCalls==0 && room==before);
    }
    Reset();
    Human(0,1);Title("!ai e8 a0");
    auto originalHumanRoom=room;
    CHECK(AddUsingNativePoolFixture(true)==0x0FBE);
    CHECK(room==originalHumanRoom && buildCalls==0);
    Reset();Human(0,1);Title("!ai e* a0");
    const auto normalPool=npcIds;npcIds={100,101};failBuild=100;
    originalHumanRoom=room;
    CHECK(AddUsingNativePoolFixture(true)==0x0FBE);
    CHECK(ViewRoom(room.data()).ai[1]==0 && ViewRoom(room.data()).ai[2]==0);
    CHECK(std::memcmp(room.data()+kSlotInfo,originalHumanRoom.data()+kSlotInfo,kSlotInfoStride)==0);
    failBuild=0;
    CHECK(AddUsingNativePoolFixture(true)==0);
    CHECK(ViewRoom(room.data()).ai[1]==0 && ViewRoom(room.data()).ai[2]==7);
    npcIds=normalPool;
    Reset();

    SCENARIO("enemy-only fill respects the native two four and six player room limits");
    for(uint8_t capacity:{uint8_t{2},uint8_t{4},uint8_t{6}}) {
        Reset();Human(0,2);room[kRoomCapacity]=capacity;Title("!ai enemy");
        CHECK(AddUsingNativePoolFixture());
        CHECK(ViewRoom(room.data()).ai[1]==capacity-1 && ViewRoom(room.data()).ai[2]==0);
    }
    Reset();Human(0,1);room[kRoomCapacity]=0;Title("!ai enemy");
    CHECK(!AddUsingNativePoolFixture() && buildCalls==0);
    Reset();

    SCENARIO("room view, host team and spawn colours");
    Human(0, 2, 1); Human(1, 1, 2); Put(room.data(), kRoomHost, 10u); Title("!ai e2 a1");
    auto v = ViewRoom(room.data());
    CHECK(v.humans[1] == 1 && v.humans[2] == 1 && v.hostTeam == 2);
    CHECK(std::strcmp(v.title, "!ai e2 a1") == 0);
    CHECK(TeamColour(room.data(), 1) == 2 && TeamColour(room.data(), 2) == 1);
    Human(1, 2, 1); CHECK(TeamColour(room.data(), 1) == 2);
    room[kRoomTitleLength] = 255; v = ViewRoom(room.data()); CHECK(std::strlen(v.title) <= kRoomTitleMax);

    SCENARIO("explicit zero bots retain native start checks");
    std::vector<uint8_t> frame(kDispatcherRoomCopy + 16);
    auto copy = frame.data(); copy[kRoomMode] = 1; copy[kRoomTitleLength] = 4;
    std::memcpy(copy + kRoomTitle, "!ai0", 4);
    CHECK(!DecideSkipChecks(frame.data() + kDispatcherRoomCopy, false));
    copy[kRoomTitleLength] = 3; CHECK(DecideSkipChecks(frame.data() + kDispatcherRoomCopy, false));
    for (int mode : {5, 8, 10, 11, 12}) { copy[kRoomMode] = static_cast<uint8_t>(mode); CHECK(!DecideSkipChecks(frame.data() + kDispatcherRoomCopy, false)); }

    SCENARIO("life cost and first-life power");
    for (int mode : {3, 14}) {
        Reset(mode); CHECK(AddAiPilot(room.data(), 6, 100, 2, 1));
        CHECK(Get<uint16_t>(Unit(6), kUnitLifeCost) == 455);
        CHECK(Get<uint16_t>(Pilot(6), kPilotPower) == 545);
        CHECK(Get<uint16_t>(Pilot(6), kPilotLifeCost) == 455);
        CHECK(Pilot(6)[4] == 2 && Pilot(6)[0x39] == 1);
    }
    for (int bad : {0, -1, 1001}) {
        Reset(3); g_settings.gradeCost[3] = bad; CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
        CHECK(Get<uint16_t>(Unit(6), kUnitLifeCost) == 500);
    }
    Reset(1); CHECK(AddAiPilot(room.data(), 0, 100, 1, 1)); CHECK(Get<uint16_t>(Unit(0), kUnitLifeCost) == 500);
    Reset(); failBuild = 100; CHECK(!AddAiPilot(room.data(), 6, 100, 2, 2)); CHECK(SlotPilotId(room.data(), 6) == 0);
    Reset(); failSetup = 100; CHECK(!AddAiPilot(room.data(), 6, 100, 2, 2)); CHECK(SlotPilotId(room.data(), 6) == 0);
    CHECK(std::all_of(Pilot(6), Pilot(6) + kPilotStride, [](uint8_t b) { return b == 0; }));

    SCENARIO("deck budget, unique units and setup failure");
    Reset(7); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    Put<uint16_t>(Pilot(6), kPilotPower, 321); Put<uint16_t>(Pilot(6), kPilotLifeCost, 654);
    BuildDeck(room.data(), 6, 100, npcIds, points);
    CHECK(Pilot(6)[kPilotTagCost] == 9 && Pilot(6)[kPilotTagUnits] >= 2 && Pilot(6)[kPilotTagUnits] <= 3);
    CHECK(Get<uint32_t>(Unit(6, 1), 0) != 100);
    CHECK(Get<uint16_t>(Pilot(6), kPilotPower) == 321 && Get<uint16_t>(Pilot(6), kPilotLifeCost) == 654);
    Reset(6); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2)); failSetup = 103;
    BuildDeck(room.data(), 6, 100, {100, 101, 102, 103, 101, 104}, {4, 3, 2, 5, 3, 0});
    CHECK(Pilot(6)[kPilotTagCost] == 9 && Pilot(6)[kPilotTagUnits] == 3 && Pilot(6)[kPilotForm] == 0);
    CHECK((std::set<uint32_t>{Get<uint32_t>(Unit(6, 1), 0), Get<uint32_t>(Unit(6, 2), 0)}) == (std::set<uint32_t>{101, 102}));

    SCENARIO("all eligible deck variants remain reachable across battles");
    std::set<uint32_t> laterUnits;
    for (unsigned seed = 1; seed <= 64; ++seed) {
        Reset(7); g_random = seed;
        CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
        BuildDeck(room.data(), 6, 100, npcIds, points);
        std::set<uint32_t> unique;
        int total = 0;
        for (int i = 0; i < Pilot(6)[kPilotTagUnits]; ++i) {
            auto id = Get<uint32_t>(Unit(6, i), 0);
            CHECK(unique.insert(id).second);
            total += Unit(6, i)[kUnitPoints];
            if (i) laterUnits.insert(id);
        }
        CHECK(total <= 10 && unique.size() <= 4);
    }
    CHECK(laterUnits.count(101) && laterUnits.count(102) && laterUnits.count(103));

    SCENARIO("native storage capacity is separate from team identity and preserves humans");
    Reset(); for (int i = 0; i < 5; ++i) Human(i, 1);
    auto before = std::vector<uint8_t>(Pilot(0), Pilot(0) + kPilotStride);
    CHECK(AddTeam(room.data(), 1, 100, {100}, {}) == 7);
    CHECK(SlotPilotId(room.data(), 5) & kAiPilotBit);
    CHECK(std::equal(before.begin(), before.end(), Pilot(0)));
    CHECK(SlotPilotId(room.data(), 6) & kAiPilotBit);

    SCENARIO("server gauges use the HUD's shared scale, including asymmetric teams");
    const int gauge[] = {0, 80, 80, 60, 80, 120, 160,160};
    for (int n = 1; n <= 7; ++n) {
        Reset(6); for (int i = 0; i < n; ++i) Human(i, 1);
        Human(11, 2); RecountBattle(room.data());
        CHECK(Get<int32_t>(room.data(), kRoomGauge1) == gauge[n]);
        CHECK(Get<int32_t>(room.data(), kRoomGauge2) == gauge[n]);
        CHECK(room[kRoomTeamSize1] == n && room[kRoomTeamSize2] == 1);
    }
    Reset(3); Human(0, 1); Human(1, 1); Human(6, 2); RecountBattle(room.data());
    CHECK(Get<int32_t>(room.data(), kRoomPower1) == 2000 && Get<int32_t>(room.data(), kRoomPower2) == 1000);
    Reset(7); Human(0, 1); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    Pilot(6)[kPilotTagCost] = 9; Pilot(6)[kPilotTagUnits] = 3;
    Put<int32_t>(room.data(), kRoomGauge1, 4); room[kRoomTagUnits1] = 1;
    RecountBattle(room.data());
    CHECK(Get<int32_t>(room.data(), kRoomGauge1) == 4 && Get<int32_t>(room.data(), kRoomGauge2) == 9);
    CHECK(room[kRoomTagUnits1] == 1 && room[kRoomTagUnits2] == 3);

    SCENARIO("only dead PvP AI respawns, unsuccessful native respawn stays dead");
    Reset(); Human(0, 1); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    RespawnAiPilots(room.data(), 99); CHECK(respawns == 0);
    Put<uint32_t>(Unit(6), kUnitHp, 0); revive = false;
    RespawnAiPilots(room.data(), 100); CHECK(respawns == 1 && refreshes == 0);
    revive = true; RespawnAiPilots(room.data(), 101); CHECK(respawns == 2 && refreshes == 1 && stateRespawns == 1);
    room[kRoomMode] = 11; Put<uint32_t>(Unit(6), kUnitHp, 0); RespawnAiPilots(room.data(), 102); CHECK(respawns == 2);

    SCENARIO("AI MVP preserves the real server host without rewriting native ranks or the MVP");
    Reset(3);Human(0,1);CHECK(AddAiPilot(room.data(),6,100,2,2));
    int resultOrder[2]={6,0},resultRanks[2]={1,2};
    ResultHost(room.data(),nullptr,resultOrder,resultRanks,1,2);
    CHECK(resultHostCalls==1 && resultHostTransfer==0 && Get<uint32_t>(room.data(),kRoomHost)==10);
    CHECK(resultOrder[0]==6 && resultOrder[1]==0 && resultRanks[0]==1 && resultRanks[1]==2);
    Human(1,2);resultOrder[0]=1;
    ResultHost(room.data(),nullptr,resultOrder,resultRanks,1,2);
    CHECK(resultHostTransfer==1 && Get<uint32_t>(room.data(),kRoomHost)==11);
    room[kRoomMode]=5;resultOrder[0]=6;
    ResultHost(room.data(),nullptr,resultOrder,resultRanks,1,2);
    CHECK(resultHostTransfer==1);
    Reset(3);Human(0,1);CHECK(AddAiPilot(room.data(),6,100,2,2));

    SCENARIO("battle end removes AI, preserves humans and releases diagnostic state");
    room[kRoomMode] = 1; Put<uint32_t>(room.data(), 0, 0x1357u);
    Put<uint32_t>(room.data(), kRoomPlayers, 1u);
    CHECK(AddAiPilot(room.data(), 7, 101, 2, 2));
    const uint32_t firstAi = SlotPilotId(room.data(), 6);
    const uint32_t secondAi = SlotPilotId(room.data(), 7);
    const uint32_t hostBeforeEnd = Get<uint32_t>(room.data(), kRoomHost);
    const uint32_t humansBeforeEnd = Get<uint32_t>(room.data(), kRoomPlayers);
    before.assign(Pilot(0), Pilot(0) + kPilotStride);
    FindDead(room.data(), SlotPilotId(room.data(), 6), true, 77);
    g_watch[0].room = room.data();
    BattleEnd(room.data(), nullptr, 0, 0, 100);
    CHECK(originals == 1 && SlotPilotId(room.data(), 6) == 0 &&
          SlotPilotId(room.data(), 7) == 0 && SlotPilotId(room.data(), 0) == 10);
    CHECK(std::equal(before.begin(), before.end(), Pilot(0)));
    CHECK(Get<uint32_t>(room.data(), kRoomHost) == hostBeforeEnd);
    CHECK(Get<uint32_t>(room.data(), kRoomPlayers) == humansBeforeEnd);
    CHECK(leavePackets.size() == 2);
    CHECK(leaveOriginalReturned.size() == 2 &&
          std::all_of(leaveOriginalReturned.begin(), leaveOriginalReturned.end(),
                      [](bool value) { return value; }));
    CHECK(leaveAiStillPresent.size() == 2 &&
          std::all_of(leaveAiStillPresent.begin(), leaveAiStillPresent.end(),
                      [](bool value) { return value; }));
    if (leavePackets.size() == 2) {
        CHECK(Get<uint32_t>(leavePackets[0].data(), 0x06) == 0x1357u);
        CHECK(Get<uint32_t>(leavePackets[0].data(), 0x0A) == humansBeforeEnd);
        CHECK(Get<uint32_t>(leavePackets[0].data(), 0x0E) == hostBeforeEnd);
        CHECK(Get<uint32_t>(leavePackets[0].data(), 0x12) == 6u);
        CHECK(Get<uint32_t>(leavePackets[0].data(), 0x16) == firstAi);
        CHECK(Get<uint32_t>(leavePackets[1].data(), 0x12) == 7u);
        CHECK(Get<uint32_t>(leavePackets[1].data(), 0x16) == secondAi);
    }
    CHECK(g_dead[0].room == nullptr && g_watch[0].room == nullptr);
    RemoveAiPilots(room.data(), false); CHECK(SlotPilotId(room.data(), 0) == 10);

    SCENARIO("battle-end cleanup preserves a replacement occupant if a slot changes after its notice");
    Reset(1); Human(0, 1); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    const uint32_t replacedAi = SlotPilotId(room.data(), 6);
    FindDead(room.data(), replacedAi, true, 77);
    replaceSlotAfterBroadcast = 6;
    replacementPilotId = 16;
    BattleEnd(room.data(), nullptr, 0, 0, 100);
    CHECK(leavePackets.size() == 1);
    CHECK(SlotPilotId(room.data(), 6) == replacementPilotId);
    CHECK(std::all_of(Pilot(6), Pilot(6) + kPilotStride,
                      [](uint8_t value) { return value == 0x5A; }));
    CHECK(g_dead[0].room == nullptr);

    SCENARIO("battle-end observer preserves the real battle identity and publishes only after the native end returns");
    Reset(1);
    room[0x09] = 0;
    room[0x0A] = 58;
    Human(0, 1);
    Put(room.data(), kRoomHost, 10u);
    Title("T7x !ai");
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    const SdgoPvpAiObserverSinkV1 sink = {
        sizeof(sink), SDGO_PVP_AI_OBSERVER_VERSION, &Observe, nullptr};
    CHECK(SdgoPvpAiRegisterObserverV1(&sink) == 1);
    SdgoPvpAiObserverEventV1 randomSelection = {};
    room[0x09] = 1;
    CHECK(ObserverCopyEvent(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, room.data(),
                            0, true, &randomSelection));
    CHECK(randomSelection.manual_map_valid == 0);
    room[0x09] = 0;
    InterlockedExchangePointer(&g_observerActiveRooms[0], room.data());
    clearIdentityAtEnd = true;
    BattleEnd(room.data(), nullptr, 0, 0, 100);
    CHECK(originals == 1);
    CHECK(observerEvents.size() == 1);
    if (observerEvents.size() == 1) {
        const auto& end = observerEvents.front();
        CHECK(end.type == SDGO_PVP_AI_EVENT_BATTLE_END);
        CHECK(end.room_cookie ==
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(room.data())));
        CHECK(end.host_pilot_id == 10);
        CHECK(end.mode == 1);
        CHECK(end.manual_map_valid == 1);
        CHECK(end.map_selection == 0);
        CHECK(end.map_number == 58);
        CHECK(std::strcmp(end.title, "T7x !ai") == 0);
        CHECK(end.active_slots == 2);
        CHECK(end.ai_team_2 == 1);
        CHECK(end.slots[0].pilot_id == 10);
        CHECK(end.slots[6].pilot_id & kAiPilotBit);
        CHECK(end.sequence == 1);
        CHECK(end.qpc != 0);
        CHECK(end.thread_id == GetCurrentThreadId());
    }
    CHECK(Get<uint32_t>(room.data(), kRoomHost) == 0);
    CHECK(room[kRoomTitleLength] == 0);
    CHECK(SlotPilotId(room.data(), 6) == 0);
    CHECK(!ObserverRoomActive(room.data()));

    SCENARIO("expanded eligible pools are sampled afresh in both deck modes");
    auto originalIds = npcIds; auto originalPoints = points;
    for (int generation = 1; generation <= 2; ++generation)
        for (int level = 1; level <= 2; ++level)
            for (int i = 0; i < 60; ++i) {
                npcIds.push_back(static_cast<uint32_t>(1000 * generation + 100 * (level - 1) + i));
                points.push_back(static_cast<uint8_t>(level + 1));
            }
    std::set<uint32_t> expandedSeen;
    for (int mode : {6, 7}) {
        for (int level : {1, 2}) {
            for (int generation : {1, 2}) {
                std::vector<uint32_t> pool;
                for (int i = 0; i < 60; ++i)
                    pool.push_back(static_cast<uint32_t>(1000 * generation + 100 * (level - 1) + i));
                for (unsigned seed = 1; seed <= 24; ++seed) {
                    Reset(mode); g_random = seed;
                    CHECK(AddTeam(room.data(), 2, 3, pool, PoolPoints(room.data(), pool)) == 3);
                    for (int slot = 6; slot < 9; ++slot) {
                        std::set<uint32_t> unique;
                        int total = 0;
                        for (int i = 0; i < Pilot(slot)[kPilotTagUnits]; ++i) {
                            uint32_t id = Get<uint32_t>(Unit(slot, i), 0);
                            uint32_t first = static_cast<uint32_t>(1000 * generation + 100 * (level - 1));
                            CHECK(id >= first && id < first + 60);
                            CHECK(unique.insert(id).second);
                            total += Unit(slot, i)[kUnitPoints]; expandedSeen.insert(id);
                        }
                        CHECK(total <= 10 && unique.size() <= 4 && unique.size() >= 2);
                    }
                }
            }
        }
    }
    CHECK(expandedSeen.size() >= 160);
    npcIds = originalIds; points = originalPoints;

    SCENARIO("deck selection excludes zero-cost and over-budget first units");
    npcIds.push_back(105); points.push_back(11);
    for (int mode : {6, 7}) {
        for (unsigned seed = 1; seed <= 24; ++seed) {
            Reset(mode); g_random = seed;
            std::vector<uint32_t> candidate{104, 105, 100};
            CHECK(AddTeam(room.data(), 2, 1, candidate, PoolPoints(room.data(), candidate)) == 1);
            CHECK(Get<uint32_t>(Unit(6), 0) == 100);
            CHECK(Pilot(6)[kPilotTagCost] == 4);
        }
        Reset(mode);
        CHECK(AddTeam(room.data(), 2, 1, {104, 105}, {0, 11}) == 0);
    }
    SCENARIO("actual built points cannot exceed the remaining deck budget");
    npcIds.push_back(106); points.push_back(8);
    Reset(7); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    BuildDeck(room.data(), 6, 100, {100, 106}, {4, 1});
    CHECK(Pilot(6)[kPilotTagCost] == 4 && Pilot(6)[kPilotTagUnits] == 1);
    npcIds = originalIds; points = originalPoints;
    SCENARIO("concurrent rooms do not lose random-state updates");
    g_random = 1234567;
    std::vector<unsigned> draws(4096);
    std::vector<std::thread> threads;
    for (size_t worker = 0; worker < 4; ++worker)
        threads.emplace_back([&, worker] {
            for (size_t i = worker * 1024; i < (worker + 1) * 1024; ++i) draws[i] = NextRandom();
        });
    for (auto& thread : threads) thread.join();
    CHECK(std::set<unsigned>(draws.begin(), draws.end()).size() == draws.size());
    CHECK(std::find(draws.begin(), draws.end(), 0u) == draws.end());
    SCENARIO("Ace snapshot sink copies post-native results outside the provider lock");
    Reset(7); Human(0, 1); Title("!ai hard");
    EmitAceSnapshot(reinterpret_cast<uint8_t*>(1)); // no sink must not inspect a room
    SdgoAceAiServerObserverSinkV1 aceSink{
        sizeof(aceSink), SDGO_ACE_AI_OBSERVER_VERSION, &ObserveAce, &aceObserverUserCookie};
    CHECK(SdgoPvpAiRegisterAceObserverV1(nullptr) == 0);
    auto invalidAceSink = aceSink;
    invalidAceSink.version = 0;
    CHECK(SdgoPvpAiRegisterAceObserverV1(&invalidAceSink) == 0);
    invalidAceSink = aceSink; invalidAceSink.callback = nullptr;
    CHECK(SdgoPvpAiRegisterAceObserverV1(&invalidAceSink) == 0);
    CHECK(SdgoPvpAiRegisterAceObserverV1(&aceSink) == 1);
    CHECK(SdgoPvpAiRegisterAceObserverV1(&aceSink) == 0);
    g_aceSettings.enabled = true;
    g_aceSettings.byDifficulty[2] = {100, battle_ai::Difficulty::Hard, 120, 150};
    CHECK(PrepareRoomItems(room.data(), battle_ai::Difficulty::Hard));
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    ApplyAceStats(room.data(), 6);
    EmitAceSnapshot(room.data());
    CHECK(aceObserverEvents.size() == 1);
    CHECK(aceObserverEvents.back().unit_count == 1 &&
          aceObserverEvents.back().units[0].max_hp == 18000 &&
          aceObserverEvents.back().units[0].original_max_hp == 15000);
    uint8_t aceWeapon[8]{};
    CHECK(AcceptedDamage(room.data(), nullptr, 100, 6, Pilot(6), aceWeapon,
                         0, 10, 200, 0, 0, 0, 0, 0) == 300);
    CHECK(aceObserverEvents.size() == 2);
    CHECK(aceObserverEvents.back().damage.proposed_damage == 200 &&
          aceObserverEvents.back().damage.adjusted_damage == 300 &&
          aceObserverEvents.back().damage.native_result == proposedDamageSeen &&
          proposedDamageSeen == 300);
    CHECK(AcceptedDamage(room.data(), nullptr, 101, 6, Pilot(6), aceWeapon,
                         0, 10, 200, 0, 0, 0, 0, 1) == 200);
    CHECK(aceObserverEvents.size() == 3 &&
          aceObserverEvents.back().damage.attack_type == 1 &&
          aceObserverEvents.back().damage.adjusted_damage == 200);
    CHECK(Get<uint32_t>(Unit(6), kUnitMaxHp) == 18000);

    SCENARIO("a faulty optional Ace sink cannot repeat or prevent a native damage call");
    Reset(); Human(0, 1);
    CHECK(PrepareRoomItems(room.data(), battle_ai::Difficulty::Medium));
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    aceSink.callback = &ThrowAceObserver;
    CHECK(SdgoPvpAiRegisterAceObserverV1(&aceSink) == 1);
    CHECK(AcceptedDamage(room.data(), nullptr, 102, 6, Pilot(6), aceWeapon,
                         0, 10, 200, 0, 0, 0, 0, 0) == 200);
    CHECK(InterlockedCompareExchangePointer(&g_aceObserverCallback, nullptr, nullptr) == nullptr);
    CHECK(nativeDamageCalls == 1);
    CHECK(AcceptedDamage(room.data(), nullptr, 103, 6, Pilot(6), aceWeapon,
                         0, 10, 201, 0, 0, 0, 0, 0) == 201);
    CHECK(nativeDamageCalls == 2);
    DeleteCriticalSection(&g_itemsLock);
    return ai_test::Finish();
}
