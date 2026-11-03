// OFFLINE-ONLY identity, readiness, and one-shot tests for the beginner
// capsule adapter. The fixture models the shipped CServiceCoinBuying clone,
// native "buying" service registration, f_buy_dlg/BuyBtn control tree, result
// popup readiness byte, and initialized inventory gate. It never starts
// GOnline, contacts a server, grants a unit, or changes account data.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../include/sdgo/sdgo_api.h"

#define private public
#include "../mods/game_test_client/native_actions.h"
#include "../mods/game_test_client/onboarding_actions.h"
#undef private

namespace
{
int g_failures = 0;

uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x700] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_onboardingObject[0x80] = {};

uint8_t g_serviceManagerStorage = 0;
void* g_serviceManager = &g_serviceManagerStorage;
uint8_t g_buyingSource[0x180] = {};
uint8_t g_buyingLive[0x180] = {};
uint8_t g_wrongBuying[0x180] = {};
uint8_t g_dialogRoot[0x40] = {};
uint8_t g_buyControl[0x180] = {};
void* g_buyingVtable[32] = {};
uint8_t g_capsuleSource[0x180] = {};
uint8_t g_capsuleLive[0x180] = {};
uint8_t g_wrongCapsule[0x180] = {};
uint8_t g_machineRoot[0x40] = {};
uint8_t g_capsuleControl[0x180] = {};
void* g_capsuleVtable[32] = {};
uint8_t g_unitResultSource[0x180] = {};
uint8_t g_unitResultLive[0x180] = {};
uint8_t g_wrongUnitResult[0x180] = {};
uint8_t g_unitResultRoot[0x40] = {};
uint8_t g_closeControl[0x180] = {};
void* g_unitResultVtable[32] = {};
void* g_wrongVtable[32] = {};

uint8_t g_resultPopup[0x60] = {};
uint8_t g_unitInfoStorage[0x40] = {};
uint8_t* g_unitInfo = g_unitInfoStorage;

bool g_serviceAvailable = false;
bool g_wrongNamedService = false;
bool g_rootAvailable = false;
bool g_controlAvailable = false;
bool g_capsuleServiceAvailable = false;
bool g_wrongNamedCapsule = false;
bool g_machineRootAvailable = false;
bool g_capsuleControlAvailable = false;
bool g_capsuleCallbackAdvancesState = true;
bool g_unitResultServiceAvailable = false;
bool g_wrongNamedUnitResult = false;
bool g_closeControlAvailable = false;
bool g_nestedCapsuleClone = false;
unsigned g_cloneCalls = 0;
unsigned g_buyCalls = 0;
unsigned g_capsuleCalls = 0;
unsigned g_unitResultCalls = 0;
unsigned g_resultCalls = 0;
void* g_lastBuySelf = nullptr;
void* g_lastBuyControl = nullptr;
const void* g_lastBuyCommand = nullptr;
uint32_t g_lastBuyEvent = 0;
void* g_lastCapsuleSelf = nullptr;
void* g_lastCapsuleControl = nullptr;
const void* g_lastCapsuleCommand = nullptr;
uint32_t g_lastCapsuleEvent = 0;
void* g_lastUnitResultSelf = nullptr;
void* g_lastUnitResultControl = nullptr;
const void* g_lastUnitResultCommand = nullptr;
uint32_t g_lastUnitResultEvent = 0;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

const wchar_t* ReadNativeString(const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(storage);
    return *reinterpret_cast<
                   const uint32_t*>(
                   bytes + 0x14) < 8
        ? reinterpret_cast<
              const wchar_t*>(bytes)
        : *reinterpret_cast<
              wchar_t* const*>(bytes);
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    auto* bytes = static_cast<uint8_t*>(self);
    std::memset(bytes, 0, 0x1C);
    *reinterpret_cast<const wchar_t**>(
        bytes) = value;
    *reinterpret_cast<uint32_t*>(
        bytes + 0x10) =
        static_cast<uint32_t>(
            std::wcslen(value));
    *reinterpret_cast<uint32_t*>(
        bytes + 0x14) = 8;
    return self;
}

