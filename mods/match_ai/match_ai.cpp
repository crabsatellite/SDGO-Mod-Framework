// match_ai - client side of the PvP AI players.
//
// The battle server mod pvp_ai adds AI pilots to a PvP room (title "!ai e3 a1 lv4") the way
// the server's AI battle mode adds its own. The client puts them into room+0x3C (records of
// 0x344 bytes, after the humans in room+0x1C), but the PvP game modes only spawn the humans:
// CNormalGameMode, CDeathMatchGameMode, CGridMatchGameMode, CLeaderMatchGameMode and
// CTagMatchGameMode (the CNetworkGameMode family; the native AI modes derive from
// CMissionMode instead). Right after each one's Init (vftable slot 4) this spawns the AI
// records with the steps of CAINormalGameMode::Init's AI loop (0x004049B3..0x00404DA5):
//   start point = UM->FindStartPoint(record team, index)           (0x00499930)
//   unit        = mode->CreateNpc(npc data, x, y, z, team, 0, 0, 1)  vt slot 13 (team = record
//                 +0x19, as the PvP Inits give their units; colour +0x1A only picks the start point); for this
//                 family the CBaseGameMode version (0x00ACA0B0): a CNPC that runs the Lua
//                 AI named by its NPC data (InitNPCAI, 0x0065A3A0), the AI battle bots' VSAI_*
//   UM->SetPlayerInfo(slot, 0, unit+4, record+4, record)            (0x0049DFE0)
//   UM->PlayerSlot(slot)+8 = NPC name (npc data +5)                 (0x004039E0, 0x004026F0)
//   UM->AddAiUnit(unit); unit+0x2B68 = record+0 (server id)         (0x0049DB40)
//   UM->CalculateTeam()                                             (0x0049E0F0)
//
// The CBaseGameMode CreateNpc leaves out what CScriptControlMode::CreateNpc (0x00ADF970)
// and the mode's NPC registration (CMissionMode vt slot 24, 0x00ACED20) do for a network
// game, so right after it this does those steps, minus the mission-only ones (objective
// list, the mode as the unit's owner, whose callbacks are mission vftable slots):
//   unit+0x3238 = npc data id; UM->AddObject(unit, 0, 0) (0x0049BA20: object id, world);
//   on the room master: +0x31ED = 0, +0x2B64 = 1, +0x3074 = 1, +0x2B65 = 0, +0x3070 = 2.
//
// A unit's AI runs only once it is switched on (CNPC::EnableAI, unit+0x326C). The game does
// that for every AI unit at GameReady phase 3, but only in the script modes; for a PvP battle with
// AI players this mod waits for the same player-control release (GameReady, 0x004B2390). The AI scripts
// need the Lua bindings the script modes register in their Init (0x0050FAB0), and their
// target searches call script mode vftable slots; both are provided for the PvP modes.
//
// The AI runs on the room master, found the way CMissionMode::Init finds it: the local
// player's record (record+0 == room+0x68) has record+0x1C set. The other clients create the
// same units as remote copies (unit+0x2B65 = 1), as the mission NPC registration does on a
// client that is not the master; creating them in the same order gives them the same object
// ids. The start point index of an AI player follows the humans of its team, as the PvP Init
// counts them per team. As in the AI battle mode, the player count (UM+0x8C) then counts the
// AI players too (the HUD sums team scores over the counted slots), and the unit manager's
// object id -> player slot steps answer for them the way that mode's PlayerSlot remap does.

#include <windows.h>
#include "sdgo/mission_ai_lifecycle.h"

static SdgoMissionClientLifecycleV1* g_missionLifecycle = nullptr;

static bool MissionClientLifecycleSupported(
    const SdgoMissionClientLifecycleV1* hooks)
{
    if (!hooks || !hooks->object_for_pilot || !hooks->slot_for_object ||
        !hooks->keep_dead) return false;
    if (hooks->version == 1) return hooks->size == sizeof(*hooks);
    if (hooks->version != 2 ||
        hooks->size < sizeof(SdgoMissionClientLifecycleV2)) return false;
    const auto* hooksV2 =
        reinterpret_cast<const SdgoMissionClientLifecycleV2*>(hooks);
    return hooksV2->respawn_complete != nullptr && hooksV2->ready != nullptr;
}

static const SdgoMissionClientLifecycleV2* MissionClientLifecycleV2()
{
    auto* hooks = reinterpret_cast<SdgoMissionClientLifecycleV1*>(
        InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(&g_missionLifecycle),
            nullptr, nullptr));
    if (!hooks || hooks->version < 2 ||
        hooks->size < sizeof(SdgoMissionClientLifecycleV2)) return nullptr;
    return reinterpret_cast<const SdgoMissionClientLifecycleV2*>(hooks);
}

extern "C" __declspec(dllexport) int __cdecl SdgoMatchAiRegisterMissionV1(
    const SdgoMissionClientLifecycleV1* hooks)
{
    if (!MissionClientLifecycleSupported(hooks)) return 0;
    auto previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(&g_missionLifecycle),
        const_cast<SdgoMissionClientLifecycleV1*>(hooks), nullptr);
    return previous == nullptr || previous == hooks;
}
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "../battle_ai/difficulty.h"
#include "../battle_ai/ace_policy.h"
#include "../battle_ai/native_map_transport.h"
#include "../battle_ai/native_map_gate.h"
#include "sdgo/room_titles_current.h"
#include "sdgo/ai_scoring.h"
#include "sdgo/mission_ai_forced_pad_observer.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "sdgo/ace_ai_observer.h"
#include "forced_pad_observer.h"
#include "transform_notice.h"
#include "gate_status_observer.h"
#include "room_host.h"
#include "sdgo/match_ai_events.h"

namespace {
    SdgoMatchAiEventsV1 g_eventSinks[8]{};
    unsigned g_eventSinkCount=0;
    void NotifyTick(uint8_t* mode,uint32_t sequence) {
        for(unsigned i=0;i<g_eventSinkCount;++i)if(g_eventSinks[i].tick)
            g_eventSinks[i].tick(mode,sequence,g_eventSinks[i].user);
    }
    void NotifyPacket(uint16_t opcode,const uint8_t* packet) {
        for(unsigned i=0;i<g_eventSinkCount;++i)if(g_eventSinks[i].packet)
            g_eventSinks[i].packet(opcode,packet,g_eventSinks[i].user);
    }
}
extern "C" __declspec(dllexport) int __cdecl SdgoMatchAiRegisterEventsV1(const SdgoMatchAiEventsV1* sink) {
    if(!sink||sink->size!=sizeof(*sink)||sink->version!=1||(!sink->tick&&!sink->packet))return 0;
    for(unsigned i=0;i<g_eventSinkCount;++i)
        if(g_eventSinks[i].tick==sink->tick&&g_eventSinks[i].packet==sink->packet&&g_eventSinks[i].user==sink->user)return 1;
    if(g_eventSinkCount>=8)return 0;
    // Registration happens during queued Mod load, before native startup.
    g_eventSinks[g_eventSinkCount++]=*sink;return 1;
}

namespace
{
    constexpr size_t kModeRoom = 0x08;          // room data*
    constexpr size_t kModeRoomMode = 0x66;      // u16, room data: battle mode
    constexpr size_t kRoomHumansBegin = 0x1C;   // vector of 0x344-byte records
    constexpr size_t kRoomHumansEnd = 0x20;
    constexpr size_t kRoomAiBegin = 0x3C;
    constexpr size_t kRoomAiEnd = 0x40;
    constexpr size_t kRoomLocalPlayer = 0x68;   // u32, the local player's id
    constexpr size_t kRecordSize = 0x344;
    constexpr size_t kRecordServerId = 0x00;
    constexpr size_t kRecordConnection = 0x04;
    constexpr size_t kRecordMaster = 0x1C;      // u8, set on the room master's record
    constexpr size_t kRecordTeam = 0x19;        // u8, team (1, 2): the unit's team (unit +0x20)
    constexpr size_t kRecordColour = 0x1A;      // u8, respawn colour (1, 2): start points go by it
    constexpr size_t kRecordNpcData = 0x44;
    constexpr size_t kStartPointPos = 0x24;     // x, y, z floats
    constexpr size_t kUnitObjectId = 0x04;
    constexpr size_t kUnitCurrentHp = 0x38;
    constexpr size_t kUnitMaxHp = 0x3C;
    constexpr size_t kUnitServerId = 0x2B68;
    constexpr size_t kUnitNpcData = 0x320C;     // npc data*, name (wchar) at +5
    constexpr size_t kUnitNpcDataId = 0x3238;   // u32
    constexpr size_t kUnitLocalControl = 0x2B64; // u8, 0x00670290
    constexpr size_t kUnitRemote = 0x2B65;      // u8, 0x006702A0 (1: no local AI)
    constexpr size_t kUnitNpcState = 0x3070;    // u32, 0x0065EEF0
    constexpr size_t kUnitMasterNpc = 0x3074;   // u8
    constexpr size_t kUnitNpcFlag = 0x31ED;     // u8
    constexpr size_t kUnitAiEnabled = 0x326C;   // u8
    constexpr size_t kUnitPendingControl = 0x30D8;
    constexpr size_t kSlotName = 0x08;          // std::wstring in the player slot
    constexpr size_t kModeCreateNpcSlot = 13;

    using ModeInitFn = void(__thiscall*)(uint8_t* mode);
    using IsNetworkFn = bool(__cdecl*)();
    using FindStartPointFn = uint8_t*(__thiscall*)(void* um, int team, int index);
    using IsObserverFn = bool(__thiscall*)(void* um);
    using CreateNpcFn = uint8_t*(__thiscall*)(uint8_t* mode, uint32_t npcData, float x, float y, float z, int team,
                                              float dx, float dy, float dz);
    using SetPlayerInfoFn = void(__thiscall*)(void* um, int slot, int isLocal, uint32_t objectId, uint32_t connection,
                                              uint8_t* record);
    using PlayerSlotFn = uint8_t*(__thiscall*)(void* um, int slot);
    using WStringAssignFn = void(__thiscall*)(void* string, const wchar_t* text, size_t length);
    using AddAiUnitFn = void(__thiscall*)(void* um, uint8_t* unit);
    // Native 0x0049BA20 exposes a boolean in AL but does not normalize the
    // upper 24 bits of EAX.  Keep the complete machine return so an observer
    // detour can return the trampoline's value byte-for-byte.
    using AddObjectFn = uint32_t(__thiscall*)(void* um, uint8_t* unit, uint32_t id, int flag);
    using CalculateTeamFn = void(__thiscall*)(void* um);
    using GameReadyFn = void(__thiscall*)(uint8_t* service);
    using EnableAiFn = void(__thiscall*)(uint8_t* unit);
    using ErrorNameFn = const char*(__cdecl*)(int code);
    using UdpSetupFn = void(__thiscall*)(uint8_t* waitRoom);
    struct lua_State;
    using LuaOnErrorFn = int(__cdecl*)(lua_State* L);
    using LuaToStringFn = const char*(__cdecl*)(lua_State* L, int index);
    using PacketDispatchFn = int(__thiscall*)(void* self, int a, int b, int c, int opcode, void* packet);
    using RegisterLuaBindingsFn = void(__cdecl*)();
    using GameLogFn = void(__cdecl*)(void* logger, int level, const wchar_t* format, ...);
    using GameAppFn = uint8_t*(__cdecl*)();
    using ModeIsAFn = bool(__thiscall*)(uint8_t* mode, int kind);
    using LockTargetFn = int(__thiscall*)(void* mode, uint32_t objectId, char p2, char p3);
    using RunLuaFunctionFn = void(__thiscall*)(void* mode, int index);
    using LuaLockFn = int(__cdecl*)(uint32_t objectId);
    using LuaLockSelectFn = int(__cdecl*)(uint32_t objectId, int select);
    using LuaRunLuaFunctionFn = void(__cdecl*)(int index);
    using LuaTeamSearchFn = int(__cdecl*)(uint8_t* npc);
    using NatCheckFn = void(__thiscall*)(uint8_t* gameMain);
    using ObjectIdFromPilotFn = uint32_t(__thiscall*)(void* um, uint32_t pilotId);
    using SetSlotColourFn = void(__thiscall*)(void* um, int index, int colour);
    using FindObjectFn = void*(__thiscall*)(void* um, uint32_t objectId);
    using GameNewFn = void*(__cdecl*)(size_t size);
    using CameraUpdateFn = void(__thiscall*)(uint8_t* camera);
    using IsPlayerObjectFn = bool(__thiscall*)(void* um, uint32_t objectId);
    using SetPlayerCountFn = void(__thiscall*)(void* um, int count);
    using FindPlayerFn = void*(__thiscall*)(void* um, uint32_t objectId);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn = int(__thiscall*)(void* grid, const uint8_t** record, uint32_t id, int type, char log);
    using AddDeckUnitFn = void(__thiscall*)(uint8_t* unit, uint32_t unitDataId);
    using SwitchDeckFn = void(__thiscall*)(uint8_t* unit, int index, char flag);
    using PlaybackUnitFn = int(__thiscall*)(void* self, uint8_t* packet);
    using BattleAiInitializeUnitFn = void(__cdecl*)(uint8_t* unit, int roomDifficulty, uint32_t generation);
    using ForceMoveSenderFn = uint8_t*(__cdecl*)();
    using NativeStateFn = uint8_t(__thiscall*)(uint8_t* controller);
    using ForceMoveNoticeFn = int(__stdcall*)(const uint8_t* packet);
    using TransformNoticeFn = int(__stdcall*)(const uint8_t* packet);
    using UnitMessageFn = void(__thiscall*)(uint8_t* unit,const uint8_t* message);

    constexpr size_t kAppGameMain = 0x4A4;      // game-main service* of the game application
    constexpr size_t kServiceMode = 0x0C;       // game mode* of the game-main service
    constexpr size_t kServiceReadyState = 0x3C; // GameReady phases: 1 waiting, 2 countdown, 3 released
    constexpr size_t kManagerPlayer = 0x88;
    constexpr size_t kPlayerControlEnabled = 0x31E9; // set by native 0x00671170 at phase 2 -> 3
    using RosterInitFn = void(__thiscall*)(uint8_t*, void*, void*);
    using RosterRowFn = void(__thiscall*)(uint8_t*, uint8_t*, int);
    RosterInitFn g_rosterInit = nullptr;
    RosterRowFn g_originalRosterRow = nullptr;
    uint8_t* g_rosterPanel = nullptr;
    SdgoPvpAiClientScoreboardStatusV1 g_scoreboardStatus{};
    constexpr size_t kUnitOwner = 0x3268;       // owning script mode*, 0 from the CBaseGameMode CreateNpc
    constexpr int kModeIsASlot = 1;
    constexpr int kModeInitSlot = 4;
    constexpr size_t kWaitRoomSlots = 0xA00;    // CServiceWaitRoomMgr: pilot id per slot
    constexpr size_t kWaitRoomSlotStride = 0x48E;
    constexpr uint32_t kAiPilotBit = 0x80000000u;

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    match_ai_host::Bindings g_roomHost;
    using ChangeRoomHostFn=bool(__thiscall*)(uint8_t*,uint32_t);
    ChangeRoomHostFn g_changeRoomHost=nullptr;

