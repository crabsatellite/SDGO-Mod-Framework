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

uint8_t g_serviceManagerStorage = 0;
void* g_serviceManager = &g_serviceManagerStorage;
uint8_t g_buyingSource[0x180] = {};
uint8_t g_buyingLive[0x180] = {};
uint8_t g_wrongBuying[0x180] = {};
uint8_t g_dialogRoot[0x40] = {};
uint8_t g_buyControl[0x180] = {};
void* g_buyingVtable[32] = {};
void* g_wrongVtable[32] = {};

uint8_t g_resultPopup[0x60] = {};
uint8_t g_unitInfoStorage[0x40] = {};
uint8_t* g_unitInfo = g_unitInfoStorage;

bool g_serviceAvailable = false;
bool g_wrongNamedService = false;
bool g_rootAvailable = false;
bool g_controlAvailable = false;
unsigned g_cloneCalls = 0;
unsigned g_buyCalls = 0;
unsigned g_resultCalls = 0;
void* g_lastBuySelf = nullptr;
void* g_lastBuyControl = nullptr;
const void* g_lastBuyCommand = nullptr;
uint32_t g_lastBuyEvent = 0;

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
        !name ||
        std::wcscmp(
            name,
            L"buying") != 0 ||
        !g_serviceAvailable)
        return nullptr;
    return g_wrongNamedService
        ? g_wrongBuying
        : g_buyingLive;
}

void* __fastcall FakeFindChild(
    void* self,
    void*,
    const void* nativeName)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    return self == g_buyingLive &&
        g_rootAvailable &&
        name &&
        std::wcscmp(
            name,
            L"f_buy_dlg") == 0
        ? g_dialogRoot
        : nullptr;
}

void* __fastcall FakeFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    return root == g_dialogRoot &&
        g_controlAvailable &&
        kind == 4 &&
        name &&
        std::wcscmp(
            name,
            L"BuyBtn") == 0
        ? g_buyControl
        : nullptr;
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
    bool enabled)
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
    g_buyControl[0x167] =
        enabled ? 1 : 0;
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
    g_cloneCalls = 0;
    g_buyCalls = 0;
    g_resultCalls = 0;
    g_lastBuySelf = nullptr;
    g_lastBuyControl = nullptr;
    g_lastBuyCommand = nullptr;
    g_lastBuyEvent = 0;
    InitializeControl(false, false);
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
    auto inventory =
        actions.InventoryState();
    Check(
        inventory.manager_present == 1 &&
            inventory.initialized == 0 &&
            inventory.slots_examined == 0,
        "inventory scan preserves the real +0x20 initialized gate");
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