void __fastcall FakeNativeWStringDtor(
    void*,
    void*)
{
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeBuyingClone(
    void*,
    void*)
{
    ++g_cloneCalls;
    return g_buyingLive;
}

void* __fastcall FakeOnboardingCapsuleClone(
    void*,
    void*)
{
    ++g_cloneCalls;
    return g_capsuleLive;
}

void* __fastcall FakeOnboardingUnitResultClone(
    void*,
    void*)
{
    ++g_cloneCalls;
    return g_unitResultLive;
}

void __fastcall FakeOnboardingUpdate(
    void* self,
    void*)
{
    if (g_nestedCapsuleClone &&
        self == g_onboardingObject &&
        *reinterpret_cast<uint32_t*>(
            g_onboardingObject + 0x40) == 2) {
        OnboardingActions::
            ObserveOnboardingCapsuleClone(
                g_capsuleSource,
                nullptr);
        *reinterpret_cast<uint32_t*>(
            g_onboardingObject + 0x40) = 100;
    }
}

void* __fastcall FakeServiceFindByName(
    void* manager,
    void*,
    const void* descriptor)
{
    const auto* bytes =
        static_cast<const uint8_t*>(
            descriptor);
    const wchar_t* name =
        bytes
            ? ReadNativeString(
                  bytes +
                  sizeof(uint32_t))
            : nullptr;
    if (manager != g_serviceManager ||
        !bytes ||
        *reinterpret_cast<
            const uint32_t*>(bytes) != 1 ||
        !name)
        return nullptr;
    if (std::wcscmp(
            name,
            L"buying") == 0 &&
        g_serviceAvailable) {
        return g_wrongNamedService
            ? g_wrongBuying
            : g_buyingLive;
    }
    if (std::wcscmp(
            name,
            L"machine") == 0 &&
        g_capsuleServiceAvailable) {
        return g_wrongNamedCapsule
            ? g_wrongCapsule
            : g_capsuleLive;
    }
    if (std::wcscmp(
            name,
            L"ServiceUnitInfoCB") == 0 &&
        g_unitResultServiceAvailable) {
        return g_wrongNamedUnitResult
            ? g_wrongUnitResult
            : g_unitResultLive;
    }
    return nullptr;
}

void* __fastcall FakeFindChild(
    void* self,
    void*,
    const void* nativeName)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (self == g_buyingLive &&
        g_rootAvailable &&
        std::wcscmp(
            name,
            L"f_buy_dlg") == 0)
        return g_dialogRoot;
    if (self == g_capsuleLive &&
        g_machineRootAvailable &&
        std::wcscmp(
            name,
            L"machine") == 0)
        return g_machineRoot;
    return nullptr;
}

void* __fastcall FakeFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (root == g_dialogRoot &&
        g_controlAvailable &&
        kind == 4 &&
        std::wcscmp(
            name,
            L"BuyBtn") == 0)
        return g_buyControl;
    if (root == g_machineRoot &&
        g_capsuleControlAvailable &&
        kind == 1 &&
        std::wcscmp(
            name,
            L"CapsuleClick") == 0)
        return g_capsuleControl;
    if (root == g_unitResultRoot &&
        g_closeControlAvailable &&
        kind == 4 &&
        std::wcscmp(
            name,
            L"CloseBtn") == 0)
        return g_closeControl;
    return nullptr;
}

uint32_t __stdcall FakeBuyAction(
    void* self,
    uint32_t event,
    const void* command,
    void* control)
{
    ++g_buyCalls;
    g_lastBuySelf = self;
    g_lastBuyEvent = event;
    g_lastBuyCommand = command;
    g_lastBuyControl = control;
    return 1;
}

uint32_t __stdcall FakeCapsuleAction(
    void* self,
    uint32_t event,
    const void* command,
    void* control)
{
    ++g_capsuleCalls;
    g_lastCapsuleSelf = self;
    g_lastCapsuleEvent = event;
    g_lastCapsuleCommand = command;
    g_lastCapsuleControl = control;
    if (g_capsuleCallbackAdvancesState &&
        self == g_capsuleLive &&
        event == 0x801)
        *reinterpret_cast<uint16_t*>(
            g_capsuleLive + 0x48) = 4;
    return 1;
}

uint32_t __stdcall FakeOnboardingUnitResultAction(
    void* self,
    uint32_t event,
    const void* command,
    void* control)
{
    ++g_unitResultCalls;
    g_lastUnitResultSelf = self;
    g_lastUnitResultEvent = event;
    g_lastUnitResultCommand = command;
    g_lastUnitResultControl = control;
    return 1;
}

void __stdcall FakeResultAction(
    void*,
    int,
    void*,
    int)
{
    ++g_resultCalls;
}

void* __fastcall FakeUnitBriefBySlot(
    void* manager,
    void*,
    void* output,
    uint16_t slot)
{
    std::memset(output, 0, 0x19);
    if (manager == g_unitInfo &&
        slot == 2) {
        *reinterpret_cast<uint32_t*>(
            output) = 11001;
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(
                output) +
            4) = 0x11223344;
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(
                output) +
            8) = 0x55667788;
    }
    return output;
}

void InitializeControl(
    bool visible,
    bool enabled,
    bool hitSelectable = true)
{
    std::memset(
        g_buyControl,
        0,
        sizeof(g_buyControl));
    auto* name =
        g_buyControl + 0x118;
    const wchar_t value[] = L"BuyBtn";
    std::memcpy(
        name,
        value,
        sizeof(value));
    *reinterpret_cast<uint32_t*>(
        name + 0x10) = 6;
    *reinterpret_cast<uint32_t*>(
        name + 0x14) = 7;
    *reinterpret_cast<void**>(
        g_buyControl + 0x15C) =
        g_dialogRoot;
    g_buyControl[0x162] =
        visible ? 1 : 0;
    g_buyControl[0x163] =
        enabled ? 1 : 0;
    g_buyControl[0x167] =
        hitSelectable ? 1 : 0;
}

