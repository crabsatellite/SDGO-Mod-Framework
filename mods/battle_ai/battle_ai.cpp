// Installs Lua decorators before native CNPC::Think. Native movement, weapons,
// damage and networking remain the executor. Never reads another process.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <intrin.h>
#include "sdgo/sdgo_api.h"
#include "combat_math.h"
#include "native_capabilities.h"
#include "native_navigation.h"
#include "native_map_gate.h"
#include "native_map_geometry.h"
#include "native_terrain_ray.h"
#include "native_terrain_support.h"
#include "native_transport_catalog.h"
#include "mission_wingman_scope.h"
#include "../match_ai/forced_pad_observer.h"
#include "map_path_search.h"
#include "difficulty.h"
#include "defence_math.h"
#include "native_defence.h"
#include "native_protection.h"
#include "native_loadout.h"
#include "sdgo/room_titles_current.h"
#include "sdgo/ai_catalog_provider.h"
#include "sdgo/battle_ai_observer.h"
#include "sdgo/mission_ai_client_observer.h"
#include "sdgo/mission_ai_forced_pad_observer.h"

namespace {
static_assert(sizeof(SdgoBattleAiUnitSnapshotV1)==208);
static_assert(sizeof(SdgoBattleAiNavigationSnapshotV1)==72);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,special_zero_completions)==164);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,ranged_attempts)==168);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,ranged_nonzero_returns)==176);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,mobile_primary_dispatches)==184);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,mobile_defensive_dispatches)==192);
static_assert(offsetof(SdgoBattleAiUnitSnapshotV1,backward_primary_dispatches)==200);
struct lua_State;
using LuaFn = int(__cdecl*)(lua_State*);
using Reader = const char*(__cdecl*)(lua_State*, void*, size_t*);
struct Lua {
    lua_State*(__cdecl* state)(lua_State*);
    int(__cdecl* gettop)(lua_State*);
    int(__cdecl* type)(lua_State*,int);
    double(__cdecl* tonumber)(lua_State*,int);
    void(__cdecl* settop)(lua_State*, int);
    const char*(__cdecl* tostring)(lua_State*, int);
    void(__cdecl* pushnumber)(lua_State*, double);
    void(__cdecl* pushstring)(lua_State*, const char*);
    void(__cdecl* pushcclosure)(lua_State*, LuaFn, int);
    void(__cdecl* gettable)(lua_State*, int);
    void(__cdecl* settable)(lua_State*, int);
    void(__cdecl* newtable)(lua_State*);
    int(__cdecl* pcall)(lua_State*, int, int, int);
    int(__cdecl* load)(lua_State*, Reader, void*, const char*);
} g_lua = {};
using ThinkFn = bool(__thiscall*)(uint8_t*);
using InitFn = void(__thiscall*)(uint8_t*, void*);
using PhaseGeometryFn = void(__thiscall*)(uint8_t*, uint32_t);
ThinkFn g_think = nullptr;
PhaseGeometryFn g_phaseHide = nullptr, g_phaseUnhide = nullptr;
std::atomic<uint64_t> g_phaseGeometryRevision{1};
std::atomic<uint64_t> g_phaseHideCount{0},g_phaseUnhideCount{0};
std::atomic<uint64_t> g_phaseEventSequence{0};
std::atomic<uint32_t> g_phaseLastObject{0},g_phaseLastAction{0};
using EffectActiveFn=bool(__thiscall*)(uint8_t*,uint8_t);
EffectActiveFn g_effectActive=nullptr;
void* g_aiBoostPauseReturn=nullptr;
InitFn g_init = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
constexpr int kGlobals = -10001;
thread_local uint8_t* g_current = nullptr;
std::unordered_map<uint8_t*, uint64_t> g_generations;
uint64_t g_serial = 0;
lua_State* g_loaded = nullptr;
std::wstring g_script, g_config;
std::wstring g_universalScript;
ULONGLONG g_checkAt = 0;
FILETIME g_scriptTime = {}, g_configTime = {};
FILETIME g_universalTime = {};
bool g_loading = false;
bool g_preparationFault = false;
bool g_policyReady = false;

using battle_ai::Vec3;
using battle_ai::native::Ray3;
using DispatchFn=int(__cdecl*)(const char*,uint8_t*);
using RayFn=Ray3*(__thiscall*)(uint8_t*,Ray3*);
using FindObjectFn=uint8_t*(__thiscall*)(uint8_t*,uint32_t);
using GoalFn=int(__thiscall*)(uint8_t*,const Vec3*,int);
using IntentFn=void(__thiscall*)(uint8_t*);
using SetIntentFn=int(__thiscall*)(uint8_t*,char);
using ShotFn=uint32_t(__thiscall*)(uint8_t*,uint32_t,char,char);
using ProjectileFn=bool(__thiscall*)(uint8_t*,void*);
using StateEventFn=void(__thiscall*)(uint8_t*,void*);
using RotateFn=void(__thiscall*)(uint8_t*,float,int);
using MessageFn=bool(__thiscall*)(uint8_t*,uint32_t,void*,uint32_t);
using RouteFn=Vec3*(__thiscall*)(uint8_t*,uint32_t,Vec3,Vec3,Vec3*,uint32_t*);
using NavigationNodeFn=uint8_t*(__thiscall*)(uint8_t*,Vec3);
using NavigationNodeAtFn=uint8_t*(__thiscall*)(uint8_t*,int);
using StartPointFn=uint8_t*(__thiscall*)(uint8_t*,int,int);
using DeleteFn=void(__cdecl*)(void*);
DispatchFn g_dispatch=nullptr;
RayFn g_ray=nullptr;
FindObjectFn g_findObject=nullptr;
NavigationNodeFn g_navigationNode=nullptr;
NavigationNodeAtFn g_navigationNodeAt=nullptr;
StartPointFn g_startPoint=nullptr;
using SelectWeaponFn=void(__thiscall*)(uint8_t*,int,char,char,char);
SelectWeaponFn g_selectWeapon=nullptr;
IntentFn g_intent=nullptr;
IntentFn g_stop=nullptr;
SetIntentFn g_setIntent=nullptr;
ShotFn g_shot=nullptr;
ProjectileFn g_projectile=nullptr;
StateEventFn g_trackingEntry=nullptr,g_trackingUpdate=nullptr,g_swingEntry=nullptr,g_fireReceipts=nullptr;
StateEventFn g_swingUpdate=nullptr;
MessageFn g_message=nullptr;
void* g_contactReturn=nullptr;
void* g_specialCompletionGateOriginal=nullptr;
IntentFn g_queueSwing=nullptr,g_faceTarget=nullptr;
IntentFn g_attackAdmission=nullptr;
IntentFn g_facingUpdate=nullptr;
RotateFn g_rotate=nullptr;
thread_local uint8_t* g_receiptFsm=nullptr;
using AppFn=uint8_t*(__cdecl*)();
using SkillGateFn=bool(__thiscall*)(uint8_t*);
using SpecialNoticeFn=int(__thiscall*)(uint8_t*,const uint8_t*);
using CanonicalFn=uint32_t(__thiscall*)(uint8_t*,uint32_t);
AppFn g_app=nullptr;
SkillGateFn g_playerSkillGate=nullptr;
SpecialNoticeFn g_specialNotice=nullptr;
SpecialNoticeFn g_meleeNotice=nullptr,g_rangedNotice=nullptr;
SpecialNoticeFn g_spStateNotice=nullptr,g_skillNotice=nullptr;
CanonicalFn g_canonical=nullptr;
int g_appliedRoomDifficulty=-1;
void* g_plainProjectileEntry=nullptr;
GoalFn g_setGoal=nullptr;
using MoveTypeFn=int(__thiscall*)(uint8_t*,int);
MoveTypeFn g_setMoveType=nullptr;
using JumpEnabledFn=void(__thiscall*)(uint8_t*,int);
JumpEnabledFn g_setJumpEnabled=nullptr;
RouteFn g_route=nullptr;
DeleteFn g_delete=nullptr;
using QueueStateFn=bool(__thiscall*)(uint8_t*,uint8_t*);
QueueStateFn g_queueState=nullptr;
uint8_t** g_document=nullptr;
battle_ai::native::NavigationApi g_navigation={};
battle_ai::native::TransportCatalog g_transportCatalog;
battle_ai::native::TerrainRayIndex g_terrainRays;
battle_ai::native::TerrainTriangleFn g_terrainTriangle=nullptr;
battle_ai::native::TerrainHeightFn g_terrainHeight=nullptr;
battle_ai::native::TerrainNormalFn g_terrainNormal=nullptr;
battle_ai::native::TerrainSupportSampler g_terrainSupport;
battle_ai::native::MapSphereObbOverlapFn g_mapSphereObb=nullptr;
battle_ai::native::MapSphereObbContactFn g_mapSphereObbContact=nullptr;
struct RuntimeSettings {
    bool generic=true,enabled=true,aim=true,lead=true,logDecisions=false;
    bool terrainRayAudit=false;
    int maxLeadMs=3000,difficulty=1,quickSwing=80,mediumAttackIntervalMs=600;
} g_runtime;
struct DefencePlan {
    battle_ai::DefenceThreat threats[32];size_t count=0;
    battle_ai::DefenceMove movement;
    Vec3 shieldThreat;
    uint32_t routeTarget=0;
    ULONGLONG at=0;
    bool threatened=false,shielding=false,meleeBusy=false,specialBusy=false,hurt=false;
    bool shieldHitKnown=false;uint8_t shieldHitType=0;
};
struct SpecialCompletionReceipt {
    uint64_t generation=0,acceptedSequence=0,terminalSequence=0;
    uint8_t* fsm=nullptr;
    uint8_t* result=nullptr;
    uint32_t action=0,caster=0,victim=0,damage=0;
    bool armed=false;
};
struct Actor {
    uint64_t generation=0;
    ULONGLONG seen=0,aimAt=0,nextPulse=0;
    uint32_t aimTarget=0;
    int intervalMs=0;
    bool pacingLogged=false;
    bool ownsIntent=false,ownsAim=false,controlled=false;
    int savedWait=0;
    float savedYawError=0,savedPitchError=0;
    std::vector<Vec3> path;
    battle_ai::MapPathSearch mapSearch;
    bool geometryOwned=false,geometryFinal=false,pathGeometry=false;
    struct PlannedTransport {
        battle_ai::MapPathSearch::Transport edge;
        battle_ai::native::TransportVolume volume;
        Vec3 forcedExit{};
    };
    std::vector<PlannedTransport> cachedTransports,searchTransports;
    std::vector<battle_ai::native::TransportVolume> searchVolumes;
    struct TransportBlocker {uint8_t* object=nullptr;uint8_t* scene=nullptr;uint64_t signature=0;};
    std::vector<TransportBlocker> transportBlockers;
    uint64_t transportRevision=0,transportGraphRevision=0,transportSearchRevision=0;
    uint64_t transportDiagnosticRevision=0;
    uint64_t phaseGeometryRevision=0;
    uint64_t phaseInvalidations=0,phaseRouteRebuilds=0,pathGeneration=0;
    bool phaseRebuildPending=false;
    uint32_t transportGroundRejected=0,transportCorridorRejected=0;
    ULONGLONG transportCheckedAt=0;
    float transportRadius=0;
    float transportRunStep=0,transportGravityField=0;
    bool transportOrdinaryReady=false;
    Vec3 transportLocalCenter{};
    uint8_t* transportDefinition=nullptr;
    uint8_t transportAlternate=0;
    struct TransportRoute {
        bool active=false,native=false;
        bool landingGoalIssued=false;
        uint32_t phase=0,id=0;
        battle_ai::native::TransportVolume volume{};
        battle_ai::native::TransportRequestLease request{};
        Vec3 entry{},forcedExit{},exit{},activated{},arrived{};
        uint32_t landingPredictedUpdates=0,landingObservedUpdates=0;
        uint64_t landingObservedThinkSerial=0;
        ULONGLONG waitingAt=0,landingAt=0,completedAt=0,avoidUntil=0;
        uint8_t* avoidObject=nullptr;
        bool leaseTraceValid=false,leaseTraceOverlap=false;
        uint32_t leaseTraceRequest=0,leaseTraceNotice=0;
        uint32_t leaseTraceRequestTick=0,leaseTraceSnapshotTick=0;
        uint32_t leaseTracePadObject=0,leaseTraceSelectedObject=0;
        uint32_t leaseTraceFlags=0;
        uint64_t selected=0,activations=0,completed=0,routeAccepted=0,routeRejected=0,rejected=0,resumed=0;
    } transport;
    int pathMoveType=2;
    Vec3 issuedGoal{};
    bool hasIssuedGoal=false;
    struct DirectIntent {bool owned=false;double direction=0;int mode=2;Vec3 goal{},start{};uint32_t facing=0;} direct;
    bool unusedGoalLogged=false,breachAvailable=false;
    bool ownsJumpPermission=false;
    uint8_t savedJumpPermission=0;
    struct Cost {
        uint64_t calls=0,total=0,peak=0;
        void Record(uint64_t start,uint64_t finish) {const uint64_t elapsed=finish-start;++calls;total+=elapsed;peak=(std::max)(peak,elapsed);}
    } thinkCost,senseCost,navigationCost;
    ULONGLONG costLogAt=0;
    ULONGLONG directionLogAt=0,lastPolicyAt=0,policyGapLogAt=0;
    uint64_t geometryProjections=0,geometryGroundMisses=0,geometryBlocked=0;
    uint32_t geometryReject=0;
    size_t waypoint=0;
    int lastHp=0;
    ULONGLONG hurtAt=0;
    uint32_t rng=0;
    bool pathRanged=false,quickSwing=false,sideCue=false,suppressMelee=false,cancelMeleeTracking=false;
    ULONGLONG startupAt=0;
    uint32_t startupTarget=0;
    float cueAngle=0;
    int specialSlotLease=-1;
    DefencePlan defence;
    Vec3 defenceAnchor;
    ULONGLONG turnedAt=0,defenceRepathAt=0,defenceEpisodeAt=0;
    bool defenceMoveOwned=false,defenceEpisode=false;
    bool recoveryControlled=false;
    ULONGLONG recoveryAt=0;
    uint32_t expectedSpecialWeapon=0;
    ULONGLONG expectedSpecialAt=0;
    bool specialAcknowledged=false;
    ULONGLONG specialAcknowledgedAt=0;
    uint64_t specialAcknowledgedSequence=0;
    uint64_t specialFinished=0;
    SpecialCompletionReceipt specialCompletion;
    uint64_t zeroCompletionCount=0;
    uint64_t thinkSerial=0,policySerial=0,nativeThinkSerial=0;
    uint32_t observedTarget=0,observedFarTargets=0;
    ULONGLONG observedTargetAt=0;
    bool observedTargetVisible=false;
    float observedTargetDistance=0,observedMaxRange=0;
    Vec3 observedTargetPoint{};
    uint64_t sharedContactFrames=0,sharedPursuitFrames=0,explorationRoutes=0,perceptionSerial=0;
    uint32_t sharedTarget=0,sharedSource=0,searchGoalKind=0,searchCursor=0,searchRng=0;
    Vec3 previousPerceptionPos{},searchGoal{};
    bool hasPerceptionPos=false,explorationOwned=false;
    ULONGLONG explorationRetryAt=0,stationarySince=0;
    ULONGLONG explorationLogAt=0;
    uint32_t explorationStatus=0,explorationNodeCount=0;
    uint32_t explorationCandidates=0,explorationValidNodes=0,explorationRouteAttempts=0;
    uint32_t routeStatus=0,followStatus=0,routePointCount=0;
    Vec3 routeStart{},routeGoal{};
    uint32_t roleUnit=0;
    bool roleAlternate=false,meleeRole=false;
    uint64_t coverQueryFrame=0;
    bool coverQueryUsed=false;
    struct CoverSearch {
        uint32_t target=0,weapon=0;
        ULONGLONG started=0;
        int sample=0,side=0,exposure=0;
        bool active=false;
        float score=1.0e9f;
        Vec3 hide,peek;
    } coverSearch;
};
std::unordered_map<uint8_t*,Actor> g_actors;
void ClearStartup(uint8_t*,Actor&,bool);
bool PlayerBotRulesMode(uint8_t* unit);
struct Motion {
    uint8_t* unit=nullptr;
    uint64_t generation=0;
    Vec3 point,velocity;
    ULONGLONG at=0;
};
std::unordered_map<uint32_t,Motion> g_motion;
struct RangedActivity {
    ULONGLONG time=0,primaryAt=0;
    uint32_t target=0,weapon=0,primaryTarget=0;
    uint64_t calls=0,attempts=0,shots=0,nonzeroReturns=0;
    uint64_t mobilePrimaryDispatches=0,mobileDefensiveDispatches=0,backwardPrimaryDispatches=0;
};
std::unordered_map<uint8_t*,RangedActivity> g_rangedShots;
struct Contact {uint32_t target=0,action=0;int combo=0;ULONGLONG time=0;uint8_t* victim=nullptr;};
std::unordered_map<uint8_t*,Contact> g_contacts;
struct Incoming {uint32_t attacker=0;Vec3 point;ULONGLONG time=0;bool melee=false;uint8_t hitType=0;bool hitKnown=false;};
std::unordered_map<uint8_t*,std::vector<Incoming>> g_incoming;
struct LearnedSpeed { float perUpdate=0;int modifier=0;ULONGLONG seen=0;bool nonlinear=false; };
struct FlightRun { uint64_t key=0;float speed=0;int stable=0;ULONGLONG seen=0;Vec3 end; };
std::unordered_map<uint64_t,LearnedSpeed> g_speeds;
std::unordered_map<uint8_t*,FlightRun> g_flights;
thread_local const char* g_nativeCallback=nullptr;
thread_local bool g_nativeFallback=false;
std::atomic<DWORD> g_gameThread{0};
std::mutex g_pendingMutex;
std::vector<uint8_t*> g_pendingInit;
struct SpNotice {
    uint32_t pilot=0,weapon=0,victimPilot=0,damage=0;
    int32_t value=-1;
    ULONGLONG time=0;
    uint64_t sequence=0;
    bool begin=false,stateOnly=false,finish=false,meleeCompletion=false;
};
std::mutex g_spMutex;
std::vector<SpNotice> g_pendingSp;
std::atomic<uint64_t> g_spSequence{0};
struct SpSnapshot {uint64_t sequence=0;int32_t value=0;};
std::unordered_map<uint8_t*,SpSnapshot> g_spSnapshots;
std::mutex g_loadoutMutex;
std::unordered_map<uint8_t*,battle_ai::loadout::ClientApplyState> g_loadouts;
battle_ai::loadout::ClientNativeFunctions g_loadoutNative;

uint64_t SpeedKey(uint32_t unitId,uint32_t weaponId,bool alternate) {
    return (uint64_t(unitId)<<32)|(uint64_t(weaponId)<<1)|(alternate?1:0);
}
int SpeedModifier(uint8_t* unit,uint8_t* weapon) {
    uint8_t range=weapon[0x6A];
    if(range==2) return *reinterpret_cast<int*>(unit+0xCEC);
    if(range==3) return *reinterpret_cast<int*>(unit+0xCF0);
    return 0;
}
float MeasuredSpeed(uint8_t* unit,const battle_ai::native::Capabilities& caps,uint32_t weaponId) {
    auto found=g_speeds.find(SpeedKey(caps.unitId,weaponId,caps.alternate));
    if(found==g_speeds.end()||found->second.nonlinear||GetTickCount64()-found->second.seen>30000) return 0;
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
    if(!record || *reinterpret_cast<uint32_t*>(record)!=weaponId ||
       found->second.modifier!=SpeedModifier(unit,record)) return 0;
    return found->second.perUpdate*60.0f;
}
float PredictionSpeed(uint8_t* unit,const battle_ai::native::Capabilities& caps,const battle_ai::native::Weapon& w) {
    auto observed=g_speeds.find(SpeedKey(caps.unitId,w.id,caps.alternate));
    if(observed!=g_speeds.end() && observed->second.nonlinear) return 0;
    return w.projectileSpeed>0?w.projectileSpeed:MeasuredSpeed(unit,caps,w.id);
}

