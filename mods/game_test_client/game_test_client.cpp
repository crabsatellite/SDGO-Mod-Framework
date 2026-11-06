#include "quiet_sockets.h"
#include "quiet_audio.h"
#include "quiet_graphics.h"
#include "quiet_wait_room_messages.h"
#include "../game_test_shared/runner.h"
#include "native_actions.h"
#include "channel_actions.h"
#include "battleship_actions.h"
#include "battleship_deck_actions.h"
#include "hangar_navigation.h"
#include "page_test_actions.h"
#include "operator_catalog_actions.h"
#include "ai_catalog_actions.h"
#include "storage_test_actions.h"
#include "capsule_catalog_actions.h"
#include "battle_combat_actions.h"
#include "apsalus_transform_probe.h"
#include "unit_presentations_actions.h"
#include "blueprint_actions.h"
#include "unit_acquisition_actions.h"
#include "capsule_draw_actions.h"
#include "unit_select_actions.h"
#include "active_selection_observer.h"
#include "shop_catalog_actions.h"
#include "sdgo/net_route_observer.h"
#include "mission_flow_actions.h"
#include "sdgo/mission_ai_client_observer.h"
#include "sdgo/battle_ai_observer.h"
#include "../mission_behavior/observer.h"
#include "gift_actions.h"
#include "room_actions.h"
#include "room_title_actions.h"
#include "room_style_preview_actions.h"
#include "tutorial_actions.h"
#include "onboarding_actions.h"
#include "onboarding_gate.h"
#include "friend_session.h"
#include "fps_test_actions.h"

#include "sdgo/oc_custom_client_observer.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "scoreboard_actions.h"
#include "battle_ai_behavior_probe.h"
#include "native_death_probe.h"
#include "native_player_combat_probe.h"
#include "review_camera.h"
#include "native_data_guard_probe.h"
#include "native_record_lifecycle_probe.h"
#include "sealed_room_title.h"
#include <cwchar>
#include <array>