void InitializeCapsuleControl(
    bool visible,
    bool enabled,
    bool hitSelectable = true)
{
    std::memset(
        g_capsuleControl,
        0,
        sizeof(g_capsuleControl));
    auto* name =
        g_capsuleControl + 0x118;
    static const wchar_t value[] =
        L"CapsuleClick";
    *reinterpret_cast<const wchar_t**>(
        name) = value;
    *reinterpret_cast<uint32_t*>(
        name + 0x10) = 12;
    *reinterpret_cast<uint32_t*>(
        name + 0x14) = 15;
    *reinterpret_cast<void**>(
        g_capsuleControl + 0x15C) =
        g_machineRoot;
    g_capsuleControl[0x162] =
        visible ? 1 : 0;
    g_capsuleControl[0x163] =
        enabled ? 1 : 0;
    g_capsuleControl[0x167] =
        hitSelectable ? 1 : 0;
}

void InitializeCloseControl(
    bool visible,
    bool enabled,
    bool hitSelectable = true)
{
    std::memset(
        g_closeControl,
        0,
        sizeof(g_closeControl));
    auto* name =
        g_closeControl + 0x118;
    static const wchar_t value[] =
        L"CloseBtn";
    *reinterpret_cast<const wchar_t**>(
        name) = value;
    *reinterpret_cast<uint32_t*>(
        name + 0x10) = 8;
    *reinterpret_cast<uint32_t*>(
        name + 0x14) = 15;
    *reinterpret_cast<void**>(
        g_closeControl + 0x15C) =
        g_unitResultRoot;
    g_closeControl[0x162] =
        visible ? 1 : 0;
    g_closeControl[0x163] =
        enabled ? 1 : 0;
    g_closeControl[0x167] =
        hitSelectable ? 1 : 0;
}

void SetCapsuleState(uint16_t state)
{
    *reinterpret_cast<uint16_t*>(
        g_capsuleLive + 0x48) =
        state;
    g_capsuleLive[0x4A] = 0xBD;
    g_capsuleLive[0x4B] = 0x03;
}