Actor& ActorFor(uint8_t* unit) {
    auto& a=g_actors[unit];
    auto gen=g_generations.find(unit);
    uint64_t generation=gen==g_generations.end()?0:gen->second;
    if(a.generation!=generation) { a=Actor{}; a.generation=generation; }
    a.seen=GetTickCount64();
    if(g_actors.size()>1024) {
        for(auto it=g_actors.begin();it!=g_actors.end();) {
            if(it->first!=unit && a.seen-it->second.seen>10000) it=g_actors.erase(it); else ++it;
        }
    }
    return a;
}
bool ObjectIsA(uint8_t* object,int kind) {
    __try {
        if(!object)return false;
        auto table=*reinterpret_cast<void***>(object);
        if(!table||!table[1])return false;
        using IsA=bool(__thiscall*)(void*,int);
        return reinterpret_cast<IsA>(table[1])(object,kind);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CombatTargetAlive(uint8_t* object,int hp) {
    if(!object)return false;
    // CheckObejctAlive (0x5013A0) uses native lifecycle +EC4 for
    // player/NPC kinds 8/10. A zero HP sample is not its death receipt.
    if(ObjectIsA(object,8)||ObjectIsA(object,10)) {
        uint32_t state=0;
        return battle_ai::native::detail::Read(object,0xEC4,state)&&state!=0;
    }
    // Destructible scene objects retain their separate HP-based lifecycle.
    return hp>0;
}
bool CombatTargetAlive(uint8_t* object) {
    int32_t hp=0;
    return object&&battle_ai::native::detail::Read(object,0x38,hp)&&CombatTargetAlive(object,hp);
}
bool Owned(uint8_t* unit) {
    __try {
        // UnitManager uses this virtual IsA(10) gate before it reads or
        // removes NPC state. Document lookup alone also returns map objects.
        if(!ObjectIsA(unit,10)||
           !battle_ai::native::detail::Readable(unit,0x326D))return false;
        return unit[0x2B65]==0 && unit[0x326C]!=0 &&
            *reinterpret_cast<uint32_t*>(unit+0x3238)>=3604001 &&
            *reinterpret_cast<uint32_t*>(unit+0x3238)<=3607000;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ClientLoadoutProfile(void* context,const void*,battle_ai::loadout::NativeProfile* output) {
    using namespace battle_ai::native::detail;
    auto unit=static_cast<uint8_t*>(context);uint8_t* fsm=nullptr;uint8_t* definition=nullptr;uint8_t alternate=0;
    if(!output||!ReadPointer(unit,0x9CC,fsm)||!ReadPointer(unit,0x9C4,definition)||!Read(unit,0xE34,alternate)||alternate>1) return false;
    battle_ai::loadout::WeaponProfile weapons[3]={};size_t count=0;
    for(size_t slot=0;slot<3;++slot) {
        uint32_t id=0,recordId=0;uint8_t* record=nullptr;uint8_t rangeType=0,style=0;
        if(!Read(definition,0x249+slot*4,id)) return false;
        if(!id) continue;
        auto bank=alternate?0x56C:0x24C;
        if(!ReadPointer(fsm,bank+slot*0xC8+0xC,record)||!Read(record,0,recordId)||recordId!=id||
           !Read(record,0x69,style)||!Read(record,0x6A,rangeType)) return false;
        if(style==2) continue;
        if(style<1||style>3||rangeType<1||rangeType>8) return false;
        auto& facts=weapons[count++];facts.style=style;facts.rangeType=rangeType;
        if(!Read(record,0x6C,facts.damage)||!Read(record,0x72,facts.range)||!Read(record,0x7C,facts.magazine)||!Read(record,0x264,facts.maxCombo)) return false;
    }
    *output=battle_ai::loadout::ProfileFromWeapons(weapons,count);return count!=0;
}
void ApplyNativeAbsoluteSp(uint8_t*,int32_t);
battle_ai::loadout::ApplyResult InitializeLoadout(uint8_t* unit,int options,uint32_t generation) {
    __try {
        int difficulty=options&0xFF;bool initial=(options&0x100)!=0;
        if(!unit||!generation||!g_api||g_preparationFault||difficulty<0||difficulty>2||
           (options&~0x1FF)!=0||
           GetPrivateProfileIntW(L"settings",L"Enabled",1,g_config.c_str())==0) return battle_ai::loadout::ApplyResult::InvalidArgument;
        auto npc=*reinterpret_cast<uint32_t*>(unit+0x3238);
        if(npc<3604001||npc>3607000) return battle_ai::loadout::ApplyResult::InvalidArgument;
        auto definition=*reinterpret_cast<uint8_t**>(unit+0x9C4);
        battle_ai::loadout::NativeLoadout plan;
        battle_ai::loadout::NativeProfileSource source{unit,ClientLoadoutProfile};
        if(!battle_ai::loadout::FromNativeSource(static_cast<battle_ai::Difficulty>(difficulty),source,definition,&plan)) return battle_ai::loadout::ApplyResult::NativeUnavailable;
        if(g_loadouts.size()>2048) g_loadouts.clear();
        auto& state=g_loadouts[unit];
        bool newGeneration=!battle_ai::loadout::SameGeneration(state,unit,generation);
        if(initial&&difficulty>=1&&newGeneration) ApplyNativeAbsoluteSp(unit,0);
        return battle_ai::loadout::ApplyClient(unit,generation,plan,g_loadoutNative,&state);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return battle_ai::loadout::ApplyResult::NativeUnavailable;}
}
Vec3 Position(uint8_t* unit) { return *reinterpret_cast<Vec3*>(*reinterpret_cast<uint8_t**>(unit+8)+0x88); }
Vec3 AimPoint(uint8_t* unit) {
    Vec3 position=Position(unit);
    auto table=*reinterpret_cast<void***>(unit);
    auto node=*reinterpret_cast<uint8_t**>(unit+8);
    if(table&&table[1]&&node) {
        using IsA=bool(__thiscall*)(uint8_t*,int);
        auto isA=reinterpret_cast<IsA>(table[1]);
        uint8_t* shape=nullptr;Vec3 point={};
        using namespace battle_ai::native::detail;
        if(isA(unit,7)&&ReadPointer(node,0xE0,shape)&&Read(shape,0x10,point)&&battle_ai::Finite(point)) return point;
        if(isA(unit,0x12)&&ReadPointer(node,0xD8,shape)&&Read(shape,0x3C,point)&&battle_ai::Finite(point)) return point;
        if(isA(unit,0x15)) return position;
    }
    auto scene=*reinterpret_cast<uint8_t**>(unit+0x34);
    if(scene) {
        auto bounds=reinterpret_cast<float*>(scene+0xBC);
        Vec3 low={bounds[0],bounds[1],bounds[2]},high={bounds[3],bounds[4],bounds[5]};
        Vec3 center=(low+high)*0.5f;
        if(battle_ai::Finite(center) && low.x<=high.x && low.y<=high.y && low.z<=high.z &&
           battle_ai::Length(center-position)<10000) return center;
    }
    return position;
}
Vec3 Velocity(uint8_t* unit,uint32_t id,Vec3 point,ULONGLONG now,uint64_t generation=0) {
    auto& m=g_motion[id];
    if(m.unit!=unit || (generation&&m.generation!=generation) || !m.at || now-m.at>1000) {
        m={unit,generation,point,{},now};return {};
    }
    if(generation&&!m.generation) m.generation=generation;
    if(now-m.at>=20) {
        Vec3 v=(point-m.point)*(1000.0f/static_cast<float>(now-m.at));
        m.velocity=battle_ai::Finite(v) && battle_ai::Length(v)<50000 ? v : Vec3{};
        m.point=point;m.at=now;
    }
    return m.velocity;
}
uint8_t* FindObject(uint32_t id) {
    if(!g_document || !*g_document || !g_findObject) return nullptr;
    // The engine's object lookup includes players, NPCs and AI participant list.
    // Resolve its immutable, manifest-verified entry once at Mod load. Targets
    // themselves are still looked up live for every shot/notice/decision.
    return g_findObject(*g_document,id);
}
bool ControlBlocked(uint8_t* unit) {
    uint8_t* movement=nullptr;
    if(!battle_ai::native::detail::ReadPointer(unit,0x9C8,movement)) return true;
    for(size_t offset:{size_t(0xD4),size_t(0xE8),size_t(0xFC),size_t(0x110),size_t(0x138)})
        if(battle_ai::native::detail::InState(movement,offset)) return true;
    return false;
}
bool ArmMeleeCompletion(uint8_t* unit,Actor& actor,const SpNotice& notice,uint64_t acceptedSequence) {
    __try {
        auto generation=g_generations.find(unit);
        auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
        if(generation==g_generations.end()||!actor.generation||actor.generation!=generation->second||
           !fsm||*reinterpret_cast<uint8_t**>(fsm+4)!=fsm+0xAC||fsm[0x9B5]!=0) return false;
        auto action=*reinterpret_cast<uint32_t*>(fsm+0x224);
        auto result=*reinterpret_cast<uint8_t**>(fsm+0x9BC);
        auto victimId=g_canonical(*g_document,notice.victimPilot);
        auto victim=FindObject(victimId);
        if(notice.damage!=0||!action||!result||!victimId||!victim||
           !CombatTargetAlive(victim)||
           *reinterpret_cast<uint32_t*>(result+0x10)!=notice.damage||
           *reinterpret_cast<uint32_t*>(result+0x14)!=victimId||
           *reinterpret_cast<uint32_t*>(victim+0x9D8)!=4||
           *reinterpret_cast<uint32_t*>(victim+0xEB8)!=*reinterpret_cast<uint32_t*>(unit+4)) return false;
        actor.specialCompletion={actor.generation,acceptedSequence,notice.sequence,fsm,result,action,
                                 *reinterpret_cast<uint32_t*>(unit+4),victimId,notice.damage,true};
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool AllowZeroDamageCompletion(uint8_t* fsm,uint8_t* owner,uint8_t* result) {
    if(GetCurrentThreadId()!=g_gameThread||!g_runtime.enabled||g_runtime.difficulty<1||
       g_preparationFault||!PlayerBotRulesMode(owner)) return false;
    __try {
        if(!Owned(owner)||!fsm||!result||owner[0xEC8]!=0||*reinterpret_cast<uint8_t**>(owner+0x9CC)!=fsm||
           *reinterpret_cast<uint8_t**>(fsm+0x9BC)!=result||*reinterpret_cast<uint8_t**>(fsm+4)!=fsm+0xAC||
           fsm[0x9B3]!=1||fsm[0x9B5]!=0||*reinterpret_cast<uint32_t*>(result+0x10)!=0) return false;
        auto found=g_actors.find(owner);
        auto generation=g_generations.find(owner);
        if(found==g_actors.end()||generation==g_generations.end()) return false;
        auto& actor=found->second;auto& receipt=actor.specialCompletion;
        auto action=*reinterpret_cast<uint32_t*>(fsm+0x224);
        auto caster=*reinterpret_cast<uint32_t*>(owner+4);
        auto victimId=*reinterpret_cast<uint32_t*>(result+0x14);
        if(!receipt.armed||!action||receipt.generation!=actor.generation||receipt.generation!=generation->second||
           receipt.acceptedSequence!=actor.specialAcknowledgedSequence||
           receipt.terminalSequence!=actor.specialFinished||receipt.fsm!=fsm||receipt.result!=result||
           receipt.action!=action||
           receipt.caster!=caster||receipt.victim!=victimId||receipt.damage!=0) return false;
        auto victim=FindObject(victimId);
        if(!CombatTargetAlive(victim)||
           *reinterpret_cast<uint32_t*>(victim+0x9D8)!=4||
           *reinterpret_cast<uint32_t*>(victim+0xEB8)!=caster) return false;
        receipt.armed=false;++actor.zeroCompletionCount;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
const DWORD g_specialCompletionDefaultMxcsr=0x1F80;
void __stdcall DispatchSpecialCompletionGate(BYTE* frame) noexcept {
    const DWORD error=GetLastError();
    __try {
        auto& flags=*reinterpret_cast<DWORD*>(frame+0x20);
        if((flags&0x40)!=0) {
            auto fsm=*reinterpret_cast<uint8_t**>(frame+4);
            auto result=*reinterpret_cast<uint8_t**>(frame+0x18);
            auto owner=*reinterpret_cast<uint8_t**>(frame+0x1C);
            if(AllowZeroDamageCompletion(fsm,owner,result)) flags&=~DWORD(0x41);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    SetLastError(error);
}
__declspec(naked) void SpecialCompletionGate() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,210h
        and esp,0FFFFFFF0h
        mov [esp+200h],eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_specialCompletionDefaultMxcsr]
        cld
        mov eax,[esp+200h]
        push eax
        call DispatchSpecialCompletionGate
        fxrstor [esp]
        mov esp,[esp+200h]
        popad
        popfd
        jmp dword ptr [g_specialCompletionGateOriginal]
    }
}
bool HurtState(uint8_t* unit) {
    uint8_t* movement=nullptr;
    if(!battle_ai::native::detail::ReadPointer(unit,0x9C8,movement)) return false;
    return battle_ai::native::detail::InState(movement,0xE8)||battle_ai::native::detail::InState(movement,0xFC)||battle_ai::native::detail::InState(movement,0x110);
}
bool FairSpecialResource(uint8_t* fsm,uint8_t* owner) {
    __try {
        uint8_t* controller=nullptr;int restriction=1;
        return owner&&*reinterpret_cast<int*>(owner+0xBC8)>=30000&&owner[0x31C4]!=1&&
            battle_ai::native::detail::ReadPointer(fsm,0xB30,controller)&&
            battle_ai::native::detail::Read(controller,0x2C0,restriction)&&restriction==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
uint8_t* CurrentRulesMode() {
    __try {
        auto app=g_app?g_app():nullptr;if(!app) return nullptr;
        auto service=*reinterpret_cast<uint8_t**>(app+0x4A4);if(!service) return nullptr;
        return *reinterpret_cast<uint8_t**>(service+0xC);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}
bool PvpPlayerBotRulesMode() {
    __try {
        auto mode=CurrentRulesMode();if(!mode) return false;
        auto room=*reinterpret_cast<uint8_t**>(mode+8);if(!room) return false;
        auto kind=*reinterpret_cast<uint16_t*>(room+0x66);
        return kind!=5&&kind!=8&&kind!=10&&kind!=11&&kind!=12;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool MissionWingmanRulesUnit(uint8_t* unit) {
    __try {
        if(!unit||!g_document||!*g_document)return false;
        auto mode=CurrentRulesMode();if(!mode)return false;
        auto room=*reinterpret_cast<uint8_t**>(mode+8);if(!room)return false;
        const auto kind=*reinterpret_cast<uint16_t*>(room+0x66);
        if(kind!=5&&kind!=8&&kind!=10)return false;
        static SdgoMissionAiClientGetSnapshotV1Fn snapshot=nullptr;
        if(!snapshot) {
            const auto module=GetModuleHandleW(L"mission_ai.dll");
            if(module)snapshot=reinterpret_cast<SdgoMissionAiClientGetSnapshotV1Fn>(
                GetProcAddress(module,SDGO_MISSION_AI_CLIENT_SNAPSHOT_EXPORT));
        }
        if(!snapshot)return false;
        SdgoMissionAiClientSnapshotV1 status{};status.size=sizeof(status);status.version=1;
        if(!snapshot(&status))return false;
        uint32_t object=0,pilot=0;
        if(!battle_ai::native::detail::Read(unit,4,object)||
           !battle_ai::native::detail::Read(unit,0x2B68,pilot))return false;
        return battle_ai::mission::AdmitsWingman(
            status,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mode)),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*g_document)),
            kind,object,pilot);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool PlayerBotRulesMode(uint8_t* unit) {
    return PvpPlayerBotRulesMode()||MissionWingmanRulesUnit(unit);
}
void RestoreIntent(uint8_t* unit,Actor& a) {
    __try {
        ClearStartup(unit,a,!ControlBlocked(unit));
        if(a.controlled||a.defenceMoveOwned) { g_setIntent(unit,0);g_stop(unit); }
        if(a.ownsIntent) *reinterpret_cast<int*>(unit+0x31F4)=a.savedWait;
        if(a.ownsAim) {
            *reinterpret_cast<float*>(unit+0x3204)=a.savedYawError;
            *reinterpret_cast<float*>(unit+0x3208)=a.savedPitchError;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if(a.ownsJumpPermission&&g_setJumpEnabled)g_setJumpEnabled(unit,a.savedJumpPermission);
    a.ownsJumpPermission=false;
    a.controlled=false;a.ownsIntent=false;a.ownsAim=false;a.aimTarget=0;a.hasIssuedGoal=false;a.direct={};
    a.nextPulse=0;
    a.suppressMelee=false;a.cancelMeleeTracking=false;a.path.clear();
    a.explorationOwned=false;
    a.geometryOwned=false;
    a.transport.active=false;a.transport.native=false;a.transport.phase=0;
    a.transport.request={};
    a.defenceMoveOwned=false;a.defence={};
    a.recoveryControlled=false;
}
bool TransportRouteProtected(const Actor::TransportRoute& trip,ULONGLONG now) {
    if(!trip.active)return false;
    if(trip.native||trip.phase==5||trip.request.committed)return true;
    return trip.phase==1&&
        ((trip.waitingAt&&now>=trip.waitingAt&&
          now-trip.waitingAt<
              battle_ai::native::TransportEntranceWaitMs(false))||
         battle_ai::native::TransportRequestAdmissionOpen(
             trip.request,now));
}
bool TransportMotorReserved(const Actor::TransportRoute& trip,ULONGLONG now) {
    return trip.active&&!trip.native&&
        (trip.phase==5||trip.request.committed||
         (trip.phase==1&&
          ((trip.waitingAt&&now>=trip.waitingAt&&
            now-trip.waitingAt<
                battle_ai::native::TransportEntranceWaitMs(false))||
           battle_ai::native::TransportRequestAdmissionOpen(
               trip.request,now))));
}
void AdvancePath(uint8_t* unit,Actor& a) {
    if(a.path.empty()) return;
    const Vec3 here=Position(unit);
    while(a.waypoint<a.path.size()&&battle_ai::Length(a.path[a.waypoint]-here)<(a.pathRanged||a.pathGeometry?25:100)) ++a.waypoint;
    if(a.waypoint>=a.path.size()) {
        a.path.clear();
        if(a.transport.active&&!a.transport.native&&a.transport.phase==1&&!a.transport.waitingAt)
            a.transport.waitingAt=GetTickCount64();
    }
}
bool MovementAvailable(uint8_t* unit,bool allowRanged) {
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    if(!fsm||ControlBlocked(unit)) return false;
    auto movement=*reinterpret_cast<uint8_t**>(unit+0x9C8);
    if(battle_ai::native::detail::InState(movement,0x124))return false;
    if(battle_ai::native::detail::Neutral(fsm)) return true;
    if(!allowRanged||*reinterpret_cast<uint8_t**>(fsm+4)!=fsm+0x34||
       (*reinterpret_cast<uint8_t**>(fsm+8)&&*reinterpret_cast<uint8_t**>(fsm+8)!=fsm+0x34)) return false;
    auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
    return record&&(record[0x8F]==1||fsm[0xB38]==1);
}
void SynchronizePhaseGeometry(uint8_t* unit,Actor& actor) {
    const uint64_t revision=g_phaseGeometryRevision.load(std::memory_order_acquire);
    if(actor.phaseGeometryRevision==revision)return;
    const bool ownedRoute=!actor.path.empty()||actor.geometryOwned||actor.explorationOwned||
        actor.defenceMoveOwned||actor.direct.owned||(actor.transport.active&&!actor.transport.native);
    if(actor.phaseGeometryRevision) {
        ++actor.phaseInvalidations;
        if(ownedRoute)actor.phaseRebuildPending=true;
    }
    actor.phaseGeometryRevision=revision;
    actor.path.clear();actor.waypoint=0;actor.geometryOwned=false;actor.geometryFinal=false;
    actor.pathGeometry=false;actor.explorationOwned=false;actor.defenceMoveOwned=false;
    actor.hasIssuedGoal=false;actor.direct={};
    if(!actor.transport.native) {
        actor.transport.active=false;actor.transport.phase=0;
        actor.transport.request={};
    }
    actor.cachedTransports.clear();actor.searchTransports.clear();actor.searchVolumes.clear();
    actor.transportBlockers.clear();actor.transportRevision=UINT64_MAX;
    if(ownedRoute&&g_stop&&MovementAvailable(unit,actor.pathRanged))g_stop(unit);
}
void RecordRouteInstall(Actor& actor) {
    ++actor.pathGeneration;
    if(actor.phaseRebuildPending) {
        ++actor.phaseRouteRebuilds;
        actor.phaseRebuildPending=false;
    }
}
void RecordPhaseGeometryEvent(uint32_t objectId,uint32_t action) {
    if(action==1)g_phaseHideCount.fetch_add(1,std::memory_order_relaxed);
    else if(action==2)g_phaseUnhideCount.fetch_add(1,std::memory_order_relaxed);
    g_phaseLastObject.store(objectId,std::memory_order_relaxed);
    g_phaseLastAction.store(action,std::memory_order_relaxed);
    g_phaseEventSequence.fetch_add(1,std::memory_order_release);
    g_phaseGeometryRevision.fetch_add(1,std::memory_order_release);
}
void __fastcall PhaseHide(uint8_t* mode,void*,uint32_t objectId) {
    g_phaseHide(mode,objectId);
    RecordPhaseGeometryEvent(objectId,1);
}
void __fastcall PhaseUnhide(uint8_t* mode,void*,uint32_t objectId) {
    g_phaseUnhide(mode,objectId);
    RecordPhaseGeometryEvent(objectId,2);
}
auto MotionSafetyFor(uint8_t* unit,Vec3 destination) {
    if(!g_document||!*g_document) return battle_ai::native::MotionSafety::Unavailable;
    return battle_ai::native::CheckMotionSafety(
        g_navigation,*g_document,unit,Position(unit),destination);
}
battle_ai::native::TerrainSupportStatus PhysicsSupport(void* context,Vec3 point,battle_ai::native::TerrainSupportSample& output) {
    if(GetCurrentThreadId()!=g_gameThread||!g_document||context!=*g_document)return battle_ai::native::TerrainSupportStatus::Invalid;
    return g_terrainSupport.Sample(static_cast<uint8_t*>(context),point,output);
}
battle_ai::native::MapGeometry MapGeometryFor(uint8_t* unit) {
    battle_ai::native::MapGeometry geometry{};
    if(!unit||!g_document||!*g_document)return geometry;
    uint8_t* scene=nullptr;
    if(!battle_ai::native::detail::ReadPointer(unit,battle_ai::native::detail::kOwnerScene,scene)||
       !battle_ai::native::detail::Read(scene,battle_ai::native::detail::kSceneAabb,geometry.ownerBounds))return geometry;
    uint8_t* sphere=nullptr;Vec3 center{};float radius=0;int32_t spheres=0;
    if(!battle_ai::native::detail::Read(scene,0xDC,spheres)||spheres<=0||spheres>4096||
       !battle_ai::native::detail::ReadPointer(scene,0xE0,sphere)||!battle_ai::native::detail::Read(sphere,0x10,center)||
       !battle_ai::native::detail::Read(sphere,0x1C,radius)||!battle_ai::Finite(center)||!std::isfinite(radius)||radius<=0)return geometry;
    const Vec3 extent={radius,radius,radius};geometry.bodyBounds={center-extent,center+extent};
    if(!battle_ai::native::detail::Read(sphere,0,geometry.bodySphere))return geometry;
    geometry.sphereObb=g_mapSphereObb;geometry.sphereObbContact=g_mapSphereObbContact;
    geometry.api=g_navigation;geometry.document=*g_document;geometry.api.supportContext=geometry.document;
    geometry.owner=unit;geometry.ownerPosition=Position(unit);
    const auto actor=g_actors.find(unit);
    geometry.deferDestroyables=actor!=g_actors.end()&&actor->second.breachAvailable;
    return geometry;
}
bool __fastcall IndexedTerrainRay(void* scene,void*,const Ray3* ray,float* distance) {
    const auto faults=g_terrainRays.stats.mismatches;
    const bool hit=g_terrainRays.Query(
        scene,ray,distance,g_runtime.terrainRayAudit);
    if(g_terrainRays.stats.mismatches!=faults&&g_api&&g_api->Log)
        g_api->Log(g_mod,SDGO_LOG_ERROR,"terrain ray audit mismatch: native geometry retained for scene %p",scene);
    return hit;
}
bool MapGeometryStep(void* context,Vec3 from,Vec3 desired,Vec3& grounded) {
    __try {return static_cast<battle_ai::native::MapGeometry*>(context)->Step(from,desired,grounded);}
    __except(EXCEPTION_EXECUTE_HANDLER) {static_cast<battle_ai::native::MapGeometry*>(context)->lastReject=9;return false;}
}
bool ImmediateMotionClear(uint8_t* unit,Vec3 destination) {
    if(!PlayerBotRulesMode(unit))return true;
    __try {
        auto geometry=MapGeometryFor(unit);geometry.deferDestroyables=false;
        return geometry.Valid()&&geometry.ClearBody(Position(unit),destination);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
uint64_t TransportBlockSignature(uint8_t* object,uint8_t* scene) {
    using battle_ai::native::detail::Read;
    uint8_t removed=0,solid=0,sphereEnabled=0;int hp=0,obbs=0,spheres=0;uint32_t id=0;
    if(!Read(object,4,id)||!Read(object,0x1C,removed)||!Read(object,0x38,hp)||
       !Read(scene,0x114,solid)||!Read(scene,0x115,sphereEnabled)||
       !Read(scene,0xD4,obbs)||!Read(scene,0xDC,spheres))return 0;
    uint64_t value=uint64_t(id)+1;
    for(uint32_t field:{uint32_t(removed),uint32_t(hp<=0),uint32_t(solid),uint32_t(sphereEnabled),uint32_t(obbs),uint32_t(spheres)})
        value=(value^field)*1099511628211ull;
    return value;
}
enum class TransportOrdinaryProfileState : uint8_t {
    Forced,
    Unsupported,
    Ready
};
bool NativeTransportMoveForced(uint8_t* unit) {
    uint8_t* movement=nullptr;
    return battle_ai::native::detail::ReadPointer(
               unit,0x9C8,movement)&&
        battle_ai::native::detail::InState(movement,0x124);
}
bool NativeTransportJumpForced(uint8_t* unit) {
    uint8_t* jump=nullptr;
    return battle_ai::native::detail::ReadPointer(
               unit,0x9D4,jump)&&
        battle_ai::native::detail::InState(jump,0x98);
}
TransportOrdinaryProfileState ReadTransportOrdinaryProfile(
    uint8_t* unit,
    battle_ai::native::TransportOrdinaryMotionProfile& profile) {
    using namespace battle_ai::native;
    profile={};
    uint8_t* movement=nullptr;uint8_t* jump=nullptr;
    uint8_t* attack=nullptr;uint8_t* definition=nullptr;
    uint8_t* context=nullptr;uint8_t* jumpOwner=nullptr;
    uint8_t* jumpMovement=nullptr;uint8_t* jumpAttack=nullptr;
    if(!detail::ReadPointer(unit,0x9C8,movement)||
       !detail::ReadPointer(unit,0x9D4,jump)||
       !detail::ReadPointer(unit,0x9CC,attack)||
       !detail::ReadPointer(unit,0x9C4,definition))
        return TransportOrdinaryProfileState::Unsupported;
    if(detail::InState(movement,0x124)||
       detail::InState(jump,0x98))
        return TransportOrdinaryProfileState::Forced;
    if(!detail::Neutral(movement)||!detail::Neutral(jump)||
       !detail::ReadPointer(jump,0xD0,jumpOwner)||
       !detail::ReadPointer(jump,0xD4,jumpMovement)||
       !detail::ReadPointer(jump,0xD8,jumpAttack)||
       jumpOwner!=unit||jumpMovement!=movement||jumpAttack!=attack||
       !detail::ReadPointer(unit,0x34,context))
        return TransportOrdinaryProfileState::Unsupported;
    uint32_t definitionId=0,attachedGround=0;
    uint32_t motionFlags=0,contextOverride=0;
    uint8_t definitionProfile=0,jumpSpecial=0;
    uint8_t jumpHeld=0,jumpSuppressRoot=0;
    uint8_t attackOverride=0;
    float runStep=0,gravityField=0;
    Vec3 velocity{};
    if(!detail::Read(definition,0,definitionId)||
       !detail::Read(definition,0x131,definitionProfile)||
       !detail::Read(jump,0xB4,jumpSpecial)||
       !detail::Read(jump,0xC8,jumpHeld)||
       !detail::Read(jump,0xBF,jumpSuppressRoot)||
       !detail::Read(unit,0xD88,attachedGround)||
       !detail::Read(attack,0xBC8,attackOverride)||
       !detail::Read(unit,0xD48,motionFlags)||
       !detail::Read(context,0xF8,contextOverride)||
       !detail::Read(unit,0xB2C,runStep)||
       !detail::Read(unit,0xD10,gravityField)||
       !detail::Read(movement,0x19C,velocity.x)||
       !detail::Read(jump,0xAC,velocity.y)||
       !detail::Read(movement,0x1A4,velocity.z)||
       !TransportOrdinaryGravityProfileAllowed(
           definitionId,definitionProfile,jumpSpecial,jumpHeld,
           jumpSuppressRoot,attachedGround,attackOverride,
           motionFlags,contextOverride,runStep,gravityField)||
       !Finite(velocity))
        return TransportOrdinaryProfileState::Unsupported;
    profile.runStep=runStep;
    profile.gravityField=gravityField;
    profile.state={Position(unit),velocity};
    return Finite(profile.state.position)?
        TransportOrdinaryProfileState::Ready:
        TransportOrdinaryProfileState::Unsupported;
}
bool TransportOrdinaryPathClear(
    battle_ai::native::MapGeometry& geometry,
    const battle_ai::native::TransportOrdinaryAirArrival& arrival,
    Vec3 landing) {
    if(!arrival.reachable||arrival.path.size()<2)return false;
    for(size_t i=1;i<arrival.path.size();++i)
        if(!geometry.ClearBody(
               arrival.path[i-1],arrival.path[i]))
            return false;
    return geometry.ClearBody(arrival.path.back(),landing);
}
bool TransportLandingGoalCurrent(
    uint8_t* unit,const Actor::TransportRoute& trip) {
    using namespace battle_ai::native;
    Vec3 goal{};uint32_t mode=0;
    return unit&&unit[0x30DC]!=0&&
        detail::Read(unit,0x3078,goal)&&
        detail::Read(unit,0xDC4,mode)&&
        TransportOrdinaryGoalModeAllowed(mode)&&
        Finite(goal)&&Length(goal-trip.exit)<=1;
}
bool EnsureTransportLandingGoal(
    uint8_t* unit,const Actor::TransportRoute& trip) {
    using namespace battle_ai::native;
    if(TransportLandingGoalCurrent(unit,trip))return true;
    return g_setGoal&&
        g_setGoal(
            unit,&trip.exit,
            static_cast<int>(kTransportOrdinaryGoalMode));
}
bool TransportLandingObservationExpired(
    uint32_t observedUpdates,uint32_t predictedUpdates) {
    // Think is the native unit update callback. One additional observation
    // lets the accepted 3A command become the movement record before rejecting
    // a goal that has failed to produce the modeled landing.
    return !predictedUpdates||
        uint64_t(observedUpdates)>
            uint64_t(predictedUpdates)+1;
}
void RefreshTransportCandidates(uint8_t* unit,Actor& actor,battle_ai::native::MapGeometry& geometry) {
    using namespace battle_ai::native;
    if(!PlayerBotRulesMode(unit)||!geometry.Valid())return;
    Aabb bounds{};if(!g_terrainSupport.Bounds(geometry.document,bounds))return;
    const auto now=GetTickCount64();g_transportCatalog.Pump(g_navigation.query,geometry.document,bounds,now);
    uint8_t* definition=nullptr;uint8_t alternate=0;
    if(!detail::ReadPointer(unit,0x9C4,definition)||!detail::Read(unit,0xE34,alternate))return;
    TransportOrdinaryMotionProfile ordinaryProfile{};
    const bool ordinaryProfileReady=
        ReadTransportOrdinaryProfile(unit,ordinaryProfile)==
            TransportOrdinaryProfileState::Ready;
    bool changedBlock=false;
    for(const auto& blocker:actor.transportBlockers)
        if(TransportBlockSignature(blocker.object,blocker.scene)!=blocker.signature) {changedBlock=true;break;}
    const bool diagnosticPending=g_runtime.logDecisions&&g_transportCatalog.Complete()&&
        actor.transportDiagnosticRevision!=g_transportCatalog.Revision();
    if(!changedBlock&&actor.transportRevision==g_transportCatalog.Revision()&&
       actor.transportDefinition==definition&&actor.transportAlternate==alternate&&
       std::abs(actor.transportRadius-geometry.bodySphere.radius)<0.01f&&
       actor.transportOrdinaryReady==ordinaryProfileReady&&
       (!ordinaryProfileReady||
        (std::abs(actor.transportRunStep-ordinaryProfile.runStep)<0.0001f&&
         std::abs(
             actor.transportGravityField-
             ordinaryProfile.gravityField)<0.0001f))&&
       battle_ai::Length(actor.transportLocalCenter-geometry.bodySphere.localCenter)<0.01f&&
       !diagnosticPending)return;
    const auto previous=actor.cachedTransports;
    actor.cachedTransports.clear();actor.transportBlockers.clear();actor.transportRevision=g_transportCatalog.Revision();actor.transportCheckedAt=now;
    actor.transportDefinition=definition;actor.transportAlternate=alternate;actor.transportRadius=geometry.bodySphere.radius;
    actor.transportOrdinaryReady=ordinaryProfileReady;
    actor.transportRunStep=ordinaryProfile.runStep;
    actor.transportGravityField=ordinaryProfile.gravityField;
    actor.transportLocalCenter=geometry.bodySphere.localCenter;
    actor.transportGroundRejected=actor.transportCorridorRejected=0;
    auto corridor=geometry;corridor.deferDestroyables=false;
    uint32_t diagnosticUnit=0;
    detail::Read(unit,4,diagnosticUnit);
    const bool logCandidates=g_runtime.logDecisions&&g_transportCatalog.Complete()&&g_api&&g_api->Log&&
        actor.transportDiagnosticRevision!=g_transportCatalog.Revision();
    std::vector<Vec3> authoredSupports;
    for(const auto& anchor:g_transportCatalog.Volumes()) {
        if(!anchor.Live())continue;
        Vec3 support{};
        if(!corridor.Ground(
               anchor.position,anchor.bounds.max.y+80,
               anchor.bounds.min.y-256,support))
            continue;
        const auto duplicate=std::find_if(
            authoredSupports.begin(),authoredSupports.end(),
            [&](Vec3 known){return Length(known-support)<=1;});
        if(duplicate==authoredSupports.end())
            authoredSupports.push_back(support);
    }
    for(const auto& volume:g_transportCatalog.Volumes()) {
        if(!volume.Live()||actor.cachedTransports.size()>=64)continue;
        Vec3 entry{NAN,NAN,NAN},predicted{NAN,NAN,NAN};
        corridor.rejectedObject=nullptr;corridor.rejectedScene=nullptr;
        corridor.rejectedShape=0;corridor.lastReject=0;
        const bool entryGround=corridor.Ground(
            volume.position,volume.bounds.max.y+80,volume.bounds.min.y-256,entry);
        const bool forecast=entryGround&&PredictTransportExit(volume,entry,predicted);
        const bool forceCorridor=forecast&&
            corridor.ClearForcedTravel(entry,predicted,bounds);
        if(!entryGround||!forecast||!forceCorridor) {
            if(logCandidates) {
                const char* stage=!entryGround?"entry_ground":
                    !forecast?"forecast":"force_corridor";
                g_api->Log(g_mod,SDGO_LOG_INFO,
                    "map transport candidate unit=%u volume=%u object=%p stage=%s reason=%u pos=%.1f,%.1f,%.1f direction=%.5f,%.5f,%.5f speed=%.1f updates=%.1f entry=%.1f,%.1f,%.1f forecast=%.1f,%.1f,%.1f body_radius=%.1f",
                    diagnosticUnit,volume.id,volume.object,stage,corridor.lastReject,
                    volume.position.x,volume.position.y,volume.position.z,
                    volume.direction.x,volume.direction.y,volume.direction.z,
                    volume.speed,volume.updates,
                    entry.x,entry.y,entry.z,predicted.x,predicted.y,predicted.z,
                    geometry.bodySphere.radius);
            }
            if(!entryGround||!forecast)++actor.transportGroundRejected;
            else ++actor.transportCorridorRejected;
            if(corridor.rejectedObject&&corridor.rejectedScene&&
               map_transport_detail::IsKind(corridor.rejectedObject,1,kDestroyableBlockKind))
                actor.transportBlockers.push_back({corridor.rejectedObject,corridor.rejectedScene,
                    TransportBlockSignature(corridor.rejectedObject,corridor.rejectedScene)});
            continue;
        }
        std::vector<Vec3> supports;
        Vec3 direct{};
        if(corridor.Ground(
               predicted,predicted.y+80,predicted.y-4096,direct))
            supports.push_back(direct);
        for(const auto support:authoredSupports)
            if(std::none_of(
                   supports.begin(),supports.end(),
                   [&](Vec3 known){return Length(known-support)<=1;}))
                supports.push_back(support);
        uint32_t supportOrdinal=0;
        for(const auto exit:supports) {
            if(actor.cachedTransports.size()>=64)break;
            ++supportOrdinal;
            if(!TransportGroundExitUseful(
                   volume,entry,predicted,exit,
                   geometry.bodySphere.radius))
                continue;
            corridor.rejectedObject=nullptr;
            corridor.rejectedScene=nullptr;
            corridor.rejectedShape=0;
            corridor.lastReject=0;
            const double directHorizontal=std::hypot(
                double(exit.x-predicted.x),
                double(exit.z-predicted.z));
            bool landingCorridor=false;
            uint32_t ordinaryUpdates=0;
            if(directHorizontal<=
               (std::max)(25.0,double(geometry.bodySphere.radius))) {
                landingCorridor=corridor.ClearBody(predicted,exit);
            } else if(ordinaryProfileReady) {
                auto arrival=PredictTransportOrdinaryAirArrival(
                    TransportForceTerminalAirState(volume,predicted),
                    exit,ordinaryProfile.runStep,
                    ordinaryProfile.gravityField);
                if(arrival.reachable) {
                    arrival=PredictTransportOrdinaryAirArrival(
                        TransportForceTerminalAirState(
                            volume,predicted),
                        exit,ordinaryProfile.runStep,
                        ordinaryProfile.gravityField,true);
                    ordinaryUpdates=arrival.updates;
                    landingCorridor=TransportOrdinaryPathClear(
                        corridor,arrival,exit);
                }
            }
            if(!landingCorridor) {
                ++actor.transportCorridorRejected;
                if(corridor.rejectedObject&&corridor.rejectedScene&&
                   map_transport_detail::IsKind(
                       corridor.rejectedObject,1,
                       kDestroyableBlockKind))
                    actor.transportBlockers.push_back({
                        corridor.rejectedObject,corridor.rejectedScene,
                        TransportBlockSignature(
                            corridor.rejectedObject,
                            corridor.rejectedScene)});
                continue;
            }
            const uint32_t edgeId=
                supportOrdinal==1?volume.id:
                0x10000u+volume.id*64u+supportOrdinal;
            const double cost=double(volume.updates)*10.0+50.0+
                double(ordinaryUpdates)*ordinaryProfile.runStep;
            actor.cachedTransports.push_back({
                {edgeId,entry,exit,cost},volume,predicted});
            if(logCandidates)
                g_api->Log(g_mod,SDGO_LOG_INFO,
                    "map transport candidate unit=%u volume=%u link=%u object=%p stage=usable reason=0 ordinary_updates=%u pos=%.1f,%.1f,%.1f direction=%.5f,%.5f,%.5f speed=%.1f updates=%.1f entry=%.1f,%.1f,%.1f forecast=%.1f,%.1f,%.1f ground_exit=%.1f,%.1f,%.1f body_radius=%.1f",
                    diagnosticUnit,volume.id,edgeId,volume.object,
                    ordinaryUpdates,
                    volume.position.x,volume.position.y,volume.position.z,
                    volume.direction.x,volume.direction.y,volume.direction.z,
                    volume.speed,volume.updates,
                    entry.x,entry.y,entry.z,
                    predicted.x,predicted.y,predicted.z,
                    exit.x,exit.y,exit.z,
                    geometry.bodySphere.radius);
        }
    }
    if(logCandidates)actor.transportDiagnosticRevision=g_transportCatalog.Revision();
    bool changed=previous.size()!=actor.cachedTransports.size();
    if(!changed)for(size_t i=0;i<previous.size();++i)
        if(previous[i].edge.id!=actor.cachedTransports[i].edge.id||
           previous[i].volume.object!=actor.cachedTransports[i].volume.object||
           std::abs(
               previous[i].edge.cost-
               actor.cachedTransports[i].edge.cost)>0.01||
           battle_ai::Length(previous[i].edge.entry-actor.cachedTransports[i].edge.entry)>1||
           battle_ai::Length(previous[i].edge.exit-actor.cachedTransports[i].edge.exit)>1||
           battle_ai::Length(
               previous[i].forcedExit-
               actor.cachedTransports[i].forcedExit)>1) {
            changed=true;break;
        }
    if(changed)++actor.transportGraphRevision;
}
void BeginGeometrySearch(uint8_t* unit,Actor& actor,battle_ai::native::MapGeometry& geometry,Vec3 goal) {
    if(TransportRouteProtected(actor.transport,GetTickCount64()))return;
    RefreshTransportCandidates(unit,actor,geometry);
    if(!actor.transport.native&&actor.transport.phase!=5) {
        actor.transport.active=false;
        actor.transport.request={};
    }
    actor.searchTransports=actor.cachedTransports;
    actor.searchVolumes.clear();
    for(const auto& volume:g_transportCatalog.Volumes())if(volume.Live())actor.searchVolumes.push_back(volume);
    actor.transportSearchRevision=actor.transportGraphRevision;
    std::vector<battle_ai::MapPathSearch::Transport> edges;
    const auto now=GetTickCount64();
    for(const auto& candidate:actor.searchTransports)
        if(candidate.volume.object!=actor.transport.avoidObject||now>=actor.transport.avoidUntil)edges.push_back(candidate.edge);
    actor.mapSearch.Begin(Position(unit),goal,100,8192,edges);
}
struct TransportWalkingContext {battle_ai::native::MapGeometry* geometry;const Actor* actor;};
bool TransportWalkingStep(void* opaque,Vec3 from,Vec3 desired,Vec3& grounded,bool entering) {
    auto& context=*static_cast<TransportWalkingContext*>(opaque);
    auto& geometry=*context.geometry;
    if(!MapGeometryStep(&geometry,from,desired,grounded))return false;
    const Vec3 offset=geometry.bodySphere.center-geometry.ownerPosition;
    const float radius=geometry.bodySphere.radius;const Vec3 extent{radius,radius,radius};
    for(const auto& volume:context.actor->searchVolumes) {
        const battle_ai::native::Aabb bounds{volume.bounds.min-offset-extent,volume.bounds.max-offset+extent};
        const bool inside=from.x>=bounds.min.x&&from.x<=bounds.max.x&&from.y>=bounds.min.y&&from.y<=bounds.max.y&&
            from.z>=bounds.min.z&&from.z<=bounds.max.z;
        if(inside)continue; // retain a legal escape from a native landing/trigger edge
        bool selectedEntrance=false;
        if(entering)for(const auto& candidate:context.actor->searchTransports)
            if(candidate.volume.object==volume.object&&battle_ai::Length(candidate.edge.entry-grounded)<1)selectedEntrance=true;
        if(selectedEntrance)continue;
        const Vec3 center=(from+grounded)*0.5f,half=(grounded-from)*0.5f;
        if(geometry.api.segmentAabb(&center,&half,&bounds))return false;
    }
    return true;
}
bool MapWalkingStep(void* context,Vec3 from,Vec3 desired,Vec3& grounded) {
    return TransportWalkingStep(context,from,desired,grounded,false);
}
bool MapEntranceStep(void* context,Vec3 from,Vec3 desired,Vec3& grounded) {
    return TransportWalkingStep(context,from,desired,grounded,true);
}
bool NativeTransportMoving(uint8_t* unit) {
    return NativeTransportMoveForced(unit)||
        NativeTransportJumpForced(unit);
}
bool SelectedTransportOverlap(
    uint8_t* unit,const Actor::TransportRoute& trip) {
    using namespace battle_ai::native;
    if(!unit||!g_mapSphereObb||!trip.active||
       !trip.volume.scene||!trip.volume.Live())return false;
    __try {
        uint8_t* ownerScene=nullptr;uint8_t* sphere=nullptr;uint8_t* pad=nullptr;
        return detail::ReadPointer(unit,0x34,ownerScene)&&
            detail::ReadPointer(ownerScene,0xE0,sphere)&&
            detail::ReadPointer(trip.volume.scene,0xD8,pad)&&
            g_mapSphereObb(
                reinterpret_cast<const MapBodySphere*>(sphere),pad);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
battle_ai::native::TransportRequestStatus NativeTransportRequestStatus(
    uint8_t* unit,
    const battle_ai::native::TransportVolume* selectedVolume=nullptr) {
    using battle_ai::native::TransportRequestStatus;
    TransportRequestStatus result{};
    static SdgoPvpAiClientGetForcedPadStatusV2Fn pvpSnapshotV2=nullptr;
    static SdgoMissionAiClientGetForcedPadStatusV2Fn missionSnapshotV2=nullptr;
    static SdgoPvpAiClientGetForcedPadStatusV1Fn pvpSnapshot=nullptr;
    static SdgoMissionAiClientGetForcedPadStatusV1Fn missionSnapshot=nullptr;
    if(!pvpSnapshotV2||!missionSnapshotV2||
       !pvpSnapshot||!missionSnapshot) {
        const auto module=GetModuleHandleW(L"match_ai.dll");
        if(module) {
            if(!pvpSnapshotV2)pvpSnapshotV2=
                reinterpret_cast<SdgoPvpAiClientGetForcedPadStatusV2Fn>(
                    GetProcAddress(
                        module,
                        SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_V2_EXPORT));
            if(!missionSnapshotV2)missionSnapshotV2=
                reinterpret_cast<SdgoMissionAiClientGetForcedPadStatusV2Fn>(
                    GetProcAddress(
                        module,
                        SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_V2_EXPORT));
            if(!pvpSnapshot)pvpSnapshot=reinterpret_cast<SdgoPvpAiClientGetForcedPadStatusV1Fn>(
                GetProcAddress(module,SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_EXPORT));
            if(!missionSnapshot)missionSnapshot=reinterpret_cast<SdgoMissionAiClientGetForcedPadStatusV1Fn>(
                GetProcAddress(module,SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_EXPORT));
        }
    }
    uint32_t object=0,pilot=0;
    if(!battle_ai::native::detail::Read(unit,4,object)||
       !battle_ai::native::detail::Read(unit,0x2B68,pilot))return result;
    const auto copyV1=[&](const auto& bot) {
        result.available=true;
        result.generation=bot.generation;
        result.requestCount=bot.request_count;
        result.noticeCount=bot.notice_count;
        result.timeoutCount=bot.timeout_count;
        result.sendFailureCount=bot.send_failure_count;
        result.ownershipLossCount=bot.ownership_loss_count;
        result.outstanding=bot.outstanding!=0;
        result.outstandingGeneration=bot.outstanding_generation;
    };
    const auto copyV2=[&](const auto& bot,uint32_t snapshotTick) {
        result.available=true;
        result.exactWitness=true;
        result.generation=bot.generation;
        result.requestCount=bot.request_count;
        result.noticeCount=bot.notice_count;
        result.outstanding=bot.outstanding!=0;
        result.outstandingGeneration=bot.outstanding_generation;
        result.snapshotTick=snapshotTick;
        result.requestTick=bot.request_tick;
        result.payloadValid=bot.payload_valid!=0;
        result.padObjectValid=bot.pad_object_valid!=0;
        result.padObjectId=bot.pad_object_id;
        result.requestPilotId=bot.request_pilot_id;
        result.requestSpeed=bot.speed;
        result.requestDurationUpdates=bot.duration_updates;
        result.requestDirection={
            bot.direction_x,bot.direction_y,bot.direction_z};
        result.exactFailure=
            bot.last_clear_reason==3||
            bot.last_clear_reason==4||
            bot.last_clear_reason==6;
        uint32_t selectedObject=0;
        if(selectedVolume)
            battle_ai::native::detail::Read(
                selectedVolume->object,4,selectedObject);
        result.selectedPadObjectId=selectedObject;
        result.exactPayload=selectedVolume&&
            selectedObject&&
            battle_ai::native::TransportPayloadMatches(
                *selectedVolume,selectedObject,
                result.payloadValid,
                result.padObjectValid,result.padObjectId,
                result.requestPilotId,pilot,
                result.requestSpeed,result.requestDurationUpdates,
                result.requestDirection);
    };
    if(PvpPlayerBotRulesMode()) {
        if(pvpSnapshotV2) {
            SdgoPvpAiClientForcedPadStatusV2 status{};
            status.size=sizeof(status);
            status.version=
                SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION;
            if(!pvpSnapshotV2(&status)||!status.bridge_available)
                return result;
            for(uint32_t i=0;i<status.bot_count&&i<12;++i) {
                const auto& bot=status.bots[i];
                if(bot.object_id==object&&bot.pilot_id==pilot) {
                    copyV2(bot,status.tick_count);return result;
                }
            }
            return result;
        }
        if(!pvpSnapshot)return result;
        SdgoPvpAiClientForcedPadStatusV1 status{};status.size=sizeof(status);status.version=1;
        if(pvpSnapshot(&status)&&status.bridge_available)
            for(uint32_t i=0;i<status.bot_count&&i<12;++i) {
                const auto& bot=status.bots[i];
                if(bot.object_id==object&&bot.pilot_id==pilot) {
                    copyV1(bot);return result;
                }
            }
        return result;
    }
    if(missionSnapshotV2) {
        SdgoMissionAiClientForcedPadStatusV2 status{};
        status.size=sizeof(status);
        status.version=
            SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_V2_VERSION;
        const auto mode=CurrentRulesMode();
        if(!missionSnapshotV2(&status)||!status.bridge_available||
           !mode||status.mode_object!=static_cast<uint32_t>(
               reinterpret_cast<uintptr_t>(mode)))
            return result;
        for(uint32_t i=0;i<status.bot_count&&i<12;++i) {
            const auto& bot=status.bots[i];
            if(bot.object_id==object&&bot.pilot_id==pilot) {
                copyV2(bot,status.tick_count);return result;
            }
        }
        return result;
    }
    if(missionSnapshot) {
        SdgoMissionAiClientForcedPadStatusV1 status{};status.size=sizeof(status);status.version=1;
        const auto mode=CurrentRulesMode();
        if(missionSnapshot(&status)&&status.bridge_available&&mode&&
           status.mode_object==static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mode)))
            for(uint32_t i=0;i<status.bot_count&&i<12;++i) {
                const auto& bot=status.bots[i];
                if(bot.object_id==object&&bot.pilot_id==pilot) {
                    copyV1(bot);return result;
                }
            }
    }
    return result;
}
void RejectTransportRoute(Actor& actor,ULONGLONG now) {
    auto& trip=actor.transport;
    trip.active=false;trip.native=false;trip.phase=4;++trip.rejected;++trip.routeRejected;
    trip.landingGoalIssued=false;trip.landingPredictedUpdates=0;
    trip.landingObservedUpdates=0;trip.landingObservedThinkSerial=0;
    trip.avoidObject=trip.volume.object;trip.avoidUntil=now+5000;
    trip.resumed=trip.completed;
    trip.request={};
    actor.path.clear();actor.geometryOwned=false;actor.geometryFinal=false;actor.hasIssuedGoal=false;
}
auto ReconcileTransportRequestStatus(
    Actor& actor,const battle_ai::native::TransportRequestStatus& status,
    bool selectedOverlap,ULONGLONG now,uint8_t* unit=nullptr) {
    using namespace battle_ai::native;
    auto& trip=actor.transport;
    if(!trip.active||trip.native||trip.phase!=1)
        return TransportRequestUpdate::Idle;
    const auto update=ObserveTransportRequestLease(
        trip.request,status,selectedOverlap,now);
    const uint32_t traceFlags=
        (status.available?1u:0u)|
        (status.exactWitness?2u:0u)|
        (status.payloadValid?4u:0u)|
        (status.padObjectValid?8u:0u)|
        (status.exactPayload?16u:0u)|
        (status.exactFailure?32u:0u)|
        (status.outstanding?64u:0u)|
        (trip.request.committed?128u:0u)|
        (trip.request.acknowledged?256u:0u)|
        (static_cast<uint32_t>(update)<<16);
    const bool traceChanged=!trip.leaseTraceValid||
        trip.leaseTraceOverlap!=selectedOverlap||
        trip.leaseTraceRequest!=status.requestCount||
        trip.leaseTraceNotice!=status.noticeCount||
        trip.leaseTraceRequestTick!=status.requestTick||
        trip.leaseTraceSnapshotTick!=status.snapshotTick||
        trip.leaseTracePadObject!=status.padObjectId||
        trip.leaseTraceSelectedObject!=status.selectedPadObjectId||
        trip.leaseTraceFlags!=traceFlags;
    if(g_runtime.logDecisions&&traceChanged&&g_api&&g_api->Log) {
        uint32_t unitObject=0,object=0;
        if(unit)battle_ai::native::detail::Read(unit,4,unitObject);
        if(actor.transport.volume.object)
            battle_ai::native::detail::Read(
                actor.transport.volume.object,4,object);
        const uint32_t tickAge=status.exactWitness?
            uint32_t(status.snapshotTick-status.requestTick):0;
        g_api->Log(g_mod,SDGO_LOG_INFO,
            "map transport lease unit=%u link=%u phase=%u overlap=%u update=%u generation=%u/%u request=%u/%u notice=%u/%u outstanding=%u payload_valid=%u pad_valid=%u pad=%u selected_pad=%u live_pad=%u exact_payload=%u exact_failure=%u snapshot_tick=%u request_tick=%u tick_age=%u committed=%u acknowledged=%u speed=%.3f updates=%.3f direction=%.6f,%.6f,%.6f",
            unitObject,trip.id,trip.phase,selectedOverlap?1u:0u,
            static_cast<uint32_t>(update),
            status.generation,status.outstandingGeneration,
            status.requestCount,trip.request.requestBaseline,
            status.noticeCount,trip.request.noticeBaseline,
            status.outstanding?1u:0u,status.payloadValid?1u:0u,
            status.padObjectValid?1u:0u,status.padObjectId,
            status.selectedPadObjectId,object,
            status.exactPayload?1u:0u,status.exactFailure?1u:0u,
            status.snapshotTick,status.requestTick,tickAge,
            trip.request.committed?1u:0u,
            trip.request.acknowledged?1u:0u,
            status.requestSpeed,status.requestDurationUpdates,
            status.requestDirection.x,status.requestDirection.y,
            status.requestDirection.z);
    }
    trip.leaseTraceValid=true;
    trip.leaseTraceOverlap=selectedOverlap;
    trip.leaseTraceRequest=status.requestCount;
    trip.leaseTraceNotice=status.noticeCount;
    trip.leaseTraceRequestTick=status.requestTick;
    trip.leaseTraceSnapshotTick=status.snapshotTick;
    trip.leaseTracePadObject=status.padObjectId;
    trip.leaseTraceSelectedObject=status.selectedPadObjectId;
    trip.leaseTraceFlags=traceFlags;
    if(update==TransportRequestUpdate::Failed||
       update==TransportRequestUpdate::GenerationChanged)
        RejectTransportRoute(actor,now);
    return update;
}
auto RefreshTransportRequest(uint8_t* unit,Actor& actor) {
    return ReconcileTransportRequestStatus(
        actor,NativeTransportRequestStatus(
            unit,&actor.transport.volume),
        SelectedTransportOverlap(unit,actor.transport),GetTickCount64(),unit);
}
void CompleteTransportRoute(Actor& actor,ULONGLONG now) {
    auto& trip=actor.transport;
    trip.active=false;trip.native=false;trip.phase=3;trip.completedAt=now;++trip.routeAccepted;
    trip.landingGoalIssued=false;trip.landingPredictedUpdates=0;
    trip.landingObservedUpdates=0;trip.landingObservedThinkSerial=0;
    trip.request={};
    actor.path.clear();actor.geometryOwned=false;actor.geometryFinal=false;
}
bool ContinueTransportLanding(uint8_t* unit,Actor& actor) {
    using namespace battle_ai::native;
    auto& trip=actor.transport;
    if(!trip.active||trip.native||trip.phase!=5)return false;
    const auto now=GetTickCount64();const Vec3 actual=Position(unit);trip.arrived=actual;
    auto geometry=MapGeometryFor(unit);
    if(*reinterpret_cast<int*>(unit+0x38)<=0||!geometry.Valid()) {
        RejectTransportRoute(actor,now);return true;
    }
    if(TransportAtRouteExit(actual,trip.exit,geometry.bodySphere.radius)) {
        CompleteTransportRoute(actor,now);return true;
    }
    if(NativeTransportMoveForced(unit)||
       NativeTransportJumpForced(unit))
        return true;
    if(trip.landingGoalIssued&&
       trip.landingObservedThinkSerial!=actor.nativeThinkSerial) {
        trip.landingObservedThinkSerial=actor.nativeThinkSerial;
        if(trip.landingObservedUpdates<UINT32_MAX)
            ++trip.landingObservedUpdates;
        if(TransportLandingObservationExpired(
               trip.landingObservedUpdates,
               trip.landingPredictedUpdates)) {
            RejectTransportRoute(actor,now);return true;
        }
    }
    if(ControlBlocked(unit)||!MovementAvailable(unit,true)) {
        RejectTransportRoute(actor,now);return true;
    }
    TransportOrdinaryMotionProfile profile{};
    if(ReadTransportOrdinaryProfile(unit,profile)!=
       TransportOrdinaryProfileState::Ready) {
        RejectTransportRoute(actor,now);return true;
    }
    auto arrival=PredictTransportOrdinaryAirArrival(
        profile.state,trip.exit,profile.runStep,
        profile.gravityField);
    if(!arrival.reachable) {
        RejectTransportRoute(actor,now);return true;
    }
    arrival=PredictTransportOrdinaryAirArrival(
        profile.state,trip.exit,profile.runStep,
        profile.gravityField,true);
    auto corridor=geometry;corridor.deferDestroyables=false;
    if(!TransportOrdinaryPathClear(
           corridor,arrival,trip.exit)) {
        RejectTransportRoute(actor,now);return true;
    }
    const bool landingGoalCurrent=
        TransportLandingGoalCurrent(unit,trip);
    if(!EnsureTransportLandingGoal(unit,trip)) {
        RejectTransportRoute(actor,now);return true;
    }
    if(!landingGoalCurrent&&g_setMoveType&&
       PlayerBotRulesMode(unit))
        g_setMoveType(unit,2);
    if(!trip.landingGoalIssued) {
        trip.landingGoalIssued=true;
        trip.landingPredictedUpdates=arrival.updates;
        trip.landingObservedUpdates=0;
        trip.landingObservedThinkSerial=actor.nativeThinkSerial;
    }
    return true;
}
void ReconcileTransportRoute(uint8_t* unit,Actor& actor) {
    using battle_ai::native::TransportRequestUpdate;
    auto& trip=actor.transport;
    if(trip.active&&!trip.native&&trip.phase==5) {
        ContinueTransportLanding(unit,actor);return;
    }
    if(trip.phase!=5&&trip.completed>trip.resumed&&!NativeTransportMoving(unit)&&!ControlBlocked(unit)) {
        Vec3 progress=Position(unit)-trip.arrived;progress.y=0;
        uint8_t* attack=nullptr;
        const bool action=battle_ai::native::detail::ReadPointer(unit,0x9CC,attack)&&actor.lastPolicyAt>=trip.completedAt&&
            (battle_ai::native::detail::InState(attack,0x34)||battle_ai::native::detail::InState(attack,0x48)||
             battle_ai::native::detail::InState(attack,0x5C));
        if(battle_ai::Length(progress)>25&&(actor.direct.owned||(actor.hasIssuedGoal&&!actor.path.empty())||action))trip.resumed=trip.completed;
    }
    if(!trip.active)return;
    if(!trip.native&&trip.phase==1) {
        const auto request=RefreshTransportRequest(unit,actor);
        if(request==TransportRequestUpdate::Failed||
           request==TransportRequestUpdate::GenerationChanged)return;
    }
    const bool moveForced=NativeTransportMoveForced(unit);
    const bool jumpForced=NativeTransportJumpForced(unit);
    if(moveForced) {
        if(!trip.native) {
            uint8_t* movement=nullptr;Vec3 direction{};
            const auto geometry=MapGeometryFor(unit);const Vec3 actual=Position(unit);
            const double reach=geometry.bodySphere.radius+battle_ai::Length(trip.volume.bounds.max-trip.volume.bounds.min)*0.5+trip.volume.speed*2;
            if(!trip.request.committed||
               !battle_ai::native::detail::ReadPointer(unit,0x9C8,movement)||
               !battle_ai::native::detail::Read(movement,0x1FC,direction)||
               battle_ai::Length(direction-trip.volume.direction)>0.01||battle_ai::Length(actual-trip.entry)>reach) {
                RejectTransportRoute(actor,GetTickCount64());return;
            }
            trip.native=true;trip.phase=2;trip.activated=actual;++trip.activations;
            trip.waitingAt=0;trip.landingAt=0;actor.direct={};actor.hasIssuedGoal=false;
        }
        actor.path.clear();actor.geometryOwned=false;actor.geometryFinal=false;return;
    }
    if(jumpForced) {
        if(trip.native) {
            actor.path.clear();actor.geometryOwned=false;
            actor.geometryFinal=false;
        }
        return;
    }
    if(!trip.native)return;
    const Vec3 actual=Position(unit);trip.arrived=actual;
    const auto now=GetTickCount64();
    auto geometry=MapGeometryFor(unit);
    const bool moved=!ControlBlocked(unit)&&*reinterpret_cast<int*>(unit+0x38)>0&&
        battle_ai::native::TransportMadeProgress(
            trip.activated,actual,trip.volume.direction);
    if(moved)++trip.completed;
    if(!moved||!geometry.Valid()||
       !battle_ai::native::TransportReachedRouteProgress(
            trip.activated,actual,trip.forcedExit,
            trip.volume,geometry.bodySphere.radius)) {
        RejectTransportRoute(actor,now);return;
    }
    trip.native=false;actor.path.clear();actor.geometryOwned=false;actor.geometryFinal=false;actor.hasIssuedGoal=false;
    if(battle_ai::native::TransportAtRouteExit(actual,trip.exit,geometry.bodySphere.radius))
        CompleteTransportRoute(actor,now);
    else {
        trip.active=true;trip.phase=5;trip.landingAt=now;
        trip.landingGoalIssued=false;
        trip.landingPredictedUpdates=0;
        trip.landingObservedUpdates=0;
        trip.landingObservedThinkSerial=0;
    }
}
bool InstallGeometryPath(uint8_t* unit,Actor& actor,battle_ai::native::MapGeometry& geometry,bool complete) {
    const auto steps=complete?actor.mapSearch.Steps():actor.mapSearch.PrefixSteps();
    std::vector<Vec3> path;
    bool usesTransport=false;
    for(const auto& point:steps) {
        if(!point.transport) {path.push_back(point.position);continue;}
        const auto selected=std::find_if(actor.searchTransports.begin(),actor.searchTransports.end(),
            [&](const auto& candidate){return candidate.edge.id==point.transport;});
        if(selected==actor.searchTransports.end()||!selected->volume.Live()) {
            actor.transportRevision=UINT64_MAX;actor.path.clear();return false;
        }
        usesTransport=true;
        auto& trip=actor.transport;
        const bool changedSelection=
            trip.id!=point.transport||
            trip.volume.object!=selected->volume.object||
            battle_ai::Length(trip.entry-selected->edge.entry)>1||
            battle_ai::Length(
                trip.forcedExit-selected->forcedExit)>1||
            battle_ai::Length(trip.exit-selected->edge.exit)>1;
        if(!trip.active||
           (!TransportRouteProtected(
                trip,GetTickCount64())&&changedSelection)) {
            trip.active=true;trip.native=false;trip.phase=1;trip.id=point.transport;trip.volume=selected->volume;
            trip.entry=selected->edge.entry;
            trip.forcedExit=selected->forcedExit;
            trip.exit=selected->edge.exit;
            trip.waitingAt=0;trip.landingAt=0;
            trip.landingGoalIssued=false;
            trip.landingPredictedUpdates=0;
            trip.landingObservedUpdates=0;
            trip.landingObservedThinkSerial=0;
            trip.leaseTraceValid=false;
            battle_ai::native::BeginTransportRequestLease(
                trip.request,NativeTransportRequestStatus(
                    unit,&selected->volume));
            ++trip.selected;
        }
        if(path.empty()||battle_ai::Length(path.back()-trip.entry)>1)path.push_back(trip.entry);
        break; // The exit is a native action, never a MoveTargetPos destination.
    }
    if(!usesTransport&&!TransportRouteProtected(
           actor.transport,GetTickCount64())) {
        actor.transport.active=false;actor.transport.phase=0;
        actor.transport.request={};
    }
    if(path.empty())return false;
    const Vec3 here=Position(unit);size_t nearest=0;double best=1e30;
    TransportWalkingContext walking{&geometry,&actor};
    for(size_t i=0;i<path.size();++i) {
        const double distance=battle_ai::Length(path[i]-here);Vec3 reached{};
        const bool entrance=actor.transport.active&&battle_ai::Length(path[i]-actor.transport.entry)<1;
        const auto step=entrance?&MapEntranceStep:&MapWalkingStep;
        if(distance<best&&distance<=800&&step(&walking,here,path[i],reached)&&std::abs(reached.y-path[i].y)<=5) {nearest=i;best=distance;}
    }
    if(best==1e30) {
        if(!actor.path.empty()&&MovementAvailable(unit,actor.pathRanged))g_stop(unit);
        const Vec3 goal=actor.mapSearch.Goal();BeginGeometrySearch(unit,actor,geometry,goal);
        actor.geometryFinal=false;actor.path.clear();return false;
    }
    while(nearest+1<path.size()&&battle_ai::Length(path[nearest]-here)<25)++nearest;
    path.erase(path.begin(),path.begin()+nearest);
    actor.path.swap(path);actor.waypoint=0;actor.pathGeometry=true;actor.geometryFinal=complete;
    RecordRouteInstall(actor);
    return true;
}
void PumpGeometryPath(uint8_t* unit,Actor& actor) {
    if(!actor.geometryOwned)return;
    battle_ai::native::detail::ReadableRegionCache readableRegions{};
    battle_ai::native::detail::ScopedReadableRegionCache readableScope(readableRegions);
    auto geometry=MapGeometryFor(unit);if(!geometry.Valid())return;
    RefreshTransportCandidates(unit,actor,geometry);
    if(actor.transportSearchRevision!=actor.transportGraphRevision&&!actor.transport.native) {
        const auto goal=actor.mapSearch.Goal();BeginGeometrySearch(unit,actor,geometry,goal);
        actor.geometryFinal=false;
    }
    LARGE_INTEGER before{},after{};QueryPerformanceCounter(&before);
    using Status=battle_ai::MapPathSearch::Status;
    TransportWalkingContext walking{&geometry,&actor};
    if(actor.mapSearch.State()==Status::Pending)actor.mapSearch.Pump(&MapWalkingStep,&walking,1,&MapEntranceStep);
    if(actor.mapSearch.State()==Status::Found&&!actor.geometryFinal)InstallGeometryPath(unit,actor,geometry,true);
    else if(actor.path.empty()&&actor.mapSearch.State()==Status::Pending)InstallGeometryPath(unit,actor,geometry,false);
    else if(actor.path.empty()&&actor.mapSearch.State()==Status::Unreachable)actor.geometryOwned=false;
    actor.geometryProjections+=geometry.projections;actor.geometryGroundMisses+=geometry.groundMisses;
    actor.geometryBlocked+=geometry.blocked;actor.geometryReject=geometry.lastReject;
    QueryPerformanceCounter(&after);actor.navigationCost.Record(before.QuadPart,after.QuadPart);
}
bool FollowPath(uint8_t* unit,Actor& a) {
    a.followStatus=1;
    ReconcileTransportRoute(unit,a);
    if(a.transport.active&&a.transport.native)return false;
    if(a.transport.active&&a.transport.phase==5) {
        ContinueTransportLanding(unit,a);return a.transport.active||a.transport.phase==3;
    }
    if(a.transport.active&&!a.transport.native&&a.transport.phase==1&&
       TransportMotorReserved(a.transport,GetTickCount64())) {
        const auto now=GetTickCount64();
        const bool pendingAdmission=
            battle_ai::native::TransportRequestAdmissionOpen(
                a.transport.request,now);
        const auto since=a.transport.request.committed?
            a.transport.request.committedAt:a.transport.waitingAt;
        const auto deadline=battle_ai::native::TransportEntranceWaitMs(
            a.transport.request.committed);
        if(!a.transport.volume.Live()||
           (a.transport.request.committed?
                (!since||now<since||now-since>=deadline):
                (!pendingAdmission&&
                 (!since||now<since||now-since>=deadline)))) {
            RejectTransportRoute(a,now);return false;
        }
        return true;
    }
    if(a.recoveryControlled&&GetTickCount64()-a.recoveryAt<250)return false;
    a.followStatus=2;
    if(!MovementAvailable(unit,a.pathRanged)) {
        // Native attacks/hurt/guard own the motor now; drop our route without
        // cancelling the movement belonging to their active state.
        a.path.clear();a.geometryOwned=false;a.geometryFinal=false;return false;
    }
    if(a.geometryOwned)PumpGeometryPath(unit,a);
    AdvancePath(unit,a);
    a.followStatus=3;
    if(a.path.empty()) {
        auto& trip=a.transport;
        if(!trip.active)return true;
        const auto now=GetTickCount64();trip.waitingAt=trip.waitingAt?trip.waitingAt:now;
        const auto request=RefreshTransportRequest(unit,a);
        if(request==battle_ai::native::TransportRequestUpdate::Failed||
           request==battle_ai::native::TransportRequestUpdate::GenerationChanged)
            return false;
        const auto since=trip.request.committed?
            trip.request.committedAt:trip.waitingAt;
        const auto deadline=battle_ai::native::TransportEntranceWaitMs(
            trip.request.committed);
        const bool pendingAdmission=
            battle_ai::native::TransportRequestAdmissionOpen(
                trip.request,now);
        if(!trip.volume.Live()||
           (trip.request.committed?
                (!since||now<since||now-since>=deadline):
                (!pendingAdmission&&
                 (!since||now<since||now-since>=deadline)))) {
            RejectTransportRoute(a,now);return false;
        }
        // Keep the already admitted goal at the real trigger. The existing
        // match_ai overlap bridge owns REQ085A/NTF0584; do not send or apply it here.
        return true;
    }
    a.followStatus=4;
    if(MotionSafetyFor(unit,a.path[a.waypoint])==battle_ai::native::MotionSafety::Blocked) {
        a.path.clear();
        a.geometryOwned=false;a.geometryFinal=false;
        if(!ControlBlocked(unit)) g_stop(unit);
        return false;
    }
    a.followStatus=5;
    // Native 3A keeps its steering Y at the segment's starting height. It is
    // not an echo of the requested endpoint, especially across floors.
    const bool newGoal=unit[0x30DC]==0||!a.hasIssuedGoal||battle_ai::Length(a.issuedGoal-a.path[a.waypoint])>1;
    if(newGoal) {
        if(!g_setGoal(unit,&a.path[a.waypoint],1)) {
            a.path.clear();return false;
        }
        a.issuedGoal=a.path[a.waypoint];a.hasIssuedGoal=true;
    }
    // The goal's final parameter remains the native default. Locomotion has
    // its own native SetMoveType transaction; do not confuse the two APIs.
    const bool descending=a.path[a.waypoint].y<Position(unit).y-25;
    const bool playerBotRules=PlayerBotRulesMode(unit);
    const int mode=!a.defenceMoveOwned&&!a.recoveryControlled&&!descending?a.pathMoveType:2;
    if(playerBotRules&&g_setMoveType&&(newGoal||unit[0xDC0]!=mode)) {
        g_setMoveType(unit,mode);
    }
    if(a.transport.completed>a.transport.resumed&&!a.transport.active&&a.hasIssuedGoal) {
        Vec3 progress=Position(unit)-a.transport.arrived;progress.y=0;
        if(battle_ai::Length(progress)>25)a.transport.resumed=a.transport.completed;
    }
    a.followStatus=6;return true;
}
struct TargetObservation {
    uint32_t id=0;
    int hp=0,maxHp=0;
    bool alive=false;
    float distance=0,rangedDistance=0,meleeDistance=0;
    bool visible=false,perceptionVisible=false,rangedVisible=false,meleeVisible=false,enemy=false,attackable=false,rangedFiring=false,meleeTracking=false,meleeSwinging=false,hurt=false,damageProtected=false;
    uint32_t selectedTarget=0;
    uint32_t teamSource=0;
    bool teamVisible=false,mapObstacle=false;
    uint8_t* object=nullptr;
    uint8_t hitType=0;bool hitKnown=false;
    bool meleeApproaching=false;
    Vec3 point,ground,velocity,forward;
};
struct AllyObservation {uint32_t id=0,target=0;Vec3 pos,viewOrigin;uint8_t* object=nullptr;bool meleeTracking=false,meleeSwinging=false;RangedActivity ranged;Contact contact;};
struct Observation {
    const char* volatile failure="native observation";
    battle_ai::native::Capabilities caps={};
    uint32_t id=0;
    uint64_t generation=0;
    int hp=0;
    Vec3 pos,goal;
    bool moving=false,goalKnown=false,hurt=false,hurtRecently=false,rangedBusy=false,meleeTracking=false,meleeSwinging=false,comboKnown=false,chainWindow=false,meleePressure=false,forcedMoving=false;
    int comboIndex=0,comboMax=0;
    AllyObservation allies[64];size_t allyCount=0;
    TargetObservation targets[128];
    size_t count=0;
    TargetObservation obstacle{};
    bool hasObstacle=false;
};
bool Destroyable(uint8_t* object) {
    if(!object)return false;
    auto table=*reinterpret_cast<void***>(object);
    if(!table||!table[1])return false;
    using IsA=bool(__thiscall*)(void*,int);
    return reinterpret_cast<IsA>(table[1])(object,battle_ai::native::kDestroyableBlockKind);
}
void ObserveBlockingGate(Observation& o,Actor& actor,ULONGLONG now,Vec3 perceptionOrigin,Vec3 meleeOrigin,bool rangedOrigin) {
    using namespace battle_ai::native::detail;
    if(!o.count||o.hurt||!g_navigation.query||!g_navigation.segmentAabb)return;
    auto fsm=*reinterpret_cast<uint8_t**>(g_current+0x9CC);
    auto currentTarget=fsm?FindObject(*reinterpret_cast<uint32_t*>(fsm+0x200)):nullptr;
    const bool retainingGate=currentTarget&&Destroyable(currentTarget)&&*reinterpret_cast<int32_t*>(currentTarget+0x38)>0;
    const float dx=o.pos.x-actor.previousPerceptionPos.x,dz=o.pos.z-actor.previousPerceptionPos.z;
    const bool wanted=o.moving||!actor.path.empty()||actor.defenceMoveOwned||retainingGate;
    if(!retainingGate&&(!actor.hasPerceptionPos||!wanted||dx*dx+dz*dz>0.01f)) {actor.stationarySince=now;return;}
    if(!actor.stationarySince)actor.stationarySince=now;
    if(!retainingGate&&now-actor.stationarySince<150)return;
    Vec3 goal=retainingGate?Position(currentTarget):actor.path.empty()?o.goal:actor.path[actor.waypoint];
    Vec3 delta=goal-o.pos;delta.y=0;
    const double length=battle_ai::Length(delta);
    if(!std::isfinite(length)||(!retainingGate&&length<1))return;
    if(length>600)delta=delta*static_cast<float>(600/length);
    uint8_t* ownScene=nullptr;uint8_t* quadtree=nullptr;
    if(!ReadPointer(g_current,0x34,ownScene)||!ReadPointer(*g_document,kDocumentQuadTree,quadtree))return;
    battle_ai::native::Aabb start{};
    if(!Read(ownScene,kSceneAabb,start)||!ValidAabb(start))return;
    const battle_ai::native::Aabb finish={start.min+delta,start.max+delta};
    const battle_ai::native::Aabb query={
        {(std::min)(start.min.x,finish.min.x),(std::min)(start.min.y,finish.min.y),(std::min)(start.min.z,finish.min.z)},
        {(std::max)(start.max.x,finish.max.x),(std::max)(start.max.y,finish.max.y),(std::max)(start.max.z,finish.max.z)}};
    void* scenes[kSceneCapacity]{};int count=0;
    g_navigation.query(quadtree,scenes,&count,kSceneCapacity,&query);
    if(count<0||count>=kSceneCapacity)return;
    const Vec3 half=(start.max-start.min)*0.5f;
    const Vec3 center=(start.min+start.max)*0.5f+delta*0.5f;
    const Vec3 halfDelta=delta*0.5f;double closest=1e30;
    for(int i=0;i<count;++i) {
        auto scene=static_cast<uint8_t*>(scenes[i]);uint8_t* object=nullptr;
        if(!scene||!ReadPointer(scene,kSceneBackPointer,object)||!Destroyable(object))continue;
        uint32_t id=0;int32_t hp=0,maxHp=0;uint8_t removed=1;
        if(!Read(object,4,id)||!id||!Read(object,0x38,hp)||hp<=0||!Read(object,0x3C,maxHp)||
           !Read(object,0x1C,removed)||removed)continue;
        auto table=*reinterpret_cast<void***>(object);
        using CanAttack=bool(__thiscall*)(void*,void*);
        if(!table||!table[10]||!reinterpret_cast<CanAttack>(table[10])(object,g_current))continue;
        battle_ai::native::Aabb bounds{};
        if(!Read(scene,kSceneAabb,bounds)||!ValidAabb(bounds))continue;
        const battle_ai::native::Aabb expanded={bounds.min-half,bounds.max+half};
        if(!g_navigation.segmentAabb(&center,&halfDelta,&expanded))continue;
        const Vec3 point=AimPoint(object);
        const double distance=battle_ai::Length(point-o.pos);
        if(!battle_ai::Finite(point)||!std::isfinite(distance)||distance>=closest)continue;
        if(!battle_ai::native::FreshLineClear(g_navigation,*g_document,g_current,id,perceptionOrigin,point,true))continue;
        auto& target=o.obstacle;target={};
        target.id=id;target.object=object;target.hp=hp;target.maxHp=maxHp;target.alive=true;
        target.enemy=true;target.attackable=true;target.visible=true;target.perceptionVisible=true;
        target.mapObstacle=true;target.point=point;target.ground=Position(object);
        target.distance=static_cast<float>(battle_ai::Length(point-perceptionOrigin));target.rangedDistance=target.distance;
        const Vec3 origin=meleeOrigin;
        const Vec3 nearest={(std::max)(bounds.min.x,(std::min)(origin.x,bounds.max.x)),
                            (std::max)(bounds.min.y,(std::min)(origin.y,bounds.max.y)),
                            (std::max)(bounds.min.z,(std::min)(origin.z,bounds.max.z))};
        target.meleeDistance=static_cast<float>(battle_ai::Length(nearest-origin));
        target.rangedVisible=rangedOrigin;
        for(const auto& weapon:o.caps.weapons) {
            if(weapon.valid&&weapon.kind==battle_ai::native::WeaponKind::Melee&&target.meleeDistance<=weapon.range)
                target.meleeVisible=battle_ai::native::FreshLineClear(g_navigation,*g_document,g_current,id,origin,point);
        }
        o.hasObstacle=true;closest=distance;
    }
}
void PlanDefence(const Observation& o,Actor& actor,ULONGLONG now) {
    const auto previousMovement=actor.defence.movement;
    actor.defence={};
    if(!g_runtime.enabled||g_runtime.difficulty<1) {actor.defenceEpisode=false;return;}
    if(actor.defenceEpisode&&now-actor.defenceEpisodeAt>1000) actor.defenceEpisode=false;
    auto& plan=actor.defence;plan.at=now;
    plan.meleeBusy=o.caps.meleeBusy;plan.specialBusy=o.caps.specialBusy;plan.hurt=o.hurt;
    Vec3 forward=*reinterpret_cast<Vec3*>(*reinterpret_cast<uint8_t**>(g_current+8)+0x6C);
    forward=battle_ai::Horizontal(forward);
    if(!battle_ai::Finite(forward)||battle_ai::Length(forward)<0.5) return;
    float effectiveRange=0;
    for(const auto& w:o.caps.weapons)
        if(w.valid&&w.slot>=0&&w.slot<3&&std::isfinite(w.range)&&w.range>effectiveRange) effectiveRange=w.range;
    double nearest=1e30;auto add=[&](uint32_t id,Vec3 point,float weight,bool melee,uint8_t hitType,bool hitKnown) {
        if(!battle_ai::Finite(point)) return;
        const double distance=battle_ai::Length(point-o.pos);
        if(!std::isfinite(distance)||distance<1) return;
        if(plan.count<32) plan.threats[plan.count++]={point,weight};
        if(!plan.routeTarget) plan.routeTarget=id;
        if(melee&&distance<nearest) {nearest=distance;plan.shieldThreat=point;plan.shielding=true;plan.shieldHitType=hitType;plan.shieldHitKnown=hitKnown;}
    };
    for(size_t i=0;i<o.count;++i) {
        const auto& t=o.targets[i];if(!t.visible||(!t.attackable&&!t.damageProtected)||!t.alive) continue;
        const double groundDistance=battle_ai::Length(t.ground-o.pos);
        if(!std::isfinite(groundDistance)) continue;
        bool engaged=!t.damageProtected&&t.selectedTarget==o.id&&(t.rangedFiring||t.meleeTracking||t.meleeSwinging);
        bool melee=engaged&&(t.meleeTracking||t.meleeSwinging)&&groundDistance<600;
        // A distant idle opponent remains observable to targeting, but does not
        // force this bot to turn side-on or abandon its current engagement.
        if(!engaged&&groundDistance>effectiveRange) continue;
        add(t.id,t.ground,melee?1.8f:(engaged?1.2f:1.0f),melee,t.hitType,t.hitKnown);
    }
    auto received=g_incoming.find(g_current);
    if(received!=g_incoming.end()) for(const auto& incoming:received->second) if(now-incoming.time<=1000) {
        bool protectedAttacker=false;
        for(size_t i=0;i<o.count;++i) if(o.targets[i].id==incoming.attacker) protectedAttacker=o.targets[i].damageProtected;
        add(incoming.attacker,incoming.point,protectedAttacker?1.0f:(incoming.melee?1.8f:1.2f),
            incoming.melee&&!protectedAttacker,incoming.hitType,incoming.hitKnown);
    }
    if(!plan.count) return;
    // Keep the front even before a path/strafe writes a new desired yaw. Waiting
    // for an enemy to become rearward lets locomotion expose the back first.
    plan.threatened=true;
    auto facing=battle_ai::DefensiveFacing(o.pos,forward,plan.threats,plan.count);
    plan.movement={o.pos,facing,false};
    if(facing.rearRisk>0.9||facing.rearExposure>0.00001) {
        if(!actor.defenceEpisode) {
            actor.defenceEpisode=true;actor.defenceAnchor=o.pos;
        }
        actor.defenceEpisodeAt=now;
        if(now>=actor.defenceRepathAt||(actor.defenceMoveOwned&&actor.path.empty()))
            plan.movement=battle_ai::DefensivePosition(o.pos,forward,plan.threats,plan.count);
        else if(actor.defenceMoveOwned&&!actor.path.empty()) {
            plan.movement.destination=previousMovement.destination;plan.movement.relocate=true;
        }
        if(plan.movement.relocate) {
            const float radius=g_runtime.difficulty>=2?1200.0f:480.0f;
            Vec3 currentOffset=o.pos-actor.defenceAnchor;currentOffset.y=0;
            Vec3 destinationOffset=plan.movement.destination-actor.defenceAnchor;destinationOffset.y=0;
            double currentDistance=battle_ai::Length(currentOffset);
            double destinationDistance=battle_ai::Length(destinationOffset);
            if(!std::isfinite(currentDistance)||!std::isfinite(destinationDistance)||currentDistance>=radius) {
                plan.movement.destination=o.pos;plan.movement.relocate=false;
            } else if(destinationDistance>radius) {
                float scale=radius/static_cast<float>(destinationDistance);
                plan.movement.destination.x=actor.defenceAnchor.x+destinationOffset.x*scale;
                plan.movement.destination.z=actor.defenceAnchor.z+destinationOffset.z*scale;
            }
            Vec3 remaining=plan.movement.destination-o.pos;remaining.y=0;
            if(plan.movement.relocate&&battle_ai::Length(remaining)<25) {
                plan.movement.destination=o.pos;plan.movement.relocate=false;
            }
        }
    }
    // The relocation's destination may have a different optimal angle. While
    // travelling, protect the actor at its observed position on this frame.
    plan.movement.facing=facing;
    if(g_runtime.difficulty<2) plan.shielding=false;
}
bool CaptureUnsafe(Observation& o) {
    o.failure="ownership and native world context";
    if(!Owned(g_current)||!g_document||!*g_document||GetCurrentThreadId()!=g_gameThread) return false;
    if(!battle_ai::native::ReadCapabilities(g_current,o.caps)) {o.failure="weapon capabilities";return false;}
    bool hasWeapon=false;
    for(const auto& w:o.caps.weapons) if(w.valid) hasWeapon=true;
    if(!hasWeapon) {o.failure="ordinary weapon availability";return false;}
    uint8_t* unit=g_current;
    o.failure="loadout role metadata";
    auto& actor=ActorFor(unit);
    uint8_t* breachFsm=nullptr;
    const bool activeRanged=battle_ai::native::detail::ReadPointer(unit,0x9CC,breachFsm)&&
        battle_ai::native::detail::InState(breachFsm,0x34);
    bool breachAvailable=false;
    for(const auto& weapon:o.caps.weapons)
        if(weapon.valid&&weapon.ordinary&&weapon.range>0&&
           (weapon.ammoReady||(activeRanged&&weapon.slot==o.caps.selectedSlot)))breachAvailable=true;
    if(actor.breachAvailable&&!breachAvailable&&actor.geometryOwned) {
        // A route through a live destroyable is no longer executable while
        // every ordinary weapon is empty. Replan with that door still solid.
        actor.path.clear();actor.geometryOwned=false;actor.geometryFinal=false;actor.hasIssuedGoal=false;
    }
    actor.breachAvailable=breachAvailable;
    if(actor.roleUnit!=o.caps.unitId||actor.roleAlternate!=o.caps.alternate) {
        actor.roleUnit=o.caps.unitId;actor.roleAlternate=o.caps.alternate;
        int nativeRole=sdgo::AiCatalogMasteredWeaponType(g_api,g_mod,o.caps.unitId);
        battle_ai::loadout::NativeProfile fallback;
        actor.meleeRole=nativeRole==0||(nativeRole<0&&ClientLoadoutProfile(unit,nullptr,&fallback)&&
            fallback.role==battle_ai::loadout::UnitRole::Melee);
    }
    o.failure="own motion and controller data";
    AdvancePath(unit,actor);
    if(actor.transport.active&&!actor.transport.native&&actor.transport.phase==1)
        RefreshTransportRequest(unit,actor);
    ULONGLONG now=GetTickCount64();
    o.id=*reinterpret_cast<uint32_t*>(unit+4);o.generation=actor.generation;
    o.hp=*reinterpret_cast<int*>(unit+0x38);o.pos=Position(unit);
    Velocity(unit,o.id,o.pos,now,o.generation);
    o.goal=*reinterpret_cast<Vec3*>(unit+0x3078);
    o.moving=unit[0x30DC]!=0;
    // 3078 is inactive native steering storage when 30DC is clear. Some
    // native initializations leave NaN there; absence of a command must not
    // prevent sensing and issuing the first valid enhanced route.
    o.goalKnown=o.moving&&battle_ai::Finite(o.goal);
    // CheckHurt consumes a latch. Observe actual/queued MovementFSM control
    // states instead; do not steal the original AI's damage notification.
    o.hurt=ControlBlocked(unit);
    o.forcedMoving=battle_ai::native::detail::InState(*reinterpret_cast<uint8_t**>(unit+0x9C8),0x124);
    if(actor.lastHp>0 && o.hp<actor.lastHp) actor.hurtAt=now;
    actor.lastHp=o.hp;o.hurtRecently=actor.hurtAt && now-actor.hurtAt<1600;
    float meleeRange=0;
    for(const auto& w:o.caps.weapons) if(w.valid&&w.ordinary&&w.slot<3) {
        if(w.kind==battle_ai::native::WeaponKind::Melee) meleeRange=(std::max)(meleeRange,w.range);
    }
    int ownTeam=*reinterpret_cast<int*>(unit+0x20);
    using CanAttack=bool(__thiscall*)(uint8_t*,uint8_t*);
    Ray3 ray={};
    o.failure="own action state";
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    if(!fsm) return false;
    o.rangedBusy=battle_ai::native::detail::InState(fsm,0x34);
    o.meleeTracking=battle_ai::native::detail::InState(fsm,0x48);o.meleeSwinging=battle_ai::native::detail::InState(fsm,0x5C);
    o.comboIndex=*reinterpret_cast<int*>(fsm+0x968);o.comboMax=*reinterpret_cast<int*>(fsm+0x96C);
    auto selectedRecord=*reinterpret_cast<uint8_t**>(fsm+0x248);
    o.comboKnown=selectedRecord&&o.comboMax>=1&&o.comboMax<=32&&o.comboIndex>=0&&o.comboIndex<o.comboMax&&selectedRecord[0x264]==o.comboMax;
    o.chainWindow=fsm[0x218]!=0;
    Vec3 meleeOrigin=o.pos;
    auto selfNode=*reinterpret_cast<uint8_t**>(unit+8);
    uint8_t* selfSphere=nullptr;Vec3 nativeMeleeOrigin={};
    if(battle_ai::native::detail::ReadPointer(selfNode,0xE0,selfSphere)&&
       battle_ai::native::detail::Read(selfSphere,0x10,nativeMeleeOrigin)&&
       battle_ai::Finite(nativeMeleeOrigin))
        meleeOrigin=nativeMeleeOrigin;
    bool rangedOrigin=false;
    for(const auto& weapon:o.caps.weapons)
        if(weapon.valid&&weapon.ordinary&&weapon.slot==o.caps.selectedSlot&&
           weapon.kind!=battle_ai::native::WeaponKind::Melee) rangedOrigin=true;
    // The native ranged-origin getter iterates muzzle-node names and does not
    // initialize its output when the selected melee weapon has no muzzle.
    // Perception uses the body's native collision point in that posture;
    // actual ranged visibility is available only for the bound gun's own ray.
    ray.origin=meleeOrigin;
    o.failure="bound weapon muzzle origin";
    if(rangedOrigin) {
        Ray3* nativeRay=g_ray(fsm,&ray);
        if(!nativeRay || !battle_ai::Finite(nativeRay->origin)) {o.failure="native ranged origin";return false;}
        if(nativeRay!=&ray) ray=*nativeRay;
    }
    for(size_t offset:{size_t(0x48),size_t(0x70),size_t(0x7C)}) {
        o.failure="native target lists";
        auto head=*reinterpret_cast<uint8_t***>(*g_document+offset);
        if(!head) continue;
        auto node=*reinterpret_cast<uint8_t***>(head);
        unsigned traversed=0;
        while(node!=head && node && traversed++<4096 && o.count<128) {
            auto candidate=reinterpret_cast<uint8_t*>(node[2]);
            node=*reinterpret_cast<uint8_t***>(node);
            if(!candidate||candidate==unit) continue;
            uint32_t id=*reinterpret_cast<uint32_t*>(candidate+4);
            bool duplicate=false;
            for(size_t i=0;i<o.count;++i) if(o.targets[i].id==id) duplicate=true;
            for(size_t i=0;i<o.allyCount;++i) if(o.allies[i].id==id) duplicate=true;
            if(!id||duplicate) continue;
            // Document lists can retain another view of an object ID across
            // native lifetime transitions. Observe the same registered object
            // that aim, admission and tracking resolve, rather than letting an
            // older list entry hide its current HP or protection state.
            candidate=FindObject(id);
            if(!candidate||*reinterpret_cast<uint32_t*>(candidate+4)!=id||candidate==unit) continue;
            int team=*reinterpret_cast<int*>(candidate+0x20);
            int hp=*reinterpret_cast<int*>(candidate+0x38);
            if(!CombatTargetAlive(candidate,hp)) continue;
            auto candidateFsm=*reinterpret_cast<uint8_t**>(candidate+0x9CC);
            if(team==ownTeam && ownTeam!=0 && ownTeam!=5) {
                if(o.allyCount<64) {
                    auto& buddy=o.allies[o.allyCount++];buddy.id=id;buddy.pos=Position(candidate);
                    buddy.object=candidate;buddy.viewOrigin=AimPoint(candidate);
                    buddy.target=candidateFsm?*reinterpret_cast<uint32_t*>(candidateFsm+0x200):0;
                    buddy.meleeTracking=battle_ai::native::detail::InState(candidateFsm,0x48);
                    buddy.meleeSwinging=battle_ai::native::detail::InState(candidateFsm,0x5C);
                    auto shot=g_rangedShots.find(candidate);if(shot!=g_rangedShots.end()) buddy.ranged=shot->second;
                    auto contact=g_contacts.find(candidate);if(contact!=g_contacts.end()) buddy.contact=contact->second;
                }
                continue;
            }
            Vec3 point=AimPoint(candidate);
            float rangedDistance=static_cast<float>(battle_ai::Length(point-ray.origin));
            float meleeDistance=static_cast<float>(battle_ai::Length(point-meleeOrigin));
            // Weapon reach constrains firing, not noticing a distant visible
            // combatant to pursue. Fresh collision queries below still own
            // visibility; Lua retains only the last visible point behind cover.
            if(!battle_ai::Finite(point)||!std::isfinite(rangedDistance)||
               !std::isfinite(meleeDistance)) continue;
            auto vtable=*reinterpret_cast<void***>(candidate);
            if(!vtable||!vtable[10]) continue;
            o.failure="target damage protection and attack permission";
            const auto protection=battle_ai::native::ReadDamageProtection(candidate);
            const bool damageProtected=!protection.known||protection.active;
            const bool attackable=reinterpret_cast<CanAttack>(vtable[10])(candidate,unit);
            auto& t=o.targets[o.count++];
            t.object=candidate;
            t.id=id;t.hp=hp;t.maxHp=*reinterpret_cast<int*>(candidate+0x3C);t.alive=true;
            t.distance=rangedDistance;t.rangedDistance=rangedDistance;t.meleeDistance=meleeDistance;
            t.enemy=true;t.attackable=attackable;t.damageProtected=damageProtected;t.point=point;t.ground=Position(candidate);
            o.failure="target action and pose";
            t.hurt=HurtState(candidate);
            t.forward=*reinterpret_cast<Vec3*>(*reinterpret_cast<uint8_t**>(candidate+8)+0x6C);
            t.selectedTarget=candidateFsm?*reinterpret_cast<uint32_t*>(candidateFsm+0x200):0;
            t.meleeTracking=battle_ai::native::detail::InState(candidateFsm,0x48);t.meleeSwinging=battle_ai::native::detail::InState(candidateFsm,0x5C);
            uint8_t* attackRecord=nullptr;
            t.hitKnown=battle_ai::native::detail::ReadPointer(candidateFsm,0x248,attackRecord)&&
                battle_ai::native::detail::Read(attackRecord,0x6B,t.hitType)&&t.hitType<=3;
            if((t.meleeTracking||t.meleeSwinging)&&t.selectedTarget==o.id) {
                battle_ai::native::Capabilities attacking={};
                if(battle_ai::native::ReadCapabilities(candidate,attacking))
                    for(const auto& w:attacking.weapons) if(w.valid&&w.slot==attacking.selectedSlot&&w.kind==battle_ai::native::WeaponKind::Melee&&meleeDistance<=w.range+150) o.meleePressure=true;
            }
            t.velocity=Velocity(candidate,id,point,now);
            if(attackRecord&&(attackRecord[0x6A]==1||attackRecord[0x6A]==8))
                t.meleeApproaching=battle_ai::Dot(battle_ai::Horizontal(o.pos-t.ground),t.velocity)>30;
            o.failure="target line of sight";
            t.perceptionVisible=(attackable||damageProtected)&&
                battle_ai::native::FreshLineClear(g_navigation,*g_document,unit,id,ray.origin,point,true);
            t.rangedVisible=rangedOrigin&&t.perceptionVisible;
            t.meleeVisible=(attackable||damageProtected)&&meleeDistance<=meleeRange&&
                battle_ai::native::FreshLineClear(g_navigation,*g_document,unit,id,meleeOrigin,point);
            t.visible=t.perceptionVisible||t.meleeVisible;
            // A target's entire loadout/boost does not need decoding just to
            // notice gun input. The event hook below also retains brief shots.
            uint8_t* enemyFsm=nullptr;uint8_t* enemyRecord=nullptr;uint8_t rangeType=0;
            if(battle_ai::native::detail::ReadPointer(candidate,0x9CC,enemyFsm) &&
               battle_ai::native::detail::ReadPointer(enemyFsm,0x248,enemyRecord) &&
               battle_ai::native::detail::Read(enemyRecord,0x6A,rangeType) && rangeType>=2 && rangeType<=7)
                t.rangedFiring=candidate[0xDCD]!=0 || candidate[0xDCE]!=0;
            auto recent=g_rangedShots.find(candidate);
            if(recent!=g_rangedShots.end() && now-recent->second.time<1600) t.rangedFiring=true;
        }
        if(traversed>=4096) return false;
    }
    // Share only a living teammate's fresh native visibility. These contacts
    // authorize pursuit, while each bot retains its own weapon ray and native
    // attack permission for firing. Humans participate through the same live
    // world objects; no cached AI policy or hidden position is treated as sight.
    o.failure="shared teammate visibility";
    for(size_t i=0;i<o.count;++i) {
        auto& target=o.targets[i];
        if(target.visible||!target.object)continue;
        auto table=*reinterpret_cast<void***>(target.object);
        if(!table||!table[10])continue;
        for(size_t j=0;j<o.allyCount;++j) {
            const auto& ally=o.allies[j];
            if(!ally.object||!battle_ai::Finite(ally.viewOrigin))continue;
            if(!target.damageProtected&&!reinterpret_cast<CanAttack>(table[10])(target.object,ally.object))continue;
            if(battle_ai::native::FreshLineClear(g_navigation,*g_document,ally.object,target.id,
                                               ally.viewOrigin,target.point,true)) {
                target.teamVisible=true;target.teamSource=ally.id;break;
            }
        }
    }
    o.failure="blocking map obstacle";
    ObserveBlockingGate(o,actor,now,ray.origin,meleeOrigin,rangedOrigin);
    o.failure="native unit position finiteness";
    if(!battle_ai::Finite(o.pos)) return false;
    actor.observedTarget=*reinterpret_cast<uint32_t*>(fsm+0x200);
    actor.observedTargetAt=0;
    actor.observedTargetVisible=false;actor.observedTargetDistance=0;actor.observedFarTargets=0;actor.observedMaxRange=0;
    actor.sharedTarget=0;actor.sharedSource=0;
    const bool countPerception=!actor.hasPerceptionPos||actor.perceptionSerial!=actor.thinkSerial;
    const float supportDx=o.pos.x-actor.previousPerceptionPos.x,supportDz=o.pos.z-actor.previousPerceptionPos.z;
    const bool supportMoved=actor.hasPerceptionPos&&!o.hurt&&supportDx*supportDx+supportDz*supportDz>0.01f;
    for(const auto& weapon:o.caps.weapons)
        if(weapon.valid&&weapon.ordinary&&weapon.slot<3) actor.observedMaxRange=(std::max)(actor.observedMaxRange,weapon.range);
    for(size_t i=0;i<o.count;++i) {
        const auto& target=o.targets[i];
        if(target.id==actor.observedTarget) {
            actor.observedTargetVisible=target.visible;actor.observedTargetDistance=target.distance;actor.observedTargetPoint=target.point;
            actor.observedTargetAt=now;
            if(target.teamVisible&&!target.visible) {
                actor.sharedTarget=target.id;actor.sharedSource=target.teamSource;
                if(countPerception) {
                    ++actor.sharedContactFrames;
                    if(supportMoved)++actor.sharedPursuitFrames;
                }
            }
        }
        if(target.visible&&target.attackable&&!target.damageProtected&&target.distance>actor.observedMaxRange) ++actor.observedFarTargets;
    }
    if(o.hasObstacle&&o.obstacle.id==actor.observedTarget) {
        actor.observedTargetVisible=o.obstacle.visible;actor.observedTargetDistance=o.obstacle.distance;
        actor.observedTargetPoint=o.obstacle.point;actor.observedTargetAt=now;
    }
    if(countPerception) {
        actor.previousPerceptionPos=o.pos;actor.hasPerceptionPos=true;actor.perceptionSerial=actor.thinkSerial;
    }
    o.failure="defensive movement plan";
    PlanDefence(o,actor,now);return true;
}
bool GuardedCapture(Observation& o) {
    __try { return CaptureUnsafe(o); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Capture(Observation& o) {
    // Reuse page-readability queries only within this observation. Every field
    // is still read live and every copy keeps its SEH guard; no targets, sight
    // tests or decisions are skipped, delayed or shared with a later update.
    // Catch native faults inside the scope so its thread-local pointer is also
    // restored when an unguarded engine access fails under /EHsc.
    battle_ai::native::detail::ReadableRegionCache readableRegions{};
    battle_ai::native::detail::ScopedReadableRegionCache readableScope(readableRegions);
    LARGE_INTEGER before{},after{};QueryPerformanceCounter(&before);
    const bool result=GuardedCapture(o);
    QueryPerformanceCounter(&after);
    auto found=g_actors.find(g_current);
    if(found!=g_actors.end())found->second.senseCost.Record(before.QuadPart,after.QuadPart);
    return result;
}
void Number(lua_State* L,const char* key,double value) {
    g_lua.pushstring(L,key);g_lua.pushnumber(L,value);g_lua.settable(L,-3);
}
void String(lua_State* L,const char* key,const char* value) {
    g_lua.pushstring(L,key);g_lua.pushstring(L,value);g_lua.settable(L,-3);
}
void Vector(lua_State* L,const char* key,Vec3 v) {
    g_lua.pushstring(L,key);g_lua.newtable(L);
    Number(L,"x",v.x);Number(L,"y",v.y);Number(L,"z",v.z);g_lua.settable(L,-3);
}
void PushTarget(lua_State* L,const TargetObservation& t) {
    g_lua.newtable(L);
    Number(L,"id",t.id);Number(L,"hp",t.hp);Number(L,"alive",t.alive);Number(L,"maxHp",t.maxHp);Number(L,"distance",t.distance);
    Number(L,"rangedDistance",t.rangedDistance);Number(L,"meleeDistance",t.meleeDistance);
    Number(L,"enemy",t.enemy);Number(L,"visible",t.visible);Number(L,"attackable",t.attackable);
    Number(L,"rangedVisible",t.rangedVisible);Number(L,"meleeVisible",t.meleeVisible);
    Number(L,"perceptionVisible",t.perceptionVisible);Number(L,"teamVisible",t.teamVisible);Number(L,"teamSource",t.teamSource);
    Number(L,"mapObstacle",t.mapObstacle);Number(L,"damageProtected",t.damageProtected);
    Number(L,"rangedFiring",t.rangedFiring);Vector(L,"pos",t.point);Vector(L,"groundPos",t.ground);Vector(L,"velocity",t.velocity);
    Number(L,"hurt",t.hurt);Number(L,"meleeApproaching",t.meleeApproaching);
    Vector(L,"forward",t.forward);Number(L,"selectedTarget",t.selectedTarget);
    Number(L,"meleeTracking",t.meleeTracking);Number(L,"meleeSwinging",t.meleeSwinging);
}
int __cdecl Observe(lua_State* L) {
    Observation o;
    bool valid=Capture(o);
    g_lua.newtable(L);Number(L,"valid",valid?1:0);
    if(!valid) {
        g_lua.pushstring(L,"failure");g_lua.pushstring(L,o.failure);g_lua.settable(L,-3);
        return 1;
    }
    if(g_runtime.generic) ActorFor(g_current).controlled=true;
    Number(L,"id",o.id);Number(L,"generation",static_cast<double>(o.generation));
    Number(L,"hp",o.hp);Number(L,"unitId",o.caps.unitId);Number(L,"alternate",o.caps.alternate);Number(L,"selectedSlot",o.caps.selectedSlot);
    Number(L,"meleeRole",ActorFor(g_current).meleeRole);
    Number(L,"moving",o.moving);Number(L,"hurtRecently",o.hurtRecently);
    Number(L,"forcedMoving",o.forcedMoving);
    const auto& trip=ActorFor(g_current).transport;
    Number(L,"transportCommitted",trip.request.committed);
    Number(L,"transportLanding",trip.active&&!trip.native&&trip.phase==5);
    Number(L,"transportMotorReserved",
        TransportMotorReserved(trip,GetTickCount64()));
    Number(L,"routeActive",!ActorFor(g_current).path.empty()||trip.active);
    Number(L,"transportCompleted",static_cast<double>(trip.completed));
    Number(L,"transportWaiting",trip.active&&!trip.native&&trip.phase==1&&
        TransportMotorReserved(trip,GetTickCount64()));
    const auto& observedActor=ActorFor(g_current);
    const bool recovering=observedActor.recoveryControlled&&GetTickCount64()-observedActor.recoveryAt<250;
    Number(L,"movementRecovery",recovering);
    Number(L,"defenceMoving",!recovering&&((observedActor.defenceMoveOwned&&!observedActor.path.empty())||
        (observedActor.defence.movement.relocate&&!o.hurt&&!o.caps.meleeBusy&&!o.caps.specialBusy)));
    Number(L,"hurt",o.hurt);
    Number(L,"boostFraction",o.caps.boostFraction);
    // Lua SetJumpEnable writes +3291. This is permission for native automatic
    // jump requests, not a promise that every form can fly; the motor decides.
    Number(L,"canJump",g_current[0x3291]!=0);
    Number(L,"meleeBusy",o.caps.meleeBusy);Number(L,"specialBusy",o.caps.specialBusy);
    Number(L,"specialFinished",static_cast<double>(ActorFor(g_current).specialFinished));
    Number(L,"specialAcknowledged",ActorFor(g_current).specialAcknowledged);
    Number(L,"specialAckSequence",static_cast<double>(ActorFor(g_current).specialAcknowledgedSequence));
    Number(L,"rangedBusy",o.rangedBusy);
    auto attackFsm=*reinterpret_cast<uint8_t**>(g_current+0x9CC);
    Number(L,"attackNeutral",attackFsm&&battle_ai::native::detail::Neutral(attackFsm));
    Number(L,"moveCancelOpen",attackFsm&&attackFsm[0xB38]==1);
    Number(L,"nativeTarget",attackFsm?*reinterpret_cast<uint32_t*>(attackFsm+0x200):0);
    uint32_t emergencyAttacker=0;auto incoming=g_incoming.find(g_current);ULONGLONG latest=0;
    if(incoming!=g_incoming.end()) for(const auto& hit:incoming->second)
        if(hit.melee&&GetTickCount64()-hit.time<1000&&hit.time>=latest) {emergencyAttacker=hit.attacker;latest=hit.time;}
    Number(L,"meleeAttacker",emergencyAttacker);
    auto selectedRecord=attackFsm?*reinterpret_cast<uint8_t**>(attackFsm+0x248):nullptr;
    Number(L,"specialSelected",o.caps.selectedSlot==3&&selectedRecord&&selectedRecord[0x69]==2);
    Number(L,"specialRange",o.caps.weapons[3].range);
    uint8_t* specialRecord=nullptr;
    bool specialKnown=attackFsm&&battle_ai::native::detail::ReadPointer(attackFsm,(o.caps.alternate?0x56C:0x24C)+3*0xC8+0xC,specialRecord)&&
        battle_ai::native::detail::Readable(specialRecord,0x6A)&&specialRecord[0x69]==2;
    Number(L,"specialAvailable",PlayerBotRulesMode(g_current)&&specialKnown&&o.caps.weapons[3].id!=0&&o.caps.weapons[3].range>0);
    uint8_t* movement=nullptr;int restriction=1;
    bool controllerKnown=attackFsm&&battle_ai::native::detail::ReadPointer(attackFsm,0xB30,movement)&&
        battle_ai::native::detail::Read(movement,0x2C0,restriction);
    // NPC IsA(9) can take the player's unconditional non-player branch too.
    // Enforce the actual player resource/restriction fields before that call.
    Number(L,"spReady",FairSpecialResource(attackFsm,g_current)&&g_playerSkillGate(attackFsm));
    bool specialControl=controllerKnown;
    if(specialControl) for(size_t offset:{size_t(0xD4),size_t(0xFC),size_t(0x110),size_t(0x160),size_t(0x174),size_t(0x188)})
        if(battle_ai::native::detail::InState(movement,offset)) specialControl=false;
    Number(L,"specialControlAllowed",specialControl);
    auto shot=g_rangedShots.find(g_current);
    Number(L,"shots",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.shots));
    Number(L,"shotAttempts",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.attempts));
    Number(L,"shotNonzeroReturns",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.nonzeroReturns));
    Number(L,"mobilePrimaryDispatches",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.mobilePrimaryDispatches));
    Number(L,"mobileDefensiveDispatches",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.mobileDefensiveDispatches));
    Number(L,"backwardPrimaryDispatches",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.backwardPrimaryDispatches));
    Number(L,"shotAt",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.primaryAt));
    auto contact=g_contacts.find(g_current);
    Number(L,"contactTarget",contact==g_contacts.end()?0:contact->second.target);
    Number(L,"contactAt",contact==g_contacts.end()?0:static_cast<double>(contact->second.time));
    Number(L,"meleeTracking",o.meleeTracking);Number(L,"meleeSwinging",o.meleeSwinging);Number(L,"comboKnown",o.comboKnown);
    Number(L,"comboIndex",o.comboIndex);Number(L,"comboMax",o.comboMax);Number(L,"chainWindow",o.chainWindow);Number(L,"meleePressure",o.meleePressure);
    Number(L,"actionHandle",attackFsm?*reinterpret_cast<uint32_t*>(attackFsm+0x224):0);
    Number(L,"contactCombo",contact==g_contacts.end()?-1:contact->second.combo);
    Number(L,"contactAction",contact==g_contacts.end()?0:contact->second.action);
    Vector(L,"pos",o.pos);
    Number(L,"goalKnown",o.goalKnown);
    if(o.goalKnown)Vector(L,"goal",o.goal);
    if(g_runtime.logDecisions&&!battle_ai::Finite(o.goal)&&!ActorFor(g_current).unusedGoalLogged) {
        ActorFor(g_current).unusedGoalLogged=true;
        g_api->Log(g_mod,SDGO_LOG_INFO,"native steering storage has no finite active destination unit=%u moving=%u raw_goal=%g,%g,%g; observation remains valid",
            o.id,o.moving,o.goal.x,o.goal.y,o.goal.z);
    }
    g_lua.pushstring(L,"weapons");g_lua.newtable(L);
    int index=0;
    for(const auto& w:o.caps.weapons) {
        if(!w.valid) continue;
        g_lua.pushnumber(L,++index);g_lua.newtable(L);
        Number(L,"slot",w.slot);Number(L,"valid",1);Number(L,"range",w.range);Number(L,"damage",w.damage);
        Number(L,"cycleMs",w.cycleMs);Number(L,"ammoReady",w.ammoReady);Number(L,"ready",w.ready);
        float speed=PredictionSpeed(g_current,o.caps,w);
        Number(L,"movingShot",w.movingShot);Number(L,"projectileSpeed",speed);Number(L,"id",w.id);
        Number(L,"active",o.rangedBusy && w.slot==o.caps.selectedSlot);
        const char* kind=w.ordinary?"opaque":"other";
        switch(w.kind) {
        case battle_ai::native::WeaponKind::Melee:kind="melee";break;
        case battle_ai::native::WeaponKind::Hitscan:kind="hitscan";break;
        case battle_ai::native::WeaponKind::Projectile:kind="projectile";break;
        case battle_ai::native::WeaponKind::Missile:kind="missile";break;
        default:break;
        }
        String(L,"kind",kind);Number(L,"ordinary",w.ordinary);
        Number(L,"attackStyle",w.attackStyle);Number(L,"rangeType",w.rangeType);
        g_lua.settable(L,-3);
    }
    g_lua.settable(L,-3);
    g_lua.pushstring(L,"targets");g_lua.newtable(L);
    for(size_t i=0;i<o.count;++i) {
        g_lua.pushnumber(L,static_cast<double>(i+1));PushTarget(L,o.targets[i]);
        g_lua.settable(L,-3);
    }
    g_lua.settable(L,-3);
    if(o.hasObstacle) {g_lua.pushstring(L,"obstacle");PushTarget(L,o.obstacle);g_lua.settable(L,-3);}
    g_lua.pushstring(L,"allies");g_lua.newtable(L);
    for(size_t i=0;i<o.allyCount;++i) {
        const auto& buddy=o.allies[i];g_lua.pushnumber(L,static_cast<double>(i+1));g_lua.newtable(L);
        Number(L,"id",buddy.id);Vector(L,"pos",buddy.pos);Number(L,"selectedTarget",buddy.target);
        Number(L,"meleeTracking",buddy.meleeTracking);Number(L,"meleeSwinging",buddy.meleeSwinging);
        Number(L,"rangedTarget",buddy.ranged.primaryTarget);Number(L,"rangedAt",static_cast<double>(buddy.ranged.primaryAt));
        Number(L,"contactTarget",buddy.contact.target);Number(L,"contactAt",static_cast<double>(buddy.contact.time));
        g_lua.settable(L,-3);
    }
    g_lua.settable(L,-3);
    return 1;
}
int __cdecl NativeFallback(lua_State* L) {
    if(!g_current||!g_nativeCallback||g_nativeFallback) { g_lua.pushnumber(L,0);return 1; }
    auto found=g_actors.find(g_current);
    if(g_lua.tonumber(L,1)!=1 && found!=g_actors.end()) RestoreIntent(g_current,found->second);
    g_nativeFallback=true;
    int result=g_dispatch(g_nativeCallback,g_current);
    g_nativeFallback=false;
    g_lua.pushnumber(L,result);return 1;
}
int __cdecl AimTarget(lua_State* L) {
    if(Owned(g_current)) {
        auto& a=ActorFor(g_current);
        double id=g_lua.tonumber(L,1);
        a.aimTarget=std::isfinite(id)&&id>0&&id<=0xffffffff ? static_cast<uint32_t>(id):0;
        a.aimAt=GetTickCount64();
    }
    return 0;
}
void CancelOwnedRoute(uint8_t* unit,Actor& a,bool handoff) {
    const bool unsafeHandoff=handoff&&unit[0x30DC]&&a.hasIssuedGoal&&
        MotionSafetyFor(unit,*reinterpret_cast<Vec3*>(unit+0x3078))!=
            battle_ai::native::MotionSafety::Safe;
    if((!handoff||unsafeHandoff)&&!a.path.empty()&&
       MovementAvailable(unit,a.pathRanged))g_stop(unit);
    a.path.clear();a.defenceMoveOwned=false;a.explorationOwned=false;
    a.hasIssuedGoal=false;a.direct={};a.geometryOwned=false;
    if(!TransportRouteProtected(a.transport,GetTickCount64())&&
       !NativeTransportMoving(unit)) {
        a.transport.active=false;a.transport.phase=0;
        a.transport.request={};
    }
}
int __cdecl CancelRoute(lua_State* L) {
    if(Owned(g_current)) {
        auto& a=ActorFor(g_current);
        const bool handoff=g_lua.gettop(L)>=1&&g_lua.tonumber(L,1)==1;
        CancelOwnedRoute(g_current,a,handoff);
    }
    return 0;
}
int __cdecl Cadence(lua_State* L) {
    if(!Owned(g_current)) return 0;
    auto& a=ActorFor(g_current);
    if(!a.ownsIntent) {
        a.savedWait=*reinterpret_cast<int*>(g_current+0x31F4);
        a.ownsIntent=true;
    }
    if(!a.ownsAim) {
        a.savedYawError=*reinterpret_cast<float*>(g_current+0x3204);
        a.savedPitchError=*reinterpret_cast<float*>(g_current+0x3208);
        a.ownsAim=true;
    }
    double interval=g_lua.tonumber(L,1);
    a.intervalMs=g_runtime.difficulty==1?g_runtime.mediumAttackIntervalMs:
        (g_runtime.difficulty>=2?0:(std::isfinite(interval)?static_cast<int>((std::max)(0.0,(std::min)(10000.0,interval))):0));
    if(!a.intervalMs) a.nextPulse=0;
    return 0;
}
int __cdecl RecoveryMode(lua_State* L) {
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)) {
        auto& actor=ActorFor(g_current);
        const bool requested=g_lua.tonumber(L,1)==1;
        if(requested&&!actor.recoveryControlled) {
            actor.recoveryAt=GetTickCount64();
            actor.path.clear();actor.defenceMoveOwned=false;actor.geometryOwned=false;actor.geometryFinal=false;
        }
        actor.recoveryControlled=requested;
    }
    return 0;
}
int __cdecl MeleeGate(lua_State* L) {
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)) {
        auto& actor=ActorFor(g_current);
        actor.suppressMelee=g_lua.tonumber(L,1)==1;
        actor.cancelMeleeTracking=actor.suppressMelee&&g_lua.gettop(L)>=2&&g_lua.tonumber(L,2)==1;
    }
    return 0;
}
int __cdecl ProfileLease(lua_State* L) {
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)) {
        int slot=static_cast<int>(g_lua.tonumber(L,1));
        ActorFor(g_current).specialSlotLease=slot>=0&&slot<3?slot:-1;
    }
    return 0;
}
int __cdecl ProfilePreviousSlot(lua_State* L) {
    auto actor=g_actors.find(g_current);
    g_lua.pushnumber(L,actor==g_actors.end()?-1:actor->second.specialSlotLease);return 1;
}
int __cdecl BoundSlot(lua_State* L) {
    battle_ai::native::Capabilities caps={};
    const bool known=Owned(g_current)&&battle_ai::native::ReadCapabilities(g_current,caps);
    int mask=known?0:-1;
    if(known) for(unsigned i=0;i<3;++i) if(caps.weapons[i].valid) mask|=1<<i;
    g_lua.pushnumber(L,known?caps.selectedSlot:-1);g_lua.pushnumber(L,mask);
    g_lua.pushnumber(L,known&&caps.specialBusy?1:0);return 3;
}
bool ReleaseHeldIntentUnsafe(uint8_t* unit) {
    using namespace battle_ai::native::detail;
    uint8_t* fsm=nullptr;uint8_t* record=nullptr;uint8_t style=0;
    if(!ReadPointer(unit,0x9CC,fsm)) return false;
    const bool rangedBusy=InState(fsm,0x34);
    const bool specialBusy=InState(fsm,0x98)||InState(fsm,0xAC)||InState(fsm,0xC0)||
        InState(fsm,0xD4)||InState(fsm,0xE8)||InState(fsm,0xFC)||InState(fsm,0x1D8)||InState(fsm,0x1EC);
    const bool specialSelected=ReadPointer(fsm,0x248,record)&&Read(record,0x69,style)&&style==2;
    const bool ordinaryMeleeBusy=InState(fsm,0x48)||InState(fsm,0x5C)||InState(fsm,0x70)||
        InState(fsm,0x84)||InState(fsm,0xE8)||InState(fsm,0xFC);
    // A begin receipt can outlive the special state until its terminal packet
    // is drained. It must never authorize clearing the input of a later
    // ordinary melee chain, even while Lua still owns the old return lease.
    if(ordinaryMeleeBusy&&!specialBusy&&!specialSelected) return false;
    auto actor=g_actors.find(unit);
    const ULONGLONG now=GetTickCount64();
    const bool admitted=actor!=g_actors.end()&&actor->second.specialSlotLease>=0&&
        actor->second.specialAcknowledged&&actor->second.specialAcknowledgedAt<=now&&
        now-actor->second.specialAcknowledgedAt<=60000&&!Neutral(fsm);
    if(!rangedBusy&&!specialBusy&&!specialSelected&&!admitted) return false;
    // CNPC::SetAttackOnOff(false) sends message 0x41. Its native receiver
    // also queues AttackFSM Neutral, which exits an active ranged/special
    // state on the next FSM tick. Clear only the held input latches here so
    // the state that admitted the action remains its sole completion owner.
    // CNPC::AttackIntent uses the same direct 312A/DCD-DCE clear in native
    // non-neutral branches where input must be released without a transition.
    unit[0x312A]=0;
    *reinterpret_cast<uint16_t*>(unit+0xDCD)=0;
    return true;
}
int __cdecl ReleaseHeldIntent(lua_State* L) {
    bool released=false;
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)) {
        __try {released=ReleaseHeldIntentUnsafe(g_current);}
        __except(EXCEPTION_EXECUTE_HANDLER) {released=false;}
    }
    g_lua.pushnumber(L,released?1:0);return 1;
}
bool RequestNativeWeapon(uint8_t* unit,int slot) {
    __try {
        battle_ai::native::Capabilities caps={};
        if(!unit||slot<0||slot>3||*reinterpret_cast<int*>(unit+0x38)<=0||
           !battle_ai::native::ReadCapabilities(unit,caps)) return false;
        if(slot<3&&!caps.weapons[slot].valid) return false;
        uint8_t* fsm=nullptr;uint8_t* record=nullptr;uint32_t boundWeapon=0;uint8_t boundStyle=0;
        const bool bound=battle_ai::native::detail::ReadPointer(unit,0x9CC,fsm)&&
            battle_ai::native::detail::ReadPointer(fsm,0x248,record)&&
            battle_ai::native::detail::Read(record,0,boundWeapon)&&battle_ai::native::detail::Read(record,0x69,boundStyle);
        if(caps.selectedSlot==slot&&bound&&boundWeapon!=0&&boundWeapon==caps.weapons[slot].id&&
           (slot!=3||boundStyle==2)) return true;
        auto table=*reinterpret_cast<void***>(unit);
        if(!table||!table[7]) return false;
        // CNPC::ChangeWeapon suppresses a request when unit+DD0 already equals
        // the desired slot, even if AttackFSM+240 never committed that slot.
        // Submit the same native 0x48 message, without that stale hint shortcut.
        // The native receiver still owns permission, timing and weapon binding.
        alignas(4) uint8_t message[0x14]={};
        *reinterpret_cast<uint16_t*>(message)=0x48;message[0x10]=static_cast<uint8_t>(slot);
        using Receive=void(__thiscall*)(uint8_t*,const uint8_t*);
        reinterpret_cast<Receive>(table[7])(unit,message);
        // The NPC message changes only its input hint. On a free native
        // controller, submit the same non-forced weapon-selection transaction
        // used by the ordinary input consumer (AttackFSM virtual +94). It owns
        // ammo admission, model binding and notifications; no slot is written
        // here. Active attacks, capture and get-up retain their native owner.
        if(fsm&&battle_ai::native::detail::Neutral(fsm)&&!ControlBlocked(unit))
            g_selectWeapon(fsm,slot,0,1,0);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
int __cdecl RequestWeapon(lua_State* L) {
    double value=g_lua.tonumber(L,1);
    int slot=std::isfinite(value)&&value>=0&&value<=3?static_cast<int>(value):-1;
    bool submitted=value==slot&&GetCurrentThreadId()==g_gameThread&&Owned(g_current)&&
        RequestNativeWeapon(g_current,slot);
    g_lua.pushnumber(L,submitted?1:0);return 1;
}
bool QueryRoute(uint8_t* nav,uint32_t id,Vec3 from,Vec3 to,Vec3** points,uint32_t* count) {
    __try { *points=g_route(nav,id,from,to,nullptr,count);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CopyRoute(const Vec3* points,Vec3* out,size_t bytes) {
    __try { memcpy(out,points,bytes);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FreeRoute(Vec3* points) {
    __try { g_delete(points);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void FailedRoute() {
    if(GetCurrentThreadId()!=g_gameThread || !Owned(g_current)) return;
    auto& a=ActorFor(g_current);a.path.clear();a.geometryOwned=false;a.geometryFinal=false;
    if(MovementAvailable(g_current,a.pathRanged))g_stop(g_current);
}
bool EmptyNavigation(uint8_t* nav) {
    using namespace battle_ai::native::detail;
    uint8_t* begin=nullptr;uint8_t* end=nullptr;uint8_t* array=nullptr;uint32_t count=0;
    return nav&&Read(nav,0x560,begin)&&Read(nav,0x564,end)&&Read(nav,0x580,count)&&Read(nav,0x584,array)&&
        !begin&&!end&&!array&&!count;
}
bool LoadGeometryRoute(uint8_t* unit,Actor& actor,Vec3 destination,bool force=false) {
    using Status=battle_ai::MapPathSearch::Status;
    auto geometry=MapGeometryFor(unit);Vec3 goal{};
    // Authored spawns may be airborne; enemy roots may be on another floor.
    // Resolve the destination's own support, never the floor below this bot.
    if(!geometry.Ground(destination,destination.y+80,destination.y-4096,goal))return false;
    auto& trip=actor.transport;
    if(trip.active&&!trip.native&&
       TransportRouteProtected(trip,GetTickCount64()))
        return FollowPath(unit,actor)&&trip.active;
    if(trip.active&&!trip.native&&trip.waitingAt&&actor.path.empty()&&
       battle_ai::Length(goal-actor.mapSearch.Goal())<=100)return FollowPath(unit,actor)&&trip.active;
    if(trip.active&&!trip.native&&!force&&actor.geometryOwned&&
       (battle_ai::Length(goal-actor.mapSearch.Goal())<=750||
        battle_ai::Length(Position(unit)-goal)>battle_ai::Length(Position(unit)-trip.entry)+
            battle_ai::Length(trip.exit-goal)+100)) {
        // A moving opponent changes the final leg, not the chosen entrance.
        // Re-evaluate the final pursuit from the actual native landing instead
        // of discarding the approach on each 100-unit target update.
        return FollowPath(unit,actor)&&(!actor.path.empty()||trip.active);
    }
    Vec3 changed=goal-actor.mapSearch.Goal();const float verticalChange=std::abs(changed.y);changed.y=0;
    if(!actor.geometryOwned||battle_ai::Length(changed)>100||verticalChange>5||actor.mapSearch.State()==Status::Unreachable||
       (force&&actor.mapSearch.State()!=Status::Pending)) {
        actor.path.clear();BeginGeometrySearch(unit,actor,geometry,goal);
        actor.hasIssuedGoal=false;
        actor.geometryOwned=true;actor.geometryFinal=false;actor.pathGeometry=true;
    }
    actor.routeStatus=11;
    const bool followed=FollowPath(unit,actor);
    actor.routePointCount=static_cast<uint32_t>(actor.mapSearch.Nodes());
    return followed&&(!actor.path.empty()||actor.transport.active);
}
bool LoadRouteUnsafe(uint32_t target,const Vec3* lastSeen,bool force) {
    if(!Owned(g_current)||!g_document||!*g_document||GetCurrentThreadId()!=g_gameThread) return false;
    auto& a=ActorFor(g_current);
    SynchronizePhaseGeometry(g_current,a);
    a.routeStatus=1;a.routePointCount=0;
    a.explorationOwned=false;
    auto other=lastSeen?nullptr:FindObject(target);
    a.routeStatus=2;
    if(!lastSeen&&!CombatTargetAlive(other)) {a.path.clear();a.geometryOwned=false;return false;}
    auto nav=*reinterpret_cast<uint8_t**>(*g_document+0x8364);
    a.routeStatus=3;
    if(!nav) {a.path.clear();a.geometryOwned=false;return false;}
    uint32_t count=0;
    Vec3 destination=lastSeen?*lastSeen:Position(other);
    Vec3* points=nullptr;
    a.routeStart=Position(g_current);a.routeGoal=destination;a.routeStatus=4;
    if(PlayerBotRulesMode(g_current)&&EmptyNavigation(nav))return LoadGeometryRoute(g_current,a,destination,force);
    a.geometryOwned=false;a.pathGeometry=false;
    a.path.clear(); // a failed native repath must never resurrect its previous goal
    if(!QueryRoute(nav,*reinterpret_cast<uint32_t*>(g_current+4),a.routeStart,destination,&points,&count))return false;
    a.routeStatus=5;a.routePointCount=count;
    if(!points)return false;
    // Copy native-owned results and release with this EXE's allocator. Never
    // mix the game's CRT heap with this DLL's std::vector allocator.
    std::vector<Vec3> staged;
    bool valid=count>0&&count<=4096;
    a.routeStatus=6;
    if(valid) { try { staged.resize(count);a.routeStatus=7; } catch(...) { valid=false; } }
    if(valid) {valid=CopyRoute(points,staged.data(),sizeof(Vec3)*count);a.routeStatus=8;}
    // No allocation or unsafe native read happens between copy and release.
    // Even a failed copy/vector allocation releases the returned native buffer.
    bool released=FreeRoute(points);
    if(!valid||!released) return false;
    a.routeStatus=9;
    for(Vec3 p:staged) if(!battle_ai::Finite(p)) return false;
    a.path.swap(staged);a.waypoint=0;RecordRouteInstall(a);
    a.routeStatus=10;
    return FollowPath(g_current,a);
}
bool LoadRoute(uint32_t id,const Vec3* lastSeen,bool force=false) {
    __try { return LoadRouteUnsafe(id,lastSeen,force); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NavigationNodeCount(uint8_t* nav,uint32_t& count) {
    using namespace battle_ai::native::detail;
    uint8_t* begin=nullptr;uint8_t* end=nullptr;
    if(!Read(nav,0x560,begin)||!Read(nav,0x564,end))return false;
    const auto first=reinterpret_cast<uintptr_t>(begin),last=reinterpret_cast<uintptr_t>(end);
    if(first!=last) {
        if(!first||last<first||(last-first)%sizeof(uint32_t))return false;
        const auto entries=(last-first)/sizeof(uint32_t);
        if(entries>1000000)return false;
        count=static_cast<uint32_t>(entries);
    } else {
        int32_t alternate=0;
        if(!Read(nav,0x580,alternate)||alternate<0||alternate>1000000)return false;
        count=static_cast<uint32_t>(alternate);
    }
    return count!=0;
}
uint32_t SearchRandom(Actor& actor,uint32_t id) {
    if(!actor.searchRng)actor.searchRng=id^static_cast<uint32_t>(actor.generation*2654435761u)^0x6D2B79F5u;
    if(!actor.searchRng)actor.searchRng=1;
    actor.searchRng^=actor.searchRng<<13;actor.searchRng^=actor.searchRng>>17;actor.searchRng^=actor.searchRng<<5;
    return actor.searchRng;
}
bool ExploreMapUnsafe() {
    using namespace battle_ai::native::detail;
    if(GetCurrentThreadId()!=g_gameThread||!Owned(g_current)||!g_document||!*g_document||
       !g_navigationNode||!g_navigationNodeAt||!g_startPoint)return false;
    auto& actor=ActorFor(g_current);
    actor.explorationStatus=1;
    if(!MovementAvailable(g_current,false))return false;
    if(actor.explorationOwned&&(actor.geometryOwned||!actor.path.empty())) {
        if(FollowPath(g_current,actor)&&!actor.path.empty())return true;
        if(actor.geometryOwned&&actor.mapSearch.State()==battle_ai::MapPathSearch::Status::Pending)return true;
        actor.explorationOwned=false;
    }
    const auto now=GetTickCount64();
    if(now<actor.explorationRetryAt)return false;
    actor.explorationRetryAt=now+150;
    uint8_t* nav=nullptr;uint32_t count=0;
    actor.explorationStatus=2;
    if(!ReadPointer(*g_document,0x8364,nav))return false;
    const bool geometry=PlayerBotRulesMode(g_current)&&EmptyNavigation(nav);
    if(!geometry&&!NavigationNodeCount(nav,count))return false;
    actor.explorationNodeCount=count;actor.explorationStatus=3;
    const Vec3 here=Position(g_current);
    if(!battle_ai::Finite(here))return false;
    const int ownTeam=*reinterpret_cast<int*>(g_current+0x20);
    const uint32_t id=*reinterpret_cast<uint32_t*>(g_current+4);
    // Public spawn anchors lead out of a closed home area; global mesh nodes
    // then cover other connected areas. Neither source reads hidden enemies.
    for(unsigned attempt=0;attempt<4;++attempt) {
        ++actor.explorationCandidates;
        const uint32_t cursor=actor.searchCursor++;
        uint8_t* node=nullptr;uint32_t kind=2;Vec3 goal{};
        if(geometry||cursor%3==0) {
            const int colour=geometry&&cursor%3==2?0:ownTeam==1?2:ownTeam==2?1:1+static_cast<int>((cursor/3)%2);
            const int index=geometry?static_cast<int>(cursor%(colour?6:12)):static_cast<int>((cursor/3)%12);
            auto record=g_startPoint(*g_document,colour,index);Vec3 authored{};
            if(record&&Read(record,0x24,authored)&&battle_ai::Finite(authored)) {
                if(geometry)goal=authored;
                else node=g_navigationNode(nav,authored);
            } else continue;
            kind=1;
        } else node=g_navigationNodeAt(nav,static_cast<int>(SearchRandom(actor,id)%count));
        if(!geometry&&(!node||!Read(node,0x180,goal)||!battle_ai::Finite(goal)))continue;
        ++actor.explorationValidNodes;
        Vec3 delta=goal-here;delta.y=0;
        if(battle_ai::Dot(delta,delta)<600.0f*600.0f)continue;
        if(actor.explorationRoutes) {
            Vec3 prior=goal-actor.searchGoal;prior.y=0;
            if(battle_ai::Dot(prior,prior)<250.0f*250.0f)continue;
        }
        actor.pathRanged=false;actor.defenceMoveOwned=false;
        ++actor.explorationRouteAttempts;
        if(!LoadRoute(0,&goal)||actor.path.empty()) {
            if(actor.geometryOwned&&actor.mapSearch.State()==battle_ai::MapPathSearch::Status::Pending) {
                actor.explorationOwned=true;actor.searchGoal=goal;actor.searchGoalKind=kind;return true;
            }
            continue;
        }
        actor.explorationOwned=true;actor.searchGoal=goal;actor.searchGoalKind=kind;
        ++actor.explorationRoutes;actor.explorationStatus=4;return true;
    }
    return false;
}
int __cdecl ExploreMap(lua_State* L) {
    bool admitted=false;
    __try {admitted=ExploreMapUnsafe();}
    __except(EXCEPTION_EXECUTE_HANDLER) {admitted=false;}
    if(g_runtime.logDecisions&&GetCurrentThreadId()==g_gameThread&&Owned(g_current)) {
        auto& actor=ActorFor(g_current);const auto now=GetTickCount64();
        if(now>=actor.explorationLogAt&&g_api&&g_api->Log) {
            actor.explorationLogAt=now+5000;
            uint8_t* nav=nullptr;uint8_t* fromNode=nullptr;uint8_t* toNode=nullptr;
            int32_t fromId=-1,toId=-1;
            __try {
                if(battle_ai::native::detail::ReadPointer(*g_document,0x8364,nav)&&nav&&g_navigationNode) {
                    fromNode=g_navigationNode(nav,actor.routeStart);toNode=g_navigationNode(nav,actor.routeGoal);
                    if(fromNode)battle_ai::native::detail::Read(fromNode,0x17C,fromId);
                    if(toNode)battle_ai::native::detail::Read(toNode,0x17C,toId);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            g_api->Log(g_mod,SDGO_LOG_INFO,"public navigation unit=%u status=%u nodes=%u candidates=%u valid=%u route_attempts=%u routes=%llu route_status=%u points=%u follow_status=%u from=%.1f,%.1f,%.1f node=%d to=%.1f,%.1f,%.1f node=%d",
                *reinterpret_cast<uint32_t*>(g_current+4),actor.explorationStatus,actor.explorationNodeCount,
                actor.explorationCandidates,actor.explorationValidNodes,actor.explorationRouteAttempts,
                static_cast<unsigned long long>(actor.explorationRoutes),actor.routeStatus,actor.routePointCount,actor.followStatus,
                actor.routeStart.x,actor.routeStart.y,actor.routeStart.z,fromId,
                actor.routeGoal.x,actor.routeGoal.y,actor.routeGoal.z,toId);
            if(actor.geometryOwned||actor.geometryProjections)g_api->Log(g_mod,SDGO_LOG_INFO,
                "map geometry unit=%u nodes=%u state=%u path=%u projections=%llu no_ground=%llu blocked=%llu last_reject=%u",
                *reinterpret_cast<uint32_t*>(g_current+4),static_cast<uint32_t>(actor.mapSearch.Nodes()),static_cast<uint32_t>(actor.mapSearch.State()),
                static_cast<uint32_t>(actor.path.size()),static_cast<unsigned long long>(actor.geometryProjections),
                static_cast<unsigned long long>(actor.geometryGroundMisses),static_cast<unsigned long long>(actor.geometryBlocked),actor.geometryReject);
        }
    }
    g_lua.pushnumber(L,admitted?1:0);return 1;
}
int __cdecl EnableNavigationJump(lua_State* L) {
    const bool valid=GetCurrentThreadId()==g_gameThread&&Owned(g_current)&&PlayerBotRulesMode(g_current);
    if(valid) {
        auto& actor=ActorFor(g_current);
        if(!actor.ownsJumpPermission) {
            actor.savedJumpPermission=g_current[0x3291];actor.ownsJumpPermission=true;
        }
        // Native collision probing selects the required obstacle height. This
        // enables that existing input gate; it changes no physical unit stats.
        g_setJumpEnabled(g_current,1);
    }
    g_lua.pushnumber(L,valid?1:0);return 1;
}
int __cdecl RouteMotion(lua_State* L) {
    const double mode=g_lua.tonumber(L,1);
    const bool valid=GetCurrentThreadId()==g_gameThread&&Owned(g_current)&&(mode==2||mode==3);
    if(valid)ActorFor(g_current).pathMoveType=static_cast<int>(mode);
    g_lua.pushnumber(L,valid?1:0);return 1;
}
int __cdecl Route(lua_State* L) {
    double number=g_lua.tonumber(L,1);
    Vec3 frozen={};
    bool provided=g_lua.gettop(L)>=5;
    if(Owned(g_current)) {auto& actor=ActorFor(g_current);actor.pathRanged=g_lua.gettop(L)>=6&&g_lua.tonumber(L,6)==1;actor.defenceMoveOwned=false;}
    if(provided) frozen={static_cast<float>(g_lua.tonumber(L,3)),static_cast<float>(g_lua.tonumber(L,4)),static_cast<float>(g_lua.tonumber(L,5))};
    const bool requested=std::isfinite(number)&&number>0&&number<=0xffffffff&&(!provided||battle_ai::Finite(frozen));
    const bool force=g_lua.gettop(L)>=2&&g_lua.tonumber(L,2)==1;
    bool ok=requested&&LoadRoute(static_cast<uint32_t>(number),provided?&frozen:nullptr,force);
    const auto actor=g_actors.find(g_current);
    const bool pending=requested&&!ok&&actor!=g_actors.end()&&actor->second.geometryOwned&&
        actor->second.mapSearch.State()==battle_ai::MapPathSearch::Status::Pending;
    if(!ok&&!pending) FailedRoute();
    g_lua.pushnumber(L,ok?1:pending?2:0);return 1;
}
bool AdvancePointUnsafe(Vec3 target,float stepLimit,bool allowRanged) {
    if(!Owned(g_current)||GetCurrentThreadId()!=g_gameThread||*reinterpret_cast<int*>(g_current+0x38)<=0)
        return false;
    auto& actor=ActorFor(g_current);
    const bool recovering=actor.recoveryControlled&&GetTickCount64()-actor.recoveryAt<250;
    if(actor.defenceMoveOwned&&actor.path.empty()&&!actor.defence.movement.relocate)
        actor.defenceMoveOwned=false;
    if(recovering||actor.defenceMoveOwned||!MovementAvailable(g_current,allowRanged))
        return false;
    Vec3 here=Position(g_current);
    if(!battle_ai::Finite(here)||!battle_ai::Finite(target)||!std::isfinite(stepLimit)) return false;
    stepLimit=(std::max)(100.0f,(std::min)(400.0f,stepLimit));
    Vec3 offset={target.x-here.x,0,target.z-here.z};
    double distance=battle_ai::Length(offset);
    if(!std::isfinite(distance)||distance<=1.0) return false;
    float scale=distance>stepLimit?stepLimit/static_cast<float>(distance):1.0f;
    Vec3 goal={here.x+offset.x*scale,here.y,here.z+offset.z*scale};
    if(!battle_ai::Finite(goal)||
       MotionSafetyFor(g_current,goal)!=battle_ai::native::MotionSafety::Safe||!ImmediateMotionClear(g_current,goal)) return false;
    std::vector<Vec3> staged;
    try { staged.push_back(goal); } catch(...) { return false; }
    if(!g_setGoal(g_current,&goal,1)) return false;
    actor.issuedGoal=goal;actor.hasIssuedGoal=true;
    actor.path.swap(staged);actor.waypoint=0;actor.pathRanged=allowRanged;actor.controlled=true;
    actor.geometryOwned=false;actor.pathGeometry=false;
    return true;
}
bool AdvancePointGuarded(Vec3 target,float stepLimit,bool allowRanged) {
    __try { return AdvancePointUnsafe(target,stepLimit,allowRanged); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int __cdecl AdvancePoint(lua_State* L) {
    bool ok=false;
    if(g_lua.gettop(L)>=5) {
        Vec3 target={static_cast<float>(g_lua.tonumber(L,1)),
                     static_cast<float>(g_lua.tonumber(L,2)),
                     static_cast<float>(g_lua.tonumber(L,3))};
        double limit=g_lua.tonumber(L,4),ranged=g_lua.tonumber(L,5);
        ok=std::isfinite(limit)&&std::isfinite(ranged)&&
            AdvancePointGuarded(target,static_cast<float>(limit),ranged==1);
    }
    g_lua.pushnumber(L,ok?1:0);return 1;
}
int SafeDirectionImpl(lua_State* L,bool adaptive) {
    double requested=g_lua.tonumber(L,1),distanceValue=g_lua.tonumber(L,2);
    double forwardValue=g_lua.tonumber(L,3),backwardValue=g_lua.tonumber(L,4);
    double leftValue=g_lua.tonumber(L,5),rightValue=g_lua.tonumber(L,6);
    double selected=-1,selectedDistance=0;
    if(g_lua.gettop(L)>=6&&Owned(g_current)&&g_document&&*g_document&&
       GetCurrentThreadId()==g_gameThread&&
       std::isfinite(requested)&&std::isfinite(distanceValue)&&distanceValue>0&&
       std::isfinite(forwardValue)&&std::isfinite(backwardValue)&&
       std::isfinite(leftValue)&&std::isfinite(rightValue)) {
        __try {
            auto node=*reinterpret_cast<uint8_t**>(g_current+8);
            Vec3 forward=node?battle_ai::Horizontal(*reinterpret_cast<Vec3*>(node+0x6C)):Vec3{};
            const double length=battle_ai::Length(forward);
            if(node&&std::isfinite(length)&&length>0.5) {
                forward=forward*static_cast<float>(1.0/length);
                const Vec3 right={forward.z,0,-forward.x};
                const Vec3 here=Position(g_current);
                struct Candidate {double value;Vec3 direction;};
                Candidate candidates[4] = {};
                auto direction=[&](double value) {
                    if(value==forwardValue) return forward;
                    if(value==backwardValue) return forward*-1.0f;
                    if(value==leftValue) return right*-1.0f;
                    if(value==rightValue) return right;
                    return Vec3{};
                };
                size_t candidateCount=0;
                const auto append=[&](double value) {
                    for(size_t i=0;i<candidateCount;++i)if(candidates[i].value==value)return;
                    const Vec3 vector=direction(value);
                    if(candidateCount<4&&battle_ai::Length(vector)>=0.5)candidates[candidateCount++]={value,vector};
                };
                append(requested);append(leftValue);append(rightValue);append(backwardValue);append(forwardValue);
                int rejected[4]={};size_t tested=0;
                battle_ai::native::MotionSafetyDiagnostic diagnostic{};
                battle_ai::native::MotionSafetyDiagnostic unavailable{};
                battle_ai::native::MapGeometry rejectedGeometry{};
                const auto clearStep=[&](const Candidate& candidate,double span,int& reason) {
                    const Vec3 destination=here+candidate.direction*static_cast<float>(span);
                    const auto safety=battle_ai::native::CheckMotionSafety(
                        g_navigation,*g_document,g_current,here,destination,g_runtime.logDecisions?&diagnostic:nullptr);
                    if(safety==battle_ai::native::MotionSafety::Unavailable) {unavailable=diagnostic;reason=100+static_cast<int>(diagnostic.stage);return false;}
                    auto geometry=MapGeometryFor(g_current);geometry.deferDestroyables=false;Vec3 grounded{};
                    const bool clear=safety==battle_ai::native::MotionSafety::Safe&&
                        (!PlayerBotRulesMode(g_current)||geometry.Step(here,destination,grounded));
                    reason=safety!=battle_ai::native::MotionSafety::Safe?11:clear?0:static_cast<int>(geometry.lastReject);
                    if(!clear&&g_runtime.logDecisions)rejectedGeometry=geometry;
                    return clear;
                };
                for(const auto& candidate:candidates) {
                    if(battle_ai::Length(candidate.direction)<0.5) continue;
                    int reason=0;const bool clear=clearStep(candidate,distanceValue,reason);rejected[tested++]=reason;
                    if(clear) {
                        selected=candidate.value;selectedDistance=distanceValue;break;
                    }
                }
                if(adaptive&&selected<0) {
                    float step=0;
                    if(battle_ai::native::detail::Read(g_current,0xB2C,step)&&std::isfinite(step)&&step>=0) {
                        const double minimum=(std::max)(25.0,double(step)*10+25);
                        for(const auto& candidate:candidates) {
                            if(battle_ai::Length(candidate.direction)<0.5||distanceValue<minimum)continue;
                            double low=0,high=distanceValue;int reason=0;
                            for(int iteration=0;iteration<6;++iteration) {
                                const double middle=(low+high)*0.5;
                                if(clearStep(candidate,middle,reason))low=middle;else high=middle;
                            }
                            if(low>=minimum&&low>selectedDistance&&clearStep(candidate,low,reason)) {
                                selected=candidate.value;selectedDistance=low;
                            }
                        }
                    }
                }
                auto& actor=ActorFor(g_current);const auto now=GetTickCount64();
                if(selected<0&&g_runtime.logDecisions&&now>=actor.directionLogAt) {
                    actor.directionLogAt=now+500;
                    g_api->Log(g_mod,SDGO_LOG_INFO,"native direction rejected unit=%u requested=%.0f distance=%.1f pos=%.1f,%.1f,%.1f rejects=%d,%d,%d,%d scene=%p object=%p predicate=%p count=%d",
                        *reinterpret_cast<uint32_t*>(g_current+4),requested,distanceValue,here.x,here.y,here.z,rejected[0],rejected[1],rejected[2],rejected[3],
                        unavailable.scene,unavailable.object,unavailable.predicate,unavailable.count);
                    auto geometry=MapGeometryFor(g_current);Vec3 low{},high{},feet{};
                    const float foot=geometry.bodySphere.center.y-geometry.bodySphere.radius;
                    const bool lowKnown=geometry.Ground(here,here.y+80,here.y-4096,low);
                    const bool highKnown=geometry.Ground(here,here.y+800,here.y-4096,high);
                    const bool feetKnown=geometry.Ground(here,foot+5,foot-4096,feet);
                    g_api->Log(g_mod,SDGO_LOG_INFO,"native support unit=%u root_y=%.3f foot_y=%.3f local_sphere_y=%.3f local_radius=%.3f sphere_y=%.3f radius=%.3f low=%u/%.3f high=%u/%.3f feet=%u/%.3f bounds_y=%.3f,%.3f",
                        *reinterpret_cast<uint32_t*>(g_current+4),here.y,foot,geometry.bodySphere.localCenter.y,geometry.bodySphere.localRadius,
                        geometry.bodySphere.center.y,geometry.bodySphere.radius,lowKnown,low.y,highKnown,high.y,feetKnown,feet.y,
                        geometry.ownerBounds.min.y,geometry.ownerBounds.max.y);
                    uint32_t objectId=0;int32_t objectHp=0;uint8_t removed=0,obbEnabled=0,sphereEnabled=0;
                    using battle_ai::native::detail::Read;
                    Read(rejectedGeometry.rejectedObject,4,objectId);Read(rejectedGeometry.rejectedObject,0x38,objectHp);
                    Read(rejectedGeometry.rejectedObject,0x1C,removed);Read(rejectedGeometry.rejectedScene,0x114,obbEnabled);
                    Read(rejectedGeometry.rejectedScene,0x115,sphereEnabled);
                    g_api->Log(g_mod,SDGO_LOG_INFO,"native solid rejection unit=%u scene=%p object=%u hp=%d removed=%u flags114=%u flags115=%u shape=%u reason=%u initial_depth=%.3f next_depth=%.3f ray_distance=%.3f obstacle_sphere=%.3f,%.3f,%.3f/%.3f body_sphere=%.3f,%.3f,%.3f/%.3f",
                        *reinterpret_cast<uint32_t*>(g_current+4),rejectedGeometry.rejectedScene,objectId,objectHp,removed,obbEnabled,sphereEnabled,
                        rejectedGeometry.rejectedShape,rejectedGeometry.shapeReject,rejectedGeometry.initialDepth,rejectedGeometry.rejectedDepth,rejectedGeometry.rejectedRayDistance,
                        rejectedGeometry.rejectedSphereCenter.x,rejectedGeometry.rejectedSphereCenter.y,rejectedGeometry.rejectedSphereCenter.z,rejectedGeometry.rejectedSphereRadius,
                        rejectedGeometry.bodySphere.center.x,rejectedGeometry.bodySphere.center.y,rejectedGeometry.bodySphere.center.z,rejectedGeometry.bodySphere.radius);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_lua.pushnumber(L,selected);
    if(adaptive){g_lua.pushnumber(L,selectedDistance);return 2;}
    return 1;
}
int __cdecl SafeDirection(lua_State* L){return SafeDirectionImpl(L,false);}
int __cdecl SafeStep(lua_State* L){return SafeDirectionImpl(L,true);}
int __cdecl Chance(lua_State* L);
bool DirectIntentUnsafe(double direction,int mode,int action) {
    if(GetCurrentThreadId()!=g_gameThread||!Owned(g_current))return false;
    if(action==2)return (mode==2||mode==3)&&std::isfinite(direction)&&
        *reinterpret_cast<int32_t*>(g_current+0x38)>0&&MovementAvailable(g_current,true);
    auto& actor=ActorFor(g_current);auto& direct=actor.direct;
    const bool active=g_current[0x30DC]!=0;
    const Vec3 goal=*reinterpret_cast<Vec3*>(g_current+0x3078);
    const Vec3 start=*reinterpret_cast<Vec3*>(g_current+0x3084);
    const uint32_t facing=*reinterpret_cast<uint32_t*>(g_current+0xDC4);
    const auto echo=[&] {
        return active&&battle_ai::Finite(goal)&&battle_ai::Finite(start)&&
            goal.x==direct.goal.x&&goal.y==direct.goal.y&&goal.z==direct.goal.z&&
            start.x==direct.start.x&&start.y==direct.start.y&&start.z==direct.start.z&&facing==direct.facing;
    };
    if(action==-1) {
        if(direct.owned&&echo()&&MovementAvailable(g_current,true))g_stop(g_current);
        direct={};return false;
    }
    if((mode!=2&&mode!=3)||!std::isfinite(direction)||!actor.path.empty()||actor.geometryOwned||
       !MovementAvailable(g_current,true)) {direct={};return false;}
    if(action==0&&(!direct.owned||direct.direction!=direction||direct.mode!=mode))return false;
    if(!active||!battle_ai::Finite(goal)||(action==0&&!echo())) {direct={};return false;}
    auto geometry=MapGeometryFor(g_current);Vec3 grounded{};
    if(MotionSafetyFor(g_current,goal)!=battle_ai::native::MotionSafety::Safe||
       (PlayerBotRulesMode(g_current)&&!geometry.Step(Position(g_current),goal,grounded))) {
        if(action==1||direct.owned)g_stop(g_current);
        direct={};return false;
    }
    if(g_setMoveType&&g_current[0xDC0]!=mode)g_setMoveType(g_current,mode);
    if(action==1) {direct.owned=true;direct.direction=direction;direct.mode=mode;direct.goal=goal;direct.start=start;
        direct.facing=*reinterpret_cast<uint32_t*>(g_current+0xDC4);}
    return true;
}
bool GuardedDirectIntent(double direction,int mode,int action) {
    __try {return DirectIntentUnsafe(direction,mode,action);} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
int __cdecl DirectIntent(lua_State* L) {
    battle_ai::native::detail::ReadableRegionCache regions{};
    battle_ai::native::detail::ScopedReadableRegionCache scope(regions);
    const double direction=g_lua.tonumber(L,1),mode=g_lua.tonumber(L,2),action=g_lua.tonumber(L,3);
    const bool valid=(mode==2||mode==3)&&(action==-1||action==0||action==1||action==2);
    const bool accepted=valid&&GuardedDirectIntent(direction,static_cast<int>(mode),static_cast<int>(action));
    g_lua.pushnumber(L,accepted?1:0);return 1;
}
int __cdecl Cover(lua_State* L) {
    g_lua.newtable(L);Number(L,"valid",0);
    if(GetCurrentThreadId()!=g_gameThread||!Owned(g_current)||!g_document||!*g_document) return 1;
    __try {
        double id=g_lua.tonumber(L,1);
        Vec3 target={static_cast<float>(g_lua.tonumber(L,2)),static_cast<float>(g_lua.tonumber(L,3)),static_cast<float>(g_lua.tonumber(L,4))};
        if(!std::isfinite(id)||id<=0||id>0xffffffff||!battle_ai::Finite(target)) return 1;
        auto targetId=static_cast<uint32_t>(id);auto other=FindObject(targetId);
        if(!CombatTargetAlive(other)) return 1;
        auto self=g_current;Vec3 here=Position(self),center=AimPoint(self);
        Vec3 toward=target-center;toward.y=0;double len=battle_ai::Length(toward);
        if(len<100) return 1;
        Vec3 side={-toward.z/static_cast<float>(len),0,toward.x/static_cast<float>(len)};
        Ray3 ray={};auto fsm=*reinterpret_cast<uint8_t**>(self+0x9CC);
        auto native=g_ray(fsm,&ray);if(!native||!battle_ai::Finite(native->origin)) return 1;
        Vec3 muzzle=native->origin-here;
        float halfWidth=60,halfHeight=60;
        auto scene=*reinterpret_cast<uint8_t**>(self+0x34);
        if(scene) {
            auto bounds=reinterpret_cast<battle_ai::native::Aabb*>(scene+0xBC);
            if(battle_ai::native::detail::ValidAabb(*bounds)) {
                halfWidth=(std::max)(30.0f,(std::min)(160.0f,(std::max)(bounds->max.x-bounds->min.x,bounds->max.z-bounds->min.z)*0.5f));
                halfHeight=(std::max)(30.0f,(std::min)(160.0f,(bounds->max.y-bounds->min.y)*0.4f));
            }
        }
        auto clear=[&](Vec3 from,Vec3 to) {return battle_ai::native::FreshLineClear(g_navigation,*g_document,self,targetId,from,to,true);};
        auto exposed=[&](Vec3 p) {
            Vec3 c=p+(center-here);int count=0;
            for(Vec3 offset:{Vec3{},side*halfWidth,side*(-halfWidth),Vec3{0,halfHeight,0},Vec3{0,-halfHeight,0}})
                if(clear(target,c+offset)) ++count;
            return count;
        };
        auto& actor=ActorFor(self);auto& search=actor.coverSearch;
        auto record=fsm?*reinterpret_cast<uint8_t**>(fsm+0x248):nullptr;
        uint32_t weapon=record?*reinterpret_cast<uint32_t*>(record):0;
        int preferred=g_lua.gettop(L)>=5?static_cast<int>(g_lua.tonumber(L,5)):0;
        auto now=GetTickCount64();
        if(actor.coverQueryUsed&&actor.coverQueryFrame==actor.thinkSerial) {Number(L,"pending",1);return 1;}
        actor.coverQueryFrame=actor.thinkSerial;actor.coverQueryUsed=true;
        if(!search.active||search.target!=targetId||search.weapon!=weapon||search.side!=preferred||now-search.started>500) {
            search={};search.active=true;search.target=targetId;search.weapon=weapon;
            search.side=preferred;search.started=now;
        }
        // One hide candidate per native Think, at most 35 search rays plus 11
        // final validation rays. Ordinary combat continues while search is pending.
        const int index=search.sample++,sign=index<3?-1:1,tucked=index%3;
        Vec3 h=here+side*static_cast<float>(sign*tucked*100);
        if(exposed(h)<=1) for(int step=1;step<=5;++step) {
            Vec3 p=h+side*static_cast<float>(-sign*step*70);
            if(!clear(p+muzzle,target)) continue;
            int n=exposed(p);if(n==0||n>3) continue;
            float score=static_cast<float>(battle_ai::Length(h-here)+battle_ai::Length(p-h))*0.05f+n*100;
            if(preferred&&preferred!=sign) score+=15;
            if(score<search.score) {search.score=score;search.hide=h;search.peek=p;search.exposure=n;}
            break;
        }
        if(search.sample<6) {Number(L,"pending",1);return 1;}
        search.active=false;
        // Candidate points may be several frames old. Revalidate against the
        // current target before granting a cover movement/shot opportunity.
        if(search.score<1.0e9f&&battle_ai::Length(search.hide-here)<=500&&battle_ai::Length(search.peek-here)<=850&&
           exposed(search.hide)<=1&&clear(search.peek+muzzle,target)) {
            const int exposure=exposed(search.peek);
            if(exposure>0&&exposure<=3) {
                Number(L,"valid",1);Vector(L,"hide",search.hide);Vector(L,"peek",search.peek);Number(L,"exposure",exposure);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 1;
}
bool WantsGeneric(uint8_t* unit) {
    return g_runtime.enabled&&g_runtime.generic&&Owned(unit)&&!g_preparationFault;
}
bool WantsAim(uint8_t* unit) {
    return g_runtime.enabled&&g_runtime.aim&&(g_runtime.generic||g_runtime.difficulty==1)&&Owned(unit)&&!g_preparationFault;
}
Vec3 ShieldDirection(uint8_t* unit,const DefencePlan& plan,Vec3 current) {
    if(!plan.shielding) return plan.movement.facing.direction;
    auto shield=battle_ai::native::ReadShield(unit);
    if(!shield.intact||(shield.type!=0&&(!plan.shieldHitKnown||shield.type!=plan.shieldHitType))) return plan.movement.facing.direction;
    Vec3 position=Position(unit);
    Vec3 toward=battle_ai::Horizontal(plan.shieldThreat-position);
    if(battle_ai::Length(toward)<0.5) return plan.movement.facing.direction;
    struct ShieldThreat {Vec3 direction;float weight;};
    ShieldThreat threats[32];size_t count=0;
    for(size_t i=0;i<plan.count;++i) {
        Vec3 incoming=battle_ai::Horizontal(plan.threats[i].point-position);
        if(battle_ai::Length(incoming)>=0.5) threats[count++]={incoming,plan.threats[i].weight};
    }
    Vec3 best=plan.movement.facing.direction;double bestRisk=1e30;
    for(unsigned kind=1;kind<=4;++kind) {
        if(!battle_ai::native::ShieldCovers(shield,kind)) continue;
        double offset=kind==2?battle_ai::kPi:(kind==3?-battle_ai::kPi/2:(kind==4?battle_ai::kPi/2:0));
        Vec3 candidate=battle_ai::Heading(battle_ai::Bearing(toward)+offset);
        double worst=0;
        for(size_t i=0;i<count;++i) {
            double risk=(1-battle_ai::Dot(candidate,threats[i].direction))*threats[i].weight;
            if(battle_ai::Dot(threats[i].direction,toward)>0.95) risk*=0.1; // the intact shield faces this attack
            worst=(std::max)(worst,risk);
        }
        double cost=worst+std::abs(battle_ai::WrapAngle(battle_ai::Bearing(candidate)-battle_ai::Bearing(current)))*0.001;
        if(cost<bestRisk) {bestRisk=cost;best=candidate;}
    }
    return best;
}
void ApplyDefence(uint8_t* unit,Actor& a) {
    auto now=GetTickCount64();auto& plan=a.defence;
    if(!plan.threatened||now-plan.at>250) {
        if(a.defenceMoveOwned) {
            if(MovementAvailable(unit,a.pathRanged)) g_stop(unit);
            a.path.clear();a.defenceMoveOwned=false;
            a.geometryOwned=false;a.geometryFinal=false;
        }
        return;
    }
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    auto movement=*reinterpret_cast<uint8_t**>(unit+0x9C8);
    if(a.sideCue||!fsm||!movement) return;
    // Lua/AttackIntent can queue a new action after Capture. Read only the two
    // live state pointers here; a cached pre-decision capability must not let
    // defence turn the body away from a just-started melee/special action.
    const auto current=*reinterpret_cast<uint32_t*>(fsm+4),pending=*reinterpret_cast<uint32_t*>(fsm+8);
    const auto currentMove=*reinterpret_cast<uint32_t*>(movement+4),pendingMove=*reinterpret_cast<uint32_t*>(movement+8);
    bool hurt=false;
    for(size_t offset:{size_t(0xD4),size_t(0xE8),size_t(0xFC),size_t(0x110),size_t(0x138)})
        hurt=hurt||battle_ai::native::detail::StateIs(movement,currentMove,offset)||battle_ai::native::detail::StateIs(movement,pendingMove,offset);
    auto active=[&](size_t offset) {
        return battle_ai::native::detail::StateIs(fsm,current,offset)||battle_ai::native::detail::StateIs(fsm,pending,offset);
    };
    bool liveMelee=false,liveSpecial=false;
    for(size_t offset:{size_t(0x48),size_t(0x5C),size_t(0x70),size_t(0x84),size_t(0x98),size_t(0xE8),size_t(0xFC),size_t(0x1D8),size_t(0x1EC)})
        liveMelee=liveMelee||active(offset);
    for(size_t offset:{size_t(0x98),size_t(0xAC),size_t(0xC0),size_t(0xD4),size_t(0x1D8),size_t(0x1EC)})
        liveSpecial=liveSpecial||active(offset);
    if(((plan.meleeBusy||liveMelee)&&!hurt)||plan.specialBusy||liveSpecial) return;
    for(size_t offset:{size_t(0xD4),size_t(0x124),size_t(0x138),size_t(0x160),size_t(0x174),size_t(0x188)})
        if(battle_ai::native::detail::InState(movement,offset)) return;
    if(a.defenceMoveOwned&&!plan.movement.relocate) {
        if(!hurt) g_stop(unit);
        a.path.clear();a.defenceMoveOwned=false;
    }
    auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
    const bool neutral=battle_ai::native::detail::StateIs(fsm,current,0x20)&&
        (pending==0||battle_ai::native::detail::StateIs(fsm,pending,0x20));
    const bool legalRanged=battle_ai::native::detail::StateIs(fsm,current,0x34)&&
        (pending==0||battle_ai::native::detail::StateIs(fsm,pending,0x34))&&record&&(record[0x8F]==1||fsm[0xB38]==1);
    const bool recovering=a.recoveryControlled&&now-a.recoveryAt<250;
    if(plan.movement.relocate&&!recovering&&!hurt&&(neutral||legalRanged)&&
       (now>=a.defenceRepathAt||(a.defenceMoveOwned&&a.path.empty()))&&plan.routeTarget) {
        a.defenceRepathAt=now+650;a.pathRanged=true;
        a.defenceMoveOwned=LoadRoute(plan.routeTarget,&plan.movement.destination);
        if(!a.defenceMoveOwned) {
            struct Alternative {Vec3 point;double rear,risk;};
            Alternative options[12];size_t count=0;
            const Vec3 position=Position(unit);
            const float radius=g_runtime.difficulty>=2?1200.0f:480.0f;
            for(int i=0;i<12;++i) {
                Vec3 p=position+battle_ai::Heading(i*2*battle_ai::kPi/12)*240.0f;
                Vec3 offset=p-a.defenceAnchor;offset.y=0;
                const double distance=battle_ai::Length(offset);
                if(distance>radius) {p.x=a.defenceAnchor.x+offset.x*radius/static_cast<float>(distance);p.z=a.defenceAnchor.z+offset.z*radius/static_cast<float>(distance);}
                if(battle_ai::Length(p-position)<25||battle_ai::Length(p-plan.movement.destination)<1) continue;
                bool close=false;
                for(size_t k=0;k<plan.count;++k)
                    if(battle_ai::Length(plan.threats[k].point-p)<180&&battle_ai::Length(plan.threats[k].point-p)<battle_ai::Length(plan.threats[k].point-position)) close=true;
                if(close) continue;
                auto facing=battle_ai::DefensiveFacing(p,plan.movement.facing.direction,plan.threats,plan.count);
                const auto& prior=plan.movement.facing;
                if(!facing.valid||facing.rearExposure>prior.rearExposure+0.00001||
                   (facing.rearExposure>=prior.rearExposure-0.01&&facing.rearRisk>prior.rearRisk-0.08)) continue;
                options[count++]={p,facing.rearExposure,facing.rearRisk};
            }
            std::sort(options,options+count,[](const Alternative& x,const Alternative& y){return x.rear!=y.rear?x.rear<y.rear:x.risk<y.risk;});
            for(size_t i=0;i<(std::min)(count,size_t(2));++i) if(LoadRoute(plan.routeTarget,&options[i].point)) {
                a.defenceMoveOwned=true;plan.movement.destination=options[i].point;break;
            }
            if(!a.defenceMoveOwned) g_stop(unit);
        }
    }
    auto node=*reinterpret_cast<uint8_t**>(unit+8);auto scene=*reinterpret_cast<uint8_t**>(unit+0x34);
    if(!node||!scene) return;
    Vec3 forward=*reinterpret_cast<Vec3*>(node+0x6C),cache=*reinterpret_cast<Vec3*>(unit+0x3090);
    if(!battle_ai::Finite(forward)||!battle_ai::Finite(cache)||battle_ai::Length(battle_ai::Horizontal(forward))<0.5||battle_ai::Length(battle_ai::Horizontal(cache))<0.5) return;
    const bool locomoting=unit[0x30DC]!=0||!a.path.empty()||a.defenceMoveOwned;
    // A grabbed, stationary Hard bot may present its intact rear shield. During
    // legal movement use front defence, independently of the waypoint direction.
    Vec3 desired=locomoting&&!hurt?plan.movement.facing.direction:ShieldDirection(unit,plan,forward);
    float step=battle_ai::TurnStepDegrees(forward,desired,900,a.turnedAt?static_cast<double>(now-a.turnedAt):16.0);
    a.turnedAt=now;
    Vec3 bounded=battle_ai::Heading(battle_ai::Bearing(forward)+step*battle_ai::kPi/180);
    // rotate_y changes the desired direction cache. Account for the native AI's
    // existing desired direction so the *consumed scene turn* stays bounded.
    float cacheDelta=static_cast<float>(battle_ai::WrapAngle(battle_ai::Bearing(bounded)-battle_ai::Bearing(cache))*180/battle_ai::kPi);
    if(std::abs(cacheDelta)>=0.01f) g_rotate(unit,cacheDelta,0);
}
void __fastcall FacingUpdate(uint8_t* unit,void*) {
    if(GetCurrentThreadId()==g_gameThread&&unit==g_current&&g_runtime.enabled&&g_runtime.difficulty>=1&&!g_preparationFault&&Owned(unit)&&*reinterpret_cast<int*>(unit+0x38)>0) {
        __try {
            auto actor=g_actors.find(unit);
            if(actor!=g_actors.end()) ApplyDefence(unit,actor->second);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_facingUpdate(unit);
}
int __cdecl Dispatch(const char* name,uint8_t* unit) {
    if(GetCurrentThreadId()!=g_gameThread) return g_dispatch(name,unit);
    const char* prior=g_nativeCallback;
    g_nativeCallback=name;
    int result=0;
    if(unit==g_current && !g_nativeFallback && g_runtime.enabled && g_runtime.difficulty>=1 && Owned(unit)) {
        auto& policy=ActorFor(unit);policy.policySerial=policy.thinkSerial;policy.lastPolicyAt=GetTickCount64();
        result=g_preparationFault||!g_policyReady ? 123:g_dispatch("SDGO_BattleAI_GenericUpdate",unit);
    } else {
        result=g_dispatch(name,unit);
        if(unit==g_current&&!g_nativeFallback&&g_runtime.enabled&&g_runtime.difficulty==1&&Owned(unit)&&!g_preparationFault)
            g_dispatch("SDGO_BattleAI_ProfileUpdate",unit);
    }
    g_nativeCallback=prior;
    return result;
}
bool NeutralPolicyAvailable(uint8_t* unit) {
    if(!WantsGeneric(unit)||g_runtime.difficulty<1||!g_policyReady||g_nativeFallback)return false;
    using namespace battle_ai::native::detail;
    ReadableRegionCache regions{};ScopedReadableRegionCache scope(regions);
    uint8_t* attack=nullptr;uint8_t* movement=nullptr;uint8_t* control=nullptr;
    int32_t hp=0;uint8_t script=0;
    // Retain the native dispatch caller's hurt, map travel, control-state,
    // installed-script admission gates. Boost Down is a resource restriction,
    // not loss of ordinary movement control.
    return Read(unit,0x38,hp)&&hp>0&&!ControlBlocked(unit)&&
        ReadPointer(unit,0x9CC,attack)&&Neutral(attack)&&
        ReadPointer(unit,0x9C8,movement)&&!InState(movement,0x124)&&
        ReadPointer(unit,0x9D4,control)&&!InState(control,0x98)&&
        Read(unit,0x30F0,script)&&script!=0;
}
bool __fastcall EffectActive(uint8_t* unit,void*,uint8_t effect) {
    const void* caller=_ReturnAddress();
    const bool active=g_effectActive(unit,effect);
    // Only the stock NPC scheduler's Boost Down pause is replaced. Player
    // physics, boost depletion and every other effect query see the real bit.
    if(active&&effect==5&&caller==g_aiBoostPauseReturn&&GetCurrentThreadId()==g_gameThread&&
       unit==g_current&&!g_nativeFallback&&g_policyReady&&g_runtime.difficulty>=1&&WantsGeneric(unit))return false;
    return active;
}
Ray3* __fastcall RangedRay(uint8_t* fsm,void*,Ray3* out) {
    Ray3* ray=g_ray(fsm,out);
    __try {
        if(GetCurrentThreadId()!=g_gameThread) return ray;
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        auto found=g_actors.find(owner);
        if(!ray || !WantsAim(owner) || found==g_actors.end()) return ray;
        uint32_t targetId=0;
        if(g_runtime.generic) {
            if(GetTickCount64()-found->second.aimAt>1000) return ray;
            targetId=found->second.aimTarget;
        } else targetId=*reinterpret_cast<uint32_t*>(fsm+0x200);
        if(!targetId) return ray;
        auto target=FindObject(targetId);
        if(!CombatTargetAlive(target)) return ray;
        battle_ai::native::Capabilities caps={};
        if(!battle_ai::native::ReadCapabilities(owner,caps)) return ray;
        float speed=0;
        bool knownSelected=false;
        for(const auto& w:caps.weapons) if(w.valid && w.slot==caps.selectedSlot) {
            knownSelected=true;
            if(w.kind==battle_ai::native::WeaponKind::Melee) return ray;
            speed=g_runtime.lead?PredictionSpeed(owner,caps,w):0;
        }
        if(!knownSelected) return ray;
        Vec3 point=AimPoint(target);
        Vec3 velocity=Velocity(target,targetId,point,GetTickCount64());
        auto aim=battle_ai::Aim(ray->origin,point,velocity,speed,g_runtime.maxLeadMs/1000.0);
        if(battle_ai::Finite(aim.direction) && battle_ai::Length(aim.direction)>0.5) ray->direction=aim.direction;
    } __except(EXCEPTION_EXECUTE_HANDLER) { /* retain original native ray */ }
    return ray;
}
void __fastcall AttackIntent(uint8_t* unit,void*) {
    if(GetCurrentThreadId()!=g_gameThread) {g_intent(unit);return;}
    auto found=g_actors.find(unit);
    bool controlled=WantsGeneric(unit)&&found!=g_actors.end()&&found->second.ownsIntent;
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    bool neutral=battle_ai::native::detail::Neutral(fsm);
    if(controlled && neutral) {
        // Remove the NPC's extra script-profile wait; the weapon FSM retains its
        // actual ammo, reload, animation and cooldown checks.
        *reinterpret_cast<int*>(unit+0x31F4)=0;
    }
    g_intent(unit);
    if(WantsAim(unit)) {
        *reinterpret_cast<float*>(unit+0x3204)=0;*reinterpret_cast<float*>(unit+0x3208)=0;
    }
    if(!controlled) return;
    found=g_actors.find(unit);
    if(found==g_actors.end()||!found->second.ownsIntent) return;
    auto& a=found->second;
    auto record=fsm?*reinterpret_cast<uint8_t**>(fsm+0x248):nullptr;
    auto current=fsm?*reinterpret_cast<uint8_t**>(fsm+4):nullptr;
    auto pending=fsm?*reinterpret_cast<uint8_t**>(fsm+8):nullptr;
    // Any already queued non-Neutral action owns its admission input. This
    // includes a swing as well as a ranged/special handoff from tracking.
    const bool committedTrackingAction=fsm&&current==fsm+0x48&&pending&&pending!=fsm+0x20;
    if(a.suppressMelee&&!committedTrackingAction&&record&&(record[0x6A]==1||record[0x6A]==8))
        unit[0xDCD]=0;
    ULONGLONG now=GetTickCount64();
    // Pace only fresh ordinary requests. Current/pending actions, melee combo
    // continuation and special admission retain their original native input.
    const int slot=fsm?*reinterpret_cast<int*>(fsm+0x240):-1;
    const bool ordinary=slot>=0&&slot<3&&record&&record[0x6A]!=2;
    if(neutral && ordinary && unit[0xDCD]) {
        if(now<a.nextPulse) {
            unit[0xDCD]=0;
            if(g_runtime.logDecisions&&!a.pacingLogged) {
                a.pacingLogged=true;
                g_api->Log(g_mod,SDGO_LOG_INFO,"Medium ordinary attack pacing observed unit=%u interval_ms=%d; native action and movement retained",
                    *reinterpret_cast<uint32_t*>(unit+4),a.intervalMs);
            }
        }
        else a.nextPulse=now+static_cast<ULONGLONG>(a.intervalMs);
    }
    if(g_runtime.aim) {
        *reinterpret_cast<float*>(unit+0x3204)=0;
        *reinterpret_cast<float*>(unit+0x3208)=0;
    }
}
struct RangedShotAttempt {
    uint8_t* fsm=nullptr;
    uint8_t* owner=nullptr;
    uint8_t* record=nullptr;
    uint64_t generation=0;
    uint32_t ownerId=0;
    uint32_t action=0;
    uint32_t target=0;
    uint32_t weapon=0;
    ULONGLONG time=0;
    bool eligible=false;
};
RangedShotAttempt BeginRangedShot(uint8_t* fsm,uint32_t event,char chained,char force) {
    RangedShotAttempt attempt;
    __try {
        if(!g_gameThread || GetCurrentThreadId()!=g_gameThread || !fsm) return attempt;
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        if(!owner || *reinterpret_cast<int*>(owner+0x38)<=0) return attempt;
        auto& shot=g_rangedShots[owner];
        shot.time=GetTickCount64();
        shot.target=*reinterpret_cast<uint32_t*>(fsm+0x200);
        ++shot.calls;
        // Only primary kind 1/8 calls inside native 6B41A0 are new attack-cycle
        // fires. Kind 7, multiple muzzles and arbitrary 6B1270 callers are not.
        const bool primary=g_receiptFsm==fsm && event==0 && chained==0 && (force==0||force==1) &&
            *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x34;
        if(primary) {
            ++shot.attempts;
            // Preserve the established primary-receipt contract used by Lua
            // cadence and Hard follow-ups. Some legitimate native generators
            // return zero, so the raw 6B1270 return cannot gate this counter.
            ++shot.shots;shot.primaryAt=shot.time;shot.primaryTarget=shot.target;
        }
        auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
        shot.weapon=record?*reinterpret_cast<uint32_t*>(record):0;
        if(primary) {
            if(*reinterpret_cast<uint8_t**>(owner+0x9CC)==fsm && record) {
                auto generation=g_generations.find(owner);
                auto actor=g_actors.find(owner);
                auto action=*reinterpret_cast<uint32_t*>(fsm+0x224);
                auto ownerId=*reinterpret_cast<uint32_t*>(owner+4);
                auto weapon=*reinterpret_cast<uint32_t*>(record);
                const bool liveGeneration=generation!=g_generations.end()&&actor!=g_actors.end()&&
                    generation->second!=0&&actor->second.generation==generation->second;
                if(generation!=g_generations.end() && actor!=g_actors.end() &&
                   generation->second!=0 && actor->second.generation==generation->second &&
                   action!=0 && ownerId!=0 && weapon!=0) {
                    attempt={fsm,owner,record,generation->second,ownerId,action,shot.target,weapon,shot.time,true};
                }
                battle_ai::native::Capabilities caps={};
                auto motion=g_motion.find(ownerId);
                const auto now=shot.time;
                const bool freshMotion=liveGeneration&&motion!=g_motion.end()&&motion->second.unit==owner&&
                    motion->second.generation==generation->second&&motion->second.at&&
                    now>=motion->second.at&&now-motion->second.at<=250;
                Vec3 physical={};
                if(freshMotion) {
                    const Vec3 delta=battle_ai::Horizontal(Position(owner)-motion->second.point);
                    const Vec3 velocity=battle_ai::Horizontal(motion->second.velocity);
                    if(battle_ai::Finite(delta)&&battle_ai::Length(delta)>0.01) physical=delta;
                    else if(battle_ai::Finite(velocity)&&battle_ai::Length(velocity)>0.01) physical=velocity;
                }
                const bool actuallyMoving=battle_ai::Finite(physical)&&battle_ai::Length(physical)>0.01;
                const bool selectedMobileGun=liveGeneration&&
                    battle_ai::native::ReadCapabilities(owner,caps)&&caps.selectedSlot>=0&&caps.selectedSlot<3&&
                    caps.weapons[caps.selectedSlot].valid&&caps.weapons[caps.selectedSlot].ordinary&&
                    caps.weapons[caps.selectedSlot].kind!=battle_ai::native::WeaponKind::Melee&&
                    caps.weapons[caps.selectedSlot].movingShot&&caps.weapons[caps.selectedSlot].id==weapon;
                const bool mobileDispatch=selectedMobileGun&&!ControlBlocked(owner)&&owner[0x30DC]!=0&&actuallyMoving;
                if(mobileDispatch) {
                    // This is a post-ammo native generator-dispatch witness.
                    // It does not claim that every generator created a projectile
                    // or returned a universal success value.
                    ++shot.mobilePrimaryDispatches;
                    const auto& state=actor->second;
                    const bool freshDefencePlan=state.defence.at&&now>=state.defence.at&&
                        now-state.defence.at<=250&&state.defence.threatened&&state.defence.movement.relocate;
                    const bool freshDefenceRecovery=state.recoveryControlled&&state.recoveryAt&&
                        now>=state.recoveryAt&&now-state.recoveryAt<=250&&state.defenceEpisode&&
                        state.defenceEpisodeAt&&now>=state.defenceEpisodeAt&&now-state.defenceEpisodeAt<=1000;
                    if(state.defenceMoveOwned||freshDefencePlan||freshDefenceRecovery)
                        ++shot.mobileDefensiveDispatches;
                    if(state.observedTarget==shot.target&&state.observedTargetAt&&now>=state.observedTargetAt&&
                       now-state.observedTargetAt<=250) {
                        const Vec3 toward=battle_ai::Horizontal(state.observedTargetPoint-Position(owner));
                        if(battle_ai::Finite(toward)&&battle_ai::Length(toward)>0.01&&
                           battle_ai::Dot(toward,physical)<0)
                            ++shot.backwardPrimaryDispatches;
                    }
                }
            }
        }
        if(g_rangedShots.size()>2048) {
            ULONGLONG now=GetTickCount64();
            for(auto it=g_rangedShots.begin();it!=g_rangedShots.end();) {
                if(now-it->second.time>5000) it=g_rangedShots.erase(it); else ++it;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return attempt;
}
void CompleteRangedShot(const RangedShotAttempt& attempt,uint32_t result) {
    if(!result||!attempt.eligible) return;
    __try {
        if(!g_gameThread||GetCurrentThreadId()!=g_gameThread||
           !attempt.owner||!attempt.fsm||!attempt.record||
           *reinterpret_cast<int*>(attempt.owner+0x38)<=0||
           *reinterpret_cast<uint32_t*>(attempt.owner+4)!=attempt.ownerId||
           *reinterpret_cast<uint8_t**>(attempt.owner+0x9CC)!=attempt.fsm||
           *reinterpret_cast<uint8_t**>(attempt.fsm+0xB2C)!=attempt.owner||
           *reinterpret_cast<uint32_t*>(attempt.fsm+0x224)!=attempt.action||
           *reinterpret_cast<uint8_t**>(attempt.fsm+0x248)!=attempt.record||
           *reinterpret_cast<uint32_t*>(attempt.record)!=attempt.weapon) return;
        auto generation=g_generations.find(attempt.owner);
        auto actor=g_actors.find(attempt.owner);
        auto shot=g_rangedShots.find(attempt.owner);
        if(generation==g_generations.end()||actor==g_actors.end()||shot==g_rangedShots.end()||
           generation->second!=attempt.generation||actor->second.generation!=attempt.generation) return;
        // Diagnostic only: a stable nonzero return is useful evidence for the
        // projectile/Lua generators that expose it, but zero is also a legal
        // result for other native firing families.
        ++shot->second.nonzeroReturns;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
uint32_t __fastcall RangedShot(uint8_t* fsm,void*,uint32_t event,char chained,char force) {
    auto attempt=BeginRangedShot(fsm,event,chained,force);
    auto result=g_shot(fsm,event,chained,force);
    CompleteRangedShot(attempt,result);
    return result;
}
void __fastcall FireReceipts(uint8_t* fsm,void*,void* event) {
    if(GetCurrentThreadId()!=g_gameThread) {g_fireReceipts(fsm,event);return;}
    auto prior=g_receiptFsm;g_receiptFsm=fsm;
    __try {g_fireReceipts(fsm,event);} __finally {g_receiptFsm=prior;}
}
void __fastcall AttackAdmission(uint8_t* fsm,void*) {
    if(GetCurrentThreadId()==g_gameThread&&g_runtime.enabled&&g_runtime.difficulty>=1&&!g_preparationFault) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
            auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
            if(Owned(owner)&&record&&record[0x69]!=2&&(record[0x6A]==1||record[0x6A]==8)) {
                auto target=FindObject(*reinterpret_cast<uint32_t*>(fsm+0x200));
                if(target&&!Destroyable(target)) {
                    const auto protection=battle_ai::native::ReadDamageProtection(target);
                    if(!protection.known||protection.active) return;
                }
            }
            // This is the shared ordinary/special admission function. Apply
            // the player SP rule only to AttackStyle 2, never normal shots.
            if(Owned(owner)&&record&&record[0x69]==2) {
                if(!FairSpecialResource(fsm,owner)) return;
                auto& actor=ActorFor(owner);auto now=GetTickCount64();auto weapon=*reinterpret_cast<uint32_t*>(record);
                if(actor.expectedSpecialWeapon!=weapon||now-actor.expectedSpecialAt>3000) {actor.expectedSpecialAt=now;actor.expectedSpecialWeapon=weapon;}
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {return;}
    }
    g_attackAdmission(fsm);
}
struct AbsoluteSpMessage {uint16_t type=0x18,padding=0;uint32_t reserved[3]={};int32_t value=0;};
static_assert(sizeof(AbsoluteSpMessage)==20);
void ApplyNativeAbsoluteSp(uint8_t* unit,int32_t value) {
    AbsoluteSpMessage message;message.value=value;
    using ReceiveFn=void(__thiscall*)(uint8_t*,const AbsoluteSpMessage*);
    auto vtable=*reinterpret_cast<void***>(unit);
    if(vtable&&vtable[7]) reinterpret_cast<ReceiveFn>(vtable[7])(unit,&message);
}
void ApplySpNotice(const SpNotice& notice) {
    if(notice.value<0||notice.value>30000||GetTickCount64()-notice.time>3000) return;
    __try {
        if(!g_document||!*g_document) return;
        auto unit=FindObject(g_canonical(*g_document,notice.pilot));auto found=g_actors.find(unit);
        if(!unit||*reinterpret_cast<uint32_t*>(unit+0x2B68)!=notice.pilot) return;
        auto npc=*reinterpret_cast<uint32_t*>(unit+0x3238);if(npc<3604001||npc>3607000) return;
        auto latest=g_spSnapshots.find(unit);
        if(latest!=g_spSnapshots.end()&&notice.sequence<latest->second.sequence) {
            // An old ordinary handler may have completed on a worker after a
            // newer state was drained. Restore the newer authoritative value.
            ApplyNativeAbsoluteSp(unit,latest->second.value);return;
        }
        if(notice.stateOnly) {
            if(notice.finish&&found!=g_actors.end()) {
                found->second.specialAcknowledged=false;found->second.specialFinished=notice.sequence;
                found->second.specialCompletion={};
            }
            if(g_spSnapshots.size()>2048) g_spSnapshots.clear();
            g_spSnapshots[unit]={notice.sequence,notice.value};
            ApplyNativeAbsoluteSp(unit,notice.value);return;
        }
        if(!g_runtime.enabled||g_runtime.difficulty<1||g_preparationFault) return;
        if(!Owned(unit)||found==g_actors.end()||*reinterpret_cast<uint32_t*>(unit+0x2B68)!=notice.pilot) return;
        auto& actor=found->second;
        if(notice.begin) {
            if(!notice.weapon||actor.expectedSpecialWeapon!=notice.weapon||notice.time<actor.expectedSpecialAt||notice.time-actor.expectedSpecialAt>3000) return;
            actor.expectedSpecialWeapon=0;actor.specialAcknowledged=true;actor.specialAcknowledgedAt=notice.time;
            actor.specialAcknowledgedSequence=notice.sequence;actor.specialCompletion={};
        } else {
            if(!actor.specialAcknowledged||notice.time<actor.specialAcknowledgedAt||notice.time-actor.specialAcknowledgedAt>60000) return;
            auto acceptedSequence=actor.specialAcknowledgedSequence;
            actor.specialAcknowledged=false;actor.specialFinished=notice.sequence;actor.specialCompletion={};
            if(notice.meleeCompletion) ArmMeleeCompletion(unit,actor,notice,acceptedSequence);
        }
        if(g_spSnapshots.size()>2048) g_spSnapshots.clear();
        g_spSnapshots[unit]={notice.sequence,notice.value};
        ApplyNativeAbsoluteSp(unit,notice.value);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
uint64_t NextSpSequence() {return g_spSequence.fetch_add(16,std::memory_order_relaxed)+16;}
bool ParseSpNotice(const uint8_t* packet,size_t valueOffset,bool begin,SpNotice& notice,uint64_t sequence) {
    __try {
        if(!packet||!g_gameThread) return false;
        notice.pilot=*reinterpret_cast<const uint32_t*>(packet+6);
        if(!(notice.pilot&0x80000000u)) return false;
        notice.value=*reinterpret_cast<const int32_t*>(packet+valueOffset);notice.begin=begin;notice.time=GetTickCount64();notice.sequence=sequence;
        if(begin) notice.weapon=*reinterpret_cast<const uint32_t*>(packet+0xA);
        return notice.value>=0&&notice.value<=30000;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ParseMeleeCompletion(const uint8_t* packet,SpNotice& notice,uint64_t sequence) {
    if(!ParseSpNotice(packet,0xE,false,notice,sequence)) return false;
    __try {
        if(*reinterpret_cast<const uint16_t*>(packet)==606) {
            notice.victimPilot=*reinterpret_cast<const uint32_t*>(packet+0x12);
            notice.damage=*reinterpret_cast<const uint32_t*>(packet+0x24);
            notice.meleeCompletion=notice.victimPilot!=0;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        notice.victimPilot=0;notice.damage=0;notice.meleeCompletion=false;
    }
    return true;
}
void SubmitSpNotice(const SpNotice& notice) {
    std::lock_guard<std::mutex> lock(g_spMutex);
    if(g_pendingSp.size()>=256) g_pendingSp.erase(g_pendingSp.begin());
    g_pendingSp.push_back(notice);
}
void DrainSpNotices() {
    std::vector<SpNotice> pending;
    {std::lock_guard<std::mutex> lock(g_spMutex);pending.swap(g_pendingSp);}
    std::sort(pending.begin(),pending.end(),[](const SpNotice& a,const SpNotice& b){return a.sequence<b.sequence;});
    for(const auto& notice:pending) ApplySpNotice(notice);
}
int __fastcall SpecialNotice(uint8_t* handler,void*,const uint8_t* packet) {
    SpNotice notice;bool valid=ParseSpNotice(packet,0x12,true,notice,NextSpSequence());
    int result=g_specialNotice(handler,packet);if(valid) SubmitSpNotice(notice);return result;
}
int __fastcall MeleeNotice(uint8_t* handler,void*,const uint8_t* packet) {
    SpNotice notice;bool valid=ParseMeleeCompletion(packet,notice,NextSpSequence());
    int result=g_meleeNotice(handler,packet);if(valid) SubmitSpNotice(notice);return result;
}
int __fastcall RangedNotice(uint8_t* handler,void*,const uint8_t* packet) {
    SpNotice notice;bool valid=ParseSpNotice(packet,0xA,false,notice,NextSpSequence());
    int result=g_rangedNotice(handler,packet);if(valid) SubmitSpNotice(notice);return result;
}
int __fastcall SkillNotice(uint8_t* handler,void*,const uint8_t* packet) {
    SpNotice notice;bool valid=ParseSpNotice(packet,0x1F,false,notice,NextSpSequence());
    int result=g_skillNotice(handler,packet);
    if(valid) {notice.stateOnly=true;SubmitSpNotice(notice);}return result;
}
bool IsSpStateOnly(const uint8_t* packet) {
    __try {return packet&&*reinterpret_cast<const uint16_t*>(packet)==0x779&&packet[0xA]==0xFF&&packet[0x17]==0&&
        *reinterpret_cast<const uint32_t*>(packet+0x13)==0&&(*reinterpret_cast<const uint32_t*>(packet+6)&0x80000000u)!=0;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
size_t CaptureOrdinarySp(const uint8_t* packet,SpNotice* notices,uint64_t sequence) {
    size_t used=0;
    __try {
        SpNotice caster;if(ParseSpNotice(packet,0xB,false,caster,sequence)) {caster.stateOnly=true;notices[used++]=caster;}
        unsigned count=packet[0x17];if(count>12) return used;
        for(unsigned i=0;i<count;++i) {
            auto row=packet+0x18+i*0x12;SpNotice victim;victim.pilot=*reinterpret_cast<const uint32_t*>(row);
            victim.value=*reinterpret_cast<const int32_t*>(row+8);victim.time=GetTickCount64();victim.sequence=sequence+i+1;victim.stateOnly=true;
            if((victim.pilot&0x80000000u)&&victim.value>=0&&victim.value<=30000) notices[used++]=victim;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return used;
}
int __fastcall SpStateNotice(uint8_t* handler,void*,const uint8_t* packet) {
    auto sequence=NextSpSequence();
    if(IsSpStateOnly(packet)) {
        SpNotice notice;if(ParseSpNotice(packet,0xB,false,notice,sequence)) {notice.stateOnly=true;notice.finish=true;SubmitSpNotice(notice);}
        return 1; // do not issue native command1C, HP, hurt or effect processing
    }
    SpNotice snapshots[13]={};size_t count=CaptureOrdinarySp(packet,snapshots,sequence);
    int result=g_spStateNotice(handler,packet);
    for(size_t i=0;i<count;++i) SubmitSpNotice(snapshots[i]);return result;
}
bool Roll(Actor& a,int percent,uint32_t id) {
    if(!a.rng) a.rng=id^static_cast<uint32_t>(a.generation*2654435761u)^0xA341316Cu;
    a.rng^=a.rng<<13;a.rng^=a.rng>>17;a.rng^=a.rng<<5;
    return a.rng%100<static_cast<unsigned>(percent);
}
int __cdecl Chance(lua_State* L) {
    double value=g_lua.tonumber(L,1);bool selected=false;
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)&&std::isfinite(value)&&value>=0&&value<=100)
        selected=Roll(ActorFor(g_current),static_cast<int>(value),*reinterpret_cast<uint32_t*>(g_current+4));
    g_lua.pushnumber(L,selected?1:0);return 1;
}
int g_quickSwingPercent=80;
void ClearStartup(uint8_t* unit,Actor& a,bool face) {
    if(a.sideCue) {
        // Zero yaw is an identity increment, not an inverse. Undo the actual
        // owner-only relative cue before restoring combat facing.
        g_rotate(unit,-a.cueAngle,0);g_rotate(unit,0,0);
        if(face) g_faceTarget(*reinterpret_cast<uint8_t**>(unit+0x9CC));
    }
    a.quickSwing=false;a.sideCue=false;a.startupTarget=0;
    a.cueAngle=0;
}
bool QuickSwingSafe(uint8_t* fsm,uint8_t* owner) {
    if(g_runtime.difficulty<2||!WantsGeneric(owner)||ControlBlocked(owner)||fsm[0x9B1]||*reinterpret_cast<int*>(owner+0x38)<=0) return false;
    auto target=FindObject(*reinterpret_cast<uint32_t*>(fsm+0x200));
    auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
    if(!target||Destroyable(target)||!CombatTargetAlive(target)||!record||record[0x6A]!=1) return false;
    const auto protection=battle_ai::native::ReadDamageProtection(target);
    if(!protection.known||protection.active) return false;
    battle_ai::native::Capabilities caps={};
    if(!battle_ai::native::ReadCapabilities(owner,caps)) return false;
    for(const auto& w:caps.weapons) if(w.valid&&w.slot==caps.selectedSlot&&w.kind==battle_ai::native::WeaponKind::Melee)
        return battle_ai::Length(Position(target)-Position(owner))<=w.range;
    return false;
}
void __fastcall TrackingEntry(uint8_t* fsm,void*,void* event) {
    g_trackingEntry(fsm,event);
    if(GetCurrentThreadId()!=g_gameThread) return;
    __try {
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        if(!QuickSwingSafe(fsm,owner)||*reinterpret_cast<int*>(fsm+0x968)!=0) return;
        auto& a=ActorFor(owner);ClearStartup(owner,a,false);
        // One roll per actual tracking Entry, never once per Think/frame.
        a.quickSwing=Roll(a,g_quickSwingPercent,*reinterpret_cast<uint32_t*>(owner+4));
        if(a.quickSwing) {
            a.startupAt=GetTickCount64();a.startupTarget=*reinterpret_cast<uint32_t*>(fsm+0x200);
            a.sideCue=true;a.cueAngle=(a.rng&1)?35.0f:-35.0f;g_rotate(owner,a.cueAngle,0);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool TryQuickSwing(uint8_t* fsm) {
    __try {
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        auto it=g_actors.find(owner);if(it==g_actors.end()||!it->second.quickSwing) return false;
        auto& a=it->second;auto elapsed=GetTickCount64()-a.startupAt;
        if(!QuickSwingSafe(fsm,owner)||a.startupTarget!=*reinterpret_cast<uint32_t*>(fsm+0x200)||elapsed>120) {
            ClearStartup(owner,a,true);return false;
        }
        if(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C) return true;
        if(*reinterpret_cast<uint8_t**>(fsm+8)) {ClearStartup(owner,a,true);return false;}
        if(elapsed<16) return true; // brief owner-only cue, no tracking dash
        g_queueSwing(fsm);
        if(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C) return true;
        ClearStartup(owner,a,true);return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void __fastcall TrackingUpdate(uint8_t* fsm,void*,void* event) {
    if(GetCurrentThreadId()==g_gameThread) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
            auto pending=*reinterpret_cast<uint8_t**>(fsm+8);
            auto found=g_actors.find(owner);
            if(WantsGeneric(owner)&&*reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x48&&
               pending&&pending!=fsm+0x20&&found!=g_actors.end())
                found->second.cancelMeleeTracking=false;
            if(WantsGeneric(owner)&&*reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x48&&(!pending||pending==fsm+0x20)) {
                auto target=FindObject(*reinterpret_cast<uint32_t*>(fsm+0x200));
                bool cancel=found!=g_actors.end()&&found->second.cancelMeleeTracking;
                if(target&&!cancel&&!Destroyable(target)) {
                    const auto protection=battle_ai::native::ReadDamageProtection(target);
                    cancel=!protection.known||protection.active;
                }
                if(cancel) {
                    if(found!=g_actors.end()) {
                        ClearStartup(owner,found->second,true);
                        found->second.cancelMeleeTracking=false;
                    }
                    g_setIntent(owner,0);
                    owner[0x312A]=0;
                    *reinterpret_cast<uint16_t*>(owner+0xDCD)=0;
                    if(!pending) g_queueState(fsm,fsm+0x20);
                    return; // native Release/Neutral Entry run at the next FSM update
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if(GetCurrentThreadId()!=g_gameThread||!TryQuickSwing(fsm)) g_trackingUpdate(fsm,event);
}
void __fastcall SwingEntry(uint8_t* fsm,void*,void* event) {
    if(GetCurrentThreadId()==g_gameThread) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);auto it=g_actors.find(owner);
            if(it!=g_actors.end()&&it->second.quickSwing) ClearStartup(owner,it->second,true);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_swingEntry(fsm,event);
}
void __fastcall SwingUpdate(uint8_t* fsm,void*,void* event) {
    bool suppress=false;
    if(GetCurrentThreadId()==g_gameThread) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);auto it=g_actors.find(owner);
            if(WantsGeneric(owner)&&it!=g_actors.end()&&event&&*reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x5C) {
                auto target=FindObject(*reinterpret_cast<uint32_t*>(fsm+0x200));
                const bool gate=target&&Destroyable(target);
                const auto protection=target&&!gate?battle_ai::native::ReadDamageProtection(target):battle_ai::native::DamageProtection{};
                suppress=it->second.suppressMelee||(target&&!gate&&(!protection.known||protection.active));
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if(!suppress) {g_swingUpdate(fsm,event);return;}
    auto input=static_cast<uint8_t*>(event);uint8_t saved=input[0xD];input[0xD]=0;
    __try {g_swingUpdate(fsm,event);} __finally {input[0xD]=saved;}
}
bool __fastcall DeliverMessage(uint8_t* document,void*,uint32_t victim,void* message,uint32_t tag) {
    void* caller=_ReturnAddress();Contact receipt;uint8_t* attacker=nullptr;
    Incoming incoming;uint8_t* receiver=nullptr;
    uint64_t attackerGeneration=0,receiverGeneration=0;
    if(GetCurrentThreadId()==g_gameThread&&message&&tag==0) {
        __try {
            auto m=static_cast<uint8_t*>(message);auto other=FindObject(victim);
            if(Owned(other)&&*reinterpret_cast<uint16_t*>(m)==0xC&&*reinterpret_cast<int*>(m+0x14)>0) {
                auto id=*reinterpret_cast<uint32_t*>(m+0x3C);auto source=FindObject(id);
                if(source&&id&&id!=victim) {
                    receiver=other;incoming={id,Position(source),GetTickCount64(),caller==g_contactReturn,m[0x1A],m[0x1A]<=3};
                    auto life=g_generations.find(receiver);if(life!=g_generations.end()) receiverGeneration=life->second;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {receiver=nullptr;}
    }
    if(GetCurrentThreadId()==g_gameThread && caller==g_contactReturn && message && tag==0) {
        __try {
            auto m=static_cast<uint8_t*>(message);
            if(*reinterpret_cast<uint16_t*>(m)==0xC && *reinterpret_cast<int*>(m+0x14)>0) {
                attacker=FindObject(*reinterpret_cast<uint32_t*>(m+0x3C));
                auto life=g_generations.find(attacker);if(life!=g_generations.end()) attackerGeneration=life->second;
                auto fsm=attacker?*reinterpret_cast<uint8_t**>(attacker+0x9CC):nullptr;
                if(fsm && *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x5C) {
                    receipt={victim,*reinterpret_cast<uint32_t*>(fsm+0x224),*reinterpret_cast<int*>(fsm+0x968),GetTickCount64(),FindObject(victim)};
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {attacker=nullptr;}
    }
    bool result=g_message(document,victim,message,tag);
    auto sameLife=[&](uint8_t* unit,uint64_t before) {
        auto life=g_generations.find(unit);return (life==g_generations.end()?0:life->second)==before;
    };
    if(result&&receiver&&sameLife(receiver,receiverGeneration)&&battle_ai::Finite(incoming.point)) {
        auto defending=g_actors.find(receiver);if(defending!=g_actors.end()) defending->second.defence.at=0;
        if(g_incoming.size()>2048) g_incoming.clear();
        auto& sources=g_incoming[receiver];bool updated=false;
        for(auto& old:sources) if(old.attacker==incoming.attacker) {old=incoming;updated=true;break;}
        if(!updated) {if(sources.size()>=8) sources.erase(sources.begin());sources.push_back(incoming);}
    }
    if(result&&attacker&&sameLife(attacker,attackerGeneration)&&receipt.target&&receipt.victim) {
        if(g_contacts.size()>2048) g_contacts.clear();
        g_contacts[attacker]=receipt;
    }
    return result;
}
struct FlightSample {
    uint8_t* owner=nullptr;uint8_t* weapon=nullptr;uint8_t* node=nullptr;
    Vec3 point;uint64_t key=0;int modifier=0;
};
bool BeforeFlight(uint8_t* projectile,FlightSample& s) {
    __try {
        if(!g_gameThread||GetCurrentThreadId()!=g_gameThread) return false;
        if(!g_runtime.lead) return false;
        if(projectile[0x1D]) return false;
        s.owner=*reinterpret_cast<uint8_t**>(projectile+0x9C4);
        s.weapon=*reinterpret_cast<uint8_t**>(projectile+0x9C8);
        s.node=*reinterpret_cast<uint8_t**>(projectile+8);
        if(!WantsAim(s.owner)||!s.weapon||!s.node||g_actors.find(s.owner)==g_actors.end()) return false;
        // SetOwner captures the launch form. A projectile can outlive a form
        // switch; never attribute its old flight to the owner's new loadout.
        if(projectile[0x9ED]!=s.owner[0xE34]) return false;
        auto def=*reinterpret_cast<uint8_t**>(s.owner+0x9C4);
        if(!def) return false;
        uint32_t unitId=*reinterpret_cast<uint32_t*>(def),weaponId=*reinterpret_cast<uint32_t*>(s.weapon);
        if(!unitId||unitId>9999999||!weaponId||weaponId>9999999) return false;
        s.key=SpeedKey(unitId,weaponId,s.owner[0xE34]!=0);
        auto table=*reinterpret_cast<void***>(projectile);
        if(!table || table[0x18/sizeof(void*)]!=g_plainProjectileEntry) {
            // Homing and split/controller wrappers can keep constant speed
            // while turning. Preserve their native guidance, not linear lead.
            g_speeds[s.key]={0,0,GetTickCount64(),true};return false;
        }
        if(projectile[0xA38]!=0 || *reinterpret_cast<float*>(projectile+0xA8C)!=0) {
            g_speeds[s.key]={0,0,GetTickCount64(),true};
            return false;
        }
        s.modifier=SpeedModifier(s.owner,s.weapon);
        s.point=*reinterpret_cast<Vec3*>(s.node+0x88);
        return battle_ai::Finite(s.point);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void AfterFlightUnsafe(uint8_t* projectile,const FlightSample& s) {
    if(projectile[0xA38]!=0 || *reinterpret_cast<float*>(projectile+0xA8C)!=0) {
        g_speeds[s.key]={0,0,GetTickCount64(),true};return;
    }
    if(projectile[0x1D] ||
       *reinterpret_cast<uint8_t**>(projectile+0x9C4)!=s.owner ||
       *reinterpret_cast<uint8_t**>(projectile+0x9C8)!=s.weapon ||
       *reinterpret_cast<uint8_t**>(projectile+8)!=s.node) return;
    if(projectile[0x9ED]!=s.owner[0xE34]) return;
    Vec3 after=*reinterpret_cast<Vec3*>(s.node+0x88);
    float speed=static_cast<float>(battle_ai::Length(after-s.point));
    if(!battle_ai::Finite(after)||!std::isfinite(speed)||speed<0.001f||speed>100000) return;
    ULONGLONG now=GetTickCount64();
    auto& run=g_flights[projectile];
    bool continuous=run.key==s.key && now-run.seen<1000 &&
                    battle_ai::Length(run.end-s.point)<1 &&
                    std::abs(run.speed-speed)<(std::max)(0.1f,speed*0.03f);
    run.stable=continuous?run.stable+1:1;
    run.key=s.key;run.speed=speed;run.seen=now;run.end=after;
    auto known=g_speeds.find(s.key);
    if(run.stable>=3 && (known==g_speeds.end() || !known->second.nonlinear)) g_speeds[s.key]={speed,s.modifier,now,false};
    if(g_flights.size()>2048) {
        for(auto it=g_flights.begin();it!=g_flights.end();) {
            if(now-it->second.seen>2000) it=g_flights.erase(it);else ++it;
        }
    }
}
void AfterFlight(uint8_t* projectile,const FlightSample& s) {
    __try { AfterFlightUnsafe(projectile,s); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool __fastcall ProjectileUpdate(uint8_t* projectile,void*,void* opaque) {
    FlightSample sample;
    bool observe=BeforeFlight(projectile,sample);
    bool result=g_projectile(projectile,opaque);
    if(observe) AfterFlight(projectile,sample);
    return result;
}

template<typename T> bool Bind(T& out, const char* name) {
    out = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return out != nullptr;
}
FILETIME Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) ? d.ftLastWriteTime : FILETIME{};
}
bool Same(FILETIME a, FILETIME b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}
bool Read(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 512 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD n = 0;
        ok = ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}
struct Chunk { const char* data; size_t size; };
const char* __cdecl ReadChunk(lua_State*, void* user, size_t* n) {
    auto& c = *static_cast<Chunk*>(user);
    *n = c.size; c.size = 0;
    return *n ? c.data : nullptr;
}
bool Run(lua_State* L, const std::string& code) {
    int top = g_lua.gettop(L);
    Chunk c = {code.data(), code.size()};
    int err = g_lua.load(L, ReadChunk, &c, "=battle_ai");
    if (!err) err = g_lua.pcall(L, 0, 0, 0);
    if (err) g_api->Log(g_mod, SDGO_LOG_ERROR, "Lua reload rejected; previous policy retained: %s",
                        g_lua.tostring(L, -1) ? g_lua.tostring(L, -1) : "unknown error");
    g_lua.settop(L, top);
    return err == 0;
}
void Register(lua_State* L, const char* name, LuaFn fn) {
    g_lua.pushstring(L, name); g_lua.pushcclosure(L, fn, 0); g_lua.settable(L, kGlobals);
}
int __cdecl Now(lua_State* L) { g_lua.pushnumber(L, static_cast<double>(GetTickCount64())); return 1; }
int __cdecl Log(lua_State* L) {
    const char* s = g_lua.tostring(L, 1);
    if (s) g_api->Log(g_mod, SDGO_LOG_INFO, "%s", s);
    return 0;
}
int __cdecl PolicyFault(lua_State* L) {
    const char* message=g_lua.tostring(L,1);
    if(message) g_api->Log(g_mod,SDGO_LOG_ERROR,"AI policy failure: %s",message);
    return 0;
}
// Layouts belong only to the manifest's verified GOnline build. Remote copies
// (+2B65) must never run an additional decision loop. Lua also checks GetNPCID.
bool Eligible() {
    __try {
        return g_current && g_current[0x2B65] == 0 && g_current[0x326C] != 0 &&
               *reinterpret_cast<uint32_t*>(g_current + 0x3238) >= 3604001 &&
               *reinterpret_cast<uint32_t*>(g_current + 0x3238) <= 3607000;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int __cdecl Authority(lua_State* L) { g_lua.pushnumber(L, Eligible() ? 1 : 0); return 1; }
int __cdecl Generation(lua_State* L) {
    if(GetCurrentThreadId()!=g_gameThread) {g_lua.pushnumber(L,0);return 1;}
    auto it = g_generations.find(g_current);
    g_lua.pushnumber(L, it == g_generations.end() ? 0 : static_cast<double>(it->second));
    return 1;
}
struct Option { const wchar_t* name; int def, low, high; };
const Option kOptions[] = {
    {L"Enabled",1,0,1}, {L"GenericMode",1,0,1}, {L"DecisionMs",0,0,1000}, {L"ReactionMs",0,0,3000},
    {L"StuckMs",1400,300,10000}, {L"ProgressDistance",30,1,300}, {L"ArrivalDistance",100,10,500},
    {L"RecoveryDistance",350,50,1500}, {L"RecoveryMs",650,150,2000}, {L"RecoveryCooldownMs",1000,250,10000},
    {L"RangedMinDistance",450,0,2000}, {L"TacticCooldownMs",0,0,10000}, {L"LogDecisions",0,0,1},
    {L"TerrainRayAudit",0,0,1},
    {L"TargetHoldMs",0,0,5000},{L"FarTargetSlack",900,0,10000},{L"FarTargetRatioPercent",200,100,1000},
    {L"WeaponHoldMs",0,0,5000},{L"AttackIntervalMs",0,0,10000},
    {L"MediumAttackIntervalMs",600,0,2000},
    {L"BurstMs",0,0,10000},{L"BurstPauseMs",0,0,5000},
    {L"AirStrafe",1,0,1},{L"StrafeSwitchMs",450,150,3000},{L"StrafeDistance",400,50,1500},
    {L"JumpMs",600,50,1500},{L"JumpCooldownMs",1500,500,10000},{L"BoostReservePercent",20,0,90},
    {L"ThreatMemoryMs",1600,100,10000},{L"RepathMs",700,150,5000},
    {L"TargetMemoryMs",2500,0,10000},{L"PatrolMs",1600,300,10000},
    {L"AimAssist",1,0,1},{L"ProjectileLead",1,0,1},{L"MaxLeadMs",3000,100,10000}
    ,{L"SwingCancelChance",80,0,100},{L"CoverChance",90,0,100},{L"CoverSearchMs",1600,500,10000},
    {L"CoverTimeoutMs",2200,500,10000},{L"OrbwalkStep",120,50,500},{L"OrbwalkMs",180,80,600},{L"FlankMs",750,300,3000}
};
int CurrentRoomDifficulty() {
    char full[SDGO_ROOM_TITLE_MAX_UTF8_BYTES+1]={};
    const auto count=sdgo::CurrentRoomTitle(g_api->ModDirectory(g_mod),full,sizeof(full));
    if(count&&count<=SDGO_ROOM_TITLE_MAX_UTF8_BYTES&&full[count]==0) return battle_ai::RoomDifficulty(std::string_view(full,count));
    wchar_t title[21]={};size_t length=0;
    __try {
        auto app=g_app();if(!app) return -1;
        auto service=*reinterpret_cast<uint8_t**>(app+0x4A4);if(!service) return -1;
        auto mode=*reinterpret_cast<uint8_t**>(service+0x0C);if(!mode) return -1;
        auto room=*reinterpret_cast<uint8_t**>(mode+8);if(!room) return -1;
        length=*reinterpret_cast<uint32_t*>(room+0x10);
        auto capacity=*reinterpret_cast<uint32_t*>(room+0x14);
        if(length>20||capacity<7||capacity<length||capacity>0x1000) return -1;
        auto text=capacity<8?reinterpret_cast<wchar_t*>(room):*reinterpret_cast<wchar_t**>(room);
        if(!text||text[length]!=0) return -1;
        memcpy(title,text,length*sizeof(wchar_t));
    } __except(EXCEPTION_EXECUTE_HANDLER) {return -1;}
    return battle_ai::RoomDifficulty(std::wstring_view(title,length));
}
std::string ConfigChunk(RuntimeSettings& next,int roomDifficulty) {
    wchar_t difficulty[32]={};
    GetPrivateProfileStringW(L"settings",L"Difficulty",L"",difficulty,32,g_config.c_str());
    if(*difficulty) {
        if(!_wcsicmp(difficulty,L"Easy")||!wcscmp(difficulty,L"0")) next.difficulty=0;
        else if(!_wcsicmp(difficulty,L"Medium")||!wcscmp(difficulty,L"1")) next.difficulty=1;
        else if(!_wcsicmp(difficulty,L"Hard")||!wcscmp(difficulty,L"2")) next.difficulty=2;
        else {next.difficulty=1;g_api->Log(g_mod,SDGO_LOG_WARN,"invalid Difficulty; using Medium");}
    }
    if(roomDifficulty>=0&&roomDifficulty<=2) next.difficulty=roomDifficulty;
    std::string code = "local config = { Difficulty="+std::to_string(next.difficulty)+",\n";
    for (const auto& o : kOptions) {
        wchar_t buf[64] = {}, *end = nullptr;
        GetPrivateProfileStringW(L"settings", o.name, L"", buf, 64, g_config.c_str());
        long value = *buf ? wcstol(buf, &end, 10) : o.def;
        while (end && (*end == L' ' || *end == L'\t')) ++end;
        if ((end && *end) || value < o.low || value > o.high) {
            value = o.def;
            g_api->Log(g_mod, SDGO_LOG_WARN, "invalid config.ini option %ls; using %d", o.name, o.def);
        }
        std::string key;
        for (const wchar_t* p = o.name; *p; ++p) key.push_back(static_cast<char>(*p));
        if(key=="GenericMode") value=next.difficulty>=1?1:0;
        if(next.difficulty>=1&&(key=="DecisionMs"||key=="ReactionMs"||key=="TargetHoldMs"||key=="WeaponHoldMs"||
           key=="AttackIntervalMs"||key=="BurstMs"||key=="BurstPauseMs"||key=="TacticCooldownMs")) value=0;
        if(key=="SwingCancelChance"&&next.difficulty<2) value=0;
        if(key=="AimAssist"||key=="ProjectileLead") value=next.difficulty>=1?1:0;
        code += key + "=" + std::to_string(value) + ",\n";
        if(key=="Enabled") next.enabled=value!=0;
        if(key=="GenericMode") next.generic=value!=0;
        if(key=="LogDecisions") next.logDecisions=value!=0;
        if(key=="TerrainRayAudit") next.terrainRayAudit=value!=0;
        if(key=="AimAssist") next.aim=value!=0;
        if(key=="ProjectileLead") next.lead=value!=0;
        if(key=="MaxLeadMs") next.maxLeadMs=static_cast<int>(value);
        if(key=="MediumAttackIntervalMs") next.mediumAttackIntervalMs=static_cast<int>(value);
        if(key=="SwingCancelChance") next.quickSwing=static_cast<int>(value);
    }
    return code + "}\n";
}
void Prepare() {
    lua_State* L = g_lua.state(nullptr);
    if (!L) return;
    ULONGLONG now = GetTickCount64();
    if (L == g_loaded && now < g_checkAt) return;
    g_checkAt = now + 2000;
    FILETIME st = Stamp(g_script), ct = Stamp(g_config), ut=Stamp(g_universalScript);
    int roomDifficulty=CurrentRoomDifficulty();
    if (L == g_loaded && roomDifficulty==g_appliedRoomDifficulty && Same(st,g_scriptTime) && Same(ct,g_configTime) && Same(ut,g_universalTime)) {
        Run(L, "if SDGO_BattleAI_Refresh then SDGO_BattleAI_Refresh() end");
        return;
    }
    // Remember a failed attempt too: no compile/error spam each frame. An edit retries.
    if(L!=g_loaded) g_policyReady=false;
    g_loaded = L; g_scriptTime = st; g_configTime = ct;g_universalTime=ut;
    std::string script,universal;
    if (!Read(g_script, script) || !Read(g_universalScript,universal)) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "cannot read AI policy scripts"); return;
    }
    Register(L,"SDGO_BattleAI_Now",Now);
    Register(L,"SDGO_BattleAI_Log",Log);
    Register(L,"SDGO_BattleAI_Fault",PolicyFault);
    Register(L,"SDGO_BattleAI_Authority",Authority);
    Register(L,"SDGO_BattleAI_Generation",Generation);
    Register(L,"SDGO_BattleAI_Observe",Observe);
    Register(L,"SDGO_BattleAI_Native",NativeFallback);
    Register(L,"SDGO_BattleAI_AimTarget",AimTarget);
    Register(L,"SDGO_BattleAI_Cadence",Cadence);
    Register(L,"SDGO_BattleAI_RecoveryMode",RecoveryMode);
    Register(L,"SDGO_BattleAI_Route",Route);
    Register(L,"SDGO_BattleAI_RouteMotion",RouteMotion);
    Register(L,"SDGO_BattleAI_EnableNavigationJump",EnableNavigationJump);
    Register(L,"SDGO_BattleAI_ExploreMap",ExploreMap);
    Register(L,"SDGO_BattleAI_AdvancePoint",AdvancePoint);
    Register(L,"SDGO_BattleAI_SafeDirection",SafeDirection);
    Register(L,"SDGO_BattleAI_SafeStep",SafeStep);
    Register(L,"SDGO_BattleAI_DirectIntent",DirectIntent);
    Register(L,"SDGO_BattleAI_CancelRoute",CancelRoute);
    Register(L,"SDGO_BattleAI_Chance",Chance);
    Register(L,"SDGO_BattleAI_Cover",Cover);
    Register(L,"SDGO_BattleAI_MeleeGate",MeleeGate);
    Register(L,"SDGO_BattleAI_ProfileLease",ProfileLease);
    Register(L,"SDGO_BattleAI_ProfilePreviousSlot",ProfilePreviousSlot);
    Register(L,"SDGO_BattleAI_BoundSlot",BoundSlot);
    Register(L,"SDGO_BattleAI_ReleaseHeldIntent",ReleaseHeldIntent);
    Register(L,"SDGO_BattleAI_RequestWeapon",RequestWeapon);
    RuntimeSettings next;
    next.generic=true;
    if (Run(L, ConfigChunk(next,roomDifficulty) + script + "\n" + universal)) {
        g_policyReady=true;
        if(next.difficulty!=g_runtime.difficulty)
            Run(L,"if SDGO_BattleAI_Universal then SDGO_BattleAI_Universal.states={} end");
        g_runtime=next;
        g_appliedRoomDifficulty=roomDifficulty;
        g_quickSwingPercent=next.quickSwing;
        g_api->Log(g_mod, SDGO_LOG_INFO, "policies loaded (config.ini); difficulty %d, generic %d",next.difficulty,next.generic?1:0);
    }
}
void GuardedPrepare() {
    __try { Prepare(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_preparationFault = true;
        g_api->Log(g_mod, SDGO_LOG_ERROR, "AI policy preparation failed");
    }
}
void ResetLifecycle(uint8_t* unit) {
    g_terrainRays.ClearLayouts();
    g_transportCatalog.Clear();
    if(g_generations.size()>=1024) g_generations.clear();
    g_generations[unit]=++g_serial;
    g_actors.erase(unit);g_rangedShots.erase(unit);g_contacts.erase(unit);g_incoming.erase(unit);g_spSnapshots.erase(unit);
}
void DrainPendingInit() {
    std::vector<uint8_t*> pending;
    {std::lock_guard<std::mutex> lock(g_pendingMutex);pending.swap(g_pendingInit);}
    for(uint8_t* unit:pending) ResetLifecycle(unit);
}
bool __fastcall Think(uint8_t* unit, void*) {
    DWORD thread=GetCurrentThreadId(),expected=0;
    g_gameThread.compare_exchange_strong(expected,thread);
    if(thread!=g_gameThread) return g_think(unit);
    DrainPendingInit();
    DrainSpNotices();
    if (!g_loading && !g_preparationFault) { g_loading = true; GuardedPrepare(); g_loading = false; }
    uint8_t* prior = g_current; g_current = unit;
    // Init can precede loading in fixtures or a late loader; give that unit an identity.
    if (g_generations.find(unit) == g_generations.end()) {
        if (g_generations.size() >= 1024) g_generations.clear();
        g_generations[unit] = ++g_serial;
    }
    if(WantsGeneric(unit)) ++ActorFor(unit).thinkSerial;
    auto actor=g_actors.find(unit);
    if(actor!=g_actors.end())++actor->second.nativeThinkSerial;
    if(actor!=g_actors.end())SynchronizePhaseGeometry(unit,actor->second);
    if(actor!=g_actors.end())ReconcileTransportRoute(unit,actor->second);
    if(actor!=g_actors.end()&&actor->second.specialSlotLease>=0&&
       (!g_runtime.enabled||g_runtime.difficulty<1||g_preparationFault)&&Owned(unit)) {
        g_dispatch("SDGO_BattleAI_ProfileRelease",unit);
    }
    if(actor!=g_actors.end()) {
        if(actor->second.sideCue&&!WantsGeneric(unit)) ClearStartup(unit,actor->second,true);
        if((!WantsGeneric(unit)&&(actor->second.controlled||actor->second.ownsIntent||actor->second.ownsJumpPermission)) ||
           (!WantsAim(unit)&&actor->second.ownsAim)) RestoreIntent(unit,actor->second);
    }
    if(WantsAim(unit)) {
        auto& a=ActorFor(unit);
        if(!a.ownsAim) {
            a.savedYawError=*reinterpret_cast<float*>(unit+0x3204);a.savedPitchError=*reinterpret_cast<float*>(unit+0x3208);a.ownsAim=true;
        }
        *reinterpret_cast<float*>(unit+0x3204)=0;*reinterpret_cast<float*>(unit+0x3208)=0;
    }
    LARGE_INTEGER costBegin{},costEnd{};QueryPerformanceCounter(&costBegin);
    bool result=g_think(unit);
    auto decision=g_actors.find(unit);
    if(decision!=g_actors.end()&&decision->second.controlled&&
       decision->second.policySerial!=decision->second.thinkSerial&&NeutralPolicyAvailable(unit)) {
        // Native action completion can omit dispatch_ai for an otherwise free
        // frame. Drive the same established policy once, preserving native
        // action ownership and any decision already made during this Think.
        decision->second.policySerial=decision->second.thinkSerial;
        decision->second.lastPolicyAt=GetTickCount64();
        g_dispatch("SDGO_BattleAI_GenericUpdate",unit);
    }
    auto startup=g_actors.find(unit);
    if(startup!=g_actors.end()&&startup->second.sideCue) {
        __try {
            auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
            if(!battle_ai::native::detail::InState(fsm,0x48)&&!battle_ai::native::detail::InState(fsm,0x5C))
                ClearStartup(unit,startup->second,!ControlBlocked(unit));
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if(WantsGeneric(unit)||(g_runtime.enabled&&g_runtime.difficulty>=1&&Owned(unit))) {
        auto it=g_actors.find(unit);
        if(it!=g_actors.end() && (!it->second.path.empty()||it->second.geometryOwned||it->second.transport.active)&&(WantsGeneric(unit)||it->second.defenceMoveOwned)) {
            __try { FollowPath(unit,it->second); } __except(EXCEPTION_EXECUTE_HANDLER) { it->second.path.clear(); }
        }
    }
    QueryPerformanceCounter(&costEnd);
    auto costActor=g_actors.find(unit);
    if(costActor!=g_actors.end()) {
        auto& actor=costActor->second;actor.thinkCost.Record(costBegin.QuadPart,costEnd.QuadPart);
        const auto now=GetTickCount64();
        if(g_runtime.logDecisions&&actor.lastPolicyAt&&now-actor.lastPolicyAt>=500&&now>=actor.policyGapLogAt) {
            actor.policyGapLogAt=now+250;
            using battle_ai::native::detail::Read;using battle_ai::native::detail::ReadPointer;
            uint32_t id=0,state=0,effects=0,pendingEffects=0;
            uint8_t enabled=0,script=0;uint8_t* secondary=nullptr;uint8_t* attack=nullptr;uint8_t* movement=nullptr;
            uint32_t sc=0,sp=0,ac=0,ap=0,mc=0,mp=0;
            Read(unit,4,id);Read(unit,0x9D8,state);Read(unit,0x326C,enabled);Read(unit,0x30F0,script);
            Read(unit,0xE90,effects);Read(unit,0xEA0,pendingEffects);
            ReadPointer(unit,0x9D4,secondary);Read(secondary,4,sc);Read(secondary,8,sp);
            ReadPointer(unit,0x9CC,attack);Read(attack,4,ac);Read(attack,8,ap);
            ReadPointer(unit,0x9C8,movement);Read(movement,4,mc);Read(movement,8,mp);
            g_api->Log(g_mod,SDGO_LOG_INFO,"native policy gap unit=%u age=%llu controlled=%u ai326c=%u state=%u effects=%08X/%08X script=%u secondary=%p:%08X/%08X attack=%p:%08X/%08X movement=%p:%08X/%08X serial=%llu/%llu",
                id,static_cast<unsigned long long>(now-actor.lastPolicyAt),actor.controlled?1u:0u,enabled,state,effects,pendingEffects,script,
                secondary,sc,sp,attack,ac,ap,movement,mc,mp,static_cast<unsigned long long>(actor.thinkSerial),static_cast<unsigned long long>(actor.policySerial));
        }
        if(g_runtime.logDecisions&&now>=actor.costLogAt&&g_api&&g_api->Log) {
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
            const double scale=1000000.0/static_cast<double>(frequency.QuadPart);
            const auto average=[&](const Actor::Cost& value){return value.calls?value.total*scale/static_cast<double>(value.calls):0.0;};
            g_api->Log(g_mod,SDGO_LOG_INFO,"AI cost unit=%u think_us=%.1f peak_us=%.1f sense_us=%.1f sense_peak_us=%.1f nav_us=%.1f nav_peak_us=%.1f calls=%llu nodes=%u",
                *reinterpret_cast<uint32_t*>(unit+4),average(actor.thinkCost),actor.thinkCost.peak*scale,
                average(actor.senseCost),actor.senseCost.peak*scale,average(actor.navigationCost),actor.navigationCost.peak*scale,
                static_cast<unsigned long long>(actor.thinkCost.calls),static_cast<uint32_t>(actor.mapSearch.Nodes()));
            const auto& terrain=g_terrainRays.stats;
            g_api->Log(g_mod,SDGO_LOG_INFO,"terrain rays indexed=%llu native=%llu triangles=%llu audits=%llu mismatches=%llu layouts=%llu rejected=%llu",
                static_cast<unsigned long long>(terrain.indexed),static_cast<unsigned long long>(terrain.native),
                static_cast<unsigned long long>(terrain.triangles),static_cast<unsigned long long>(terrain.audits),
                static_cast<unsigned long long>(terrain.mismatches),static_cast<unsigned long long>(terrain.layouts),static_cast<unsigned long long>(terrain.rejected));
            float nativeStep=0;uint8_t nativeMode=0;
            battle_ai::native::detail::Read(unit,0xB2C,nativeStep);battle_ai::native::detail::Read(unit,0xDC0,nativeMode);
            const Vec3 here=Position(unit);const bool hasWaypoint=!actor.path.empty()&&actor.waypoint<actor.path.size();
            const Vec3 waypoint=hasWaypoint?actor.path[actor.waypoint]:here;
            g_api->Log(g_mod,SDGO_LOG_INFO,"native route motor unit=%u mode=%u requested=%d stop_range=%.2f waypoint_distance=%.2f pos=%.1f,%.1f,%.1f waypoint=%.1f,%.1f,%.1f",
                *reinterpret_cast<uint32_t*>(unit+4),nativeMode,actor.pathMoveType,nativeStep*10,battle_ai::Length(waypoint-here),
                here.x,here.y,here.z,waypoint.x,waypoint.y,waypoint.z);
            actor.thinkCost={};actor.senseCost={};actor.navigationCost={};actor.costLogAt=now+5000;
        }
    }
    g_current = prior;
    return result;
}
void __fastcall Init(uint8_t* unit, void*, void* data) {
    g_init(unit, data);
    if(GetCurrentThreadId()==g_gameThread) ResetLifecycle(unit);
    else {std::lock_guard<std::mutex> lock(g_pendingMutex);g_pendingInit.push_back(unit);}
}
}

// match_ai calls this optional endpoint only after native spawn/deck creation.
// Missing/disabled battle_ai leaves match_ai's original standalone behavior.
extern "C" __declspec(dllexport) void __cdecl SdgoBattleAiInitializeUnit(uint8_t* unit,int difficulty,uint32_t generation) {
    std::lock_guard<std::mutex> lock(g_loadoutMutex);
    auto result=InitializeLoadout(unit,difficulty,generation);
    if(g_api&&result!=battle_ai::loadout::ApplyResult::Applied&&result!=battle_ai::loadout::ApplyResult::AlreadyApplied&&
       result!=battle_ai::loadout::ApplyResult::InvalidArgument)
        g_api->Log(g_mod,SDGO_LOG_WARN,"native bot loadout initialization incomplete (%d)",static_cast<int>(result));
}
extern "C" __declspec(dllexport) int __cdecl SdgoBattleAiGetUnitSnapshotV1(uint32_t id,SdgoBattleAiUnitSnapshotV1* output) {
    __try {
        if(!output||output->size<sizeof(*output)||output->version!=1||GetCurrentThreadId()!=g_gameThread) return 0;
        auto unit=FindObject(id);auto found=g_actors.find(unit);
        if(!unit||found==g_actors.end()||unit[0x2B65]!=0||
           *reinterpret_cast<uint32_t*>(unit+0x3238)<3604001||*reinterpret_cast<uint32_t*>(unit+0x3238)>3607000) return 0;
        battle_ai::native::Capabilities caps={};if(!battle_ai::native::ReadCapabilities(unit,caps)) return 0;
        const auto& actor=found->second;SdgoBattleAiUnitSnapshotV1 result{};
        result.size=sizeof(result);result.version=1;result.object_id=id;result.unit_id=caps.unitId;result.generation=actor.generation;
        result.hp=*reinterpret_cast<int32_t*>(unit+0x38);result.hurt_state=*reinterpret_cast<uint32_t*>(unit+0x9D8);
        result.captured_by=*reinterpret_cast<uint32_t*>(unit+0xEB8);result.control_blocked=ControlBlocked(unit);result.special_busy=caps.specialBusy;
        result.ai_enabled=unit[0x326C]!=0;
        result.selected_slot=caps.selectedSlot;result.special_finished=actor.specialFinished;
        result.special_zero_completions=static_cast<uint32_t>(
            std::min<uint64_t>(actor.zeroCompletionCount,UINT32_MAX));
        auto shot=g_rangedShots.find(unit);if(shot!=g_rangedShots.end()) result.ranged_shots=shot->second.shots;
        if(shot!=g_rangedShots.end()) result.ranged_attempts=shot->second.attempts;
        if(shot!=g_rangedShots.end()) result.ranged_nonzero_returns=shot->second.nonzeroReturns;
        if(shot!=g_rangedShots.end()) result.mobile_primary_dispatches=shot->second.mobilePrimaryDispatches;
        if(shot!=g_rangedShots.end()) result.mobile_defensive_dispatches=shot->second.mobileDefensiveDispatches;
        if(shot!=g_rangedShots.end()) result.backward_primary_dispatches=shot->second.backwardPrimaryDispatches;
        auto attack=*reinterpret_cast<uint8_t**>(unit+0x9CC);auto movement=*reinterpret_cast<uint8_t**>(unit+0x9C8);
        result.target_id=attack?*reinterpret_cast<uint32_t*>(attack+0x200):0;
        if(result.target_id==actor.observedTarget) {
            result.target_visible=actor.observedTargetVisible;result.target_distance=actor.observedTargetDistance;
            result.target_x=actor.observedTargetPoint.x;result.target_y=actor.observedTargetPoint.y;result.target_z=actor.observedTargetPoint.z;
        }
        result.defence_moving=actor.defenceMoveOwned||actor.defence.movement.relocate;
        result.moving=unit[0x30DC]!=0;
        if(caps.selectedSlot>=0&&caps.selectedSlot<4) {
            result.moving_shot=caps.weapons[caps.selectedSlot].movingShot;result.weapon_range=caps.weapons[caps.selectedSlot].range;
        }
        result.max_ordinary_range=actor.observedMaxRange;result.visible_far_targets=actor.observedFarTargets;
        const Vec3 pos=Position(unit);result.x=pos.x;result.y=pos.y;result.z=pos.z;
        auto offset=[](uint8_t* fsm,size_t field)->uint32_t {
            if(!fsm) return 0;auto state=*reinterpret_cast<uint8_t**>(fsm+field);
            return state>=fsm&&state<fsm+0x210?static_cast<uint32_t>(state-fsm):0;
        };
        result.attack_state=offset(attack,4);result.attack_pending=offset(attack,8);
        result.movement_state=offset(movement,4);result.movement_pending=offset(movement,8);
        if(attack) {
            result.special_end_event=attack[0x9B3];result.special_result_applied=attack[0x9B5];
            uint8_t* specialResult=nullptr;
            if(battle_ai::native::detail::ReadPointer(attack,0x9BC,specialResult)) {
                battle_ai::native::detail::Read(specialResult,0x14,result.special_result_victim);
                battle_ai::native::detail::Read(specialResult,0x10,result.special_result_damage);
            }
            result.special_owner_flag=unit[0xEC8];
        }
        *output=result;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
extern "C" __declspec(dllexport) int __cdecl SdgoBattleAiGetNavigationSnapshotV1(uint32_t id,SdgoBattleAiNavigationSnapshotV1* output) {
    __try {
        if(!output||output->size<sizeof(*output)||output->version!=1||GetCurrentThreadId()!=g_gameThread)return 0;
        auto unit=FindObject(id);auto found=g_actors.find(unit);
        if(!unit||found==g_actors.end()||unit[0x2B65]!=0||
           *reinterpret_cast<uint32_t*>(unit+0x3238)<3604001||*reinterpret_cast<uint32_t*>(unit+0x3238)>3607000)return 0;
        const auto& actor=found->second;
        SdgoBattleAiNavigationSnapshotV1 result{};
        result.size=sizeof(result);result.version=1;result.object_id=id;result.generation=actor.generation;
        auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
        if(fsm&&actor.sharedTarget==*reinterpret_cast<uint32_t*>(fsm+0x200)) {
            result.shared_target=actor.sharedTarget;result.shared_source=actor.sharedSource;
        }
        result.shared_contact_frames=actor.sharedContactFrames;result.shared_pursuit_frames=actor.sharedPursuitFrames;
        result.exploration_routes=actor.explorationRoutes;result.search_goal_kind=actor.searchGoalKind;
        result.goal_x=actor.searchGoal.x;result.goal_y=actor.searchGoal.y;result.goal_z=actor.searchGoal.z;
        *output=result;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
extern "C" __declspec(dllexport) int __cdecl SdgoBattleAiGetTransportSnapshotV1(uint32_t id,SdgoBattleAiTransportSnapshotV1* output) {
    __try {
        if(!output||output->size<sizeof(*output)||output->version!=1||GetCurrentThreadId()!=g_gameThread)return 0;
        auto unit=FindObject(id);auto found=g_actors.find(unit);
        if(!unit||found==g_actors.end()||!Owned(unit))return 0;
        const auto& actor=found->second;const auto& trip=actor.transport;
        SdgoBattleAiTransportSnapshotV1 result{};
        result.size=sizeof(result);result.version=1;result.object_id=id;result.generation=actor.generation;
        result.phase=trip.phase;result.link_id=trip.id;result.catalog_count=static_cast<uint32_t>(g_transportCatalog.Volumes().size());
        result.catalog_complete=g_transportCatalog.Complete();result.catalog_queries=g_transportCatalog.Queries();result.catalog_saturated=g_transportCatalog.Saturated();
        result.selected=trip.selected;result.activations=trip.activations;result.completed=trip.completed;result.rejected=trip.rejected;result.resumed=trip.resumed;
        result.entry_x=trip.entry.x;result.entry_y=trip.entry.y;result.entry_z=trip.entry.z;
        result.exit_x=trip.exit.x;result.exit_y=trip.exit.y;result.exit_z=trip.exit.z;
        result.activated_x=trip.activated.x;result.activated_y=trip.activated.y;result.activated_z=trip.activated.z;
        result.arrived_x=trip.arrived.x;result.arrived_y=trip.arrived.y;result.arrived_z=trip.arrived.z;
        result.usable_links=static_cast<uint32_t>(actor.cachedTransports.size());result.ground_rejected=actor.transportGroundRejected;
        result.corridor_rejected=actor.transportCorridorRejected;result.search_nodes=static_cast<uint32_t>(actor.mapSearch.Nodes());
        if(trip.volume.scene&&trip.volume.Live()&&battle_ai::Length(Position(unit)-trip.entry)<250) {
            using namespace battle_ai::native;
            uint8_t* scene=nullptr;uint8_t* sphere=nullptr;uint8_t* obb=nullptr;Aabb own{};
            if(detail::ReadPointer(unit,0x34,scene)&&detail::ReadPointer(scene,0xE0,sphere)&&
               detail::ReadPointer(trip.volume.scene,0xD8,obb)&&detail::Read(scene,0xBC,own)) {
                Vec3 center{};float radius=0;
                if(detail::Read(sphere,0x10,center)&&detail::Read(sphere,0x1C,radius)) {
                    result.contact_available=1;result.sphere_x=center.x;result.sphere_y=center.y;result.sphere_z=center.z;result.sphere_radius=radius;
                    result.pad_min_x=trip.volume.bounds.min.x;result.pad_min_y=trip.volume.bounds.min.y;result.pad_min_z=trip.volume.bounds.min.z;
                    result.pad_max_x=trip.volume.bounds.max.x;result.pad_max_y=trip.volume.bounds.max.y;result.pad_max_z=trip.volume.bounds.max.z;
                    result.owner_min_x=own.min.x;result.owner_min_y=own.min.y;result.owner_min_z=own.min.z;
                    result.owner_max_x=own.max.x;result.owner_max_y=own.max.y;result.owner_max_z=own.max.z;
                    result.native_overlap=g_mapSphereObb(reinterpret_cast<const MapBodySphere*>(sphere),obb);
                    void* candidates[128]{};int count=0;
                    auto tree=*reinterpret_cast<void**>(*g_document+0x82C4);
                    g_navigation.query(tree,candidates,&count,128,&own);result.query_count=static_cast<uint32_t>(count);
                    if(count>=0&&count<128)for(int i=0;i<count;++i)if(candidates[i]==trip.volume.scene)result.query_contains_pad=1;
                }
            }
        }
        *output=result;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
extern "C" __declspec(dllexport) int __cdecl SdgoBattleAiGetPhaseGeometrySnapshotV1(
    uint32_t id,SdgoBattleAiPhaseGeometrySnapshotV1* output) {
    __try {
        if(!output||output->size<sizeof(*output)||output->version!=1||
           GetCurrentThreadId()!=g_gameThread)return 0;
        SdgoBattleAiPhaseGeometrySnapshotV1 result{};
        result.size=sizeof(result);result.version=1;
        result.global_revision=
            g_phaseGeometryRevision.load(std::memory_order_acquire);
        result.hide_count=g_phaseHideCount.load(std::memory_order_relaxed);
        result.unhide_count=g_phaseUnhideCount.load(std::memory_order_relaxed);
        result.last_object_id=
            g_phaseLastObject.load(std::memory_order_relaxed);
        result.last_action=
            g_phaseLastAction.load(std::memory_order_relaxed);
        result.last_event_sequence=
            g_phaseEventSequence.load(std::memory_order_acquire);
        if(id) {
            auto unit=FindObject(id);auto found=g_actors.find(unit);
            if(unit&&found!=g_actors.end()&&Owned(unit)) {
                const auto& actor=found->second;
                result.actor_valid=1;
                result.actor_generation=actor.generation;
                result.actor_seen_revision=actor.phaseGeometryRevision;
                result.actor_invalidation_count=actor.phaseInvalidations;
                result.actor_route_rebuild_count=actor.phaseRouteRebuilds;
                result.actor_path_generation=actor.pathGeneration;
                result.route_active=!actor.path.empty()||
                    actor.geometryOwned||actor.transport.active;
            }
        }
        *output=result;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    if(!api||api->version!=SDGO_API_VERSION||
       api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite)||!api->Symbol||!api->HookSite||
       !api->Symbol(mod,"combat.special_melee_completion_layout")) return 0;
    if (!Bind(g_lua.state,"lua_tinker.state") || !Bind(g_lua.gettop,"lua.gettop") ||
        !Bind(g_lua.type,"lua.type") || !Bind(g_lua.tonumber,"lua.tonumber") ||
        !Bind(g_lua.settop,"lua.settop") || !Bind(g_lua.tostring,"lua.tostring") ||
        !Bind(g_lua.pushnumber,"lua.pushnumber") || !Bind(g_lua.pushstring,"lua.pushstring") ||
        !Bind(g_lua.pushcclosure,"lua.pushcclosure") || !Bind(g_lua.gettable,"lua.gettable") ||
        !Bind(g_lua.settable,"lua.settable") || !Bind(g_lua.newtable,"lua.newtable") ||
        !Bind(g_lua.pcall,"lua.pcall") || !Bind(g_lua.load,"lua.load") ||
        !Bind(g_document,"game.document") || !Bind(g_findObject,"um.find_object") || !Bind(g_route,"navigation.route") ||
        !Bind(g_navigationNode,"navigation.node_for_point") || !Bind(g_navigationNodeAt,"navigation.node_at") ||
        !Bind(g_startPoint,"um.find_start_point") ||
        !Bind(g_setIntent,"npc.set_attack_intent") || !Bind(g_selectWeapon,"combat.select_weapon") || !Bind(g_stop,"npc.move_stop") ||
        !Bind(g_setGoal,"npc.set_move_goal") || !Bind(g_setMoveType,"npc.set_move_type") || !Bind(g_setJumpEnabled,"npc.set_jump_enabled") || !Bind(g_delete,"crt.operator_delete") ||
        !Bind(g_navigation.query,"collision.query") || !Bind(g_navigation.segmentAabb,"collision.segment_aabb") ||
        !Bind(g_navigation.terrainRay,"collision.terrain_ray") || !Bind(g_terrainTriangle,"collision.terrain_triangle") || !Bind(g_navigation.rayObb,"collision.ray_obb") ||
        !Bind(g_navigation.raySphere,"collision.ray_sphere") ||
        !Bind(g_mapSphereObb,"collision.sphere_obb_overlap") ||
        !Bind(g_mapSphereObbContact,"collision.sphere_obb_contact")) return 0;
    g_terrainRays.Bind(g_navigation.terrainRay,g_terrainTriangle);
    if(!Bind(g_terrainHeight,"navigation.terrain_height")||!Bind(g_terrainNormal,"navigation.terrain_normal"))return 0;
    g_terrainSupport.Bind(g_terrainHeight,g_terrainNormal);
    g_navigation.support=&PhysicsSupport;
    g_navigation.terrainRay=reinterpret_cast<battle_ai::native::TerrainRayFn>(&IndexedTerrainRay);
    if(!Bind(g_app,"game.app")||!Bind(g_queueSwing,"combat.queue_melee_swing")||!Bind(g_faceTarget,"npc.face_target")||!Bind(g_rotate,"unit.rotate_y")) return 0;
    if(!Bind(g_contactReturn,"combat.melee_contact_return")||!Bind(g_queueState,"fsm.queue_state")||
       !Bind(g_aiBoostPauseReturn,"npc.ai_boost_pause_return")) return 0;
    if(!Bind(g_playerSkillGate,"combat.player_sp_gate")) return 0;
    if(!Bind(g_canonical,"um.object_id_from_pilot")) return 0;
    if(!Bind(g_loadoutNative.setUnitLevel,"loadout.set_unit_level")||!Bind(g_loadoutNative.applySkillPart,"loadout.apply_skill_part")) return 0;
    std::wstring dir = api->ModDirectory(mod);
    g_script = dir + L"\\battle_ai.lua"; g_config = dir + L"\\config.ini";
    g_universalScript=dir+L"\\universal_ai.lua";
    g_plainProjectileEntry=api->Symbol(mod,"combat.projectile_update");
    std::string check;
    if (!Read(g_script,check)||!Read(g_universalScript,check)) api->Log(mod,SDGO_LOG_ERROR,"missing AI policy script");
    if (!api->HookCode(mod,"npc.think",reinterpret_cast<void*>(&Think),reinterpret_cast<void**>(&g_think)) ||
        !api->HookCode(mod,"npc.init_ai",reinterpret_cast<void*>(&Init),reinterpret_cast<void**>(&g_init)) ||
        !api->HookCode(mod,"map.phase_hide",reinterpret_cast<void*>(&PhaseHide),reinterpret_cast<void**>(&g_phaseHide)) ||
        !api->HookCode(mod,"map.phase_unhide",reinterpret_cast<void*>(&PhaseUnhide),reinterpret_cast<void**>(&g_phaseUnhide)) ||
        !api->HookCode(mod,"npc.dispatch_ai",reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&g_dispatch)) ||
        !api->HookCode(mod,"unit.effect_active",reinterpret_cast<void*>(&EffectActive),reinterpret_cast<void**>(&g_effectActive)) ||
        !api->HookCode(mod,"npc.ranged_ray",reinterpret_cast<void*>(&RangedRay),reinterpret_cast<void**>(&g_ray)) ||
        !api->HookCode(mod,"npc.attack_intent",reinterpret_cast<void*>(&AttackIntent),reinterpret_cast<void**>(&g_intent)) ||
        !api->HookCode(mod,"npc.facing_update",reinterpret_cast<void*>(&FacingUpdate),reinterpret_cast<void**>(&g_facingUpdate)) ||
        !api->HookCode(mod,"combat.ranged_shot",reinterpret_cast<void*>(&RangedShot),reinterpret_cast<void**>(&g_shot)) ||
        !api->HookCode(mod,"combat.fire_receipts",reinterpret_cast<void*>(&FireReceipts),reinterpret_cast<void**>(&g_fireReceipts)) ||
        !api->HookCode(mod,"combat.attack_admission",reinterpret_cast<void*>(&AttackAdmission),reinterpret_cast<void**>(&g_attackAdmission)) ||
        !api->HookCode(mod,"game.special_notice",reinterpret_cast<void*>(&SpecialNotice),reinterpret_cast<void**>(&g_specialNotice)) ||
        !api->HookCode(mod,"game.special_melee_notice",reinterpret_cast<void*>(&MeleeNotice),reinterpret_cast<void**>(&g_meleeNotice)) ||
        !api->HookCode(mod,"game.special_ranged_notice",reinterpret_cast<void*>(&RangedNotice),reinterpret_cast<void**>(&g_rangedNotice)) ||
        !api->HookCode(mod,"game.sp_state_notice",reinterpret_cast<void*>(&SpStateNotice),reinterpret_cast<void**>(&g_spStateNotice)) ||
        !api->HookCode(mod,"game.skill_state_notice",reinterpret_cast<void*>(&SkillNotice),reinterpret_cast<void**>(&g_skillNotice)) ||
        !api->HookCode(mod,"npc.tracking_entry",reinterpret_cast<void*>(&TrackingEntry),reinterpret_cast<void**>(&g_trackingEntry)) ||
        !api->HookCode(mod,"npc.tracking_update",reinterpret_cast<void*>(&TrackingUpdate),reinterpret_cast<void**>(&g_trackingUpdate)) ||
        !api->HookCode(mod,"npc.swing_entry",reinterpret_cast<void*>(&SwingEntry),reinterpret_cast<void**>(&g_swingEntry)) ||
        !api->HookCode(mod,"npc.swing_update",reinterpret_cast<void*>(&SwingUpdate),reinterpret_cast<void**>(&g_swingUpdate)) ||
        !api->HookCode(mod,"um.deliver_message",reinterpret_cast<void*>(&DeliverMessage),reinterpret_cast<void**>(&g_message)) ||
        !api->HookCode(mod,"combat.projectile_update",reinterpret_cast<void*>(&ProjectileUpdate),reinterpret_cast<void**>(&g_projectile)) ||
        !api->HookSite(mod,"combat.special_melee_completion_gate",reinterpret_cast<void*>(&SpecialCompletionGate),
                       &g_specialCompletionGateOriginal)) return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: capability-driven local battle bots; native projectiles and collisions");
    return 1;
}
