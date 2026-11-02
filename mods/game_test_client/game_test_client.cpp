#include "quiet_sockets.h"
#include "quiet_audio.h"
#include "quiet_graphics.h"
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
#include "capsule_draw_actions.h"
#include "unit_select_actions.h"
#include "active_selection_observer.h"
#include "shop_catalog_actions.h"
#include "sdgo/net_route_observer.h"
#include "mission_flow_actions.h"
#include "sdgo/mission_ai_client_observer.h"
#include "gift_actions.h"
#include "room_actions.h"
#include "room_title_actions.h"
#include "tutorial_actions.h"
#include "onboarding_actions.h"
#include "onboarding_gate.h"
#include "sdgo/pvp_ai_client_observer.h"
#include <cwchar>

namespace {
static_assert(static_cast<uint32_t>(TutorialActions::kLeftGame) == sdgo_game_test::kTutorialLeftGame, "tutorial observation contract changed");
static_assert(static_cast<uint32_t>(OnboardingActions::kStarterAccepted) == sdgo_game_test::kStarterAccepted, "starter observation contract changed");
static_assert(static_cast<uint32_t>(OnboardingActions::kInitialLoadingAccepted) == sdgo_game_test::kInitialLoadingAccepted, "initial-loading observation contract changed");
using AppMethod = void(__thiscall*)(uint8_t*);
using FindWindowFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
using MessageBoxWFn=int(WINAPI*)(HWND,LPCWSTR,LPCWSTR,UINT);
using MessageBoxAFn=int(WINAPI*)(HWND,LPCSTR,LPCSTR,UINT);
AppMethod originalUpdate;
FindWindowFn originalFindWindow;
MessageBoxWFn originalMessageBoxW;
MessageBoxAFn originalMessageBoxA;
DWORD mainThread;
NativeActions actions;
ChannelActions channels;
BattleShipActions battleships;
BattleShipDeckActions decks;
HangarNavigation hangarNavigation;
PageTestActions pages;
OperatorCatalogActions operatorCatalog;
AiCatalogActions aiCatalog;
StorageTestActions storage;
CapsuleCatalogActions capsules;
BattleCombatActions battleCombat;
CapsuleDrawActions collection;
UnitSelectActions unitSelection;
ActiveSelectionObserver activeSelection;
ActiveSelectionObserver::Snapshot selectionBeforeRound;
bool randomLoadout=false;
ShopCatalogActions shopCatalog;
bool testShopCatalog=false;
MissionFlowActions mission;
uint16_t missionMode=0;
SdgoMissionAiClientGetSnapshotV1Fn getMissionSnapshot=nullptr;
uint64_t playUnitUid=0;
bool collectionDraw=false;
std::vector<SdgoHangarGameTestUnitV1> collectionBeforeUnits;
const void* roomListVtable=nullptr;
const void* modalLoadingVtable=nullptr;
GiftActions gifts;
RoomActions rooms;
RoomTitleActions roomTitles;
bool roomTitlesOnly=false;
TutorialActions tutorial;
OnboardingActions onboarding;
SdgoPvpAiClientGetSnapshotV1Fn getSnapshot;
SdgoPvpAiClientGetLuaStatusV1Fn getLuaStatus;
char accountName[32] = {}, accountPassword[128] = {}, roomTitle[21] = {};
unsigned loginAttempts = 0, retryCount = 1;
uint32_t loginReplySeen = 0;
bool expectFirstFailure = false;
bool passwordIsMd5 = false;
bool importedExistingAccount = false;
bool bootstrapOnly = false;
bool verifyDirectNetwork = false;
bool verifyRoutedNetwork = false;
bool verifyQuietRuntime = false;
bool quietLoopback = false;
QuietSockets quietSockets;
QuietAudio quietAudio;
QuietGraphics quietGraphics;
void PublishQuietSocketReceipt(bool force=false);
bool battleshipOnly = false, giftRequestsOnly = false, deckOnly = false, pagesOnly = false;
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
uint16_t RequestedMode() { return missionMode?missionMode:requestedModes.at(modeIndex); }
struct MotionSample {
    uint32_t pilot, object, npc;
    float x, z;
    bool moved;
};
std::vector<MotionSample> motionSamples;

bool LoadPlan() {
    const std::wstring job = sdgo_test::directory + L"\\run.ini";
    wchar_t networkPolicy[24]{};
    GetPrivateProfileStringW(L"run",L"network_policy",L"legacy",networkPolicy,24,job.c_str());
    verifyRoutedNetwork=!wcscmp(networkPolicy,L"routed_loopback");
    verifyDirectNetwork = !wcscmp(networkPolicy,L"direct") || verifyRoutedNetwork;
    verifyQuietRuntime=GetPrivateProfileIntW(L"run",L"quiet",0,job.c_str())!=0;
    quietLoopback=verifyQuietRuntime && GetPrivateProfileIntW(L"run",L"quiet_loopback",0,job.c_str())==1;
    bootstrapOnly = GetPrivateProfileIntW(L"run", L"bootstrap_only", 0, job.c_str()) != 0;
    battleshipOnly = GetPrivateProfileIntW(L"run", L"battleship_only", 0, job.c_str()) != 0;
    giftRequestsOnly = GetPrivateProfileIntW(L"run", L"gift_requests_only", 0, job.c_str()) != 0;
    deckOnly = GetPrivateProfileIntW(L"run", L"deck_only", 0, job.c_str()) != 0;
    pagesOnly = GetPrivateProfileIntW(L"run", L"pages_only", 0, job.c_str()) != 0;
    roomTitlesOnly = GetPrivateProfileIntW(L"run", L"room_titles_only", 0, job.c_str()) != 0;
    wchar_t shopAssertions[128]{},shopDisplay[128]{};
    GetPrivateProfileStringW(L"run",L"shop_assertions_path",L"",shopAssertions,128,job.c_str());
    GetPrivateProfileStringW(L"run",L"shop_display_path",L"",shopDisplay,128,job.c_str());
    testShopCatalog=shopAssertions[0] || shopDisplay[0];
    if(testShopCatalog && (!pagesOnly || wcscmp(shopAssertions,L"inputs/shop-assertions.ini") ||
        wcscmp(shopDisplay,L"inputs/shop-runtime-display.ini")))return false;
    collectionDraw=GetPrivateProfileIntW(L"run",L"collection_draw",0,job.c_str())!=0;
    if(collectionDraw && !pagesOnly)return false;
    missionMode=static_cast<uint16_t>(GetPrivateProfileIntW(L"run",L"mission_mode",0,job.c_str()));
    if(missionMode && (missionMode!=5 && missionMode!=8))return false;
    wchar_t playUid[32]{};
    GetPrivateProfileStringW(L"run",L"play_unit_uid",L"",playUid,32,job.c_str());
    if(playUid[0]) {
        if(wcsspn(playUid,L"0123456789")!=wcslen(playUid))return false;
        wchar_t* end=nullptr;errno=0;playUnitUid=_wcstoui64(playUid,&end,10);
        if(!playUnitUid || errno || !end || *end)return false;
    }
    if (static_cast<unsigned>(bootstrapOnly) + battleshipOnly + giftRequestsOnly + deckOnly + pagesOnly + roomTitlesOnly > 1) return false;
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
    randomLoadout=GetPrivateProfileIntW(L"run",L"random_loadout",0,job.c_str())!=0;
    if(randomLoadout && strlen(narrowNonce)!=4)return false;
    if(sprintf_s(roomTitle,randomLoadout?"%s !ai e%u a%u !rand":"%s !ai e%u a%u",narrowNonce,(expectedAi+1)/2,expectedAi/2)<0)return false;
    if(missionMode) {
        expectedAi=GetPrivateProfileIntW(L"run",L"mission_ai_count",0,job.c_str());
        const auto level=GetPrivateProfileIntW(L"run",L"mission_ai_level",0,job.c_str());
        if(expectedAi!=3 || level!=2 || sprintf_s(roomTitle,"%s !ai3 lv2",narrowNonce)<0)return false;
        wchar_t actualTitle[32]{};GetPrivateProfileStringW(L"run",L"room_title",L"",actualTitle,32,job.c_str());
        const std::wstring expectedTitle(roomTitle,roomTitle+strlen(roomTitle));
        if(expectedTitle!=actualTitle)return false;
    }
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
    const uint32_t state = actions.AppState();
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
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Login(const SdgoGameTestContextV1* context) {
    const auto auth = actions.LoginState();
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
            if ((RequestedMode()==6 || RequestedMode()==7) && replies.create_reply_error==0x0BC2 &&
                !replies.create_reply_room_id && channels.ExcludeSelectedChannel()) {
                context->Observe("native server rejected this ship-room setting; testing the next server-provided regular VS channel without changing its restrictions");
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
    context->Assert(actions.OnboardingState().profile_pilot_id == authenticatedPilot, "the waiting room retains this run's authenticated pilot");
    context->Assert(nativeRoom.model_present && nativeRoom.local_is_host && nativeRoom.player_count >= 1 && nativeRoom.player_count <= 12,
                    "the normal native room model contains the authenticated host");
    context->Assert(nativeRoom.room_mode == RequestedMode(), "the real waiting room has the requested PvP mode");
    context->Assert(1, "the original client entered its native waiting room after the real server reply");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Start(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 snapshot{};
    if (!startRequested) {
        Snapshot(snapshot); startSequence = snapshot.received_battle_start_count;
        endSequence = rooms.BattleEndState().sequence;
        if (!rooms.StartBattleNativeHost()) return SDGO_GAME_TEST_WAIT;
        startRequested = true; context->Observe("called the original room-host Start business routine");
    }
    if (!Snapshot(snapshot) || snapshot.native_app_state != 22 || snapshot.received_battle_start_count <= startSequence) return SDGO_GAME_TEST_WAIT;
    activeBattleStartSequence = snapshot.received_battle_start_count;
    context->Assert(snapshot.room_mode == RequestedMode(), "the native game mode matches the requested real room mode");
    context->Assert(snapshot.received_battle_start_count > startSequence, "a genuine server battle-start packet preceded native GAME_MAIN");
    return SDGO_GAME_TEST_PASS;
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
int __cdecl AiSpawn(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 snapshot{};
    if (!context->tick) motionSamples.clear();
    if (!Snapshot(snapshot) || snapshot.native_app_state != 22) return SDGO_GAME_TEST_WAIT;
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
    char observation[160];
    sprintf_s(observation, "real AI mode=%u bots=%u allies=%u enemies=%u moved=%u lua=%u observer=%u", RequestedMode(), snapshot.bot_count, allies, enemies, moved, snapshot.lua_error_count, snapshot.lua_error_observer_available);
    context->Observe(observation);
    context->Assert(authenticatedPilot && snapshot.profile_pilot_id == authenticatedPilot, "the battle uses the pilot captured after this run's accepted login");
    const std::wstring expectedTitle(roomTitle, roomTitle + strlen(roomTitle));
    context->Assert(!wcscmp(snapshot.room_title, expectedTitle.c_str()), "the actual battle belongs to this run's server-created room");
    context->Assert(snapshot.human_participant_count == 1, "the real room contains the authenticated test player");
    context->Assert(snapshot.ai_participant_count == expectedAi && snapshot.bot_count == expectedAi, "all server-provided AI participants spawned as native units");
    context->Assert(snapshot.unit_manager_player_count == 1 + expectedAi, "native player and HUD count includes the real player and every AI unit");
    context->Assert(snapshot.local_team >= 1 && snapshot.local_team <= 2 && allies == expectedAi / 2 && enemies == (expectedAi + 1) / 2, "actual AI team placement matches the requested allies and enemies");
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
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Return(const SdgoGameTestContextV1* context) {
    SdgoPvpAiClientSnapshotV1 live{};
    if(Snapshot(live) && live.native_app_state==22) {
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
        endObserved = true;
        char message[128];
        sprintf_s(message, "real battle end opcode=%04X result=%u app=%u sequence=%u", ended.reply_opcode, ended.result_type, ended.native_app_state, ended.sequence);
        context->Observe(message);
        context->Assert(ended.reply_opcode == 0x0871, "the original client consumed the genuine server battle-end reply");
        context->Assert(1, "Lua observation stayed available with zero errors through the entire real battle");
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
        context->Observe("invoked the original first-unit Buy business callback on the live onboarding capsule");
    if (starter.reply_sequence != previousStarterReply) {
        previousStarterReply = starter.reply_sequence;
        char message[144]; sprintf_s(message, "real starter reply opcode=%04X error=%u unit=%u slot=%u tutorial=%u", starter.reply_opcode, starter.reply_error, starter.picked_unit_id, starter.picked_unit_slot, starter.tutorial_level);
        context->Observe(message);
        if (starter.result == OnboardingActions::kStarterRejected) {
            context->Assert(0, "the real backend rejected the first-unit request"); return SDGO_GAME_TEST_FAIL;
        }
        context->Assert(starter.picked_unit_id != 0, "the real first-unit reply supplied a unit id");
    }
    if (onboarding.ConfirmStarterResultPopup())
        context->Observe("invoked the original starter-result confirmation business callback on the real result popup");
    if (onboarding.ConfirmInitialLoadingPopup())
        context->Observe("called the original initial-loading popup confirmation business method");
    const auto initial = onboarding.InitialLoadingState();
    if (initial.result == OnboardingActions::kInitialLoadingRejected) {
        context->Assert(0, "the backend rejected initial-loading completion"); return SDGO_GAME_TEST_FAIL;
    }
    const auto inventory = onboarding.InventoryState();
    static uint32_t previousOwned = UINT32_MAX;
    if (inventory.initialized && inventory.owned_unit_count != previousOwned) {
        char message[128]; sprintf_s(message, "real inventory owned=%u first_unit=%u slot=%u scanned=%u/%u", inventory.owned_unit_count, inventory.first_unit_id, inventory.first_unit_slot, inventory.slots_examined, inventory.slot_capacity);
        context->Observe(message); previousOwned = inventory.owned_unit_count;
    }
    if (inventory.native_fault) {
        context->Assert(0, "the read-only native inventory observation failed"); return SDGO_GAME_TEST_FAIL;
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
        context->Assert(state.tutorial_level >= 4 && (state.ship_uid_low || state.ship_uid_high),
                        "the original game profile contains a real acquired and selected battleship");
        if (result == BattleShipActions::kConfirmed)
            context->Assert(state.reply_opcode == 0x08EF && state.reply_consumed && state.confirm_sequence,
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
    const auto result=storage.Read(context);
    if(result>=StorageTestActions::Invalid) {
        context->Assert(0,"storage native observer is available and belongs to this process/run/UI thread");
        return SDGO_GAME_TEST_FAIL;
    }
    if(result==StorageTestActions::Ready) {
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
    static bool clicked=false;
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
    const bool ready=capsules.Check(sdgo_test::directory+L"\\..\\mods\\capsule_catalog\\runtime.ini");
    const auto state=capsules.State();char message[192];
    sprintf_s(message,"native capsule tables=%u rows=%u/%u collection=%u price=%u error=%u",state.tables,state.verified,state.expected,
        state.collection_machine,state.collection_price,state.error);context->Observe(message);
    context->Assert(ready,"all five configured capsule tables are registered in the actual native grid with exact counts and row identities");
    context->Assert(state.collection_machine==990001 && state.collection_price==100000,"the collection machine has its original 100000 GP price in the real client registry");
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
int ReadShopCatalog(const SdgoGameTestContextV1* context) {
    const auto before=shopCatalog.State();
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
    static uint32_t previous=UINT32_MAX,verified=UINT32_MAX,lastPending=UINT32_MAX;
    if(previous!=result || verified!=state.verified || lastPending!=state.native_request_pending) {
        const auto sent=actions.TransportState();
        char message[512];sprintf_s(message,"native shop result=%s stage=%u verified=%u/%u page=%u/%u product=%u option=%u category=%u validity=%d price=%d payment=%u previews=%u/%u cancel=%u wallet=%u inventory=%u slots=%u/%u bytes=%u pending=%u send=%04X/%u seq=%u",
            ShopCatalogActions::ResultName(result),state.stage,state.verified,state.configured,state.current_page,state.max_page,
            state.product_id,state.option_id,state.category,state.validity,state.price,state.payment,state.buy_previews,state.gift_previews,
            state.cancellations,state.wallet_unchanged,state.inventory_unchanged,state.native_page_nonempty,state.native_page_slots,state.native_page_bytes,state.native_request_pending,sent.opcode,sent.packet_size,sent.send_sequence);context->Observe(message);previous=result;verified=state.verified;lastPending=state.native_request_pending;
        if(state.inventory_failure) {
            char inventory[320];sprintf_s(inventory,"native shop inventory failure=%u ready=%u capacity=%d probeSlot=%u brief=%u/%u/%llu full=%u/%u/%llu",
                state.inventory_failure,state.inventory_ready,state.inventory_capacity,state.inventory_probe_slot,
                state.inventory_brief_unit,state.inventory_brief_slot,static_cast<unsigned long long>(state.inventory_brief_uid),
                state.inventory_full_unit,state.inventory_full_slot,static_cast<unsigned long long>(state.inventory_full_uid));context->Observe(inventory);
        }
    }
    if(result>=ShopCatalogActions::kInvalidContext || result==ShopCatalogActions::kUnbound || result==ShopCatalogActions::kUnconfigured) {
        context->Assert(0,"native stock catalog and preview controls must match the sealed read-only assertions");return SDGO_GAME_TEST_FAIL;
    }
    if(result!=ShopCatalogActions::kComplete)return SDGO_GAME_TEST_WAIT;
    context->Assert(state.stock_assertions && state.configured && state.verified==state.configured &&
        state.buy_previews==state.configured && state.gift_previews==state.configured && state.cancellations==2*state.configured &&
        state.wallet_unchanged && state.inventory_unchanged,
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
    if(result>=CapsuleDrawActions::kMachineServiceMismatch || result==CapsuleDrawActions::kUnbound) {
        context->Assert(0,"activity capsule native action/reply identity is valid and no failed purchase is retried");return SDGO_GAME_TEST_FAIL;
    }
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl PreviewCollection(const SdgoGameTestContextV1* context) {
    const auto result=collection.EnsurePreview();const int pending=CollectionResult(context,result);
    if(pending==SDGO_GAME_TEST_FAIL)return pending;
    if(result!=CapsuleDrawActions::kPreviewReady)return SDGO_GAME_TEST_WAIT;
    const auto s=collection.State();
    context->Assert(s.target_machine==990001 && s.target_price==100000 && s.preview_ready && !s.buy_attempted,
        "original activity controls selected the real collection machine and initialized its own confirmation without purchasing");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl DrawCollection(const SdgoGameTestContextV1* context) {
    auto state=collection.State();
    auto result=state.buy_attempted?collection.Poll():collection.DrawOnce();
    if(result==CapsuleDrawActions::kResultReady || result==CapsuleDrawActions::kClosingResult)result=collection.CloseResult();
    const int pending=CollectionResult(context,result);if(pending==SDGO_GAME_TEST_FAIL)return pending;
    if(result!=CapsuleDrawActions::kComplete)return SDGO_GAME_TEST_WAIT;
    state=collection.State();
    context->Assert(state.buy_attempted && state.buy_submitted && state.request_opcode==0x05D3 && state.request_size==10 &&
        !state.result_reply_status && state.acknowledgement_opcode==0x09C6 &&
        state.original_reply_completed && state.dropbox_serial && state.reward_item_category>=1100000 &&
        state.reward_item_category<1200000 && state.reward_amount==1,
        "one original activity purchase consumed genuine delivery acknowledgements and its original result-close callback was accepted");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl DeliverCollection(const SdgoGameTestContextV1* context) {
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
int MissionProgress(const SdgoGameTestContextV1* context,MissionFlowActions::Result result) {
    static uint32_t previous=UINT32_MAX;
    if(previous!=result) {
        const auto s=mission.PollSnapshot();char message[384];
        sprintf_s(message,"native mission result=%s app=%u channel=%u/%u mission=%u mode=%u room=%u/%u create=%04X consumed=%u start=%04X queued=%u consumed=%u control=%u/%u main=%08X/%u command=%08X ctx=%u reply=%04X/%04X",
            MissionFlowActions::ResultName(result),s.native_app_state,s.selected_channel_group,s.selected_channel,s.selected_mission_id,
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
    const auto result=mission.EnsureRoom(missionMode,roomTitle);const auto pending=MissionProgress(context,result);
    if(pending==SDGO_GAME_TEST_FAIL || result!=MissionFlowActions::kRoomReady)return pending;
    const auto s=mission.PollSnapshot();
    context->Assert(s.native_room_id && s.native_room_id==s.requested_room_id && s.native_room_mode==missionMode && s.local_is_host &&
        s.create_reply_consumed && s.create_opcode==0x0640,"a genuine server-created mission room and original reply consumer retain the selected listed mission and real host");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl StartMission(const SdgoGameTestContextV1* context) {
    const auto result=mission.Start();const auto pending=MissionProgress(context,result);
    if(pending==SDGO_GAME_TEST_FAIL || result!=MissionFlowActions::kBattleStartConsumed)return pending;
    const auto s=mission.PollSnapshot();
    context->Assert(s.start_request_sent && s.start_opcode==0x08A7 && s.battle_start_consumed,
        "the original mission start command sent 08A7 and consumed its own genuine 06C3 before stage-three entry");
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
    return SDGO_GAME_TEST_PASS;
}
int __cdecl ObserveMissionCombat(const SdgoGameTestContextV1* context) {
    SdgoMissionAiClientSnapshotV1 s{};
    if(!MissionSnapshot(s) || !s.active)return SDGO_GAME_TEST_WAIT;
    if(s.room_mode!=missionMode || s.lua_pcall_error_count || s.native_exception_count){context->Assert(0,"whole native mission smoke observation remains error-free and same-mode");return SDGO_GAME_TEST_FAIL;}
    unsigned moved=0,targets=0;
    for(uint32_t i=0;i<s.ai_record_count && i<12;++i) {
        const auto& p=s.participants[i];if(!p.mapping_valid || !p.valid_registered_object)continue;
        if(p.movement_observed)++moved;if(p.target_valid && p.target_hp>0)++targets;
    }
    if(context->elapsed_ms<15000 || !moved || !targets)return SDGO_GAME_TEST_WAIT;
    char message[192];sprintf_s(message,"native mission bounded observation mode=%u generation=%u spawned=%u moved=%u targets=%u lua=%u tick=%u",
        s.room_mode,s.battle_generation,s.spawned_wingman_count,moved,targets,s.lua_pcall_error_count,s.lua_tick_count);context->Observe(message);
    context->Assert(moved && targets && s.lua_successful_tick_count>0,"real mission wingmen move and acquire living native targets across ordinary updates with zero cumulative Lua error");
    context->Observe("bounded native mission smoke ends here; objective clear, natural results, active exit and a remote peer are not claimed");
    return SDGO_GAME_TEST_PASS;
}
const SdgoGameTestCaseV1 missionCases[]={
    {"mission.room",180000,&CreateMission},{"mission.start",180000,&StartMission},
    {"mission.ai_spawn",120000,&SpawnMissionAi},{"mission.combat",120000,&ObserveMissionCombat},
};
const SdgoGameTestCaseV1 collectionCases[]={
    {"pages.collection.preview",120000,&PreviewCollection},
    {"pages.collection.draw",120000,&DrawCollection},
    {"pages.collection.delivery",180000,&DeliverCollection},
};
int __cdecl TitlePvpCreate(const SdgoGameTestContextV1* c) {
    if(actions.AppState()==6){
        const auto result=hangarNavigation.EnsureGameRoom(roomListVtable,modalLoadingVtable,rooms.NativeRoomPageGeneration());
        if(result!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
    }
    return roomTitles.Poll(RoomTitleActions::PvpCreate,c);
}
int __cdecl TitlePvpRename(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::PvpRename,c);}
int __cdecl TitleMissionCreate(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::MissionCreate,c);}
int __cdecl TitleMissionSettings(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::MissionSettings,c);}
int __cdecl TitleMissionRename(const SdgoGameTestContextV1* c){return roomTitles.Poll(RoomTitleActions::MissionRename,c);}
int __cdecl TitleLeave(const SdgoGameTestContextV1* c){
    if(actions.AppState()==6){roomTitles.Reset();return SDGO_GAME_TEST_PASS;}
    static uint64_t last=0;if(GetTickCount64()-last>2000){last=GetTickCount64();if(!rooms.Leave())roomTitles.Leave(c);}
    return SDGO_GAME_TEST_WAIT;
}
const SdgoGameTestCaseV1 titleCases[]={
    {"room_titles.pvp.create",120000,&TitlePvpCreate},
    {"room_titles.pvp.rename",120000,&TitlePvpRename},
    {"room_titles.pvp.leave",60000,&TitleLeave},
    {"room_titles.mission.create",180000,&TitleMissionCreate},
    {"room_titles.mission.settings",120000,&TitleMissionSettings},
    {"room_titles.mission.rename",120000,&TitleMissionRename},
    {"room_titles.mission.leave",60000,&TitleLeave},
};
const SdgoGameTestCaseV1 realCases[] = {
    {"framework.bootstrap", 60000, &Bootstrap},
    {"flow.login", 120000, &Login},
    {"flow.onboarding", 180000, &Onboarding},
    {"pvp.room", 120000, &Room},
    {"pvp.start", 120000, &Start},
    {"pvp.ai_spawn", 60000, &AiSpawn},
    {"pvp.return", 600000, &Return},
};
SdgoGameTestProviderV1 provider{sizeof(provider), SDGO_GAME_TEST_VERSION, "pvp-ai", "client", 7, realCases};
std::vector<std::string> caseNames;
std::vector<SdgoGameTestCaseV1> plannedCases;
void RegisterPlannedCases() {
    if(missionMode) {
        plannedCases.assign(realCases,realCases+3);
        if(playUnitUid)plannedCases.push_back(playUnitCase);
        plannedCases.insert(plannedCases.end(),std::begin(missionCases),std::end(missionCases));
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    if (bootstrapOnly) { provider.count = 3; return; }
    if (roomTitlesOnly) {
        plannedCases.assign(realCases,realCases+3);
        plannedCases.insert(plannedCases.end(),std::begin(titleCases),std::end(titleCases));
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    if (pagesOnly) {
        plannedCases.assign(realCases,realCases+3);
        if(playUnitUid)plannedCases.push_back(playUnitCase);
        plannedCases.insert(plannedCases.end(),std::begin(pageCases),std::begin(pageCases)+5);
        if(testShopCatalog)plannedCases.push_back(shopCatalogCase);
        plannedCases.insert(plannedCases.end(),std::begin(pageCases)+5,std::end(pageCases));
        if(collectionDraw)plannedCases.insert(plannedCases.end(),std::begin(collectionCases),std::end(collectionCases));
        provider.count=static_cast<uint32_t>(plannedCases.size());provider.cases=plannedCases.data();return;
    }
    const bool needsShip = battleshipOnly || deckOnly || std::find(requestedModes.begin(),requestedModes.end(),6)!=requestedModes.end() ||
        std::find(requestedModes.begin(),requestedModes.end(),7)!=requestedModes.end();
    if (requestedModes.size() == 1 && !needsShip && !giftRequestsOnly && !playUnitUid) return;
    const size_t count = 4 + requestedModes.size() * 4;
    caseNames.reserve(count); plannedCases.reserve(count);
    for (unsigned i = 0; i < 3; ++i) plannedCases.push_back(realCases[i]);
    if(playUnitUid)plannedCases.push_back(playUnitCase);
    if (giftRequestsOnly) plannedCases.push_back(giftCase);
    else if (needsShip) plannedCases.push_back(battleshipCase);
    if (deckOnly) plannedCases.push_back(deckCase);
    const char* suffixes[] = {"room", "start", "ai_spawn", "return"};
    if (!giftRequestsOnly && !battleshipOnly && !deckOnly) for (const auto mode : requestedModes) {
        for (unsigned i = 0; i < 4; ++i) {
            if (requestedModes.size()==1) { plannedCases.push_back(realCases[3+i]);continue; }
            caseNames.push_back("pvp.mode" + std::to_string(mode) + "." + suffixes[i]);
            auto test = realCases[3 + i];
            test.id = caseNames.back().c_str();
            plannedCases.push_back(test);
        }
    }
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
    PublishQuietSocketReceipt();
    sdgo_test::Fail(message);
    return IDCANCEL;
}
int WINAPI QuietMessageBoxA(HWND owner,LPCSTR text,LPCSTR caption,UINT type) {
    if (!verifyQuietRuntime)return originalMessageBoxA(owner,text,caption,type);
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
        graphics.dispatch_failures==previousGraphics.dispatch_failures && !graphics.proxy_visible && !graphics.proxy_appwindow)return;
    if (written && !memcmp(&previous,&observed,sizeof(observed)) && !memcmp(&previousAudio,&audio,sizeof(audio)) &&
        !memcmp(&previousGraphics,&graphics,sizeof(graphics)))return;
    const auto path=sdgo_test::directory+L"\\quiet-sockets.ini";
    const auto temporary=path+L".tmp";
    FILE* file=_wfopen(temporary.c_str(),L"wb");
    if (!file)return;
    fprintf(file,"[run]\nid=%s\n[policy]\npid=%lu\nactive=%u\nloopback_only=%u\n[sockets]\nbinds=%u\nredirected=%u\nfailed=%u\nescaped=%u\n[audio]\nactive=%u\nopened=%u\nmuted=%u\nfailed=%u\n",
        sdgo_test::runId.c_str(),GetCurrentProcessId(),observed.active,quietLoopback ? 1u:0u,observed.binds,observed.redirected,observed.failed,observed.escaped,
        audio.active,audio.opened,audio.muted,audio.failed);
    fprintf(file,"[graphics]\nactive=%u\nworker_ready=%u\ndispatch_failures=%u\ncreated_devices=%u\nfailed_devices=%u\ncreate_hresult=%ld\npresents=%u\nproxy_hwnd=%lu\nproxy_visible=%u\nproxy_noactivate=%u\nproxy_toolwindow=%u\nproxy_appwindow=%u\n",
        graphics.active,graphics.worker_ready,graphics.dispatch_failures,graphics.created_devices,graphics.failed_devices,
        static_cast<long>(graphics.last_create_hresult),graphics.presents,reinterpret_cast<unsigned long>(graphics.proxy_hwnd),
        graphics.proxy_visible,graphics.proxy_noactivate,graphics.proxy_toolwindow,graphics.proxy_appwindow);
    fprintf(file,"ime_disabled=%u\nrequested_width=%u\nrequested_height=%u\nclient_width=%u\nclient_height=%u\nresolved_width=%u\nresolved_height=%u\nactual_width=%u\nactual_height=%u\nbackbuffer_hresult=%ld\ncooperative_checks=%u\ncooperative_hresult=%ld\n",
        graphics.ime_disabled,graphics.last_requested_width,graphics.last_requested_height,graphics.last_client_width,graphics.last_client_height,
        graphics.last_resolved_width,graphics.last_resolved_height,graphics.last_actual_width,graphics.last_actual_height,
        static_cast<long>(graphics.last_backbuffer_hresult),graphics.cooperative_checks,static_cast<long>(graphics.last_cooperative_hresult));
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
            if (!renderer || app[0xA7] || app[0xA8] || app[0xA9] || app[0xAA] || app[0xAB] || renderer[0x19C])
                sdgo_test::Fail("native graphics did not apply the copied no-postprocessing profile");
        }
    }
    sdgo_test::Poll();
}
}
SDGO_GAME_TEST_PROVIDER_ENTRY { return &provider; }
SDGO_MOD_ENTRY {
    if (!sdgo_test::Open(api, mod, "client")) return 0;
    const bool loaded = LoadPlan();
    if (verifyQuietRuntime) {
        if (!quietAudio.Install(api,mod) || (quietLoopback && !quietSockets.Install(api,mod)) ||
            !api->HookImport(mod,"user32.dll","MessageBoxW",reinterpret_cast<void*>(&QuietMessageBoxW),reinterpret_cast<void**>(&originalMessageBoxW)) ||
            !api->HookImport(mod,"user32.dll","MessageBoxA",reinterpret_cast<void*>(&QuietMessageBoxA),reinterpret_cast<void**>(&originalMessageBoxA)))return 0;
        PublishQuietSocketReceipt();
    }
    // Map 0 selects the game's normal quick-room random-map policy; the
    // actual map and participants remain provided by the server.
    if (!loaded) sdgo_test::Fail("invalid private account or native scenario configuration");
    RegisterPlannedCases();
    const NativeActions::Config config{
        accountName,
        accountPassword,
        roomTitle,
        static_cast<uint16_t>(loaded ? (missionMode?missionMode:RequestedMode()) : 1),
        0,
        passwordIsMd5};
    if (!loaded || !actions.Bind(api, mod, config) ||
        (!missionMode && !channels.Bind(api, mod, &actions)) ||
        (missionMode && !mission.Bind(api,mod,&actions)) ||
        !battleships.Bind(api,mod,&actions) || !gifts.Bind(api,mod,&actions) ||
        (deckOnly && !decks.Bind(api,mod,&actions)) ||
        (!hangarNavigation.Bind(api,mod,&actions)) ||
        (pagesOnly && !pages.Bind()) ||
        (pagesOnly && !operatorCatalog.Bind(api,mod)) ||
        (pagesOnly && !aiCatalog.Bind(api,mod)) ||
        (pagesOnly && !storage.Bind()) ||
        (pagesOnly && !capsules.Bind(api,mod)) ||
        (collectionDraw && !collection.Bind(api,mod,&actions)) ||
        (playUnitUid && !unitSelection.Bind(api,mod,&actions)) ||
        (randomLoadout && !activeSelection.Bind(api,mod)) ||
        (testShopCatalog && (!shopCatalog.Bind(api,mod) || !shopCatalog.Configure(
            sdgo_test::directory+L"\\inputs\\shop-assertions.ini",sdgo_test::directory+L"\\inputs\\shop-runtime-display.ini"))) ||
        (roomTitlesOnly && !roomTitles.Bind(api,mod,&actions)) ||
        (!missionMode && !rooms.Bind(api, mod, &actions, config)) ||
        !tutorial.Bind(api, mod, &actions) || !onboarding.Bind(api, mod, &actions))
        sdgo_test::Fail("normal-flow native request bindings or private account configuration are unavailable");
    roomListVtable=api->Symbol(mod,"flow.native_room_list_vtable");
    modalLoadingVtable=api->Symbol(mod,"flow.native_modal_loading_vtable");
    if (!modalLoadingVtable)sdgo_test::Fail("native modal-loading page identity is unavailable");
    if (!roomListVtable)sdgo_test::Fail("native Game Room page identity is unavailable");
    getSnapshot = reinterpret_cast<SdgoPvpAiClientGetSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_SNAPSHOT_EXPORT));
    if (!getSnapshot) sdgo_test::Fail("production PvP AI observation export is unavailable");
    getLuaStatus = reinterpret_cast<SdgoPvpAiClientGetLuaStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), SDGO_PVP_AI_CLIENT_LUA_STATUS_EXPORT));
    if (!getLuaStatus) sdgo_test::Fail("production whole-battle Lua counter export is unavailable");
    if(missionMode) {
        getMissionSnapshot=reinterpret_cast<SdgoMissionAiClientGetSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"mission_ai.dll"),SDGO_MISSION_AI_CLIENT_SNAPSHOT_EXPORT));
        if(!getMissionSnapshot)sdgo_test::Fail("production native mission observation export is unavailable");
    }
    if (!AddVectoredExceptionHandler(1, &NativeFault)) return 0;
    if (!api->HookImport(mod, "user32.dll", "FindWindowW", reinterpret_cast<void*>(&FindOwnInstance), reinterpret_cast<void**>(&originalFindWindow)) ||
        !api->HookCode(mod, "test.client_update", reinterpret_cast<void*>(&Update), reinterpret_cast<void**>(&originalUpdate)))
        return 0;
    if (verifyQuietRuntime && !quietGraphics.Install(api,mod))return 0;
    PublishQuietSocketReceipt();
    api->Log(mod, SDGO_LOG_INFO, "observing normal launcher/login/room/battle flow");
    return 1;
}