void ResetFixture(
    NativeActions& native,
    OnboardingActions& actions)
{
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_onboardingObject,
        0,
        sizeof(g_onboardingObject));
    std::memset(
        g_buyingSource,
        0,
        sizeof(g_buyingSource));
    std::memset(
        g_buyingLive,
        0,
        sizeof(g_buyingLive));
    std::memset(
        g_wrongBuying,
        0,
        sizeof(g_wrongBuying));
    std::memset(
        g_capsuleSource,
        0,
        sizeof(g_capsuleSource));
    std::memset(
        g_capsuleLive,
        0,
        sizeof(g_capsuleLive));
    std::memset(
        g_wrongCapsule,
        0,
        sizeof(g_wrongCapsule));
    std::memset(
        g_capsuleControl,
        0,
        sizeof(g_capsuleControl));
    std::memset(
        g_unitResultSource,
        0,
        sizeof(g_unitResultSource));
    std::memset(
        g_unitResultLive,
        0,
        sizeof(g_unitResultLive));
    std::memset(
        g_wrongUnitResult,
        0,
        sizeof(g_wrongUnitResult));
    std::memset(
        g_unitResultRoot,
        0,
        sizeof(g_unitResultRoot));
    std::memset(
        g_closeControl,
        0,
        sizeof(g_closeControl));
    std::memset(
        g_resultPopup,
        0,
        sizeof(g_resultPopup));
    std::memset(
        g_unitInfoStorage,
        0,
        sizeof(g_unitInfoStorage));
    std::memset(
        g_buyingVtable,
        0,
        sizeof(g_buyingVtable));
    std::memset(
        g_capsuleVtable,
        0,
        sizeof(g_capsuleVtable));
    std::memset(
        g_unitResultVtable,
        0,
        sizeof(g_unitResultVtable));
    std::memset(
        g_wrongVtable,
        0,
        sizeof(g_wrongVtable));

    g_buyingVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeBuyAction);
    g_buyingVtable[
        0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeBuyingClone);
    g_buyingVtable[
        0x5C / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeFindChild);
    *reinterpret_cast<void**>(
        g_buyingSource) =
        g_buyingVtable;
    *reinterpret_cast<void**>(
        g_buyingLive) =
        g_buyingVtable;
    *reinterpret_cast<void**>(
        g_wrongBuying) =
        g_wrongVtable;

    g_capsuleVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeCapsuleAction);
    g_capsuleVtable[
        0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeOnboardingCapsuleClone);
    g_capsuleVtable[
        0x5C / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeFindChild);
    *reinterpret_cast<void**>(
        g_capsuleSource) =
        g_capsuleVtable;
    *reinterpret_cast<void**>(
        g_capsuleLive) =
        g_capsuleVtable;
    *reinterpret_cast<void**>(
        g_wrongCapsule) =
        g_wrongVtable;

    g_unitResultVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeOnboardingUnitResultAction);
    g_unitResultVtable[
        0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeOnboardingUnitResultClone);
    *reinterpret_cast<void**>(
        g_unitResultSource) =
        g_unitResultVtable;
    *reinterpret_cast<void**>(
        g_unitResultLive) =
        g_unitResultVtable;
    *reinterpret_cast<void**>(
        g_wrongUnitResult) =
        g_wrongVtable;

    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 10;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0x138) =
        10230;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0xBC) = 0;
    g_profileStorage[0x62E] = 0;

    native.gameApp_ = &FakeGameApp;
    native.profilePointer_ = &g_profile;

    actions.native_ = &native;
    actions.nativeWStringCtor_ =
        reinterpret_cast<
            OnboardingActions::
                NativeWStringCtorFn>(
                    &FakeNativeWStringCtor);
    actions.nativeWStringDtor_ =
        reinterpret_cast<
            OnboardingActions::
                NativeWStringDtorFn>(
                    &FakeNativeWStringDtor);
    actions.onboardingUpdate_ =
        reinterpret_cast<
            OnboardingActions::
                OnboardingUpdateFn>(
                    &FakeOnboardingUpdate);
    actions.onboardingCapsuleClone_ =
        reinterpret_cast<
            OnboardingActions::
                OnboardingCapsuleCloneFn>(
                    &FakeOnboardingCapsuleClone);
    actions.onboardingCapsuleCloneEntry_ =
        reinterpret_cast<void*>(
            &FakeOnboardingCapsuleClone);
    actions.capsuleResultAction_ =
        &FakeCapsuleAction;
    actions.serviceManagedPointer_ =
        &g_serviceManager;
    actions.serviceFindByName_ =
        reinterpret_cast<
            OnboardingActions::
                ServiceFindByNameFn>(
                    &FakeServiceFindByName);
    actions.nativeUiFindControl_ =
        reinterpret_cast<
            OnboardingActions::
                NativeUiFindControlFn>(
                    &FakeFindControl);
    actions.starterBuyingVtable_ =
        g_buyingVtable;
    actions.starterBuyingClone_ =
        reinterpret_cast<
            OnboardingActions::
                StarterBuyingCloneFn>(
                    &FakeBuyingClone);
    actions.capsuleBuyAction_ =
        &FakeBuyAction;
    actions.onboardingUnitResultVtable_ =
        g_unitResultVtable;
    actions.onboardingUnitResultClone_ =
        reinterpret_cast<
            OnboardingActions::
                OnboardingUnitResultCloneFn>(
                    &FakeOnboardingUnitResultClone);
    actions.onboardingUnitResultCloneEntry_ =
        reinterpret_cast<void*>(
            &FakeOnboardingUnitResultClone);
    actions.onboardingUnitResultAction_ =
        &FakeOnboardingUnitResultAction;
    actions.onboardingStage_ = 100;
    actions.starterResultPopupAction_ =
        reinterpret_cast<
            OnboardingActions::
                StarterResultPopupActionFn>(
                    &FakeResultAction);
    actions.unitInfoPointer_ = &g_unitInfo;
    actions.unitBriefBySlot_ =
        reinterpret_cast<
            OnboardingActions::
                UnitBriefBySlotFn>(
                    &FakeUnitBriefBySlot);
    OnboardingActions::active_ =
        &actions;

    g_serviceAvailable = false;
    g_wrongNamedService = false;
    g_rootAvailable = false;
    g_controlAvailable = false;
    g_capsuleServiceAvailable = false;
    g_wrongNamedCapsule = false;
    g_machineRootAvailable = false;
    g_capsuleControlAvailable = false;
    g_capsuleCallbackAdvancesState = true;
    g_unitResultServiceAvailable = false;
    g_wrongNamedUnitResult = false;
    g_closeControlAvailable = false;
    g_nestedCapsuleClone = false;
    g_cloneCalls = 0;
    g_buyCalls = 0;
    g_capsuleCalls = 0;
    g_unitResultCalls = 0;
    g_resultCalls = 0;
    g_lastBuySelf = nullptr;
    g_lastBuyControl = nullptr;
    g_lastBuyCommand = nullptr;
    g_lastBuyEvent = 0;
    g_lastCapsuleSelf = nullptr;
    g_lastCapsuleControl = nullptr;
    g_lastCapsuleCommand = nullptr;
    g_lastCapsuleEvent = 0;
    g_lastUnitResultSelf = nullptr;
    g_lastUnitResultControl = nullptr;
    g_lastUnitResultCommand = nullptr;
    g_lastUnitResultEvent = 0;
    InitializeControl(false, false);
    InitializeCapsuleControl(false, false);
    InitializeCloseControl(false, false);
}