namespace {
static_assert(static_cast<uint32_t>(TutorialActions::kLeftGame) == sdgo_game_test::kTutorialLeftGame, "tutorial observation contract changed");
static_assert(static_cast<uint32_t>(OnboardingActions::kStarterAccepted) == sdgo_game_test::kStarterAccepted, "starter observation contract changed");
static_assert(static_cast<uint32_t>(OnboardingActions::kInitialLoadingAccepted) == sdgo_game_test::kInitialLoadingAccepted, "initial-loading observation contract changed");
using AppMethod = void(__thiscall*)(uint8_t*);
using FindWindowFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
using MessageBoxWFn=int(WINAPI*)(HWND,LPCWSTR,LPCWSTR,UINT);
using MessageBoxAFn=int(WINAPI*)(HWND,LPCSTR,LPCSTR,UINT);
AppMethod originalUpdate;
using ErrorNameFn = const char*(__cdecl*)(int);
ErrorNameFn originalErrorName = nullptr;
volatile LONG nativeErrorSequence=0,nativeLastError=0;
const char* __cdecl ObserveNativeErrorName(int code) {
    const char* name=originalErrorName(code);
    if (code && InterlockedExchange(&nativeLastError,code)!=code) {
        InterlockedIncrement(&nativeErrorSequence);
        char safeName[160]{};size_t used=0;
        if (name) for (const unsigned char* p=reinterpret_cast<const unsigned char*>(name);*p && used+5<sizeof(safeName);++p) {
            if (*p>=32 && *p<127) safeName[used++]=static_cast<char>(*p);
            else used+=sprintf_s(safeName+used,sizeof(safeName)-used,"\\x%02X",static_cast<unsigned>(*p));
        }
        char message[224];sprintf_s(message,"native error code=%04X name=%s",code,name ? safeName:"unknown");
        sdgo_test::Observe(message);
    }
    return name;
}
FindWindowFn originalFindWindow;
MessageBoxWFn originalMessageBoxW;
MessageBoxAFn originalMessageBoxA;
DWORD mainThread;
NativeActions actions;
ChannelActions channels;
BattleShipActions battleships;
BattleShipDeckActions decks;
HangarNavigation hangarNavigation;
BlueprintActions blueprint;
BlueprintActions::Config blueprintConfig{};
std::vector<uint32_t> nativeIdProbeIds;
uint32_t nativeIdProbeWitness=0;
bool nativeIdProbeRequireAbsent=false;
std::vector<uint32_t> nativeRecordLifecycleIds;
NativeRecordLifecycleProbe nativeRecordLifecycle;
PageTestActions pages;
OperatorCatalogActions operatorCatalog;
AiCatalogActions aiCatalog;
StorageTestActions storage;
CapsuleCatalogActions capsules;
BattleCombatActions battleCombat;
NativeDataGuardProbe nativeDataGuardProbe;
QuietWaitRoomMessages quietWaitRoomMessages;
CapsuleDrawActions collection;
UnitSelectActions unitSelection;
ActiveSelectionObserver activeSelection;
ActiveSelectionObserver::Snapshot selectionBeforeRound;
bool randomLoadout=false;
ShopCatalogActions shopCatalog;
bool testShopCatalog=false;
bool shopVisualOnly=false;
MissionFlowActions mission;
uint16_t missionMode=0;
uint32_t missionGameModeId=0;
uint32_t missionBehaviorProfile=0; // 1: OC, 2: GCO
uint32_t missionRespawns=0; // 1: player quota, 2: infinite; explicit native probe scope
uint32_t observedMissionGeneration=0,observedMissionRoom=0;
uint32_t observedMissionEndBaseline=0;
uint64_t observedMissionBossSpecialBaseline=0;
uint64_t observedMissionStartedAt=0;
SdgoMissionAiClientGetSnapshotV1Fn getMissionSnapshot=nullptr;
SdgoMissionBehaviorClientGetStatusV1Fn getMissionBehaviorStatus=nullptr;
SdgoBattleAiGetMissionBossSpecialV1Fn getMissionBossSpecial=nullptr;
uint64_t playUnitUid=0;
// OC six-stat acceptance: allocate ocAllocatePoints normal points to stat
// ocAllocateStat (0 speed .. 5 special) on the play unit before the battle.
int ocAllocateStat=-1,ocAllocatePoints=0;
// Open the original factory upgrade page at the end and check the OC panel.
bool ocPanel=false;
// Also press the panel's OC challenge (it can change the unit's OC level).
bool ocChallenge=false;
bool collectionDraw=false;
bool collectionPreviewOnly=false;
uint32_t collectionExpectedStatus=0;
std::vector<SdgoHangarGameTestUnitV1> collectionBeforeUnits;
const void* roomListVtable=nullptr;
const void* modalLoadingVtable=nullptr;
GiftActions gifts;
RoomActions rooms;
RoomJoinActions roomJoin;
PvpSessionActions pvpSession;
RoomTitleActions roomTitles;
bool roomTitlesOnly=false;
uint32_t roomStylePreview=0;
RoomStylePreviewActions roomStylePreviewActions;
TutorialActions tutorial;
OnboardingActions onboarding;
friends::FriendActions friendActions;
friends::FriendExtraActions friendExtras;
friends::FriendSession friendSession;
friends::FriendInviteSender friendInviteSender;
friends::FriendInviteReceiver friendInviteReceiver;
friends::FriendRoomActions friendRoomActions;
SdgoPvpAiClientGetSnapshotV1Fn getSnapshot;
SdgoPvpAiClientGetLuaStatusV1Fn getLuaStatus;
SdgoPvpAiClientGetStartStatusV1Fn getStartStatus;
SdgoPvpAiClientGetScoreboardStatusV1Fn getScoreboardStatus;
ScoreboardActions scoreboardInput;
BattleAiBehaviorProbe battleAiBehavior;
NativeDeathProbe nativeDeathProbe;
NativePlayerCombatProbe nativePlayerCombat;
uint32_t scoreboardCombatSequence = 0, scoreboardCombatChanges = 0;
SdgoPvpAiClientScoreboardStatusV1 scoreboardLast{};
char accountName[32] = {}, accountPassword[128] = {}, roomTitle[21] = {};
unsigned loginAttempts = 0, retryCount = 1;
uint32_t loginReplySeen = 0;
bool expectFirstFailure = false;
bool passwordIsMd5 = false;
bool importedExistingAccount = false;
bool bootstrapOnly = false;
bool fpsOnly = false;
int fpsRequested = 60;
bool fpsModEnabled = true;
FpsTestActions fpsActions;
bool verifyDirectNetwork = false;
bool verifyRoutedNetwork = false;
bool verifyQuietRuntime = false;
bool reviewFootage = false;
bool rosterClosedForReview = false;
ReviewCamera reviewCamera;
bool quietLoopback = false;
QuietSockets quietSockets;
QuietAudio quietAudio;
QuietGraphics quietGraphics;
bool captureNativeFrames=false;
bool unitPagesOnly=false;
bool activityDrawOnce=false;
bool testUnitPresentations=false;
bool captureUnitPortraits=false;
UnitPresentationsActions unitPresentations;
UnitAcquisitionActions unitAcquisition;
UnitAcquisitionActions::Config unitAcquisitionConfig{};
void PublishQuietSocketReceipt(bool force=false);
bool battleshipOnly = false, giftRequestsOnly = false, deckOnly = false, pagesOnly = false, friendsOnly = false;
bool hangarPagesOnly = false;
uint64_t deckUnitUids[4]{};
char giftRecipient[17]{};
uint32_t giftSenderPilot = 0, giftRecipientPilot = 0, giftCount = 0, giftSent = 0;
uint32_t giftPages[4]{}, giftPositions[4]{};
char giftBodies[4][301]{};
uint64_t rejectedAt = 0;
bool nicknameRequested = false, nicknameAccepted = false;
uint32_t nicknameReplySeen = 0;
bool roomRequested = false, startRequested = false, leaveRequested = false;
uint32_t startSequence = 0;
uint32_t activeBattleStartSequence = 0;
uint32_t endSequence = 0, leaveSequence = 0;
uint32_t authenticatedPilot = 0;
uint32_t roomCreationSequence = 0;
uint32_t roomReplySequence = 0;
bool endObserved = false;
std::vector<uint16_t> requestedModes;
size_t modeIndex = 0;
unsigned expectedAi = 3;
unsigned expectedAllies=1,expectedEnemies=2;
bool randomTeams=false;
bool sameRoomRematch=false;
unsigned sameRoomRound=0;
uint32_t sameRoomId=0;
uint16_t RequestedMode() { return missionMode?missionMode:requestedModes.at(modeIndex); }
struct MotionSample {
    uint32_t pilot, object, npc;
    float x, z;
    bool moved;
};
std::vector<MotionSample> motionSamples;

bool LoadPlan() {
    const std::wstring job = sdgo_test::directory + L"\\run.ini";
    const std::wstring blueprintIni=sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini";
    const auto probeCount=GetPrivateProfileIntW(L"settings",L"NativeIdProbeCount",0,blueprintIni.c_str());
    const auto probeChunks=GetPrivateProfileIntW(L"settings",L"NativeIdProbeChunks",0,blueprintIni.c_str());
    nativeIdProbeWitness=GetPrivateProfileIntW(L"settings",L"NativeIdProbeWitness",0,blueprintIni.c_str());
    nativeIdProbeRequireAbsent=GetPrivateProfileIntW(L"settings",L"NativeIdProbeRequireAbsent",0,blueprintIni.c_str())==1;
    if(probeCount>3072 || probeChunks!=(probeCount+255)/256 ||
       ((probeCount!=0)!=(nativeIdProbeWitness!=0)) || nativeIdProbeWitness>9999999)return false;
    nativeIdProbeIds.clear();
    for(uint32_t index=0;index<probeChunks;++index) {
        wchar_t key[48]{},value[4096]{};swprintf_s(key,L"NativeIdProbeChunk%u",index);
        const auto length=GetPrivateProfileStringW(L"settings",key,L"",value,4096,blueprintIni.c_str());
        if(!length || length>=4095)return false;
        const auto before=nativeIdProbeIds.size();const wchar_t* cursor=value;
        while(*cursor) {
            wchar_t* end=nullptr;errno=0;const auto id=wcstoul(cursor,&end,10);
            if(errno || end==cursor || !id || id>9999999 || (*end && *end!=L',') ||
                (!nativeIdProbeIds.empty() && id<=nativeIdProbeIds.back()))return false;
            nativeIdProbeIds.push_back(id);
            if(!*end)break;
            cursor=end+1;if(!*cursor)return false;
        }
        if(nativeIdProbeIds.size()-before!=std::min<uint32_t>(256,probeCount-index*256))return false;
    }
    if(nativeIdProbeIds.size()!=probeCount)return false;
    const auto lifecycleCount=GetPrivateProfileIntW(L"settings",L"NativeRecordLifecycleCount",0,blueprintIni.c_str());
    if(lifecycleCount>16)return false;
    nativeRecordLifecycleIds.clear();
    wchar_t lifecycleIds[256]{};
    const auto lifecycleLength=GetPrivateProfileStringW(L"settings",L"NativeRecordLifecycleIds",L"",lifecycleIds,256,blueprintIni.c_str());
    if(lifecycleLength>=255 || ((lifecycleCount!=0)!=(lifecycleLength!=0)))return false;
    for(const wchar_t* cursor=lifecycleIds;*cursor;) {
        wchar_t* end=nullptr;errno=0;const auto id=wcstoul(cursor,&end,10);
        if(errno || end==cursor || !id || id>9999999 || (*end && *end!=L',') ||
           (!nativeRecordLifecycleIds.empty() && id<=nativeRecordLifecycleIds.back()))return false;
        nativeRecordLifecycleIds.push_back(id);if(!*end)break;
        cursor=end+1;if(!*cursor)return false;
    }
    if(nativeRecordLifecycleIds.size()!=lifecycleCount)return false;
    blueprintConfig.recipe=GetPrivateProfileIntW(L"settings",L"BlueprintRecipe",0,blueprintIni.c_str());
    wchar_t bookText[32]{};GetPrivateProfileStringW(L"settings",L"BlueprintBookUid",L"0",bookText,32,blueprintIni.c_str());
    blueprintConfig.book=_wcstoui64(bookText,nullptr,10);
    if((blueprintConfig.recipe==0)!=(blueprintConfig.book==0))return false;
    blueprintConfig.material_count=GetPrivateProfileIntW(L"settings",L"BlueprintMaterialCount",0,blueprintIni.c_str());
    blueprintConfig.synthesize=GetPrivateProfileIntW(L"settings",L"BlueprintSynthesize",0,blueprintIni.c_str())==1;
    blueprintConfig.output_unit=GetPrivateProfileIntW(L"settings",L"BlueprintOutputUnit",0,blueprintIni.c_str());
    if(blueprintConfig.synthesize && (!blueprintConfig.material_count || !blueprintConfig.output_unit))return false;
    if(!blueprintConfig.synthesize && blueprintConfig.output_unit)return false;
    if(blueprintConfig.material_count && (blueprintConfig.material_count<3 || blueprintConfig.material_count>5 || !blueprintConfig.recipe))return false;
    for(uint32_t i=0;i<blueprintConfig.material_count;++i) {
        wchar_t key[48]{};swprintf_s(key,L"BlueprintMaterialUnit%u",i);
        blueprintConfig.material_units[i]=GetPrivateProfileIntW(L"settings",key,0,blueprintIni.c_str());
        swprintf_s(key,L"BlueprintMaterialUid%u",i);wchar_t value[32]{};
        GetPrivateProfileStringW(L"settings",key,L"0",value,32,blueprintIni.c_str());
        blueprintConfig.material_uids[i]=_wcstoui64(value,nullptr,10);
        if(!blueprintConfig.material_units[i] || !blueprintConfig.material_uids[i])return false;
        for(uint32_t j=0;j<i;++j)if(blueprintConfig.material_uids[i]==blueprintConfig.material_uids[j])return false;
    }
    unitPagesOnly=GetPrivateProfileIntW(L"settings",L"UnitPagesOnly",0,
        (sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini").c_str())==1;
    captureNativeFrames=GetPrivateProfileIntW(L"settings",L"CaptureNativeFrames",0,
        (sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini").c_str())==1;
    testUnitPresentations=GetPrivateProfileIntW(L"settings",L"UnitPresentations",0,
        (sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini").c_str())==1;
    captureUnitPortraits=GetPrivateProfileIntW(L"settings",L"UnitPortraits",0,
        (sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini").c_str())==1;
    collectionPreviewOnly=GetPrivateProfileIntW(L"settings",L"CollectionPreviewOnly",0,
        (sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini").c_str())==1;
    collectionExpectedStatus=GetPrivateProfileIntW(L"settings",L"CollectionExpectedStatus",0,
        (sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini").c_str());
    if(collectionExpectedStatus!=0 && collectionExpectedStatus!=6040)return false;
    const std::wstring acquisitionIni=sdgo_test::directory+L"\\..\\mods\\game_test_client\\mod.ini";
    auto capsuleTarget=collection.ExpectedTarget();
    capsuleTarget.machine=GetPrivateProfileIntW(L"settings",L"CapsulePreviewMachine",capsuleTarget.machine,acquisitionIni.c_str());
    capsuleTarget.price=GetPrivateProfileIntW(L"settings",L"CapsulePreviewPrice",capsuleTarget.price,acquisitionIni.c_str());
    capsuleTarget.currency=GetPrivateProfileIntW(L"settings",L"CapsulePreviewCurrency",capsuleTarget.currency,acquisitionIni.c_str());
    if(!collection.ConfigureTarget(capsuleTarget))return false;
    activityDrawOnce=GetPrivateProfileIntW(L"settings",L"ActivityDrawOnce",0,acquisitionIni.c_str())==1;
    const auto rewardCount=GetPrivateProfileIntW(L"settings",L"ActivityRewardCount",0,acquisitionIni.c_str());
    if(rewardCount>128 || (activityDrawOnce && rewardCount<2) || (!activityDrawOnce && rewardCount))return false;
    std::vector<CapsuleDrawActions::Reward> activityRewards;
    for(unsigned i=0;i<rewardCount;++i) {
        wchar_t key[64]{};swprintf_s(key,L"ActivityRewardCategory%u",i);
        const auto category=GetPrivateProfileIntW(L"settings",key,0,acquisitionIni.c_str());
        swprintf_s(key,L"ActivityRewardAmount%u",i);
        const auto amount=GetPrivateProfileIntW(L"settings",key,0,acquisitionIni.c_str());
        activityRewards.push_back({category,amount});
    }
    if(!collection.ConfigureRewards(activityRewards))return false;
    unitAcquisitionConfig.machine_kind=GetPrivateProfileIntW(L"settings",L"UnitAcquisitionKind",0,acquisitionIni.c_str());
    unitAcquisitionConfig.machine_id=GetPrivateProfileIntW(L"settings",L"UnitAcquisitionMachine",0,acquisitionIni.c_str());
    unitAcquisitionConfig.unit_id=GetPrivateProfileIntW(L"settings",L"UnitAcquisitionUnit",0,acquisitionIni.c_str());
    wchar_t networkPolicy[24]{};
    GetPrivateProfileStringW(L"run",L"network_policy",L"legacy",networkPolicy,24,job.c_str());
    verifyRoutedNetwork=!wcscmp(networkPolicy,L"routed_loopback");
    verifyDirectNetwork = !wcscmp(networkPolicy,L"direct") || verifyRoutedNetwork;
    verifyQuietRuntime=GetPrivateProfileIntW(L"run",L"quiet",0,job.c_str())!=0;
    reviewFootage=GetPrivateProfileIntW(L"run",L"review_footage",0,job.c_str())!=0;
    quietLoopback=verifyQuietRuntime && GetPrivateProfileIntW(L"run",L"quiet_loopback",0,job.c_str())==1;
    fpsOnly=sdgo_test::suiteId=="native-fps-limit";
    if(collectionPreviewOnly && fpsOnly)return false;
    if (fpsOnly) {
        fpsRequested=GetPrivateProfileIntW(L"run",L"fps_cap",0,job.c_str());
        const int enabled=GetPrivateProfileIntW(L"run",L"fps_mod_enabled",-1,job.c_str());
        fpsModEnabled=enabled==1;
        // The FPS-only suite stops on the real login page. No account file,
        // credentials or authentication binding is required. The native client
        // still performs its ordinary transport connection to the backend.
        verifyDirectNetwork=false;
        return verifyQuietRuntime && fpsRequested>=1 && fpsRequested<=1000 && (enabled==0 || enabled==1);
    }
    bootstrapOnly = GetPrivateProfileIntW(L"run", L"bootstrap_only", 0, job.c_str()) != 0;
    battleshipOnly = GetPrivateProfileIntW(L"run", L"battleship_only", 0, job.c_str()) != 0;
    giftRequestsOnly = GetPrivateProfileIntW(L"run", L"gift_requests_only", 0, job.c_str()) != 0;
    deckOnly = GetPrivateProfileIntW(L"run", L"deck_only", 0, job.c_str()) != 0;
    pagesOnly = GetPrivateProfileIntW(L"run", L"pages_only", 0, job.c_str()) != 0;
    friendsOnly = GetPrivateProfileIntW(L"run", L"friends_only", 0, job.c_str()) != 0;
    roomTitlesOnly = GetPrivateProfileIntW(L"run", L"room_titles_only", 0, job.c_str()) != 0;
    wchar_t stylePreview[16]{};
    GetPrivateProfileStringW(L"run",L"room_style_preview",L"",stylePreview,16,job.c_str());
    if (stylePreview[0]) {
        roomStylePreview = !wcscmp(stylePreview,L"pvp") ? 1 : !wcscmp(stylePreview,L"mission") ? 2 : 0;
        if (!roomStylePreview || !roomTitlesOnly) return false;
    }
    wchar_t shopAssertions[128]{},shopDisplay[128]{};
    GetPrivateProfileStringW(L"run",L"shop_assertions_path",L"",shopAssertions,128,job.c_str());
    GetPrivateProfileStringW(L"run",L"shop_display_path",L"",shopDisplay,128,job.c_str());
    testShopCatalog=shopAssertions[0] || shopDisplay[0];
    shopVisualOnly=GetPrivateProfileIntW(L"run",L"shop_visual_only",0,job.c_str())!=0;
    if(shopVisualOnly && !testShopCatalog)return false;
    if(testShopCatalog && (!pagesOnly || wcscmp(shopAssertions,L"inputs/shop-assertions.ini") ||
        wcscmp(shopDisplay,L"inputs/shop-runtime-display.ini")))return false;
    collectionDraw=GetPrivateProfileIntW(L"run",L"collection_draw",0,job.c_str())!=0;
    wchar_t pageGroup[16]{};
    const auto pageGroupChars=GetPrivateProfileStringW(L"run",L"page_group",L"",pageGroup,16,job.c_str());
    if(pageGroupChars) {
        if(pageGroupChars>=15 || wcscmp(pageGroup,L"hangar") || !pagesOnly || testShopCatalog || collectionDraw ||
           collectionPreviewOnly || testUnitPresentations || captureUnitPortraits || unitAcquisitionConfig.machine_kind)return false;
        hangarPagesOnly=true;
    }
    if(collectionDraw && !pagesOnly)return false;
    if(collectionPreviewOnly && (!pagesOnly || collectionDraw))return false;
    if(activityDrawOnce && (!unitPagesOnly || !collectionPreviewOnly || collectionDraw || !testUnitPresentations))return false;
    if((capsuleTarget.machine!=990001 || capsuleTarget.price!=100000 || capsuleTarget.currency!=0) &&
        (!collectionPreviewOnly || collectionDraw))return false;
    if(unitAcquisitionConfig.machine_kind && (!pagesOnly || collectionDraw || collectionPreviewOnly ||
        !testUnitPresentations || !unitAcquisitionConfig.machine_id || !unitAcquisitionConfig.unit_id ||
        (unitAcquisitionConfig.machine_kind!=UnitAcquisitionActions::kFixedMachine &&
         unitAcquisitionConfig.machine_kind!=UnitAcquisitionActions::kCustomMachine)))return false;
    missionMode=static_cast<uint16_t>(GetPrivateProfileIntW(L"run",L"mission_mode",0,job.c_str()));
    if(missionMode && (missionMode!=5 && missionMode!=8))return false;
    wchar_t missionProfile[8]{};
    GetPrivateProfileStringW(
        L"run",
        L"mission_behavior_profile",
        L"",
        missionProfile,
        static_cast<DWORD>(std::size(missionProfile)),
        job.c_str());
    missionGameModeId=static_cast<uint32_t>(
        GetPrivateProfileIntW(
            L"run",
            L"mission_game_mode_id",
            0,
            job.c_str()));
    missionBehaviorProfile=
        !wcscmp(missionProfile,L"oc") ? 1u :
        !wcscmp(missionProfile,L"gco") ? 2u : 0u;
    if(missionProfile[0] || missionGameModeId) {
        if(!missionBehaviorProfile || missionMode!=5 ||
           (missionGameModeId!=88006 && missionGameModeId!=88009))
            return false;
    }
    wchar_t playUid[32]{};
    GetPrivateProfileStringW(L"run",L"play_unit_uid",L"",playUid,32,job.c_str());
    if(playUid[0]) {
        if(wcsspn(playUid,L"0123456789")!=wcslen(playUid))return false;
        wchar_t* end=nullptr;errno=0;playUnitUid=_wcstoui64(playUid,&end,10);
        if(!playUnitUid || errno || !end || *end)return false;
    }
    wchar_t ocAllocate[16]{};
    GetPrivateProfileStringW(L"run",L"oc_allocate",L"",ocAllocate,16,job.c_str());
    if(ocAllocate[0]) {
        if(!playUnitUid || wcslen(ocAllocate)<3 || wcslen(ocAllocate)>4 || ocAllocate[0]<L'0' || ocAllocate[0]>L'5' ||
           ocAllocate[1]!=L':' || wcsspn(ocAllocate+2,L"0123456789")!=wcslen(ocAllocate+2))return false;
        ocAllocateStat=ocAllocate[0]-L'0';ocAllocatePoints=_wtoi(ocAllocate+2);
        if(ocAllocatePoints<1 || ocAllocatePoints>20)return false;
    }
    ocPanel=GetPrivateProfileIntW(L"run",L"oc_panel",0,job.c_str())!=0;
    ocChallenge=GetPrivateProfileIntW(L"run",L"oc_challenge",0,job.c_str())!=0;
    if(ocChallenge && !ocPanel && ocAllocateStat<0)return false;
    if(ocPanel && (!playUnitUid || (ocAllocateStat<0 && !hangarPagesOnly)))return false;
    if(testUnitPresentations && !playUnitUid)return false;
    if(unitPagesOnly && (!pagesOnly || (!testUnitPresentations && !blueprintConfig.recipe && nativeIdProbeIds.empty() && nativeRecordLifecycleIds.empty()) || collectionDraw || testShopCatalog))return false;
    if(!nativeRecordLifecycleIds.empty() && (!verifyQuietRuntime || !unitPagesOnly || !pagesOnly || !playUnitUid ||
        testUnitPresentations || !nativeIdProbeIds.empty() || blueprintConfig.recipe || unitAcquisitionConfig.machine_kind ||
        collectionPreviewOnly || activityDrawOnce || testShopCatalog || captureUnitPortraits || missionMode))return false;
    if(!nativeIdProbeIds.empty() && (!unitPagesOnly || !pagesOnly || !playUnitUid || testUnitPresentations ||
        blueprintConfig.recipe || captureUnitPortraits || unitAcquisitionConfig.machine_kind || collectionPreviewOnly ||
        collectionDraw || activityDrawOnce || testShopCatalog || roomTitlesOnly))return false;
    if(blueprintConfig.recipe && (!unitPagesOnly || !pagesOnly || !playUnitUid || testUnitPresentations ||
            captureUnitPortraits || unitAcquisitionConfig.machine_kind || collectionPreviewOnly || collectionDraw ||
            activityDrawOnce || testShopCatalog || roomTitlesOnly))return false;
    if(captureUnitPortraits && (!testUnitPresentations || !captureNativeFrames))return false;
    if (static_cast<unsigned>(bootstrapOnly) + battleshipOnly + giftRequestsOnly + deckOnly + pagesOnly + friendsOnly + roomTitlesOnly > 1) return false;
    if (friendsOnly && (missionMode || playUnitUid || collectionDraw || testShopCatalog))return false;
    wchar_t relative[512] = {}, nonce[16] = {}, user[32] = {}, password[128] = {};
    GetPrivateProfileStringW(L"run", L"account_path", L"private/account.ini", relative, 512, job.c_str());
    std::wstring accountPath = relative;
    if (accountPath.find(L':') == std::wstring::npos && accountPath[0] != L'\\')
        accountPath = sdgo_test::directory + L"\\" + accountPath;
    GetPrivateProfileStringW(L"account", L"username", L"", user, 32, accountPath.c_str());
    GetPrivateProfileStringW(L"account", L"password", L"", password, 128, accountPath.c_str());
    wchar_t passwordFormat[16] = {};
    GetPrivateProfileStringW(L"account", L"password_format", L"plain", passwordFormat, 16, accountPath.c_str());
    wchar_t registrationState[32] = {};
    GetPrivateProfileStringW(L"registration", L"state", L"", registrationState, 32, accountPath.c_str());
    importedExistingAccount = !wcscmp(registrationState, L"existing_account");
    passwordIsMd5 = !wcscmp(passwordFormat, L"md5");
    if (!passwordIsMd5 && wcscmp(passwordFormat, L"plain")) return false;
    if (passwordIsMd5) {
        if (!importedExistingAccount) return false;
    }
    if (!user[0] || !password[0] || wcslen(user) > 14 || wcslen(password) > 64) return false;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, user, -1, accountName, sizeof(accountName), nullptr, nullptr) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, accountPassword, sizeof(accountPassword), nullptr, nullptr)) return false;
    SecureZeroMemory(password, sizeof(password));
    if (deckOnly) {
        for (unsigned i=0; i<4; ++i) {
            wchar_t key[32]{}, value[32]{};
            swprintf_s(key,L"unit_uid%u",i);
            GetPrivateProfileStringW(L"deck",key,L"",value,32,accountPath.c_str());
            if (!value[0] || wcsspn(value,L"0123456789") != wcslen(value)) return false;
            wchar_t* end=nullptr;
            deckUnitUids[i]=_wcstoui64(value,&end,10);
            if (!deckUnitUids[i] || !end || *end) return false;
            for (unsigned j=0; j<i; ++j) if (deckUnitUids[j]==deckUnitUids[i]) return false;
        }
    }
    if (giftRequestsOnly) {
        if (!importedExistingAccount) return false;
        wchar_t recipient[32]{};
        GetPrivateProfileStringW(L"gift", L"recipient_nickname", L"", recipient, 32, accountPath.c_str());
        if (!recipient[0] || !WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, recipient, -1,
                giftRecipient, sizeof(giftRecipient), nullptr, nullptr)) return false;
        giftSenderPilot = GetPrivateProfileIntW(L"gift", L"sender_pilot_id", 0, accountPath.c_str());
        giftRecipientPilot = GetPrivateProfileIntW(L"gift", L"recipient_pilot_id", 0, accountPath.c_str());
        giftCount = GetPrivateProfileIntW(L"gift", L"count", 0, accountPath.c_str());
        if (!giftSenderPilot || !giftRecipientPilot || giftCount < 1 || giftCount > 4) return false;
        for (uint32_t i = 0; i < giftCount; ++i) {
            wchar_t key[32]{};
            wchar_t body[301]{};swprintf_s(key,L"body%u",i);
            const auto bodyLength=GetPrivateProfileStringW(L"gift",key,L"",body,301,accountPath.c_str());
            if(bodyLength) {
                if(bodyLength>=300 || !WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,body,-1,giftBodies[i],301,nullptr,nullptr))return false;
                continue;
            }
            swprintf_s(key, L"page%u", i);
            giftPages[i] = GetPrivateProfileIntW(L"gift", key, 0, accountPath.c_str());
            swprintf_s(key, L"position%u", i);
            giftPositions[i] = GetPrivateProfileIntW(L"gift", key, 0, accountPath.c_str());
            if (!giftPages[i] || giftPositions[i] < 1 || giftPositions[i] > 6) return false;
        }
    }
    GetPrivateProfileStringW(L"run", L"title_nonce", L"gt", nonce, 16, job.c_str());
    char narrowNonce[16] = {};
    if (!WideCharToMultiByte(CP_UTF8, 0, nonce, -1, narrowNonce, sizeof(narrowNonce), nullptr, nullptr)) return false;
    if (strlen(narrowNonce) > 8) return false;
    expectedAi = GetPrivateProfileIntW(L"run", L"expected_ai", 3, job.c_str());
    if (expectedAi < 1 || expectedAi > 7) return false;
    expectedAllies=GetPrivateProfileIntW(L"run",L"ai_allies",expectedAi/2,job.c_str());
    if(expectedAllies>=expectedAi)return false;
    expectedEnemies=expectedAi-expectedAllies;
    const auto randomTeamFlag=GetPrivateProfileIntW(L"run",L"random_teams",0,job.c_str());
    if(randomTeamFlag>1)return false;
    randomTeams=randomTeamFlag==1;
    randomLoadout=GetPrivateProfileIntW(L"run",L"random_loadout",0,job.c_str())!=0;
    const auto rematchFlag=GetPrivateProfileIntW(L"run",L"same_room_rematch",0,job.c_str());
    if(rematchFlag>1)return false;
    sameRoomRematch=rematchFlag==1;
    if(sameRoomRematch && (missionMode || bootstrapOnly || battleshipOnly || giftRequestsOnly || deckOnly || pagesOnly))return false;
    if(missionMode) {
        expectedAi=GetPrivateProfileIntW(L"run",L"mission_ai_count",0,job.c_str());
        const auto level=GetPrivateProfileIntW(L"run",L"mission_ai_level",0,job.c_str());
        wchar_t respawns[16]{};GetPrivateProfileStringW(L"run",L"mission_respawns",L"",respawns,16,job.c_str());
        if(*respawns) {
            if(wcscmp(respawns,L"player")==0)missionRespawns=1;
            else if(wcscmp(respawns,L"infinite")==0)missionRespawns=2;
            else return false;
        }
        if(missionBehaviorProfile && missionRespawns)return false;
        if(expectedAi!=3 || level!=2 || sprintf_s(roomTitle,missionRespawns==2?"%s !ai3 lv2 inf":"%s !ai3 lv2",narrowNonce)<0)return false;
        wchar_t actualTitle[32]{};GetPrivateProfileStringW(L"run",L"room_title",L"",actualTitle,32,job.c_str());
        const std::wstring expectedTitle(roomTitle,roomTitle+strlen(roomTitle));
        if(expectedTitle!=actualTitle)return false;
    }
    if (!missionMode && !ReadPvpRoomTitle(job,narrowNonce,randomLoadout,expectedEnemies,expectedAllies,roomTitle)) return false;
    if (!missionMode && !ReadSealedAiTeamCounts(job,expectedAi,roomTitle,expectedAllies,expectedEnemies)) return false;
    retryCount = static_cast<unsigned>(GetPrivateProfileIntW(L"run", L"retry_count", 1, job.c_str()));
    if (retryCount < 1 || retryCount > 2) return false;
    expectFirstFailure = GetPrivateProfileIntW(L"run", L"expect_first_fail", 0, job.c_str()) != 0;
    if (importedExistingAccount && (retryCount != 1 || expectFirstFailure)) return false;
    if (expectFirstFailure && retryCount != 2) return false;
    if(missionMode) {
        wchar_t forbidden[128]{};
        if(GetPrivateProfileStringW(L"run",L"mode_sequence",L"",forbidden,128,job.c_str()) ||
            GetPrivateProfileStringW(L"run",L"expected_ai",L"",forbidden,128,job.c_str()))return false;
        if(bootstrapOnly || battleshipOnly || giftRequestsOnly || deckOnly || pagesOnly || roomTitlesOnly || randomLoadout)return false;
        return true;
    }
    wchar_t modes[128] = {};
    GetPrivateProfileStringW(L"run", L"mode_sequence", L"1", modes, 128, job.c_str());
    for (const wchar_t* p = modes; *p;) {
        if (*p < L'0' || *p > L'9') return false;
        const uint16_t mode = static_cast<uint16_t>(*p++ - L'0');
        if (mode != 1 && mode != 3 && mode != 6 && mode != 7 && mode != 9) return false;
        if (std::find(requestedModes.begin(), requestedModes.end(), mode) != requestedModes.end()) return false;
        requestedModes.push_back(mode);
        if (!*p) break;
        if (*p++ != L',' || !*p) return false;
    }
    if (requestedModes.empty()) return false;
    return true;
}

int __cdecl Bootstrap(const SdgoGameTestContextV1* context) {
    const uint32_t state = fpsOnly ? fpsActions.State():actions.AppState();
    if (state == UINT32_MAX || state < 4) return SDGO_GAME_TEST_WAIT;
    if (verifyQuietRuntime) {
        const auto file=sdgo_test::directory+L"\\quiet-verified.ini";
        wchar_t id[64]{};
        GetPrivateProfileStringW(L"run",L"id",L"",id,64,file.c_str());
        const std::wstring expected(sdgo_test::runId.begin(),sdgo_test::runId.end());
        if (expected!=id || GetPrivateProfileIntW(L"quiet",L"pid",0,file.c_str())!=GetCurrentProcessId() ||
            GetPrivateProfileIntW(L"quiet",L"verified",0,file.c_str())!=1 ||
            GetPrivateProfileIntW(L"quiet",L"audio_muted",0,file.c_str())!=1 ||
            GetPrivateProfileIntW(L"quiet",L"firewall_verified",0,file.c_str())!=1) return SDGO_GAME_TEST_WAIT;
        context->Assert(1,"the owned client has an isolated background desktop, muted audio and a verified listener policy before login");
    }
    if (verifyDirectNetwork) {
        const auto file=sdgo_test::directory+L"\\network-verified.ini";
        wchar_t id[64]{};
        GetPrivateProfileStringW(L"run",L"id",L"",id,64,file.c_str());
        const std::wstring expected(sdgo_test::runId.begin(),sdgo_test::runId.end());
        wchar_t expectedEndpoint[160]{}, actualIP[32]{};
        const auto planFile=sdgo_test::directory+L"\\run.ini";
        GetPrivateProfileStringW(L"run",L"endpoint",L"",expectedEndpoint,160,planFile.c_str());
        GetPrivateProfileStringW(L"network",L"ipv4",L"",actualIP,32,file.c_str());
        const auto actualPort=GetPrivateProfileIntW(L"network",L"port",0,file.c_str());
        const std::wstring actualEndpoint=std::wstring(actualIP)+L":"+std::to_wstring(actualPort);
        if(verifyRoutedNetwork) {
            wchar_t logical[32]{},physical[32]{},profileHash[65]{},receiptHash[65]{},policy[32]{},receiptLogical[32]{},receiptPhysical[32]{};
            GetPrivateProfileStringW(L"run",L"logical_login_ipv4",L"",logical,32,planFile.c_str());
            GetPrivateProfileStringW(L"run",L"physical_login_ipv4",L"",physical,32,planFile.c_str());
            GetPrivateProfileStringW(L"run",L"route_profile_sha256",L"",profileHash,65,planFile.c_str());
            const auto logicalPort=GetPrivateProfileIntW(L"run",L"logical_login_port",0,planFile.c_str());
            const auto physicalPort=GetPrivateProfileIntW(L"run",L"physical_login_port",0,planFile.c_str());
            GetPrivateProfileStringW(L"network",L"policy",L"",policy,32,file.c_str());
            GetPrivateProfileStringW(L"network",L"logical_ipv4",L"",receiptLogical,32,file.c_str());
            GetPrivateProfileStringW(L"network",L"physical_ipv4",L"",receiptPhysical,32,file.c_str());
            GetPrivateProfileStringW(L"network",L"route_profile_sha256",L"",receiptHash,65,file.c_str());
            if(wcscmp(policy,L"routed_loopback") || !logicalPort || logicalPort>65535 || !physicalPort || physicalPort>65535 ||
                wcsncmp(logical,L"127.",4) || wcsncmp(physical,L"127.",4) || wcslen(profileHash)!=64 ||
                wcsspn(profileHash,L"0123456789abcdef")!=64 || wcscmp(profileHash,receiptHash) ||
                std::wstring(logical)+L":"+std::to_wstring(logicalPort)!=expectedEndpoint ||
                wcscmp(logical,receiptLogical) || wcscmp(physical,receiptPhysical) ||
                GetPrivateProfileIntW(L"network",L"logical_port",0,file.c_str())!=logicalPort ||
                GetPrivateProfileIntW(L"network",L"physical_port",0,file.c_str())!=physicalPort)return SDGO_GAME_TEST_WAIT;
            swprintf_s(expectedEndpoint,L"%s:%u",physical,physicalPort);
            const auto routing=GetModuleHandleW(L"net_route.dll");
            const auto observe=routing?reinterpret_cast<SdgoNetRouteGetSnapshotV1Fn>(GetProcAddress(routing,SDGO_NET_ROUTE_SNAPSHOT_EXPORT)):nullptr;
            SdgoNetRouteSnapshotV1 routes{};routes.size=sizeof(routes);routes.version=SDGO_NET_ROUTE_OBSERVER_VERSION;
            const auto hash=sdgo::game_test_page_observer::ClientRunHash(sdgo_test::runId.c_str(),sdgo_test::runId.size(),"client",6);
            bool matched=false;
            if(observe && observe(hash,&routes) && routes.run_hash==hash && routes.process_id==GetCurrentProcessId() && routes.observation_active &&
                routes.route_count==GetPrivateProfileIntW(L"run",L"route_profile_route_count",0,planFile.c_str()) && routes.route_count<=SDGO_NET_ROUTE_OBSERVER_MAX_ROUTES) {
                for(uint32_t index=0;index<routes.route_count;++index) {
                    const auto& route=routes.routes[index];wchar_t from[32]{},to[32]{};
                    swprintf_s(from,L"%u.%u.%u.%u",route.logical.ipv4[0],route.logical.ipv4[1],route.logical.ipv4[2],route.logical.ipv4[3]);
                    swprintf_s(to,L"%u.%u.%u.%u",route.physical.ipv4[0],route.physical.ipv4[1],route.physical.ipv4[2],route.physical.ipv4[3]);
                    matched=matched || (route.socket_type==1 && !wcscmp(from,logical) && !wcscmp(to,physical) && route.logical.port==logicalPort &&
                        route.physical.port==physicalPort && route.translated_send_calls>0);
                }
            }
            context->Assert(matched,"the same-run production translator observed the exact logical-to-physical native login connection");
            if(!matched)return SDGO_GAME_TEST_FAIL;
        }
        if (expected!=id || GetPrivateProfileIntW(L"network",L"pid",0,file.c_str())!=GetCurrentProcessId() ||
            GetPrivateProfileIntW(L"network",L"verified",0,file.c_str())!=1 || actualEndpoint!=expectedEndpoint)
            return SDGO_GAME_TEST_WAIT;
        context->Assert(1,verifyRoutedNetwork?
            "the actual routed TCP peer matches the immutable exact loopback profile, logical endpoint, physical endpoint and configuration hash before authentication":
            "the owned process established its actual TCP login peer at the explicit test endpoint before authentication");
    }
    context->Assert(GetCurrentThreadId() == mainThread, "test polling runs on the observed original app-update thread");
    context->Observe("the original client update reached the login stage without supplied game data or an application-state write");
    if (!nativeDataGuardProbe.Run(sdgo_test::api, sdgo_test::mod, context)) return SDGO_GAME_TEST_FAIL;
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Login(const SdgoGameTestContextV1* context) {
    const auto auth = actions.LoginState();
    static uint64_t nextProgress=0;
    if(!context->tick)nextProgress=0;
    if(context->elapsed_ms>=nextProgress) {
        const auto transport=actions.TransportState();
        const auto progress=actions.OnboardingState();
        char message[256];
        sprintf_s(message,"native login progress app=%u service=%u auth=%u replies=%u pilot=%u pending=%u path=%u transport=%04X/%u/%u/%u",
            auth.native_app_state,auth.native_login_state,auth.result,auth.reply_sequence,auth.profile_pilot_id,
            progress.pending_login_event_count,progress.login_path_state,transport.opcode,transport.packet_size,
            transport.send_sequence,transport.result);
        context->Observe(message);nextProgress=context->elapsed_ms+3000;
    }
    if (auth.reply_sequence != loginReplySeen) {
        loginReplySeen = auth.reply_sequence;
        char message[128];
        sprintf_s(message, "real login reply opcode=%04X status=%u attempt=%u service_state=%u pilot=%u", auth.reply_opcode, auth.reply_status, loginAttempts, auth.native_login_state, auth.profile_pilot_id);
        context->Observe(message);
        if (auth.result == NativeActions::kAuthAccepted) {
            if (expectFirstFailure && loginAttempts < 2) {
                context->Assert(0, "backend accepted the first attempt although this plan requires a registration retry");
                return SDGO_GAME_TEST_FAIL;
            }
            context->Assert(1, "the original login service accepted a real reply from the backend");
            return SDGO_GAME_TEST_PASS;
        }
        if (auth.result == NativeActions::kAuthRejected) {
            if (loginAttempts >= retryCount) {
                context->Assert(0, "real backend rejected every configured login attempt");
                return SDGO_GAME_TEST_FAIL;
            }
            rejectedAt = GetTickCount64();
            context->Observe("real first-attempt rejection observed; registration retry remains in the normal client request path");
        }
    }
    if (!loginAttempts || (rejectedAt && GetTickCount64() - rejectedAt >= 1000)) {
        rejectedAt = 0;
        ++loginAttempts;
        if (!actions.Login()) {
            context->Assert(0, "original client login request could not be sent");
            return SDGO_GAME_TEST_FAIL;
        }
        context->Observe(loginAttempts == 1 ? "sent native login request, attempt 1" : "sent native login request, registration retry");
    }
    return SDGO_GAME_TEST_WAIT;
}
bool Snapshot(SdgoPvpAiClientSnapshotV1& snapshot) {
    snapshot = {}; snapshot.size = sizeof(snapshot); snapshot.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    return getSnapshot && getSnapshot(&snapshot) != 0;
}
int __cdecl Room(const SdgoGameTestContextV1* context) {
    if(sameRoomRematch && sameRoomRound==1) {
        const auto waiting=rooms.NativeRoomState();
        if((actions.AppState()!=18 && actions.AppState()!=20) || !waiting.model_present)return SDGO_GAME_TEST_WAIT;
        const bool valid=sameRoomId && waiting.room_id==sameRoomId && waiting.room_mode==RequestedMode() &&
            waiting.local_is_host && waiting.player_count==1 && !waiting.ai_pilot_count &&
            waiting.occupied_slot_count==waiting.player_count;
        context->Assert(valid,"the second battle reuses the settled room with its human host and no leftover AI slots; no new create request");
        return valid ? SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
    }
    static size_t roomPageBaselineMode=SIZE_MAX;
    static size_t hangarExitMode=SIZE_MAX;
    if(actions.AppState()==5) {
        if(hangarExitMode!=modeIndex) {
            const auto transition=hangarNavigation.ClickBottomRadio(L"GameRoomRadio",0x48);
            if(transition>=HangarNavigation::kDownmenuServiceMismatch || transition==HangarNavigation::kUnbound) {
                context->Assert(0,"the native GameRoomRadio must return from equipped-unit hangar before VS selection");return SDGO_GAME_TEST_FAIL;
            }
            if(transition==HangarNavigation::kReady) {
                hangarExitMode=modeIndex;context->Observe("original GameRoomRadio invoked after native unit selection; waiting for actual lobby arrival");
            }
        }
        return SDGO_GAME_TEST_WAIT;
    }
    static uint32_t roomPageBaselineGeneration=0;
    if(roomPageBaselineMode!=modeIndex){roomPageBaselineMode=modeIndex;roomPageBaselineGeneration=rooms.NativeRoomPageGeneration();}
    if (!roomRequested) {
        const auto state = actions.OnboardingState();
        if (!state.profile_pilot_id || actions.AppState() == 4 || actions.AppState() == 25) return SDGO_GAME_TEST_WAIT;
        {
            // Grid/Tag are room rules on a regular battle channel. Native
            // channel type2 is the time-gated clan channel, not a ship mode.
            const uint32_t requiredType = 1;
            const ChannelActions::Result result = channels.EnsureVsType(requiredType);
            const auto channel = channels.State();
            if(result==ChannelActions::kAccepted && channels.TakeSceneReloadRequirement()) {
                hangarNavigation.RequireGameRoomReentry(roomPageBaselineGeneration);
                context->Observe("native channel reply accepted; waiting for old room-list teardown and the new scene's page entry");
                return SDGO_GAME_TEST_WAIT;
            }
            static uint32_t lastResult = UINT32_MAX;
            static uint32_t lastReplySequence = UINT32_MAX;
            static uint32_t lastTransportSequence = UINT32_MAX;
            if (channel.result != lastResult ||
                channel.reply_sequence != lastReplySequence ||
                channel.request_transport_sequence != lastTransportSequence) {
                char message[256];
                sprintf_s(
                    message,
                    "native VS channel precondition result=%s selected=%u/%u target=%u/%u page=%u/%u request=%u reply=%04X/%04X/%u consumed=%u",
                    ChannelActions::ResultName(result),
                    channel.selected_group,
                    channel.selected_channel,
                    channel.target_group,
                    channel.target_channel,
                    channel.current_page,
                    channel.target_page,
                    channel.request_transport_sequence,
                    channel.reply_opcode,
                    channel.reply_error,
                    channel.reply_channel,
                    channel.reply_consumed);
                context->Observe(message);
                lastResult = channel.result;
                lastReplySequence = channel.reply_sequence;
                lastTransportSequence =
                    channel.request_transport_sequence;
            }
            if (result == ChannelActions::kWaitingForLobby ||
                result == ChannelActions::kSelectorOpened ||
                result == ChannelActions::kPaging ||
                result == ChannelActions::kNativeRequestPending ||
                result == ChannelActions::kRequestSent)
                return SDGO_GAME_TEST_WAIT;
            if (result != ChannelActions::kAlreadySelected &&
                result != ChannelActions::kAccepted) {
                char failure[128];
                sprintf_s(
                    failure,
                    "native VS channel precondition failed: %s",
                    ChannelActions::ResultName(result));
                context->Assert(0, failure);
                return SDGO_GAME_TEST_FAIL;
            }
        }
        if (!rooms.SetRoomMode(RequestedMode())) { context->Assert(0, "invalid PvP mode in test plan"); return SDGO_GAME_TEST_FAIL; }
        const auto page=hangarNavigation.EnsureGameRoom(roomListVtable,modalLoadingVtable,rooms.NativeRoomPageGeneration());
        static uint32_t lastPageResult=UINT32_MAX;
        if (lastPageResult!=page) {
            char message[96];sprintf_s(message,"native Game Room page precondition=%s",HangarNavigation::ResultName(page));
            context->Observe(message);lastPageResult=page;
        }
        if (page>=HangarNavigation::kDownmenuServiceMismatch || page==HangarNavigation::kUnbound) {
            context->Assert(0,"native Game Room page entry rejected its actual service or control");return SDGO_GAME_TEST_FAIL;
        }
        if (page!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
        const auto selectedChannel=channels.State();
        context->Assert(selectedChannel.selected_group==selectedChannel.target_group && selectedChannel.selected_channel &&
            selectedChannel.selected_channel==selectedChannel.target_channel,
            "the original selector reply placed the real profile in a server-provided VS channel with the type required by this mode");
        SdgoPvpAiClientSnapshotV1 before{};
        if (!Snapshot(before)) return SDGO_GAME_TEST_WAIT;
        roomReplySequence = before.create_reply_sequence;
        rooms.ResetObservations();
        roomCreationSequence = rooms.FreshRoomState().sequence;
        if(randomLoadout) {
            if(!activeSelection.Read(authenticatedPilot,&selectionBeforeRound) || !selectionBeforeRound.inventory_matches)
                return SDGO_GAME_TEST_WAIT;
            char selected[160];sprintf_s(selected,"native selection before round pilot=%u unit=%u uid=%llu slot=%u",
                selectionBeforeRound.pilot,selectionBeforeRound.unit,static_cast<unsigned long long>(selectionBeforeRound.uid),selectionBeforeRound.slot);context->Observe(selected);
        }
        roomRequested = true;
        char selection[128];
        sprintf_s(selection, "native create plan mode=%u channel=%u group=%u", RequestedMode(), state.channel_id, state.channel_group);
        context->Observe(selection);
        if (!rooms.Create()) { context->Assert(0, "original create-room request could not be sent"); return SDGO_GAME_TEST_FAIL; }
        context->Observe("sent the native PvP create-room request with the test nonce and AI title command");
    }
    const auto room = rooms.CreateState();
    SdgoPvpAiClientSnapshotV1 replies{};
    if (Snapshot(replies) && replies.create_reply_sequence > roomReplySequence) {
        static uint32_t lastReply = UINT32_MAX;
        if (replies.create_reply_sequence != lastReply) {
            char message[128];
            sprintf_s(message, "real create reply opcode=%04X error=%04X room=%u sequence=%u", replies.create_reply_opcode, replies.create_reply_error, replies.create_reply_room_id, replies.create_reply_sequence);
            context->Observe(message); lastReply = replies.create_reply_sequence;
        }
        if (replies.create_reply_opcode == 0x0A0A || (replies.create_reply_opcode == 0x0918 && !replies.create_reply_room_id)) {
            if ((replies.create_reply_error==0x0BC2 || replies.create_reply_error==0x08A5 ||
                 replies.create_reply_error==0x08A4) &&
                !replies.create_reply_room_id && channels.ExcludeSelectedChannel()) {
                context->Observe("native server requires an available ship/deck on this channel; testing the next server-provided regular VS channel without changing its restrictions");
                roomRequested=false;
                return SDGO_GAME_TEST_WAIT;
            }
            context->Assert(0, "the real backend rejected the native create-room request");
            return SDGO_GAME_TEST_FAIL;
        }
    }
    const auto nativeRoom = rooms.NativeRoomState();
    const auto freshRoom = rooms.FreshRoomState();
    static uint32_t lastFreshSequence = UINT32_MAX, lastFreshModel = UINT32_MAX, lastFreshId = UINT32_MAX, lastFreshMode = UINT32_MAX;
    if (freshRoom.sequence != lastFreshSequence || freshRoom.model_present != lastFreshModel || freshRoom.room_id != lastFreshId || freshRoom.room_mode != lastFreshMode) {
        char message[144];
        sprintf_s(message, "fresh factory sequence=%u before=%u model=%u id=%u mode=%u success=%u", freshRoom.sequence, roomCreationSequence, freshRoom.model_present, freshRoom.room_id, freshRoom.room_mode, freshRoom.original_factory_succeeded);
        context->Observe(message);
        lastFreshSequence = freshRoom.sequence; lastFreshModel = freshRoom.model_present; lastFreshId = freshRoom.room_id; lastFreshMode = freshRoom.room_mode;
    }
    static uint32_t lastRoomReply = UINT32_MAX, lastRoomResult = UINT32_MAX, lastRoomApp = UINT32_MAX, lastNativeId = UINT32_MAX;
    static bool lastMenu = false;
    if (room.reply_sequence != lastRoomReply || room.result != lastRoomResult || rooms.HasObservedWaitMenu() != lastMenu || nativeRoom.native_app_state != lastRoomApp || nativeRoom.room_id != lastNativeId) {
        char message[160];
        sprintf_s(message, "observed room seq=%u op=%04X result=%u id=%u app=%u model=%u host=%u players=%u menu=%u", room.reply_sequence, room.reply_opcode, room.result, room.room_id, nativeRoom.native_app_state, nativeRoom.model_present, nativeRoom.local_is_host, nativeRoom.player_count, nativeRoom.wait_menu_observed);
        context->Observe(message); lastRoomReply = room.reply_sequence; lastRoomResult = room.result; lastMenu = rooms.HasObservedWaitMenu();
        lastRoomApp = nativeRoom.native_app_state; lastNativeId = nativeRoom.room_id;
    }
    if (room.result == RoomActions::kCreateRejected) { context->Assert(0, "the real backend rejected room creation"); return SDGO_GAME_TEST_FAIL; }
    if ((actions.AppState() != 18 && actions.AppState() != 20) || !nativeRoom.model_present || !nativeRoom.local_is_host ||
        freshRoom.sequence <= roomCreationSequence || !freshRoom.original_factory_succeeded || !freshRoom.model_present ||
        freshRoom.room_mode != RequestedMode() || nativeRoom.room_mode != RequestedMode() || freshRoom.room_id != nativeRoom.room_id)
        return SDGO_GAME_TEST_WAIT;
    context->Assert(freshRoom.room_id == nativeRoom.room_id && freshRoom.room_mode == RequestedMode(), "a new native room factory completion belongs to this create request and room model");
    context->Assert(nativeRoom.room_id != 0, "the server-created native room model supplied a room id");
    if(sameRoomRematch)sameRoomId=nativeRoom.room_id;
    context->Assert(actions.OnboardingState().profile_pilot_id == authenticatedPilot, "the waiting room retains this run's authenticated pilot");
    context->Assert(nativeRoom.model_present && nativeRoom.local_is_host && nativeRoom.player_count >= 1 && nativeRoom.player_count <= 12,
                    "the normal native room model contains the authenticated host");
    context->Assert(nativeRoom.room_mode == RequestedMode(), "the real waiting room has the requested PvP mode");
    context->Assert(1, "the original client entered its native waiting room after the real server reply");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Start(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 snapshot{};
    static LONG errorBaseline=0;
    static uint64_t lastStartAttempt=0;
    static uint32_t startSendBaseline=0,startReplyBaseline=0;
    if(!context->tick){
        // A coordinated SDK can submit another Start in the same room. Its
        // prior observer flags cannot satisfy the next native start reply.
        startRequested=false;startSequence=0;activeBattleStartSequence=0;
        lastStartAttempt=0;startSendBaseline=actions.GameStartSendCount();startReplyBaseline=rooms.NativeStartReplySequence();
    }
    if(rooms.NativeStartReplySequence()>startReplyBaseline && rooms.NativeStartReplyError()!=0) {
        char failure[96];sprintf_s(failure,"native server rejected battle start: 07B5/%04X",rooms.NativeStartReplyError());
        context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
    }
    static uint64_t nextStartObservation=0;
    if(context->elapsed_ms>=nextStartObservation) {
        const auto transport=actions.TransportState();
        char message[192];sprintf_s(message,"native start prerequisite=%d command_state_and_queue=%d transport=%04X/%u/%u attempted=%u",rooms.NativeStartPrerequisite(),rooms.NativeStartQueueState(),transport.opcode,transport.packet_size,transport.send_sequence,startRequested?1u:0u);
        context->Observe(message);nextStartObservation=context->elapsed_ms+1000;
    }
    if (!startRequested) {
        startSendBaseline=actions.GameStartSendCount();startReplyBaseline=rooms.NativeStartReplySequence();
        errorBaseline=InterlockedCompareExchange(&nativeErrorSequence,0,0);
        Snapshot(snapshot); startSequence = snapshot.received_battle_start_count;
        endSequence = rooms.BattleEndState().sequence;
        if (!rooms.StartBattleNativeHost()) return SDGO_GAME_TEST_WAIT;
        startRequested = true;lastStartAttempt=context->elapsed_ms;
        context->Observe("queued the original room-host Start business; await native command progress and server reply");
    }
    // Native 00731240 also repeats AddState while 0x0C has not entered the
    // active queue. A queue request during scene initialization can be lost;
    // only retry the unchanged native business when no command progressed.
    else if(actions.GameStartSendCount()==startSendBaseline && context->elapsed_ms-lastStartAttempt>=1000 && rooms.NativeStartQueueState()==0 &&
            rooms.NativeStartPrerequisite()==0 && rooms.StartBattleNativeHost()) {
        lastStartAttempt=context->elapsed_ms;
        context->Observe("native start command has not progressed; retried its original queue business without a packet or state write");
    }
    const LONG rejection=InterlockedCompareExchange(&nativeLastError,0,0);
    if (InterlockedCompareExchange(&nativeErrorSequence,0,0)>errorBaseline && rejection>=0x0FAC && rejection<=0x0FC0) {
        char message[112];sprintf_s(message,"the original client received native battle-start rejection %04X",static_cast<unsigned>(rejection));
        context->Assert(0,message);return SDGO_GAME_TEST_FAIL;
    }
    if (!Snapshot(snapshot) || snapshot.native_app_state != 22 || snapshot.received_battle_start_count <= startSequence) return SDGO_GAME_TEST_WAIT;
    activeBattleStartSequence = snapshot.received_battle_start_count;
    context->Assert(snapshot.room_mode == RequestedMode(), "the native game mode matches the requested real room mode");
    context->Assert(snapshot.received_battle_start_count > startSequence, "a genuine server battle-start packet preceded native GAME_MAIN");
    return SDGO_GAME_TEST_PASS;
}
int PvpGuestChannel(uint32_t group,uint16_t channel) {
    static uint32_t generation=rooms.NativeRoomPageGeneration();
    const auto result=channels.EnsureVsGroup(group);
    const auto channelState=channels.State();
    static uint32_t lastChannel=UINT32_MAX,lastResult=UINT32_MAX;
    if(channelState.selected_channel!=lastChannel||result!=lastResult) {
        sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_INFO,"native peer channel result=%s selected=%u/%u expected=%u/%u generation=%u",
            ChannelActions::ResultName(result),channelState.selected_group,channelState.selected_channel,group,channel,rooms.NativeRoomPageGeneration());
        lastChannel=channelState.selected_channel;lastResult=result;
    }
    if(result==ChannelActions::kAccepted&&channels.TakeSceneReloadRequirement()) {
        hangarNavigation.RequireGameRoomReentry(generation);return SDGO_GAME_TEST_WAIT;
    }
    if(result!=ChannelActions::kAlreadySelected&&result!=ChannelActions::kAccepted) {
        if(result==ChannelActions::kWaitingForLobby||result==ChannelActions::kSelectorOpened||
           result==ChannelActions::kPaging||result==ChannelActions::kNativeRequestPending||result==ChannelActions::kRequestSent)
            return SDGO_GAME_TEST_WAIT;
        return SDGO_GAME_TEST_FAIL;
    }
    const auto selected=channels.State();
    if(selected.selected_channel!=channel||selected.selected_group!=group)return SDGO_GAME_TEST_FAIL;
    const auto page=hangarNavigation.EnsureGameRoom(roomListVtable,modalLoadingVtable,rooms.NativeRoomPageGeneration());
    static uint32_t lastPage=UINT32_MAX;
    if(page!=lastPage) {
        sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_INFO,"native peer room-list entry=%s generation=%u",HangarNavigation::ResultName(page),rooms.NativeRoomPageGeneration());lastPage=page;
    }
    if(page==HangarNavigation::kReady)return SDGO_GAME_TEST_PASS;
    if(page==HangarNavigation::kUnbound||page>=HangarNavigation::kDownmenuServiceMismatch)return SDGO_GAME_TEST_FAIL;
    return SDGO_GAME_TEST_WAIT;
}
bool PvpControlEnabled(uint32_t sequence) {
    SdgoPvpAiClientStartStatusV1 control{};control.size=sizeof(control);control.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    return getStartStatus&&getStartStatus(&control)&&control.battle_start_sequence==sequence&&control.player_control_enabled;
}
uint32_t UnitDataForNpc(const SdgoGameTestContextV1* context, uint32_t npcId) {
    using GridData = void*(__cdecl*)();
    using GridFind = uint8_t(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
    const auto data = reinterpret_cast<GridData>(context->api->Symbol(sdgo_test::mod, "grid.data"));
    const auto find = reinterpret_cast<GridFind>(context->api->Symbol(sdgo_test::mod, "grid.find"));
    const uint8_t* record = nullptr;
    if (!data || !find || !find(data(), &record, npcId, 0x29, 0) || !record) return 0;
    return *reinterpret_cast<const uint32_t*>(record + 0x69);
}
int CheckGameplayFps(const SdgoGameTestContextV1* context, uint32_t generation) {
    const int requested=sdgo_test::api->SettingInt(sdgo_test::mod,"FpsGameplayCap",0);
    if (!requested)return SDGO_GAME_TEST_PASS;
    static FpsTestActions probe;
    static uint32_t boundGeneration=UINT32_MAX;
    static bool bound=false,capChecked=false,complete=false;
    if (boundGeneration!=generation) {
        probe=FpsTestActions{};boundGeneration=generation;
        bound=capChecked=complete=false;
    }
    if (complete)return SDGO_GAME_TEST_PASS;
    if (!bound) {
        bound=probe.Bind(sdgo_test::api,sdgo_test::mod,requested,true,true);
        if (!bound) {
            context->Assert(false,"gameplay FPS check requires the actual FPS Mod and native application bindings");
            return SDGO_GAME_TEST_FAIL;
        }
    }
    const auto graphics=quietGraphics.State();
    if (!capChecked) {
        if (probe.Cap(context,graphics.present_qpc_frequency,graphics.successful_present_min_gap_ticks,
                      graphics.successful_present_max_gap_ticks)==SDGO_GAME_TEST_WAIT)return SDGO_GAME_TEST_WAIT;
        capChecked=true;
    }
    const int result=probe.Cadence(context,graphics.successful_presents,graphics.present_qpc_frequency,
                                  graphics.successful_present_min_gap_ticks,graphics.successful_present_max_gap_ticks);
    if (result==SDGO_GAME_TEST_PASS)complete=true;
    return result;
}
int __cdecl AiSpawn(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 snapshot{};
    if (!context->tick) motionSamples.clear();
    if (!Snapshot(snapshot) || snapshot.native_app_state != 22) return SDGO_GAME_TEST_WAIT;
    SdgoPvpAiClientStartStatusV1 activityStart{};
    activityStart.size=sizeof(activityStart);activityStart.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    if(getStartStatus&&getStartStatus(&activityStart)&&
       activityStart.battle_start_sequence==snapshot.received_battle_start_count&&activityStart.player_control_enabled)
        if(!battleAiBehavior.Observe(context,snapshot))return SDGO_GAME_TEST_FAIL;
    scoreboardInput.Hold(true);
    if (sdgo_test::api->SettingInt(sdgo_test::mod, "ApsalusRoundtrip", 0)) {
        SdgoPvpAiClientStartStatusV1 control{};
        control.size=sizeof(control);control.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        if (!getStartStatus || !getStartStatus(&control) ||
            control.battle_start_sequence!=activeBattleStartSequence || !control.player_control_enabled)
            return SDGO_GAME_TEST_WAIT;
    }
    const int transformProbe = apsalus_probe::Poll(context, sdgo_test::api, sdgo_test::mod, scoreboardInput);
    if (transformProbe != SDGO_GAME_TEST_PASS) return transformProbe;
    bool ready = snapshot.bot_count == expectedAi;
    unsigned allies = 0, enemies = 0, moved = 0;
    for (unsigned i = 0; i < snapshot.bot_count && i < 12; ++i) {
        const auto& bot = snapshot.bots[i];
        ready = ready && bot.valid_registered_object && bot.ai_enabled;
        if (bot.team == snapshot.local_team) ++allies;
        else if (bot.team == 3 - snapshot.local_team) ++enemies;
        if (!bot.valid_registered_object || !bot.ai_enabled || !bot.local_control || bot.remote_copy || !bot.master_npc || !bot.position_valid) continue;
        auto sample = std::find_if(motionSamples.begin(), motionSamples.end(), [&](const auto& value) { return value.pilot == bot.pilot_id; });
        if (sample == motionSamples.end()) motionSamples.push_back({bot.pilot_id, bot.object_id, bot.npc_id, bot.position_x, bot.position_z, false});
        else if (sample->object != bot.object_id || sample->npc != bot.npc_id)
            *sample = {bot.pilot_id, bot.object_id, bot.npc_id, bot.position_x, bot.position_z, false};
        else {
            const float dx = bot.position_x - sample->x, dz = bot.position_z - sample->z;
            if (dx * dx + dz * dz > 1.0f) sample->moved = true;
        }
        sample = std::find_if(motionSamples.begin(), motionSamples.end(), [&](const auto& value) { return value.pilot == bot.pilot_id; });
        if (sample != motionSamples.end() && sample->moved) ++moved;
    }
    if (context->elapsed_ms < 4000 || context->tick < 120 || ((!ready || !moved) && context->elapsed_ms < 30000)) return SDGO_GAME_TEST_WAIT;
    const int fpsResult=CheckGameplayFps(context,snapshot.received_battle_start_count);
    if (fpsResult!=SDGO_GAME_TEST_PASS)return fpsResult;
    char observation[160];
    sprintf_s(observation, "real AI mode=%u bots=%u allies=%u enemies=%u moved=%u lua=%u observer=%u", RequestedMode(), snapshot.bot_count, allies, enemies, moved, snapshot.lua_error_count, snapshot.lua_error_observer_available);
    context->Observe(observation);
    SdgoPvpAiClientStartStatusV1 startStatus{};
    startStatus.size = sizeof(startStatus); startStatus.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    if (!getStartStatus || !getStartStatus(&startStatus)) {
        context->Assert(0, "native opening-freeze observation is available");
        return SDGO_GAME_TEST_FAIL;
    }
    char opening[224];
    sprintf_s(opening, "native opening mode=%u sequence=%u phase=%u frozen_frames=%u early=%u releases=%u release_phase=%u player_control=%u bots=%u enabled=%u",
        RequestedMode(), startStatus.battle_start_sequence, startStatus.native_ready_state,
        startStatus.frozen_update_count, startStatus.early_enabled_count, startStatus.release_count,
        startStatus.release_ready_state, startStatus.player_control_enabled, startStatus.bot_count, startStatus.ai_enabled_count);
    context->Observe(opening);
    context->Assert(startStatus.battle_start_sequence == activeBattleStartSequence && startStatus.bot_count == expectedAi,
        "opening-freeze observation belongs to this genuine battle and its AI participants");
    context->Assert(startStatus.frozen_update_count > 0 && startStatus.early_enabled_count == 0,
        "AI stayed disabled on every observed native opening-freeze frame");
    context->Assert(startStatus.release_count == 1 && startStatus.release_ready_state == 3 &&
        startStatus.player_control_enabled && startStatus.ai_enabled_count == expectedAi,
        "all AI were enabled once on the native frame that released player control");
    SdgoPvpAiClientScoreboardStatusV1 roster{};
    roster.size = sizeof(roster); roster.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    if (!getScoreboardStatus || !getScoreboardStatus(&roster)) {
        context->Assert(0, "the actual native TAB roster observer is available"); return SDGO_GAME_TEST_FAIL;
    }
    bool allRendered = roster.panel_visible && roster.panel_state == 2;
    for (uint32_t i = 0; i < roster.bot_count && i < 12; ++i) allRendered = allRendered && roster.bots[i].render_count > 0;
    if (!allRendered && context->elapsed_ms < 30000) return SDGO_GAME_TEST_WAIT;
    context->Assert(scoreboardInput.Queries() > 0 && allRendered && roster.battle_start_sequence == activeBattleStartSequence &&
        roster.bot_count == expectedAi && roster.native_row_count == 1 + expectedAi,
        "native TAB input opened the animated battle roster and its original kill/assist/points renderer drew every AI row");
    char tab[180]; sprintf_s(tab, "native TAB mode=%u rows=%u original=%u bots=%u visible=%u state=%u input_queries=%u",
        RequestedMode(),roster.native_row_count,roster.original_row_count,roster.bot_count,roster.panel_visible,roster.panel_state,scoreboardInput.Queries());
    context->Observe(tab);
    for (uint32_t i = 0; i < roster.bot_count && i < 12; ++i) {
        const auto& row = roster.bots[i];
        context->Assert(row.pilot_id == snapshot.bots[i].pilot_id && row.object_id == snapshot.bots[i].object_id &&
            row.team == snapshot.bots[i].team && row.render_count && row.kills >= 0 && row.assists >= 0,
            "the actual native TAB row retains this AI pilot, object, team and native combat counters");
    }
    context->Assert(authenticatedPilot && snapshot.profile_pilot_id == authenticatedPilot, "the battle uses the pilot captured after this run's accepted login");
    const std::wstring expectedTitle(roomTitle, roomTitle + strlen(roomTitle));
    context->Assert(!wcscmp(snapshot.room_title, expectedTitle.c_str()), "the actual battle belongs to this run's server-created room");
    context->Assert(snapshot.human_participant_count == 1, "the real room contains the authenticated test player");
    context->Assert(snapshot.ai_participant_count == expectedAi && snapshot.bot_count == expectedAi, "all server-provided AI participants spawned as native units");
    context->Assert(snapshot.unit_manager_player_count == 1 + expectedAi, "native player and HUD count includes the real player and every AI unit");
    context->Assert(snapshot.local_team >= 1 && snapshot.local_team <= 2 && allies == expectedAllies && enemies == expectedEnemies, "actual AI team placement matches the requested allies and enemies relative to the authenticated host");
    std::set<uint32_t> pilotIds, objectIds;
    for (unsigned i = 0; i < snapshot.bot_count && i < 12; ++i) {
        const auto& bot = snapshot.bots[i];
        context->Assert(bot.valid_registered_object && bot.ai_enabled, "the native AI unit is registered and enabled by the normal game-ready flow");
        context->Assert(bot.team == 1 || bot.team == 2, "the native AI is assigned to a valid battle team");
        context->Assert((bot.pilot_id & 0x80000000u) && pilotIds.insert(bot.pilot_id).second && bot.object_id && objectIds.insert(bot.object_id).second, "each AI has its own server pilot and registered native object");
        context->Assert(bot.npc_id && bot.local_control && !bot.remote_copy && bot.master_npc && bot.position_valid, "the room host owns each AI with a real unit and finite world position");
        context->Assert(bot.native_deck_count_valid && bot.raw_deck_count >= 1 && bot.raw_deck_count <= 4,
                        "the native AI deck vector has a valid, unclamped size");
        if (RequestedMode() == 6 || RequestedMode() == 7) {
            const SdgoPvpAiClientStartDeckSnapshotV1* deck = nullptr;
            for (unsigned j = 0; j < snapshot.received_ai_deck_count && j < 12; ++j)
                if (snapshot.received_ai_decks[j].pilot_id == bot.pilot_id) deck = &snapshot.received_ai_decks[j];
            bool unitInDeck = false;
            if (deck) for (unsigned j = 0; j < deck->unit_count && j < 4; ++j)
                if (deck->npc_ids[j] == bot.npc_id) unitInDeck = true;
            context->Assert(deck && deck->unit_count_valid && bot.native_deck_count_valid && deck->unit_count == bot.raw_deck_count && unitInDeck,
                            "the native deck count agrees with the server record and its current unit belongs to that deck");
            if (deck && deck->unit_count_valid && bot.native_deck_count_valid && deck->unit_count == bot.raw_deck_count) {
                for (unsigned j = 0; j < deck->unit_count; ++j) {
                    const uint32_t expected = UnitDataForNpc(context, deck->npc_ids[j]);
                    context->Assert(expected && bot.native_deck_unit_data_ids[j] == expected,
                                    "each native deck entry matches the unit data resolved from its server-provided NPC");
                }
            }
        }
    }
    if (RequestedMode() == 6 || RequestedMode() == 7) {
        std::set<uint32_t> serverPilots;
        for (unsigned i = 0; i < snapshot.received_ai_deck_count && i < 12; ++i) serverPilots.insert(snapshot.received_ai_decks[i].pilot_id);
        context->Assert(snapshot.received_ai_deck_count == snapshot.bot_count && serverPilots == pilotIds,
                        "the server deck records and native AI objects refer to exactly the same pilots");
    }
    context->Assert(moved != 0, "at least one locally enabled AI moved horizontally across normal game ticks");
    context->Assert(snapshot.lua_error_observer_available, "Lua error observation is active for this real battle");
    context->Assert(snapshot.lua_error_count == 0, "the real battle ran without a Lua AI error");
    if (reviewFootage && !rosterClosedForReview) {
        // Review footage: the roster has been verified above; a second TAB press closes it so it does
        // not cover the recording. Its later render-based combat check is skipped for such runs.
        scoreboardInput.Hold(false); scoreboardInput.Hold(true); rosterClosedForReview = true;
    }
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Return(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 live{};
    if(Snapshot(live) && live.native_app_state==22) {
        // Review footage: the closing TAB press was delivered on the frame the spawn case passed; release
        // the key now so the native roster animates shut and stays out of the recording.
        if (reviewFootage && rosterClosedForReview) scoreboardInput.Hold(false);
        if(!battleAiBehavior.Observe(context,live)) return SDGO_GAME_TEST_FAIL;
        if(!nativeDeathProbe.Observe(context)) return SDGO_GAME_TEST_FAIL;
        SdgoPvpAiClientStartStatusV1 nativeControl{};
        nativeControl.size=sizeof(nativeControl);nativeControl.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        if(getStartStatus) getStartStatus(&nativeControl);
        // Review footage keeps the battle running on a failed combat-input observation: the
        // failure is still recorded, but the recording is the point of such a run.
        if(!nativePlayerCombat.Poll(context,live,nativeControl,scoreboardInput) && !reviewFootage) return SDGO_GAME_TEST_FAIL;
        SdgoPvpAiClientScoreboardStatusV1 roster{};
        roster.size = sizeof(roster); roster.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        if (getScoreboardStatus && getScoreboardStatus(&roster) && roster.battle_start_sequence == activeBattleStartSequence) {
            if (scoreboardCombatSequence != activeBattleStartSequence) {
                scoreboardCombatSequence = activeBattleStartSequence; scoreboardCombatChanges = 0; scoreboardLast = {};
            }
            for (uint32_t i = 0; i < roster.bot_count && i < 12; ++i) {
                const auto& row = roster.bots[i]; const auto& before = scoreboardLast.bots[i];
                if (row.render_count && (row.kills != before.kills || row.assists != before.assists || row.points != before.points)) {
                    ++scoreboardCombatChanges;
                    char stats[192]; sprintf_s(stats, "native TAB stats mode=%u AI=%08X team=%u row=%u kills=%d assists=%d points=%d draws=%u",
                        RequestedMode(),row.pilot_id,row.team,row.row_index,row.kills,row.assists,row.points,row.render_count);
                    context->Observe(stats);
                }
            }
            scoreboardLast = roster;
        }
        const std::wstring title(roomTitle,roomTitle+strlen(roomTitle));
        const auto result=battleCombat.Observe(live,{activeBattleStartSequence,authenticatedPilot,RequestedMode(),title.c_str()});
        if(result==BattleCombatActions::kInvalidExpectation || result==BattleCombatActions::kInvalidSnapshot ||
           result==BattleCombatActions::kForeignBattle || result==BattleCombatActions::kLuaError) {
            context->Assert(0,"whole-battle target/HP observations must retain the genuine battle identity and error-free native snapshot");
            return SDGO_GAME_TEST_FAIL;
        }
    }
    SdgoPvpAiClientLuaStatusV1 lua{sizeof(lua), SDGO_PVP_AI_CLIENT_OBSERVER_VERSION, 0, 0, 0};
    if (!getLuaStatus || !getLuaStatus(&lua) || !lua.observer_available ||
        !activeBattleStartSequence || lua.battle_start_sequence != activeBattleStartSequence) {
        context->Assert(0, "whole-battle Lua observation is unavailable or belongs to another battle");
        return SDGO_GAME_TEST_FAIL;
    }
    if (lua.error_count != 0) {
        char message[112];
        sprintf_s(message, "the real battle raised %u Lua error(s), including after initial AI spawn", lua.error_count);
        context->Assert(0, message);
        return SDGO_GAME_TEST_FAIL;
    }
    const auto ended = rooms.BattleEndState();
    if (!endObserved && ended.sequence > endSequence && ended.original_handler_completed) {
        scoreboardInput.Hold(false);
        endObserved = true;
        char message[128];
        sprintf_s(message, "real battle end opcode=%04X result=%u app=%u sequence=%u", ended.reply_opcode, ended.result_type, ended.native_app_state, ended.sequence);
        context->Observe(message);
        context->Assert(ended.reply_opcode == 0x0871, "the original client consumed the genuine server battle-end reply");
        context->Assert(1, "Lua observation stayed available with zero errors through the entire real battle");
        battleAiBehavior.Finish(context);
        nativeDeathProbe.Finish(context);
        nativePlayerCombat.Finish(context,scoreboardInput);
        context->Assert(reviewFootage || (scoreboardCombatSequence == activeBattleStartSequence && scoreboardCombatChanges > 0),
            "the native TAB kill/assist/points renderer reflected actual AI combat-counter changes during this full battle");
        const auto combat=battleCombat.State();char evidence[256];
        sprintf_s(evidence,"native combat frames=%u targets=%u moved=%u hp_events=%u hp_total=%llu witness=%u:%d->%d",
            combat.validated_frame_count,combat.target_acquisition_events,combat.moved_identity_count,combat.hp_decrease_events,
            static_cast<unsigned long long>(combat.total_hp_decrease),combat.damaged_target_object_id,
            combat.damaged_target_hp_before,combat.damaged_target_hp_after);context->Observe(evidence);
        context->Assert(combat.complete_evidence!=0,
            "continuous genuine frames show a validated AI acquiring a target, moving, and a target losing HP; this does not attribute a damage source or tactical quality");
    }
    if (!endObserved) return SDGO_GAME_TEST_WAIT;
    const auto state = actions.AppState();
    if (!leaveRequested && (state == 18 || state == 20)) {
        const auto waiting=rooms.NativeRoomState();
        if(!RoomActions::SettledRoomReady(waiting))return SDGO_GAME_TEST_WAIT;
        static uint32_t rosterBattleSequence=0;
        static uint64_t rosterWaitStarted=0;
        if(rosterBattleSequence!=activeBattleStartSequence) {
            rosterBattleSequence=activeBattleStartSequence;rosterWaitStarted=context->elapsed_ms;
        }
        // The native end and room-departure notifications can be consumed in
        // adjacent UI ticks. Require the actual roster to settle, not merely
        // its independent human count, before either leaving or starting again.
        if(waiting.ai_pilot_count && context->elapsed_ms-rosterWaitStarted<2000)return SDGO_GAME_TEST_WAIT;
        char roster[160];
        sprintf_s(roster,"settled roster humans=%u occupied=%u AI=%u room=%u battle=%u",
            waiting.player_count,waiting.occupied_slot_count,waiting.ai_pilot_count,
            waiting.room_id,activeBattleStartSequence);context->Observe(roster);
        const bool cleanRoster=!waiting.ai_pilot_count && waiting.occupied_slot_count==waiting.player_count;
        context->Assert(cleanRoster,"all twelve native waiting-room slots contain only the remaining real humans after settlement");
        if(!cleanRoster)return SDGO_GAME_TEST_FAIL;
        char ownership[192];
        sprintf_s(ownership,"settled room=%u host=%08X authenticated=%u players=%u mode=%u local_host=%u",
            waiting.room_id,waiting.host_pilot_id,authenticatedPilot,waiting.player_count,
            waiting.room_mode,waiting.local_is_host);
        context->Observe(ownership);
        context->Assert(waiting.local_is_host,
            "after settlement the sole authenticated human remains room host; an AI MVP cannot receive room ownership");
        if(!waiting.local_is_host)return SDGO_GAME_TEST_FAIL;
        if(sameRoomRematch && sameRoomRound==0) {
            const bool valid=waiting.room_id==sameRoomId && waiting.room_mode==RequestedMode() && waiting.player_count==1;
            context->Assert(valid,"settlement returned to the same waiting room with only the real human; AI battle-only participants have left");
            if(!valid)return SDGO_GAME_TEST_FAIL;
            context->Observe("kept the settled native room open for a second real Start instead of leaving and recreating it");
            sameRoomRound=1;
            activeBattleStartSequence=0;
            startRequested=leaveRequested=endObserved=false;
            return SDGO_GAME_TEST_PASS;
        }
        leaveSequence = rooms.LeaveState().reply_sequence;
        if (!rooms.Leave()) return SDGO_GAME_TEST_WAIT;
        leaveRequested = true;
        context->Observe("native results returned to the waiting room; sent its original leave-room request");
    }
    const auto left = rooms.LeaveState();
    if (leaveRequested && left.reply_sequence > leaveSequence && left.result == RoomActions::kLeaveRejected) {
        context->Assert(0, "the real backend rejected the leave-room request");
        return SDGO_GAME_TEST_FAIL;
    }
    if (!leaveRequested || left.reply_sequence <= leaveSequence || left.result != RoomActions::kLeaveAccepted || state != 6)
        return SDGO_GAME_TEST_WAIT;
    if(randomLoadout) {
        static size_t returnedMode=SIZE_MAX;static uint64_t firstLobbyRead=0;
        if(returnedMode!=modeIndex){returnedMode=modeIndex;firstLobbyRead=context->elapsed_ms;}
        ActiveSelectionObserver::Snapshot after;
        const bool readable=activeSelection.Read(authenticatedPilot,&after);
        const bool restored=readable && after.inventory_matches && after.SameSelection(selectionBeforeRound);
        if(!restored && context->elapsed_ms-firstLobbyRead<2000)return SDGO_GAME_TEST_WAIT;
        char selected[192];sprintf_s(selected,"native selection after round readable=%u inventory_matches=%u pilot=%u unit=%u uid=%llu slot=%u restored=%u",
            readable?1u:0u,after.inventory_matches?1u:0u,after.pilot,after.unit,static_cast<unsigned long long>(after.uid),after.slot,restored?1u:0u);context->Observe(selected);
        context->Assert(restored,"after genuine leave and lobby return, the client profile and owned inventory expose the original equipped UID, model and slot");
        if(!restored)return SDGO_GAME_TEST_FAIL;
    }
    context->Assert(1, "the original client consumed the real leave-room reply and returned to its lobby");
    ++modeIndex;
    sameRoomRound=0;sameRoomId=0;
    channels.Reset();
    activeBattleStartSequence = 0;
    roomRequested = startRequested = leaveRequested = endObserved = false;
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Onboarding(const SdgoGameTestContextV1* context) {
    const auto transport = actions.TransportState();
    static uint32_t lastSend = 0;
    if (transport.send_sequence != lastSend) {
        char message[112];
        sprintf_s(message, "observed original transport opcode=%04X bytes=%u result=%u", transport.opcode, transport.packet_size, transport.result);
        context->Observe(message); lastSend = transport.send_sequence;
    }
    const auto state = actions.OnboardingState();
    // Login acceptance precedes the real profile load. Freeze the first
    // nonzero server-loaded identity here, rather than expecting a pilot id
    // in the earlier authentication acknowledgement or comparing live aliases.
    if (!authenticatedPilot && state.profile_pilot_id) {
        authenticatedPilot = state.profile_pilot_id;
        char message[96]; sprintf_s(message, "captured real post-login profile pilot=%u", authenticatedPilot);
        context->Observe(message);
    }
    static uint32_t lastStage = UINT32_MAX, lastLevel = UINT32_MAX, lastPath = UINT32_MAX, lastEvents = UINT32_MAX;
    if (state.native_login_state != lastStage || state.tutorial_level != lastLevel ||
        state.login_path_state != lastPath || state.pending_login_event_count != lastEvents) {
        char message[180];
        sprintf_s(message, "real onboarding state=%u tutorial_level=%u login_path=%u pending=%u channel=%u pilot=%u", state.native_login_state, state.tutorial_level, state.login_path_state, state.pending_login_event_count, state.channel_id, state.profile_pilot_id);
        context->Observe(message);
        lastStage = state.native_login_state; lastLevel = state.tutorial_level; lastPath = state.login_path_state; lastEvents = state.pending_login_event_count;
    }
    const auto nickname = actions.NicknameState();
    if (nickname.reply_sequence != nicknameReplySeen) {
        nicknameReplySeen = nickname.reply_sequence;
        char message[144];
        sprintf_s(message, "real nickname reply opcode=%04X status=%u service_state=%u pilot=%u", nickname.reply_opcode, nickname.reply_status, nickname.native_login_state, nickname.profile_pilot_id);
        context->Observe(message);
        if (nickname.result == NativeActions::kNicknameRejected) {
            context->Assert(0, "real backend rejected the generated test nickname");
            return SDGO_GAME_TEST_FAIL;
        }
        if (nickname.result == NativeActions::kNicknameAccepted) {
            nicknameAccepted = true;
            context->Assert(1, "the real nickname-create reply was accepted by the original client service");
        }
    }
    if (!nicknameRequested && actions.LoginState().native_login_state == 4) {
        if (importedExistingAccount) {
            context->Assert(0, "an imported existing account unexpectedly requires a nickname; its identity was preserved");
            return SDGO_GAME_TEST_FAIL;
        }
        nicknameRequested = true;
        if (!actions.CreateNickname()) {
            context->Assert(0, "original nickname request could not be sent");
            return SDGO_GAME_TEST_FAIL;
        }
        const auto sent = actions.TransportState();
        context->Assert(sent.opcode == 0x232C && sent.packet_size == 0x63 && sent.result != 0,
                        "the original confirmation business routine transmitted a real nickname request");
        char transportMessage[112];
        sprintf_s(transportMessage, "observed original transport opcode=%04X bytes=%u result=%u", sent.opcode, sent.packet_size, sent.result);
        context->Observe(transportMessage); lastSend = sent.send_sequence;
        context->Observe("sent the original nickname-create request for the generated test account");
    }
    const auto trial = tutorial.State();
    if (trial.result == TutorialActions::kNativeFault) {
        context->Assert(0, "the original beginner-exit business method raised a native exception");
        return SDGO_GAME_TEST_FAIL;
    }
    if (trial.update_sequence && !trial.exit_request_sequence && state.native_app_state == 22 && state.tutorial_level < 2) {
        if (tutorial.ExitBeginnerTrial())
            context->Observe("invoked the original beginner-trial exit business method on the real tutorial mode");
    }
    static bool trialLeftReported = false;
    if (!trialLeftReported && trial.result == TutorialActions::kLeftGame) {
        trialLeftReported = true;
        context->Assert(1, "the original beginner exit completed its normal scene transition");
    }
    auto starter = onboarding.StarterState();
    static uint32_t previousStarterStage = UINT32_MAX, previousStarterReply = 0;
    if (starter.onboarding_stage != previousStarterStage) {
        char message[96]; sprintf_s(message, "observed native onboarding stage=%u app_state=%u", starter.onboarding_stage, starter.native_app_state);
        context->Observe(message); previousStarterStage = starter.onboarding_stage;
    }
    if (starter.native_fault) {
        context->Assert(0, "the normal first-unit business callback raised a native exception"); return SDGO_GAME_TEST_FAIL;
    }
    if (state.tutorial_level < 2 && !starter.request_attempted && onboarding.DrawStarterUnit())
        context->Observe("invoked the original first-unit Buy business callback on the live native buying dialog");
    if (starter.reply_sequence != previousStarterReply) {
        previousStarterReply = starter.reply_sequence;
        char message[144]; sprintf_s(message, "real starter reply opcode=%04X error=%u unit=%u slot=%u tutorial=%u", starter.reply_opcode, starter.reply_error, starter.picked_unit_id, starter.picked_unit_slot, starter.tutorial_level);
        context->Observe(message);
        if (starter.result == OnboardingActions::kStarterRejected) {
            context->Assert(0, "the real backend rejected the first-unit request"); return SDGO_GAME_TEST_FAIL;
        }
        context->Assert(starter.picked_unit_id != 0, "the real first-unit reply supplied a unit id");
    }
    if (onboarding.AdvanceStarterCapsuleResult())
        context->Observe("invoked the original live CapsuleClick callback after the accepted first-unit reply; native state advanced from 2 to 4");
    if (onboarding.ConfirmStarterResultPopup())
        context->Observe("invoked the original starter-result confirmation business callback on the real result popup");
    if (onboarding.ConfirmInitialLoadingPopup())
        context->Observe("called the original initial-loading popup confirmation business method");
    const auto initial = onboarding.InitialLoadingState();
    if (initial.result == OnboardingActions::kInitialLoadingRejected) {
        context->Assert(0, "the backend rejected initial-loading completion"); return SDGO_GAME_TEST_FAIL;
    }
    const auto inventory = onboarding.InventoryState();
    const std::array<uint32_t,7> unitResultGate{
        starter.unit_result_clone_sequence,starter.unit_result_captured_app_state,starter.unit_result_ui_state,
        starter.unit_result_unit_id,starter.unit_result_captured,starter.unit_result_named_identity_matched,
        starter.unit_result_control_ready};
    static std::array<uint32_t,7> previousUnitResultGate{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    if(unitResultGate!=previousUnitResultGate) {
        char message[256];sprintf_s(message,"native starter unit-result clone=%u app=%u ui=%u unit=%u captured=%u named=%u control_ready=%u confirm=%u/%u",
            unitResultGate[0],unitResultGate[1],unitResultGate[2],unitResultGate[3],unitResultGate[4],unitResultGate[5],unitResultGate[6],
            starter.result_confirm_attempted,starter.result_confirm_sequence);
        context->Observe(message);previousUnitResultGate=unitResultGate;
    }
    const std::array<uint32_t,8> capsuleGate{
        starter.capsule_clone_sequence,starter.capsule_ui_state,starter.capsule_native_state,
        starter.capsule_click_sequence,starter.capsule_captured,starter.capsule_named_identity_matched,
        starter.capsule_control_ready,starter.capsule_click_attempted};
    static std::array<uint32_t,8> previousCapsuleGate{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    if(capsuleGate!=previousCapsuleGate) {
        char message[256];sprintf_s(message,"native starter capsule clone=%u ui=%u state=%u clicks=%u captured=%u named=%u control_ready=%u attempted=%u",
            capsuleGate[0],capsuleGate[1],capsuleGate[2],capsuleGate[3],capsuleGate[4],capsuleGate[5],capsuleGate[6],capsuleGate[7]);
        context->Observe(message);previousCapsuleGate=capsuleGate;
    }
    const std::array<uint32_t,7> buyingGate{
        starter.buying_clone_sequence,starter.buying_captured_app_state,starter.buying_ui_state,
        starter.buying_captured,starter.buying_named_identity_matched,starter.buying_control_ready,
        starter.buying_validation_failed};
    static std::array<uint32_t,7> previousBuyingGate{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    if(buyingGate!=previousBuyingGate) {
        char message[256];sprintf_s(message,"native starter buying clone=%u captured_app=%u state=%u captured=%u named=%u control_ready=%u validation_failed=%u",
            buyingGate[0],buyingGate[1],buyingGate[2],buyingGate[3],buyingGate[4],buyingGate[5],buyingGate[6]);
        context->Observe(message);previousBuyingGate=buyingGate;
    }
    // The native login flow owns the initial 0x048A inventory stream. A scene
    // transition here can retire its reply consumer after the first batch.
    // Wait for genuine completion; later scenario steps enter the hangar.
    const std::array<uint32_t,8> inventoryPage{
        inventory.manager_present,inventory.initialized,inventory.allocated_block_count,
        inventory.slot_capacity,inventory.page_count,inventory.tail_count,
        inventory.last_uid_cursor_low,inventory.last_uid_cursor_high};
    static std::array<uint32_t,8> previousInventoryPage{};
    static bool inventoryPageObserved=false;
    if(!inventoryPageObserved || inventoryPage!=previousInventoryPage) {
        char detail[256];
        sprintf_s(detail,"native inventory pagination manager=%u initialized=%u blocks=%u capacity=%u pages=%u tail=%u cursor_low=%u cursor_high=%u",
            inventoryPage[0],inventoryPage[1],inventoryPage[2],inventoryPage[3],
            inventoryPage[4],inventoryPage[5],inventoryPage[6],inventoryPage[7]);
        context->Observe(detail);
        previousInventoryPage=inventoryPage;inventoryPageObserved=true;
    }
    if(roomTitlesOnly){
        static uint32_t appSeen=UINT32_MAX,initializedSeen=UINT32_MAX,managerSeen=UINT32_MAX,capacitySeen=UINT32_MAX;
        if(appSeen!=state.native_app_state || initializedSeen!=inventory.initialized || managerSeen!=inventory.manager_present || capacitySeen!=inventory.slot_capacity){
            char message[192];sprintf_s(message,"room-title precondition app=%u inventory_manager=%u initialized=%u capacity=%u complete=%u owned=%u",state.native_app_state,inventory.manager_present,inventory.initialized,inventory.slot_capacity,inventory.scan_complete,inventory.owned_unit_count);context->Observe(message);
            appSeen=state.native_app_state;initializedSeen=inventory.initialized;managerSeen=inventory.manager_present;capacitySeen=inventory.slot_capacity;
        }
    }
    static uint32_t previousOwned = UINT32_MAX;
    if (inventory.initialized && inventory.owned_unit_count != previousOwned) {
        char message[128]; sprintf_s(message, "real inventory owned=%u first_unit=%u slot=%u scanned=%u/%u", inventory.owned_unit_count, inventory.first_unit_id, inventory.first_unit_slot, inventory.slots_examined, inventory.slot_capacity);
        context->Observe(message); previousOwned = inventory.owned_unit_count;
    }
    if (inventory.native_fault) {
        context->Assert(0, "the read-only native inventory observation failed"); return SDGO_GAME_TEST_FAIL;
    }
    const std::array<uint32_t,12> gate{
        starter.result_popup_clone_sequence,starter.result_popup_captured_app_state,
        starter.result_popup_captured,starter.result_popup_ready,starter.result_confirm_attempted,
        starter.result_confirm_sequence,initial.popup_state,initial.confirm_sequence,
        inventory.manager_present,inventory.initialized,inventory.scan_complete,inventory.owned_unit_count};
    static std::array<uint32_t,12> previousGate{};static bool gateObserved=false;
    if(!gateObserved||gate!=previousGate) {
        char detail[320];
        sprintf_s(detail,"native onboarding gates popup_clone=%u captured_app=%u captured=%u ready=%u confirm_attempted=%u confirm_sequence=%u initial_popup=%u initial_confirm=%u inventory_manager=%u initialized=%u scanned=%u owned=%u",
            gate[0],gate[1],gate[2],gate[3],gate[4],gate[5],gate[6],gate[7],gate[8],gate[9],gate[10],gate[11]);
        context->Observe(detail);previousGate=gate;gateObserved=true;
    }
    const sdgo_game_test::OnboardingCompletionEvidence completion{
        nicknameRequested,
        nicknameAccepted,
        trial.exit_request_sequence,
        trial.result,
        starter.request_attempted != 0,
        starter.result,
        starter.result_confirm_sequence,
        initial.confirm_sequence,
        initial.result,
        state.tutorial_level,
        state.starter_draw_busy != 0,
        inventory.initialized != 0,
        inventory.scan_complete != 0,
        inventory.owned_unit_count,
        inventory.first_unit_id,
        actions.AppState(),
        authenticatedPilot,
        state.profile_pilot_id,
    };
    if (sdgo_game_test::IsOnboardingComplete(completion)) {
        context->Assert(1, "the authenticated account has a real initialized inventory containing an owned unit");
        context->Assert(authenticatedPilot && state.profile_pilot_id == authenticatedPilot, "onboarding retains the real profile loaded after the accepted login");
        context->Assert(1, "every attempted one-time native onboarding action completed before the lobby");
        return SDGO_GAME_TEST_PASS;
    }
    // Continue only after the normal tutorial/starter-unit/window workflow has
    // provided real profile and inventory data. Nickname success alone is insufficient.
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl AcquireBattleship(const SdgoGameTestContextV1* context) {
    auto state = battleships.State();
    BattleShipActions::Result result;
    if (state.reply_sequence) {
        if (state.reply_error || !state.reply_consumed) {
            context->Assert(0, "the real initial-battleship reply was rejected or not consumed");
            return SDGO_GAME_TEST_FAIL;
        }
        result = battleships.ConfirmAcquisitionPopup();
    } else {
        result = battleships.OpenTutorial();
        if (result == BattleShipActions::kTutorialOpened)
            result = battleships.DriveTutorial();
    }
    state = battleships.State();
    static uint32_t lastResult = UINT32_MAX, lastReply = UINT32_MAX;
    static uint8_t lastPage = UINT8_MAX;
    if (state.result != lastResult || state.reply_sequence != lastReply || state.page != lastPage) {
        char text[224];
        sprintf_s(text, "native battleship acquisition result=%s tutorial=%u page=%u/%u request=%u reply=%04X/%04X consumed=%u ship=%u uid=%u:%u leader=%u:%u popup=%u",
            BattleShipActions::ResultName(result), state.tutorial_level, state.page, state.page_count,
            state.request_transport_sequence,state.reply_opcode,state.reply_error,state.reply_consumed,
            state.ship_id,state.ship_uid_high,state.ship_uid_low,state.leader_uid_high,state.leader_uid_low,state.result_popup_ready);
        context->Observe(text); lastResult=state.result;lastReply=state.reply_sequence;lastPage=state.page;
    }
    if (result == BattleShipActions::kAlreadyCompleted || result == BattleShipActions::kConfirmed) {
        context->Assert(state.profile_pilot_id == authenticatedPilot && authenticatedPilot,
                        "the native battleship belongs to this authenticated test account");
        // A sealed fixture can already own a native starter fleet while its
        // tutorial stays at the template's declared level 2. This scenario
        // verifies that real asset; tutorial completion belongs to the path
        // that actually acquired a fleet through the popup.
        context->Assert(state.ship_id && (state.ship_uid_low || state.ship_uid_high) &&
                        (state.leader_uid_low || state.leader_uid_high),
                        "the server-loaded native profile contains a selected battleship and its real leader unit");
        if (result == BattleShipActions::kConfirmed)
            context->Assert(state.tutorial_level >= 4 && state.reply_opcode == 0x08EF && state.reply_consumed && state.confirm_sequence,
                            "the real acquisition reply and original result confirmation completed");
        return SDGO_GAME_TEST_PASS;
    }
    if (result == BattleShipActions::kWaitingForLobby || result == BattleShipActions::kWaitingForTutorial ||
        result == BattleShipActions::kTutorialOpened || result == BattleShipActions::kPaging ||
        result == BattleShipActions::kRequestSent || result == BattleShipActions::kNativeRequestPending ||
        result == BattleShipActions::kResultPopupUnavailable || result == BattleShipActions::kAccepted)
        return SDGO_GAME_TEST_WAIT;
    char failure[128];sprintf_s(failure,"native battleship acquisition failed: %s",BattleShipActions::ResultName(result));
    context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
}

int __cdecl SendGiftRequests(const SdgoGameTestContextV1* context) {
    if (authenticatedPilot != giftSenderPilot) {
        context->Assert(0,"the native authenticated sender differs from the existing gift-helper account");
        return SDGO_GAME_TEST_FAIL;
    }
    const auto receipt = sdgo_test::directory + L"\\gift-processor.ini";
    wchar_t runId[64]{};
    GetPrivateProfileStringW(L"run",L"id",L"",runId,64,receipt.c_str());
    const std::wstring expected(sdgo_test::runId.begin(),sdgo_test::runId.end());
    if (expected != runId) return SDGO_GAME_TEST_WAIT;
    const auto sender=GetPrivateProfileIntW(L"gift",L"sender",0,receipt.c_str());
    const auto recipient=GetPrivateProfileIntW(L"gift",L"recipient",0,receipt.c_str());
    const auto processed=GetPrivateProfileIntW(L"gift",L"processed_count",0,receipt.c_str());
    const auto succeeded=GetPrivateProfileIntW(L"gift",L"success_count",0,receipt.c_str());
    if (sender!=giftSenderPilot || recipient!=giftRecipientPilot || processed>giftSent || succeeded!=processed) {
        context->Assert(0,"the normal mail processor rejected a request or reported another identity");
        return SDGO_GAME_TEST_FAIL;
    }
    if (processed != giftSent) return SDGO_GAME_TEST_WAIT;
    if (giftSent == giftCount) {
        context->Assert(1,"every native mail request was processed by the normal isolated gift service");
        return SDGO_GAME_TEST_PASS;
    }
    if (giftSent) {
        wchar_t tickText[32]{};
        GetPrivateProfileStringW(L"gift",L"last_processed_tick",L"0",tickText,32,receipt.c_str());
        const auto processedTick=_wcstoui64(tickText,nullptr,10);
        if (!processedTick || GetTickCount64() < processedTick || GetTickCount64()-processedTick < 61000)
            return SDGO_GAME_TEST_WAIT;
    }
    const auto result=giftBodies[giftSent][0]?gifts.SendMail(giftRecipient,giftBodies[giftSent]):
        gifts.SendUnitGift(giftRecipient,giftPages[giftSent],giftPositions[giftSent]);
    if (result==GiftActions::kWaitingForLobby) return SDGO_GAME_TEST_WAIT;
    if (result!=GiftActions::kRequestSent) {
        context->Assert(0,"the original native mail request could not be constructed and transported");
        return SDGO_GAME_TEST_FAIL;
    }
    ++giftSent;
    context->Assert(1,"the original mail constructor and transport sent one real gift command");
    char text[96];sprintf_s(text,"native gift mail sent %u/%u; awaiting independent normal server processing",giftSent,giftCount);
    context->Observe(text);return SDGO_GAME_TEST_WAIT;
}

int __cdecl ConfigureDeck(const SdgoGameTestContextV1* context) {
    if (!deckOnly) {
        BattleShipDeckActions::Observation prepared{};
        const auto result=decks.ObserveExistingDeck(deckUnitUids,prepared);
        if (result==BattleShipDeckActions::kWaitingForShipData || result==BattleShipDeckActions::kWaitingForInventory)
            return SDGO_GAME_TEST_WAIT;
        context->Assert(result==BattleShipDeckActions::kAccepted && prepared.profile_matches &&
            prepared.selected_count>=1 && prepared.selected_count<=4 && prepared.total_cost<=10,
            "the server-loaded selected fleet has one to four unique owned units within the native ten-point budget");
        char state[160];sprintf_s(state,"existing native fleet ship=%llu leader=%llu units=%u cost=%u; no acquisition or deck edit requested",
            prepared.ship_uid,prepared.leader_uid,prepared.selected_count,prepared.total_cost);context->Observe(state);
        return result==BattleShipDeckActions::kAccepted ? SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
    }
    if (actions.AppState()!=20) {
        const auto result=hangarNavigation.EnsureHangar();
        const auto navigation=hangarNavigation.State();
        static HangarNavigation::Observation lastNavigation{};
        if (memcmp(&lastNavigation,&navigation,sizeof(navigation))) {
            char message[256];
            sprintf_s(message,"native hangar result=%s app=%u request=%u/%04X/%u radio=%u services=%u/%u/%u roots=%u/%u",
                HangarNavigation::ResultName(result),navigation.native_app_state,navigation.request_transport_sequence,
                navigation.request_opcode,navigation.request_size,navigation.unit_radio_invocations,
                navigation.downmenu_present,navigation.mainframe_present,navigation.unit_manager_present,
                navigation.mainframe_root_ready,navigation.unit_manager_root_ready);
            context->Observe(message);lastNavigation=navigation;
        }
        if (result>=HangarNavigation::kDownmenuServiceMismatch || result==HangarNavigation::kUnbound) {
            context->Assert(0,"original hangar navigation rejected its native service or control identity");
            return SDGO_GAME_TEST_FAIL;
        }
        if (result!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    }
    const auto result=decks.EnsureDeck(deckUnitUids);
    const auto state=decks.State();
    static BattleShipDeckActions::Observation previous{};
    if (memcmp(&previous,&state,sizeof(state))) {
        char message[480];
        sprintf_s(message,"native deck result=%s app=%u stage=%u ship=%llu leader=%llu selected=%u cost=%u units=%llu,%llu,%llu,%llu save=%u/%04X goout=%u/%04X editor/manager/profile=%u/%u/%u",
            BattleShipDeckActions::ResultName(result),state.native_app_state,state.stage,state.ship_uid,state.leader_uid,
            state.selected_count,state.total_cost,state.unit_uids[0],state.unit_uids[1],state.unit_uids[2],state.unit_uids[3],
            state.save_reply_sequence,state.save_reply_opcode,state.goout_reply_sequence,state.goout_reply_opcode,
            state.editor_matches,state.manager_matches,state.profile_matches);
        context->Observe(message);previous=state;
    }
    if (result==BattleShipDeckActions::kAccepted) {
        context->Assert(state.editor_matches && state.manager_matches && state.profile_matches,
            "the original save and GoOut consumers agree with the genuine four-unit deck in the editor, manager and profile");
        return SDGO_GAME_TEST_PASS;
    }
    if (result>=BattleShipDeckActions::kInvalidTarget || result==BattleShipDeckActions::kUnbound) {
        context->Assert(0,"native battleship deck configuration rejected the real account or native state");
        return SDGO_GAME_TEST_FAIL;
    }
    return SDGO_GAME_TEST_WAIT;
}
// Original startup/IME initialization can block main-thread dispatch for more
// than two minutes on a busy native host. Login still requires the real ACK;
// allow five minutes for initialization without changing room assertions.
const SdgoGameTestCaseV1 deckCase{"setup.deck",180000,&ConfigureDeck};
const SdgoGameTestCaseV1 battleshipCase{"flow.battleship",180000,&AcquireBattleship};
const SdgoGameTestCaseV1 giftCase{"setup.gift_requests",360000,&SendGiftRequests};
int PageResult(const SdgoGameTestContextV1* context,PageTestActions::Result result,
               PageTestActions::Result expected) {
    const auto state=pages.State();
    static uint32_t lastResult=UINT32_MAX,lastReceipt=UINT32_MAX;
    if (state.result!=lastResult || state.receipt_count!=lastReceipt) {
        char message[384];
        sprintf_s(message,"native hangar page result=%s stage=%u scope=%u units=%u/%u capacity=%u layout=%u active=%u gb/item=%u/%u rejected=%u actions=%u replies=%u receipts=%u",
            PageTestActions::ResultName(result),state.stage,state.scope,state.before_unit_count,state.current_unit_count,
            state.snapshot.capacity,state.layout_unchanged,state.active_slot_unchanged,state.gb_preview_removed,
            state.item_preview_removed,state.confirm_rejections,state.snapshot.action_sequence,state.snapshot.reply_sequence,state.receipt_count);
        context->Observe(message);lastResult=state.result;lastReceipt=state.receipt_count;
    }
    if (result>=PageTestActions::kInvalidContext || result==PageTestActions::kHangarUnavailable) {
        context->Assert(0,"the native hangar Mod observer rejected the same-run page or original action");
        return SDGO_GAME_TEST_FAIL;
    }
    if (result!=expected)return SDGO_GAME_TEST_WAIT;
    context->Assert(state.snapshot.loaded && state.snapshot.visible && state.current_unit_count>0,
        "the authenticated native unit-management page exposes genuine inventory through the Mod observer");
    context->Assert(state.layout_unchanged && state.active_slot_unchanged,
        "the preview and cancel flow preserves every owned unit UID, slot and active unit");
    if (expected==PageTestActions::kReadReady)
        context->Assert(state.snapshot.capacity>=1200,"the genuine server inventory response exposes the automatic minimum 1200-slot hangar capacity");
    if (expected==PageTestActions::kPreviewReady && state.scope==PageTestActions::kScopeProtectedOrNoDuplicateNegative)
        context->Assert(state.confirm_rejections==2,"both empty exchange previews reject confirmation through the original in-game handlers");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl ReadHangarPage(const SdgoGameTestContextV1* context) {
    const auto navigation=hangarNavigation.EnsureUnitManagementPage();
    if (navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound) {
        context->Assert(0,"native page navigation rejected a service or control identity");
        return SDGO_GAME_TEST_FAIL;
    }
    if (navigation!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    return PageResult(context,pages.PollHangarRead(context),PageTestActions::kReadReady);
}
int __cdecl PreviewHangarPage(const SdgoGameTestContextV1* context) {
    return PageResult(context,pages.PollHangarPreview(context),PageTestActions::kPreviewReady);
}
int __cdecl CancelHangarPage(const SdgoGameTestContextV1* context) {
    return PageResult(context,pages.PollHangarCancel(context),PageTestActions::kCancelled);
}
int __cdecl ReadOperatorCatalog(const SdgoGameTestContextV1* context) {
    if (!GetModuleHandleW(L"operator_catalog.dll")) {
        context->Assert(0,"configured operator catalog Mod is required for native catalog acceptance");
        return SDGO_GAME_TEST_FAIL;
    }
    const bool ready=operatorCatalog.Check(sdgo_test::directory+L"\\..\\mods\\operator_catalog\\display.ini");
    const auto state=operatorCatalog.State();char message[160];
    sprintf_s(message,"native operator catalog configured=%u verified=%u last=%u/%u error=%u",
        state.configured,state.verified,state.operator_id,state.category_id,state.error);
    context->Observe(message);
    context->Assert(ready,"every configured operator name and enabled category reference exists in the real native grid after normal loading");
    return ready ? SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
int __cdecl ReadAiCatalog(const SdgoGameTestContextV1* context) {
    if (!GetModuleHandleW(L"ai_catalog.dll")) {
        context->Assert(0,"owner-provided AI catalog Mod is required for native record acceptance");
        return SDGO_GAME_TEST_FAIL;
    }
    const bool ready=aiCatalog.Check(sdgo_test::directory+L"\\..\\mods\\ai_catalog\\catalog.bin");
    const auto state=aiCatalog.State();char message[160];
    sprintf_s(message,"native AI catalog configured=%u verified=%u levels=%u last=%u/%u error=%u",
        state.configured,state.verified,state.levels,state.npc_id,state.unit_id,state.error);
    context->Observe(message);
    context->Assert(ready,"every owner-provided NPC and its source unit resolve from the real native grid with the exact catalog bytes and unit hash");
    return ready ? SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
int __cdecl ReadStoragePage(const SdgoGameTestContextV1* context) {
    static bool shopExitRequested=false;
    static bool clicked=false;
    if(actions.AppState()==9) {
        if(!shopExitRequested) {
            const auto navigation=hangarNavigation.ClickBottomRadio(L"GameRoomRadio",0x48);
            if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound) {
                context->Assert(0,"the original game-room radio remains available after the shop preview");
                return SDGO_GAME_TEST_FAIL;
            }
            shopExitRequested=navigation==HangarNavigation::kReady;
        }
        return SDGO_GAME_TEST_WAIT;
    }
    const auto result=storage.Read(context);
    if(result>=StorageTestActions::Invalid) {
        context->Assert(0,"storage native observer is available and belongs to this process/run/UI thread");
        return SDGO_GAME_TEST_FAIL;
    }
    if(result==StorageTestActions::Ready) {
        clicked=false;
        const auto state=storage.State();
        if(!state.timer_observed)return SDGO_GAME_TEST_WAIT;
        char message[192];
        sprintf_s(message,"native storage tab=%u total=%u entries=%u loaded=%u visible=%u generation=%u",
            state.current_tab,state.total,state.page_entry_count,state.loaded,state.visible,state.generation);
        context->Observe(message);
        context->Assert(state.timer_observed!=0,"the attached native storage Mod runs its timer on the real private-desktop message loop");
        context->Assert(!state.busy && !state.outstanding,"normal storage list and native business are idle before batch confirmation");
        return SDGO_GAME_TEST_PASS;
    }
    if(!clicked) {
        const auto navigation=hangarNavigation.ClickOperatorMenuButton(L"SalaryBtn",0x74);
        if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound) {
            context->Assert(0,"storage entry requires the real native SalaryBtn and its original callback");
            return SDGO_GAME_TEST_FAIL;
        }
        clicked=navigation==HangarNavigation::kReady;
    }
    return SDGO_GAME_TEST_WAIT;
}
int FriendStorageAction(const SdgoGameTestContextV1* context,uint32_t operation) {
    static bool bound=false;
    if(!bound){if(!storage.Bind())return SDGO_GAME_TEST_FAIL;bound=true;}
    if(operation==1)return ReadStoragePage(context);
    if(operation==3) {
        static bool storageClosed=false;
        if(!storageClosed) {
            const auto closed=storage.Close(context);
            if(closed>=StorageTestActions::Invalid)return SDGO_GAME_TEST_FAIL;
            if(closed!=StorageTestActions::Complete)return SDGO_GAME_TEST_WAIT;
            storageClosed=true;
        }
        static bool boundHangar=false;
        if(!boundHangar){
            if(!hangarNavigation.Bind(sdgo_test::api,sdgo_test::mod,&actions) || !pages.Bind())return SDGO_GAME_TEST_FAIL;
            boundHangar=true;
        }
        const auto navigation=hangarNavigation.EnsureUnitManagementPage();
        static uint32_t previousNavigation=UINT32_MAX;
        if(navigation!=previousNavigation){char text[128];sprintf_s(text,"post-storage native hangar entry=%s",HangarNavigation::ResultName(navigation));context->Observe(text);previousNavigation=navigation;}
        if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound)return SDGO_GAME_TEST_FAIL;
        if(navigation!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
        const auto read=pages.PollHangarRead(context);
        static uint32_t previousRead=UINT32_MAX;
        if(read!=previousRead){char text[128];sprintf_s(text,"post-storage native inventory read=%s",PageTestActions::ResultName(read));context->Observe(text);previousRead=read;}
        if(read>=PageTestActions::kInvalidContext)return SDGO_GAME_TEST_FAIL;
        if(read==PageTestActions::kReadReady){storageClosed=false;return SDGO_GAME_TEST_PASS;}
        return SDGO_GAME_TEST_WAIT;
    }
    const auto result=storage.Claim(context);
    if(result>=StorageTestActions::Invalid)return SDGO_GAME_TEST_FAIL;
    return result==StorageTestActions::Complete?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_WAIT;
}
void FriendStorageWrite(FILE* file) {
    const auto state=storage.State();
    fprintf(file,"[storage]\nloaded=%u\nvisible=%u\ntab=%u\ntotal=%u\nentries=%u\ncompleted=%u\nphase=%u\nreason=%u\nbusy=%u\noutstanding=%u\nactions=%u\nreplies=%u\npreflight_replies=%u\n",
        state.loaded,state.visible,state.current_tab,state.total,state.page_entry_count,state.completed,
        state.batch_phase,state.batch_reason,state.busy,state.outstanding,state.action_sequence,state.reply_sequence,state.preflight_reply_sequence);
    const auto& units=pages.CurrentUnits();
    fprintf(file,"[storage_inventory]\ncount=%u\n",static_cast<unsigned>(units.size()));
    for(unsigned i=0;i<units.size();++i)
        fprintf(file,"[storage_inventory_unit%u]\nuid=%llu\nunit=%u\nslot=%u\n",i,
            static_cast<unsigned long long>(units[i].uid),units[i].unit_id,units[i].slot);
}
int __cdecl ClaimStoragePage(const SdgoGameTestContextV1* context) {
    static bool verifyingInventory=false;
    static std::vector<SdgoHangarGameTestUnitV1> beforeClaim;
    if(verifyingInventory) {
        const auto navigation=hangarNavigation.EnsureUnitManagementPage();
        if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound){context->Assert(0,"native inventory entry remains available after the claim");return SDGO_GAME_TEST_FAIL;}
        if(navigation!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
        const auto read=pages.PollHangarRead(context);
        if(read>=PageTestActions::kInvalidContext){context->Assert(0,"native inventory readback after claim is valid");return SDGO_GAME_TEST_FAIL;}
        if(read!=PageTestActions::kReadReady)return SDGO_GAME_TEST_WAIT;
        const auto& current=pages.CurrentUnits();bool retained=true;unsigned added=0;
        for(const auto& old:beforeClaim)retained=retained && std::any_of(current.begin(),current.end(),[&](const auto& u){return u.uid==old.uid && u.unit_id==old.unit_id && u.slot==old.slot;});
        for(const auto& u:current)if(std::none_of(beforeClaim.begin(),beforeClaim.end(),[&](const auto& old){return old.uid==u.uid;})) {
            ++added;char message[128];sprintf_s(message,"claimed native inventory added UID=%llu unit=%u slot=%u",static_cast<unsigned long long>(u.uid),u.unit_id,u.slot);context->Observe(message);
        }
        context->Assert(retained && current.size()>=beforeClaim.size(),"normal post-claim hangar readback preserves every original owned UID, unit and slot");
        char message[96];sprintf_s(message,"native inventory after positive claim before=%u after=%u added=%u",static_cast<unsigned>(beforeClaim.size()),static_cast<unsigned>(current.size()),added);context->Observe(message);
        return SDGO_GAME_TEST_PASS;
    }
    const auto result=storage.Claim(context);const auto state=storage.State();
    static uint32_t previous=UINT32_MAX,previousReplies=UINT32_MAX;
    if(previous!=static_cast<uint32_t>(result) || previousReplies!=state.reply_sequence) {
        char message[256];
        sprintf_s(message,"native storage claim result=%u actions=%u replies=%u preflight=%u phase=%u reason=%u complete=%u armed=%u outstanding=%u total=%u",
            result,state.action_sequence,state.reply_sequence,state.preflight_reply_sequence,state.batch_phase,state.batch_reason,
            state.completed,state.armed,state.outstanding,state.total);
        context->Observe(message);previous=result;previousReplies=state.reply_sequence;
    }
    if(result>=StorageTestActions::Invalid) {
        context->Assert(0,"native storage batch preserves run/session identity and completes only on genuine correlated replies");
        return SDGO_GAME_TEST_FAIL;
    }
    if(result!=StorageTestActions::Complete)return SDGO_GAME_TEST_WAIT;
    context->Assert(state.action_sequence==2 && state.reply_sequence>0 && !state.outstanding,
        "exactly two original button actions confirm one native list/claim batch and retain its server replies");
    context->Observe(state.completed?"positive native storage claim completed; delivery/readback is a separate assertion":"empty native storage list completed without any item claim; positive delivery remains untested");
    if(state.completed) {
        beforeClaim=pages.BaselineUnits();
        if(!hangarNavigation.Bind(sdgo_test::api,sdgo_test::mod,&actions) || !pages.Bind()){context->Assert(0,"native inventory observer rebinds after positive claim");return SDGO_GAME_TEST_FAIL;}
        verifyingInventory=true;return SDGO_GAME_TEST_WAIT;
    }
    return SDGO_GAME_TEST_PASS;
}
int __cdecl ReadCapsuleCatalog(const SdgoGameTestContextV1* context) {
    if(collectionDraw)collectionBeforeUnits=pages.BaselineUnits();
    const bool acquisition=unitAcquisitionConfig.machine_kind!=0;
    const bool ready=capsules.Check(sdgo_test::directory+L"\\..\\mods\\capsule_catalog\\runtime.ini",acquisition);
    const auto state=capsules.State();char message[192];
    sprintf_s(message,"native capsule tables=%u rows=%u/%u collection=%u price=%u error=%u",state.tables,state.verified,state.expected,
        state.collection_machine,state.collection_price,state.error);context->Observe(message);
    context->Assert(ready,acquisition
        ? "the isolated acquisition profile matches every declared native table, including explicitly empty tables, with exact row identities"
        : "all five configured capsule tables are registered in the actual native grid with exact counts and row identities");
    if(!acquisition)context->Assert(state.collection_machine==990001 && state.collection_price>0,
        "the collection machine has the configured positive GP price in the verified native registry");
    return ready?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
int __cdecl ReadCapsuleCurrency(const SdgoGameTestContextV1* context) {
    static bool clicked=false;static uint32_t baseline=0;
    SdgoCapsuleGameTestSnapshotV1 state{};
    if(!capsules.Currency(state)) {
        context->Assert(0,"the optional native capsule list-load observer belongs to the current GameTest");return SDGO_GAME_TEST_FAIL;
    }
    if(!clicked) {
        baseline=state.generation;
        const auto navigation=hangarNavigation.ClickBottomRadio(L"CapsuleRoomRadio",0x4C);
        if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound) {
            context->Assert(0,"capsule page entry validates the original bottom-menu radio and callback");return SDGO_GAME_TEST_FAIL;
        }
        clicked=navigation==HangarNavigation::kReady;return SDGO_GAME_TEST_WAIT;
    }
    if(state.generation<=baseline)return SDGO_GAME_TEST_WAIT;
    char message[192];sprintf_s(message,"native activity list load generation=%u result=%u grid=%u machine=%u price=%u currency=%u registry=%u",
        state.generation,state.result,state.grid_id,state.machine_id,state.price,state.currency,state.registry_currency);context->Observe(message);
    context->Assert(state.process_id==GetCurrentProcessId() && state.ui_thread_id==GetCurrentThreadId(),"the original activity-list load ran on this client's native UI thread after real page entry");
    context->Assert(state.grid_id==145093 && state.machine_id==9301 && state.price==99 && state.currency==2 && state.registry_currency==2,
        "machine 9301 uses token currency in both its loaded UI list and the registry used by the original confirmation lookup, with price 99 retained");
    return SDGO_GAME_TEST_PASS;
}
int ReadShopReady(const SdgoGameTestContextV1* context) {
    const auto state=actions.OnboardingState();
    static NativeActions::OnboardingObservation previous{};
    static bool observed=false;
    if(!observed || memcmp(&state,&previous,sizeof(state))!=0) {
        char message[224]{};
        sprintf_s(message,"native shop readiness app=%u login=%u pilot=%u tutorial=%u path=%u pending=%u",
            state.native_app_state,state.native_login_state,state.profile_pilot_id,state.tutorial_level,
            state.login_path_state,state.pending_login_event_count);
        context->Observe(message);previous=state;observed=true;
    }
    if(!state.profile_pilot_id || state.tutorial_level<2 ||
       (state.native_app_state!=5 && state.native_app_state!=6))return SDGO_GAME_TEST_WAIT;
    if(!authenticatedPilot)authenticatedPilot=state.profile_pilot_id;
    context->Assert(authenticatedPilot==state.profile_pilot_id,"the existing shop test identity is the genuine server-loaded profile after accepted login");
    context->Observe("read-only shop scope uses the native lobby or hangar; it does not claim battle-channel admission or create starter assets");
    return authenticatedPilot==state.profile_pilot_id?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
const SdgoGameTestCaseV1 shopReadyCase{"flow.shop_ready",120000,&ReadShopReady};
int ReadShopCatalog(const SdgoGameTestContextV1* context) {
    static bool diagnosticRequested=false,diagnosticComplete=false;
    if(diagnosticRequested && !diagnosticComplete) {
        const auto captured=quietGraphics.CaptureFrameResult();
        if(captured==E_PENDING)return SDGO_GAME_TEST_WAIT;
        diagnosticComplete=true;
        context->Observe(SUCCEEDED(captured)?"native shop diagnostic backbuffer saved (not a passing case)":"native shop diagnostic frame was unavailable");
        context->Assert(0,"native stock catalog and preview controls must match the sealed read-only assertions");
        return SDGO_GAME_TEST_FAIL;
    }
    static bool capturePending=false;
    static uint32_t capturedBuy=0,capturedGift=0;
    static uint32_t capturedHover=0;
    static bool hoverCapture=false;
    static bool capturedList=false;
    static ShopCatalogActions::Result capturedFailure=ShopCatalogActions::kComplete;
    if(capturePending) {
        if(hoverCapture && !shopCatalog.RefreshHover())return SDGO_GAME_TEST_FAIL;
        const auto status=quietGraphics.FrameCaptureState();
        if(status==QuietFrameCapture::Pending || status==QuietFrameCapture::Capturing)return SDGO_GAME_TEST_WAIT;
        context->Assert(status==QuietFrameCapture::Complete,"native shop framebuffer capture completed on the genuine D3D9 device");
        capturePending=false;
        hoverCapture=false;
        if(status!=QuietFrameCapture::Complete || capturedFailure!=ShopCatalogActions::kComplete)return SDGO_GAME_TEST_FAIL;
    }
    const auto before=shopCatalog.State();
    if(shopVisualOnly && !capturedList && before.stage==ShopCatalogActions::kStageScanPage) {
        const auto path=sdgo_test::directory+L"\\shop-list.bmp";
        capturedList=true;
        capturePending=quietGraphics.RequestFrameCapture(path.c_str());
        context->Assert(capturePending,"capture the populated native shop before issuing another page request");
        return capturePending?SDGO_GAME_TEST_WAIT:SDGO_GAME_TEST_FAIL;
    }
    if(before.stage==ShopCatalogActions::kStageIdle || before.stage==ShopCatalogActions::kStageBaseline) {
        // The unit-management popup only loads briefs. Enter the original
        // 3D unitmgr so its genuine REQ_UNIT_SLOT_PAGE consumer is present.
        const auto navigation=hangarNavigation.EnsureHangar();
        if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound) {
            context->Assert(0,"the original hangar unitmgr must own full inventory replies before shop validation");return SDGO_GAME_TEST_FAIL;
        }
        if(navigation!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    }
    const auto result=shopCatalog.Poll(context);const auto state=shopCatalog.State();
    if(shopCatalog.VisualOnly() && (result>=ShopCatalogActions::kInvalidContext ||
        state.buy_previews>capturedBuy || state.gift_previews>capturedGift || state.hover_previews>capturedHover ||
        (!capturedList && state.stage==ShopCatalogActions::kStageWaitPage))) {
        const bool failed=result>=ShopCatalogActions::kInvalidContext;
        wchar_t filename[96]{};
        swprintf_s(filename,L"\\shop-%s-%u.bmp",failed?L"failure":state.buy_previews>capturedBuy?L"buy":state.gift_previews>capturedGift?L"gift":state.hover_previews>capturedHover?L"hover":L"list",state.product_id);
        hoverCapture=state.hover_previews>capturedHover;
        capturedHover=state.hover_previews;
        if(state.stage==ShopCatalogActions::kStageWaitPage)capturedList=true;
        const auto path=sdgo_test::directory+filename;
        capturePending=quietGraphics.RequestFrameCapture(path.c_str());
        context->Assert(capturePending,"queue an actual shop frame while its native page or preview remains open");
        capturedBuy=state.buy_previews;capturedGift=state.gift_previews;
        capturedFailure=failed?result:ShopCatalogActions::kComplete;
        char message[128]{};sprintf_s(message,"native shop visual checkpoint result=%s product=%u",ShopCatalogActions::ResultName(result),state.product_id);context->Observe(message);
        if(failed) {char info[192]{};sprintf_s(info,"native shop failed details: options=%u nativeNameChars=%u visibleNameChars=%u first=%04X art_phase=%u",state.native_page_options,static_cast<unsigned>(state.native_name.size()),static_cast<unsigned>(state.visible_name.size()),state.visible_name.empty()?0u:static_cast<unsigned>(state.visible_name[0]),state.art_phase);context->Observe(info);}
        if(failed) {char info[512]{};sprintf_s(info,"native popup title control=%u chars=%u codeunits:",state.popup_name_found,static_cast<unsigned>(state.popup_name.size()));for(size_t i=0;i<state.popup_name.size() && i<32;++i){char code[8]{};sprintf_s(code," %04X",static_cast<unsigned>(state.popup_name[i]));strcat_s(info,code);}context->Observe(info);}
        if(failed || state.hover_previews) {
            char info[192]{};sprintf_s(info,"native tooltip readback name=%u description=%u usage=%u pointer=%u/%u",static_cast<unsigned>(state.tooltip_name.size()),static_cast<unsigned>(state.tooltip_description.size()),static_cast<unsigned>(state.tooltip_usage.size()),state.pointer_position_reads,state.pointer_state_reads);context->Observe(info);
        }
        if(failed && !state.tooltip_description.empty()) {
            const auto hex=[](const std::wstring& value){std::string text;for(const auto c:value){char code[8]{};sprintf_s(code,"%02X%02X",static_cast<unsigned>(c)&255u,static_cast<unsigned>(c)>>8);text+=code;}return text;};
            const auto description=hex(state.tooltip_description),usage=hex(state.tooltip_usage);
            context->Observe((std::string("native tooltip description UTF16LE=")+description).c_str());
            context->Observe((std::string("native tooltip usage UTF16LE=")+usage).c_str());
        }
        if(capturePending)return SDGO_GAME_TEST_WAIT;
        return SDGO_GAME_TEST_FAIL;
    }
    static uint32_t previous=UINT32_MAX,verified=UINT32_MAX,lastPending=UINT32_MAX;
    if(previous!=result || verified!=state.verified || lastPending!=state.native_request_pending) {
        const auto sent=actions.TransportState();
        char message[512];sprintf_s(message,"native shop result=%s stage=%u verified=%u/%u page=%u/%u product=%u option=%u category=%u validity=%d price=%d payment=%u previews=%u/%u cancel=%u wallet=%u inventory=%u slots=%u/%u bytes=%u pending=%u send=%04X/%u seq=%u",
            ShopCatalogActions::ResultName(result),state.stage,state.verified,state.configured,state.current_page,state.max_page,
            state.product_id,state.option_id,state.category,state.validity,state.price,state.payment,state.buy_previews,state.gift_previews,
            state.cancellations,state.wallet_unchanged,state.inventory_unchanged,state.native_page_nonempty,state.native_page_slots,state.native_page_bytes,state.native_request_pending,sent.opcode,sent.packet_size,sent.send_sequence);context->Observe(message);previous=result;verified=state.verified;lastPending=state.native_request_pending;
        char rows[160];sprintf_s(rows,"native shop page products=%u,%u,%u,%u,%u,%u,%u,%u",
            state.native_page_product_ids[0],state.native_page_product_ids[1],state.native_page_product_ids[2],state.native_page_product_ids[3],
            state.native_page_product_ids[4],state.native_page_product_ids[5],state.native_page_product_ids[6],state.native_page_product_ids[7]);context->Observe(rows);
        if(!state.native_name.empty()) {
            const auto utf8=[](const std::wstring& value){
                const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
                std::string text(static_cast<size_t>(length),0);
                if(length)WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),text.data(),length,nullptr,nullptr);
                return text;
            };
            const auto native=utf8(state.native_name),visible=utf8(state.visible_name);
            char names[512];sprintf_s(names,"native shop target title source=\"%s\" visible=\"%s\"",native.c_str(),visible.c_str());context->Observe(names);
            if(!state.option_label.empty()) {
                const auto label=utf8(state.option_label);char text[1024];
                sprintf_s(text,"native shop popup label=\"%s\"",label.c_str());context->Observe(text);
            }
            if(!state.popup_price_text.empty()) {
                const auto label=utf8(state.popup_price_text);char text[1024];
                sprintf_s(text,"native shop popup price control=\"%s\"",label.c_str());context->Observe(text);
            }
        }
        if(state.popup_phase) {
            char popup[320];sprintf_s(popup,"native shop popup phase=%u options=%u product=%u option=%u category=%u price=%d validity=%d payment=%u combo=%u state=%u comboVisible=%u priceVisible=%u",
                state.popup_phase,state.popup_option_count,state.popup_product,state.popup_option,state.popup_category,state.popup_price,
                state.popup_validity,state.popup_payment,state.popup_combo_count,state.popup_native_state,
                state.popup_combo_visible,state.popup_price_visible);context->Observe(popup);
        }
        if(state.inventory_failure) {
            char inventory[320];sprintf_s(inventory,"native shop inventory failure=%u ready=%u capacity=%d probeSlot=%u brief=%u/%u/%llu full=%u/%u/%llu",
                state.inventory_failure,state.inventory_ready,state.inventory_capacity,state.inventory_probe_slot,
                state.inventory_brief_unit,state.inventory_brief_slot,static_cast<unsigned long long>(state.inventory_brief_uid),
                state.inventory_full_unit,state.inventory_full_slot,static_cast<unsigned long long>(state.inventory_full_uid));context->Observe(inventory);
        }
    }
    if(result>=ShopCatalogActions::kInvalidContext || result==ShopCatalogActions::kUnbound || result==ShopCatalogActions::kUnconfigured) {
        if(captureNativeFrames && !diagnosticRequested) {
            const auto folder=sdgo_test::directory+L"\\visual";
            if(CreateDirectoryW(folder.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS)
                diagnosticRequested=quietGraphics.CaptureNextFrame((folder+L"\\diagnostic-shop.bmp").c_str());
            if(diagnosticRequested)return SDGO_GAME_TEST_WAIT;
        }
        context->Assert(0,"native stock catalog and preview controls must match the sealed read-only assertions");return SDGO_GAME_TEST_FAIL;
    }
    if(result!=ShopCatalogActions::kComplete)return SDGO_GAME_TEST_WAIT;
    context->Assert(state.stock_assertions && state.configured && state.verified==state.configured &&
        state.buy_previews==state.configured && state.gift_previews==state.configured && state.cancellations==2*state.configured &&
        state.wallet_unchanged && state.inventory_unchanged,
        shopVisualOnly ? "native Chinese shop names, options and real preview frames match; cancellation preserves wallet and every inventory brief" :
        "native stock names, options, permanent/rental labels and buy/gift previews match; cancellation preserves wallet and complete inventory");
    return SDGO_GAME_TEST_PASS;
}
const SdgoGameTestCaseV1 shopCatalogCase{"pages.shop.catalog",300000,&ReadShopCatalog};
const SdgoGameTestCaseV1 pageCases[]={
    {"pages.hangar.read",120000,&ReadHangarPage},
    {"pages.hangar.preview",60000,&PreviewHangarPage},
    {"pages.hangar.cancel",60000,&CancelHangarPage},
    {"catalog.operator",60000,&ReadOperatorCatalog},
    {"catalog.ai",60000,&ReadAiCatalog},
    {"pages.storage.read",60000,&ReadStoragePage},
    {"pages.storage.claim",120000,&ClaimStoragePage},
    {"catalog.capsule",60000,&ReadCapsuleCatalog},
    {"pages.capsule.currency",60000,&ReadCapsuleCurrency},
};
int CollectionResult(const SdgoGameTestContextV1* context,CapsuleDrawActions::Result result) {
    static uint32_t previous=UINT32_MAX,reply=UINT32_MAX;
    const auto s=collection.State();
    if(previous!=result || reply!=s.acknowledgement_sequence) {
        char message[384];sprintf_s(message,"native collection result=%u page=%u/%u target=%u slot=%u price=%u clone=%u request=%04X/%u/%u reply=%04X/%u ack=%04X serial=%llu category=%u amount=%u remainingGP=%u",
            static_cast<unsigned>(result),s.current_page,s.target_page,s.target_machine,s.target_slot,s.target_price,s.dialog_clone_sequence,
            s.request_opcode,s.request_size,s.request_result,s.result_reply_opcode,s.result_reply_status,s.acknowledgement_opcode,
            static_cast<unsigned long long>(s.dropbox_serial),s.reward_item_category,s.reward_amount,s.remaining_gp);
        context->Observe(message);previous=result;reply=s.acknowledgement_sequence;
    }
    // The original World maps a nonzero activity SQL return to wire status
    // 6040. An isolated SQL comparison must separately establish quota/refund.
    if(result==CapsuleDrawActions::kServerRejected && collectionExpectedStatus==6040 &&
       s.result_reply_status==6040 && s.original_reply_completed)return SDGO_GAME_TEST_WAIT;
    if(result>=CapsuleDrawActions::kMachineServiceMismatch || result==CapsuleDrawActions::kUnbound) {
        context->Assert(0,"activity capsule native action/reply identity is valid and no failed purchase is retried");return SDGO_GAME_TEST_FAIL;
    }
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl PreviewCollection(const SdgoGameTestContextV1* context) {
    // The full pages suite enters capsules in its currency case. A focused
    // encyclopedia scope must perform that real navigation itself.
    static bool focusedEntry=false;
    if(unitPagesOnly && !focusedEntry) {
        const auto hangar=hangarNavigation.EnsureUnitManagementPage();
        if(hangar==HangarNavigation::kUnbound || hangar>=HangarNavigation::kDownmenuServiceMismatch) {
            context->Assert(0,"focused capsule preview returns through the original unit-management entry");return SDGO_GAME_TEST_FAIL;
        }
        if(hangar!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
        const auto navigation=hangarNavigation.ClickBottomRadio(L"CapsuleRoomRadio",0x4C);
        if(navigation==HangarNavigation::kUnbound || navigation>=HangarNavigation::kDownmenuServiceMismatch) {
            context->Assert(0,"focused capsule preview opens the original CapsuleRoomRadio");return SDGO_GAME_TEST_FAIL;
        }
        focusedEntry=navigation==HangarNavigation::kReady;
        return SDGO_GAME_TEST_WAIT;
    }
    const auto result=collection.EnsurePreview();const int pending=CollectionResult(context,result);
    if(pending==SDGO_GAME_TEST_FAIL)return pending;
    if(result!=CapsuleDrawActions::kPreviewReady)return SDGO_GAME_TEST_WAIT;
    const auto s=collection.State();
    const auto expected=collection.ExpectedTarget();
    context->Assert(s.target_machine==expected.machine && s.target_price==expected.price && s.preview_ready && !s.buy_attempted,
        "original activity controls selected the configured native machine and verified its own currency/price confirmation without purchasing");
    if(expected.machine==990001)context->Assert(s.target_price>0 && s.target_price==capsules.State().collection_price,
        "collection confirmation uses the current native catalog price");
    context->Assert(!s.buy_submitted && !s.request_transport_sequence,
        "the preview has not submitted an activity-capsule purchase transport");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl DrawCollection(const SdgoGameTestContextV1* context) {
    auto state=collection.State();
    auto result=state.buy_attempted?collection.Poll():collection.DrawOnce();
    if(result==CapsuleDrawActions::kResultReady || result==CapsuleDrawActions::kClosingResult)result=collection.CloseResult();
    const int pending=CollectionResult(context,result);if(pending==SDGO_GAME_TEST_FAIL)return pending;
    if(collectionExpectedStatus==6040) {
        state=collection.State();
        if(state.acknowledgement_sequence || state.dropbox_serial || state.reward_amount) {
            context->Assert(0,"an exhausted quota must reject the purchase instead of delivering a prize");
            return SDGO_GAME_TEST_FAIL;
        }
        if(result!=CapsuleDrawActions::kServerRejected)return SDGO_GAME_TEST_WAIT;
        context->Assert(state.buy_submitted && state.request_opcode==0x05D3 && state.request_size==10 &&
            state.result_reply_status==6040 && state.original_reply_completed &&
            !state.acknowledgement_sequence && !state.dropbox_serial && !state.reward_amount,
            "one genuine native collection purchase was rejected by the configured quota with no prize acknowledgement");
        return SDGO_GAME_TEST_PASS;
    }
    if(result!=CapsuleDrawActions::kComplete)return SDGO_GAME_TEST_WAIT;
    state=collection.State();
    context->Assert(state.buy_attempted && state.buy_submitted && state.request_opcode==0x05D3 && state.request_size==10 &&
        !state.result_reply_status && state.acknowledgement_opcode==0x09C6 &&
        state.original_reply_completed && state.dropbox_serial && collection.AcceptsReward(state.reward_item_category,state.reward_amount),
        "one original activity purchase consumed genuine delivery acknowledgements and its original result-close callback was accepted");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl DeliverCollection(const SdgoGameTestContextV1* context) {
    if(collectionExpectedStatus==6040) {
        const auto state=collection.State();
        context->Assert(state.result_reply_status==6040 && state.original_reply_completed &&
            !state.dropbox_serial && !state.reward_item_category && !state.reward_amount,
            "an exhausted-quota rejection has no collection prize to claim");
        return SDGO_GAME_TEST_PASS;
    }
    static uint32_t stage=0;static bool clicked=false;
    if(stage==0) {
        if(actions.AppState()==6) { if(!storage.Bind()){context->Assert(0,"native storage export binding remains available");return SDGO_GAME_TEST_FAIL;}stage=1;clicked=false; }
        else if(!clicked) { const auto r=hangarNavigation.ClickBottomRadio(L"GameRoomRadio",0x48);
            if(r>=HangarNavigation::kDownmenuServiceMismatch || r==HangarNavigation::kUnbound){context->Assert(0,"original GameRoomRadio returns from capsule before claim");return SDGO_GAME_TEST_FAIL;}
            clicked=r==HangarNavigation::kReady; }
        return SDGO_GAME_TEST_WAIT;
    }
    if(stage==1) {
        const auto r=storage.Read(context);
        if(r>=StorageTestActions::Invalid){context->Assert(0,"same-run native storage opened after collection delivery");return SDGO_GAME_TEST_FAIL;}
        if(r==StorageTestActions::Ready && storage.State().timer_observed) {context->Assert(storage.State().total==1,"the freshly delivered collection reward is the only pending item");stage=2;}
        else if(!clicked) {const auto n=hangarNavigation.ClickOperatorMenuButton(L"SalaryBtn",0x74);if(n>=HangarNavigation::kDownmenuServiceMismatch || n==HangarNavigation::kUnbound){context->Assert(0,"original SalaryBtn opens native drop-box");return SDGO_GAME_TEST_FAIL;}clicked=n==HangarNavigation::kReady;}
        return SDGO_GAME_TEST_WAIT;
    }
    if(stage==2) {
        const auto r=storage.Claim(context);
        if(r>=StorageTestActions::Invalid){context->Assert(0,"collection claim requires native preflight and correlated claim reply");return SDGO_GAME_TEST_FAIL;}
        if(r!=StorageTestActions::Complete)return SDGO_GAME_TEST_WAIT;
        context->Assert(storage.State().completed==1 && storage.State().preflight_reply_sequence>0,"one native drop-box item was actually claimed after its server preflight");
        if(!hangarNavigation.Bind(sdgo_test::api,sdgo_test::mod,&actions) || !pages.Bind()){context->Assert(0,"native page observers rebind after claim");return SDGO_GAME_TEST_FAIL;}
        stage=3;return SDGO_GAME_TEST_WAIT;
    }
    const auto navigation=hangarNavigation.EnsureUnitManagementPage();
    if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound){context->Assert(0,"original unit-management entry returns after claim");return SDGO_GAME_TEST_FAIL;}
    if(navigation!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    const auto read=pages.PollHangarRead(context);
    if(read>=PageTestActions::kInvalidContext){context->Assert(0,"native hangar readback remains valid after claim");return SDGO_GAME_TEST_FAIL;}
    if(read!=PageTestActions::kReadReady)return SDGO_GAME_TEST_WAIT;
    const auto& current=pages.CurrentUnits();bool retained=true;
    for(const auto& old:collectionBeforeUnits)retained=retained && std::any_of(current.begin(),current.end(),[&](const auto& u){return u.uid==old.uid && u.unit_id==old.unit_id && u.slot==old.slot;});
    context->Assert(retained && current.size()==collectionBeforeUnits.size()+1,"real native inventory contains one added unit and every original UID/unit/slot is retained");
    using DataFn=void*(__cdecl*)();using FindFn=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    const auto data=reinterpret_cast<DataFn>(context->api->Symbol(sdgo_test::mod,"grid.data"));
    const auto find=reinterpret_cast<FindFn>(context->api->Symbol(sdgo_test::mod,"grid.find"));
    const uint8_t* category=nullptr;uint32_t unit=0;
    const auto delivery=collection.State();
    if(data && find && find(data(),&category,delivery.reward_item_category,0x22,0) && category)memcpy(&unit,category+0x69,4);
    const auto added=std::find_if(current.begin(),current.end(),[&](const auto& candidate){return candidate.unit_id==unit &&
        std::none_of(collectionBeforeUnits.begin(),collectionBeforeUnits.end(),[&](const auto& old){return old.uid==candidate.uid;});});
    context->Assert(unit && added!=current.end(),"the added native unit matches the exact item category in the genuine activity delivery acknowledgement");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl SelectPlayUnit(const SdgoGameTestContextV1* context) {
    ActiveSelectionObserver::Snapshot equipped;
    const bool observed=activeSelection.Read(authenticatedPilot,&equipped);
    if(observed && equipped.inventory_matches && equipped.uid==playUnitUid) {
        char message[192];sprintf_s(message,"native active unit already equipped: pilot=%u unit=%u uid=%llu slot=%u; no hangar transition or selection request",equipped.pilot,equipped.unit,static_cast<unsigned long long>(equipped.uid),equipped.slot);
        context->Observe(message);
        context->Assert(equipped.pilot==authenticatedPilot && equipped.inventory_matches,
            "the server-loaded active unit matches the authenticated pilot and its real owned inventory record");
        return SDGO_GAME_TEST_PASS;
    }
    if(!observed && context->elapsed_ms<5000)return SDGO_GAME_TEST_WAIT;
    static bool diagnosticFailure=false;static uint64_t failureAt=0;
    if(diagnosticFailure) {
        if(context->elapsed_ms-failureAt<10000)return SDGO_GAME_TEST_WAIT;
        context->Assert(0,"owned unit selection failed; diagnostic pause performed no further native actions");return SDGO_GAME_TEST_FAIL;
    }
    const auto navigation=hangarNavigation.EnsureHangar();
    if(navigation>=HangarNavigation::kDownmenuServiceMismatch || navigation==HangarNavigation::kUnbound){context->Assert(0,"native hangar entry precedes owned unit selection");return SDGO_GAME_TEST_FAIL;}
    if(navigation!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    const auto result=unitSelection.Poll(playUnitUid);const auto state=unitSelection.State();
    static uint32_t previous=UINT32_MAX,reply=UINT32_MAX;
    if(previous!=result || reply!=state.reply_sequence) {
        char message[448];sprintf_s(message,"native unit select result=%u target=%llu unit=%u slot=%u current=%llu/%u request=%04X/%u/%u reply=%04X/%u consumed=%u inventory=%u/%u badslot=%u raw=%u/%llu/%u click=%d/%d accepted=%u/%u",
            static_cast<unsigned>(result),static_cast<unsigned long long>(state.target_uid),state.target_unit_id,state.target_slot,
            static_cast<unsigned long long>(state.current_uid),state.current_slot,state.request_opcode,state.request_size,state.request_result,
            state.reply_opcode,state.reply_status,state.reply_consumed,state.inventory_ready,state.inventory_capacity,state.inventory_bad_slot,
            state.inventory_bad_unit_id,static_cast<unsigned long long>(state.inventory_bad_uid),state.inventory_bad_record_slot,
            state.native_click_x,state.native_click_y,state.native_mouse_down_accepted,state.native_mouse_up_accepted);context->Observe(message);previous=result;reply=state.reply_sequence;
        char graph[384];sprintf_s(graph,"unit graph root=%08X list=%08X row=%08X control=%08X getter=%u xybits=%08X/%08X finite=%u/%u indices=%d/%d page=%u occupied=%u rowbefore=%u rowafter=%u",
            state.native_root_pointer,state.native_list_pointer,state.native_row_pointer,state.native_row_control_pointer,state.native_row_getter_return,
            state.native_row_x_bits,state.native_row_y_bits,state.native_row_x_finite,state.native_row_y_finite,
            state.native_list_pressed_index,state.native_list_selected_index,state.unit_manager_page,state.native_target_occupied,state.native_row_selected_before,state.native_row_selected);
        context->Observe(graph);
        char pager[192];sprintf_s(pager,"native unit page raw=%d max=%d group=%d maxgroup=%d target=%u",
            state.unit_manager_page_raw,state.unit_manager_max_page_raw,state.unit_manager_page_group_raw,
            state.unit_manager_max_page_group_raw,state.target_page);context->Observe(pager);
    }
    if(result>=UnitSelectActions::kInvalidTarget || result==UnitSelectActions::kUnbound){diagnosticFailure=true;failureAt=context->elapsed_ms;context->Observe("unit selection failure latched; ten-second read-only diagnostic pause with no more input/business calls");return SDGO_GAME_TEST_WAIT;}
    if(result==UnitSelectActions::kAlreadySelected) {
        context->Assert(state.current_uid==playUnitUid && state.target_matches,"native inventory and profile already expose the configured owned unit; no new change is claimed");return SDGO_GAME_TEST_PASS;
    }
    if(result!=UnitSelectActions::kComplete)return SDGO_GAME_TEST_WAIT;
    context->Assert(state.request_opcode==0x04CF && state.request_size==14 && state.reply_opcode==0x09B4 && state.reply_consumed &&
        !state.reply_status && state.current_uid==playUnitUid && state.target_matches,"the original equip request and genuine reply select exactly the configured owned unit in the real profile");
    return SDGO_GAME_TEST_PASS;
}
const SdgoGameTestCaseV1 playUnitCase{"setup.active_unit",120000,&SelectPlayUnit};
// Real OC lobby round trip through the production client Mod: select the play
// unit, read its OC profile from the server, add the configured normal points
// to one stat and save. The following battle then runs with that allocation.
int __cdecl OcAllocate(const SdgoGameTestContextV1* context) {
    static SdgoOcCustomClientGetSnapshotV1Fn snapshotFn=nullptr;
    static SdgoOcCustomClientActionV1Fn actionFn=nullptr;
    static int phase=0;static uint8_t before=0;static uint32_t beforeRevision=0,beforeReplies=0;
    if(!context->tick){phase=0;before=0;beforeRevision=0;beforeReplies=0;}
    if(!snapshotFn || !actionFn) {
        const HMODULE module=GetModuleHandleW(L"oc_custom_client.dll");
        snapshotFn=module ? reinterpret_cast<SdgoOcCustomClientGetSnapshotV1Fn>(GetProcAddress(module,"SdgoOcCustomClientGetSnapshotV1")):nullptr;
        actionFn=module ? reinterpret_cast<SdgoOcCustomClientActionV1Fn>(GetProcAddress(module,"SdgoOcCustomClientActionV1")):nullptr;
        if(!snapshotFn || !actionFn){context->Assert(0,"the OC client Mod exposes its observer and action exports");return SDGO_GAME_TEST_FAIL;}
    }
    SdgoOcCustomClientSnapshotV1 s{};s.size=sizeof(s);s.version=SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
    if(!snapshotFn(&s)){context->Assert(0,"the OC client observer snapshot is readable");return SDGO_GAME_TEST_FAIL;}
    const auto act=[&](uint32_t action,uint32_t stat){
        SdgoOcCustomClientActionV1 request{};request.size=sizeof(request);request.version=SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
        request.action=action;request.stat=stat;request.argument64=playUnitUid;return actionFn(&request)!=0;};
    const auto stat=static_cast<uint32_t>(ocAllocateStat);
    if(phase==0) {
        if(!s.transport_ready) {
            if(context->elapsed_ms>90000){context->Assert(0,"the friend-gift carrier becomes ready for OC requests");return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        if(!act(9,0)){context->Assert(0,"the OC unit selection action is accepted on the game thread");return SDGO_GAME_TEST_FAIL;}
        beforeReplies=s.reply_count;phase=1;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==1) {
        if(s.busy || s.reply_count<=beforeReplies)return SDGO_GAME_TEST_WAIT;
        if(!s.profile_present) {
            char failure[200];sprintf_s(failure,"the server runs OC mode and returned the play unit's OC profile (oc_mode=%u status=%d ret=%d command=0x%X)",
                s.server_oc_mode,s.last_status,s.last_ret_val,s.last_command);
            context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
        }
        context->Assert(s.server_oc_mode,"the server reports OC mode for the lobby session");
        context->Assert(s.unit_uid==playUnitUid,"the OC profile belongs to the configured play unit");
        before=s.normal[stat];beforeRevision=s.revision;
        char message[320];sprintf_s(message,"oc profile uid=%llu unit=%u oc_level=%u native_level=%u revision=%u normal_left=%u over_left=%u base=%d/%d/%d/%d/%d/%d gain=%d/%d/%d/%d/%d/%d normal=%u/%u/%u/%u/%u/%u",
            static_cast<unsigned long long>(s.unit_uid),s.unit_number,s.oc_level,s.native_level,s.revision,s.normal_points_left,s.over_points_left,
            s.base[0],s.base[1],s.base[2],s.base[3],s.base[4],s.base[5],s.normal_gain[0],s.normal_gain[1],s.normal_gain[2],s.normal_gain[3],s.normal_gain[4],s.normal_gain[5],
            s.normal[0],s.normal[1],s.normal[2],s.normal[3],s.normal[4],s.normal[5]);
        context->Observe(message);
        if(s.normal_points_left<static_cast<uint32_t>(ocAllocatePoints)){context->Assert(0,"the play unit has enough unallocated normal OC points for the requested allocation");return SDGO_GAME_TEST_FAIL;}
        for(int i=0;i<ocAllocatePoints;++i)if(!act(2,stat)){context->Assert(0,"the OC plus action is accepted");return SDGO_GAME_TEST_FAIL;}
        if(!act(4,0)){context->Assert(0,"the OC apply action is accepted");return SDGO_GAME_TEST_FAIL;}
        beforeReplies=s.reply_count;phase=2;return SDGO_GAME_TEST_WAIT;
    }
    if(s.busy || s.reply_count<=beforeReplies)return SDGO_GAME_TEST_WAIT;
    context->Assert(s.last_status==0 && s.last_ret_val==0,"the server accepted the OC allocation through the original rule procedure");
    context->Assert(s.normal[stat]==static_cast<uint8_t>(before+ocAllocatePoints),"the stored OC allocation increased by the requested points");
    context->Assert(s.revision>beforeRevision,"the OC profile revision advanced after the save");
    char message[256];sprintf_s(message,"oc allocation saved uid=%llu stat=%d points=%d before=%u after=%u revision=%u->%u status=%d ret=%d normal_left=%u",
        static_cast<unsigned long long>(s.unit_uid),ocAllocateStat,ocAllocatePoints,before,s.normal[stat],beforeRevision,s.revision,s.last_status,s.last_ret_val,s.normal_points_left);
    context->Observe(message);
    return SDGO_GAME_TEST_PASS;
}
const SdgoGameTestCaseV1 ocAllocateCase{"oc.allocate",180000,&OcAllocate};
// Real factory round trip for the OC client panel after the battle: open the
// original factory room from the operator menu, select its unit upgrade tab,
// wait for the Mod panel to attach to that page, press the panel's refresh
// button through the page's original UI callback, check the profile reply and
// keep a frame of the page under visual\.
int __cdecl OcPanel(const SdgoGameTestContextV1* context) {
    static SdgoOcCustomClientGetSnapshotV1Fn snapshotFn=nullptr;
    static int phase=0;static void* page=nullptr;static uint32_t beforeReplies=0,beforeRequests=0;static uint64_t readyAt=0;
    static int captureState=0;static HangarNavigation::Result previous=HangarNavigation::kUnbound;
    static uint32_t resetCost=0,sumBefore=0,leftBefore=0,revisionBefore=0;
    if(!context->tick){phase=0;page=nullptr;beforeReplies=0;beforeRequests=0;readyAt=0;captureState=0;previous=HangarNavigation::kUnbound;
        resetCost=sumBefore=leftBefore=revisionBefore=0;}
    // Keep one frame of the real page: first the original page alone, later the page with the panel.
    const auto capture=[&](const wchar_t* name)->int {
        if(captureState==0) {
            if(GetTickCount64()-readyAt<1500)return SDGO_GAME_TEST_WAIT;
            const auto folder=sdgo_test::directory+L"\\visual";CreateDirectoryW(folder.c_str(),nullptr);
            captureState=quietGraphics.CaptureNextFrame((folder+L"\\"+name+L".bmp").c_str()) ? 1:2;
            if(captureState==2)context->Observe("native frame capture unavailable for the factory page");
            return SDGO_GAME_TEST_WAIT;
        }
        if(captureState==1) {
            const auto captured=quietGraphics.CaptureFrameResult();
            if(captured==E_PENDING)return SDGO_GAME_TEST_WAIT;
            context->Observe(FAILED(captured) ? "native frame capture of the factory page failed" : "native frame of the factory page saved under visual\\");
            captureState=2;
        }
        return SDGO_GAME_TEST_PASS;
    };
    if(phase==0) {
        const auto result=hangarNavigation.EnsureFactoryUpgradePage(&page);
        if(result!=previous) {
            char message[200];sprintf_s(message,"native factory navigation result=%s app=%u clicks=%u/%u service_vtable=%08X",
                HangarNavigation::ResultName(result),actions.AppState(),hangarNavigation.FactoryClicks(),hangarNavigation.FactoryTabClicks(),
                static_cast<unsigned>(hangarNavigation.LastServiceVtable()));
            context->Observe(message);previous=result;
        }
        if(result>=HangarNavigation::kDownmenuServiceMismatch || result==HangarNavigation::kUnbound) {
            context->Assert(0,"original factory navigation accepted its native service and control identities");return SDGO_GAME_TEST_FAIL;
        }
        if(result!=HangarNavigation::kReady || !page)return SDGO_GAME_TEST_WAIT;
        context->Observe("original factory unit upgrade page is open");readyAt=GetTickCount64();captureState=0;phase=1;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==1) {
        if(capture(L"factory-upgrade")!=SDGO_GAME_TEST_PASS)return SDGO_GAME_TEST_WAIT;
        context->Observe("original factory upgrade page stayed alive for the frame");readyAt=GetTickCount64();phase=2;return SDGO_GAME_TEST_WAIT;
    }
    if(!snapshotFn) {
        const HMODULE module=GetModuleHandleW(L"oc_custom_client.dll");
        snapshotFn=module ? reinterpret_cast<SdgoOcCustomClientGetSnapshotV1Fn>(GetProcAddress(module,"SdgoOcCustomClientGetSnapshotV1")):nullptr;
        if(!snapshotFn){context->Assert(0,"the OC client Mod is loaded and exposes its observer export");return SDGO_GAME_TEST_FAIL;}
    }
    SdgoOcCustomClientSnapshotV1 s{};s.size=sizeof(s);s.version=SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
    if(!snapshotFn(&s)){context->Assert(0,"the OC client observer snapshot is readable");return SDGO_GAME_TEST_FAIL;}
    if(phase==2) {
        if(!s.panel_attached) {
            if(GetTickCount64()-readyAt>60000){context->Assert(0,"the OC panel attached to the original factory upgrade page");return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        if(s.busy)return SDGO_GAME_TEST_WAIT;
        char attached[256];sprintf_s(attached,"oc panel attached uid=%llu unit=%u profile=%u carrier=%u delta=%d/%d/%d/%d/%d/%d requests=%u replies=%u status=%d",
            static_cast<unsigned long long>(s.unit_uid),s.unit_number,s.profile_present,s.carrier_present,s.carrier_delta[0],s.carrier_delta[1],
            s.carrier_delta[2],s.carrier_delta[3],s.carrier_delta[4],s.carrier_delta[5],s.request_count,s.reply_count,s.last_status);
        context->Observe(attached);
        context->Assert(s.unit_uid==playUnitUid,"the panel follows the page's active unit record, which is the configured play unit");
        readyAt=GetTickCount64();phase=6;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==6) {
        // The OC overlay opens from its toggle button on the page.
        const auto clicked=hangarNavigation.ClickFactoryPageControl(page,L"ModOcToggle");
        if(clicked==HangarNavigation::kWaitingForFactoryPage && GetTickCount64()-readyAt<20000)return SDGO_GAME_TEST_WAIT;
        if(clicked!=HangarNavigation::kReady) {
            char failure[200];sprintf_s(failure,"the OC toggle button is reachable through the page's original UI callback (%s flags=%u)",
                HangarNavigation::ResultName(clicked),hangarNavigation.LastPageFlags());
            context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
        }
        context->Observe("opened the OC overlay with its toggle button through the page's original UI callback");
        readyAt=GetTickCount64();phase=5;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==5) {
        // The refresh button is enabled once the panel's pending requests settle.
        if(s.busy)return SDGO_GAME_TEST_WAIT;
        beforeReplies=s.reply_count;beforeRequests=s.request_count;
        const auto clicked=hangarNavigation.ClickFactoryPageControl(page,L"ModOcRefresh");
        if(clicked==HangarNavigation::kWaitingForFactoryPage && GetTickCount64()-readyAt<20000)return SDGO_GAME_TEST_WAIT;
        if(clicked!=HangarNavigation::kReady) {
            char failure[200];sprintf_s(failure,"the panel's refresh button is reachable through the page's original UI callback (%s flags=%u)",
                HangarNavigation::ResultName(clicked),hangarNavigation.LastPageFlags());
            context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
        }
        context->Observe("pressed the panel's refresh button through the page's original UI callback");
        readyAt=GetTickCount64();phase=3;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==3) {
        if(s.busy || s.reply_count<=beforeReplies) {
            if(GetTickCount64()-readyAt>90000){context->Assert(0,"the refresh pressed on the panel produced a profile reply");return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        context->Assert(s.request_count>beforeRequests,"the refresh button sent a profile request through the page's original callback");
        context->Assert(s.last_command==0x1001 && s.last_status==0 && s.last_ret_val==0 && s.profile_present && s.unit_uid==playUnitUid,
            "the server answered the panel's refresh with the play unit's OC profile");
        char message[320];sprintf_s(message,"oc panel refresh uid=%llu oc_level=%u native_level=%u revision=%u normal=%u/%u/%u/%u/%u/%u normal_left=%u over_left=%u carrier=%u delta=%d/%d/%d/%d/%d/%d",
            static_cast<unsigned long long>(s.unit_uid),s.oc_level,s.native_level,s.revision,s.normal[0],s.normal[1],s.normal[2],s.normal[3],s.normal[4],s.normal[5],
            s.normal_points_left,s.over_points_left,s.carrier_present,s.carrier_delta[0],s.carrier_delta[1],s.carrier_delta[2],s.carrier_delta[3],s.carrier_delta[4],s.carrier_delta[5]);
        context->Observe(message);
        readyAt=GetTickCount64();phase=10;return SDGO_GAME_TEST_WAIT;
    }
    // The remaining OC rules run through the panel's own buttons, each pressed
    // through the page's original UI callback once the panel enables it.
    const auto press=[&](const wchar_t* name,int next)->int {
        if(s.busy){
            if(GetTickCount64()-readyAt>60000){context->Assert(0,"the OC panel settled before the next button press");return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        const auto clicked=hangarNavigation.ClickFactoryPageControl(page,name);
        if(clicked==HangarNavigation::kWaitingForFactoryPage && GetTickCount64()-readyAt<20000)return SDGO_GAME_TEST_WAIT;
        if(clicked!=HangarNavigation::kReady) {
            char failure[200];sprintf_s(failure,"the panel button %ls is enabled and reachable through the page's original UI callback (%s flags=%u)",
                name,HangarNavigation::ResultName(clicked),hangarNavigation.LastPageFlags());
            context->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
        }
        char pressed[96];sprintf_s(pressed,"pressed panel button %ls",name);context->Observe(pressed);
        beforeReplies=s.reply_count;readyAt=GetTickCount64();phase=next;return SDGO_GAME_TEST_WAIT;
    };
    const auto replied=[&](const char* what)->int {
        if(s.busy || s.reply_count<=beforeReplies) {
            if(GetTickCount64()-readyAt>60000){context->Assert(0,what);return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        return SDGO_GAME_TEST_PASS;
    };
    const auto sum=[&](){uint32_t total=0;for(int i=0;i<6;++i)total+=s.normal[i];return total;};
    if(phase==10) {
        sumBefore=sum();leftBefore=s.normal_points_left;
        return press(L"ModOcReset",11);
    }
    if(phase==11) {
        const auto r=replied("the reset button produced a reset cost reply");if(r!=SDGO_GAME_TEST_PASS)return r;
        context->Assert(s.last_command==0x1005 && s.last_status==0 && s.last_ret_val==0,
            "the server quoted the normal-point reset cost through the original reset-cost procedure");
        resetCost=s.last_reset_gp;
        char message[128];sprintf_s(message,"oc reset cost gp=%u normal=%u left=%u",resetCost,sumBefore,leftBefore);context->Observe(message);
        readyAt=GetTickCount64();phase=12;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==12)return press(L"ModOcReset",13);
    if(phase==13) {
        const auto r=replied("the confirmed reset produced a reset reply");if(r!=SDGO_GAME_TEST_PASS)return r;
        context->Assert(s.last_command==0x1006 && s.last_status==0 && s.last_ret_val==0,
            "the server reset the normal points and charged the quoted GP in one transaction");
        context->Assert(sum()==0 && s.normal_points_left==leftBefore+sumBefore,
            "every normal point returned to the unit's unallocated budget");
        char message[160];sprintf_s(message,"oc reset done gp=%u normal=%u/%u/%u/%u/%u/%u left=%u revision=%u",resetCost,
            s.normal[0],s.normal[1],s.normal[2],s.normal[3],s.normal[4],s.normal[5],s.normal_points_left,s.revision);
        context->Observe(message);revisionBefore=s.revision;
        readyAt=GetTickCount64();phase=14;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==14)return press(L"ModOcPlus1",15);
    if(phase==15) {
        if(s.pending_normal[1]!=1) {
            if(GetTickCount64()-readyAt>10000){context->Assert(0,"the HP plus button added one pending normal point");return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        return press(L"ModOcApply",16);
    }
    if(phase==16) {
        const auto r=replied("the apply button produced a save reply");if(r!=SDGO_GAME_TEST_PASS)return r;
        context->Assert(s.last_command==0x1002 && s.last_status==0 && s.last_ret_val==0 && s.normal[1]==1 && s.revision>revisionBefore,
            "the panel saved one HP normal point through the original allocation procedure");
        char message[160];sprintf_s(message,"oc panel allocation saved normal=%u/%u/%u/%u/%u/%u left=%u revision=%u",
            s.normal[0],s.normal[1],s.normal[2],s.normal[3],s.normal[4],s.normal[5],s.normal_points_left,s.revision);
        context->Observe(message);
        readyAt=GetTickCount64();phase=ocChallenge ? 17:19;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==17)return press(L"ModOcChallenge",18);
    if(phase==18) {
        const auto r=replied("the challenge button produced an upgrade reply");if(r!=SDGO_GAME_TEST_PASS)return r;
        context->Assert(s.last_command==0x1004 && s.last_status==0,
            "the server answered the OC challenge through the original upgrade procedure");
        char message[200];sprintf_s(message,"oc challenge ret=%d success=%u oc_level=%u over_left=%u normal_left=%u revision=%u",
            s.last_ret_val,s.last_upgrade_success,s.oc_level,s.over_points_left,s.normal_points_left,s.revision);
        context->Observe(message);
        readyAt=GetTickCount64();phase=19;return SDGO_GAME_TEST_WAIT;
    }
    if(phase==19){readyAt=GetTickCount64();captureState=0;phase=4;return SDGO_GAME_TEST_WAIT;}
    return capture(L"oc-panel");
}
const SdgoGameTestCaseV1 ocPanelCase{"oc.panel",240000,&OcPanel};
bool ReadNativeIdOccupancy(const SdgoGameTestContextV1* context,uint8_t* present,uintptr_t* gridAddress) {
    using Data=void*(__cdecl*)();
    using Find=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    using MapFind=void(__thiscall*)(void*,void**,const uint32_t*);
    const auto data=reinterpret_cast<Data>(context->api->Symbol(sdgo_test::mod,"grid.data"));
    const auto find=reinterpret_cast<Find>(context->api->Symbol(sdgo_test::mod,"grid.find"));
    const auto mapFind=reinterpret_cast<MapFind>(context->api->Symbol(sdgo_test::mod,"grid.map_find"));
    if(!data || !find || !mapFind)return false;
    __try {
        auto* grid=data();if(!grid)return false;
        auto* end=*reinterpret_cast<void**>(static_cast<uint8_t*>(grid)+4);if(!end)return false;
        const uint8_t* witness=nullptr;
        if(!find(grid,&witness,nativeIdProbeWitness,2,0) || !witness ||
            *reinterpret_cast<const uint32_t*>(witness)!=nativeIdProbeWitness)return false;
        void* witnessNode=nullptr;mapFind(grid,&witnessNode,&nativeIdProbeWitness);
        if(!witnessNode || witnessNode==end)return false;
        *gridAddress=reinterpret_cast<uintptr_t>(grid);
        // The original map lookup is the same key-only query used by the
        // importer's collision guard. It does not allocate/register records,
        // traverse assumed node layouts, or filter out unknown record kinds.
        for(size_t index=0;index<nativeIdProbeIds.size();++index) {
            void* node=nullptr;const auto id=nativeIdProbeIds[index];mapFind(grid,&node,&id);
            if(!node)return false;
            present[index]=node!=end;
        }
        return data()==grid && *reinterpret_cast<void**>(static_cast<uint8_t*>(grid)+4)==end;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
int __cdecl NativeIdOccupancy(const SdgoGameTestContextV1* context) {
    uint8_t present[3072]{};uintptr_t grid=0;
    if(nativeIdProbeIds.empty() || !ReadNativeIdOccupancy(context,present,&grid)) {
        context->Assert(0,"the original native grid and positive unit witness are available");return SDGO_GAME_TEST_FAIL;
    }
    uint32_t occupied=0;
    std::string report="{\"schema\":\"sdgo.native-id-occupancy/1\",\"run_id\":\""+std::string(context->run_id)+
        "\",\"witness_unit_id\":"+std::to_string(nativeIdProbeWitness)+",\"native_grid\":"+std::to_string(grid)+
        ",\"thread_id\":"+std::to_string(GetCurrentThreadId())+",\"queried_ids\":[";
    for(size_t index=0;index<nativeIdProbeIds.size();++index) {
        if(index)report+=",";report+=std::to_string(nativeIdProbeIds[index]);
    }
    report+="],\"occupied_ids\":[";
    for(size_t index=0;index<nativeIdProbeIds.size();++index)if(present[index]) {
        if(occupied++)report+=",";report+=std::to_string(nativeIdProbeIds[index]);
    }
    report+="],\"ids_reserved\":false,\"database_writes\":false,\"scope\":\"post-login prepared client global-tree lookup; revalidation at installation remains required\"}\n";
    const auto path=sdgo_test::directory+L"\\native-id-occupancy.json";
    auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) {
        context->Assert(0,"a fresh owned native-ID observation file is created");return SDGO_GAME_TEST_FAIL;
    }
    DWORD written=0;const auto ok=WriteFile(file,report.data(),static_cast<DWORD>(report.size()),&written,nullptr);
    CloseHandle(file);
    if(!ok || written!=report.size()) {
        context->Assert(0,"the complete native-ID observation is written");return SDGO_GAME_TEST_FAIL;
    }
    char message[192];sprintf_s(message,"original native map queried=%u occupied=%u witness=%u grid=%08X; no registration or reservation",
        static_cast<uint32_t>(nativeIdProbeIds.size()),occupied,nativeIdProbeWitness,static_cast<uint32_t>(grid));
    context->Observe(message);
    context->Assert(!nativeIdProbeRequireAbsent || !occupied,"every required candidate ID is absent from this actual native global tree");
    return nativeIdProbeRequireAbsent && occupied ? SDGO_GAME_TEST_FAIL:SDGO_GAME_TEST_PASS;
}
int BlueprintResult(const SdgoGameTestContextV1* context,BlueprintActions::Result result) {
    const auto s=blueprint.State();
    static uint32_t previous=0,phase=0,rows=0;static uintptr_t form=0;
    if(previous!=result || phase!=s.phase || rows!=s.rows || form!=s.form) {
        char message[384];sprintf_s(message,"original blueprint result=%u phase=%u recipe=%u book=%llu selected=%llu rows=%u row=%u page=%u materials=%u form=%08X list=%08X mixing=%08X slots=%08X visible=%u mouse=%u select=%u radio=%ls",
            result,s.phase,s.recipe,static_cast<unsigned long long>(s.book),static_cast<unsigned long long>(s.selected_book),
            s.rows,s.row_index,s.page,s.material_count,static_cast<uint32_t>(s.form),static_cast<uint32_t>(s.list),
            static_cast<uint32_t>(s.mixing),static_cast<uint32_t>(s.slots),s.visible,s.mouse_calls,s.book_calls,s.entry_control);
        context->Observe(message);previous=result;phase=s.phase;rows=s.rows;form=s.form;
    }
    if(result>=BlueprintActions::Invalid) {
        context->Assert(0,"original blueprint services, controls and authentic book identity remain valid");return SDGO_GAME_TEST_FAIL;
    }
    if(result!=BlueprintActions::Ready)return SDGO_GAME_TEST_WAIT;
    if(captureNativeFrames) {
        static std::string capturedCase;static uint64_t readyAt=0;static bool requested=false;
        if(capturedCase!=context->case_id) {
            capturedCase=context->case_id;readyAt=GetTickCount64();requested=false;
        }
        if(!requested) {
            if(GetTickCount64()-readyAt<500)return SDGO_GAME_TEST_WAIT;
            const auto folder=sdgo_test::directory+L"\\visual";CreateDirectoryW(folder.c_str(),nullptr);
            const std::wstring name(capturedCase.begin(),capturedCase.end());
            requested=quietGraphics.CaptureNextFrame((folder+L"\\"+name+L".bmp").c_str());
            if(!requested){context->Assert(0,"the original blueprint backbuffer capture was requested");return SDGO_GAME_TEST_FAIL;}
            return SDGO_GAME_TEST_WAIT;
        }
        const auto captured=quietGraphics.CaptureFrameResult();
        if(captured==E_PENDING)return SDGO_GAME_TEST_WAIT;
        if(FAILED(captured)){context->Assert(0,"the original blueprint backbuffer was saved");return SDGO_GAME_TEST_FAIL;}
    }
    context->Assert(1,"the original blueprint UI reached the requested state without supplied selection data");return SDGO_GAME_TEST_PASS;
}
int __cdecl BlueprintBook(const SdgoGameTestContextV1* context){
    const auto result=blueprint.EnsureBook();
    if(result>=BlueprintActions::Invalid && captureNativeFrames) {
        static bool requested=false;
        if(!requested) {
            const auto folder=sdgo_test::directory+L"\\visual";CreateDirectoryW(folder.c_str(),nullptr);
            requested=quietGraphics.CaptureNextFrame((folder+L"\\blueprint-entry-diagnostic.bmp").c_str());
            if(requested)return SDGO_GAME_TEST_WAIT;
        } else if(quietGraphics.CaptureFrameResult()==E_PENDING)return SDGO_GAME_TEST_WAIT;
    }
    return BlueprintResult(context,result);
}
int __cdecl BlueprintRecipe(const SdgoGameTestContextV1* context){return BlueprintResult(context,blueprint.SelectBook());}
int __cdecl BlueprintMaterials(const SdgoGameTestContextV1* context){
    const auto result=blueprint.SelectMaterials();const auto s=blueprint.State();
    static uint32_t calls=~0u;
    if(calls!=s.material_calls) {
        char message[256];sprintf_s(message,"original material selection index=%u calls=%u slot=%u wanted=%llu actual=%llu phase=%u",
            s.material_index,s.material_calls,s.material_slot,static_cast<unsigned long long>(s.material_expected_uid),
            static_cast<unsigned long long>(s.material_actual_uid),s.phase);
        context->Observe(message);calls=s.material_calls;
    }
    return BlueprintResult(context,result);
}
int __cdecl BlueprintSynthesis(const SdgoGameTestContextV1* context){
    const auto result=blueprint.Synthesize();const auto s=blueprint.State();
    static uint32_t phase=0;
    if(phase!=s.phase || result>=BlueprintActions::Invalid) {
        char message[256];sprintf_s(message,"original synthesis phase=%u confirms=%u request=%u/%u reply=%u output=%u uid=%llu slot=%u",
            s.phase,s.confirms,s.request_sequence,s.request_size,s.reply_sequence,s.output_unit,
            static_cast<unsigned long long>(s.output_uid),s.output_slot);
        context->Observe(message);phase=s.phase;
    }
    return BlueprintResult(context,result);
}

int PresentationResult(const SdgoGameTestContextV1* context,UnitPresentationsActions::Result result) {
    const auto& s=unitPresentations.State();
    static uint32_t previous=0,phase=0,flags=0,first=0,detailFlags=0;static uintptr_t model=0,detail=0;
    const uint32_t currentDetailFlags=s.detail_active|(s.detail_visible<<8)|(s.projection_mode<<16)|(s.pending_actions<<24);
    if(previous!=result || model!=s.model || phase!=s.phase || flags!=s.list_flags || first!=s.first_visible_id || detail!=s.detail || detailFlags!=currentDetailFlags) {
        char message[448];sprintf_s(message,"original unit presentations result=%u unit=%u index=%u slot=%u form=%u animation=%.2f list=%08X detail=%08X model=%08X select=%u switch=%u phase=%u rank=%u flags=%04X shown=%u first=%u detail_unit=%u active=%u visible=%u projection=%u pending=%u",
            result,s.unit,s.index_id,s.slot,s.form,s.animation_state,static_cast<uint32_t>(s.list),
            static_cast<uint32_t>(s.detail),static_cast<uint32_t>(s.model),s.selection_calls,s.form_calls,
            s.phase,s.rank,s.list_flags,s.visible_count,s.first_visible_id,s.detail_unit,s.detail_active,s.detail_visible,s.projection_mode,s.pending_actions);
        context->Observe(message);previous=result;model=s.model;phase=s.phase;flags=s.list_flags;first=s.first_visible_id;detail=s.detail;detailFlags=currentDetailFlags;
    }
    if(result>=UnitPresentationsActions::Invalid) {
        context->Assert(0,"the original encyclopedia service, control, requested unit and model identities remain valid");
        return SDGO_GAME_TEST_FAIL;
    }
    if(result!=UnitPresentationsActions::Ready)return SDGO_GAME_TEST_WAIT;
    context->Assert(1,"the original unit UI reached the requested state through its own callbacks");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl ImportedIndex(const SdgoGameTestContextV1* context) {
    static bool diagnosticRequested=false,diagnosticComplete=false;
    if(diagnosticRequested && !diagnosticComplete) {
        const auto captured=quietGraphics.CaptureFrameResult();
        if(captured==E_PENDING)return SDGO_GAME_TEST_WAIT;
        diagnosticComplete=true;
        context->Observe(SUCCEEDED(captured)?"native encyclopedia diagnostic backbuffer saved (not a passing case)":"native encyclopedia diagnostic frame was unavailable");
    }
    ActiveSelectionObserver::Snapshot s;
    if(!activeSelection.Read(authenticatedPilot,&s) || !s.inventory_matches || s.uid!=playUnitUid) {
        context->Assert(0,"encyclopedia checks use the genuinely equipped owned unit");return SDGO_GAME_TEST_FAIL;
    }
    const auto result=unitPresentations.EnsureList(s.unit);
    if(captureNativeFrames && !diagnosticRequested && result==UnitPresentationsActions::Waiting &&
            unitPresentations.State().phase==5 && context->elapsed_ms>=3000) {
        const auto folder=sdgo_test::directory+L"\\visual";
        if(CreateDirectoryW(folder.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS)
            diagnosticRequested=quietGraphics.CaptureNextFrame((folder+L"\\diagnostic-unit-index.bmp").c_str());
    }
    return PresentationResult(context,result);
}
int __cdecl ImportedDetail(const SdgoGameTestContextV1* context) {
    return PresentationResult(context,unitPresentations.OpenDetail());
}
bool ImportedTransformTarget(uint32_t& target) {
    using Grid=void*(__cdecl*)();using Find=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    const auto grid=reinterpret_cast<Grid>(sdgo_test::api->Symbol(sdgo_test::mod,"grid.data"));
    const auto find=reinterpret_cast<Find>(sdgo_test::api->Symbol(sdgo_test::mod,"grid.find"));
    const uint8_t* row=nullptr;
    if(!grid || !find || !find(grid(),&row,unitPresentations.State().unit,2,0) || !row)return false;
    memcpy(&target,row+316,4);return true;
}
int ImportedForm(const SdgoGameTestContextV1* context,uint32_t form) {
    uint32_t target=0;
    if(!ImportedTransformTarget(target)) {context->Assert(0,"native imported unit has a readable transform reference");return SDGO_GAME_TEST_FAIL;}
    if(!target) {context->Assert(1,"single-form native unit declares no transformation; no form action is submitted");return SDGO_GAME_TEST_PASS;}
    return PresentationResult(context,unitPresentations.SwitchForm(form));
}
int __cdecl ImportedAlternate(const SdgoGameTestContextV1* context){return ImportedForm(context,1);}
int __cdecl ImportedBase(const SdgoGameTestContextV1* context){return ImportedForm(context,0);}
int __cdecl ImportedClose(const SdgoGameTestContextV1* context){return PresentationResult(context,unitPresentations.Close());}
int __cdecl ImportedAcquisition(const SdgoGameTestContextV1* context) {
    if(unitPresentations.State().unit!=unitAcquisitionConfig.unit_id) {
        context->Assert(0,"the sealed acquisition target matches the genuinely owned unit already inspected");
        return SDGO_GAME_TEST_FAIL;
    }
    const auto result=unitAcquisition.Poll();const auto s=unitAcquisition.State();
    static uint32_t previous=UINT32_MAX,sequence=UINT32_MAX;
    if(previous!=result || sequence!=s.unitview_event_sequence) {
        char message[512];sprintf_s(message,"native acquisition result=%u kind=%u machine=%u unit=%u machine_page=%u slot=%u candidate_page=%u slot=%u control=%08X view=%08X event=%u/%u model=%08X dimensions=%d/%d",
            static_cast<unsigned>(result),s.machine_kind,s.machine_id,s.unit_id,s.machine_page,s.machine_slot,
            s.candidate_page,s.candidate_slot,s.candidate_control_pointer,s.unitview_pointer,
            s.unitview_event_sequence,s.unitview_event_unit,s.unitview_model_pointer,s.render_width,s.render_height);
        context->Observe(message);previous=result;sequence=s.unitview_event_sequence;
    }
    if(result==UnitAcquisitionActions::kUnbound || result>=UnitAcquisitionActions::kInvalidConfig) {
        context->Assert(0,"the native capsule machine, candidate control and unitview consumer identities remain valid");
        return SDGO_GAME_TEST_FAIL;
    }
    if(result!=UnitAcquisitionActions::kReady)return SDGO_GAME_TEST_WAIT;
    context->Assert(s.unitview_event_consumed && s.unitview_event_unit==unitAcquisitionConfig.unit_id &&
        s.unitview_model_ready && s.unitview_dialog_visible && s.unitview_render_visible,
        "the original candidate callback reached its actual unitview consumer and visible native model without purchasing");
    return SDGO_GAME_TEST_PASS;
}
const SdgoGameTestCaseV1 unitAcquisitionCase{"pages.unit_acquisition",180000,&ImportedAcquisition};
const SdgoGameTestCaseV1 unitPresentationCases[]={
    {"pages.unit_index.list",120000,&ImportedIndex},
    {"pages.unit_index.detail",120000,&ImportedDetail},
    {"pages.unit_index.alternate",60000,&ImportedAlternate},
    {"pages.unit_index.base",60000,&ImportedBase},
    {"pages.unit_index.close",60000,&ImportedClose},
};
int UnitPortrait(const SdgoGameTestContextV1* context,bool choice) {
    static std::string current;static bool issued=false;static uint64_t issuedAt=0;
    if(current!=context->case_id){current=context->case_id;issued=false;issuedAt=0;}
    if(!issued) {
        const auto result=unitPresentations.ReadyPose(choice);
        if(result>=UnitPresentationsActions::Invalid)return PresentationResult(context,result);
        if(result!=UnitPresentationsActions::Ready)return SDGO_GAME_TEST_WAIT;
        issued=true;issuedAt=context->elapsed_ms;return SDGO_GAME_TEST_WAIT;
    }
    if(context->elapsed_ms-issuedAt<150)return SDGO_GAME_TEST_WAIT;
    const auto folder=sdgo_test::directory+L"\\visual";
    if(!CreateDirectoryW(folder.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
        context->Assert(0,"native portrait output directory is available");return SDGO_GAME_TEST_FAIL;
    }
    const std::wstring name(current.begin(),current.end());
    const bool saved=unitPresentations.WritePortraitSource((folder+L"\\"+name+L".json").c_str(),choice?"choice_01":"wait_01");
    context->Assert(saved,"original detail entity plays its actual ready/selection animation and exposes its real pixel viewport");
    return saved?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
int __cdecl PortraitWait(const SdgoGameTestContextV1* context){return UnitPortrait(context,false);}
int __cdecl PortraitReady(const SdgoGameTestContextV1* context){return UnitPortrait(context,true);}
const SdgoGameTestCaseV1 portraitCases[]={
    {"pages.unit_index.pose_wait",60000,&PortraitWait},{"pages.unit_index.pose_ready",60000,&PortraitReady},
};
int MissionProgress(const SdgoGameTestContextV1* context,MissionFlowActions::Result result) {
    static uint32_t previous=UINT32_MAX;
    if(previous!=result) {
        const auto s=mission.PollSnapshot();char message[384];
        sprintf_s(message,"native mission result=%s app=%u channel=%u/%u mission=%u/%u mode=%u room=%u/%u create=%04X consumed=%u start=%04X queued=%u consumed=%u control=%u/%u main=%08X/%u command=%08X ctx=%u reply=%04X/%04X",
            MissionFlowActions::ResultName(result),s.native_app_state,s.selected_channel_group,s.selected_channel,
            s.requested_mission_id,s.selected_mission_id,
            s.native_room_mode,s.requested_room_id,s.native_room_id,s.create_opcode,s.create_reply_consumed,s.start_opcode,s.start_queued,s.battle_start_consumed,s.create_control_kind,s.create_control_flags,s.native_main_vtable,s.native_main_state,s.native_command_vtable,s.native_context_flags,s.last_start_reply_opcode,s.last_start_reply_status);
        context->Observe(message);previous=result;
        char channel[384];sprintf_s(channel,"mission channel scan=%u ready=%u valid=%u polls=%u callbacks=%u pageMode=%u request=%u groups=%u raw=%u mode2=%u eligible=%u rejected=%u/%u/%u/%u/%u",
            s.channel_scan_status,s.channel_list_ready,s.channel_scan_valid,s.channel_wait_polls,s.channel_choice_callbacks,
            s.channel_page_mode,s.channel_request_state,s.advertised_group_count,s.advertised_raw_count,s.advertised_mode2_count,s.advertised_eligible_count,
            s.rejected_zero_id_count,s.rejected_non_mode2_count,s.rejected_disabled_count,s.rejected_zero_capacity_count,s.rejected_full_count);context->Observe(channel);
    }
    if(result>=MissionFlowActions::kInvalidMode || result==MissionFlowActions::kUnbound){context->Assert(0,"native mission admission preserves actual channel, descriptor, request and reply ownership");return SDGO_GAME_TEST_FAIL;}
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl CreateMission(const SdgoGameTestContextV1* context) {
    const auto result=mission.EnsureRoom(
        missionMode,
        roomTitle,
        true,
        missionGameModeId);
    const auto pending=MissionProgress(context,result);
    if(pending==SDGO_GAME_TEST_FAIL || result!=MissionFlowActions::kRoomReady)return pending;
    const auto s=mission.PollSnapshot();
    context->Assert(s.native_room_id && s.native_room_id==s.requested_room_id && s.native_room_mode==missionMode && s.local_is_host &&
        s.create_reply_consumed && s.create_opcode==0x0640,"a genuine server-created mission room and original reply consumer retain the selected listed mission and real host");
    if(missionGameModeId)
        context->Assert(
            s.requested_mission_id==missionGameModeId &&
            s.selected_mission_id==missionGameModeId,
            "the real mission create dialog and accepted room retain the sealed exact GameModeID");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl StartMission(const SdgoGameTestContextV1* context) {
    const auto result=mission.Start();const auto pending=MissionProgress(context,result);
    if(pending==SDGO_GAME_TEST_FAIL || result!=MissionFlowActions::kBattleStartConsumed)return pending;
    const auto s=mission.PollSnapshot();
    context->Assert(s.start_request_sent && s.start_opcode==0x08A7 && s.battle_start_consumed,
        "the original mission start command sent 08A7 and consumed its own genuine 06C3 before stage-three entry");
    observedMissionEndBaseline=rooms.BattleEndState().sequence;
    observedMissionStartedAt=GetTickCount64();
    SdgoBattleAiMissionBossSpecialV1 boss{};
    boss.size=sizeof(boss);boss.version=1;
    observedMissionBossSpecialBaseline=
        getMissionBossSpecial && getMissionBossSpecial(&boss)
            ? boss.sequence : 0;
    return SDGO_GAME_TEST_PASS;
}
bool MissionSnapshot(SdgoMissionAiClientSnapshotV1& s) {
    s={};s.size=sizeof(s);s.version=SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;return getMissionSnapshot && getMissionSnapshot(&s)!=0;
}
int __cdecl SpawnMissionAi(const SdgoGameTestContextV1* context) {
    SdgoMissionAiClientSnapshotV1 s{};if(!MissionSnapshot(s) || !s.active)return SDGO_GAME_TEST_WAIT;
    if(s.room_mode!=missionMode || !s.exact_plain_mission || !s.room_records_valid || s.lua_pcall_error_count || s.native_exception_count) {
        context->Assert(0,"the exact current native mission and whole-generation Lua/native observer remain valid");return SDGO_GAME_TEST_FAIL;
    }
    bool ready=s.ai_record_count==expectedAi && s.spawned_wingman_count==expectedAi;
    for(uint32_t i=0;i<s.ai_record_count && i<12;++i)ready=ready && s.participants[i].mapping_valid && s.participants[i].valid_registered_object && s.participants[i].ai_enabled;
    if(!ready)return SDGO_GAME_TEST_WAIT;
    context->Assert(s.battle_generation && s.local_is_host && !s.local_is_observer && s.local_wingman_count==expectedAi && s.human_participant_count==1 &&
        s.unit_manager_player_count==1,"the actual co-op room spawned every server wingman while preserving the original human-count contract");
    context->Assert(ready,"each real mission AI maps its pilot/NPC to its uniquely registered native object");
    observedMissionGeneration=s.battle_generation;observedMissionRoom=s.room_object;
    return SDGO_GAME_TEST_PASS;
}
int __cdecl ObserveMissionCombat(const SdgoGameTestContextV1* context) {
    SdgoMissionAiClientSnapshotV1 s{};
    const bool missionSnapshotAvailable=MissionSnapshot(s);
    const auto player=actions.OnboardingState();
    const auto ended=rooms.BattleEndState();
    const bool returnedToWaitRoom=player.native_app_state==18;
    const bool nativeResultObserved=
        ended.sequence>observedMissionEndBaseline;
    if(returnedToWaitRoom || nativeResultObserved) {
        nativePlayerCombat.Finish(context,scoreboardInput);
        char terminal[448]{};
        sprintf_s(terminal,
            "native mission terminated before required combat gate elapsed=%llu app=%u pilot=%u snapshot=%u active=%u generation=%u/%u room=%u/%u records=%u spawned=%u lua=%u/%u exception=%u end_sequence=%u/%u end_opcode=%04X result=%u handler=%u",
            static_cast<unsigned long long>(context->elapsed_ms),
            player.native_app_state,player.profile_pilot_id,
            missionSnapshotAvailable?1u:0u,s.active,
            s.battle_generation,observedMissionGeneration,
            s.room_object,observedMissionRoom,
            s.ai_record_count,s.spawned_wingman_count,
            s.lua_tick_count,s.lua_successful_tick_count,
            s.native_exception_count,
            ended.sequence,observedMissionEndBaseline,
            ended.reply_opcode,ended.result_type,
            ended.original_handler_completed);
        context->Observe(terminal);
        context->Assert(0,
            "the genuine mission must remain in its released battle state until the required mapped-input and AI observation gate completes");
        return SDGO_GAME_TEST_FAIL;
    }
    if(!missionSnapshotAvailable || !s.active)return SDGO_GAME_TEST_WAIT;
    if(s.room_mode!=missionMode || s.lua_pcall_error_count || s.native_exception_count){context->Assert(0,"whole native mission smoke observation remains error-free and same-mode");return SDGO_GAME_TEST_FAIL;}
    if(!nativePlayerCombat.PollMission(
            context,s,observedMissionGeneration,
            observedMissionRoom,player.native_app_state,
            player.profile_pilot_id,scoreboardInput))
        return SDGO_GAME_TEST_FAIL;
    unsigned moved=0,targets=0;
    for(uint32_t i=0;i<s.ai_record_count && i<12;++i) {
        const auto& p=s.participants[i];if(!p.mapping_valid || !p.valid_registered_object)continue;
        if(p.movement_observed)++moved;if(p.target_valid && p.target_hp>0)++targets;
    }
    if(context->elapsed_ms<15000 || !moved || !targets)return SDGO_GAME_TEST_WAIT;
    char message[192];sprintf_s(message,"native mission bounded observation mode=%u generation=%u spawned=%u moved=%u targets=%u lua=%u tick=%u",
        s.room_mode,s.battle_generation,s.spawned_wingman_count,moved,targets,s.lua_pcall_error_count,s.lua_tick_count);context->Observe(message);
    nativePlayerCombat.Finish(context,scoreboardInput);
    context->Assert(moved && targets && s.lua_successful_tick_count>0,"real mission wingmen move and acquire living native targets across ordinary updates with zero cumulative Lua error");
    context->Observe(
        missionBehaviorProfile
            ? "bounded wingman smoke is complete; profile activation and one genuine Boss special event are checked next, while the full repeated pattern remains a manual acceptance"
            : "bounded native mission smoke ends here; objective clear, natural results, active exit and a remote peer are not claimed");
    return SDGO_GAME_TEST_PASS;
}
enum class MissionBehaviorReadiness {
    Waiting,
    Ready,
    Failed,
};
MissionBehaviorReadiness MissionBehaviorClientReady(
    const SdgoGameTestContextV1* context,
    SdgoMissionBehaviorClientStatusV1& status)
{
    status={};
    status.size=sizeof(status);
    status.version=1;
    if(!getMissionBehaviorStatus ||
       !getMissionBehaviorStatus(&status) ||
       !status.hook_installed ||
       !status.table_generation)
        return MissionBehaviorReadiness::Waiting;
    if(status.active_profile!=missionBehaviorProfile ||
       status.requested_profile!=missionBehaviorProfile ||
       status.conflict_records ||
       status.observed_records!=12) {
        char failure[224];
        sprintf_s(
            failure,
            "mission behavior client mismatch requested=%u active=%u valid=%u generation=%u observed=%u applied=%u restored=%u unchanged=%u conflicts=%u",
            status.requested_profile,status.active_profile,status.profile_data_valid,
            status.table_generation,status.observed_records,status.applied_records,
            status.restored_records,status.unchanged_records,status.conflict_records);
        context->Observe(failure);
        context->Assert(0,"the production mission behavior Mod must activate the sealed profile without native-record conflicts");
        return MissionBehaviorReadiness::Failed;
    }
    const uint32_t accepted=
        missionBehaviorProfile==1
            ? status.applied_records+status.unchanged_records
            : status.restored_records+status.unchanged_records;
    if(accepted<12)
        return MissionBehaviorReadiness::Waiting;
    if(missionBehaviorProfile==1 && !status.profile_data_valid) {
        context->Assert(0,"the OC profile requires the validated owner mission bundle");
        return MissionBehaviorReadiness::Failed;
    }
    char message[224];
    sprintf_s(
        message,
        "mission behavior client profile=%s generation=%u observed=%u applied=%u restored=%u unchanged=%u data=%u flags=%u",
        missionBehaviorProfile==1?"oc":"gco",status.table_generation,
        status.observed_records,status.applied_records,status.restored_records,
        status.unchanged_records,status.profile_data_valid,status.profile_data_flags);
    context->Observe(message);
    context->Assert(
        accepted>=12 && !status.conflict_records,
        "all twelve bounded M1 Boss bindings were accepted through the production client table loader");
    return MissionBehaviorReadiness::Ready;
}
int __cdecl ObserveMissionBehavior(const SdgoGameTestContextV1* context) {
    SdgoMissionBehaviorClientStatusV1 status{};
    const auto readiness=MissionBehaviorClientReady(context,status);
    if(readiness!=MissionBehaviorReadiness::Ready)
        return readiness==MissionBehaviorReadiness::Failed
            ? SDGO_GAME_TEST_FAIL : SDGO_GAME_TEST_WAIT;
    SdgoMissionAiClientSnapshotV1 missionSnapshot{};
    if(!MissionSnapshot(missionSnapshot) || !missionSnapshot.active)
        return SDGO_GAME_TEST_WAIT;
    const bool sameBattle=
        missionSnapshot.room_mode==missionMode &&
        missionSnapshot.battle_generation==observedMissionGeneration &&
        missionSnapshot.room_object==observedMissionRoom &&
        !missionSnapshot.lua_pcall_error_count &&
        !missionSnapshot.native_exception_count;
    if(!sameBattle) {
        context->Assert(
            0,
            "mission behavior observation remains attached to the same error-free native battle generation");
        return SDGO_GAME_TEST_FAIL;
    }
    if(missionGameModeId==88009) {
        context->Observe(
            "M2 client admission is complete; the authoritative server profile receipt is evaluated separately and is required before the overall run can pass");
        return SDGO_GAME_TEST_PASS;
    }
    if(missionBehaviorProfile==2) {
        context->Observe(
            "GCO restored all twelve current Boss bindings and intentionally retains the current task behavior");
        return SDGO_GAME_TEST_PASS;
    }
    SdgoBattleAiMissionBossSpecialV1 boss{};
    boss.size=sizeof(boss);boss.version=1;
    if(!getMissionBossSpecial || !getMissionBossSpecial(&boss) ||
       boss.sequence<=observedMissionBossSpecialBaseline)
        return SDGO_GAME_TEST_WAIT;
    const bool exactEasyBoss=
        boss.npc_data_id==85054 ||
        boss.npc_data_id==85079 ||
        boss.npc_data_id==85080;
    const bool threshold=
        boss.hp_before>0 && boss.max_hp_before>0 &&
        static_cast<int64_t>(boss.hp_before)*100<=
            static_cast<int64_t>(boss.max_hp_before)*70;
    char message[320];
    sprintf_s(
        message,
        "native M1 Boss special sequence=%llu epoch=%llu mission=%08X unit=%08X pilot=%u object=%u npc=%u weapon=%u hp=%d/%d->%d/%d app=%u state=%u/%u busy=%u/%u original=%d",
        static_cast<unsigned long long>(boss.sequence),
        static_cast<unsigned long long>(boss.battle_epoch),
        boss.mission_object,boss.unit,boss.pilot_id,boss.object_id,
        boss.npc_data_id,boss.weapon_id,boss.hp_before,boss.max_hp_before,
        boss.hp_after,boss.max_hp_after,boss.native_app_state,
        boss.attack_state_before,boss.attack_state_after,
        boss.special_busy_before,boss.special_busy_after,
        boss.original_result);
    context->Observe(message);
    const bool valid=
        boss.battle_epoch &&
        boss.event_tick_ms>=observedMissionStartedAt &&
        boss.mission_object && boss.unit && boss.pilot_id &&
        boss.object_id && exactEasyBoss && boss.weapon_id &&
        boss.native_app_state==22 && threshold &&
        boss.hp_after>0 && boss.max_hp_after==boss.max_hp_before &&
        boss.state_before_valid && boss.state_after_valid &&
        boss.original_returned;
    context->Assert(
        valid,
        "an exact OC M1 Easy Boss at or below 70-percent HP issued a genuine special-start notice consumed by the original client in this battle");
    return valid?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
const SdgoGameTestCaseV1 missionCases[]={
    {"mission.room",180000,&CreateMission},{"mission.start",180000,&StartMission},
    {"mission.ai_spawn",120000,&SpawnMissionAi},{"mission.combat",120000,&ObserveMissionCombat},
};
const SdgoGameTestCaseV1 missionBehaviorCases[]={
    {"mission.behavior",120000,&ObserveMissionBehavior},
};
// Read each atomic probe receipt without the profile API's persistent file
// cache, which can prevent the server replacing its preceding receipt.
struct MissionRespawnReceipt {
    std::wstring text;
    bool Load(const std::wstring& path) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return false;
        LARGE_INTEGER size{};DWORD read=0;
        bool ok=GetFileSizeEx(file,&size) && size.QuadPart>=2 && size.QuadPart<=16384 && size.QuadPart%2==0;
        if(ok) {
            text.resize(static_cast<size_t>(size.QuadPart/2));
            ok=ReadFile(file,text.data(),static_cast<DWORD>(size.QuadPart),&read,nullptr) &&
                read==size.QuadPart && text[0]==0xFEFF;
        }
        CloseHandle(file);return ok;
    }
    std::wstring Value(const wchar_t* key) const {
        const std::wstring prefix=L"\n"+std::wstring(key)+L"=";
        const auto found=text.find(prefix);
        if(found==std::wstring::npos)return {};
        const auto begin=found+prefix.size(),end=text.find_first_of(L"\r\n",begin);
        return text.substr(begin,end==std::wstring::npos?end:end-begin);
    }
    uint32_t Number(const wchar_t* key) const {
        const auto value=Value(key);wchar_t* end=nullptr;
        const auto number=wcstoul(value.c_str(),&end,10);
        return !value.empty() && end && !*end ? static_cast<uint32_t>(number):0;
    }
};
int __cdecl ObserveMissionRespawns(const SdgoGameTestContextV1* context) {
    static bool basicConfirmed=false;
    SdgoMissionAiClientSnapshotV1 s{};
    if(!basicConfirmed) {
        if(!MissionSnapshot(s) || !s.active) {
            if(rooms.BattleEndState().sequence>observedMissionEndBaseline) {
                const auto ended=rooms.BattleEndState();char early[128];sprintf_s(early,"native early mission end opcode=%04X result=%u app=%u",ended.reply_opcode,ended.result_type,ended.native_app_state);context->Observe(early);
                context->Assert(0,"mission ended before native revival verification");return SDGO_GAME_TEST_FAIL;
            }
            return SDGO_GAME_TEST_WAIT;
        }
        if(s.lua_pcall_error_count || s.native_exception_count || s.room_mode!=missionMode ||
           s.battle_generation!=observedMissionGeneration || s.room_object!=observedMissionRoom) {
            context->Assert(0,"native mission remains valid throughout repeated wingman death and respawn");return SDGO_GAME_TEST_FAIL;
        }
    }
    const std::wstring expectedRun(context->run_id,context->run_id+strlen(context->run_id));
    const std::wstring receipt=std::wstring(context->directory)+L"\\mission-respawn-server-"+expectedRun+L".ini";
    MissionRespawnReceipt values;
    if(!values.Load(receipt))return SDGO_GAME_TEST_WAIT;
    wchar_t status[16]{},run[64]{};
    wcsncpy_s(status,values.Value(L"status").c_str(),_TRUNCATE);
    wcsncpy_s(run,values.Value(L"run_id").c_str(),_TRUNCATE);
    if(!*status)return SDGO_GAME_TEST_WAIT;
    if(expectedRun!=run || (wcscmp(status,L"cycle")!=0 && wcscmp(status,L"basic")!=0 && wcscmp(status,L"pass")!=0)) {
        context->Assert(0,"same-run native mission respawn server probe completed successfully");return SDGO_GAME_TEST_FAIL;
    }
    const auto pilot=values.Number(L"pilot");
    const auto limit=values.Number(L"life_limit");
    const auto deaths=values.Number(L"deaths");
    const auto required=values.Number(L"client_cycles_required");
    static uint64_t lastNativeDiagnostic=0;
    if(!basicConfirmed && context->elapsed_ms-lastNativeDiagnostic>=3000) {
        for(uint32_t i=0;i<s.ai_record_count && i<12;++i)if(s.participants[i].pilot_id==pilot) {
            const auto& p=s.participants[i];char diagnostic[224];sprintf_s(diagnostic,
                "native revival readback pilot=%u object=%u hp=%d observed=%u required=%u mapping=%u registered=%u AI=%u position=%u",
                pilot,p.object_id,p.current_hp,p.respawn_count,required,p.mapping_valid,p.valid_registered_object,p.ai_enabled,p.position_valid);context->Observe(diagnostic);
        }
        lastNativeDiagnostic=context->elapsed_ms;
    }
    if(wcscmp(status,L"pass")==0) {
        if(!basicConfirmed) {context->Assert(0,"client verifies actual revivals before the human exhaustion fixture");return SDGO_GAME_TEST_FAIL;}
        const auto humanDeaths=values.Number(L"human_deaths");
        const auto aiAlive=values.Number(L"ai_alive_before_failure");
        const auto nativeFailure=values.Number(L"native_human_failure");
        if(!nativeFailure || humanDeaths<limit || !aiAlive) {
            context->Assert(0,"exhausting the last human life immediately fails the native mission despite living AI");return SDGO_GAME_TEST_FAIL;
        }
        const auto ended=rooms.BattleEndState();
        if(ended.sequence<=observedMissionEndBaseline || !ended.original_handler_completed)return SDGO_GAME_TEST_WAIT;
        context->Assert(ended.reply_opcode==0x0871,"the original client consumes the genuine mission failure end reply");
        context->Assert(nativeFailure && humanDeaths>=limit && aiAlive,
            "native human-only life exhaustion triggers the existing failure gate while AI is still alive");
        if(missionRespawns==2) {
            const auto serverRespawns=values.Number(L"respawns");
            context->Assert(deaths>limit && serverRespawns>limit,
                "separate server-native infinite boundary stress exceeds the player quota after a genuine client revival");
        }
        char message[192];sprintf_s(message,"native human exhaustion deaths=%u limit=%u living_AI=%u end=%04X/%u",humanDeaths,limit,aiAlive,ended.reply_opcode,ended.result_type);context->Observe(message);
        return SDGO_GAME_TEST_PASS;
    }
    if(basicConfirmed)return SDGO_GAME_TEST_WAIT;
    if(wcscmp(status,L"cycle")!=0 || required!=1) {
        context->Assert(0,"a genuine client-originated revival is verified before server-only quota stress");return SDGO_GAME_TEST_FAIL;
    }
    for(uint32_t i=0;i<s.ai_record_count && i<12;++i) {
        const auto& p=s.participants[i];if(p.pilot_id!=pilot)continue;
        if(p.respawn_count<required)return SDGO_GAME_TEST_WAIT;
        if(!p.mapping_valid || !p.valid_registered_object ||
           p.current_hp<=0 || !p.ai_enabled || !p.position_valid)return SDGO_GAME_TEST_WAIT;
        context->Assert(p.mapping_valid && p.valid_registered_object && p.respawn_count>=required,
            "the original client retains its registered wingman through a genuine combat death and revival");
        context->Assert(p.ai_enabled && p.position_valid,
            "the revived wingman has active native AI and a finite scene position");
        char message[192];sprintf_s(message,"native mission respawn policy=%u pilot=%u limit=%u deaths=%u client_respawns=%u hp=%d",missionRespawns,pilot,limit,deaths,p.respawn_count,p.current_hp);context->Observe(message);
        const std::wstring ack=std::wstring(context->directory)+L"\\mission-respawn-client-ack-"+expectedRun+L".ini";
        if(!WritePrivateProfileStringW(L"ack",L"run_id",expectedRun.c_str(),ack.c_str())) {
            context->Assert(0,"native revival observations are acknowledged before testing human exhaustion");return SDGO_GAME_TEST_FAIL;
        }
        basicConfirmed=true;
        return SDGO_GAME_TEST_WAIT;
    }
    context->Assert(0,"same-run respawn probe identifies a real wingman of this exact mission");return SDGO_GAME_TEST_FAIL;
}
const SdgoGameTestCaseV1 missionRespawnCase={"mission.respawn",360000,&ObserveMissionRespawns};
const SdgoGameTestCaseV1 collectionCases[]={
    {"pages.collection.preview",120000,&PreviewCollection},
    {"pages.collection.draw",120000,&DrawCollection},
    {"pages.collection.delivery",180000,&DeliverCollection},
};
int TitleCreateVs(const SdgoGameTestContextV1* c,uint32_t type,RoomTitleActions::Surface surface) {
    if(!roomTitles.Running() && actions.AppState()==5){
        static bool requested=false;
        if(!requested){
            const auto result=hangarNavigation.ClickBottomRadio(L"GameRoomRadio",0x48);
            if(result>=HangarNavigation::kDownmenuServiceMismatch || result==HangarNavigation::kUnbound){c->Assert(0,"native GameRoomRadio must return from the owned-unit hangar before title tests");return SDGO_GAME_TEST_FAIL;}
            if(result==HangarNavigation::kReady){requested=true;c->Observe("native GameRoomRadio requested after fixture unit selection");}
        }
        return SDGO_GAME_TEST_WAIT;
    }
    if(!roomTitles.Running() && actions.AppState()==6){
        const auto channel=channels.EnsureVsType(type);
        if(channel==ChannelActions::kNativeRequestPending)return SDGO_GAME_TEST_WAIT;
        if(channel>=ChannelActions::kInvalidTarget){
            const auto state=channels.State();
            char line[160];sprintf_s(line,"native PvP channel result=%u selected=%u/%u target=%u/%u",channel,
                state.selected_group,state.selected_channel,state.target_group,state.target_channel);
            c->Observe(line);c->Assert(0,"native PvP channel selection failed");return SDGO_GAME_TEST_FAIL;
        }
        if(channel!=ChannelActions::kAccepted && channel!=ChannelActions::kAlreadySelected)return SDGO_GAME_TEST_WAIT;
        if(channels.TakeSceneReloadRequirement())hangarNavigation.RequireGameRoomReentry(rooms.NativeRoomPageGeneration());
        const auto result=hangarNavigation.EnsureGameRoom(roomListVtable,modalLoadingVtable,rooms.NativeRoomPageGeneration());
        if(result!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    }
    return roomTitles.Poll(surface,c);
}
int __cdecl TitlePvpCreate(const SdgoGameTestContextV1* c){return TitleCreateVs(c,1,RoomTitleActions::PvpCreate);}
int __cdecl TitleTeamCreate(const SdgoGameTestContextV1* c){return TitleCreateVs(c,1,RoomTitleActions::PvpTeamCreate);}
int __cdecl TitleGridCreate(const SdgoGameTestContextV1* c){return TitleCreateVs(c,1,RoomTitleActions::PvpGridCreate);}
int __cdecl TitlePvpRename(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::PvpRename,c);}
int __cdecl TitleMissionCreate(const SdgoGameTestContextV1* c){
    if(!roomTitles.Running()){
        const auto result=mission.EnsureRoom(5,"GTRoomTitleInput",false);
        if(result>=MissionFlowActions::kInvalidMode || result==MissionFlowActions::kUnbound){
            c->Observe(MissionFlowActions::ResultName(result));c->Assert(0,"native mission channel admission failed");return SDGO_GAME_TEST_FAIL;
        }
        if(result!=MissionFlowActions::kCreateDialogReady)return SDGO_GAME_TEST_WAIT;
    }
    return roomTitles.Poll(RoomTitleActions::MissionCreate,c);
}
int __cdecl PreviewMissionCreate(const SdgoGameTestContextV1* c) {
    // A standalone preview starts directly after owned-unit selection, which
    // leaves the real client in its hangar rather than the room-list scene.
    if (!roomTitles.Running() && actions.AppState()==5) {
        static bool requested=false;
        if (!requested) {
            const auto result=hangarNavigation.ClickBottomRadio(L"GameRoomRadio",0x48);
            if (result>=HangarNavigation::kDownmenuServiceMismatch || result==HangarNavigation::kUnbound) {
                c->Assert(0,"native GameRoomRadio must return from the owned-unit hangar before the mission preview");
                return SDGO_GAME_TEST_FAIL;
            }
            if (result==HangarNavigation::kReady) requested=true;
        }
        return SDGO_GAME_TEST_WAIT;
    }
    return TitleMissionCreate(c);
}
int __cdecl TitleTeamRename(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::TeamRename,c);}
int __cdecl TitleTeamSettings(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::TeamSettings,c);}
int __cdecl TitleMissionSettings(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::MissionSettings,c);}
int __cdecl TitleMissionRename(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::MissionRename,c);}
int __cdecl RoomStylePreview(const SdgoGameTestContextV1* c) {
    return roomStylePreviewActions.Poll(c,roomStylePreview==2,actions,quietGraphics,sdgo_test::directory);
}
int __cdecl TitleLeave(const SdgoGameTestContextV1* c){
    if(actions.AppState()==6){
        std::array<char,513> title{};
        const auto size=sdgo::CurrentRoomTitle(c->api->ModDirectory(sdgo_test::mod),title.data(),static_cast<uint32_t>(title.size()));
        if(size && c->elapsed_ms<5000)return SDGO_GAME_TEST_WAIT;
        c->Assert(1,"the original room-exit flow returned the client to its native lobby state");
        c->Assert(size==0,"the previous room's confirmed full title is no longer active after leaving");
        roomTitles.Reset();channels.Reset();return size?SDGO_GAME_TEST_FAIL:SDGO_GAME_TEST_PASS;
    }
    static uint64_t last=0;if(GetTickCount64()-last>2000){last=GetTickCount64();if(!rooms.Leave())roomTitles.Leave(c);}
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl TitleTeamNativeView(const SdgoGameTestContextV1* c) {
    if(actions.AppState()!=18 || !rooms.IsModernTeamRoomReady())return SDGO_GAME_TEST_WAIT;
    void* bar=roomTitles.Service("room_titles.ui_titlebar_vtable");
    const auto* room=static_cast<const uint8_t*>(roomTitles.Root(bar,0x10C));
    if(!bar || !room)return SDGO_GAME_TEST_WAIT;
    char details[128];sprintf_s(details,"native Team model id=%u field4=%u host_state=%u",
        *reinterpret_cast<const uint32_t*>(room),room[4],
        *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(bar)+0x100));
    if (!roomTitles.Running()) {
        c->Observe(details);
        c->Assert(1,"the created team uses its original modern3D Team manager and ready Main state");
        c->Assert(roomTitles.Service("title_test.team_titlebar_vtable")==nullptr,
                  "the legacy2D team settings header is absent from this supported modern room scene");
    }
    if (room[4]==4) {
        c->Assert(1,"the native category-4 handler disables the inline rename entry");
        return SDGO_GAME_TEST_PASS;
    }
    if (*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(bar)+0x100)!=2)
        return SDGO_GAME_TEST_WAIT;
    // Factory type 1 identifies the Team manager, while model byte +4 comes
    // from the server's room payload. Exercise the real inline editor when
    // that native category permits it instead of assuming all Teams forbid it.
    return roomTitles.Poll(RoomTitleActions::PvpRename,c);
}
int __cdecl TitleStartSmoke(const SdgoGameTestContextV1* c) {
    static uint32_t startBaseline=0,sendBaseline=0,replyBaseline=0;
    static uint64_t lastAttempt=0;
    static bool requested=false;
    static std::string confirmedTitle;
    if (!c->tick) {
        std::array<char,513> title{};
        const auto size=sdgo::CurrentRoomTitle(c->api->ModDirectory(sdgo_test::mod),title.data(),static_cast<uint32_t>(title.size()));
        c->Assert(size>20,"battle-start smoke begins with the server-confirmed extended native input");
        if (size<=20) return SDGO_GAME_TEST_FAIL;
        confirmedTitle.assign(title.data(),size);
        startBaseline=mission.NativeStartConsumedSequence();
        sendBaseline=actions.GameStartSendCount();replyBaseline=rooms.NativeStartReplySequence();
        requested=false;lastAttempt=0;
    }
    if (rooms.NativeStartReplySequence()>replyBaseline && rooms.NativeStartReplyError()!=0) {
        char failure[96];sprintf_s(failure,"native long-title battle start rejected: 07B5/%04X",rooms.NativeStartReplyError());
        c->Assert(0,failure);return SDGO_GAME_TEST_FAIL;
    }
    if ((!requested || (actions.GameStartSendCount()==sendBaseline && c->elapsed_ms-lastAttempt>=1000 &&
                       rooms.NativeStartQueueState()==0 && rooms.NativeStartPrerequisite()==0)) && rooms.StartBattleNativeHost()) {
        requested=true;lastAttempt=c->elapsed_ms;
        c->Observe("original room-host Start business requested for the confirmed long-title AI room");
    }
    if (actions.AppState()!=22 || mission.NativeStartConsumedSequence()<=startBaseline ||
        actions.GameStartSendCount()<=sendBaseline)
        return SDGO_GAME_TEST_WAIT;
    std::array<char,513> current{};
    const auto size=sdgo::CurrentRoomTitle(c->api->ModDirectory(sdgo_test::mod),current.data(),static_cast<uint32_t>(current.size()));
    c->Assert(actions.AppState()==22 && mission.NativeStartConsumedSequence()>startBaseline &&
                  actions.GameStartSendCount()>sendBaseline,
              "a real server battle-start packet admitted the long-title room into native GAME_MAIN");
    c->Assert(std::string(current.data(),size)==confirmedTitle,"the confirmed full title survives the genuine battle-start transition");
    c->Observe("bounded battle-start smoke only; combat, results and natural battle return are not claimed");
    return SDGO_GAME_TEST_PASS;
}
const SdgoGameTestCaseV1 titleCases[]={
    {"room_titles.pvp.create",120000,&TitlePvpCreate},
    {"room_titles.pvp.rename",120000,&TitlePvpRename},
    {"room_titles.pvp.leave",60000,&TitleLeave},
    {"room_titles.grid.create",120000,&TitleGridCreate},
    {"room_titles.grid.rename",120000,&TitlePvpRename},
    {"room_titles.grid.leave",60000,&TitleLeave},
    {"room_titles.mission.create",180000,&TitleMissionCreate},
    {"room_titles.mission.rename",120000,&TitleMissionRename},
    {"room_titles.mission.leave",60000,&TitleLeave},
    {"room_titles.team.create",120000,&TitleTeamCreate},
    {"room_titles.team.native_view",60000,&TitleTeamNativeView},
    {"room_titles.team.leave",60000,&TitleLeave},
    {"room_titles.start.create",120000,&TitleGridCreate},
    {"room_titles.start.rename",120000,&TitlePvpRename},
    {"room_titles.start",180000,&TitleStartSmoke},
};
// These original windows belong to the compiled legacy APP14/APP16 graph.
// Current creation success always enters APP18, whose mission rename uses
// TitleDlg instead. Retain legacy adapters with the protocol/ABI regressions;
// do not construct hidden scenes to manufacture end-to-end coverage.
const SdgoGameTestCaseV1 legacyTitleCases[]={
    {"room_titles.team.settings",120000,&TitleTeamSettings},
    {"room_titles.team.rename",120000,&TitleTeamRename},
    {"room_titles.mission.settings",120000,&TitleMissionSettings},
};
const SdgoGameTestCaseV1 realCases[] = {
    {"framework.bootstrap", 60000, &Bootstrap},
    {"flow.login", 300000, &Login},
    {"flow.onboarding", 180000, &Onboarding},
    {"pvp.room", 120000, &Room},
    {"pvp.start", 120000, &Start},
    {"pvp.ai_spawn", 60000, &AiSpawn},
    {"pvp.return", 600000, &Return},
};
int __cdecl FpsCap(const SdgoGameTestContextV1* context) {
    const auto graphics=quietGraphics.State();
    return fpsActions.Cap(context,graphics.present_qpc_frequency,
        graphics.successful_present_min_gap_ticks,graphics.successful_present_max_gap_ticks);
}
int __cdecl FpsCadence(const SdgoGameTestContextV1* context) {
    const auto graphics=quietGraphics.State();
    return fpsActions.Cadence(context,graphics.successful_presents,graphics.present_qpc_frequency,
        graphics.successful_present_min_gap_ticks,graphics.successful_present_max_gap_ticks);
}
const SdgoGameTestCaseV1 fpsCases[]={
    {"framework.bootstrap",180000,&Bootstrap},
    {"fps.native_cap",10000,&FpsCap},
    {"fps.native_cadence",150000,&FpsCadence},
};
SdgoGameTestProviderV1 provider{sizeof(provider), SDGO_GAME_TEST_VERSION, "pvp-ai", "client", 7, realCases};
std::vector<std::string> caseNames;
std::vector<SdgoGameTestCaseV1> plannedCases;
void AppendUnitPresentations() {
    if(!testUnitPresentations)return;
    plannedCases.insert(plannedCases.end(),std::begin(unitPresentationCases),std::begin(unitPresentationCases)+2);
    if(captureUnitPortraits)plannedCases.insert(plannedCases.end(),std::begin(portraitCases),std::end(portraitCases));
    plannedCases.insert(plannedCases.end(),std::begin(unitPresentationCases)+2,std::end(unitPresentationCases));
}
std::vector<SdgoGameTestCaseV1> capturedOriginalCases,capturedCases;
int __cdecl ObserveThenCapture(const SdgoGameTestContextV1* context) {
    static std::string current;
    static bool nativeComplete=false,requested=false;
    static unsigned frame=0;
    static uint64_t capturedAt=0;
    if(current!=context->case_id) {
        current=context->case_id;nativeComplete=false;requested=false;frame=0;capturedAt=0;
    }
    if(!nativeComplete) {
        const auto found=std::find_if(capturedOriginalCases.begin(),capturedOriginalCases.end(),
            [&](const auto& test){return current==test.id;});
        if(found==capturedOriginalCases.end())return SDGO_GAME_TEST_FAIL;
        const int result=found->poll(context);
        if(result!=SDGO_GAME_TEST_PASS)return result;
        nativeComplete=true;
    }
    const unsigned target=current=="pages.unit_index.pose_ready" ? 3u:2u;
    const uint64_t interval=current.find("pages.unit_index.pose_")==0 ? 350:800;
    if(frame==target)return SDGO_GAME_TEST_PASS;
    if(frame && context->elapsed_ms-capturedAt<interval)return SDGO_GAME_TEST_WAIT;
    if(!requested) {
        const auto folder=sdgo_test::directory+L"\\visual";
        if(!CreateDirectoryW(folder.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
            context->Assert(0,"the owned native frame output directory is writable");return SDGO_GAME_TEST_FAIL;
        }
        const std::wstring name(current.begin(),current.end());
        const auto path=folder+L"\\"+name+L"-"+std::to_wstring(frame)+L".bmp";
        if(!quietGraphics.CaptureNextFrame(path.c_str())) {
            context->Assert(0,"the real native D3D frame observation is available");return SDGO_GAME_TEST_FAIL;
        }
        requested=true;return SDGO_GAME_TEST_WAIT;
    }
    const HRESULT result=quietGraphics.CaptureFrameResult();
    if(result==E_PENDING)return SDGO_GAME_TEST_WAIT;
    context->Assert(SUCCEEDED(result),"the real native backbuffer was read before Present without another renderer");
    if(FAILED(result))return SDGO_GAME_TEST_FAIL;
    char captured[128];sprintf_s(captured,"native visual frame %u captured after genuine business assertions and ordinary updates",frame);
    context->Observe(captured);
    ++frame;requested=false;capturedAt=context->elapsed_ms;
    return frame==target ? SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_WAIT;
}
void WrapNativeCaptures() {
    if(!captureNativeFrames)return;
    capturedOriginalCases.assign(provider.cases,provider.cases+provider.count);
    capturedCases=capturedOriginalCases;
    // Login/bootstrap can finish before the native renderer starts presenting.
    // Capture the requested native pages without holding authentication behind
    // a screenshot of an initialization state.
    for(auto& test:capturedCases)
        if(std::strncmp(test.id,"pages.",6)==0)test.poll=&ObserveThenCapture;
    provider.cases=capturedCases.data();
}

friends::FriendActions::Result CreateFriendInviteRoom(void*,const SdgoGameTestContextV1* context) {
    const auto result=Room(context);
    return result==SDGO_GAME_TEST_PASS ? friends::FriendActions::kReady :
        result==SDGO_GAME_TEST_FAIL ? friends::FriendActions::kNativeRequestNotSent : friends::FriendActions::kWaitingForReply;
}
int __cdecl FriendFlow(const SdgoGameTestContextV1* context) {
    static bool hangarExitRequested=false;
    if(!friendSession.Ready() && actions.AppState()==5) {
        if(!hangarExitRequested) {
            const auto result=hangarNavigation.ClickBottomRadio(L"GameRoomRadio",0x48);
            if(result>=HangarNavigation::kDownmenuServiceMismatch || result==HangarNavigation::kUnbound) {
                context->Assert(0,"native GameRoomRadio must enter the lobby before friend operations");return SDGO_GAME_TEST_FAIL;
            }
            if(result==HangarNavigation::kReady) {
                hangarExitRequested=true;context->Observe("original GameRoomRadio requested after onboarding inventory initialization");
            }
        }
        return SDGO_GAME_TEST_WAIT;
    }
    const auto result=friendSession.Poll(context);
    const auto cohort=pvpSession.State();
    if(cohort.battle_seen) {
        SdgoPvpAiClientSnapshotV1 live{};
        if(!cohort.settled && Snapshot(live) && live.native_app_state==22 &&
           live.received_battle_start_count==cohort.start_sequence &&
           live.profile_pilot_id==cohort.pilot && live.human_participant_count==cohort.humans) {
            SdgoPvpAiClientStartStatusV1 control{};
            control.size=sizeof(control);control.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
            if(getStartStatus)getStartStatus(&control);
            if(!nativePlayerCombat.Poll(context,live,control,scoreboardInput))return SDGO_GAME_TEST_FAIL;
        } else if(cohort.settled)nativePlayerCombat.Finish(context,scoreboardInput);
    }
    return result;
}
void RegisterPlannedCases() {
    if (fpsOnly) {
        provider.suite="native-fps-limit";provider.count=3;provider.cases=fpsCases;return;
    }
    if(missionMode) {
        plannedCases.assign(realCases,realCases+3);
        if(playUnitUid)plannedCases.push_back(playUnitCase);
        plannedCases.insert(plannedCases.end(),std::begin(missionCases),std::end(missionCases));
        if(missionBehaviorProfile)
            plannedCases.insert(
                plannedCases.end(),
                std::begin(missionBehaviorCases),
                std::end(missionBehaviorCases));
        if(missionRespawns)plannedCases.push_back(missionRespawnCase);
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    if (bootstrapOnly) { provider.count = 3; return; }
    if (friendsOnly) {
        plannedCases.assign(realCases,realCases+3);
        plannedCases.push_back({"friends.session",3600000,&FriendFlow});
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    if (roomTitlesOnly) {
        static_cast<void>(legacyTitleCases);
        plannedCases.assign(realCases,realCases+3);
        if(playUnitUid)plannedCases.push_back(playUnitCase);
        if (roomStylePreview) {
            plannedCases.push_back(roomStylePreview==2 ? SdgoGameTestCaseV1{
                "room_titles.mission.create",180000,&PreviewMissionCreate} : titleCases[0]);
            plannedCases.push_back({roomStylePreview==2 ? "room_style.mission.preview" : "room_style.pvp.preview", 90000, &RoomStylePreview});
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        plannedCases.insert(plannedCases.end(),std::begin(titleCases),std::end(titleCases));
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    if (pagesOnly) {
        plannedCases.assign(realCases,realCases+3);
        if(playUnitUid)plannedCases.push_back(playUnitCase);
        if(hangarPagesOnly) {
            plannedCases.insert(plannedCases.end(),std::begin(pageCases),std::begin(pageCases)+3);
            if(ocPanel)plannedCases.push_back(ocPanelCase);
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        if(testShopCatalog && shopVisualOnly) {
            plannedCases.assign(realCases,realCases+2);
            plannedCases.push_back(shopReadyCase);
            if(playUnitUid)plannedCases.push_back(playUnitCase);
            plannedCases.push_back(shopCatalogCase);
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        if(blueprintConfig.recipe) {
            plannedCases.push_back({"pages.blueprint.book",120000,&BlueprintBook});
            plannedCases.push_back({"pages.blueprint.recipe",120000,&BlueprintRecipe});
            if(blueprintConfig.material_count)plannedCases.push_back({"pages.blueprint.materials",120000,&BlueprintMaterials});
            if(blueprintConfig.synthesize)plannedCases.push_back({"pages.blueprint.synthesis",120000,&BlueprintSynthesis});
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        if(!nativeIdProbeIds.empty()) {
            plannedCases.push_back({"catalog.native_id_occupancy",120000,&NativeIdOccupancy});
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        if(!nativeRecordLifecycleIds.empty()) {
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        AppendUnitPresentations();
        if(unitPagesOnly) {
            if(unitAcquisitionConfig.machine_kind || collectionPreviewOnly)plannedCases.push_back(pageCases[7]);
            if(unitAcquisitionConfig.machine_kind)plannedCases.push_back(unitAcquisitionCase);
            if(collectionPreviewOnly)plannedCases.push_back(collectionCases[0]);
            if(activityDrawOnce)plannedCases.push_back({"pages.activity.draw",120000,&DrawCollection});
            provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
        }
        plannedCases.insert(plannedCases.end(),std::begin(pageCases),std::begin(pageCases)+5);
        if(testShopCatalog)plannedCases.push_back(shopCatalogCase);
        plannedCases.insert(plannedCases.end(),std::begin(pageCases)+5,std::end(pageCases));
        if(collectionDraw)plannedCases.insert(plannedCases.end(),std::begin(collectionCases),std::end(collectionCases));
        else if(collectionPreviewOnly)plannedCases.push_back(collectionCases[0]);
        if(unitAcquisitionConfig.machine_kind)plannedCases.push_back(unitAcquisitionCase);
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    const bool needsShip = battleshipOnly || deckOnly || std::find(requestedModes.begin(),requestedModes.end(),6)!=requestedModes.end() ||
        std::find(requestedModes.begin(),requestedModes.end(),7)!=requestedModes.end();
    if (requestedModes.size() == 1 && !needsShip && !giftRequestsOnly && !playUnitUid && !sameRoomRematch && ocAllocateStat<0 && !ocPanel) return;
    const size_t count = 4 + requestedModes.size() * (sameRoomRematch?8:4);
    caseNames.reserve(count); plannedCases.reserve(count);
    for (unsigned i = 0; i < 3; ++i) plannedCases.push_back(realCases[i]);
    if(playUnitUid && ocAllocateStat>=0)plannedCases.push_back(ocAllocateCase);
    if(playUnitUid)plannedCases.push_back(playUnitCase);
    AppendUnitPresentations();
    if (giftRequestsOnly) plannedCases.push_back(giftCase);
    else if (needsShip) plannedCases.push_back(battleshipCase);
    if (deckOnly) plannedCases.push_back(deckCase);
    const char* suffixes[] = {"room", "start", "ai_spawn", "return"};
    if (!giftRequestsOnly && !battleshipOnly && !deckOnly) for (const auto mode : requestedModes) {
      for(unsigned round=0;round<(sameRoomRematch?2u:1u);++round) {
        for (unsigned i = 0; i < 4; ++i) {
            if (requestedModes.size()==1 && !sameRoomRematch) { plannedCases.push_back(realCases[3+i]);continue; }
            const std::string prefix=requestedModes.size()==1 ? "pvp":"pvp.mode"+std::to_string(mode);
            const char* step=sameRoomRematch && !round && i==3 ? "waiting":suffixes[i];
            caseNames.push_back(prefix+(round?".rematch.":".")+step);
            auto test = realCases[3 + i];
            test.id = caseNames.back().c_str();
            plannedCases.push_back(test);
        }
      }
    }
    // The OC panel check runs last, after the settled return to the lobby.
    if(playUnitUid && (ocAllocateStat>=0 || ocPanel) && !giftRequestsOnly && !battleshipOnly && !deckOnly)plannedCases.push_back(ocPanelCase);
    provider.count = static_cast<uint32_t>(plannedCases.size());
    provider.cases = plannedCases.data();
}

HWND WINAPI FindOwnInstance(LPCWSTR className, LPCWSTR title) {
    // Allow a separate test client while preserving the other client's window.
    // Login, the real launcher event, initialization and all game requests stay native.
    if (className && !IS_INTRESOURCE(className) && !wcscmp(className, L"_GONLINE_") &&
        title && !IS_INTRESOURCE(title) && !wcscmp(title, L"Gundam Online")) return nullptr;
    return originalFindWindow(className, title);
}
int WINAPI QuietMessageBoxW(HWND owner,LPCWSTR text,LPCWSTR caption,UINT type) {
    if (!verifyQuietRuntime)return originalMessageBoxW(owner,text,caption,type);
    char message[1024]="native Windows dialog during quiet test";
    if (text)WideCharToMultiByte(CP_UTF8,0,text,-1,message,sizeof(message),nullptr,nullptr);
    PublishQuietSocketReceipt(true);
    sdgo_test::Fail(message);
    return IDCANCEL;
}
int WINAPI QuietMessageBoxA(HWND owner,LPCSTR text,LPCSTR caption,UINT type) {
    if (!verifyQuietRuntime)return originalMessageBoxA(owner,text,caption,type);
    PublishQuietSocketReceipt(true);
    sdgo_test::Fail(text ? text:"native Windows dialog during quiet test");
    return IDCANCEL;
}
void PublishQuietSocketReceipt(bool force) {
    if (!verifyQuietRuntime) return;
    const auto observed=quietSockets.State();
    const auto audio=quietAudio.State();
    const auto graphics=quietGraphics.State();
    static QuietSockets::Observation previous{};
    static QuietAudio::Observation previousAudio{};
    static QuietGraphics::Observation previousGraphics{};
    static bool written=false;
    static uint64_t lastWritten=0;
    const auto now=GetTickCount64();
    if (!force && written && now-lastWritten<250 && observed.escaped==previous.escaped && audio.failed==previousAudio.failed &&
        graphics.dispatch_failures==previousGraphics.dispatch_failures &&
        graphics.dispatch_message_pumps==previousGraphics.dispatch_message_pumps &&
        !graphics.proxy_visible && !graphics.proxy_appwindow)return;
    if (written && !memcmp(&previous,&observed,sizeof(observed)) && !memcmp(&previousAudio,&audio,sizeof(audio)) &&
        !memcmp(&previousGraphics,&graphics,sizeof(graphics)))return;
    const auto path=sdgo_test::directory+L"\\quiet-sockets.ini";
    const auto temporary=path+L".tmp";
    FILE* file=_wfopen(temporary.c_str(),L"wb");
    if (!file)return;
    fprintf(file,"[run]\nid=%s\n[policy]\npid=%lu\nactive=%u\nloopback_only=%u\n[sockets]\nbinds=%u\nredirected=%u\nfailed=%u\nescaped=%u\nhost_lookups=%u\nhost_redirected=%u\nhost_delegated=%u\n[audio]\nactive=%u\nopened=%u\nmuted=%u\nfailed=%u\nopen_failed=%u\n",
        sdgo_test::runId.c_str(),GetCurrentProcessId(),observed.active,quietLoopback ? 1u:0u,observed.binds,observed.redirected,observed.failed,observed.escaped,
        observed.host_lookups,observed.host_redirected,observed.host_delegated,
        audio.active,audio.opened,audio.muted,audio.failed,audio.open_failed);
    fprintf(file,"[graphics]\nactive=%u\nworker_ready=%u\ndispatch_failures=%u\ncreated_devices=%u\nfailed_devices=%u\ncreate_hresult=%ld\npresents=%u\nsuccessful_presents=%u\npresent_qpc_frequency=%llu\nsuccessful_present_min_gap_ticks=%llu\nsuccessful_present_max_gap_ticks=%llu\nproxy_hwnd=%lu\nproxy_visible=%u\nproxy_noactivate=%u\nproxy_toolwindow=%u\nproxy_appwindow=%u\n",

        graphics.active,graphics.worker_ready,graphics.dispatch_failures,graphics.created_devices,graphics.failed_devices,
        static_cast<long>(graphics.last_create_hresult),graphics.presents,graphics.successful_presents,
        static_cast<unsigned long long>(graphics.present_qpc_frequency),
        static_cast<unsigned long long>(graphics.successful_present_min_gap_ticks),
        static_cast<unsigned long long>(graphics.successful_present_max_gap_ticks),
        reinterpret_cast<unsigned long>(graphics.proxy_hwnd),
        graphics.proxy_visible,graphics.proxy_noactivate,graphics.proxy_toolwindow,graphics.proxy_appwindow);
    fprintf(file,"ime_disabled=%u\nrequested_width=%u\nrequested_height=%u\nclient_width=%u\nclient_height=%u\nresolved_width=%u\nresolved_height=%u\nactual_width=%u\nactual_height=%u\nbackbuffer_hresult=%ld\ncooperative_checks=%u\ncooperative_hresult=%ld\n",
        graphics.ime_disabled,graphics.last_requested_width,graphics.last_requested_height,graphics.last_client_width,graphics.last_client_height,
        graphics.last_resolved_width,graphics.last_resolved_height,graphics.last_actual_width,graphics.last_actual_height,
        static_cast<long>(graphics.last_backbuffer_hresult),graphics.cooperative_checks,static_cast<long>(graphics.last_cooperative_hresult));
    fprintf(file,"named_desktop_fallback=%u\ndispatch_message_pumps=%u\n",
        graphics.named_desktop_fallback,graphics.dispatch_message_pumps);
    fprintf(file,"worker_messages=%u\nworker_last_message=%u\nworker_requests=%u\nworker_busy=%u\n",
        graphics.worker_messages,graphics.worker_last_message,graphics.worker_requests,graphics.worker_busy);
    fprintf(file,"texture_calls=%u\ntexture_failures=%u\nrender_target_calls=%u\nrender_target_failures=%u\nrender_target_null=%u\nlast_rt_width=%u\nlast_rt_height=%u\nlast_rt_format=%u\nlast_rt_usage=%u\nlast_rt_hresult=%ld\nlast_rt_return_address=%lu\n",
        graphics.texture_calls,graphics.texture_failures,graphics.render_target_texture_calls,graphics.render_target_texture_failures,
        graphics.render_target_texture_null_results,graphics.last_render_target_width,graphics.last_render_target_height,
        graphics.last_render_target_format,graphics.last_render_target_usage,static_cast<long>(graphics.last_render_target_hresult),
        static_cast<unsigned long>(graphics.last_render_target_return_address));
    fclose(file);
    if (MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        previous=observed;previousAudio=audio;previousGraphics=graphics;written=true;lastWritten=now;
    }
}
LONG CALLBACK NativeFault(EXCEPTION_POINTERS* fault) {
    if (!fault || !fault->ExceptionRecord || !fault->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = fault->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_INT_OVERFLOW || code == EXCEPTION_FLT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        static PVOID volatile sites[32]{};
        static LONG reported[32]{};
        bool report=false;
        for(unsigned index=0;index<32;++index) {
            const auto address=fault->ExceptionRecord->ExceptionAddress;
            const auto existing=InterlockedCompareExchangePointer(&sites[index],address,nullptr);
            if(!existing || existing==address){report=InterlockedIncrement(&reported[index])<=8;break;}
        }
        if (report) {
            PublishQuietSocketReceipt(true);
            const auto* c = fault->ContextRecord;
            sdgo_test::api->Log(sdgo_test::mod, SDGO_LOG_ERROR,
                "native exception %08lX at %p thread=%lu: eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX",
                code, fault->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(),c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            if(code==EXCEPTION_ACCESS_VIOLATION && fault->ExceptionRecord->NumberParameters>=2)
                sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native access kind=%lu address=%p",
                    static_cast<DWORD>(fault->ExceptionRecord->ExceptionInformation[0]),
                    reinterpret_cast<void*>(fault->ExceptionRecord->ExceptionInformation[1]));
            // Keep bounded code/frame diagnostics independent of game singleton
            // access, which may itself be unavailable during the original fault.
            __try {
                const auto* bytes=reinterpret_cast<const uint8_t*>(c->Eip);
                char instruction[97]{};
                for(unsigned i=0;i<32;++i)sprintf_s(instruction+i*3,sizeof(instruction)-i*3,"%02X ",bytes[i]);
                sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native fault code=%s",instruction);
                uintptr_t frame=c->Ebp;
                for(unsigned i=0;i<16 && frame>=c->Esp && frame-c->Esp<0x10000 && !(frame&3);++i) {
                    const auto* words=reinterpret_cast<const uint32_t*>(frame);
                    const uintptr_t next=words[0],address=words[1];
                    sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native frame[%u] base=%p return=%p",i,
                        reinterpret_cast<void*>(frame),reinterpret_cast<void*>(address));
                    if(next<=frame)break;
                    frame=next;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            HMODULE faultModule=nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(fault->ExceptionRecord->ExceptionAddress),&faultModule)) {
                wchar_t filename[MAX_PATH]{};
                if (GetModuleFileNameW(faultModule,filename,MAX_PATH)) {
                    const wchar_t* leaf=wcsrchr(filename,L'\\');
                    sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native exception module=%ls rva=%08lX",
                        leaf ? leaf+1:filename,static_cast<DWORD>(reinterpret_cast<uintptr_t>(fault->ExceptionRecord->ExceptionAddress)-reinterpret_cast<uintptr_t>(faultModule)));
                }
            }
            __try {
                using AppFn=uint8_t*(__cdecl*)();
                const auto appFn=reinterpret_cast<AppFn>(sdgo_test::api->Symbol(sdgo_test::mod,"game.app"));
                const auto app=appFn ? appFn():nullptr;
                if (app && *reinterpret_cast<uint32_t*>(app+0x4A0)==22) {
                    const auto renderer=*reinterpret_cast<const uint8_t* const*>(app+0x4A4);
                    sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native postfx app=%u/%u/%u/%u/%u renderer=%p aggregate=%u flags=%u/%u/%u/%u/%u",
                        app[0xA7],app[0xA8],app[0xA9],app[0xAA],app[0xAB],renderer,renderer ? renderer[0x19C]:255,
                        renderer ? renderer[0x19D]:255,renderer ? renderer[0x19E]:255,renderer ? renderer[0x19F]:255,
                        renderer ? renderer[0x1A0]:255,renderer ? renderer[0x1A1]:255);
                }
                const auto* stack=reinterpret_cast<const uint32_t*>(c->Esp);
                for (unsigned i=0;i<16;++i) {
                    HMODULE owner=nullptr;
                    const auto address=reinterpret_cast<const wchar_t*>(stack[i]);
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,address,&owner))
                        sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_ERROR,"native stack[%u]=%08X module=%p offset=%08X",
                            i,stack[i],owner,stack[i]-reinterpret_cast<uint32_t>(owner));
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void __fastcall Update(uint8_t* app, void*) {
    if (verifyQuietRuntime && !quietGraphics.RefreshDeviceHooks()) {
        sdgo_test::Fail("quiet graphics could not preserve the real device's native dispatch chain");
        PublishQuietSocketReceipt(true);
        sdgo_test::Poll();
        return;
    }
    originalUpdate(app);
    PublishQuietSocketReceipt();
    if (!mainThread) mainThread = GetCurrentThreadId();
    if (GetCurrentThreadId() != mainThread) return;
    static uint32_t previousState = UINT32_MAX;
    uint32_t state = *reinterpret_cast<uint32_t*>(app + 0x4A0);
    if (state != previousState) {
        char text[96]; sprintf_s(text, "observed normal client state=%lu", static_cast<unsigned long>(state));
        sdgo_test::Observe(text); previousState = state;
        if (verifyQuietRuntime && state==22) {
            const auto renderer=*reinterpret_cast<const uint8_t* const*>(app+0x4A4);
            sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_INFO,
                "quiet native graphics effective postfx=%u/%u/%u/%u/%u aggregate=%u thread=%lu",
                app[0xA7],app[0xA8],app[0xA9],app[0xAA],app[0xAB],renderer ? renderer[0x19C]:255,
                GetCurrentThreadId());
            if (!renderer || app[0xA7] || app[0xA8] || app[0xA9] || app[0xAA] || app[0xAB] || renderer[0x19C]) {
                if (reviewFootage)
                    sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_INFO,"review footage keeps the template's native post-processing profile");
                else
                    sdgo_test::Fail("native graphics did not apply the copied no-postprocessing profile");
            }
        }
    }
    if (reviewCamera.Enabled()) {
        SdgoPvpAiClientSnapshotV1 pvp{}; const bool havePvp=Snapshot(pvp);
        SdgoMissionAiClientSnapshotV1 missionSnapshot{}; missionSnapshot.size=sizeof(missionSnapshot);
        missionSnapshot.version=SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;
        const bool haveMission=getMissionSnapshot && getMissionSnapshot(&missionSnapshot)!=0;
        reviewCamera.Tick(state==22,havePvp ? &pvp:nullptr,haveMission ? &missionSnapshot:nullptr);
    }
    sdgo_test::Poll();
    if (!nativeRecordLifecycleIds.empty() && sdgo_test::finished && !sdgo_test::failures) {
        static bool closeRequested=false;
        if (!closeRequested) {
            const HWND window=*reinterpret_cast<const HWND*>(app+0x3c);
            DWORD pid=0;GetWindowThreadProcessId(window,&pid);
            wchar_t name[64]{};GetClassNameW(window,name,64);
            if (window && pid==GetCurrentProcessId() && !wcscmp(name,L"_GONLINE_")) {
                closeRequested=PostMessageW(window,WM_CLOSE,0,0)!=FALSE;
                sdgo_test::api->Log(sdgo_test::mod,SDGO_LOG_INFO,
                    "owned native lifecycle close requested=%u hwnd=%p",closeRequested,window);
            }
        }
    }
}
}
SDGO_GAME_TEST_PROVIDER_ENTRY { return &provider; }
extern "C" __declspec(dllexport) int __cdecl SdgoGameTestCaptureFrameV1(const wchar_t* path) {
    return quietGraphics.RequestFrameCapture(path) ? 1 : 0;
}
extern "C" __declspec(dllexport) LONG __cdecl SdgoGameTestCaptureStatusV1(HRESULT* result) {
    return quietGraphics.FrameCaptureState(result);
}
SDGO_MOD_ENTRY {
    if (!sdgo_test::Open(api, mod, "client")) return 0;
    const bool loaded = LoadPlan();
    if (!nativeRecordLifecycleIds.empty() && !nativeRecordLifecycle.Install(api,mod,
        sdgo_test::runId,sdgo_test::directory+L"\\native-record-lifecycle.json",nativeRecordLifecycleIds))return 0;
    sdgo_test::nativeShutdownAfterSuccess=!nativeRecordLifecycleIds.empty();
    if (!nativeDeathProbe.Install(api,mod)) return 0;
    if (!nativePlayerCombat.Install(api,mod)) return 0;
    reviewCamera.Install(api,mod);
    if (verifyQuietRuntime) {
        // DLLs have their own import tables. Observe every dialog in this
        // isolated test process so a Mod cannot leak a desktop prompt while
        // the runner waits for its assertion failure.
        if (!quietWaitRoomMessages.Install(api,mod) ||
            api->size < offsetof(SdgoApi,HookSystemExport)+sizeof(api->HookSystemExport) || !api->HookSystemExport ||
            !quietAudio.Install(api,mod) || (quietLoopback && !quietSockets.Install(api,mod)) ||
            !api->HookSystemExport(mod,L"user32.dll","MessageBoxW",reinterpret_cast<void*>(&QuietMessageBoxW),reinterpret_cast<void**>(&originalMessageBoxW)) ||
            !api->HookSystemExport(mod,L"user32.dll","MessageBoxA",reinterpret_cast<void*>(&QuietMessageBoxA),reinterpret_cast<void**>(&originalMessageBoxA)))return 0;
        PublishQuietSocketReceipt();
    }
    // Select the configured map through the normal native room page. Zero
    // keeps the game's quick-room random-map policy.
    const int battleMap = api->SettingInt(mod,"BattleMap",0);
    if (battleMap < 0 || battleMap > UINT16_MAX) {
        api->Log(mod,SDGO_LOG_ERROR,"invalid native GameTest BattleMap");
        return 0;
    }
    if (!loaded) sdgo_test::Fail("invalid private account or native scenario configuration");
    RegisterPlannedCases();
    WrapNativeCaptures();
    if (fpsOnly) {
        if (!fpsActions.Bind(api,mod,fpsRequested,fpsModEnabled))sdgo_test::Fail("native FPS readback binding is unavailable");
    } else {
    const NativeActions::Config config{
        accountName,
        accountPassword,
        roomTitle,
        static_cast<uint16_t>(loaded ? (missionMode?missionMode:RequestedMode()) : 1),
        static_cast<uint32_t>(battleMap),
        passwordIsMd5,randomTeams};
    if (!loaded || !actions.Bind(api, mod, config) ||
        (!missionMode && !channels.Bind(api, mod, &actions)) ||
        ((missionMode || roomTitlesOnly) && !mission.Bind(api,mod,&actions)) ||
        !battleships.Bind(api,mod,&actions) || !gifts.Bind(api,mod,&actions) ||
        (deckOnly && !decks.Bind(api,mod,&actions)) ||
        (!hangarNavigation.Bind(api,mod,&actions)) ||
        (blueprintConfig.recipe && !blueprint.Bind(api,mod,&actions,&hangarNavigation,blueprintConfig)) ||
        (testUnitPresentations && !unitPresentations.Bind(api,mod,&hangarNavigation)) ||
        (unitAcquisitionConfig.machine_kind && !unitAcquisition.Bind(api,mod,&hangarNavigation,unitAcquisitionConfig)) ||
        (pagesOnly && !pages.Bind()) ||
        (pagesOnly && !operatorCatalog.Bind(api,mod)) ||
        (pagesOnly && !aiCatalog.Bind(api,mod)) ||
        (pagesOnly && !storage.Bind()) ||
        (pagesOnly && !capsules.Bind(api,mod)) ||
        ((collectionDraw || collectionPreviewOnly) && !collection.Bind(api,mod,&actions)) ||
        (playUnitUid && !unitSelection.Bind(api,mod,&actions)) ||
        ((randomLoadout || playUnitUid) && !activeSelection.Bind(api,mod)) ||
        (testShopCatalog && (!shopCatalog.Bind(api,mod) || !shopCatalog.Configure(
            sdgo_test::directory+L"\\inputs\\shop-assertions.ini",sdgo_test::directory+L"\\inputs\\shop-runtime-display.ini") || shopCatalog.VisualOnly()!=shopVisualOnly)) ||
        (roomTitlesOnly && !roomTitles.Bind(api,mod,&actions)) ||
        (!missionMode && !rooms.Bind(api, mod, &actions, config)) ||
        (missionRespawns && !rooms.BindBattleEndObserver(api,mod,&actions)) ||
        !tutorial.Bind(api, mod, &actions) || !onboarding.Bind(api, mod, &actions))
        sdgo_test::Fail("normal-flow native request bindings or private account configuration are unavailable");
    if (friendsOnly) {
        if (!friendActions.Bind(api,mod,&actions)||!friendExtras.Bind(api,mod)||
            !friendInviteSender.Bind(api,mod,&actions)||!friendInviteReceiver.Bind(api,mod,&actions)||
            !friendRoomActions.Bind(&actions,&rooms,&friendInviteSender,&friendInviteReceiver,&CreateFriendInviteRoom) ||
            !roomJoin.Bind(api,mod,&actions))
            sdgo_test::Fail("native friend operation bindings are unavailable");
        roomJoin.SetRoomResolver([](void* opaque)->const uint8_t*{return static_cast<RoomActions*>(opaque)->LiveRoomInfo();},&rooms);
        if(!pvpSession.Bind(&actions,&rooms,&roomJoin,&Snapshot,&Start,roomTitle,RequestedMode(),expectedAi,&PvpControlEnabled,&PvpGuestChannel))
            sdgo_test::Fail("coordinated native PvP operation bindings are unavailable");
        friendSession.Bind(&friendActions,&friendExtras,api,mod,&friendRoomActions);
        friendSession.BindPvp(&pvpSession);
        friendSession.BindStorageActions(&FriendStorageAction,&FriendStorageWrite);
        friendSession.BindFrameCapture(
            [](const wchar_t* path){return quietGraphics.RequestFrameCapture(path);},
            []()->HRESULT{HRESULT result=E_PENDING;
                return quietGraphics.FrameCaptureState(&result)>=QuietFrameCapture::Complete?result:E_PENDING;});
    }
    roomListVtable=api->Symbol(mod,"flow.native_room_list_vtable");
    modalLoadingVtable=api->Symbol(mod,"flow.native_modal_loading_vtable");
    if (!modalLoadingVtable)sdgo_test::Fail("native modal-loading page identity is unavailable");
    if (!roomListVtable)sdgo_test::Fail("native Game Room page identity is unavailable");
    getSnapshot = reinterpret_cast<SdgoPvpAiClientGetSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_SNAPSHOT_EXPORT));
    api->HookCode(mod,"game.error_name",reinterpret_cast<void*>(&ObserveNativeErrorName),reinterpret_cast<void**>(&originalErrorName));
    getStartStatus = reinterpret_cast<SdgoPvpAiClientGetStartStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_START_STATUS_EXPORT));
    getScoreboardStatus = reinterpret_cast<SdgoPvpAiClientGetScoreboardStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_SCOREBOARD_STATUS_EXPORT));
    if (!fpsOnly && !bootstrapOnly && !pagesOnly && !scoreboardInput.Install(api, mod)) return 0;
    if (!getSnapshot) sdgo_test::Fail("production PvP AI observation export is unavailable");
    getLuaStatus = reinterpret_cast<SdgoPvpAiClientGetLuaStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_LUA_STATUS_EXPORT));
    if (!getLuaStatus) sdgo_test::Fail("production whole-battle Lua counter export is unavailable");
    if(missionMode) {
        getMissionSnapshot=reinterpret_cast<SdgoMissionAiClientGetSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"mission_ai.dll"),SDGO_MISSION_AI_CLIENT_SNAPSHOT_EXPORT));
        if(!getMissionSnapshot)sdgo_test::Fail("production native mission observation export is unavailable");
        if(missionBehaviorProfile) {
            getMissionBehaviorStatus=
                reinterpret_cast<SdgoMissionBehaviorClientGetStatusV1Fn>(
                    GetProcAddress(
                        GetModuleHandleW(L"mission_behavior.dll"),
                        SDGO_MISSION_BEHAVIOR_CLIENT_STATUS_EXPORT));
            if(!getMissionBehaviorStatus)
                sdgo_test::Fail(
                    "production mission behavior profile observation export is unavailable");
            getMissionBossSpecial=
                reinterpret_cast<SdgoBattleAiGetMissionBossSpecialV1Fn>(
                    GetProcAddress(
                        GetModuleHandleW(L"battle_ai.dll"),
                        SDGO_BATTLE_AI_MISSION_BOSS_SPECIAL_EXPORT));
            if(!getMissionBossSpecial)
                sdgo_test::Fail(
                    "passive production mission Boss special observation export is unavailable");
        }
    }
    } // normal authenticated scenarios
    if (!AddVectoredExceptionHandler(1, &NativeFault)) return 0;
    if (!api->HookImport(mod, "user32.dll", "FindWindowW", reinterpret_cast<void*>(&FindOwnInstance), reinterpret_cast<void**>(&originalFindWindow)) ||
        !api->HookCode(mod, "test.client_update", reinterpret_cast<void*>(&Update), reinterpret_cast<void**>(&originalUpdate)))
        return 0;
    const bool namedProxyAllowed=fpsOnly ||
        (quietLoopback&&verifyDirectNetwork&&!verifyRoutedNetwork&&!missionMode);
    if (verifyQuietRuntime && missionBehaviorProfile) {
        // This sealed mission suite uses native edit setters, not IME input.
        // Disable IMM only inside its test process before creating windows;
        // a private-desktop text-services loader can otherwise contend with
        // the graphics worker's first native driver load.
        using DisableImeFn=BOOL(WINAPI*)(DWORD);
        HMODULE imm=GetModuleHandleW(L"imm32.dll");
        const auto disable=imm ? reinterpret_cast<DisableImeFn>(
            GetProcAddress(imm,"ImmDisableIME")) : nullptr;
        const BOOL disabled=disable ? disable(static_cast<DWORD>(-1)) : FALSE;
        api->Log(mod,disabled ? SDGO_LOG_INFO : SDGO_LOG_WARN,
            "sealed mission process IME isolation: %s",disabled ? "active" : "unavailable");
    }
    if (verifyQuietRuntime && !quietWaitRoomMessages.Install(api,mod))return 0;
    if (verifyQuietRuntime && !quietGraphics.Install(api,mod,namedProxyAllowed))return 0;
    PublishQuietSocketReceipt();
    api->Log(mod, SDGO_LOG_INFO, fpsOnly ? "observing native FPS cap and cadence without authentication":"observing normal launcher/login/room/battle flow");
    return 1;
}