    bool __fastcall ChangeRoomHost(uint8_t* model,void*,uint32_t candidate)
    {
        uint32_t selected=candidate;
        __try { selected=g_roomHost.Select(model,candidate); }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod,SDGO_LOG_WARN,"could not verify the native room host candidate");
        }
        if(selected!=candidate)
            g_api->Log(g_mod,SDGO_LOG_INFO,"AI room host candidate %08lX resolved to human %08lX",
                static_cast<unsigned long>(candidate),static_cast<unsigned long>(selected));
        // An empty human roster cannot grant an AI control of a player room.
        // A native no-change result (the selected human is already host) is
        // preserved, as are all ordinary transfers to another real player.
        return selected ? g_changeRoomHost(model,selected):false;
    }

    struct GameCalls
    {
        void** unitManager;
        IsNetworkFn isNetwork;
        FindStartPointFn findStartPoint;
        IsObserverFn isObserver;
        CreateNpcFn baseCreateNpc;
        SetPlayerInfoFn setPlayerInfo;
        PlayerSlotFn playerSlot;
        WStringAssignFn assign;
        AddAiUnitFn addAiUnit;
        AddObjectFn addObject;
        CalculateTeamFn calculateTeam;
        EnableAiFn enableAi;
        EnableAiFn disableAi;
        EnableAiFn releaseAttack;
        EnableAiFn moveStop;
        EnableAiFn resetInputs;
    };
    GameCalls g_game = {};
    GameReadyFn g_originalGameReady = nullptr;
    ErrorNameFn g_originalErrorName = nullptr;
    UdpSetupFn g_originalUdpSetup = nullptr;
    LuaOnErrorFn g_originalLuaOnError = nullptr;
    LuaToStringFn g_luaToString = nullptr;
    PacketDispatchFn g_originalPacketDispatch = nullptr;
    PacketDispatchFn g_originalServicePacket = nullptr;
    RegisterLuaBindingsFn g_registerLuaBindings = nullptr;
    GameLogFn g_originalGameLog = nullptr;
    GameAppFn g_gameApp = nullptr;
    uint8_t** g_profilePointer = nullptr;
    LockTargetFn g_lockTarget = nullptr;
    RunLuaFunctionFn g_runLuaFunction = nullptr;
    LuaLockFn g_originalLockPlayerA = nullptr;
    LuaLockFn g_originalLockPlayer = nullptr;
    LuaLockFn g_originalLockPlayerAibs = nullptr;
    LuaLockSelectFn g_originalLockTargetSelect = nullptr;
    LuaRunLuaFunctionFn g_originalRunLuaFunction = nullptr;
    LuaTeamSearchFn g_originalTeamSearch = nullptr;
    NatCheckFn g_originalNatCheck = nullptr;
    bool g_natCheckSkipped = false;
    bool g_creatingAiUnit = false;   // set while CreateNpc builds an AI player's unit
    int g_lastOpcode = -1;           // diagnostics: last server packet
    volatile LONG g_luaErrors = 0;   // current real battle; observed even when diagnostic logging is off
    volatile LONG g_luaErrorObserverAvailable = 0;
    ObjectIdFromPilotFn g_originalObjectIdFromPilot = nullptr;
    SetSlotColourFn g_originalSetSlotColour = nullptr;
    FindObjectFn g_findObject = nullptr;
    FindObjectFn g_originalFindObject = nullptr;
    GameNewFn g_gameNew = nullptr;
    CameraUpdateFn g_originalTrackingUpdate = nullptr;
    IsPlayerObjectFn g_originalIsPlayerObject = nullptr;
    PlayerSlotFn g_originalPlayerSlot = nullptr;
    SetPlayerCountFn g_setPlayerCount = nullptr;
    FindPlayerFn g_originalFindPlayer = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;
    AddDeckUnitFn g_addDeckUnit = nullptr;
    SwitchDeckFn g_switchDeck = nullptr;
    PlaybackUnitFn g_originalPlaybackUnit = nullptr;
    TransformNoticeFn g_originalTransformNotice = nullptr;
    UnitMessageFn g_originalUnitNetworkMessage=nullptr,g_unitCombatEvent=nullptr;
    struct TransformConfirmation {bool active=false;uint32_t pilot=0,object=0,target=0;};
    thread_local TransformConfirmation g_transformConfirmation{};
    struct MissionRespawnPending {
        void* unitManager = nullptr;
        uint32_t objectId = 0;
        int colour = -1;
    };
    thread_local std::vector<MissionRespawnPending> g_missionRespawnPending;
    thread_local SdgoAceAiMissionPlaybackContextV1 g_missionPlaybackContext{};
    uint8_t* g_missionReadyMode = nullptr;
    bool g_missionReadyComplete = false;
    HMODULE g_battleAiModule = nullptr;
    BattleAiInitializeUnitFn g_battleAiInitializeUnit = nullptr;
    using BattleAiBeginBattleFn = int(__cdecl*)(uint32_t);
    BattleAiBeginBattleFn g_battleAiBeginBattle = nullptr;
    HMODULE g_aceAiModule = nullptr;
    using AcePilotNameFn = int(__cdecl*)(uint32_t, wchar_t*, uint32_t);
    using AceAnnounceTeamFn = int(__cdecl*)(uint8_t*, uint32_t);
    AcePilotNameFn g_acePilotName = nullptr;
    AceAnnounceTeamFn g_aceAnnounceTeam = nullptr;
    ForceMoveSenderFn g_forceMoveSender = nullptr;
    battle_ai::native::SendForceMovePacketFn g_sendForceMovePacket = nullptr;
    NativeStateFn g_movementState = nullptr;
    NativeStateFn g_attackState = nullptr;
    ForceMoveNoticeFn g_originalForceMoveNotice = nullptr;
    battle_ai::native::MapTransportApi g_forcedPadApi{};
    bool g_forcedPadBridgeAvailable = false;

    // The AI players spawned for the current battle and its game mode; their AI is switched
    // on at game ready.
    uint8_t* g_botMode = nullptr;
    bool g_aiEnabled = false; // the AI players' AI was switched on at game ready
    SdgoPvpAiClientStartStatusV1 g_startStatus{};
    // The game mode of a battle with AI players whose only human is this client.
    uint8_t* g_soloAiMode = nullptr;

    // The AI players of the battle in g_botMode: server pilot id, object id, player slot.
    struct ForcedPadState
    {
        volatile LONG outstanding = 0;
        uint32_t generation = 0;
        uint32_t outstandingGeneration = 0;
        DWORD requestTick = 0;
        DWORD activationTick = 0;
        uint32_t requestCount = 0;
        uint32_t noticeCount = 0;
        uint32_t displacementCount = 0;
        uint32_t timeoutCount = 0;
        uint32_t sendFailureCount = 0;
        uint32_t ownershipLossCount = 0;
        uint32_t lastResult =
            static_cast<uint32_t>(battle_ai::native::MapTransportResult::InvalidContract);
        uint32_t lastClearReason = 0;
        volatile LONG requestVersion = 0;
        uint32_t requestSequence = 0;
        uint32_t padObjectId = 0;
        battle_ai::native::ForceMoveRequest085A request{};
        battle_ai::Vec3 activationPosition{};
        bool activationPositionValid = false;
        bool awaitingDisplacement = false;
        battle_ai::native::MapTransportWitness transport{};
    };

    struct Bot
    {
        uint32_t pilotId;
        uint32_t objectId;
        int slot;
        uint8_t* unit;
        // deck (modes 6, 7): NPC data (id and data*) per deck entry, the entry in play, a pending switch
        int units = 1;
        uint32_t deckNpc[4] = {};
        const uint8_t* deckNpcData[4] = {};
        int form = 0;
        int pendingForm = -1;
        uint32_t generation = 0;
        uint32_t startDeckGeneration = 0;
        bool pendingInitialize = false;
        bool missionLifeObserved = false;
        bool missionAlive = false;
        bool ace = false;
        uint8_t team = 0;
        uint8_t colour = 0;
        battle_ai::Difficulty effectiveDifficulty = battle_ai::Difficulty::Medium;
        std::wstring acePilotName;
        ForcedPadState forcedPad{};
    };
    std::vector<Bot> g_botTable;
    std::vector<Bot> g_missionPadBots;
    uint8_t* g_missionPadMode = nullptr;
    volatile LONG g_missionForcedPadTicks = 0;
    int g_roomDifficulty = 1;
    uint32_t g_nextUnitGeneration = 0;
    uint32_t g_nextBattleGeneration = 0;
    bool g_logPackets = false; // diagnostics: server packets from the spawn of AI players on
    int g_viewStateLogs = 0;   // diagnostics: dead view clicks logged this game
    bool g_logDiagnostics = false;
    bool g_aceAnnouncementDone[3] = {};
    bool g_aceAnnouncementProviderLogged = false;


    struct ModeClass
    {
        const char* name;
        const char* slot; // symbol of its vftable Init slot
        ModeInitFn original;
        const char* updateSlot;
        int(__thiscall* updateOriginal)(uint8_t*);
    };
    ModeClass g_classes[] = {
        {"normal", "CNormalGameMode.vtbl.Init", nullptr,"CNormalGameMode.vtbl.Update",nullptr},
        {"death match", "CDeathMatchGameMode.vtbl.Init", nullptr,"CDeathMatchGameMode.vtbl.Update",nullptr},
        {"grid match", "CGridMatchGameMode.vtbl.Init", nullptr,"CGridMatchGameMode.vtbl.Update",nullptr},
        {"leader match", "CLeaderMatchGameMode.vtbl.Init", nullptr,"CLeaderMatchGameMode.vtbl.Update",nullptr},
        {"tag match", "CTagMatchGameMode.vtbl.Init", nullptr,"CTagMatchGameMode.vtbl.Update",nullptr},
    };
    void** g_vtables[5] = {}; // vftable of each class, from its Init slot

    int ModeClassIndex(uint8_t* mode)
    {
        void** vtable = *reinterpret_cast<void***>(mode);
        for (int i = 0; i < 5; ++i)
            if (g_vtables[i] && vtable == g_vtables[i])
                return i;
        return -1;
    }

    // The client battle room begins with its native std::wstring title.  A missing or
    // malformed title keeps the requested default, Medium.  This is deliberately the same
    // parser battle_ai uses, so "!ai easy/medium/hard" has one meaning in both mods.
    int BattleDifficulty(const uint8_t* room)
    {
        char full[SDGO_ROOM_TITLE_MAX_UTF8_BYTES+1]={};
        const auto n=g_api&&g_api->ModDirectory?sdgo::CurrentRoomTitle(g_api->ModDirectory(g_mod),full,sizeof(full)):0;
        if(n&&n<=SDGO_ROOM_TITLE_MAX_UTF8_BYTES&&full[n]==0) {
            int value=battle_ai::RoomDifficulty(std::string_view(full,n));return value<0?1:value;
        }
        wchar_t title[21] = {};
        size_t length = 0;
        __try {
            if (!room)
                return 1;
            length = *reinterpret_cast<const uint32_t*>(room + 0x10);
            size_t capacity = *reinterpret_cast<const uint32_t*>(room + 0x14);
            if (length > 20 || capacity < 7 || capacity < length || capacity > 0x1000)
                return 1;
            const wchar_t* text =
                capacity < 8 ? reinterpret_cast<const wchar_t*>(room)
                             : *reinterpret_cast<const wchar_t* const*>(room);
            if (!text || text[length] != L'\0')
                return 1;
            memcpy(title, text, length * sizeof(wchar_t));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 1;
        }
        int difficulty = battle_ai::RoomDifficulty(std::wstring_view(title, length));
        return difficulty >= 0 && difficulty <= 2 ? difficulty : 1;
    }

    uint32_t NextUnitGeneration()
    {
        ++g_nextUnitGeneration;
        if (g_nextUnitGeneration == 0)
            ++g_nextUnitGeneration;
        return g_nextUnitGeneration;
    }

    void ResolveBattleAiBridge()
    {
        HMODULE module = GetModuleHandleW(L"battle_ai.dll");
        if (module != g_battleAiModule) {
            g_battleAiModule = module;
            g_battleAiInitializeUnit = nullptr;
            g_battleAiBeginBattle = nullptr;
        }
        if (module && !g_battleAiInitializeUnit)
            g_battleAiInitializeUnit = reinterpret_cast<BattleAiInitializeUnitFn>(
                GetProcAddress(module, "SdgoBattleAiInitializeUnit"));
        if (module && !g_battleAiBeginBattle)
            g_battleAiBeginBattle = reinterpret_cast<BattleAiBeginBattleFn>(
                GetProcAddress(module, "SdgoBattleAiBeginBattleV1"));
    }

    BattleAiInitializeUnitFn OptionalBattleAiInitializer()
    {
        ResolveBattleAiBridge();
        return g_battleAiInitializeUnit;
    }

    void BeginBattleAiProfiles()
    {
        ResolveBattleAiBridge();
        if (!g_battleAiBeginBattle) return;
        if (++g_nextBattleGeneration == 0) ++g_nextBattleGeneration;
        if (g_battleAiBeginBattle(g_nextBattleGeneration) != 1)
            g_api->Log(g_mod, SDGO_LOG_WARN, "battle AI profile registry did not accept battle generation %lu",
                       static_cast<unsigned long>(g_nextBattleGeneration));
    }

    void ResolveAceProvider()
    {
        HMODULE module = GetModuleHandleW(L"ace_ai.dll");
        if (module != g_aceAiModule) {
            g_aceAiModule = module;
            g_acePilotName = nullptr;
            g_aceAnnounceTeam = nullptr;
        }
        if (module && !g_acePilotName)
            g_acePilotName = reinterpret_cast<AcePilotNameFn>(
                GetProcAddress(module, "SdgoAceAiPilotNameV1"));
        if (module && !g_aceAnnounceTeam)
            g_aceAnnounceTeam = reinterpret_cast<AceAnnounceTeamFn>(
                GetProcAddress(module, "SdgoAceAiAnnounceTeamV1"));
    }

    int GuardedAcePilotName(uint32_t unitId, wchar_t* value, uint32_t capacity)
    {
        __try { return g_acePilotName ? g_acePilotName(unitId, value, capacity) : 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_acePilotName = nullptr; return 0; }
    }

    std::wstring AcePilotName(uint32_t unitId)
    {
        ResolveAceProvider();
        wchar_t value[65]{};
        if (GuardedAcePilotName(unitId, value,
                                static_cast<uint32_t>(sizeof(value) / sizeof(value[0]))) != 1)
            return {};
        return value;
    }

    void ApplyAcePilotName(void* um, const Bot& bot)
    {
        if (!um || !bot.ace || bot.acePilotName.empty() || bot.slot < 0) return;
        __try {
            uint8_t* slot = g_game.playerSlot(um, bot.slot);
            if (slot) g_game.assign(slot + kSlotName, bot.acePilotName.c_str(), bot.acePilotName.size());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "AI player %08lX: restoring Ace pilot name failed (%08lX)",
                       static_cast<unsigned long>(bot.pilotId), GetExceptionCode());
        }
    }

    void InitializeBattleAiUnit(Bot& bot, bool initialMatchSpawn = false)
    {
        BattleAiInitializeUnitFn initialize = OptionalBattleAiInitializer();
        if (!initialize || !bot.unit || bot.generation == 0)
            return;
        // The low byte is the public room difficulty.  0x100 marks only the first
        // completed spawn in this match; respawns and deck changes must retain SP.
        int difficultyAndFlags = static_cast<int>(bot.effectiveDifficulty) | (initialMatchSpawn ? 0x100 : 0);
        __try {
            initialize(bot.unit, difficultyAndFlags, bot.generation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "battle_ai initialization fault %08lX for AI player %08lX generation %lu; initialization aborted",
                       GetExceptionCode(), static_cast<unsigned long>(bot.pilotId),
                       static_cast<unsigned long>(bot.generation));
        }
    }

    // What each PvP Init sets in a player's slot (+0x48, the score the HUD sums per team)
    // right after SetPlayerInfo: death match (0x00AD98B0) 1000, the player's share of the
    // team power; tag match (0x00ADB390: the death match Init, then record +0x30 * 100, at
    // most 1000); grid match (0x00ADB790) 1; normal and leader match nothing.
    // In tag match record +0x30 is the player's deck points; PT_NTF_DEAD_UNIT divides by the
    // slot record's +0x30 (0x004D92CB). An AI player's record has none: it gets its deck's
    // points from the battle start packet (see the deck block below), else its one unit's
    // cost, the NPC points the client counts for an NPC (npc data +0xF1).
    constexpr size_t kSlotScore = 0x48;
    constexpr size_t kSlotRecordCopy = 0x44;    // the slot's copy of the player record
    constexpr size_t kRecordTagCost = 0x30;
    constexpr size_t kNpcPoints = 0xF1;

    int StartDeckPoints(uint32_t pilotId);

    void InitAiSlot(int classIndex, uint8_t* playerSlot, uint8_t* unit)
    {
        auto score = reinterpret_cast<int32_t*>(playerSlot + kSlotScore);
        if (classIndex == 1) {
            *score = 1000;
        } else if (classIndex == 2) {
            *score = 1;
        } else if (classIndex == 4) {
            uint8_t* record = *reinterpret_cast<uint8_t**>(playerSlot + kSlotRecordCopy);
            auto cost = reinterpret_cast<int32_t*>(record + kRecordTagCost);
            const uint8_t* npc = *reinterpret_cast<uint8_t**>(unit + kUnitNpcData);
            int deckPoints = StartDeckPoints(*reinterpret_cast<uint32_t*>(record));
            g_api->Log(g_mod, SDGO_LOG_INFO, "tag match: AI player %08lX record +0x30 = %ld, deck points %d, NPC points %ld",
                       static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(record)), static_cast<long>(*cost), deckPoints,
                       npc ? static_cast<long>(*reinterpret_cast<const int32_t*>(npc + kNpcPoints)) : -1L);
            if (*cost <= 0 && deckPoints > 0)
                *cost = deckPoints;
            if (*cost <= 0 && npc)
                *cost = *reinterpret_cast<const int32_t*>(npc + kNpcPoints);
            // The death handler divides by this value. Missing/invalid NPC
            // points must not leave a zero divisor, or overflow the HUD score.
            if (*cost <= 0)
                *cost = 1;
            *score = *cost <= 10 ? *cost * 100 : 1000;
        }
    }

    // ---- modes 6 and 7: the AI player's deck ---------------------------------------------
    //
    // The battle server gives an AI pilot of grid match (6) and tag match (7) a deck of NPC
    // units (tag match: one life each; grid match: the next one on each respawn). Its
    // battle start packet (PT_NTF_BATTLE_START 0x6C3, built by 0x004B61E0) has a 0x10F-byte
    // entry per slot from +0x20: pilot id (+0), deck points (+0xB, pilot +0x1CA0) and units
    // (+0xC), and the first 0x32 bytes of the 4 unit records (+0xE + i * 0x32), whose first
    // dword is an AI unit's NPC data id. Each wait room keeps its own copy (CServiceWaitRoomMgr
    // 0x00A3E590 at +0x4258; the 3D wait room's main state 0x00752E10 in a new 0xCD4-byte
    // buffer, 0x0074C9B0), so the decks are read where the packet reaches either: the service
    // manager's packet slot (CServiceManaged 0x00701DE0), which every game state calls (the
    // wait room's CGameState 0x004F7740, CGameMain 0x004DC3D0 only in battle). The records
    // the game modes get carry neither. A player's deck is built from its record by
    // 0x00665CC0, which skips NPC units, and its message 7 handler switches the deck on a
    // respawn; the NPC handler does neither. Here the AI unit gets the other deck units with
    // the same AddDeckUnit (unit data id = NPC data +0x69; NPC data is grid data type 0x29,
    // GridData::Find 0x0059D2B0), and on a respawn its NPC data is set to the new entry's
    // before message 7 (CNPC::Add re-inits the AI script from it) and the deck switched
    // after it, as CPlayer does.
    constexpr int kPacketBattleStart = 0x6C3;
    constexpr size_t kStartPacketSlots = 0x20;
    constexpr size_t kStartSlotStride = 0x10F;
    constexpr size_t kStartSlotPoints = 0x0B;
    constexpr size_t kStartSlotUnits = 0x0C;
    constexpr size_t kStartSlotUnitBlocks = 0x0E;
    constexpr size_t kStartUnitBlock = 0x32;

    // Native 0x004B61E0 copies the first 0x32 bytes of each authoritative
    // server unit record into this packet block.  Server unit +4 is its
    // post-policy maximum HP; +0x34 current HP is deliberately not copied.
    constexpr size_t kStartUnitMaxHp = 0x04;
    constexpr size_t kStartUnitLifeCost = 0x08; // u16, unit record +8: the life cost the server counts

    struct StartDeck
    {
        uint32_t pilotId;
        int points;
        int units;
        uint32_t npc[4];
        uint32_t maxHp[4];
        int lifeCost[4];
        uint32_t generation;
    };
    StartDeck g_startDecks[12] = {};
    uint32_t g_startDeckGeneration = 0;
    volatile LONG g_receivedBattleStartCount = 0;
    volatile LONG g_receivedBattleStartMode = 0;
    volatile LONG g_createReplyVersion = 0;
    volatile LONG g_createReplySequence = 0;
    volatile LONG g_createReplyRoom = 0;
    volatile LONG g_createReplyDetails = 0;

    void ReadStartDecks(const uint8_t* packet)
    {
        memset(g_startDecks, 0, sizeof(g_startDecks));
        if (++g_startDeckGeneration == 0) ++g_startDeckGeneration;
        const uint32_t generation = g_startDeckGeneration;
        for (int slot = 0; slot < 12; ++slot) {
            const uint8_t* entry = packet + kStartPacketSlots + slot * kStartSlotStride;
            uint32_t pilotId = *reinterpret_cast<const uint32_t*>(entry);
            if (!(pilotId & kAiPilotBit))
                continue;
            StartDeck& deck = g_startDecks[slot];
            deck.pilotId = pilotId;
            deck.points = entry[kStartSlotPoints];
            deck.units = entry[kStartSlotUnits];
            deck.generation = generation;
            for (int i = 0; i < 4; ++i) {
                const uint8_t* block = entry + kStartSlotUnitBlocks + i * kStartUnitBlock;
                deck.npc[i] = *reinterpret_cast<const uint32_t*>(block);
                deck.maxHp[i] = *reinterpret_cast<const uint32_t*>(block + kStartUnitMaxHp);
                deck.lifeCost[i] = *reinterpret_cast<const uint16_t*>(block + kStartUnitLifeCost);
            }
            g_api->Log(g_mod, SDGO_LOG_INFO,
                       "battle start: AI pilot %08lX (slot %d) deck %d points, %d unit(s): %lu %lu %lu %lu, life cost %d",
                       static_cast<unsigned long>(pilotId), slot, deck.points, deck.units,
                       static_cast<unsigned long>(deck.npc[0]), static_cast<unsigned long>(deck.npc[1]),
                       static_cast<unsigned long>(deck.npc[2]), static_cast<unsigned long>(deck.npc[3]), deck.lifeCost[0]);
        }
    }

    const StartDeck* FindStartDeck(uint32_t pilotId)
    {
        for (const StartDeck& deck : g_startDecks)
            if (deck.pilotId == pilotId && pilotId != 0)
                return &deck;
        return nullptr;
    }

    bool ImplicitSingleUnitStartDeck(const StartDeck& deck, int form)
    {
        if (deck.units != 0 || form != 0 || !g_botMode)
            return false;
        __try {
            const uint8_t* room = *reinterpret_cast<uint8_t* const*>(g_botMode + kModeRoom);
            if (!room)
                return false;
            const uint16_t roomMode =
                *reinterpret_cast<const uint16_t*>(room + kModeRoomMode);
            // Native ordinary-mode starts advertise zero deck units even though the
            // first copied unit block is the active machine.  Tag/grid modes use the
            // count as deck authority, so an absent count must stay fail-closed there.
            return roomMode != 6 && roomMode != 7;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ApplyAdvertisedAceHealth(Bot& bot, int form, bool initializeFreshCurrent)
    {
        if (!bot.unit || bot.startDeckGeneration == 0 || form < 0 || form >= 4 ||
            (bot.pilotId & (battle_ai::ace::kAiPilotBit | battle_ai::ace::kAcePilotBit)) !=
                (battle_ai::ace::kAiPilotBit | battle_ai::ace::kAcePilotBit))
            return false;
        const StartDeck* deck = FindStartDeck(bot.pilotId);
        if (!deck || deck->generation != bot.startDeckGeneration)
            return false;
        const bool implicitSingle = ImplicitSingleUnitStartDeck(*deck, form);
        if (!implicitSingle &&
            (deck->units < 1 || deck->units > 4 || form >= deck->units))
            return false;
        const uint32_t advertised = deck->maxHp[form];
        if (advertised == 0 || advertised > 0x7fffffffu || deck->npc[form] == 0)
            return false;

        __try {
            if (*reinterpret_cast<const uint32_t*>(bot.unit + kUnitObjectId) != bot.objectId ||
                *reinterpret_cast<const uint32_t*>(bot.unit + kUnitServerId) != bot.pilotId ||
                *reinterpret_cast<const uint32_t*>(bot.unit + kUnitNpcDataId) != deck->npc[form])
                return false;
            const uint32_t nativeMaximum =
                *reinterpret_cast<const uint32_t*>(bot.unit + kUnitMaxHp);
            const uint32_t nativeCurrent =
                *reinterpret_cast<const uint32_t*>(bot.unit + kUnitCurrentHp);
            const bool freshFullSeed = initializeFreshCurrent && nativeMaximum != 0 &&
                nativeMaximum <= 0x7fffffffu && nativeCurrent == nativeMaximum;
            *reinterpret_cast<uint32_t*>(bot.unit + kUnitMaxHp) = advertised;
            // The start packet has no current-HP field.  Preserve all server-authored
            // current HP except the exact native full-health seed of a newly created
            // actor, where the advertised maximum is the only truthful full value.
            if (freshFullSeed)
                *reinterpret_cast<uint32_t*>(bot.unit + kUnitCurrentHp) = advertised;
            if (nativeMaximum != advertised)
                g_api->Log(g_mod, SDGO_LOG_INFO,
                           "AI player %08lX deck unit %d: native max HP %lu -> advertised %lu%s",
                           static_cast<unsigned long>(bot.pilotId), form,
                           static_cast<unsigned long>(nativeMaximum),
                           static_cast<unsigned long>(advertised),
                           freshFullSeed ? ", fresh full HP initialized" : "");
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod, SDGO_LOG_WARN,
                       "AI player %08lX deck unit %d: applying advertised HP failed (%08lX)",
                       static_cast<unsigned long>(bot.pilotId), form, GetExceptionCode());
            return false;
        }
    }

    bool GuardedReadStartDecks(const uint8_t* packet)
    {
        __try {
            ReadStartDecks(packet);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(g_startDecks, 0, sizeof(g_startDecks));
            return false;
        }
    }

    uint16_t ReadReceivedBattleMode()
    {
        using namespace battle_ai::native::detail;
        __try {
            uint8_t* app=g_gameApp?g_gameApp():nullptr;uint8_t* service=nullptr;uint8_t* mode=nullptr;uint8_t* room=nullptr;
            uint16_t value=0;
            // The start notice can arrive while the native service is still
            // changing from its wait-room representation to the battle mode.
            if(app&&ReadPointer(app,kAppGameMain,service)&&ReadPointer(service,kServiceMode,mode)&&
               ReadPointer(mode,kModeRoom,room)&&Read(room,kModeRoomMode,value))return value;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return 0; // Mode Init publishes the actual mode once the world exists.
    }

    int __fastcall ServicePacket(void* self, void* /*edx*/, int a, int b, int c, int opcode, void* packet)
    {
        const uint16_t operation = static_cast<uint16_t>(opcode);
        // Battle combat packets have their own master dispatcher. The service
        // dispatcher observes lobby/start traffic, not authoritative kills.
        if(operation!=0x0570)NotifyPacket(operation,static_cast<const uint8_t*>(packet));
        bool createReply = false;
        uint32_t createdRoom = 0;
        uint16_t createError = 0;
        // 0918's native consumer reads a u32 room id at +6; the distinct
        // A0A rejection reads only a u16 error there. Copy before the original
        // dispatch can release the packet, then publish after it completes.
        if (packet && (operation == 0x0918 || operation == 0x0A0A)) {
            __try {
                const uint8_t* data = static_cast<const uint8_t*>(packet);
                if (operation == 0x0918) createdRoom = *reinterpret_cast<const uint32_t*>(data + 6);
                else createError = *reinterpret_cast<const uint16_t*>(data + 6);
                createReply = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { createReply = false; }
        }
        if ((opcode & 0xFFFF) == kPacketBattleStart && packet) {
            InterlockedIncrement(&g_receivedBattleStartCount);
            InterlockedExchange(&g_receivedBattleStartMode,ReadReceivedBattleMode());
            if (!GuardedReadStartDecks(static_cast<const uint8_t*>(packet)))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception reading the AI decks of the battle start packet");
        }
        const int handled = g_originalServicePacket(self, a, b, c, opcode, packet);
        if (createReply) {
            InterlockedIncrement(&g_createReplyVersion);
            InterlockedExchange(&g_createReplyRoom, static_cast<LONG>(createdRoom));
            InterlockedExchange(&g_createReplyDetails, static_cast<LONG>(operation | (static_cast<uint32_t>(createError) << 16)));
            InterlockedIncrement(&g_createReplySequence);
            InterlockedIncrement(&g_createReplyVersion);
        }
        return handled;
    }

    constexpr size_t kNpcUnitDataId = 0x69;     // NPC data: its unit data id
    constexpr uint32_t kGridNpcData = 0x29;     // grid data type of NPC data
    constexpr size_t kUnitDeckBegin = 0x2ED0;   // unit deck entries, 0x60C bytes each
    constexpr size_t kUnitDeckEnd = 0x2ED4;
    constexpr size_t kUnitDeckEntry = 0x60C;
    constexpr size_t kUnitDeckIndex = 0x2EE4;   // deck entry in play
    const uint8_t* FindNpcData(uint32_t npcId)
    {
        const uint8_t* npc = nullptr;
        if (!g_gridFind(g_gridData(), &npc, npcId, kGridNpcData, 0))
            return nullptr;
        return npc;
    }

    int DeckSize(uint8_t* unit)
    {
        auto begin = reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(unit + kUnitDeckBegin));
        auto end = reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(unit + kUnitDeckEnd));
        if (!begin || end <= begin || (end - begin) % kUnitDeckEntry != 0 || (end - begin) / kUnitDeckEntry > 4)
            return 0;
        return static_cast<int>((end - begin) / kUnitDeckEntry);
    }

    int StartDeckPoints(uint32_t pilotId)
    {
        const StartDeck* deck = FindStartDeck(pilotId);
        return deck ? deck->points : 0;
    }

    // Adds the other units of the AI player's deck (battle start packet) to its unit (modes 6, 7).
    void BuildAiDeck(Bot& bot, uint32_t firstNpc)
    {
        bot.deckNpc[0] = firstNpc;
        bot.deckNpcData[0] = *reinterpret_cast<const uint8_t**>(bot.unit + kUnitNpcData);
        const StartDeck* deck = FindStartDeck(bot.pilotId);
        if (!deck || deck->units < 1 || deck->units > 4 || deck->npc[0] != firstNpc) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "AI player %08lX: no matching deck in the battle start packet; one unit",
                       static_cast<unsigned long>(bot.pilotId));
            return;
        }
        char text[128];
        int length = sprintf_s(text, "%lu", static_cast<unsigned long>(firstNpc));
        for (int i = 1; i < deck->units; ++i) {
            uint32_t npcId = deck->npc[i];
            if (npcId == 0)
                break;
            const uint8_t* npc = FindNpcData(npcId);
            if (!npc) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "AI player %08lX: deck unit %d, NPC %lu has no NPC data here",
                           static_cast<unsigned long>(bot.pilotId), i, static_cast<unsigned long>(npcId));
                // Native deck entries are consecutive. Skipping this entry would
                // assign a later NPC to the wrong server respawn index.
                break;
            }
            int before = DeckSize(bot.unit);
            g_addDeckUnit(bot.unit, *reinterpret_cast<const uint32_t*>(npc + kNpcUnitDataId));
            if (DeckSize(bot.unit) != before + 1) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "AI player %08lX: adding deck unit %d failed; remaining entries not added",
                           static_cast<unsigned long>(bot.pilotId), i);
                break;
            }
            bot.deckNpc[bot.units] = npcId;
            bot.deckNpcData[bot.units] = npc;
            ++bot.units;
            length += sprintf_s(text + length, sizeof(text) - length, " + %lu", static_cast<unsigned long>(npcId));
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "AI player %08lX: deck %s, %d unit(s) (server: %d, %d points), unit deck size %d",
                   static_cast<unsigned long>(bot.pilotId), text, bot.units, deck->units, deck->points, DeckSize(bot.unit));
    }

    int RecordCount(uint8_t* room, size_t begin, size_t end)
    {
        auto first = reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(room + begin));
        auto last = reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(room + end));
        if (!first || last <= first || (last - first) % kRecordSize != 0 || (last - first) / kRecordSize > 12)
            return 0;
        return static_cast<int>((last - first) / kRecordSize);
    }

    bool LocalIsRoomMaster(uint8_t* room)
    {
        uint32_t local = *reinterpret_cast<uint32_t*>(room + kRoomLocalPlayer);
        uint8_t* records = *reinterpret_cast<uint8_t**>(room + kRoomHumansBegin);
        int humans = RecordCount(room, kRoomHumansBegin, kRoomHumansEnd);
        for (int k = 0; k < humans; ++k) {
            uint8_t* record = records + k * kRecordSize;
            if (record[kRecordMaster] != 0 && *reinterpret_cast<uint32_t*>(record + kRecordServerId) == local)
                return true;
        }
        return false;
    }

    constexpr int kPlayerSlots = 12; // UM player slots (UM +0x94, 0x70 each)

    struct SpawnContext
    {
        uint8_t* mode;
        const char* className;
        void* um;
        bool master;
        int humans = 0;
        int nextIndex[3] = {}; // next start point per colour (1, 2), after the humans'
        int spawned = 0;
    };

    uint8_t* AiStartPoint(void* um,int colour,int index)
    {
        if((colour!=1 && colour!=2) || index<0 || index>=kPlayerSlots)return nullptr;
        if(auto* exact=g_game.findStartPoint(um,colour,index))return exact;
        // Asymmetric teams may outnumber a map's start points. Reuse only
        // existing native points, cycling over them instead of stacking every
        // overflow participant on point zero. No invented map coordinates.
        uint8_t* points[kPlayerSlots]{};
        int count=0;
        for(int candidate=0;candidate<kPlayerSlots;++candidate)
            if(auto* point=g_game.findStartPoint(um,colour,candidate))points[count++]=point;
        return count ? points[index%count]:nullptr;
    }

    void SpawnAiPlayer(SpawnContext& c, uint8_t* record, int k)
    {
        uint32_t serverId = *reinterpret_cast<uint32_t*>(record + kRecordServerId);
        int team = record[kRecordTeam];
        int colour = record[kRecordColour];
        uint32_t npcData = *reinterpret_cast<uint32_t*>(record + kRecordNpcData);
        int index = (colour == 1 || colour == 2) ? c.nextIndex[colour]++ : k;
        uint8_t* start = AiStartPoint(c.um,colour,index);
        if (!start) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "AI player %08lX: the map has no colour %d start point",
                       static_cast<unsigned long>(serverId), colour);
            return;
        }
        auto pos = reinterpret_cast<float*>(start + kStartPointPos);
        g_creatingAiUnit = true;
        uint8_t* unit = g_game.baseCreateNpc(c.mode, npcData, pos[0], pos[1], pos[2], team, 0.0f, 0.0f, 1.0f);
        g_creatingAiUnit = false;
        if (!unit) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "AI player %08lX: creating NPC %lu failed",
                       static_cast<unsigned long>(serverId), static_cast<unsigned long>(npcData));
            return;
        }
        // what CScriptControlMode::CreateNpc and the mission NPC registration (0x00ACED20)
        // add for a network game: the room master runs the AI, the other clients get a
        // remote copy
        *reinterpret_cast<uint32_t*>(unit + kUnitNpcDataId) = npcData;
        g_game.addObject(c.um, unit, 0, 0);
        unit[kUnitNpcFlag] = 0;
        if (c.master) {
            unit[kUnitLocalControl] = 1;
            unit[kUnitMasterNpc] = 1;
            unit[kUnitRemote] = 0;
        } else {
            unit[kUnitRemote] = 1;
        }
        *reinterpret_cast<uint32_t*>(unit + kUnitNpcState) = 2;
        int slot = c.humans + c.spawned;
        uint32_t objectId = *reinterpret_cast<uint32_t*>(unit + kUnitObjectId);
        g_game.setPlayerInfo(c.um, slot, 0, objectId, *reinterpret_cast<uint32_t*>(record + kRecordConnection), record);
        const uint8_t* npc = *reinterpret_cast<uint8_t**>(unit + kUnitNpcData);
        const auto aceOutcome = battle_ai::ace::DecodePilotId(
            serverId, static_cast<battle_ai::Difficulty>(g_roomDifficulty));
        std::wstring acePilotName;
        if (aceOutcome.ace && npc) {
            const uint32_t unitId = *reinterpret_cast<const uint32_t*>(npc + kNpcUnitDataId);
            acePilotName = AcePilotName(unitId);
            if (acePilotName.empty())
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "AI player %08lX: server selected Ace but unit %lu has no valid pilot identity",
                           static_cast<unsigned long>(serverId), static_cast<unsigned long>(unitId));
        }
        if (!acePilotName.empty()) {
            g_game.assign(g_game.playerSlot(c.um, slot) + kSlotName,
                          acePilotName.c_str(), acePilotName.size());
        } else if (npc) {
            auto name = reinterpret_cast<const wchar_t*>(npc + 5);
            g_game.assign(g_game.playerSlot(c.um, slot) + kSlotName, name, wcslen(name));
        }
        InitAiSlot(ModeClassIndex(c.mode), g_game.playerSlot(c.um, slot), unit);
        g_game.addAiUnit(c.um, unit);
        *reinterpret_cast<uint32_t*>(unit + kUnitServerId) = serverId;
        Bot bot{serverId, objectId, slot, unit};
        if (const StartDeck* startDeck = FindStartDeck(serverId))
            bot.startDeckGeneration = startDeck->generation;
        bot.ace = aceOutcome.ace && !acePilotName.empty();
        bot.team = static_cast<uint8_t>(team);
        bot.colour = static_cast<uint8_t>(colour);
        bot.effectiveDifficulty = aceOutcome.effective;
        bot.acePilotName = acePilotName;
        uint8_t* room = *reinterpret_cast<uint8_t**>(c.mode + kModeRoom);
        uint16_t roomMode = *reinterpret_cast<uint16_t*>(room + kModeRoomMode);
        if (roomMode == 6 || roomMode == 7) {
            g_creatingAiUnit = true;
            BuildAiDeck(bot, npcData);
            g_creatingAiUnit = false;
        }
        bot.generation = NextUnitGeneration();
        ApplyAdvertisedAceHealth(bot, 0, true);
        InitializeBattleAiUnit(bot, true);
        // Native CreateNpc/InitNPCAI may already enable the controller. Apply
        // the per-unit steps of AllNpcStopControl (0x00AE5A20), scoped to this
        // newly created match_ai participant, before the first world update.
        g_game.disableAi(unit);
        g_game.releaseAttack(unit);
        g_game.moveStop(unit);
        g_game.resetInputs(unit);
        *reinterpret_cast<uint32_t*>(unit + kUnitPendingControl) = 0;
        g_botTable.push_back(bot);
        g_logPackets = true;
        ++c.spawned;
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "%s: AI player %08lX joined as object %lu (NPC %lu, team %d, colour %d, start point %d, slot %d, %s)",
                   c.className, static_cast<unsigned long>(serverId), static_cast<unsigned long>(objectId),
                   static_cast<unsigned long>(npcData), team, colour, index, slot,
                   c.master ? "AI runs here" : "copy of the room master's");
    }

    // One guard per AI player: a fault leaves the others and the player count right.
    bool GuardedSpawnAiPlayer(SpawnContext& c, uint8_t* record, int k, DWORD* code)
    {
        __try {
            SpawnAiPlayer(c, record, k);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_creatingAiUnit = false;
            *code = GetExceptionCode();
            return false;
        }
    }

    // The real TAB panel caches UM::playerCount in its Init (0x00532C00),
    // before the PvP mode Init returns and we add AI. Its own row count and
    // slide-in offsets must include the newly registered slots as well.
    void SyncScoreboardRoster(uint8_t* mode, int count)
    {
        if (count < 1 || count > 12 || g_botTable.empty()) return;
        uint8_t* hud = *reinterpret_cast<uint8_t**>(mode + 0x0C);
        uint8_t* panel = hud ? *reinterpret_cast<uint8_t**>(hud + 0x1C08) : nullptr;
        if (!panel || (*reinterpret_cast<void***>(panel))[10] != reinterpret_cast<void*>(g_rosterInit)) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "native TAB roster panel unavailable after AI spawn");
            return;
        }
        auto& rows = *reinterpret_cast<int32_t*>(panel + 0x150);
        if (rows < 0 || rows > count) return;
        g_scoreboardStatus.original_row_count = static_cast<uint32_t>(rows);
        for (int i = rows; i < count; ++i) {
            auto offset = reinterpret_cast<float*>(panel + 0x170 + i * 12);
            offset[0] = *reinterpret_cast<float*>(panel + 0x48);
            offset[1] = offset[2] = 0;
        }
        rows = count;
        g_rosterPanel = panel;
        ++g_scoreboardStatus.roster_sync_count;
    }

    void SpawnAiPlayers(uint8_t* mode, const char* className)
    {
        g_botTable.clear();
        g_aiEnabled = false;
        std::fill(std::begin(g_aceAnnouncementDone), std::end(g_aceAnnouncementDone), false);
        g_startStatus = {};
        g_scoreboardStatus = {};
        g_rosterPanel = nullptr;
        g_logPackets = false;
        g_viewStateLogs = 0;
        g_botMode = mode;
        g_soloAiMode = nullptr;
        g_natCheckSkipped = false;
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        if (!room)
            return;
        g_roomDifficulty = BattleDifficulty(room);

        InterlockedExchange(
            &g_receivedBattleStartMode,
            static_cast<LONG>(*reinterpret_cast<uint16_t*>(room + kModeRoomMode)));
        int count = RecordCount(room, kRoomAiBegin, kRoomAiEnd);
        if (count == 0)
            return;
        BeginBattleAiProfiles();
        if (RecordCount(room, kRoomHumansBegin, kRoomHumansEnd) == 1)
            g_soloAiMode = mode;
        if (!g_game.isNetwork()) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "%d AI player(s) in the room, but the battle took the offline path", count);
            return;
        }
        void* um = *g_game.unitManager;
        bool master = LocalIsRoomMaster(room) && !g_game.isObserver(um);
        auto vtable = *reinterpret_cast<void* const* const*>(mode);
        if (vtable[kModeCreateNpcSlot] != reinterpret_cast<void*>(g_game.baseCreateNpc)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "%s: CreateNpc is not the CBaseGameMode one; AI players not spawned",
                       className);
            return;
        }

        SpawnContext c{mode, className, um, master};
        c.humans = RecordCount(room, kRoomHumansBegin, kRoomHumansEnd);
        uint8_t* humanRecords = *reinterpret_cast<uint8_t**>(room + kRoomHumansBegin);
        for (int k = 0; k < c.humans; ++k) {
            int colour = humanRecords[k * kRecordSize + kRecordColour];
            if (colour == 1 || colour == 2)
                ++c.nextIndex[colour];
        }

        uint8_t* records = *reinterpret_cast<uint8_t**>(room + kRoomAiBegin);
        for (int k = 0; k < count; ++k) {
            if (c.humans + c.spawned >= kPlayerSlots) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "%d AI player(s) left out: the unit manager has %d player slots",
                           count - k, kPlayerSlots);
                break;
            }
            DWORD code = 0;
            if (!GuardedSpawnAiPlayer(c, records + k * kRecordSize, k, &code))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while spawning AI player %d", code, k);
        }
        if (c.spawned > 0) {
            // the AI battle mode's player count: humans and AI players (0x0040417C)
            g_setPlayerCount(um, c.humans + c.spawned);
            g_api->Log(g_mod, SDGO_LOG_INFO, "player count %d (%d humans, %d AI players)", c.humans + c.spawned, c.humans,
                       c.spawned);
        }
        g_game.calculateTeam(um);
        SyncScoreboardRoster(mode, c.humans + c.spawned);
    }

    // ---- grid match: the gauge counts the AI players ---------------------------------------
    //
    // CGridMatchGameMode's Init (0x00ADB790) sets the team gauge (mode +0x30) from the number
    // of human records, (n - 2) into a table: 2, 4 and 8 players 80, 6 players 60, 10 players
    // 120, 12 players 160, odd counts nothing. With one human and AI players nothing was set,
    // so the HUD's grid bar stayed empty while the server scored. With AI players the gauge
    // is set the way the server sets each team's (pvp_ai GridGauge, from 0x004B6EB0): by the
    // players per team, humans and AI, the larger team where they differ. The HUD has its
    // own copy: the Init's last step, the mode's CreateInterface (vt slot 18, 0x00AD7650),
    // creates it (mode +0x0C) and passes it the gauge through its vt slot 23 (0x004F7EC0),
    // before the AI players spawn; it gets the new gauge the same way.
    constexpr size_t kModeGridGauge = 0x30;
    constexpr size_t kModeInterface = 0x0C;
    constexpr int kInterfaceSetGaugeSlot = 23;
    using SetGaugeFn = void(__thiscall*)(uint8_t* hud, int gauge);

    void SetGridGauge(uint8_t* mode)
    {
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        int team[3] = {};
        for (size_t begin : {kRoomHumansBegin, kRoomAiBegin}) {
            uint8_t* records = *reinterpret_cast<uint8_t**>(room + begin);
            int count = RecordCount(room, begin, begin + 4);
            for (int k = 0; k < count; ++k) {
                int t = records[k * kRecordSize + kRecordTeam];
                ++team[t == 1 || t == 2 ? t : 0];
            }
        }
        int gauge = sdgo::ai::GridGaugeForTeams(team[1], team[2]);
        auto& field = *reinterpret_cast<int32_t*>(mode + kModeGridGauge);
        g_api->Log(g_mod, SDGO_LOG_INFO, "grid match: gauge %ld -> %d (teams %d/%d with the AI players)",
                   static_cast<long>(field), gauge, team[1], team[2]);
        if (gauge <= 0)
            return;
        field = gauge;
        uint8_t* hud = *reinterpret_cast<uint8_t**>(mode + kModeInterface);
        if (hud) {
            auto setGauge = reinterpret_cast<SetGaugeFn>((*reinterpret_cast<void***>(hud))[kInterfaceSetGaugeSlot]);
            setGauge(hud, gauge);
        }
    }

    bool GuardedSpawn(uint8_t* mode, const char* className, DWORD* code)
    {
        __try {
            SpawnAiPlayers(mode, className);
            if (mode == g_botMode && !g_botTable.empty() && ModeClassIndex(mode) == 2)
                SetGridGauge(mode);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_creatingAiUnit = false;
            *code = GetExceptionCode();
            return false;
        }
    }

    // The units' AI scripts call methods on their npc argument (CNPC); the game registers
    // those Lua bindings at the start of every script mode's Init (0x0050FAB0), never in a
    // PvP mode. A PvP battle with AI players does the same before its Init.
    bool RoomHasAiPlayers(uint8_t* mode)
    {
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        return room && RecordCount(room, kRoomAiBegin, kRoomAiEnd) > 0;
    }

    bool GuardedHasAi(uint8_t* mode)
    {
        __try {
            return RoomHasAiPlayers(mode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <int I>
    void __fastcall ModeInit(uint8_t* mode, void* /*edx*/)
    {
        if (GuardedHasAi(mode)) {
            InterlockedExchange(&g_luaErrors, 0);
            g_registerLuaBindings();
            g_api->Log(g_mod, SDGO_LOG_INFO, "%s: Lua bindings registered for the AI players", g_classes[I].name);
        }
        g_classes[I].original(mode);
        DWORD code = 0;
        if (!GuardedSpawn(mode, g_classes[I].name, &code))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while spawning AI players", code);
    }

    using DetourFn = void(__fastcall*)(uint8_t*, void*);
    const DetourFn g_detours[] = {&ModeInit<0>, &ModeInit<1>, &ModeInit<2>, &ModeInit<3>, &ModeInit<4>};

    // ---- game ready: switch the AI on --------------------------------------------------
    //
    // GameReady (0x004B2390) owns the startup handshake while players are frozen.
    // At 0x004B25DA it commits phase 3, then releases player control (0x00671170)
    // and enables script-mode AI (0x004B272B). PvP bots must wait for that same
    // native phase, regardless of difficulty or whether battle_ai is installed.
    // Map volumes are polled by each PvP mode's continuous Update instead.

    bool GuardedUpdateForcedPads();
    void AnnounceAceTeams(uint8_t* mode);
    void DispatchMissionReady(uint8_t* mode, bool reset);
    volatile LONG g_forcedPadTicks=0;
    template<size_t I>
    int __fastcall ModeUpdate(uint8_t* mode,void*)
    {
        const int result=g_classes[I].updateOriginal(mode);
        NotifyTick(mode,static_cast<uint32_t>(InterlockedCompareExchange(&g_receivedBattleStartCount,0,0)));
        if(mode==g_botMode&&g_aiEnabled) {
            InterlockedIncrement(&g_forcedPadTicks);
            GuardedUpdateForcedPads();
            AnnounceAceTeams(mode);
        }
        return result;
    }
    using ModeUpdateDetour=int(__fastcall*)(uint8_t*,void*);
    const ModeUpdateDetour g_updateDetours[]={&ModeUpdate<0>,&ModeUpdate<1>,&ModeUpdate<2>,&ModeUpdate<3>,&ModeUpdate<4>};

    void AnnounceAceTeams(uint8_t* mode)
    {
        if (!mode || g_botTable.empty()) return;
        bool present[3] = {};
        for (const Bot& bot : g_botTable)
            if (bot.ace && (bot.colour == 1 || bot.colour == 2)) present[bot.colour] = true;
        ResolveAceProvider();
        if (!g_aceAnnouncementProviderLogged && (present[1] || present[2])) {
            g_aceAnnouncementProviderLogged = true;
            g_api->Log(g_mod, SDGO_LOG_INFO,
                       "Ace chat candidates: red=%u blue=%u provider=%u",
                       present[1] ? 1u : 0u, present[2] ? 1u : 0u,
                       g_aceAnnounceTeam ? 1u : 0u);
        }
        if (!g_aceAnnounceTeam) return;
        for (uint32_t colour = 1; colour <= 2; ++colour) {
            if (!present[colour] || g_aceAnnouncementDone[colour]) continue;
            int result = 0;
            __try { result = g_aceAnnounceTeam(mode, colour); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_aceAnnounceTeam = nullptr; }
            if (result == 1) g_aceAnnouncementDone[colour] = true;
        }
    }

    void EnableBots(uint8_t* service)
    {
        uint8_t* mode = *reinterpret_cast<uint8_t**>(service + kServiceMode);
        if (!mode || mode != g_botMode || g_aiEnabled || g_botTable.empty())
            return;
        if (*reinterpret_cast<const uint32_t*>(service + kServiceReadyState) != 3)
            return;
        void* um = g_game.unitManager ? *g_game.unitManager : nullptr;
        for (const Bot& bot : g_botTable) {
            ApplyAcePilotName(um, bot);
            g_game.enableAi(bot.unit);
        }
        AnnounceAceTeams(mode);
        g_aiEnabled = true;
        ++g_startStatus.release_count;
        g_startStatus.release_ready_state = *reinterpret_cast<uint32_t*>(service + kServiceReadyState);
        g_api->Log(g_mod, SDGO_LOG_INFO, "game ready: AI switched on for %u AI player(s)",
                   static_cast<unsigned>(g_botTable.size()));
    }

    void ObserveStart(uint8_t* service, bool releasedNow)
    {
        if (*reinterpret_cast<uint8_t**>(service + kServiceMode) != g_botMode || g_botTable.empty())
            return;
        g_startStatus.native_ready_state = *reinterpret_cast<uint32_t*>(service + kServiceReadyState);
        g_startStatus.bot_count = static_cast<uint32_t>(g_botTable.size());
        g_startStatus.ai_enabled_count = 0;
        for (const Bot& bot : g_botTable)
            if (bot.unit[0x326C]) ++g_startStatus.ai_enabled_count;
        const uint8_t* manager = static_cast<const uint8_t*>(*g_game.unitManager);
        const uint8_t* player = manager ? *reinterpret_cast<const uint8_t* const*>(manager + kManagerPlayer) : nullptr;
        if (!g_startStatus.release_count || releasedNow)
            g_startStatus.player_control_enabled = player && player[kPlayerControlEnabled] ? 1u : 0u;
        if (g_startStatus.native_ready_state != 3) {
            ++g_startStatus.frozen_update_count;
            if (g_startStatus.ai_enabled_count) ++g_startStatus.early_enabled_count;
        }
        if (releasedNow && !g_startStatus.player_control_enabled && !g_game.isObserver(*g_game.unitManager))
            ++g_startStatus.early_enabled_count;
    }

    bool GuardedEnable(uint8_t* service)
    {
        __try {
            const bool enabledBefore = g_aiEnabled;
            EnableBots(service);
            ObserveStart(service, !enabledBefore && g_aiEnabled);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void __fastcall GameReady(uint8_t* service, void* /*edx*/)
    {
        g_originalGameReady(service);
        if (service) {
            DispatchMissionReady(
                *reinterpret_cast<uint8_t**>(service + kServiceMode), true);
            if (!GuardedEnable(service))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception while switching on the AI players");
        }
    }

    // ---- battle start: no UDP link to AI pilots ------------------------------------------
    //
    // On a normal wait room's battle start (0x00A0FE80) the client asks for a UDP link to the
    // pilot of every other slot and counts the requests against the links that come back.
    // An AI pilot has no client to answer, so for that call its slot reads as empty.

    void __fastcall UdpSetup(uint8_t* waitRoom, void* /*edx*/)
    {
        uint32_t hidden[12] = {};
        int count = 0;
        if (waitRoom) {
            for (int slot = 0; slot < 12; ++slot) {
                auto id = reinterpret_cast<uint32_t*>(waitRoom + kWaitRoomSlots + slot * kWaitRoomSlotStride);
                if (*id & kAiPilotBit) {
                    hidden[slot] = *id;
                    *id = 0;
                    ++count;
                }
            }
        }
        __try {
            g_originalUdpSetup(waitRoom);
        } __finally {
            for (int slot = 0; slot < 12 && count; ++slot)
                if (hidden[slot])
                    *reinterpret_cast<uint32_t*>(waitRoom + kWaitRoomSlots + slot * kWaitRoomSlotStride) = hidden[slot];
        }
        if (count)
            g_api->Log(g_mod, SDGO_LOG_INFO, "battle start: no UDP link asked for %d AI pilot(s)", count);
    }

    // ---- NAT check: no peer for the only human ------------------------------------------
    //
    // CGameMain's update (vt slot 3, 0x004BC620) runs a NAT check (0x004B6FD0) in a network
    // battle of every mode but the missions and AI modes (5, 8, 10, 11, 12): once the unit
    // manager's counter +0x82CC reaches 1000 and no message from another player has come in
    // (CGameMain +0xF8 and +0xFC, raised by 0x004C2090 and 0x004CBA90), it shows message
    // 120003 (blocked connection, firewall) and takes the mouse away. The only human of a
    // battle with AI players has no other player, so for that battle the check is skipped,
    // as the game skips it for its AI modes.

    void __fastcall NatCheck(uint8_t* gameMain, void* /*edx*/)
    {
        if (gameMain && g_soloAiMode && *reinterpret_cast<uint8_t**>(gameMain + kServiceMode) == g_soloAiMode) {
            if (!g_natCheckSkipped) {
                g_natCheckSkipped = true;
                g_api->Log(g_mod, SDGO_LOG_INFO, "NAT check skipped: no other player in this battle with AI players");
            }
            return;
        }
        g_originalNatCheck(gameMain);
    }

    // ---- the AI's Lua functions that need a script mode ----------------------------------
    //
    // The AI scripts aim through SearchLockPlayer & co. (0x00500D50, 0x00500E00, 0x00501120,
    // 0x00501270), which call vftable slot 18 of the current game mode, and through
    // GameInterfaceRunLuafunction (0x00502550, slot 22). Only the script modes have those
    // slots; on CNormalGameMode slot 18 reads 0 and the call jumps to address 0. In a PvP mode
    // these hooks run what the AI battle mode runs there (CAINormalGameMode slots 18 and 22,
    // 0x00AE35B0 and 0x00ACBC60). Of the mode, those two read only its objective list
    // (+0x108/+0x10C, the object ids 0x00AE0160 collects) and its counter list (+0x60C/+0x610);
    // a PvP battle has neither, so they get a stand-in with both lists empty.
    // OurTeamTargetSearch (0x0050AA90) reads the NPC's owning script mode, which the AI
    // players have not (+0x3268 = 0); it returns 0 there, its result for a mode +0xE4 <= 1
    // (the script mode default, 0x00AE6DF2).

    alignas(16) uint8_t g_scriptModeStandIn[0x620] = {};
    int g_lockFaults = 0;
    thread_local void* g_targetSearchUm = nullptr;
    constexpr uint32_t kSearchParticipantLimit = 12;
    const Bot* BotByObject(void* um, uint32_t objectId);
    void* g_targetTeamOriginal = nullptr;
    void* g_targetParticipants = nullptr;
    void* g_scoreboardNpcGateOriginal = nullptr;

    // Integer/TLS-only helper: the site detour preserves integer registers and
    // flags, and this function does not touch x87 or SIMD state.
    int __cdecl ParticipantSearchActive() { return g_targetSearchUm != nullptr; }
    __declspec(naked) void TargetTeamGate()
    {
        __asm {
            pushfd
            pushad
            call ParticipantSearchActive
            test eax, eax
            jz native_team_path
            popad
            popfd
            // The earlier NPC-only pass stored its weaker fallback here.
            // Do not retain it when every real participant fails eligibility.
            mov dword ptr [ebp-3Ch], 0
            jmp dword ptr [g_targetParticipants]
        native_team_path:
            popad
            popfd
            jmp dword ptr [g_targetTeamOriginal]
        }
    }

    bool IsPvpMode(uint8_t* mode)
    {
        return mode && ModeClassIndex(mode) >= 0;
    }

    // The current game mode of any class, or NULL (the way the Lua functions find it:
    // GameApp +0x4A4 -> service +0x0C).
    uint8_t* CurrentMode()
    {
        __try {
            uint8_t* app = g_gameApp();
            uint8_t* service = app ? *reinterpret_cast<uint8_t**>(app + kAppGameMain) : nullptr;
            return service ? *reinterpret_cast<uint8_t**>(service + kServiceMode) : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool MissionControlReleased(uint8_t* mode)
    {
        if (!mode || !g_gameApp || !g_game.unitManager) return false;
        __try {
            uint8_t* app = g_gameApp();
            uint8_t* service =
                app ? *reinterpret_cast<uint8_t**>(app + kAppGameMain) : nullptr;
            if (!service ||
                *reinterpret_cast<uint8_t**>(service + kServiceMode) != mode ||
                *reinterpret_cast<uint32_t*>(service + kServiceReadyState) != 3)
                return false;
            auto* manager = static_cast<uint8_t*>(*g_game.unitManager);
            if (!manager) return false;
            if (g_game.isObserver && g_game.isObserver(manager)) return true;
            auto* player = *reinterpret_cast<uint8_t**>(manager + kManagerPlayer);
            return player && player[kPlayerControlEnabled] != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void DispatchMissionReady(uint8_t* mode, bool reset)
    {
        if (reset || mode != g_missionReadyMode) {
            g_missionReadyMode = mode;
            g_missionReadyComplete = false;
            if (reset) g_missionRespawnPending.clear();
        }
        if (!mode || g_missionReadyComplete || !MissionControlReleased(mode))
            return;
        const auto* mission = MissionClientLifecycleV2();
        if (!mission) return;
        int result = 0;
        __try { result = mission->ready(mode); }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = 0; }
        if (result == 1) g_missionReadyComplete = true;
    }

    // The current game mode when it is one of the PvP modes, else NULL.
    uint8_t* CurrentPvpMode()
    {
        uint8_t* mode = CurrentMode();
        __try {
            return IsPvpMode(mode) ? mode : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool ModeIsA(uint8_t* mode, int kind)
    {
        auto isA = reinterpret_cast<ModeIsAFn>((*reinterpret_cast<void***>(mode))[kModeIsASlot]);
        return isA(mode, kind);
    }

    int LockFault(DWORD code, void* address)
    {
        if (g_lockFaults < 10) {
            ++g_lockFaults;
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX at %p in the AI's target search", code, address);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    int LockTarget(uint32_t objectId, char p2, char p3)
    {
        __try {
            void* previous = g_targetSearchUm;
            void* um = g_game.unitManager ? *g_game.unitManager : nullptr;
            g_targetSearchUm = um && BotByObject(um, objectId) ? um : nullptr;
            __try {
                return g_lockTarget(g_scriptModeStandIn, objectId, p2, p3);
            } __finally {
                g_targetSearchUm = previous;
            }
        } __except (LockFault(GetExceptionCode(), (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
            return 0;
        }
    }

    int __cdecl LockPlayerA(uint32_t objectId)
    {
        if (CurrentPvpMode())
            return LockTarget(objectId, 0, 0);
        return g_originalLockPlayerA(objectId);
    }

    int __cdecl LockPlayer(uint32_t objectId)
    {
        uint8_t* mode = CurrentPvpMode();
        if (mode && !ModeIsA(mode, 0))
            return LockTarget(objectId, 1, 0);
        return g_originalLockPlayer(objectId);
    }

    int __cdecl LockPlayerAibs(uint32_t objectId)
    {
        if (CurrentPvpMode())
            return LockTarget(objectId, 1, 0);
        return g_originalLockPlayerAibs(objectId);
    }

    int __cdecl LockTargetSelect(uint32_t objectId, int select)
    {
        uint8_t* mode = CurrentPvpMode();
        if (mode && !ModeIsA(mode, 0))
            return LockTarget(objectId, 1, static_cast<char>(select));
        return g_originalLockTargetSelect(objectId, select);
    }

    void __cdecl RunLuaFunction(int index)
    {
        if (CurrentPvpMode()) {
            g_runLuaFunction(g_scriptModeStandIn, index);
            return;
        }
        g_originalRunLuaFunction(index);
    }

    int __cdecl TeamSearch(uint8_t* npc)
    {
        if (npc && *reinterpret_cast<void**>(npc + kUnitOwner) == nullptr)
            return 0;
        return g_originalTeamSearch(npc);
    }

    // ---- server pilot ids and object ids of the AI players -------------------------------
    //
    // The battle packets name a player by server pilot id; UM::ObjectIdFromPilotId (0x0049BF00)
    // finds the object id among the player slots below the player count (UM +0x8C), then
    // among the live AI units (UM +0x70), so a dead AI player was not found and its respawn
    // (PT_NTF_PLAYBACK_UNIT) was dropped. Kills, deaths and score: the PT_NTF_DEAD_UNIT and
    // PT_NTF_MAP_DAMAGE handlers credit them to UM::PlayerSlot(object id - 1) only when
    // UM::IsPlayerObject(object id) (1..12). The AI battle mode credits an NPC through
    // PlayerSlot, which there maps object id - 1 >= 1000 to the AI players' slots; elsewhere it
    // uses the index as it is, far past the unit manager (the score sync 0x004C5140 writes
    // there). The respawn handler's UM::SetSlotColour(object id - 1, colour) (0x004C1DD0) maps
    // the same way. HUD loops over the counted slots look each slot's unit up with
    // UM::FindPlayer (0x004999A0), which knows only AddPlayer's units (humans); the death
    // match HUD (0x00530010) reads it without a NULL check and skips NPC units (IsA 10).
    //
    // All of them answer for an AI player of the table while its player slot still holds it
    // (object id at slot +4, record at slot +0x44 with the pilot id at +0), also after the
    // battle (result screen, teardown). A slot index >= 1000 that is no AI player gets a
    // scratch slot in the PvP modes, where the game has no such players, instead of memory
    // past the unit manager; its callers are logged.

    constexpr size_t kSlotObjectId = 0x04;
    constexpr size_t kSlotRecord = 0x44;
    constexpr uint16_t kTeleportRelocationOpcode = 0x07D4;
    constexpr uint8_t kTeleportRelocationPointCount = 12;
    const DWORD g_teleportDefaultMxcsr = 0x1F80;

    bool SlotHolds(void* um, const Bot& bot)
    {
        __try {
            uint8_t* slot = g_originalPlayerSlot(um, bot.slot);
            if (*reinterpret_cast<uint32_t*>(slot + kSlotObjectId) != bot.objectId)
                return false;
            uint8_t* record = *reinterpret_cast<uint8_t**>(slot + kSlotRecord);
            return record && *reinterpret_cast<uint32_t*>(record) == bot.pilotId;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    const Bot* BotByPilot(void* um, uint32_t pilotId)
    {
        if (!(pilotId & kAiPilotBit))
            return nullptr;
        for (const Bot& bot : g_botTable)
            if (bot.pilotId == pilotId && SlotHolds(um, bot))
                return &bot;
        return nullptr;
    }

    const Bot* BotByObject(void* um, uint32_t objectId)
    {
        if (objectId < 1001)
            return nullptr;
        for (const Bot& bot : g_botTable)
            if (bot.objectId == objectId && SlotHolds(um, bot))
                return &bot;
        return nullptr;
    }

    // ---- authored ForcedMove map volumes for locally owned AI participants ----------------
    //
    // The stock map-volume path admits only the native local-player kind.  A match_ai
    // participant is a registered CNPC, so the room master's copy must perform the same
    // read-only map query and submit the same ordinary request.  PvP bots and co-op mission
    // wingmen retain separate native participant lifecycles.  The server remains the
    // authority: no local movement state, position, HP, or notification is manufactured.

    constexpr size_t kUnitMovementController = 0x9C8;
    constexpr size_t kUnitAttackController = 0x9CC;
    constexpr size_t kUnitControlWord = 0x9D8;
    constexpr size_t kUnitPositionNode = 0x08;
    constexpr size_t kForcedPadNodePosition = 0x88;
    constexpr DWORD kForcedPadRequestTimeoutMs = battle_ai::native::kMapTransportRequestTimeoutMs;
    constexpr DWORD kForcedPadDisplacementTimeoutMs = 10000;
    constexpr float kForcedPadDisplacementSquared = 0.0001f;
    constexpr size_t kForcedMoveStateNode = 0x124;
    constexpr size_t kBlockedAttackNodes[] = {0x98, 0xAC, 0xC0, 0xD4, 0xE8, 0xFC};

    // This is the integration's explicit finite payload contract.  Authored data outside
    // this envelope is rejected instead of being guessed, clamped, or written back.
    const auto kForcedPadBounds=battle_ai::native::kAuthoredMapTransportBounds;

    enum class ForcedPadClearReason : uint32_t
    {
        None = 0,
        Notice = 1,
        NativeState = 2,
        Timeout = 3,
        OwnershipLoss = 4,
        GenerationChange = 5,
        SendFailure = 6
    };

    enum class ForcedPadScope : uint8_t
    {
        Pvp,
        Mission
    };

    struct ForcedPadAttempt
    {
        Bot* bot;
        void* unitManager;
        ForcedPadScope scope = ForcedPadScope::Pvp;
        uint8_t* padObject = nullptr;
        uint32_t padObjectId = 0;
    };

    bool IsMissionRoomMode(uint16_t mode)
    {
        return mode == 5 || mode == 8 || mode == 10;
    }

    bool MissionBotRegistered(void* um, const Bot& bot)
    {
        if (!um || !g_missionLifecycle || !bot.unit ||
            g_missionLifecycle->object_for_pilot(um, bot.pilotId) !=
                bot.objectId ||
            g_missionLifecycle->slot_for_object(um, bot.objectId) !=
                bot.slot) {
            return false;
        }
        return g_findObject(um, bot.objectId) == bot.unit;
    }

    const Bot* MissionBotByPilot(void* um, uint32_t pilotId)
    {
        if (!(pilotId & kAiPilotBit) || !g_missionPadMode ||
            CurrentMode() != g_missionPadMode) {
            return nullptr;
        }
        for (const Bot& bot : g_missionPadBots)
            if (bot.pilotId == pilotId && MissionBotRegistered(um, bot))
                return &bot;
        return nullptr;
    }

    bool ReadBotPosition(const Bot& bot, battle_ai::Vec3& position)
    {
        uint8_t* node = nullptr;
        return bot.unit &&
               battle_ai::native::detail::ReadPointer(
                   bot.unit, kUnitPositionNode, node) &&
               battle_ai::native::detail::Read(
                   node, kForcedPadNodePosition, position) &&
               battle_ai::Finite(position);
    }

    bool NativeForcedMoving(const uint8_t* unit)
    {
        uint8_t* movement = nullptr;
        return unit &&
               battle_ai::native::detail::ReadPointer(
                   unit, kUnitMovementController, movement) &&
               battle_ai::native::detail::InState(
                   movement, kForcedMoveStateNode);
    }

    bool ClearForcedPadOutstanding(
        Bot& bot, ForcedPadClearReason reason)
    {
        ForcedPadState& state = bot.forcedPad;
        if (InterlockedCompareExchange(&state.outstanding, 0, 1) != 1)
            return false;
        state.outstandingGeneration = 0;
        state.lastClearReason = static_cast<uint32_t>(reason);
        return true;
    }

    void ResetForcedPadGeneration(Bot& bot)
    {
        ForcedPadState& state = bot.forcedPad;
        InterlockedExchange(&state.outstanding, 0);
        state.generation = bot.generation;
        state.outstandingGeneration = 0;
        InterlockedIncrement(&state.requestVersion);
        state.request = {};
        state.requestSequence = 0;
        state.padObjectId = 0;
        state.requestTick = 0;
        InterlockedIncrement(&state.requestVersion);
        state.activationTick = 0;
        state.activationPosition = {};
        state.activationPositionValid = false;
        state.awaitingDisplacement = false;
        state.lastClearReason =
            static_cast<uint32_t>(ForcedPadClearReason::GenerationChange);
    }

    bool MissionPadScope(
        uint8_t* mode, uint8_t** roomOut, void** unitManagerOut)
    {
        if (!mode || mode != CurrentMode() || !g_missionLifecycle ||
            !g_game.unitManager || !g_game.isNetwork ||
            !g_game.isNetwork()) {
            return false;
        }
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        void* unitManager = *g_game.unitManager;
        if (!room || !unitManager ||
            !IsMissionRoomMode(
                *reinterpret_cast<uint16_t*>(room + kModeRoomMode)) ||
            !LocalIsRoomMaster(room) ||
            g_game.isObserver(unitManager)) {
            return false;
        }
        *roomOut = room;
        *unitManagerOut = unitManager;
        return true;
    }

    void RefreshMissionPadBots(
        uint8_t* mode, uint8_t* room, void* unitManager)
    {
        std::vector<Bot> refreshed;
        const int count =
            RecordCount(room, kRoomAiBegin, kRoomAiEnd);
        uint8_t* records =
            *reinterpret_cast<uint8_t**>(room + kRoomAiBegin);
        if (count <= 0 || !records) {
            g_missionPadBots.clear();
            g_missionPadMode = nullptr;
            return;
        }
        refreshed.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            uint8_t* record = records + index * kRecordSize;
            const uint32_t pilotId =
                *reinterpret_cast<uint32_t*>(
                    record + kRecordServerId);
            if (!(pilotId & kAiPilotBit))
                continue;
            const uint32_t objectId =
                g_missionLifecycle->object_for_pilot(
                    unitManager, pilotId);
            const int slot =
                objectId
                    ? g_missionLifecycle->slot_for_object(
                          unitManager, objectId)
                    : -1;
            auto* unit = objectId
                             ? static_cast<uint8_t*>(
                                   g_findObject(unitManager, objectId))
                             : nullptr;
            if (!unit || slot < 0)
                continue;

            Bot* previous = nullptr;
            for (Bot& candidate : g_missionPadBots)
                if (candidate.pilotId == pilotId &&
                    candidate.objectId == objectId &&
                    candidate.slot == slot &&
                    candidate.unit == unit) {
                    previous = &candidate;
                    break;
                }
            Bot bot =
                previous ? *previous
                         : Bot{pilotId, objectId, slot, unit};
            if (!previous)
                bot.generation = NextUnitGeneration();

            int32_t hp = 0;
            const bool alive =
                battle_ai::native::detail::Read(
                    unit, static_cast<size_t>(0x38), hp) &&
                hp > 0;
            if (bot.missionLifeObserved &&
                bot.missionAlive != alive) {
                bot.generation = NextUnitGeneration();
            }
            bot.missionLifeObserved = true;
            bot.missionAlive = alive;
            if (bot.forcedPad.generation != bot.generation)
                ResetForcedPadGeneration(bot);
            refreshed.push_back(bot);
        }
        g_missionPadBots.swap(refreshed);
        g_missionPadMode =
            g_missionPadBots.empty() ? nullptr : mode;
    }

    void MarkForcedPadActivation(Bot& bot, DWORD now)
    {
        ForcedPadState& state = bot.forcedPad;
        state.activationPositionValid =
            ReadBotPosition(bot, state.activationPosition);
        state.activationTick = now;
        state.awaitingDisplacement = true;
    }

    bool ForcedPadOwnedLocalAi(void* opaque, const uint8_t* unit)
    {
        auto* attempt = static_cast<ForcedPadAttempt*>(opaque);
        if (!attempt || !attempt->bot || !attempt->unitManager ||
            !g_game.unitManager || *g_game.unitManager != attempt->unitManager) {
            return false;
        }
        const Bot& bot = *attempt->bot;
        const bool registered =
            attempt->scope == ForcedPadScope::Pvp
                ? g_aiEnabled && g_botMode &&
                      CurrentMode() == g_botMode &&
                      SlotHolds(attempt->unitManager, bot)
                : g_missionPadMode &&
                      CurrentMode() == g_missionPadMode &&
                      MissionBotRegistered(
                          attempt->unitManager, bot);
        if (!unit || bot.unit != unit || bot.generation == 0 ||
            bot.forcedPad.generation != bot.generation ||
            !registered ||
            g_findObject(attempt->unitManager, bot.objectId) != unit) {
            return false;
        }
        uint32_t objectId = 0;
        uint32_t pilotId = 0;
        uint8_t localControl = 0;
        uint8_t remote = 0;
        uint8_t masterNpc = 0;
        const bool identity =
               battle_ai::native::detail::Read(
                   unit, kUnitObjectId, objectId) &&
               battle_ai::native::detail::Read(
                   unit, kUnitServerId, pilotId) &&
               battle_ai::native::detail::Read(
                   unit, kUnitLocalControl, localControl) &&
               battle_ai::native::detail::Read(
                   unit, kUnitRemote, remote) &&
               battle_ai::native::detail::Read(
                   unit, kUnitMasterNpc, masterNpc) &&
               objectId == bot.objectId && pilotId == bot.pilotId &&
               (pilotId & kAiPilotBit) != 0 &&
               localControl != 0 && remote == 0 && masterNpc != 0;
        if (!identity ||
            attempt->scope == ForcedPadScope::Pvp)
            return identity;
        uint8_t aiEnabled = 0;
        return battle_ai::native::detail::Read(
                   unit, kUnitAiEnabled, aiEnabled) &&
               aiEnabled != 0;
    }

    bool ReadLocalTransformActor(uint32_t pilot,match_ai::transform::LocalActor& actor)
    {
        if(!(pilot&kAiPilotBit)||!g_game.unitManager||!g_game.isNetwork||!g_game.isNetwork())return false;
        void* um=*g_game.unitManager;
        uint8_t* mode=CurrentMode();
        if(!um||!mode||g_game.isObserver(um))return false;
        uint8_t* room=nullptr;
        Bot mission{};const Bot* bot=nullptr;
        if(mode==g_botMode) {
            room=*reinterpret_cast<uint8_t**>(mode+kModeRoom);
            bot=BotByPilot(um,pilot);
            if(!bot)return false;
        } else {
            void* checkedUm=nullptr;
            if(!MissionPadScope(mode,&room,&checkedUm)||checkedUm!=um||!g_missionLifecycle)return false;
            const int count=RecordCount(room,kRoomAiBegin,kRoomAiEnd);
            uint8_t* records=*reinterpret_cast<uint8_t**>(room+kRoomAiBegin);
            bool listed=false;
            for(int i=0;records&&i<count&&i<12;++i)
                if(*reinterpret_cast<uint32_t*>(records+i*kRecordSize+kRecordServerId)==pilot)listed=true;
            if(!listed)return false;
            const uint32_t object=g_missionLifecycle->object_for_pilot(um,pilot);
            const int slot=object?g_missionLifecycle->slot_for_object(um,object):-1;
            if(!object||slot<0)return false;
            mission=Bot{pilot,object,slot,static_cast<uint8_t*>(g_findObject(um,object))};
            if(!MissionBotRegistered(um,mission))return false;
            bot=&mission;
        }
        if(!room||!LocalIsRoomMaster(room)||!bot->unit||g_findObject(um,bot->objectId)!=bot->unit)return false;
        using namespace battle_ai::native::detail;
        uint8_t *move=nullptr,*owner=nullptr;
        uint8_t local=0,remote=1,master=0;
        uint32_t current=0,pending=0;
        if(!Read(bot->unit,kUnitObjectId,actor.liveObject)||!Read(bot->unit,kUnitServerId,actor.livePilot)||
           !Read(bot->unit,kUnitLocalControl,local)||!Read(bot->unit,kUnitRemote,remote)||
           !Read(bot->unit,kUnitMasterNpc,master)||!Read(bot->unit,0xEC4,actor.lifeState)||
           !Read(bot->unit,kUnitControlWord,actor.hurtState)||
           !ReadPointer(bot->unit,kUnitMovementController,move)||!ReadPointer(move,0x21C,owner)||owner!=bot->unit||
           !Read(move,4,current)||!Read(move,8,pending)||!Read(move,0x2B0,actor.latchedTarget))return false;
        actor.registered=true;actor.network=true;actor.roomMaster=true;actor.observer=false;
        actor.pilot=bot->pilotId;actor.object=bot->objectId;
        actor.localControl=local!=0;actor.remoteCopy=remote!=0;actor.masterNpc=master!=0;
        actor.transforming=StateIs(move,current,0xD4);
        return true;
    }

    bool MatchesLocalTransformMode(uint8_t* unit,uint32_t mode,uint32_t target)
    {
        if(mode==1)return true;
        if(mode!=2||!target)return false;
        // TransformInit may already have swapped the definition while D4
        // still owns the animation. The native notice then produces mode2;
        // only the local receiver consumes that completion phase.
        using namespace battle_ai::native::detail;
        uint8_t* definition=nullptr;uint32_t current=0;
        return ReadPointer(unit,kActiveUnitDefinition,definition)&&
            Read(definition,kUnitId,current)&&current==target;
    }

    void __fastcall UnitNetworkMessage(uint8_t* unit,void*,const uint8_t* message)
    {
        if(!g_transformConfirmation.active) {g_originalUnitNetworkMessage(unit,message);return;}
        const DWORD savedError=GetLastError();bool local=false;
        __try {
            using battle_ai::native::detail::Read;
            uint16_t type=0;uint32_t object=0,mode=0,target=0;
            match_ai::transform::LocalActor actor{};
            local=Read(unit,kUnitObjectId,object)&&object==g_transformConfirmation.object&&
                Read(message,0,type)&&type==0x1D&&Read(message,0x10,mode)&&
                Read(message,0x14,target)&&target==g_transformConfirmation.target&&
                MatchesLocalTransformMode(unit,mode,target)&&
                ReadLocalTransformActor(g_transformConfirmation.pilot,actor)&&
                match_ai::transform::ReusesLocalTransform(actor,actor.pilot,target,0);
            if(local&&mode==2&&g_logDiagnostics)g_api->Log(g_mod,SDGO_LOG_INFO,
                "native AI transform completion object=%u target=%u mode=2 via local receiver",object,target);
        } __except(EXCEPTION_EXECUTE_HANDLER) {local=false;}
        SetLastError(savedError);
        // The local receiver consumes the original phase1/phase2 Msg1D.
        // The generic receiver requeues D4 for phase1 and ignores phase2.
        // The native receiver owns every FSM/model change.
        if(local)g_unitCombatEvent(unit,message);
        else g_originalUnitNetworkMessage(unit,message);
    }

    int __stdcall TransformNotice(const uint8_t* packet)
    {
        const DWORD savedError=GetLastError();
        uint8_t adjusted[0x13]{};const uint8_t* delivered=packet;
        const auto previous=g_transformConfirmation;
        __try {
            using battle_ai::native::detail::Read;
            uint16_t opcode=0;uint32_t pilot=0,target=0;uint8_t replay=0;
            match_ai::transform::LocalActor actor{};
            if(packet&&Read(packet,0,opcode)&&opcode==0xA44&&Read(packet,6,pilot)&&
               Read(packet,0xA,target)&&Read(packet,0xE,replay)) {
                const bool owned=ReadLocalTransformActor(pilot,actor);
                const bool retained=owned&&match_ai::transform::ReusesLocalTransform(actor,pilot,target,replay);
                if(retained) {
                    memcpy(adjusted,packet,sizeof(adjusted));adjusted[0xE]=0;delivered=adjusted;
                    g_transformConfirmation={true,pilot,actor.object,target};
                }
                if(g_logDiagnostics&&(pilot&kAiPilotBit))g_api->Log(g_mod,SDGO_LOG_INFO,
                    "native AI transform notice pilot=%08X object=%u target=%u replay=%u owned=%u transforming=%u latch=%u hurt=%u local=%u remote=%u master=%u retained=%u",
                    pilot,actor.object,target,replay,owned,actor.transforming,actor.latchedTarget,actor.hurtState,
                    actor.localControl,actor.remoteCopy,actor.masterNpc,retained);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {delivered=packet;g_transformConfirmation=previous;}
        SetLastError(savedError);
        int result=0;
        __try {result=g_originalTransformNotice(delivered);}
        __finally {g_transformConfirmation=previous;}
        return result;
    }

    bool ForcedPadActionAllowed(
        void*, const uint8_t* unit);
    bool ForcedPadControlAllowed(
        void*, const uint8_t* unit);

    bool GateOwnedLocalNpc(void*,uint32_t pilot,uint32_t objectId)
    {
        if(!g_aiEnabled||!g_botMode||CurrentPvpMode()!=g_botMode||!g_game.unitManager||!g_game.isNetwork||!g_game.isNetwork())return false;
        void* um=*g_game.unitManager;
        uint8_t* room=*reinterpret_cast<uint8_t**>(g_botMode+kModeRoom);
        if(!um||!room||!LocalIsRoomMaster(room)||g_game.isObserver(um))return false;
        const Bot* bot=BotByPilot(um,pilot);
        if(!bot||!bot->unit||!bot->generation||bot->objectId!=objectId||!SlotHolds(um,*bot)||g_findObject(um,objectId)!=bot->unit)return false;
        // Completion ownership survives an attacker's death, just like the
        // original local-human path. Registration and generation remain live.
        return *reinterpret_cast<uint32_t*>(bot->unit+kUnitObjectId)==objectId&&
               *reinterpret_cast<uint32_t*>(bot->unit+kUnitServerId)==pilot&&
               bot->unit[kUnitLocalControl]!=0&&bot->unit[kUnitRemote]==0&&bot->unit[kUnitMasterNpc]!=0;
    }
    SdgoPvpAiClientGateStatusV1 g_gateStatus{};
    void* g_gateAuthorityOriginal=nullptr;
    void __stdcall GateAuthorityForMessage(const uint8_t* packet,uint32_t attackerObject,uint8_t* gate,uint8_t* authority)
    {
        __try {
            if(!packet||!gate||!authority||*authority||*reinterpret_cast<const uint16_t*>(packet)!=0x534)return;
            const uint8_t count=packet[0xA];
            if(!count||count>battle_ai::native::kGateDamageCapacity)return;
            auto table=*reinterpret_cast<void***>(gate);
            using IsA=bool(__thiscall*)(void*,int);
            if(!table||!table[1]||!reinterpret_cast<IsA>(table[1])(gate,battle_ai::native::kDestroyableBlockKind))return;
            const uint32_t gateId=*reinterpret_cast<uint32_t*>(gate+4);bool listed=false;
            for(unsigned i=0;i<count;++i)if(*reinterpret_cast<const uint32_t*>(packet+0xB+4*i)==gateId)listed=true;
            if(!listed||!g_game.unitManager||!*g_game.unitManager)return;
            auto um=static_cast<uint8_t*>(*g_game.unitManager);auto human=*reinterpret_cast<uint8_t**>(um+0x88);
            const uint32_t pilot=*reinterpret_cast<const uint32_t*>(packet+6);
            if(!battle_ai::native::OwnsGateCountdown(false,pilot,attackerObject,human?*reinterpret_cast<uint32_t*>(human+4):0,&GateOwnedLocalNpc,nullptr))return;
            *authority=1;
            const uint32_t sequence=static_cast<uint32_t>(InterlockedCompareExchange(&g_receivedBattleStartCount,0,0));
            if(g_gateStatus.battle_start_sequence!=sequence)g_gateStatus={};
            g_gateStatus.battle_start_sequence=sequence;++g_gateStatus.authority_count;
            g_gateStatus.last_attacker_pilot=pilot;g_gateStatus.last_attacker_object=attackerObject;g_gateStatus.last_gate_object=gateId;
            if(const auto bot=BotByPilot(um,pilot))g_gateStatus.last_generation=bot->generation;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    __declspec(naked) void GateAuthoritySite()
    {
        __asm {
            pushfd
            pushad
            mov eax,esp
            sub esp,210h
            and esp,0FFFFFFF0h
            mov [esp+200h],eax
            fxsave [esp]
            lea eax,[ebp-20h]
            push eax
            push edi
            push esi
            push dword ptr [ebp+8]
            call GateAuthorityForMessage
            fxrstor [esp]
            mov esp,[esp+200h]
            popad
            popfd
            jmp dword ptr [g_gateAuthorityOriginal]
        }
    }
    bool ForcedPadActionAllowed(void*, const uint8_t* unit)
    {
        if (!unit || !g_movementState || !g_attackState)
            return false;
        uint8_t* movement = nullptr;
        uint8_t* attack = nullptr;
        if (!battle_ai::native::detail::ReadPointer(
                unit, kUnitMovementController, movement) ||
            !battle_ai::native::detail::ReadPointer(
                unit, kUnitAttackController, attack)) {
            return false;
        }
        const uint8_t movementState = g_movementState(movement);
        const uint8_t attackState = g_attackState(attack);
        if (movementState == 0x0E ||
            (attackState >= 7 && attackState <= 12) ||
            battle_ai::native::detail::InState(
                movement, kForcedMoveStateNode)) {
            return false;
        }
        for (size_t node : kBlockedAttackNodes)
            if (battle_ai::native::detail::InState(attack, node))
                return false;
        return true;
    }

    bool ForcedPadControlAllowed(void*, const uint8_t* unit)
    {
        uint32_t control = 0;
        return unit &&
               battle_ai::native::detail::Read(
                   unit, kUnitControlWord, control) &&
               control != 3 && control != 4;
    }

    bool ForcedPadMoving(void* opaque, const uint8_t* unit)
    {
        auto* attempt = static_cast<ForcedPadAttempt*>(opaque);
        if (attempt && attempt->bot) {
            const ForcedPadState& state = attempt->bot->forcedPad;
            if (state.generation == attempt->bot->generation &&
                state.outstandingGeneration == attempt->bot->generation &&
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.outstanding), 0, 0) != 0) {
                return true;
            }
        }
        return NativeForcedMoving(unit);
    }

    uint8_t* ForcedPadRoom(const ForcedPadAttempt& attempt)
    {
        uint8_t* mode = nullptr;
        if (attempt.scope == ForcedPadScope::Pvp) {
            mode = CurrentPvpMode();
            if (mode != g_botMode)
                return nullptr;
        } else {
            mode = CurrentMode();
            if (mode != g_missionPadMode)
                return nullptr;
        }
        return mode
                   ? *reinterpret_cast<uint8_t**>(
                         mode + kModeRoom)
                   : nullptr;
    }

    const Bot* ForcedPadBotByPilot(
        const ForcedPadAttempt& attempt, uint32_t pilotId)
    {
        return attempt.scope == ForcedPadScope::Pvp
                   ? BotByPilot(attempt.unitManager, pilotId)
                   : MissionBotByPilot(
                         attempt.unitManager, pilotId);
    }

    bool ForcedPadAuthority(
        void* opaque, const uint8_t* unit,
        const uint8_t* forcedMoveObject,
        const battle_ai::native::ForceMoveRequest085A* request)
    {
        auto* attempt = static_cast<ForcedPadAttempt*>(opaque);
        if (!attempt || !attempt->bot || !forcedMoveObject || !request ||
            !ForcedPadOwnedLocalAi(opaque, unit) ||
            !g_game.isNetwork || !g_game.isNetwork() ||
            request->opcode != battle_ai::native::kForceMoveRequestOpcode ||
            request->routeId != 0 ||
            request->pilotId != attempt->bot->pilotId ||
            (request->pilotId & kAiPilotBit) == 0) {
            return false;
        }
        uint8_t* room = ForcedPadRoom(*attempt);
        if (!room || !LocalIsRoomMaster(room) ||
            g_game.isObserver(attempt->unitManager)) {
            return false;
        }
        if (ForcedPadBotByPilot(
                *attempt, request->pilotId) !=
            attempt->bot)
            return false;
        attempt->padObject = const_cast<uint8_t*>(
            forcedMoveObject);
        uint32_t objectId = 0;
        attempt->padObjectId =
            battle_ai::native::detail::Read(
                forcedMoveObject,
                kUnitObjectId,
                objectId)
                ? objectId
                : 0;
        return true;
    }

    bool ForcedPadSubmit(
        void* opaque,
        const battle_ai::native::ForceMoveRequest085A* request)
    {
        auto* attempt = static_cast<ForcedPadAttempt*>(opaque);
        if (!attempt || !attempt->bot || !request ||
            !ForcedPadOwnedLocalAi(opaque, attempt->bot->unit) ||
            !ForcedPadActionAllowed(opaque, attempt->bot->unit) ||
            !ForcedPadControlAllowed(opaque, attempt->bot->unit) ||
            ForcedPadMoving(opaque, attempt->bot->unit) ||
            !g_game.isNetwork || !g_game.isNetwork()) {
            return false;
        }

        uint8_t* room = ForcedPadRoom(*attempt);
        if (!room || !LocalIsRoomMaster(room) ||
            g_game.isObserver(attempt->unitManager)) {
            return false;
        }

        // The native getter can initialize its singleton.  Reach it only after
        // the live battle, ownership, host, and network checks above.
        uint8_t* sender = g_forceMoveSender ? g_forceMoveSender() : nullptr;
        uint32_t connection = 0;
        if (!sender || !g_sendForceMovePacket ||
            !battle_ai::native::ReadForceMoveConnection(sender,connection)) {
            return false;
        }

        Bot& bot = *attempt->bot;
        ForcedPadState& state = bot.forcedPad;
        if (InterlockedCompareExchange(&state.outstanding, 1, 0) != 0)
            return false;
        state.outstandingGeneration = bot.generation;
        InterlockedIncrement(&state.requestVersion);
        state.request = *request;
        state.requestSequence = 0;
        state.padObjectId =
            attempt->padObjectId;
        state.requestTick = GetTickCount();
        InterlockedIncrement(&state.requestVersion);
        state.awaitingDisplacement = false;
        state.activationPositionValid = false;
        state.lastClearReason =
            static_cast<uint32_t>(ForcedPadClearReason::None);

        ++*reinterpret_cast<uint32_t*>(sender + 0x58);
        *reinterpret_cast<uint32_t*>(sender + 0x5C) +=
            static_cast<uint32_t>(sizeof(*request));
        // 6D0C21..6D0C36 forwards sender+14 as an opaque connection value and
        // uses the sender itself as ECX. It does not dereference that value or
        // inspect sender+10. ReadPointer would reject valid native handles.
        const int sent = g_sendForceMovePacket(
            sender, reinterpret_cast<void*>(static_cast<uintptr_t>(connection)), request, static_cast<int>(sizeof(*request)), 0);
        if (!sent) {
            ++state.sendFailureCount;
            InterlockedIncrement(
                &state.requestVersion);
            state.requestSequence = 0;
            InterlockedIncrement(
                &state.requestVersion);
            ClearForcedPadOutstanding(
                bot, ForcedPadClearReason::SendFailure);
            return false;
        }
        InterlockedIncrement(&state.requestVersion);
        ++state.requestCount;
        state.requestSequence =
            state.requestCount;
        InterlockedIncrement(&state.requestVersion);
        return true;
    }

    void ReconcileForcedPad(
        Bot& bot, void* unitManager, DWORD now,
        ForcedPadScope scope)
    {
        if (bot.forcedPad.generation != bot.generation)
            ResetForcedPadGeneration(bot);

        ForcedPadAttempt attempt{&bot, unitManager, scope};
        if (!ForcedPadOwnedLocalAi(&attempt, bot.unit)) {
            if (ClearForcedPadOutstanding(
                    bot, ForcedPadClearReason::OwnershipLoss)) {
                ++bot.forcedPad.ownershipLossCount;
            }
            bot.forcedPad.awaitingDisplacement = false;
            bot.forcedPad.activationPositionValid = false;
            return;
        }

        ForcedPadState& state = bot.forcedPad;
        if (NativeForcedMoving(bot.unit) &&
            ClearForcedPadOutstanding(
                bot, ForcedPadClearReason::NativeState)) {
            MarkForcedPadActivation(bot, now);
        }

        if (InterlockedCompareExchange(&state.outstanding, 0, 0) != 0 &&
            state.outstandingGeneration == bot.generation &&
            static_cast<DWORD>(now - state.requestTick) >=
                kForcedPadRequestTimeoutMs) {
            if (ClearForcedPadOutstanding(
                    bot, ForcedPadClearReason::Timeout)) {
                ++state.timeoutCount;
            }
        }

        if (!state.awaitingDisplacement)
            return;
        if (static_cast<DWORD>(now - state.activationTick) >=
            kForcedPadDisplacementTimeoutMs) {
            state.awaitingDisplacement = false;
            state.activationPositionValid = false;
            return;
        }

        battle_ai::Vec3 position{};
        if (!ReadBotPosition(bot, position))
            return;
        if (!state.activationPositionValid) {
            state.activationPosition = position;
            state.activationPositionValid = true;
            return;
        }
        const battle_ai::Vec3 delta =
            position - state.activationPosition;
        const double distanceSquared =
            static_cast<double>(delta.x) * delta.x +
            static_cast<double>(delta.y) * delta.y +
            static_cast<double>(delta.z) * delta.z;
        if (std::isfinite(distanceSquared) &&
            distanceSquared >= kForcedPadDisplacementSquared) {
            ++state.displacementCount;
            state.awaitingDisplacement = false;
        }
    }

    void UpdateForcedPads()
    {
        if (!g_forcedPadBridgeAvailable || g_botTable.empty())
            return;
        void* unitManager =
            g_game.unitManager ? *g_game.unitManager : nullptr;
        const DWORD now = GetTickCount();
        for (Bot& bot : g_botTable) {
            ReconcileForcedPad(
                bot, unitManager, now,
                ForcedPadScope::Pvp);
            if (!unitManager || !g_aiEnabled || !g_botMode ||
                CurrentMode() != g_botMode || bot.generation == 0) {
                continue;
            }
            ForcedPadAttempt attempt{
                &bot, unitManager, ForcedPadScope::Pvp};
            battle_ai::native::MapTransportApi api = g_forcedPadApi;
            api.context = &attempt;
            const auto result = battle_ai::native::TryMapTransport(
                api, kForcedPadBounds,
                static_cast<uint8_t*>(unitManager), bot.unit,
                &bot.forcedPad.transport);
            bot.forcedPad.lastResult = static_cast<uint32_t>(result);
        }
    }

    void UpdateMissionForcedPads(uint8_t* mode)
    {
        if (!g_forcedPadBridgeAvailable) {
            g_missionPadBots.clear();
            g_missionPadMode = nullptr;
            return;
        }
        uint8_t* room = nullptr;
        void* unitManager = nullptr;
        if (!MissionPadScope(
                mode, &room, &unitManager)) {
            g_missionPadBots.clear();
            g_missionPadMode = nullptr;
            return;
        }
        RefreshMissionPadBots(
            mode, room, unitManager);
        const DWORD now = GetTickCount();
        for (Bot& bot : g_missionPadBots) {
            ReconcileForcedPad(
                bot, unitManager, now,
                ForcedPadScope::Mission);
            if (g_missionPadMode != mode ||
                bot.generation == 0) {
                continue;
            }
            ForcedPadAttempt attempt{
                &bot, unitManager,
                ForcedPadScope::Mission};
            battle_ai::native::MapTransportApi api =
                g_forcedPadApi;
            api.context = &attempt;
            const auto result =
                battle_ai::native::TryMapTransport(
                    api, kForcedPadBounds,
                    static_cast<uint8_t*>(unitManager),
                    bot.unit,
                    &bot.forcedPad.transport);
            bot.forcedPad.lastResult =
                static_cast<uint32_t>(result);
        }
    }

    int g_forcedPadFaults = 0;

    bool GuardedUpdateForcedPads()
    {
        __try {
            UpdateForcedPads();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (g_forcedPadFaults++ < 5)
                g_api->Log(
                    g_mod, SDGO_LOG_ERROR,
                    "exception %08lX in the forced-pad bridge",
                    GetExceptionCode());
        }
        return true;
    }

    void ObserveForceMoveNotice(
        const battle_ai::native::ForceMoveNotice0584& notice)
    {
        if (notice.opcode != battle_ai::native::kForceMoveNoticeOpcode ||
            !(notice.pilotId & kAiPilotBit) ||
            !g_game.unitManager) {
            return;
        }
        void* unitManager = *g_game.unitManager;
        const Bot* found = nullptr;
        if (g_botMode && CurrentMode() == g_botMode)
            found = BotByPilot(
                unitManager, notice.pilotId);
        if (!found && g_missionPadMode &&
            CurrentMode() == g_missionPadMode)
            found = MissionBotByPilot(
                unitManager, notice.pilotId);
        if (!found)
            return;
        Bot& bot = *const_cast<Bot*>(found);
        if (bot.forcedPad.generation != bot.generation)
            ResetForcedPadGeneration(bot);
        ++bot.forcedPad.noticeCount;
        if (bot.forcedPad.outstandingGeneration == bot.generation)
            ClearForcedPadOutstanding(
                bot, ForcedPadClearReason::Notice);
        if (NativeForcedMoving(bot.unit))
            MarkForcedPadActivation(bot, GetTickCount());
    }

    int __stdcall ForceMoveNotice(const uint8_t* packet)
    {
        battle_ai::native::ForceMoveNotice0584 notice{};
        const bool copied =
            packet && battle_ai::native::detail::Copy(
                          packet, &notice, sizeof(notice));
        const int result = g_originalForceMoveNotice(packet);
        if (copied) {
            __try {
                ObserveForceMoveNotice(notice);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (g_forcedPadFaults++ < 5)
                    g_api->Log(
                        g_mod, SDGO_LOG_ERROR,
                        "exception %08lX observing forced-move notice",
                        GetExceptionCode());
            }
        }
        return result;
    }

    // The Teleport item's 0x07D4 notification normally sends a player through
    // the native start-point relocation path.  A match_ai participant is a CNPC,
    // so the stock handler takes its local-authority NPC fallback instead and
    // reports PT_REQ_MAP_DAMAGE (0x0492), destroying a healthy PvP bot.
    //
    // Admit only the exact live, locally controlled participant named by this
    // packet.  The site detour then clears the saved IsA(10) result so the
    // original handler performs its ordinary point lookup and message 0x34.
    int __stdcall TeleportRelocationParticipant(uint8_t* unit, const uint8_t* packet)
    {
        const DWORD incomingError = GetLastError();
        int admitted = 0;
        __try {
            if (unit && packet && g_game.unitManager && g_aiEnabled &&
                *reinterpret_cast<const uint16_t*>(packet) == kTeleportRelocationOpcode &&
                packet[10] < kTeleportRelocationPointCount) {
                uint8_t* mode = CurrentPvpMode();
                void* um = *g_game.unitManager;
                if (mode && mode == g_botMode && um) {
                    const uint32_t pilotId = *reinterpret_cast<const uint32_t*>(packet + 6);
                    const Bot* bot =
                        BotByObject(um, *reinterpret_cast<uint32_t*>(unit + kUnitObjectId));
                    admitted =
                        bot && bot->unit == unit && bot->generation != 0 &&
                        bot->pilotId == pilotId &&
                        *reinterpret_cast<uint32_t*>(unit + kUnitServerId) == pilotId &&
                        unit[kUnitLocalControl] != 0 && unit[kUnitRemote] == 0 &&
                        unit[kUnitMasterNpc] != 0;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        SetLastError(incomingError);
        return admitted;
    }

    void* g_teleportNpcGateOriginal = nullptr;

    __declspec(naked) void TeleportNpcGate()
    {
        __asm {
            pushfd
            pushad
            mov eax, esp
            sub esp, 210h
            and esp, 0FFFFFFF0h
            mov [esp+200h], eax
            fxsave [esp]
            fninit
            ldmxcsr dword ptr [g_teleportDefaultMxcsr]
            cld
            push dword ptr [ebp-70h]
            push edi
            call TeleportRelocationParticipant
            mov [esp+204h], eax
            fxrstor [esp]
            mov eax, [esp+204h]
            mov esp, [esp+200h]
            test eax, eax
            jz native_npc_path
            // pushad saved EAX at +28. Clear only AL, the IsA(10) result
            // consumed by the relocated native `test al, al`.
            mov byte ptr [esp+28], 0
        native_npc_path:
            popad
            popfd
            jmp dword ptr [g_teleportNpcGateOriginal]
        }
    }

    // Observe the actual native TAB kill/assist/points renderer, rather than infer
    // visibility from unit registration or from the separate teammate HUD.
    void __fastcall ScoreboardRow(uint8_t* panel, void*, uint8_t* slot, int row)
    {
        g_originalRosterRow(panel, slot, row);
        if (panel != g_rosterPanel || !slot || row < 0 || row >= 12) return;
        __try {
            void* um = *g_game.unitManager;
            const Bot* bot = BotByObject(um, *reinterpret_cast<uint32_t*>(slot + kSlotObjectId));
            if (!bot || g_originalPlayerSlot(um, bot->slot) != slot) return;
            const size_t index = static_cast<size_t>(bot - g_botTable.data());
            if (index >= 12) return;
            auto& recorded = g_scoreboardStatus.bots[index];
            recorded.pilot_id = bot->pilotId;
            recorded.object_id = bot->objectId;
            const uint8_t* record = *reinterpret_cast<const uint8_t* const*>(slot + kSlotRecord);
            recorded.team = record[kRecordTeam];
            recorded.kills = *reinterpret_cast<int32_t*>(slot + 0x4C);
            recorded.assists = *reinterpret_cast<int32_t*>(slot + 0x54);
            recorded.points = *reinterpret_cast<int32_t*>(slot + 0x58);
            recorded.row_index = static_cast<uint32_t>(row);
            ++recorded.render_count;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // The teammate HUD builds one row per player slot, but after FindPlayer it
    // rejects every IsA(10) unit.  match_ai participants are CNPCs, so that native
    // check hides every AI row even though the exact 1001+ object-id lookup succeeds.
    // Override only the saved IsA result for a live, registered match_ai participant;
    // ordinary NPCs and stale/reused slots retain the native filter.
    int __stdcall ScoreboardParticipant(uint8_t* unit)
    {
        if (!unit || !g_game.unitManager)
            return 0;
        __try {
            void* um = *g_game.unitManager;
            if (!um)
                return 0;
            const Bot* bot = BotByObject(um, *reinterpret_cast<uint32_t*>(unit + kUnitObjectId));
            return bot && bot->unit == unit;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // The native life-cache pass (0x5394E0) has its own NPC filter, separate
    // from the roster and teammate HUD. Its result is unit+0x2F9C, read by
    // the original TAB renderer at 0x532E64. Keep the original calculation,
    // slot remapping and deck rules; admit only our registered participants.
    int __stdcall ScoreboardLifeCost(uint8_t* unit, int nativeCost)
    {
        if (!ScoreboardParticipant(unit))
            return nativeCost;
        __try {
            const Bot* bot = BotByObject(*g_game.unitManager,
                *reinterpret_cast<uint32_t*>(unit + kUnitObjectId));
            const StartDeck* deck = bot ? FindStartDeck(bot->pilotId) : nullptr;
            const int form = bot && bot->form >= 0 && bot->form < 4 ? bot->form : 0;
            return deck && deck->lifeCost[form] > 0 ? deck->lifeCost[form] : nativeCost;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nativeCost;
        }
    }

    int __stdcall ScoreboardLifeParticipant(uint8_t* unit, uint8_t* hud)
    {
        if (!hud || !ScoreboardParticipant(unit))
            return 0;
        __try {
            // The death-match branch dereferences unit data before dividing.
            // An invalid record must not introduce a new native zero divisor.
            if (*reinterpret_cast<uint32_t*>(hud + 0x30) == 3) {
                const uint8_t* data = *reinterpret_cast<uint8_t**>(unit + 0x9C4);
                if (!data)
                    return 0;
                const int nativeCost = *reinterpret_cast<const int16_t*>(data + 0x137);
                if (ScoreboardLifeCost(unit, nativeCost) <= 0)
                    return 0;
            }
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    void* g_scoreboardLifeNpcOriginal = nullptr;
    void* g_scoreboardLifeCostOriginal = nullptr;

    __declspec(naked) void ScoreboardLifeNpcGate()
    {
        __asm {
            pushfd
            pushad
            push ebx
            push esi
            call ScoreboardLifeParticipant
            test eax, eax
            jz native_filter
            mov byte ptr [esp+28], 0
        native_filter:
            popad
            popfd
            jmp dword ptr [g_scoreboardLifeNpcOriginal]
        }
    }

    __declspec(naked) void ScoreboardLifeCostGate()
    {
        __asm {
            pushfd
            pushad
            push ebx
            push esi
            call ScoreboardLifeCost
            // Only EBX, the divisor about to be saved by the native pass,
            // changes. Preserve the slot's power in EDI and all other state.
            mov dword ptr [esp+16], eax
            popad
            popfd
            jmp dword ptr [g_scoreboardLifeCostOriginal]
        }
    }

    __declspec(naked) void ScoreboardNpcGate()
    {
        __asm {
            pushfd
            pushad
            push dword ptr [ebp-84h]
            call ScoreboardParticipant
            test eax, eax
            jz native_filter
            // pushad saved EAX at +28.  Clear only AL, the IsA(10) result that
            // the relocated native `test al, al` consumes in the trampoline.
            mov byte ptr [esp+28], 0
        native_filter:
            popad
            popfd
            jmp dword ptr [g_scoreboardNpcGateOriginal]
        }
    }

    // No game mode, or one of the PvP modes: slot indexes >= 1000 are not mapped by the game.
    bool OutsideAiModes()
    {
        uint8_t* mode = CurrentMode();
        if (!mode)
            return true;
        __try {
            return IsPvpMode(mode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return true;
        }
    }

    alignas(16) uint8_t g_scratchSlot[0x70] = {};
    void* g_strayCallers[8] = {};

    void LogStraySlot(int index, void* caller)
    {
        for (void*& seen : g_strayCallers) {
            if (seen == caller)
                return;
            if (!seen) {
                seen = caller;
                g_api->Log(g_mod, SDGO_LOG_WARN, "player slot %d asked for by %p: no AI player there, scratch slot given",
                           index, caller);
                return;
            }
        }
    }

    uint32_t __fastcall ObjectIdFromPilot(void* um, void* /*edx*/, uint32_t pilotId)
    {
        if (const Bot* bot = BotByPilot(um, pilotId))
            return bot->objectId;
        if (g_missionLifecycle)
            if (const uint32_t object = g_missionLifecycle->object_for_pilot(um, pilotId))
                return object;
        return g_originalObjectIdFromPilot(um, pilotId);
    }

    bool __fastcall IsPlayerObject(void* um, void* /*edx*/, uint32_t objectId)
    {
        return g_originalIsPlayerObject(um, objectId) || BotByObject(um, objectId) != nullptr ||
            (g_missionLifecycle && g_missionLifecycle->slot_for_object(um, objectId) >= 0);
    }

    uint8_t* __fastcall PlayerSlot(void* um, void* /*edx*/, int index)
    {
        if (index >= 1000) {
            if (const Bot* bot = BotByObject(um, static_cast<uint32_t>(index) + 1))
                return g_originalPlayerSlot(um, bot->slot);
            const int missionSlot = g_missionLifecycle ?
                g_missionLifecycle->slot_for_object(um, static_cast<uint32_t>(index) + 1) : -1;
            if (missionSlot >= 0) return g_originalPlayerSlot(um, missionSlot);
            if (OutsideAiModes()) {
                LogStraySlot(index, _ReturnAddress());
                memset(g_scratchSlot, 0, sizeof(g_scratchSlot));
                return g_scratchSlot;
            }
        }
        return g_originalPlayerSlot(um, index);
    }

    uint8_t* SearchParticipant(void* um, uint32_t compactId)
    {
        if (!um || um != g_targetSearchUm || compactId < 1 || compactId > kSearchParticipantLimit)
            return nullptr;
        for (const Bot& bot : g_botTable) {
            if (bot.slot >= 0 && static_cast<uint32_t>(bot.slot + 1) == compactId && SlotHolds(um, bot))
                return static_cast<uint8_t*>(g_originalFindObject(um, bot.objectId));
        }
        return nullptr;
    }

    void* __fastcall FindObjectForSearch(void* um, void* /*edx*/, uint32_t objectId)
    {
        void* unit = g_originalFindObject(um, objectId);
        // Native eligibility/visibility helpers repeat FindObject using the
        // same compact loop index. They must resolve the same real participant.
        return unit ? unit : SearchParticipant(um, objectId);
    }

    void* __fastcall FindPlayer(void* um, void* /*edx*/, uint32_t objectId)
    {
        void* unit = g_originalFindPlayer(um, objectId);
        if (!unit && BotByObject(um, objectId))
            unit = g_findObject(um, objectId);
        // Native LockTarget enumerates 1..playerCount, but AddObject assigns
        // these AI participants ids 1001+. Supply the missing compact *search
        // index* only inside that call. Native filtering and SetAttackTarget
        // still use candidate+4, its real object id. HUD/packet lookups outside
        // this scope must retain exact-id semantics, including missing humans.
        if (!unit) unit = SearchParticipant(um, objectId);
        return unit;
    }

    // After the respawn packet (message 7 placed the unit): the deck switch of an AI player
    // whose deck entry changed, as CPlayer's message 7 handler switches a player's.
    void SwitchPendingDecks()
    {
        for (Bot& bot : g_botTable) {
            if (bot.pendingForm < 0 && !bot.pendingInitialize)
                continue;
            int form = bot.pendingForm;
            bot.pendingForm = -1;
            if (form >= 0) {
                int size = DeckSize(bot.unit);
                if (size < 2 || form >= size) {
                    g_api->Log(g_mod, SDGO_LOG_WARN,
                               "AI player %08lX: deck index %d, but the unit deck has %d entries",
                               static_cast<unsigned long>(bot.pilotId), form, size);
                    bot.pendingInitialize = false;
                    continue;
                } else {
                    g_switchDeck(bot.unit, form, 0);
                    *reinterpret_cast<int*>(bot.unit + kUnitDeckIndex) = form;
                    bot.form = form;
                    g_api->Log(g_mod, SDGO_LOG_INFO, "AI player %08lX: now deck unit %d (NPC %lu)",
                               static_cast<unsigned long>(bot.pilotId), form,
                               static_cast<unsigned long>(bot.deckNpc[form]));
                }
            }
            if (bot.pendingInitialize) {
                bot.pendingInitialize = false;
                bot.generation = NextUnitGeneration();
                ApplyAdvertisedAceHealth(bot, bot.form, false);
                ApplyAcePilotName(g_game.unitManager ? *g_game.unitManager : nullptr, bot);
                InitializeBattleAiUnit(bot);
            }
        }
    }

    // ---- dead view: AI players can be watched -------------------------------------------
    //
    // In death match, tag match and grid match a down local player's click points the camera
    // at the next unit of its team in a copy of UM's player list (0x00AD7A40), which holds
    // only CPlayer units. The AI players' units join that copy, at its end, with nodes from
    // the game's own operator new: the game frees the list itself.

    struct ListNode
    {
        ListNode* next;
        ListNode* prev;
        uint8_t* unit;
    };

    struct UnitList
    {
        ListNode* head;
        uint32_t size;
    };

    int g_deadViewLogs = 0;

    // Diagnostic (LogDiagnostics), the first clicks of a game: the camera (UM +0x82C8: target
    // +0x344, position +0x348..+0x350, offsets +0x354..+0x35C) and, for each unit of the list,
    // its unit data's camera offsets (+0xFB5..+0xFBD) and its aim (+0xD94, +0xD9C).
    constexpr size_t kUmCamera = 0x82C8;
    constexpr size_t kCameraTarget = 0x344;
    constexpr size_t kUnitData = 0x9C4;

    float FloatAt(const uint8_t* p, size_t offset)
    {
        return *reinterpret_cast<const float*>(p + offset);
    }

    void LogViewUnit(const char* who, const uint8_t* unit)
    {
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(unit + kUnitData);
        g_api->Log(g_mod, SDGO_LOG_INFO, "dead view %s: object %lu, camera offsets (%.1f, %.1f, %.1f), aim (%.3f, %.3f)",
                   who, static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(unit + kUnitObjectId)),
                   data ? FloatAt(data, 0xFB5) : 0.0f, data ? FloatAt(data, 0xFB9) : 0.0f, data ? FloatAt(data, 0xFBD) : 0.0f,
                   FloatAt(unit, 0xD94), FloatAt(unit, 0xD9C));
    }

    void LogViewState(void* um, UnitList* list)
    {
        const uint8_t* camera = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(um) + kUmCamera);
        if (camera)
            g_api->Log(g_mod, SDGO_LOG_INFO, "dead view camera: target %p, at (%.1f, %.1f, %.1f), offsets (%.1f, %.1f, %.1f)",
                       *reinterpret_cast<void* const*>(camera + kCameraTarget), FloatAt(camera, 0x348),
                       FloatAt(camera, 0x34C), FloatAt(camera, 0x350), FloatAt(camera, 0x354), FloatAt(camera, 0x358),
                       FloatAt(camera, 0x35C));
        for (ListNode* node = list->head->next; node != list->head; node = node->next)
            if (node->unit)
                LogViewUnit("player", node->unit);
        for (const Bot& bot : g_botTable)
            if (auto unit = static_cast<uint8_t*>(g_findObject(um, bot.objectId)))
                LogViewUnit("AI", unit);
    }

    void AddAiToDeadView(UnitList* list)
    {
        __try {
            if (!list || !list->head || !g_botMode || CurrentPvpMode() != g_botMode)
                return;
            void* um = *g_game.unitManager;
            if (!um)
                return;
            if (g_logDiagnostics && g_viewStateLogs < 4) {
                ++g_viewStateLogs;
                LogViewState(um, list);
            }
            int added = 0;
            for (const Bot& bot : g_botTable) {
                auto unit = static_cast<uint8_t*>(g_findObject(um, bot.objectId));
                if (!unit)
                    continue;
                bool listed = false;
                for (ListNode* node = list->head->next; node != list->head && !listed; node = node->next)
                    listed = node->unit == unit;
                if (listed)
                    continue;
                auto node = static_cast<ListNode*>(g_gameNew(sizeof(ListNode)));
                if (!node)
                    break;
                node->unit = unit;
                node->next = list->head;
                node->prev = list->head->prev;
                list->head->prev->next = node;
                list->head->prev = node;
                ++list->size;
                ++added;
            }
            if (g_deadViewLogs < 3) {
                ++g_deadViewLogs;
                g_api->Log(g_mod, SDGO_LOG_INFO, "dead view: %d AI player(s) added to %lu player(s)", added,
                           static_cast<unsigned long>(list->size - added));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (g_deadViewLogs < 3) {
                ++g_deadViewLogs;
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX adding the AI players to the dead view",
                           GetExceptionCode());
            }
        }
    }

    // ---- the camera looks where an AI player fights ---------------------------------------
    //
    // Each frame the tracking camera (CTrackingCamera vt slot 7, 0x00493F30) takes its yaw and
    // pitch from the target's aim (unit +0xD94, +0xD9C), a player's view. An AI unit's are not
    // where it fights (often up in the sky), so while the camera follows an AI player's unit
    // it gets, for that update, the direction from the unit to its lock target (unit +0x3130,
    // object id, set by message 0x42 in 0x00667DFB; none: its own yaw, level), eased in. The
    // camera turns its offset by D3DXMatrixRotationX(pitch) * RotationY(yaw), so yaw is
    // atan2(dx, dz) and a positive pitch looks down; the unit's position is its scene node's
    // (unit +8, +0x88..+0x90).
    constexpr size_t kUnitAimYaw = 0xD94;
    constexpr size_t kUnitAimPitch = 0xD9C;
    constexpr size_t kUnitLockTarget = 0x3130;
    constexpr size_t kUnitNode = 0x08;
    constexpr size_t kNodePosition = 0x88;
    constexpr float kPi = 3.14159265f;

    struct CameraAim
    {
        const uint8_t* unit;
        float yaw;
        float pitch;
    } g_cameraAim = {};

    float AngleDelta(float to, float from)
    {
        if (!std::isfinite(to) || !std::isfinite(from))
            return 0.0f;
        // Subtract in double to avoid overflow. Repeated float subtraction can
        // stop making progress entirely for large, but finite, input angles.
        float d = static_cast<float>(std::fmod(static_cast<double>(to) - from, 2.0 * kPi));
        if (d > kPi)
            d -= 2.0f * kPi;
        if (d < -kPi)
            d += 2.0f * kPi;
        return d;
    }

    const float* NodePosition(const uint8_t* object)
    {
        const uint8_t* node = *reinterpret_cast<uint8_t* const*>(object + kUnitNode);
        return node ? reinterpret_cast<const float*>(node + kNodePosition) : nullptr;
    }

    bool AiCameraAim(uint8_t* unit, float* yaw, float* pitch)
    {
        if (!unit || !g_botMode || CurrentPvpMode() != g_botMode)
            return false;
        bool bot = false;
        for (const Bot& b : g_botTable)
            bot = bot || b.unit == unit;
        if (!bot)
            return false;
        float wantYaw = FloatAt(unit, kUnitAimYaw);
        float wantPitch = 0.0f;
        uint32_t lockId = *reinterpret_cast<const uint32_t*>(unit + kUnitLockTarget);
        void* um = *g_game.unitManager;
        const uint8_t* other = lockId && um ? static_cast<const uint8_t*>(g_findObject(um, lockId)) : nullptr;
        const float* from = NodePosition(unit);
        const float* to = other ? NodePosition(other) : nullptr;
        if (from && to) {
            float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
            float flat = sqrtf(dx * dx + dz * dz);
            if (flat > 1.0f) {
                wantYaw = atan2f(dx, dz);
                wantPitch = atan2f(-dy, flat);
            }
        }
        if (wantPitch < -0.87f)
            wantPitch = -0.87f;
        if (wantPitch > 1.04f)
            wantPitch = 1.04f;
        if (g_cameraAim.unit != unit) {
            g_cameraAim = {unit, wantYaw, wantPitch};
        } else {
            g_cameraAim.yaw += AngleDelta(wantYaw, g_cameraAim.yaw) * 0.15f;
            g_cameraAim.pitch += (wantPitch - g_cameraAim.pitch) * 0.15f;
        }
        *yaw = g_cameraAim.yaw;
        *pitch = g_cameraAim.pitch;
        return true;
    }

    bool GuardedAiCameraAim(uint8_t* unit, float* yaw, float* pitch)
    {
        __try {
            return AiCameraAim(unit, yaw, pitch);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void __fastcall TrackingUpdate(uint8_t* camera, void* /*edx*/)
    {
        uint8_t* target = camera ? *reinterpret_cast<uint8_t**>(camera + kCameraTarget) : nullptr;
        float yaw = 0.0f, pitch = 0.0f;
        if (!GuardedAiCameraAim(target, &yaw, &pitch)) {
            g_cameraAim.unit = nullptr;
            g_originalTrackingUpdate(camera);
            return;
        }
        auto aimYaw = reinterpret_cast<float*>(target + kUnitAimYaw);
        auto aimPitch = reinterpret_cast<float*>(target + kUnitAimPitch);
        float ownYaw = *aimYaw, ownPitch = *aimPitch;
        *aimYaw = yaw;
        *aimPitch = pitch;
        __try {
            g_originalTrackingUpdate(camera);
        } __finally {
            *aimYaw = ownYaw;
            *aimPitch = ownPitch;
        }
    }

    // ---- death match: an AI player's life costs what the server counts ---------------------
    //
    // On a death in death match (mode IsA(2) or IsA(14), not tag match) PT_NTF_DEAD_UNIT
    // takes the unit's life cost from its unit data (unit +0x9C4, word +0x137) off the dead
    // player's slot +0x48 (0x004D929C, through the mode's vt +0x24). For an AI player that is
    // the NPC unit's own cost, while the server counts the player cost of its grade (pvp_ai
    // LifeCostGrade), so the HUD's team bar ran out while the AI player still had a life. Its
    // unit record in the battle start packet carries the server's cost (+8); that one is used.
    int g_deathCostLogs = 0;

    int AiDeathCost(uint8_t* unit, int cost)
    {
        if (!unit || !g_botMode || CurrentPvpMode() != g_botMode)
            return cost;
        for (const Bot& bot : g_botTable) {
            if (bot.unit != unit)
                continue;
            const StartDeck* deck = FindStartDeck(bot.pilotId);
            int form = bot.form >= 0 && bot.form < 4 ? bot.form : 0;
            int serverCost = deck ? deck->lifeCost[form] : 0;
            if (g_deathCostLogs < 10) {
                ++g_deathCostLogs;
                g_api->Log(g_mod, SDGO_LOG_INFO, "AI player %08lX is down: life cost %d (unit data %d)",
                           static_cast<unsigned long>(bot.pilotId), serverCost > 0 ? serverCost : cost, cost);
            }
            return serverCost > 0 ? serverCost : cost;
        }
        return cost;
    }

    int g_deckFaults = 0;

    void GuardedSwitchDecks()
    {
        __try {
            SwitchPendingDecks();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (g_deckFaults++ < 5)
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX switching an AI player's deck", GetExceptionCode());
        }
    }

    void GuardedMissionRespawnComplete(
        const SdgoMissionClientLifecycleV2* mission, void* unitManager,
        uint32_t objectId)
    {
        __try {
            mission->respawn_complete(unitManager, objectId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    int __fastcall PlaybackUnit(void* self, void* /*edx*/, uint8_t* packet)
    {
        int result = g_originalPlaybackUnit(self, packet);
        GuardedSwitchDecks();
        if (!g_missionRespawnPending.empty()) {
            const MissionRespawnPending pending = g_missionRespawnPending.front();
            g_missionRespawnPending.erase(g_missionRespawnPending.begin());
            if (const auto* mission = MissionClientLifecycleV2()) {
                g_missionPlaybackContext = {};
                g_missionPlaybackContext.size = sizeof(g_missionPlaybackContext);
                g_missionPlaybackContext.version = SDGO_ACE_AI_OBSERVER_VERSION;
                g_missionPlaybackContext.object_id = pending.objectId;
                g_missionPlaybackContext.colour = pending.colour >= 0 ?
                    static_cast<uint32_t>(pending.colour) : 0xffffffffu;
                g_missionPlaybackContext.native_result =
                    static_cast<uint32_t>(result);
                g_missionPlaybackContext.active = 1;
                GuardedMissionRespawnComplete(
                    mission, pending.unitManager, pending.objectId);
                g_missionPlaybackContext.active = 0;
            }
        }
        return result;
    }

    void __fastcall SetSlotColour(void* um, void* /*edx*/, int index, int colour)
    {
        if (index >= 1000) {
            const uint32_t objectId = static_cast<uint32_t>(index) + 1;
            const int missionSlot = g_missionLifecycle ?
                g_missionLifecycle->slot_for_object(um, objectId) : -1;
            if (missionSlot >= 0) {
                if (MissionClientLifecycleV2())
                    g_missionRespawnPending.push_back({um, objectId, colour});
                g_originalSetSlotColour(um, missionSlot, colour);
                return;
            }
            if (const Bot* found = BotByObject(um, static_cast<uint32_t>(index) + 1)) {
                Bot* bot = const_cast<Bot*>(found);
                g_api->Log(g_mod, SDGO_LOG_INFO, "AI player %08lX respawns: object %lu %s, slot %d, deck index %d",
                           static_cast<unsigned long>(bot->pilotId), static_cast<unsigned long>(bot->objectId),
                           g_findObject(um, bot->objectId) ? "present" : "MISSING", bot->slot, colour);
                if (bot->units > 1 && colour >= 0 && colour < bot->units && colour != bot->form && bot->deckNpcData[colour]) {
                    // the NPC of the new deck entry, before message 7 re-inits the AI from it
                    *reinterpret_cast<const uint8_t**>(bot->unit + kUnitNpcData) = bot->deckNpcData[colour];
                    *reinterpret_cast<uint32_t*>(bot->unit + kUnitNpcDataId) = bot->deckNpc[colour];
                    bot->pendingForm = colour;
                }
                // PlaybackUnit completes the native respawn (and any pending deck switch).
                // Reapply only after that point, with a fresh generation even when the same
                // deck entry respawns.
                bot->pendingInitialize = true;
                g_originalSetSlotColour(um, bot->slot, colour);
                return;
            }
            if (OutsideAiModes()) {
                LogStraySlot(index, _ReturnAddress());
                return;
            }
        }
        g_originalSetSlotColour(um, index, colour);
    }

    // ---- diagnostics: Lua errors of unit AI and the server's packets ----------------------

    int __cdecl LuaOnError(lua_State* L)
    {
        const LONG count = InterlockedIncrement(&g_luaErrors);
        if (g_logDiagnostics && count <= 40) {
            const char* message = g_luaToString ? g_luaToString(L, -1) : nullptr;
            g_api->Log(g_mod, SDGO_LOG_WARN, "Lua error: %s", message ? message : "(not a string)");
        }
        return g_originalLuaOnError(L);
    }

    int g_opcodeRepeat = 0;
    int g_packetLines = 0;

    // The game's own log (disabled in this client), copied into sdgo.log from the spawn of
    // AI players on: network and battle state messages.
    int g_gameLogLines = 0;

    void __cdecl GameLog(void* logger, int level, const wchar_t* format, ...)
    {
        // the game's logger writes only while enabled (+4) and level <= its level (+8)
        auto fields = reinterpret_cast<const int*>(logger);
        bool enabled = fields && fields[1] != 0 && level <= fields[2];
        bool copy = g_logPackets && g_gameLogLines < 6000;
        if (!format || (!enabled && !copy))
            return;
        wchar_t text[1024];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(text, _TRUNCATE, format, args);
        va_end(args);
        if (copy) {
            char utf8[2048];
            if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0) {
                ++g_gameLogLines;
                g_api->Log(g_mod, SDGO_LOG_INFO, "game[%d]: %s", level, utf8);
            }
        }
        if (enabled)
            g_originalGameLog(logger, level, L"%s", text);
    }

    int __fastcall PacketDispatch(void* self, void* /*edx*/, int a, int b, int c, int opcode, void* packet)
    {
        int op = opcode & 0xFFFF;
        // 004DC16C dispatches 0570 to the original 004D8AC0 death consumer.
        if(op==0x0570)NotifyPacket(static_cast<uint16_t>(op),static_cast<const uint8_t*>(packet));
        if (g_logPackets && g_packetLines < 3000) {
            if (op == g_lastOpcode) {
                ++g_opcodeRepeat;
            } else {
                if (g_opcodeRepeat > 0)
                    g_api->Log(g_mod, SDGO_LOG_INFO, "  (opcode %04X repeated %d more)", g_lastOpcode, g_opcodeRepeat);
                g_api->Log(g_mod, SDGO_LOG_INFO, "packet %04X", op);
                g_lastOpcode = op;
                g_opcodeRepeat = 0;
                ++g_packetLines;
            }
        }
        return g_originalPacketDispatch(self, a, b, c, opcode, packet);
    }

    // ---- diagnostics: every error the client names ------------------------------------
    //
    // The server error popup (0x006FD480) and the client error popups name the error code
    // through ErrorName (0x006DFC60); logging the code with the callers shows where an error
    // comes from.

    int g_errorNames = 0;

    const char* __cdecl ErrorName(int code)
    {
        const char* name = g_originalErrorName(code);
        if (code == 0 || g_errorNames >= 30)
            return name; // ERR_NULL: named for every server message, not an error
        ++g_errorNames;
        void* frames[4] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
        g_api->Log(g_mod, SDGO_LOG_INFO, "error %d (%s), called from %p %p %p %p", code, name ? name : "?", n > 0 ? frames[0] : nullptr,
                   n > 1 ? frames[1] : nullptr, n > 2 ? frames[2] : nullptr, n > 3 ? frames[3] : nullptr);
        return name;
    }

    // ---- diagnostics: access violations ------------------------------------------------
    //
    // The game's crash log (zoa_log\Exp_*.txt) has no faulting address and often no call
    // stack. This logs the first access violations with the address, the registers and the
    // stack words that point into GOnline.exe (return address candidates).

    int g_faults = 0;
    uintptr_t g_imageBegin = 0;
    uintptr_t g_imageEnd = 0;

    LONG CALLBACK FaultLogger(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_faults >= 8)
            return EXCEPTION_CONTINUE_SEARCH;
        ++g_faults;
        const CONTEXT* c = info->ContextRecord;
        const ULONG_PTR* params = info->ExceptionRecord->ExceptionInformation;
        g_api->Log(g_mod, SDGO_LOG_WARN,
                   "access violation at %p (%s %08lX): eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX "
                   "ebp %08lX esp %08lX",
                   info->ExceptionRecord->ExceptionAddress, params[0] ? "write" : "read",
                   static_cast<unsigned long>(params[1]), c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        auto tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
        auto top = reinterpret_cast<uintptr_t>(tib->StackBase);
        char line[512];
        int length = 0;
        int found = 0;
        for (uintptr_t at = c->Esp; at + 4 <= top && at < c->Esp + 0x800 && found < 16; at += 4) {
            uintptr_t value = *reinterpret_cast<const uintptr_t*>(at);
            if (value > g_imageBegin && value < g_imageEnd) {
                length += sprintf_s(line + length, sizeof(line) - length, " %08lX", static_cast<unsigned long>(value));
                ++found;
            }
        }
        g_api->Log(g_mod, SDGO_LOG_WARN, "  stack code addresses:%s", found ? line : " none");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void InstallFaultLogger()
    {
        auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        g_imageBegin = reinterpret_cast<uintptr_t>(base) + 0x1000;
        g_imageEnd = reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
        if (!AddVectoredExceptionHandler(1, &FaultLogger))
            g_api->Log(g_mod, SDGO_LOG_WARN, "access violation logging not available");
    }

    // ---- mode 7: an AI player keeps its own unit -------------------------------------------
    //
    // CUnit::InitUnitData (0x0067F170) in a network battle of mode 7 takes every unit from a
    // human player's deck: human record room +0x1C [n], n = the players UM has added so far.
    // For an AI player's unit n is past the humans and the copy read garbage (the spawn failed,
    // no AI player showed). NPC units keep the unit they are built with, so for an AI player's
    // unit (while it is created, or one of this battle's later) the branch is skipped.

    int __stdcall KeepOwnUnit(uint8_t* unit)
    {
        if (g_creatingAiUnit)
            return 1;
        if (!unit || !g_botMode || CurrentPvpMode() != g_botMode)
            return 0;
        for (const Bot& bot : g_botTable)
            if (bot.unit == unit)
                return 1;
        return 0;
    }

    // ---- dead AI players stay for their respawn ------------------------------------------
    //
    // UnitManager::Update (0x0049F6A0) handles an object that is gone in two steps, each
    // decided by the current mode's IsA(9) (the AI battle mode): it removes an NPC from the
    // NPC list (+0x70) and moves it into the NPC reuse pool (mapPool +0x1EA0), which then owns
    // it. Only the AI battle modes keep their dead NPCs, for the respawn. A PvP mode pooled the
    // dead AI player; its respawn (CUnit::Add) brought the pooled object back while the pool
    // still owned it, and the teardown failed on it (heap corruption freeing the unit). For an
    // AI player of this battle both steps now get the AI battle mode's answer.

    int g_keptLogs = 0;

    int __stdcall KeepDeadAiPlayer(uint8_t* node, int step)
    {
        __try {
            uint8_t* object = *reinterpret_cast<uint8_t**>(node + 0x10);
            if (object && g_missionLifecycle && g_missionLifecycle->keep_dead(object)) return 1;
            if (!object || !g_botMode || CurrentPvpMode() != g_botMode)
                return 0;
            for (const Bot& bot : g_botTable) {
                if (bot.unit != object)
                    continue;
                if (g_keptLogs < 20) {
                    ++g_keptLogs;
                    g_api->Log(g_mod, SDGO_LOG_INFO, "AI player %08lX (object %lu) is down: kept for its respawn (%s)",
                               static_cast<unsigned long>(bot.pilotId), static_cast<unsigned long>(bot.objectId),
                               step == 0 ? "NPC list" : "not pooled");
                }
                return 1;
            }
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
}

#include "observer.inl"

extern "C" __declspec(dllexport) uint32_t __cdecl SdgoMatchAiGetLifeCostV1(uint32_t pilot) {
    for(const auto& bot:g_botTable)if(bot.pilotId==pilot) {
        const auto* deck=FindStartDeck(pilot);
        const int form=bot.form>=0&&bot.form<4?bot.form:0;
        return deck&&deck->lifeCost[form]>0?deck->lifeCost[form]:0;
    }
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl
SdgoMatchAiMissionTickV1(uint8_t* mode)
{
    if (!mode || !g_api)
        return 0;
    __try {
        NotifyTick(mode,static_cast<uint32_t>(InterlockedCompareExchange(&g_receivedBattleStartCount,0,0)));
        DispatchMissionReady(mode, false);
        InterlockedIncrement(
            &g_missionForcedPadTicks);
        UpdateMissionForcedPads(mode);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_missionPadBots.clear();
        g_missionPadMode = nullptr;
        if (g_forcedPadFaults++ < 5)
            g_api->Log(
                g_mod, SDGO_LOG_ERROR,
                "exception %08lX in the mission forced-pad bridge",
                GetExceptionCode());
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoMissionAiClientGetForcedPadStatusV1(
    SdgoMissionAiClientForcedPadStatusV1* status)
{
    __try {
        if (!status || status->size < sizeof(*status) ||
            status->version !=
                SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_VERSION) {
            return 0;
        }
        SdgoMissionAiClientForcedPadStatusV1 snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_VERSION;
        snapshot.mode_object = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(
                g_missionPadMode));
        snapshot.bridge_available =
            g_forcedPadBridgeAvailable ? 1u : 0u;
        snapshot.tick_count = static_cast<uint32_t>(
            InterlockedCompareExchange(
                &g_missionForcedPadTicks, 0, 0));
        const size_t count = (std::min)(
            g_missionPadBots.size(),
            static_cast<size_t>(12));
        snapshot.bot_count =
            static_cast<uint32_t>(count);
        for (size_t index = 0; index < count;
             ++index) {
            const Bot& bot =
                g_missionPadBots[index];
            const ForcedPadState& state =
                bot.forcedPad;
            auto& copied = snapshot.bots[index];
            copied.pilot_id = bot.pilotId;
            copied.object_id = bot.objectId;
            copied.generation = bot.generation;
            copied.request_count =
                state.requestCount;
            copied.notice_count =
                state.noticeCount;
            copied.displacement_count =
                state.displacementCount;
            copied.timeout_count =
                state.timeoutCount;
            copied.send_failure_count =
                state.sendFailureCount;
            copied.ownership_loss_count =
                state.ownershipLossCount;
            copied.outstanding =
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &state.outstanding),
                    0, 0) != 0
                    ? 1u
                    : 0u;
            copied.outstanding_generation =
                state.outstandingGeneration;
            copied.last_result = state.lastResult;
            copied.last_clear_reason =
                state.lastClearReason;
            copied.activation_position_valid =
                state.activationPositionValid ? 1u
                                              : 0u;
            copied.activation_position_x =
                state.activationPosition.x;
            copied.activation_position_y =
                state.activationPosition.y;
            copied.activation_position_z =
                state.activationPosition.z;
        }
        *status = snapshot;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoPvpAiClientGetForcedPadStatusV1(
    SdgoPvpAiClientForcedPadStatusV1* status)
{
    __try {
        if (!status || status->size < sizeof(*status) ||
            status->version !=
                SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_VERSION) {
            return 0;
        }
        SdgoPvpAiClientForcedPadStatusV1 snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_VERSION;
        snapshot.battle_start_sequence =
            static_cast<uint32_t>(InterlockedCompareExchange(
                &g_receivedBattleStartCount, 0, 0));
        snapshot.bridge_available =
            g_forcedPadBridgeAvailable ? 1u : 0u;
        const size_t count =
            (std::min)(g_botTable.size(), static_cast<size_t>(12));
        snapshot.bot_count = static_cast<uint32_t>(count);
        for (size_t index = 0; index < count; ++index) {
            const Bot& bot = g_botTable[index];
            const ForcedPadState& state = bot.forcedPad;
            auto& copied = snapshot.bots[index];
            copied.pilot_id = bot.pilotId;
            copied.object_id = bot.objectId;
            copied.generation = bot.generation;
            copied.request_count = state.requestCount;
            copied.notice_count = state.noticeCount;
            copied.displacement_count = state.displacementCount;
            copied.timeout_count = state.timeoutCount;
            copied.send_failure_count = state.sendFailureCount;
            copied.ownership_loss_count = state.ownershipLossCount;
            copied.outstanding =
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.outstanding), 0, 0) != 0
                    ? 1u
                    : 0u;
            copied.outstanding_generation =
                state.outstandingGeneration;
            copied.last_result = state.lastResult;
            copied.last_clear_reason = state.lastClearReason;
            copied.activation_position_valid =
                state.activationPositionValid ? 1u : 0u;
            copied.activation_position_x = state.activationPosition.x;
            copied.activation_position_y = state.activationPosition.y;
            copied.activation_position_z = state.activationPosition.z;
        }
        *status = snapshot;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoMissionAiClientGetForcedPadStatusV2(
    SdgoMissionAiClientForcedPadStatusV2* status)
{
    __try {
        if (!status || status->size < sizeof(*status) ||
            status->version !=
                SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION) {
            return 0;
        }
        SdgoMissionAiClientForcedPadStatusV2 snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION;
        snapshot.mode_object = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(g_missionPadMode));
        snapshot.bridge_available =
            g_forcedPadBridgeAvailable ? 1u : 0u;
        snapshot.tick_count = GetTickCount();
        const size_t count = (std::min)(
            g_missionPadBots.size(),
            static_cast<size_t>(12));
        snapshot.bot_count = static_cast<uint32_t>(count);
        for (size_t index = 0; index < count; ++index) {
            const Bot& bot = g_missionPadBots[index];
            const ForcedPadState& state = bot.forcedPad;
            auto& copied = snapshot.bots[index];
            copied.pilot_id = bot.pilotId;
            copied.object_id = bot.objectId;
            copied.generation = bot.generation;
            copied.notice_count = state.noticeCount;
            copied.outstanding =
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.outstanding), 0, 0) != 0
                    ? 1u
                    : 0u;
            copied.outstanding_generation =
                state.outstandingGeneration;
            copied.last_result = state.lastResult;
            copied.last_clear_reason = state.lastClearReason;

            battle_ai::native::ForceMoveRequest085A request{};
            uint32_t requestCount = 0;
            uint32_t requestSequence = 0;
            uint32_t padObjectId = 0;
            DWORD requestTick = 0;
            bool stable = false;
            for (int retry = 0; retry < 8; ++retry) {
                const LONG before = InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.requestVersion), 0, 0);
                if ((before & 1) != 0)
                    continue;
                request = state.request;
                requestCount = state.requestCount;
                requestSequence = state.requestSequence;
                padObjectId = state.padObjectId;
                requestTick = state.requestTick;
                MemoryBarrier();
                const LONG after = InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.requestVersion), 0, 0);
                if (before == after && (after & 1) == 0) {
                    stable = true;
                    break;
                }
            }
            copied.request_count = requestCount;
            copied.request_tick = requestTick;
            const bool payloadValid =
                stable &&
                state.generation == bot.generation &&
                requestSequence != 0 &&
                requestSequence == requestCount &&
                request.opcode ==
                    battle_ai::native::kForceMoveRequestOpcode &&
                request.routeId == 0 &&
                request.pilotId == bot.pilotId;
            copied.payload_valid = payloadValid ? 1u : 0u;
            if (payloadValid) {
                copied.pad_object_valid =
                    padObjectId != 0 ? 1u : 0u;
                copied.pad_object_id = padObjectId;
                copied.request_pilot_id = request.pilotId;
                copied.speed = request.speed;
                copied.duration_updates =
                    request.durationUpdates;
                copied.direction_x = request.direction.x;
                copied.direction_y = request.direction.y;
                copied.direction_z = request.direction.z;
            }
        }
        *status = snapshot;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoPvpAiClientGetForcedPadStatusV2(
    SdgoPvpAiClientForcedPadStatusV2* status)
{
    __try {
        if (!status || status->size < sizeof(*status) ||
            status->version !=
                SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION) {
            return 0;
        }
        SdgoPvpAiClientForcedPadStatusV2 snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION;
        snapshot.battle_start_sequence =
            static_cast<uint32_t>(InterlockedCompareExchange(
                &g_receivedBattleStartCount, 0, 0));
        snapshot.bridge_available =
            g_forcedPadBridgeAvailable ? 1u : 0u;
        snapshot.tick_count = GetTickCount();
        const size_t count =
            (std::min)(g_botTable.size(), static_cast<size_t>(12));
        snapshot.bot_count = static_cast<uint32_t>(count);
        for (size_t index = 0; index < count; ++index) {
            const Bot& bot = g_botTable[index];
            const ForcedPadState& state = bot.forcedPad;
            auto& copied = snapshot.bots[index];
            copied.pilot_id = bot.pilotId;
            copied.object_id = bot.objectId;
            copied.generation = bot.generation;
            copied.notice_count = state.noticeCount;
            copied.outstanding =
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.outstanding), 0, 0) != 0
                    ? 1u
                    : 0u;
            copied.outstanding_generation =
                state.outstandingGeneration;
            copied.last_result = state.lastResult;
            copied.last_clear_reason = state.lastClearReason;

            battle_ai::native::ForceMoveRequest085A request{};
            uint32_t requestCount = 0;
            uint32_t requestSequence = 0;
            uint32_t padObjectId = 0;
            DWORD requestTick = 0;
            bool stable = false;
            for (int retry = 0; retry < 8; ++retry) {
                const LONG before = InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.requestVersion), 0, 0);
                if ((before & 1) != 0)
                    continue;
                request = state.request;
                requestCount = state.requestCount;
                requestSequence = state.requestSequence;
                padObjectId = state.padObjectId;
                requestTick = state.requestTick;
                MemoryBarrier();
                const LONG after = InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&state.requestVersion), 0, 0);
                if (before == after && (after & 1) == 0) {
                    stable = true;
                    break;
                }
            }
            copied.request_count = requestCount;
            copied.request_tick = requestTick;
            const bool payloadValid =
                stable &&
                state.generation == bot.generation &&
                requestSequence != 0 &&
                requestSequence == requestCount &&
                request.opcode ==
                    battle_ai::native::kForceMoveRequestOpcode &&
                request.routeId == 0 &&
                request.pilotId == bot.pilotId;
            copied.payload_valid = payloadValid ? 1u : 0u;
            if (payloadValid) {
                copied.pad_object_valid =
                    padObjectId != 0 ? 1u : 0u;
                copied.pad_object_id = padObjectId;
                copied.request_pilot_id = request.pilotId;
                copied.speed = request.speed;
                copied.duration_updates =
                    request.durationUpdates;
                copied.direction_x = request.direction.x;
                copied.direction_y = request.direction.y;
                copied.direction_z = request.direction.z;
            }
        }
        *status = snapshot;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) uint32_t __cdecl SdgoPvpAiClientGetForcedPadTickCountV1()
{
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_forcedPadTicks,0,0));
}

extern "C" __declspec(dllexport) int __cdecl SdgoPvpAiClientGetGateStatusV1(SdgoPvpAiClientGateStatusV1* output)
{
    __try {
        if(!output||output->size<sizeof(*output)||output->version!=1)return 0;
        const uint32_t sequence=static_cast<uint32_t>(InterlockedCompareExchange(&g_receivedBattleStartCount,0,0));
        SdgoPvpAiClientGateStatusV1 snapshot{};
        if(g_gateStatus.battle_start_sequence==sequence)snapshot=g_gateStatus;
        snapshot.size=sizeof(snapshot);snapshot.version=1;snapshot.battle_start_sequence=sequence;
        *output=snapshot;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}

// The two decisions of UnitManager::Update: al holds mode->IsA(9), ebx the object's node. The
// detours set al for an AI player of this battle, then resume with the original test/jne.
static void* g_deadListResume = nullptr;
static void* g_deadPoolResume = nullptr;

static int __stdcall KeepDeadInList(uint8_t* node)
{
    return KeepDeadAiPlayer(node, 0);
}

static int __stdcall KeepDeadOutOfPool(uint8_t* node)
{
    return KeepDeadAiPlayer(node, 1);
}

// CUnit::InitUnitData's mode 7 check (esi = the unit): an AI player's unit goes straight to
// the end of the player-deck branch.
static void* g_deckCheckResume = nullptr;
static void* g_deckSkip = nullptr;

// PT_NTF_DEAD_UNIT's death match branch, after it loaded the life cost into ecx (esi = the
// dead unit): an AI player's cost is replaced in the saved ecx (pushad frame +0x18).
static void* g_deathCostResume = nullptr;

static int __stdcall AiDeathCostThunk(uint8_t* unit, int cost)
{
    return AiDeathCost(unit, cost);
}

static __declspec(naked) void DeathCostDetour()
{
    __asm {
        pushad
        push ecx
        push esi
        call AiDeathCostThunk
        mov [esp + 0x18], eax
        popad
        jmp dword ptr [g_deathCostResume]
    }
}

// The dead view, right after it copied UM's player list into its list at ebp-0x20.
static void* g_deadViewResume = nullptr;

static void __stdcall AddAiToDeadViewThunk(void* list)
{
    AddAiToDeadView(static_cast<UnitList*>(list));
}

static __declspec(naked) void DeadViewDetour()
{
    __asm {
        pushad
        lea eax, [ebp - 0x20]
        push eax
        call AddAiToDeadViewThunk
        popad
        jmp dword ptr [g_deadViewResume]
    }
}

static int __stdcall KeepOwnUnitThunk(uint8_t* unit)
{
    return KeepOwnUnit(unit);
}

static __declspec(naked) void DeckCheckDetour()
{
    __asm {
        pushad
        push esi
        call KeepOwnUnitThunk
        test eax, eax
        popad
        jnz skip_deck
        jmp dword ptr [g_deckCheckResume]
    skip_deck:
        jmp dword ptr [g_deckSkip]
    }
}

// CUnit::SwitchDeck's skill setup (esi = the unit) reads the owner's human record at index
// object id - 1; an AI player's unit (object 1001 and up) has none and the record read past
// the humans crashed the game at its first deck switch (CSkill, 0x00669189). Its unit goes
// to the end of the block, as when the skill system is off.
static void* g_switchSkillResume = nullptr;
static void* g_switchSkillSkip = nullptr;

static __declspec(naked) void SwitchSkillDetour()
{
    __asm {
        pushad
        push esi
        call KeepOwnUnitThunk
        test eax, eax
        popad
        jnz skip_skills
        jmp dword ptr [g_switchSkillResume]
    skip_skills:
        jmp dword ptr [g_switchSkillSkip]
    }
}

static __declspec(naked) void DeadNpcListDetour()
{
    __asm {
        pushad
        push ebx
        call KeepDeadInList
        test eax, eax
        jz keep_answer
        mov byte ptr [esp + 28], 1
    keep_answer:
        popad
        jmp dword ptr [g_deadListResume]
    }
}

static __declspec(naked) void DeadNpcPoolDetour()
{
    __asm {
        pushad
        push ebx
        call KeepDeadOutOfPool
        test eax, eax
        jz keep_answer
        mov byte ptr [esp + 28], 1
    keep_answer:
        popad
        jmp dword ptr [g_deadPoolResume]
    }
}

SDGO_MOD_ENTRY
{
    if (!api || !mod ||
        api->version != SDGO_API_VERSION ||
        api->size <
            offsetof(SdgoApi, HookSite) +
                sizeof(api->HookSite) ||
        !api->Symbol || !api->HookCode ||
        !api->HookSite || !api->Log)
        return 0;
    g_api = api;
    g_mod = mod;
    bool bound = Bind(g_game.unitManager, "game.unit_manager") && Bind(g_game.isNetwork, "game.is_network") &&
                 Bind(g_game.findStartPoint, "um.find_start_point") && Bind(g_game.isObserver, "um.is_observer") &&
                 Bind(g_game.baseCreateNpc, "mode.base_create_npc") && Bind(g_game.setPlayerInfo, "um.set_player_info") &&
                 Bind(g_game.playerSlot, "um.player_slot") && Bind(g_game.assign, "wstring.assign") &&
                 Bind(g_game.addAiUnit, "um.add_ai_unit") && Bind(g_game.addObject, "um.add_object") &&
                 Bind(g_game.calculateTeam, "um.calculate_team") && Bind(g_game.enableAi, "npc.enable_ai") &&
                 Bind(g_game.disableAi, "npc.disable_ai") && Bind(g_game.releaseAttack, "npc.freeze_release_attack") &&
                 Bind(g_game.moveStop, "npc.move_stop") && Bind(g_game.resetInputs, "npc.freeze_reset_inputs") &&
                 Bind(g_registerLuaBindings, "lua.register_bindings") && Bind(g_gameApp, "game.app") &&
                 Bind(g_lockTarget, "ainormal.lock_target") && Bind(g_runLuaFunction, "ainormal.run_lua_function") &&
                 Bind(g_targetParticipants, "ainormal.target_participants") &&
                 Bind(g_findObject, "um.find_object") && Bind(g_setPlayerCount, "um.set_player_count") &&
                 Bind(g_gridData, "grid.data") && Bind(g_gridFind, "grid.find") &&
                 Bind(g_addDeckUnit, "unit.add_deck_unit") && Bind(g_switchDeck, "unit.switch_deck");
    if (!bound)
        return 0;
    if(!Bind(g_unitCombatEvent,"match_ai.unit_combat_event")||
       !api->HookCode(mod,"match_ai.unit_network_message",reinterpret_cast<void*>(&UnitNetworkMessage),
                      reinterpret_cast<void**>(&g_originalUnitNetworkMessage))||
       !api->HookCode(mod,"match_ai.transform_notice",reinterpret_cast<void*>(&TransformNotice),
                      reinterpret_cast<void**>(&g_originalTransformNotice)))return 0;
    if (!Bind(g_forcedPadApi.query, "match_ai.pad.query") ||
        !Bind(g_forcedPadApi.overlap, "match_ai.pad.overlap") ||
        !Bind(g_forceMoveSender, "match_ai.pad.sender") ||
        !Bind(g_sendForceMovePacket, "match_ai.pad.send") ||
        !Bind(g_movementState, "match_ai.pad.movement_state") ||
        !Bind(g_attackState, "match_ai.pad.attack_state") ||
        !api->HookCode(
            mod, "match_ai.pad.notice",
            reinterpret_cast<void*>(&ForceMoveNotice),
            reinterpret_cast<void**>(&g_originalForceMoveNotice))) {
        return 0;
    }
    g_forcedPadApi.ownedLocalAi = &ForcedPadOwnedLocalAi;
    g_forcedPadApi.actionAllowed = &ForcedPadActionAllowed;
    g_forcedPadApi.controlAllowed = &ForcedPadControlAllowed;
    g_forcedPadApi.forcedMoving = &ForcedPadMoving;
    g_forcedPadApi.authority = &ForcedPadAuthority;
    g_forcedPadApi.submit = &ForcedPadSubmit;
    g_forcedPadBridgeAvailable = true;
    if(!Bind(g_roomHost.roomInfo,"pvp_ai_client.room_info") ||
       !Bind(g_roomHost.hostGetter,"pvp_ai_client.host_id") ||
       !api->HookCode(mod,"pvp_ai_client.change_host",reinterpret_cast<void*>(&ChangeRoomHost),
                      reinterpret_cast<void**>(&g_changeRoomHost)))return 0;
    if (!Bind(g_rosterInit, "hud.battle_roster_init") ||
        !api->HookCode(mod, "hud.battle_roster_row", reinterpret_cast<void*>(&ScoreboardRow),
                       reinterpret_cast<void**>(&g_originalRosterRow)))
        return 0;
    if (api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->HookSite ||
        !api->HookSite(mod, "ainormal.target_team_gate", reinterpret_cast<void*>(&TargetTeamGate), &g_targetTeamOriginal))
        return 0;
    if (!api->HookSite(mod, "hud.scoreboard_npc_gate", reinterpret_cast<void*>(&ScoreboardNpcGate),
                       &g_scoreboardNpcGateOriginal))
        return 0;
    if (!api->HookSite(mod, "hud.scoreboard_life_npc_gate", reinterpret_cast<void*>(&ScoreboardLifeNpcGate),
                       &g_scoreboardLifeNpcOriginal) ||
        !api->HookSite(mod, "hud.scoreboard_life_cost", reinterpret_cast<void*>(&ScoreboardLifeCostGate),
                       &g_scoreboardLifeCostOriginal))
        return 0;
    if (!api->HookSite(mod, "game.teleport_relocation_npc_gate",
                       reinterpret_cast<void*>(&TeleportNpcGate),
                       &g_teleportNpcGateOriginal))
        return 0;
    if(!api->HookSite(mod,"match_ai.gate.countdown_authority",reinterpret_cast<void*>(&GateAuthoritySite),&g_gateAuthorityOriginal))
        return 0;
    // The profile pointer is only used by the optional read-only GameTest
    // observer. Older supported symbol maps must still load the production AI
    // feature; their snapshots simply report pilot id 0.
    Bind(g_profilePointer, "game.profile_pointer");
    // The AI players' scripts call these; without them they would jump to address 0.
    if (!api->HookCode(mod, "lua.search_lock_player_a", reinterpret_cast<void*>(&LockPlayerA),
                       reinterpret_cast<void**>(&g_originalLockPlayerA)) ||
        !api->HookCode(mod, "lua.search_lock_player", reinterpret_cast<void*>(&LockPlayer),
                       reinterpret_cast<void**>(&g_originalLockPlayer)) ||
        !api->HookCode(mod, "lua.search_lock_player_aibs", reinterpret_cast<void*>(&LockPlayerAibs),
                       reinterpret_cast<void**>(&g_originalLockPlayerAibs)) ||
        !api->HookCode(mod, "lua.search_lock_target_select", reinterpret_cast<void*>(&LockTargetSelect),
                       reinterpret_cast<void**>(&g_originalLockTargetSelect)) ||
        !api->HookCode(mod, "lua.game_interface_run_lua_function", reinterpret_cast<void*>(&RunLuaFunction),
                       reinterpret_cast<void**>(&g_originalRunLuaFunction)) ||
        !api->HookCode(mod, "lua.our_team_target_search", reinterpret_cast<void*>(&TeamSearch),
                       reinterpret_cast<void**>(&g_originalTeamSearch)))
        return 0;
    // Without these an AI player that died does not come back, and SetSlotColour would write
    // out of bounds for it.
    if (!api->HookCode(mod, "um.object_id_from_pilot", reinterpret_cast<void*>(&ObjectIdFromPilot),
                       reinterpret_cast<void**>(&g_originalObjectIdFromPilot)) ||
        !api->HookCode(mod, "um.set_slot_colour", reinterpret_cast<void*>(&SetSlotColour),
                       reinterpret_cast<void**>(&g_originalSetSlotColour)) ||
        !api->HookCode(mod, "um.is_player_object", reinterpret_cast<void*>(&IsPlayerObject),
                       reinterpret_cast<void**>(&g_originalIsPlayerObject)) ||
        !api->HookCode(mod, "um.player_slot", reinterpret_cast<void*>(&PlayerSlot),
                       reinterpret_cast<void**>(&g_originalPlayerSlot)) ||
        !api->HookCode(mod, "um.find_player", reinterpret_cast<void*>(&FindPlayer),
                       reinterpret_cast<void**>(&g_originalFindPlayer)) ||
        !api->HookCode(mod, "um.find_object", reinterpret_cast<void*>(&FindObjectForSearch),
                       reinterpret_cast<void**>(&g_originalFindObject)))
        return 0;
    // Without this an AI player keeps its first unit for every life in modes 6 and 7.
    if (!api->HookCode(mod, "game.playback_unit", reinterpret_cast<void*>(&PlaybackUnit),
                       reinterpret_cast<void**>(&g_originalPlaybackUnit)))
        return 0;
    // Without this an AI player's unit is not created in mode 7.
    g_deckSkip = api->Symbol(mod, "unit.init_deck_skip");
    if (!g_deckSkip ||
        !api->HookCode(mod, "unit.init_deck_check", reinterpret_cast<void*>(&DeckCheckDetour), &g_deckCheckResume))
        return 0;
    // Without this the first deck switch of an AI player (modes 6, 7) crashes the game.
    g_switchSkillSkip = api->Symbol(mod, "unit.switch_deck_skill_skip");
    if (!g_switchSkillSkip ||
        !api->HookCode(mod, "unit.switch_deck_skill_check", reinterpret_cast<void*>(&SwitchSkillDetour),
                       &g_switchSkillResume))
        return 0;
    if (!api->HookCode(mod, "game.dead_unit_life_cost", reinterpret_cast<void*>(&DeathCostDetour), &g_deathCostResume))
        api->Log(mod, SDGO_LOG_WARN, "death match life cost not hooked; the HUD bar counts an AI player's NPC cost");
    if (!api->HookCode(mod, "camera.tracking_update", reinterpret_cast<void*>(&TrackingUpdate),
                       reinterpret_cast<void**>(&g_originalTrackingUpdate)))
        api->Log(mod, SDGO_LOG_WARN, "tracking camera not hooked; the dead view takes an AI player's aim");
    if (!Bind(g_gameNew, "crt.operator_new") ||
        !api->HookCode(mod, "mode.dead_view_players", reinterpret_cast<void*>(&DeadViewDetour), &g_deadViewResume))
        api->Log(mod, SDGO_LOG_WARN, "dead view not hooked; a down player cannot watch AI players");
    // Without these a dead AI player is pooled, and its respawn corrupts the heap.
    if (!api->HookCode(mod, "um.update_dead_npc_list", reinterpret_cast<void*>(&DeadNpcListDetour),
                       &g_deadListResume) ||
        !api->HookCode(mod, "um.update_dead_npc_pool", reinterpret_cast<void*>(&DeadNpcPoolDetour),
                       &g_deadPoolResume))
        return 0;
    if (!api->HookCode(mod, "game.nat_check", reinterpret_cast<void*>(&NatCheck),
                       reinterpret_cast<void**>(&g_originalNatCheck)))
        api->Log(mod, SDGO_LOG_WARN, "NAT check not hooked; a battle with AI players and one human shows a blocked connection");
    if (!api->HookCode(mod, "game.service_game_ready", reinterpret_cast<void*>(&GameReady),
                       reinterpret_cast<void**>(&g_originalGameReady)))
        return 0;
    if (!api->HookCode(mod, "waitroom.battle_udp_setup", reinterpret_cast<void*>(&UdpSetup),
                       reinterpret_cast<void**>(&g_originalUdpSetup)))
        return 0;
    g_logDiagnostics = api->SettingInt(mod, "LogDiagnostics", 0) != 0;
    if (g_logDiagnostics)
        Bind(g_luaToString, "lua.tostring");
    if (api->HookCode(mod, "lua_tinker.on_error", reinterpret_cast<void*>(&LuaOnError),
                      reinterpret_cast<void**>(&g_originalLuaOnError)))
        InterlockedExchange(&g_luaErrorObserverAvailable, 1);
    else
        api->Log(mod, SDGO_LOG_WARN, "Lua error counting not available");
    if (g_logDiagnostics) {
        InstallFaultLogger();
        if (!api->HookCode(mod, "game.log", reinterpret_cast<void*>(&GameLog), reinterpret_cast<void**>(&g_originalGameLog)))
            api->Log(mod, SDGO_LOG_WARN, "game log copy not available");
    }
    if (!api->HookCode(mod, "game.packet_dispatch", reinterpret_cast<void*>(&PacketDispatch),
                       reinterpret_cast<void**>(&g_originalPacketDispatch)))return 0;
    // Without this an AI player of modes 6 and 7 has one unit, and in mode 7 its team gauge
    // counts only that.
    if (!api->HookCode(mod, "service.dispatch_packet", reinterpret_cast<void*>(&ServicePacket),
                       reinterpret_cast<void**>(&g_originalServicePacket)))
        return 0;
    if (api->SettingInt(mod, "LogErrors", 0) != 0 &&
        !api->HookCode(mod, "game.error_name", reinterpret_cast<void*>(&ErrorName),
                       reinterpret_cast<void**>(&g_originalErrorName)))
        api->Log(mod, SDGO_LOG_WARN, "error logging not available");
    int hooked = 0;
    for (int i = 0; i < 5; ++i) {
        if (auto slot = reinterpret_cast<void**>(api->Symbol(mod, g_classes[i].slot)))
            g_vtables[i] = slot - kModeInitSlot;
        if (api->HookSlot(mod, g_classes[i].slot, reinterpret_cast<void*>(g_detours[i]),
                          reinterpret_cast<void**>(&g_classes[i].original))) {
            if(!api->HookSlot(mod,g_classes[i].updateSlot,reinterpret_cast<void*>(g_updateDetours[i]),
                              reinterpret_cast<void**>(&g_classes[i].updateOriginal)))return 0;
            ++hooked;
        }
        else
            api->Log(mod, SDGO_LOG_WARN, "%s mode not hooked; it gets no AI players", g_classes[i].name);
    }
    if (hooked == 0)
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: PvP battles spawn the AI players the server adds (room title !ai)");
    return 1;
}