void TestBuyingLifecycle()
{
    NativeActions native;
    OnboardingActions actions;
    ResetFixture(native, actions);

    *reinterpret_cast<void**>(
        g_buyingSource) =
        g_wrongVtable;
    OnboardingActions::
        ObserveStarterBuyingClone(
            g_buyingSource,
            nullptr);
    Check(
        actions.starterBuying_ == nullptr &&
            actions.starterBuyingCloneSequence_ ==
                0,
        "wrong starter-buying source identity is not captured");

    *reinterpret_cast<void**>(
        g_buyingSource) =
        g_buyingVtable;
    OnboardingActions::
        ObserveStarterBuyingClone(
            g_buyingSource,
            nullptr);
    auto observed = actions.StarterState();
    Check(
        observed.buying_captured == 1 &&
            observed.buying_clone_sequence == 1,
        "real CServiceCoinBuying live clone is captured once");
    Check(
        observed.buying_ui_state ==
            OnboardingActions::
                kStarterBuyingWaiting &&
            observed.request_attempted == 0,
        "missing named buying service remains a non-purchasing wait");
    Check(
        !actions.DrawStarterUnit() &&
            g_buyCalls == 0,
        "starter draw does not call Buy on the capsule or an unnamed clone");

    g_serviceAvailable = true;
    g_wrongNamedService = true;
    observed = actions.StarterState();
    Check(
        observed.buying_ui_state ==
            OnboardingActions::
                kStarterBuyingIdentityMismatch &&
            observed.buying_validation_failed == 1,
        "named service must be the exact captured live clone");
    Check(
        !actions.DrawStarterUnit() &&
            g_buyCalls == 0,
        "identity mismatch cannot submit a starter request");

    g_wrongNamedService = false;
    observed = actions.StarterState();
    Check(
        observed.buying_named_identity_matched ==
            1 &&
            observed.buying_ui_state ==
                OnboardingActions::
                    kStarterBuyingWaiting,
        "matching named clone still waits for f_buy_dlg");

    g_rootAvailable = true;
    g_controlAvailable = true;
    InitializeControl(false, true);
    observed = actions.StarterState();
    Check(
        observed.buying_ui_state ==
            OnboardingActions::
                kStarterBuyingWaiting &&
            observed.buying_control_ready == 0,
        "hidden BuyBtn remains a non-purchasing wait");

    InitializeControl(true, false, true);
    observed = actions.StarterState();
    Check(
        observed.buying_ui_state ==
                OnboardingActions::
                    kStarterBuyingWaiting &&
            observed.buying_control_ready == 0,
        "disabled native BuyBtn remains a non-purchasing wait");

    InitializeControl(true, true, false);
    observed = actions.StarterState();
    Check(
        observed.buying_ui_state ==
                OnboardingActions::
                    kStarterBuyingWaiting &&
            observed.buying_control_ready == 0,
        "non-hit-selectable BuyBtn remains a non-purchasing wait");

    InitializeControl(true, true);
    observed = actions.StarterState();
    Check(
        observed.buying_ui_state ==
            OnboardingActions::
                kStarterBuyingReady &&
            observed.buying_control_ready == 1,
        "visible enabled native BuyBtn is ready");
    Check(
        actions.DrawStarterUnit(),
        "ready native buying window submits through original callback");
    Check(
        g_buyCalls == 1 &&
            g_lastBuySelf == g_buyingLive &&
            g_lastBuySelf !=
                g_buyingSource &&
            g_lastBuyEvent == 0x101,
        "Buy callback receives the live CServiceCoinBuying object once");
    Check(
        g_lastBuyControl ==
                g_buyControl &&
            g_lastBuyCommand ==
                g_buyControl + 0x118 &&
            std::wcscmp(
                ReadNativeString(
                    g_lastBuyCommand),
                L"BuyBtn") == 0,
        "Buy callback receives the real control and its native name");
    Check(
        !actions.DrawStarterUnit() &&
            g_buyCalls == 1,
        "starter purchase is at most once");
}

void TestStarterCapsuleNestedCreationCapture()
{
    NativeActions native;
    OnboardingActions actions;
    ResetFixture(native, actions);

    actions.onboardingStage_ = 2;
    *reinterpret_cast<uint32_t*>(
        g_onboardingObject + 0x40) = 2;
    OnboardingActions::
        ObserveOnboardingCapsuleClone(
            g_capsuleSource,
            nullptr);
    Check(
        actions.onboardingCapsule_ == nullptr &&
            actions.starterCapsuleCloneSequence_ ==
                0,
        "stage-two Tur clone is rejected outside the creating Update call");

    g_nestedCapsuleClone = true;
    OnboardingActions::
        ObserveOnboardingUpdate(
            g_onboardingObject,
            nullptr);
    Check(
        actions.onboardingCapsule_ ==
                g_capsuleLive &&
            actions.starterCapsuleCloneSequence_ ==
                1,
        "stage-two Update captures the nested real Tur clone before stage one hundred is published");
    Check(
        actions.onboardingStage_ == 100 &&
            actions.onboardingUpdateSelf_ ==
                nullptr &&
            actions.onboardingUpdateThreadId_ ==
                0 &&
            actions.onboardingUpdateEntryStage_ ==
                -1,
        "Update publishes the native stage one hundred and clears its scoped creation context");
}

void TestStarterCapsuleResultLifecycle()
{
    NativeActions native;
    OnboardingActions actions;
    ResetFixture(native, actions);

    *reinterpret_cast<void**>(
        g_capsuleSource) =
        g_wrongVtable;
    OnboardingActions::
        ObserveOnboardingCapsuleClone(
            g_capsuleSource,
            nullptr);
    Check(
        actions.onboardingCapsule_ == nullptr &&
            actions.starterCapsuleCloneSequence_ ==
                0 &&
            actions.starterCapsuleUiState_ ==
                OnboardingActions::
                    kStarterCapsuleIdentityMismatch,
        "wrong starter capsule source identity is not captured");

    *reinterpret_cast<void**>(
        g_capsuleSource) =
        g_capsuleVtable;
    OnboardingActions::
        ObserveOnboardingCapsuleClone(
            g_capsuleSource,
            nullptr);
    auto observed = actions.StarterState();
    Check(
        observed.capsule_captured == 1 &&
            observed.capsule_clone_sequence == 1,
        "real CServiceCapsuleMachineTur live clone is captured once");
    Check(
        observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleWaiting &&
            observed.capsule_click_attempted == 0,
        "starter capsule waits before the manager registers machine");
    Check(
        !actions.AdvanceStarterCapsuleResult() &&
            g_capsuleCalls == 0,
        "starter capsule cannot advance before an accepted 0x054A reply");

    actions.starterResult_ =
        OnboardingActions::
            kStarterAccepted;
    actions.starterReply_ = 0x054A;
    actions.starterReplyTutorialLevel_ = 2;
    SetCapsuleState(2);

    g_capsuleServiceAvailable = true;
    g_wrongNamedCapsule = true;
    observed = actions.StarterState();
    Check(
        observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleIdentityMismatch &&
            observed.capsule_named_identity_matched ==
                0,
        "named machine service must be the exact captured Tur clone");
    Check(
        !actions.AdvanceStarterCapsuleResult() &&
            g_capsuleCalls == 0,
        "machine identity mismatch cannot invoke CapsuleClick");

    g_wrongNamedCapsule = false;
    SetCapsuleState(1);
    observed = actions.StarterState();
    Check(
        observed.capsule_named_identity_matched ==
                1 &&
            observed.capsule_native_state == 1 &&
            observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleWaiting,
        "starter capsule waits for native result state two");
    Check(
        !actions.AdvanceStarterCapsuleResult() &&
            g_capsuleCalls == 0,
        "pre-result native state cannot invoke CapsuleClick");

    SetCapsuleState(2);
    g_machineRootAvailable = true;
    g_capsuleControlAvailable = true;
    InitializeCapsuleControl(false, true);
    observed = actions.StarterState();
    Check(
        observed.capsule_native_state == 2 &&
            observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleWaiting &&
            observed.capsule_control_ready == 0,
        "hidden CapsuleClick remains a non-advancing wait");

    InitializeCapsuleControl(true, false, true);
    observed = actions.StarterState();
    Check(
        observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleWaiting &&
            observed.capsule_control_ready == 0,
        "disabled native CapsuleClick remains a non-advancing wait");

    InitializeCapsuleControl(true, true, false);
    observed = actions.StarterState();
    Check(
        observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleWaiting &&
            observed.capsule_control_ready == 0,
        "non-hit-selectable CapsuleClick remains a non-advancing wait");

    InitializeCapsuleControl(true, true);
    observed = actions.StarterState();
    Check(
        observed.capsule_ui_state ==
                OnboardingActions::
                    kStarterCapsuleReady &&
            observed.capsule_control_ready == 1,
        "visible enabled native CapsuleClick is ready");
    Check(
        actions.AdvanceStarterCapsuleResult(),
        "ready starter capsule invokes the original result callback");
    Check(
        g_capsuleCalls == 1 &&
            g_lastCapsuleSelf ==
                g_capsuleLive &&
            g_lastCapsuleSelf !=
                g_capsuleSource &&
            g_lastCapsuleEvent == 0x801,
        "CapsuleClick callback receives the live Tur clone once");
    Check(
        g_lastCapsuleControl ==
                g_capsuleControl &&
            g_lastCapsuleCommand ==
                g_capsuleControl + 0x118 &&
            std::wcscmp(
                ReadNativeString(
                    g_lastCapsuleCommand),
                L"CapsuleClick") == 0,
        "CapsuleClick callback receives the real control and native name");
    observed = actions.StarterState();
    Check(
        observed.capsule_native_state == 4 &&
            observed.capsule_click_attempted == 1 &&
            observed.capsule_click_sequence == 1 &&
            g_capsuleLive[0x4A] == 0xBD &&
            g_capsuleLive[0x4B] == 0x03,
        "shipped callback must move the Tur native state from two to four");
    Check(
        !actions.AdvanceStarterCapsuleResult() &&
            g_capsuleCalls == 1,
        "starter CapsuleClick callback is at most once");

    NativeActions rejectedNative;
    OnboardingActions rejectedActions;
    ResetFixture(
        rejectedNative,
        rejectedActions);
    OnboardingActions::
        ObserveOnboardingCapsuleClone(
            g_capsuleSource,
            nullptr);
    rejectedActions.starterResult_ =
        OnboardingActions::
            kStarterAccepted;
    rejectedActions.starterReply_ = 0x054A;
    rejectedActions.starterReplyTutorialLevel_ = 2;
    SetCapsuleState(2);
    g_capsuleServiceAvailable = true;
    g_machineRootAvailable = true;
    g_capsuleControlAvailable = true;
    InitializeCapsuleControl(true, true);
    g_capsuleCallbackAdvancesState = false;
    Check(
        !rejectedActions.AdvanceStarterCapsuleResult() &&
            g_capsuleCalls == 1 &&
            rejectedActions.starterCapsuleClickSequence_ ==
                0 &&
            rejectedActions.starterCapsuleNativeState_ ==
                2,
        "adapter rejects a callback that does not perform the native state transition");
}

void TestOnboardingUnitResultLifecycle()
{
    NativeActions native;
    OnboardingActions actions;
    ResetFixture(native, actions);

    actions.starterResult_ =
        OnboardingActions::kStarterAccepted;
    actions.starterReply_ = 0x054A;
    actions.starterReplyTutorialLevel_ = 2;
    actions.starterUnitId_ = 11001;
    actions.starterUnitSlot_ = 2;
    g_profileStorage[0xBC] = 2;
    actions.onboardingCapsule_ =
        g_capsuleLive;
    SetCapsuleState(4);
    *reinterpret_cast<uint32_t*>(
        g_capsuleLive + 0x50) = 11001;
    g_capsuleLive[0x80] = 2;

    *reinterpret_cast<uint32_t*>(
        g_unitResultSource + 0x8C) = 11001;
    g_unitResultSource[0x114] = 2;
    *reinterpret_cast<uint32_t*>(
        g_unitResultSource + 0x128) = 11001;
    *reinterpret_cast<uint32_t*>(
        g_unitResultLive + 0x8C) = 11001;
    g_unitResultLive[0x114] = 2;
    *reinterpret_cast<uint32_t*>(
        g_unitResultLive + 0x128) = 11001;

    *reinterpret_cast<void**>(
        g_unitResultSource) =
        g_wrongVtable;
    OnboardingActions::
        ObserveOnboardingUnitResultClone(
            g_unitResultSource,
            nullptr);
    Check(
        actions.onboardingUnitResultPopup_ ==
                nullptr &&
            actions.onboardingUnitResultCloneSequence_ ==
                0 &&
            actions.onboardingUnitResultUiState_ ==
                OnboardingActions::
                    kOnboardingUnitResultIdentityMismatch,
        "wrong unit-result class identity is not captured");

    *reinterpret_cast<void**>(
        g_unitResultSource) =
        g_unitResultVtable;
    *reinterpret_cast<uint32_t*>(
        g_unitResultSource + 0x128) = 99999;
    OnboardingActions::
        ObserveOnboardingUnitResultClone(
            g_unitResultSource,
            nullptr);
    Check(
        actions.onboardingUnitResultPopup_ ==
                nullptr &&
            actions.onboardingUnitResultCloneSequence_ ==
                0 &&
            actions.onboardingUnitResultUiState_ ==
                OnboardingActions::
                    kOnboardingUnitResultTargetMismatch,
        "unrelated unit-result target is not captured");

    *reinterpret_cast<uint32_t*>(
        g_unitResultSource + 0x128) = 11001;
    OnboardingActions::
        ObserveOnboardingUnitResultClone(
            g_unitResultSource,
            nullptr);
    auto observed = actions.StarterState();
    Check(
        observed.unit_result_captured == 1 &&
            observed.unit_result_clone_sequence == 1 &&
            observed.unit_result_unit_id == 11001,
        "exact CServiceUnitInformationPopUp live clone is captured");
    Check(
        observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultWaiting &&
            observed.unit_result_control_ready == 0,
        "unit result waits for manager registration");

    g_unitResultServiceAvailable = true;
    g_wrongNamedUnitResult = true;
    observed = actions.StarterState();
    Check(
        observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultIdentityMismatch &&
            observed.unit_result_named_identity_matched ==
                0,
        "named ServiceUnitInfoCB must be the captured live clone");
    Check(
        !actions.ConfirmStarterResultPopup() &&
            g_unitResultCalls == 0,
        "unit-result identity mismatch cannot emit 0x0FA2");

    g_wrongNamedUnitResult = false;
    observed = actions.StarterState();
    Check(
        observed.unit_result_named_identity_matched ==
                1 &&
            observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultWaiting,
        "matching unit result waits for its real CloseBtn");

    *reinterpret_cast<void**>(
        g_unitResultLive + 0x6C) =
        g_unitResultRoot;
    *reinterpret_cast<void**>(
        g_unitResultLive + 0x70) =
        g_closeControl;
    g_closeControlAvailable = true;
    InitializeCloseControl(false, true);
    observed = actions.StarterState();
    Check(
        observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultWaiting &&
            observed.unit_result_control_ready == 0,
        "hidden CloseBtn remains a non-confirming wait");

    InitializeCloseControl(true, false, true);
    observed = actions.StarterState();
    Check(
        observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultWaiting &&
            observed.unit_result_control_ready == 0,
        "disabled native CloseBtn remains a non-confirming wait");

    InitializeCloseControl(true, true, false);
    observed = actions.StarterState();
    Check(
        observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultWaiting &&
            observed.unit_result_control_ready == 0,
        "non-hit-selectable CloseBtn remains a non-confirming wait");

    InitializeCloseControl(true, true);
    observed = actions.StarterState();
    Check(
        observed.unit_result_ui_state ==
                OnboardingActions::
                    kOnboardingUnitResultReady &&
            observed.unit_result_control_ready == 1 &&
            g_unitResultLive[0x2C] == 0,
        "visible enabled CloseBtn is ready without the item-modal +0x2C gate");
    Check(
        actions.ConfirmStarterResultPopup(),
        "ready unit result invokes the original CloseBtn callback");
    Check(
        g_unitResultCalls == 1 &&
            g_lastUnitResultSelf ==
                g_unitResultLive &&
            g_lastUnitResultSelf !=
                g_unitResultSource &&
            g_lastUnitResultEvent == 0x101,
        "unit-result callback receives the live exact clone once");
    Check(
        g_lastUnitResultControl ==
                g_closeControl &&
            g_lastUnitResultCommand ==
                g_closeControl + 0x118 &&
            std::wcscmp(
                ReadNativeString(
                    g_lastUnitResultCommand),
                L"CloseBtn") == 0,
        "unit-result callback receives the real CloseBtn and native name");
    Check(
        actions.starterResultConfirmSequence_ ==
                1 &&
            actions.starterResultConfirmAttempted_ ==
                1 &&
            !actions.ConfirmStarterResultPopup() &&
            g_unitResultCalls == 1,
        "unit-result confirmation is at most once");
}

void TestExistingResultAndInventoryGates()
{
    NativeActions native;
    OnboardingActions actions;
    ResetFixture(native, actions);

    actions.starterResult_ =
        OnboardingActions::
            kStarterAccepted;
    actions.starterResultPopup_ =
        g_resultPopup;
    Check(
        !actions.ConfirmStarterResultPopup() &&
            g_resultCalls == 0,
        "result popup still waits for its class-specific +0x2C ready byte");
    g_resultPopup[0x2C] = 1;
    Check(
        actions.ConfirmStarterResultPopup() &&
            g_resultCalls == 1,
        "ready result popup uses the original Ok callback");

    *reinterpret_cast<int32_t*>(
        g_unitInfoStorage + 0x0C) = 3;
    *reinterpret_cast<uint32_t*>(
        g_unitInfoStorage + 0x04) = 2;
    *reinterpret_cast<uint32_t*>(
        g_unitInfoStorage + 0x10) = 7;
    *reinterpret_cast<uint32_t*>(
        g_unitInfoStorage + 0x14) = 5;
    *reinterpret_cast<uint32_t*>(
        g_unitInfoStorage + 0x18) = 0x12345678;
    *reinterpret_cast<uint32_t*>(
        g_unitInfoStorage + 0x1C) = 0x9ABCDEF0;
    auto inventory =
        actions.InventoryState();
    Check(
        inventory.manager_present == 1 &&
            inventory.initialized == 0 &&
            inventory.slot_capacity == 3 &&
            inventory.allocated_block_count == 2 &&
            inventory.page_count == 7 &&
            inventory.tail_count == 5 &&
            inventory.last_uid_cursor_low == 0x12345678 &&
            inventory.last_uid_cursor_high == 0x9ABCDEF0 &&
            inventory.slots_examined == 0,
        "inventory diagnostics are visible before the real +0x20 initialized gate");
    g_unitInfoStorage[0x20] = 1;
    inventory = actions.InventoryState();
    Check(
        inventory.initialized == 1 &&
            inventory.scan_complete == 1 &&
            inventory.slots_examined == 3 &&
            inventory.owned_unit_count == 1 &&
            inventory.first_unit_id == 11001 &&
            inventory.first_unit_slot == 2,
        "initialized inventory is read through the shipped slot accessor");
}
}

int main()
{
    TestBuyingLifecycle();
    TestStarterCapsuleNestedCreationCapture();
    TestStarterCapsuleResultLifecycle();
    TestOnboardingUnitResultLifecycle();
    TestExistingResultAndInventoryGates();
    OnboardingActions::active_ = nullptr;
    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "%d onboarding action checks failed\n",
            g_failures);
        return EXIT_FAILURE;
    }
    std::printf(
        "all onboarding action checks passed\n");
    return EXIT_SUCCESS;
}
