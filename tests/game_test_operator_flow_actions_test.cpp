// OFFLINE-ONLY state, ABI, and identity tests for OperatorFlowActions.
// The fixture models the shipped x86 option/confirmation object layouts and
// original-callback transport observations. It never starts GOnline, contacts
// a server, writes SQL, grants currency, or creates a real operator instance.

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "../mods/game_test_client/operator_flow_actions.h"

namespace
{
constexpr uint32_t kOperatorId = 60047;
constexpr uint32_t kProductId = 1039001;
constexpr uint32_t kOptionId = 9547;
constexpr uint32_t kCategoryId = 1219047;
constexpr int32_t kPrice = 100;

struct TransportFixture
{
    std::vector<NativeActions::TransportObservation> events;
};

int g_failures = 0;
uint8_t g_profileStorage[0x7000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_managerStorage[0x80] = {};
void* g_manager = g_managerStorage;

uint8_t g_options[0x180] = {};
uint8_t g_optionsRoot[0x180] = {};
uint8_t g_optionsCombo[0x300] = {};
uint8_t g_optionsBuy[0x180] = {};
sdgo::shop::Option g_option = {};

uint8_t g_confirmation[0x180] = {};
uint8_t g_confirmationRoot[0x180] = {};
uint8_t g_confirmationOk[0x180] = {};

void* g_optionsVtable[32] = {};
void* g_confirmationVtable[32] = {};
void* g_wrongVtable[32] = {};

uint8_t g_operatorInventory[0x200] = {};
uint8_t g_operatorRoot[0x180] = {};
uint8_t g_operatorList[0x300] = {};
uint8_t g_operatorRightOne[0x180] = {};
uint8_t g_operatorLeftOne[0x180] = {};
uint8_t g_operatorActivate[0x180] = {};
uint8_t g_operatorRecords[12 * 0x50] = {};
uint8_t g_operatorReplyRecords[12 * 0x50] = {};
uint8_t g_operatorRows[12][0x40] = {};
uint8_t g_operatorRowControls[12][0x180] = {};
void* g_operatorVtable[32] = {};

uint8_t g_operatorInput[0x80] = {};
uint8_t g_operatorInputRoot[0x180] = {};
uint8_t g_operatorInputEdit[0x300] = {};
uint8_t g_operatorInputButton[0x180] = {};
void* g_operatorInputVtable[32] = {};

void* g_mainframeVtable[32] = {};
unsigned g_operatorPageCalls = 0;
unsigned g_operatorActivationCalls = 0;
unsigned g_operatorNamingCalls = 0;
int g_operatorPressedRow = -1;
OperatorInventoryActions* g_operatorActions = nullptr;

uint8_t g_sentinel[12] = {};
uint8_t g_node[12] = {};
uint8_t g_activeSentinel[12] = {};
uint8_t g_activeNode[12] = {};

TransportFixture g_transport;
OperatorFlowActions::InventoryImage g_inventory;
unsigned g_optionsCalls = 0;
unsigned g_confirmationCalls = 0;
bool g_emit0931 = true;
uint32_t g_0931Result = 1;
bool g_transferConfirmationToActive = false;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void DestroyNativeString(void* storage)
{
    if (!storage)
        return;
    auto* bytes = static_cast<uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<uint32_t*>(bytes + 0x14);
    if (capacity >= 8) {
        void* allocation =
            *reinterpret_cast<void**>(bytes);
        if (allocation)
            HeapFree(
                GetProcessHeap(),
                0,
                allocation);
    }
    std::memset(storage, 0, 0x1C);
}

bool WriteNativeString(void* storage, const wchar_t* value)
{
    if (!storage || !value)
        return false;
    DestroyNativeString(storage);
    auto* bytes = static_cast<uint8_t*>(storage);
    const size_t length = std::wcslen(value);
    if (length < 8) {
        std::memcpy(
            bytes,
            value,
            (length + 1) * sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (length + 1) * sizeof(wchar_t);
        auto* allocation =
            static_cast<wchar_t*>(
                HeapAlloc(
                    GetProcessHeap(),
                    HEAP_ZERO_MEMORY,
                    byteCount));
        if (!allocation)
            return false;
        std::memcpy(
            allocation,
            value,
            byteCount);
        *reinterpret_cast<wchar_t**>(bytes) =
            allocation;
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) =
            static_cast<uint32_t>(length);
    }
    *reinterpret_cast<uint32_t*>(
        bytes + 0x10) =
        static_cast<uint32_t>(length);
    return true;
}

const wchar_t* ReadNativeString(const void* storage)
{
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

void* __fastcall FacadeNativeStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    return WriteNativeString(
               self,
               value)
               ? self
               : nullptr;
}

void __fastcall FacadeNativeStringDtor(
    void* self,
    void*)
{
    DestroyNativeString(self);
}

void* __fastcall FacadeServiceFind(
    void*,
    void*,
    const void* descriptor)
{
    if (!descriptor)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(
            descriptor);
    const wchar_t* name =
        ReadNativeString(
            bytes + sizeof(uint32_t));
    if (name &&
        std::wcscmp(name, L"OpInv") == 0)
        return g_operatorInventory;
    return nullptr;
}

void* __fastcall FacadeFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (root == g_operatorRoot) {
        if (kind == 7 &&
            std::wcscmp(name, L"List") == 0)
            return g_operatorList;
        if (kind == 4 &&
            std::wcscmp(
                name,
                L"RightBtn") == 0)
            return g_operatorRightOne;
        if (kind == 4 &&
            std::wcscmp(
                name,
                L"LeftBtn") == 0)
            return g_operatorLeftOne;
        if (kind == 4 &&
            std::wcscmp(
                name,
                L"OpSallyBtn") == 0)
            return g_operatorActivate;
    }
    if (root == g_operatorInputRoot) {
        if (kind == 8 &&
            std::wcscmp(name, L"Edit") == 0)
            return g_operatorInputEdit;
        if (kind == 4 &&
            std::wcscmp(name, L"Btn2") == 0)
            return g_operatorInputButton;
    }
    return nullptr;
}

void* __fastcall FacadeInputRoot(
    void* service,
    void*,
    const void* nativeName)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    return service == g_operatorInput &&
                   name &&
                   std::wcscmp(
                       name,
                       L"dlg") == 0
               ? g_operatorInputRoot
               : nullptr;
}

void __fastcall FacadeEditSetText(
    void* edit,
    void*,
    const void* nativeText,
    int)
{
    if (edit != g_operatorInputEdit)
        return;
    WriteNativeString(
        g_operatorInputEdit + 0x200,
        ReadNativeString(nativeText));
}

const void* __fastcall FacadeEditGetText(
    void* edit,
    void*)
{
    return edit == g_operatorInputEdit
               ? g_operatorInputEdit + 0x200
               : nullptr;
}

void* __fastcall FacadeListItemByIndex(void* list, void*, int index)
{
    // Original OpInv passes page*6+local to CUIList::GetItemByIndex.
    return list == g_operatorList && index >= 0 && index < 12
        ? g_operatorRows[index] : nullptr;
}

uint8_t __fastcall FacadeListMouse(
    void* list,
    void*,
    uint32_t event,
    int32_t x,
    int32_t y,
    uint32_t,
    uint32_t)
{
    if (list != g_operatorList)
        return 0;
    const int page =
        *reinterpret_cast<int32_t*>(
            g_operatorInventory + 0xB8);
    int hit = -1;
    for (int local = 0;
         local < 6;
         ++local) {
        const int index =
            page * 6 + local;
        const int expectedX =
            static_cast<int>(
                *reinterpret_cast<float*>(
                    g_operatorRowControls[
                        index] +
                    0x148)) +
            1;
        const int expectedY =
            static_cast<int>(
                *reinterpret_cast<float*>(
                    g_operatorRowControls[
                        index] +
                    0x14C)) +
            1;
        if (x == expectedX &&
            y == expectedY) {
            hit = index;
            break;
        }
    }
    if (hit < 0)
        return 0;
    if (event == 0x201) {
        g_operatorPressedRow = hit;
        return 1;
    }
    if (event != 0x202 ||
        g_operatorPressedRow != hit)
        return 0;
    for (int index = 0;
         index < 12;
         ++index)
        g_operatorRows[index][0x25] =
            index == hit ? 1 : 0;
    g_operatorPressedRow = -1;
    return 1;
}

void EmitOperatorPageReply()
{
    if (!g_operatorActions)
        return;
    const int page =
        *reinterpret_cast<int32_t*>(
            g_operatorInventory + 0xB8);
    if (page < 0 || page > 2)
        return;
    if (page == 2) {
        std::array<uint8_t, 7> packet = {};
        g_operatorActions->ObservePageReply(
            0x059F,
            packet.data(),
            static_cast<uint32_t>(
                packet.size()));
        return;
    }
    std::array<uint8_t, 7 + 6 * 0x50>
        packet = {};
    packet[6] = 6;
    std::memcpy(
        packet.data() + 7,
        g_operatorReplyRecords +
            page * 6 * 0x50,
        6 * 0x50);
    for (uint32_t index = 0;
         index < 6;
         ++index) {
        const uint8_t* source =
            packet.data() + 7 +
            index * 0x50;
        uint16_t slot = 0;
        std::memcpy(
            &slot,
            source + 0x10,
            sizeof(slot));
        if (!slot || slot > 12)
            return;
        std::memcpy(
            g_operatorRecords +
                (slot - 1) * 0x50,
            source,
            0x50);
    }
    g_operatorActions->ObservePageReply(
        0x059F,
        packet.data(),
        static_cast<uint32_t>(
            packet.size()));
}

void __stdcall FacadeMainframeCallback(
    void*,
    uint32_t,
    const void*,
    void*)
{
}

void __stdcall FacadeInventoryCallback(
    void* service,
    uint32_t event,
    const void* name,
    void* control)
{
    if (service != g_operatorInventory ||
        event != 0x101 ||
        !name || !control)
        return;
    const wchar_t* value =
        ReadNativeString(name);
    if (control == g_operatorRightOne &&
        std::wcscmp(
            value,
            L"RightOneBtn") == 0) {
        ++g_operatorPageCalls;
        ++*reinterpret_cast<int32_t*>(
            g_operatorInventory + 0xB8);
        EmitOperatorPageReply();
    } else if (
        control == g_operatorLeftOne &&
        std::wcscmp(
            value,
            L"LeftOneBtn") == 0) {
        ++g_operatorPageCalls;
        --*reinterpret_cast<int32_t*>(
            g_operatorInventory + 0xB8);
        EmitOperatorPageReply();
    } else if (
        control == g_operatorActivate &&
        std::wcscmp(
            value,
            L"OpSallyBtn") == 0) {
        ++g_operatorActivationCalls;
    }
}

void __stdcall FacadeInputCallback(
    void* service,
    uint32_t event,
    const void* name,
    void* control)
{
    if (service == g_operatorInput &&
        event == 0x101 &&
        control == g_operatorInputButton &&
        std::wcscmp(
            ReadNativeString(name),
            L"Btn2") == 0)
        ++g_operatorNamingCalls;
}

void InitializeControl(
    uint8_t* control,
    void* root,
    const wchar_t* name,
    bool ready = true)
{
    std::memset(control, 0, 0x180);
    WriteNativeString(
        control + 0x118,
        name);
    *reinterpret_cast<void**>(
        control + 0x15C) = root;
    control[0x162] = ready ? 1 : 0;
    control[0x167] = ready ? 1 : 0;
}

void SetManagedList(
    size_t sentinelOffset,
    size_t countOffset,
    uint8_t* sentinel,
    uint8_t* node,
    void* service)
{
    *reinterpret_cast<void**>(
        g_managerStorage + sentinelOffset) =
        sentinel;
    if (!service) {
        *reinterpret_cast<void**>(
            sentinel) = sentinel;
        *reinterpret_cast<void**>(
            sentinel + 4) = sentinel;
        *reinterpret_cast<uint32_t*>(
            g_managerStorage + countOffset) = 0;
        return;
    }
    *reinterpret_cast<void**>(
        sentinel) = node;
    *reinterpret_cast<void**>(
        sentinel + 4) = node;
    *reinterpret_cast<void**>(
        node) = sentinel;
    *reinterpret_cast<void**>(
        node + 4) = sentinel;
    *reinterpret_cast<void**>(
        node + 8) = service;
    *reinterpret_cast<uint32_t*>(
        g_managerStorage + countOffset) = 1;
}

void SetPopup(void* service)
{
    std::memset(
        g_managerStorage,
        0,
        sizeof(g_managerStorage));
    std::memset(
        g_sentinel,
        0,
        sizeof(g_sentinel));
    std::memset(g_node, 0, sizeof(g_node));
    std::memset(
        g_activeSentinel,
        0,
        sizeof(g_activeSentinel));
    std::memset(
        g_activeNode,
        0,
        sizeof(g_activeNode));
    SetManagedList(
        0x44,
        0x48,
        g_activeSentinel,
        g_activeNode,
        nullptr);
    SetManagedList(
        0x54,
        0x58,
        g_sentinel,
        g_node,
        service);
}

void SetActivePopup(void* service)
{
    SetPopup(nullptr);
    SetManagedList(
        0x44,
        0x48,
        g_activeSentinel,
        g_activeNode,
        service);
}

void PushTransport(
    uint16_t opcode,
    uint32_t size,
    uint32_t result)
{
    NativeActions::TransportObservation value = {};
    value.send_sequence =
        static_cast<uint32_t>(
            g_transport.events.size() + 1);
    value.packet_size = size;
    value.result = result;
    value.opcode = opcode;
    g_transport.events.push_back(value);
}

NativeActions::TransportObservation __cdecl
TransportState(void* context)
{
    const auto* fixture =
        static_cast<const TransportFixture*>(
            context);
    if (!fixture ||
        fixture->events.empty())
        return {};
    return fixture->events.back();
}

NativeActions::TransportObservation __cdecl
FindTransportSince(
    void* context,
    uint32_t baseline,
    uint16_t opcode)
{
    const auto* fixture =
        static_cast<const TransportFixture*>(
            context);
    if (!fixture)
        return {};
    for (const auto& event :
         fixture->events) {
        if (event.send_sequence > baseline &&
            event.opcode == opcode)
            return event;
    }
    return {};
}

bool __cdecl ReadInventory(
    void*,
    OperatorFlowActions::InventoryImage* output)
{
    if (!output)
        return false;
    *output = g_inventory;
    return true;
}

void __stdcall OptionsCallback(
    void* service,
    uint32_t event,
    const void* name,
    void* control)
{
    ++g_optionsCalls;
    if (service != g_options ||
        event != 0x101 ||
        control != g_optionsBuy ||
        std::wcscmp(
            ReadNativeString(name),
            L"BuyBtn") != 0)
        return;
    if (g_transferConfirmationToActive)
        SetActivePopup(g_confirmation);
    else
        SetPopup(g_confirmation);
}

void __stdcall ConfirmationCallback(
    void* service,
    uint32_t event,
    const void* name,
    void* control)
{
    ++g_confirmationCalls;
    if (service != g_confirmation ||
        event != 0x101 ||
        control != g_confirmationOk ||
        std::wcscmp(
            ReadNativeString(name),
            L"OKBtn") != 0)
        return;
    if (g_emit0931)
        PushTransport(
            0x0931,
            0x0E,
            g_0931Result);
    SetPopup(nullptr);
}

void SetWallet(uint32_t gp, uint32_t cash)
{
    std::memcpy(
        g_profileStorage + 0x18F,
        &gp,
        sizeof(gp));
    std::memcpy(
        g_profileStorage + 0x58C,
        &cash,
        sizeof(cash));
}

OperatorFlowActions::InventoryImage BaselineInventory()
{
    OperatorFlowActions::InventoryImage value;
    for (uint16_t slot = 1;
         slot <= 11;
         ++slot) {
        OperatorFlowActions::OperatorRecord record;
        record.uid =
            0x100000000ULL + slot;
        record.operator_id =
            50000 + slot;
        record.slot = slot;
        record.enabled = slot == 1 ? 1 : 0;
        value.records.push_back(record);
    }
    return value;
}

void CleanupStrings()
{
    DestroyNativeString(
        g_optionsRoot + 0x118);
    DestroyNativeString(
        g_optionsCombo + 0x118);
    DestroyNativeString(
        g_optionsBuy + 0x118);
    DestroyNativeString(
        g_confirmationRoot + 0x118);
    DestroyNativeString(
        g_confirmationOk + 0x118);
}

void ResetFixture()
{
    CleanupStrings();
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(g_options, 0, sizeof(g_options));
    std::memset(
        g_optionsRoot,
        0,
        sizeof(g_optionsRoot));
    std::memset(
        g_optionsCombo,
        0,
        sizeof(g_optionsCombo));
    std::memset(
        g_optionsBuy,
        0,
        sizeof(g_optionsBuy));
    std::memset(
        g_confirmation,
        0,
        sizeof(g_confirmation));
    std::memset(
        g_confirmationRoot,
        0,
        sizeof(g_confirmationRoot));
    std::memset(
        g_confirmationOk,
        0,
        sizeof(g_confirmationOk));
    std::memset(
        g_optionsVtable,
        0,
        sizeof(g_optionsVtable));
    std::memset(
        g_confirmationVtable,
        0,
        sizeof(g_confirmationVtable));
    std::memset(
        g_wrongVtable,
        0,
        sizeof(g_wrongVtable));

    g_optionsVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &OptionsCallback);
    g_confirmationVtable[0x38 /
                         sizeof(void*)] =
        reinterpret_cast<void*>(
            &ConfirmationCallback);
    *reinterpret_cast<void**>(g_options) =
        g_optionsVtable;
    *reinterpret_cast<void**>(
        g_confirmation) =
        g_confirmationVtable;
    *reinterpret_cast<uint32_t*>(
        g_options + 0x44) = 4;
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x44) = 3;

    InitializeControl(
        g_optionsRoot,
        nullptr,
        L"dlg");
    InitializeControl(
        g_optionsCombo,
        g_optionsRoot,
        L"Combo");
    InitializeControl(
        g_optionsBuy,
        g_optionsRoot,
        L"BuyBtn");
    InitializeControl(
        g_confirmationRoot,
        nullptr,
        L"dlg");
    InitializeControl(
        g_confirmationOk,
        g_confirmationRoot,
        L"OKBtn");

    *reinterpret_cast<void**>(
        g_options + 0x4C) =
        g_optionsRoot;
    *reinterpret_cast<void**>(
        g_options + 0x50) =
        g_optionsCombo;
    *reinterpret_cast<void**>(
        g_options + 0x5C) =
        g_optionsBuy;
    *reinterpret_cast<int32_t*>(
        g_optionsCombo + 0x21C) = 0;

    g_option = {};
    g_option.productId = kProductId;
    g_option.availableQuantity = 1;
    g_option.id = kOptionId;
    g_option.price = kPrice;
    g_option.validity = 1;
    g_option.category = kCategoryId;
    g_option.payment = 1;
    *reinterpret_cast<sdgo::shop::Option**>(
        g_options + 0xB0) =
        &g_option;
    *reinterpret_cast<sdgo::shop::Option**>(
        g_options + 0xB4) =
        &g_option + 1;

    *reinterpret_cast<void**>(
        g_confirmation + 0x4C) =
        g_confirmationRoot;
    *reinterpret_cast<void**>(
        g_confirmation + 0x78) =
        g_confirmationOk;
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x84) =
        kProductId;
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x8C) =
        kOptionId;

    SetPopup(g_options);
    SetWallet(1000, 500);
    g_inventory = BaselineInventory();
    g_transport.events.clear();
    g_optionsCalls = 0;
    g_confirmationCalls = 0;
    g_emit0931 = true;
    g_0931Result = 1;
    g_transferConfirmationToActive = false;
}

OperatorFlowActions::Config Config()
{
    OperatorFlowActions::Config value;
    value.operator_id = kOperatorId;
    value.product_id = kProductId;
    value.option_id = kOptionId;
    value.category_id = kCategoryId;
    value.price = kPrice;
    value.validity = 1;
    value.payment = 1;
    value.ui_timeout_ms = 100;
    value.network_timeout_ms = 100;
    value.readback_timeout_ms = 100;
    return value;
}

OperatorFlowActions::Bindings Bindings()
{
    OperatorFlowActions::Bindings value;
    value.profile_pointer = &g_profile;
    value.service_managed_pointer =
        &g_manager;
    value.image_base =
        reinterpret_cast<uintptr_t>(
            g_confirmationVtable) -
        0x2000u;
    value.options_vtable = g_optionsVtable;
    value.options_callback =
        &OptionsCallback;
    value.confirmation_vtable =
        g_confirmationVtable;
    value.confirmation_callback =
        &ConfirmationCallback;
    value.transport_context = &g_transport;
    value.transport_state =
        &TransportState;
    value.find_transport_since =
        &FindTransportSince;
    value.inventory_context = &g_inventory;
    value.read_inventory =
        &ReadInventory;
    return value;
}

SdgoGameTestContextV1 Context(
    uint64_t elapsed = 0,
    const char* run = "operator-flow-run")
{
    SdgoGameTestContextV1 value = {};
    value.size = sizeof(value);
    value.version = SDGO_GAME_TEST_VERSION;
    value.run_id = run;
    value.role = "client";
    value.case_id = "operator.purchase";
    value.elapsed_ms = elapsed;
    return value;
}

struct Driver
{
    OperatorFlowActions actions;
    SdgoGameTestContextV1 context =
        Context();

    bool Setup()
    {
        const auto baseline =
            BaselineInventory();
        return actions.Configure(Config()) &&
               actions.Bind(Bindings()) &&
               actions.PrimeBaseline(
                   {1000, 500},
                   baseline) &&
               actions.AdoptOptionsPopup(
                   g_options);
    }

    OperatorFlowActions::Result Poll(
        uint64_t advance = 1)
    {
        context.elapsed_ms += advance;
        return actions.Poll(&context);
    }

    void ReachWait074D()
    {
        Poll();
        Poll();
        Poll();
        Poll();
        Poll();
    }

    void Accept074D(
        uint32_t product = kProductId,
        uint32_t option = kOptionId)
    {
        std::array<uint8_t, 0x33> packet = {};
        std::memcpy(
            packet.data() + 6,
            &product,
            sizeof(product));
        std::memcpy(
            packet.data() + 10,
            &option,
            sizeof(option));
        actions.ObserveShopReply(
            0x074D,
            packet.data(),
            static_cast<uint32_t>(
                packet.size()),
            true);
    }

    void Accept0574()
    {
        std::array<uint8_t, 0xD4> packet = {};
        actions.ObserveShopReply(
            0x0574,
            packet.data(),
            static_cast<uint32_t>(
                packet.size()),
            true);
    }
};

void AddPurchasedOperator(
    uint64_t uid = 0x20000002AULL)
{
    OperatorFlowActions::OperatorRecord record;
    record.uid = uid;
    record.operator_id = kOperatorId;
    record.slot = 12;
    record.enabled = 0;
    g_inventory.records.push_back(record);
}

void TestSuccessfulPurchase()
{
    ResetFixture();
    Driver driver;
    Check(driver.Setup(),
          "valid native purchase fixture binds");
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kOpeningConfirmation,
          "validated exact selected option");
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kWaitingForConfirmation &&
              g_optionsCalls == 1,
          "original options BuyBtn callback runs once");
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kSubmittingPurchase,
          "exact confirmation dialog is accepted");
    const auto confirmationProbe =
        driver.actions.State();
    Check(confirmationProbe.
                  managed_active_matches == 0 &&
              confirmationProbe.
                  managed_pending_matches == 1 &&
              confirmationProbe.
                  managed_expected_vtable_rva ==
                  0x2000u &&
              confirmationProbe.
                  managed_pending_first_vtable_rva ==
                  0x2000u &&
              confirmationProbe.
                  managed_pending_first_matches_expected ==
                  1 &&
              confirmationProbe.
                  managed_pending_first_native_state ==
                  3 &&
              confirmationProbe.
                  confirmation_native_state == 3 &&
              confirmationProbe.
                  confirmation_product_observed ==
                  kProductId &&
              confirmationProbe.
                  confirmation_option_observed ==
                  kOptionId &&
              confirmationProbe.
                  confirmation_readiness == 0x1FF,
          "pending confirmation telemetry records exact vtable, native state and callback readiness");
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kWaitingForPreflightRequest &&
              g_confirmationCalls == 1,
          "original OKBtn callback runs once");
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kWaitingForPreflightReply,
          "real 0931 size and successful transport observed");

    driver.Accept074D();
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kWaitingForAutomaticCommit,
          "original 074D identity observed after dispatch");
    PushTransport(0x0758, 0x39, 1);
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kWaitingForPurchaseAck,
          "original automatic 0758 transport observed");

    SetWallet(900, 500);
    AddPurchasedOperator();
    driver.Accept0574();
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kWaitingForNativeReadback,
          "original 0574 completion gates readback");
    Check(driver.Poll() ==
              OperatorFlowActions::kComplete,
          "exact wallet and unique new UID complete purchase");
    const auto state = driver.actions.State();
    Check(state.request_0931_sequence == 1 &&
              state.request_0758_sequence == 2 &&
              state.reply_074d_sequence == 1 &&
              state.reply_0574_sequence == 2,
          "transport and reply ordering is retained");
    Check(state.new_operator_uid ==
                  0x20000002AULL &&
              state.wallet_verified &&
              state.operator_verified &&
              state.original_0574_completed,
          "native receipt records exact purchased instance");
}

void TestActiveConfirmationPurchase()
{
    ResetFixture();
    g_transferConfirmationToActive = true;
    Driver driver;
    Check(driver.Setup(),
          "active confirmation fixture binds");
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kOpeningConfirmation,
          "active confirmation fixture validates options");
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kWaitingForConfirmation &&
              g_optionsCalls == 1,
          "original callback transfers confirmation to the active manager list");
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kSubmittingPurchase,
          "confirmation transferred from pending to active service list is accepted");
}

void TestConfirmationDiscoveryTelemetry()
{
    ResetFixture();
    Driver mismatched;
    Check(mismatched.Setup(),
          "mismatched confirmation telemetry fixture binds");
    mismatched.Poll();
    mismatched.Poll();
    *reinterpret_cast<void**>(
        g_confirmation) = g_wrongVtable;
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x44) = 4;
    Check(mismatched.Poll() ==
              OperatorFlowActions::
                  kWaitingForConfirmation,
          "unmatched pending service remains a read-only confirmation wait");
    const auto absent =
        mismatched.actions.State();
    Check(absent.managed_pending_count == 1 &&
              absent.managed_pending_matches == 0 &&
              absent.
                  managed_pending_first_vtable_rva !=
                  0 &&
              absent.
                  managed_pending_first_vtable_rva !=
                  absent.
                      managed_expected_vtable_rva &&
              absent.
                  managed_pending_first_matches_expected ==
                  0 &&
              absent.
                  managed_pending_first_native_state ==
                  4,
          "telemetry distinguishes a real state-4 pending object with the wrong vtable");

    ResetFixture();
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x44) = 2;
    Driver initializing;
    Check(initializing.Setup(),
          "initializing confirmation telemetry fixture binds");
    initializing.Poll();
    initializing.Poll();
    Check(initializing.Poll() ==
              OperatorFlowActions::
                  kWaitingForConfirmation,
          "matched pending service in native fade-in state 2 remains a confirmation wait");
    Check(!initializing.actions.
              ConfirmationReadyForCapture(),
          "a state-2 confirmation cannot consume the prequeued final screenshot");
    const auto matched =
        initializing.actions.State();
    Check(matched.managed_pending_count == 1 &&
              matched.managed_pending_matches == 1 &&
              matched.
                  managed_pending_first_matches_expected ==
                  1 &&
              matched.
                  managed_pending_first_native_state ==
                  2 &&
              matched.
                  confirmation_native_state == 2 &&
              matched.
                  confirmation_product_observed ==
                  kProductId &&
              matched.
                  confirmation_option_observed ==
                  kOptionId &&
              matched.
                  confirmation_readiness == 0x1FF,
          "telemetry distinguishes a state-2 confirmation whose callback inputs are present before the native state machine publishes interactive state 3");
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x44) = 3;
    Check(initializing.Poll() ==
              OperatorFlowActions::
                  kSubmittingPurchase,
          "the exact native interactive state 3 advances the fully ready confirmation");
}

void TestOptionsValidationRefusesMutation()
{
    ResetFixture();
    g_option.price = 101;
    Driver driver;
    Check(driver.Setup(),
          "mismatched option fixture still stages");
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kOptionsMismatch &&
              g_optionsCalls == 0 &&
              g_confirmationCalls == 0,
          "wrong native price fails before either callback");

    ResetFixture();
    InitializeControl(
        g_optionsBuy,
        g_optionsRoot,
        L"WrongBuy");
    Driver wrongControl;
    Check(wrongControl.Setup(),
          "wrong control fixture still stages");
    Check(wrongControl.Poll() ==
                  OperatorFlowActions::
                      kOptionsControlMismatch &&
              g_optionsCalls == 0,
          "wrong BuyBtn identity refuses UI action");
}

void TestConfirmationValidationRefusesMutation()
{
    ResetFixture();
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x8C) =
        kOptionId + 1;
    Driver driver;
    Check(driver.Setup(),
          "wrong confirmation fixture stages");
    driver.Poll();
    driver.Poll();
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kConfirmationMismatch &&
              g_confirmationCalls == 0 &&
              !driver.actions.State().
                  mutation_attempted,
          "wrong confirmation option fails before mutation gate");

    ResetFixture();
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x44) = 4;
    Driver closing;
    Check(closing.Setup(),
          "closing confirmation fixture stages");
    closing.Poll();
    closing.Poll();
    Check(closing.Poll() ==
                  OperatorFlowActions::
                      kConfirmationMismatch &&
              g_confirmationCalls == 0 &&
              !closing.actions.State().
                  mutation_attempted,
          "native state 4 is refused as a closed confirmation instead of being treated as ready");
}

void TestConfirmationRootNameIsDiagnostic()
{
    ResetFixture();
    DestroyNativeString(
        g_confirmationRoot + 0x118);
    InitializeControl(
        g_confirmationRoot,
        nullptr,
        L"");
    Driver driver;
    Check(driver.Setup(),
          "unnamed native confirmation root fixture stages");
    driver.Poll();
    driver.Poll();
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kSubmittingPurchase &&
              driver.actions.State().
                  confirmation_readiness ==
                  0x1EF &&
              driver.actions.State().
                  confirmation_validated &&
              driver.actions.
                  ConfirmationReadyForCapture() &&
              !driver.actions.State().
                  mutation_attempted,
          "the exact root pointer and OKBtn parent identity accept an unnamed native root while preserving the callback seal");
}

void TestControlsAreRevalidatedBeforeCallbacks()
{
    ResetFixture();
    Driver options;
    Check(options.Setup(),
          "options revalidation fixture stages");
    Check(options.Poll() ==
              OperatorFlowActions::
                  kOpeningConfirmation,
          "options fixture reaches callback boundary");
    g_optionsBuy[0x167] = 0;
    Check(options.Poll() ==
                  OperatorFlowActions::
                      kOptionsControlMismatch &&
              g_optionsCalls == 0,
          "BuyBtn is revalidated immediately before callback");

    ResetFixture();
    Driver confirmation;
    Check(confirmation.Setup(),
          "confirmation revalidation fixture stages");
    confirmation.Poll();
    confirmation.Poll();
    Check(confirmation.Poll() ==
              OperatorFlowActions::
                  kSubmittingPurchase,
          "confirmation fixture reaches callback boundary");
    g_confirmationOk[0x162] = 0;
    Check(!confirmation.actions.
              ConfirmationReadyForCapture() &&
              confirmation.Poll() ==
                  OperatorFlowActions::
                      kConfirmationControlMismatch &&
              g_confirmationCalls == 0 &&
              !confirmation.actions.State().
                   mutation_attempted,
          "OKBtn is revalidated before mutation seal");

    ResetFixture();
    Driver closingConfirmation;
    Check(closingConfirmation.Setup(),
          "confirmation state revalidation fixture stages");
    closingConfirmation.Poll();
    closingConfirmation.Poll();
    Check(closingConfirmation.Poll() ==
              OperatorFlowActions::
                  kSubmittingPurchase,
          "state revalidation fixture reaches the financial callback boundary");
    *reinterpret_cast<uint32_t*>(
        g_confirmation + 0x44) = 4;
    Check(!closingConfirmation.actions.
              ConfirmationReadyForCapture() &&
              closingConfirmation.Poll() ==
                  OperatorFlowActions::
                      kConfirmationControlMismatch &&
              g_confirmationCalls == 0 &&
              !closingConfirmation.actions.State().
                  mutation_attempted,
          "a confirmation that closes after validation is refused before the mutation seal");
}

void TestMutationIsNeverRetried()
{
    ResetFixture();
    g_emit0931 = false;
    Driver driver;
    Check(driver.Setup(),
          "missing transport fixture stages");
    driver.Poll();
    driver.Poll();
    driver.Poll();
    driver.Poll();
    Check(g_confirmationCalls == 1 &&
              driver.actions.State().
                  mutation_attempted,
          "mutation gate burns before original callback");
    Check(driver.Poll(101) ==
                  OperatorFlowActions::kTimedOut &&
              g_confirmationCalls == 1,
          "lost 0931 becomes terminal without retry");
    Check(!driver.actions.ResetRun() &&
              driver.actions.State().result ==
                  OperatorFlowActions::
                      kMutationAttemptSealed &&
              g_confirmationCalls == 1,
          "ResetRun cannot reopen uncertain mutation");
}

void TestTransportAndReplyFailures()
{
    ResetFixture();
    Driver stale;
    Check(stale.Setup(),
          "stale reply fixture stages");
    std::array<uint8_t, 0x33> stale074D = {};
    std::memcpy(
        stale074D.data() + 6,
        &kProductId,
        sizeof(kProductId));
    std::memcpy(
        stale074D.data() + 10,
        &kOptionId,
        sizeof(kOptionId));
    Check(!stale.actions.ObserveShopReply(
              0x074D,
              stale074D.data(),
              static_cast<uint32_t>(
                  stale074D.size()),
              true),
          "074D before the sealed original 0931 is ignored");

    ResetFixture();
    g_0931Result = 0;
    Driver rejectedSend;
    Check(rejectedSend.Setup(),
          "rejected transport fixture stages");
    rejectedSend.Poll();
    rejectedSend.Poll();
    rejectedSend.Poll();
    rejectedSend.Poll();
    Check(rejectedSend.Poll() ==
              OperatorFlowActions::
                  kPreflightRequestRejected,
          "failed original 0931 transport is terminal");

    ResetFixture();
    Driver wrongIdentity;
    Check(wrongIdentity.Setup(),
          "wrong 074D fixture stages");
    wrongIdentity.ReachWait074D();
    wrongIdentity.Accept074D(
        kProductId + 1,
        kOptionId);
    Check(wrongIdentity.Poll() ==
              OperatorFlowActions::
                  kPreflightReplyRejected,
          "mismatched 074D echo cannot advance");

    ResetFixture();
    Driver early0574;
    Check(early0574.Setup(),
          "early 0574 fixture stages");
    early0574.ReachWait074D();
    early0574.Accept074D();
    early0574.Poll();
    std::array<uint8_t, 0xD4> stale0574 = {};
    Check(!early0574.actions.ObserveShopReply(
              0x0574,
              stale0574.data(),
              static_cast<uint32_t>(
                  stale0574.size()),
              true),
          "0574 before original automatic 0758 is ignored");

    ResetFixture();
    Driver serverReject;
    Check(serverReject.Setup(),
          "server rejection fixture stages");
    serverReject.ReachWait074D();
    std::array<uint8_t, 8> packet = {};
    const uint16_t status = 0x84A;
    std::memcpy(
        packet.data() + 6,
        &status,
        sizeof(status));
    serverReject.actions.ObserveShopReply(
        0x05D8,
        packet.data(),
        static_cast<uint32_t>(
            packet.size()),
        true);
    Check(serverReject.Poll() ==
                  OperatorFlowActions::
                      kServerRejected &&
              serverReject.actions.State().
                      rejected_reply_status ==
                  0x84A,
          "native shop error opcode and status are terminal");
}

void TestAutomaticCommitTimeout()
{
    ResetFixture();
    Driver driver;
    Check(driver.Setup(),
          "automatic commit timeout fixture stages");
    driver.ReachWait074D();
    driver.Accept074D();
    Check(driver.Poll() ==
              OperatorFlowActions::
                  kWaitingForAutomaticCommit,
          "074D alone does not fabricate final commit");
    Check(driver.Poll(101) ==
              OperatorFlowActions::kTimedOut,
          "missing original 0758 is terminal");
}

void ReachReadback(Driver& driver)
{
    driver.ReachWait074D();
    driver.Accept074D();
    driver.Poll();
    PushTransport(0x0758, 0x39, 1);
    driver.Poll();
    driver.Accept0574();
    driver.Poll();
}

void TestReadbackMustBeExact()
{
    ResetFixture();
    Driver walletMismatch;
    Check(walletMismatch.Setup(),
          "wallet mismatch fixture stages");
    ReachReadback(walletMismatch);
    AddPurchasedOperator();
    SetWallet(901, 500);
    Check(walletMismatch.Poll() ==
                  OperatorFlowActions::
                      kWaitingForNativeReadback &&
              walletMismatch.Poll(101) ==
              OperatorFlowActions::
                  kWalletMismatch,
          "wallet evidence waits for settlement then requires exact debit");

    ResetFixture();
    Driver duplicateAdditions;
    Check(duplicateAdditions.Setup(),
          "duplicate additions fixture stages");
    ReachReadback(duplicateAdditions);
    SetWallet(900, 500);
    AddPurchasedOperator(0x20000002AULL);
    OperatorFlowActions::OperatorRecord extra;
    extra.uid = 0x20000002BULL;
    extra.operator_id = 60048;
    extra.slot = 13;
    g_inventory.records.push_back(extra);
    Check(duplicateAdditions.Poll() ==
                  OperatorFlowActions::
                      kWaitingForNativeReadback &&
              duplicateAdditions.Poll(101) ==
              OperatorFlowActions::
                  kOperatorReadbackMismatch,
          "duplicate additions wait for settlement then refuse receipt");
}

void TestContextIdentity()
{
    ResetFixture();
    Driver driver;
    Check(driver.Setup(),
          "context identity fixture stages");
    driver.Poll();
    driver.context.run_id = "changed-run";
    Check(driver.Poll() ==
                  OperatorFlowActions::
                      kRunChanged &&
              g_optionsCalls == 0,
          "run identity change fails before UI callback");
}

void TestInventoryServiceReadback()
{
    std::array<uint8_t, 0x100> service = {};
    std::array<uint8_t, 0xF0> records = {};
    auto writeRecord =
        [&records](size_t index,
                   uint64_t uid,
                   uint32_t operatorId,
                   uint16_t slot,
                   uint8_t enabled) {
            uint8_t* record =
                records.data() + index * 0x50;
            std::memcpy(record, &uid, sizeof(uid));
            std::memcpy(
                record + 8,
                &operatorId,
                sizeof(operatorId));
            std::memcpy(
                record + 0x10,
                &slot,
                sizeof(slot));
            record[0x12] = enabled;
        };
    writeRecord(0, 0xAA, 60047, 1, 1);
    writeRecord(1, 0xBB, 60070, 2, 0);
    writeRecord(2, 0, 0, 3, 0);
    *reinterpret_cast<uint8_t**>(
        service.data() + 0xC0) =
        records.data();
    *reinterpret_cast<uint8_t**>(
        service.data() + 0xC4) =
        records.data() + records.size();
    OperatorFlowActions::InventoryImage image;
    Check(
        OperatorFlowActions::
                ReadOperatorInventoryService(
                    service.data(),
                    &image) &&
            image.records.size() == 2 &&
            image.records[0].uid == 0xAA &&
            image.records[0].operator_id ==
                60047 &&
            image.records[0].enabled == 1 &&
            image.records[1].slot == 2,
        "OpInv reader preserves occupied records and skips native empty slots");

    writeRecord(2, 0xCC, 0, 3, 0);
    Check(
        !OperatorFlowActions::
             ReadOperatorInventoryService(
                 service.data(),
                 &image),
        "partial OpInv identity is rejected");
    writeRecord(2, 0, 0, 3, 0);

    *reinterpret_cast<uint8_t**>(
        service.data() + 0xC4) =
        records.data() + records.size() - 1;
    Check(
        !OperatorFlowActions::
             ReadOperatorInventoryService(
                 service.data(),
                 &image),
        "misaligned OpInv vector is rejected");
}

void TestOperatorInventoryFacade()
{
    DestroyNativeString(
        g_operatorRoot + 0x118);
    DestroyNativeString(
        g_operatorList + 0x118);
    DestroyNativeString(
        g_operatorRightOne + 0x118);
    DestroyNativeString(
        g_operatorLeftOne + 0x118);
    DestroyNativeString(
        g_operatorActivate + 0x118);
    DestroyNativeString(
        g_operatorInputRoot + 0x118);
    DestroyNativeString(
        g_operatorInputEdit + 0x118);
    DestroyNativeString(
        g_operatorInputEdit + 0x200);
    DestroyNativeString(
        g_operatorInputButton + 0x118);
    std::memset(
        g_operatorInventory,
        0,
        sizeof(g_operatorInventory));
    std::memset(
        g_operatorRecords,
        0,
        sizeof(g_operatorRecords));
    std::memset(
        g_operatorReplyRecords,
        0,
        sizeof(g_operatorReplyRecords));
    std::memset(
        g_operatorRows,
        0,
        sizeof(g_operatorRows));
    std::memset(
        g_operatorRowControls,
        0,
        sizeof(g_operatorRowControls));
    std::memset(
        g_operatorInput,
        0,
        sizeof(g_operatorInput));
    std::memset(
        g_operatorVtable,
        0,
        sizeof(g_operatorVtable));
    std::memset(
        g_operatorInputVtable,
        0,
        sizeof(g_operatorInputVtable));
    std::memset(
        g_mainframeVtable,
        0,
        sizeof(g_mainframeVtable));

    g_operatorVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FacadeInventoryCallback);
    g_operatorInputVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FacadeInputCallback);
    g_operatorInputVtable[
        0x5C / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FacadeInputRoot);
    g_mainframeVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FacadeMainframeCallback);
    *reinterpret_cast<void**>(
        g_operatorInventory) =
        g_operatorVtable;
    *reinterpret_cast<void**>(
        g_operatorInput) =
        g_operatorInputVtable;

    InitializeControl(
        g_operatorRoot,
        nullptr,
        L"operator_root");
    InitializeControl(
        g_operatorList,
        g_operatorRoot,
        L"List");
    InitializeControl(
        g_operatorRightOne,
        g_operatorRoot,
        L"RightBtn");
    InitializeControl(
        g_operatorLeftOne,
        g_operatorRoot,
        L"LeftBtn");
    InitializeControl(
        g_operatorActivate,
        g_operatorRoot,
        L"OpSallyBtn");
    InitializeControl(
        g_operatorInputRoot,
        nullptr,
        L"dlg");
    InitializeControl(
        g_operatorInputEdit,
        g_operatorInputRoot,
        L"Edit");
    InitializeControl(
        g_operatorInputButton,
        g_operatorInputRoot,
        L"Btn2");
    WriteNativeString(
        g_operatorInputEdit + 0x200,
        L"");

    *reinterpret_cast<void**>(
        g_operatorInventory + 0x64) =
        g_operatorRoot;
    *reinterpret_cast<void**>(
        g_operatorInventory + 0xD4) =
        g_operatorActivate;
    *reinterpret_cast<int32_t*>(
        g_operatorInventory + 0xB8) = 0;
    *reinterpret_cast<int32_t*>(
        g_operatorInventory + 0xBC) = 2;
    *reinterpret_cast<uint8_t**>(
        g_operatorInventory + 0xC0) =
        g_operatorRecords;
    *reinterpret_cast<uint8_t**>(
        g_operatorInventory + 0xC4) =
        g_operatorRecords +
        sizeof(g_operatorRecords);

    constexpr uint64_t targetUid =
        0x700000000000002AULL;
    for (uint16_t slot = 1;
         slot <= 12;
         ++slot) {
        uint8_t* replyRecord =
            g_operatorReplyRecords +
            (slot - 1) * 0x50;
        uint8_t* record =
            g_operatorRecords +
            (slot - 1) * 0x50;
        const uint64_t uid =
            slot == 12
                ? targetUid
                : 0x6000000000000000ULL +
                      slot;
        const uint32_t operatorId =
            slot == 12
                ? kOperatorId
                : 50000 + slot;
        std::memcpy(
            replyRecord,
            &uid,
            sizeof(uid));
        std::memcpy(
            replyRecord + 8,
            &operatorId,
            sizeof(operatorId));
        std::memcpy(
            replyRecord + 0x10,
            &slot,
            sizeof(slot));
        replyRecord[0x12] =
            slot == 1 ? 1 : 0;
        replyRecord[0x4D] =
            slot == 12 ? 0 : 1;
        if (slot <= 6)
            std::memcpy(
                record,
                replyRecord,
                0x50);
        else
            std::memcpy(
                record + 0x10,
                &slot,
                sizeof(slot));

        *reinterpret_cast<void**>(
            g_operatorRows[
                slot - 1] +
            0x1C) =
            g_operatorRowControls[
                slot - 1];
        *reinterpret_cast<uint8_t**>(
            g_operatorRows[
                slot - 1] +
            0x2C) = record;
        const float x =
            100.0f + slot * 10.0f;
        const float y =
            200.0f + slot * 10.0f;
        std::memcpy(
            g_operatorRowControls[
                slot - 1] +
                0x148,
            &x,
            sizeof(x));
        std::memcpy(
            g_operatorRowControls[
                slot - 1] +
                0x14C,
            &y,
            sizeof(y));
    }

    *reinterpret_cast<uint32_t*>(g_operatorInput + 0x44) = 3;
    *reinterpret_cast<uint32_t*>(
        g_operatorInput + 0x4C) =
        kOperatorId;
    const uint32_t targetLow =
        static_cast<uint32_t>(targetUid);
    const uint32_t targetHigh =
        static_cast<uint32_t>(
            targetUid >> 32);
    *reinterpret_cast<uint32_t*>(
        g_operatorInput + 0x50) =
        targetLow;
    *reinterpret_cast<uint32_t*>(
        g_operatorInput + 0x54) =
        targetHigh;
    SetPopup(g_operatorInput);
    g_operatorPageCalls = 0;
    g_operatorActivationCalls = 0;
    g_operatorNamingCalls = 0;
    g_operatorPressedRow = -1;

    OperatorInventoryActions::Bindings bindings;
    bindings.profile_pointer = &g_profile;
    bindings.service_managed_pointer =
        &g_manager;
    bindings.service_find_by_name =
        reinterpret_cast<
            OperatorInventoryActions::
                ServiceFindByNameFn>(
            &FacadeServiceFind);
    bindings.native_wstring_ctor =
        reinterpret_cast<
            OperatorInventoryActions::
                NativeWStringCtorFn>(
            &FacadeNativeStringCtor);
    bindings.native_wstring_dtor =
        reinterpret_cast<
            OperatorInventoryActions::
                NativeWStringDtorFn>(
            &FacadeNativeStringDtor);
    bindings.native_ui_find_control =
        reinterpret_cast<
            OperatorInventoryActions::
                NativeUiFindControlFn>(
            &FacadeFindControl);
    bindings.edit_set_text =
        reinterpret_cast<
            OperatorInventoryActions::
                EditSetTextFn>(
            &FacadeEditSetText);
    bindings.edit_get_text =
        reinterpret_cast<
            OperatorInventoryActions::
                EditGetTextFn>(
            &FacadeEditGetText);
    bindings.mainframe_vtable =
        g_mainframeVtable;
    bindings.mainframe_callback =
        &FacadeMainframeCallback;
    bindings.inventory_vtable =
        g_operatorVtable;
    bindings.inventory_callback =
        &FacadeInventoryCallback;
    bindings.input_vtable =
        g_operatorInputVtable;
    bindings.input_callback =
        &FacadeInputCallback;
    bindings.list_mouse_business =
        reinterpret_cast<
            OperatorInventoryActions::
                ListMouseBusinessFn>(
            &FacadeListMouse);
    bindings.list_item_by_index =
        reinterpret_cast<
            OperatorInventoryActions::
                ListItemByIndexFn>(
            &FacadeListItemByIndex);

    HangarNavigation navigation;
    OperatorInventoryActions actions;
    Check(actions.Bind(
              bindings,
              &navigation),
          "operator inventory facade binds exact native callbacks");
    g_operatorActions = &actions;
    Check(actions.EnsureInventory() ==
              OperatorInventoryActions::
                  kReady,
          "existing OpInv is resolved and validated dynamically");
    OperatorFlowActions::InventoryImage image;
    bool inventoryReady = false;
    for (unsigned attempt = 0;
         attempt < 10 && !inventoryReady;
         ++attempt)
        inventoryReady =
            OperatorInventoryActions::
                ReadInventory(
                    &actions,
                    &image);
    Check(
        inventoryReady &&
            image.records.size() == 12 &&
            image.records.back().uid ==
                targetUid,
        "facade inventory reader requests every native page reply and exposes the complete global backing vector");
    const auto inventoryState = actions.State();
    Check(
        inventoryState.page_reply_sequence == 2 &&
            inventoryState.page_reply_page == 2 &&
            inventoryState.page_reply_size == 7 &&
            inventoryState.page_reply_count == 0 &&
            inventoryState.page_reply_empty == 1,
        "a native 059F count-zero size-seven reply completes the uniquely pending empty page without synthesizing records");

    *reinterpret_cast<int32_t*>(
        g_operatorInventory + 0xB8) = 0;
    g_operatorPageCalls = 0;

    Check(actions.SubmitNickname(
              kOperatorId,
              targetUid,
              L"Pilot47") ==
                  OperatorInventoryActions::
                      kNavigatingPage &&
              g_operatorPageCalls == 1,
          "naming navigates with original RightOneBtn callback");
    Check(actions.NamingPopupReady(kOperatorId, targetUid),
          "pending target naming popup is observed before submission");
    SetActivePopup(g_operatorInput);
    SetManagedList(0x54, 0x58, g_sentinel, g_node, g_options);
    Check(actions.NamingPopupReady(kOperatorId, targetUid),
          "active target naming popup remains visible alongside an unrelated pending service");
    Check(!actions.NamingPopupReady(kOperatorId, targetUid + 1),
          "active naming popup never accepts a different owned UID");
    *reinterpret_cast<void**>(g_options) = g_operatorInputVtable;
    *reinterpret_cast<uint32_t*>(g_options + 0x44) = 3;
    *reinterpret_cast<uint32_t*>(g_options + 0x4C) = kOperatorId + 1;
    *reinterpret_cast<uint32_t*>(g_options + 0x50) = static_cast<uint32_t>(targetUid + 1);
    *reinterpret_cast<uint32_t*>(g_options + 0x54) = static_cast<uint32_t>((targetUid + 1) >> 32);
    Check(actions.NamingPopupReady(kOperatorId, targetUid),
          "unrelated ready input service does not shadow the exact owned target");
    *reinterpret_cast<uint32_t*>(g_options + 0x4C) = kOperatorId;
    *reinterpret_cast<uint32_t*>(g_options + 0x50) = static_cast<uint32_t>(targetUid);
    *reinterpret_cast<uint32_t*>(g_options + 0x54) = static_cast<uint32_t>(targetUid >> 32);
    *reinterpret_cast<uint32_t*>(g_options + 0x44) = 4;
    Check(actions.NamingPopupReady(kOperatorId, targetUid),
          "closing duplicate target input is not actionable");
    *reinterpret_cast<uint32_t*>(g_options + 0x44) = 3;
    Check(!actions.NamingPopupReady(kOperatorId, targetUid),
          "two distinct ready dialogs for one owned UID remain ambiguous");
    SetManagedList(0x54, 0x58, g_sentinel, g_node, g_operatorInput);
    Check(actions.NamingPopupReady(kOperatorId, targetUid),
          "one popup moving between active and pending lists is not a duplicate mutation target");
    Check(actions.SubmitNickname(
              kOperatorId,
              targetUid,
              L"Pilot47") ==
                  OperatorInventoryActions::
                      kNamingSubmitted &&
              g_operatorNamingCalls == 1 &&
              std::wcscmp(
                  ReadNativeString(
                      g_operatorInputEdit +
                      0x200),
                  L"Pilot47") == 0,
          "automatic target popup uses native edit and original Btn2 once");
    Check(actions.SubmitNickname(
              kOperatorId,
              targetUid,
              L"Pilot47") ==
              OperatorInventoryActions::
                  kMutationAttemptSealed,
          "nickname callback cannot be retried");

    uint8_t* targetRecord =
        g_operatorRecords + 11 * 0x50;
    std::memcpy(
        targetRecord + 0x14,
        "Pilot47",
        sizeof("Pilot47"));
    targetRecord[0x4D] = 1;
    OperatorFlowActions::OperatorRecord state;
    std::string nickname;
    uint8_t setNameState = 0;
    Check(actions.ReadOperator(
              targetUid,
              &state,
              &nickname,
              &setNameState) &&
              state.operator_id ==
                  kOperatorId &&
              state.slot == 12 &&
              nickname == "Pilot47" &&
              setNameState == 1,
          "operator readback exposes exact UID identity nickname and set-name state");

    Check(actions.Activate(
              kOperatorId,
              targetUid) ==
                  OperatorInventoryActions::
                      kActivationSubmitted &&
              g_operatorActivationCalls == 1 &&
              actions.State().row_selected == 1,
          "activation uses genuine list down/up selection and original OpSallyBtn");
    Check(actions.Activate(
              kOperatorId,
              targetUid) ==
              OperatorInventoryActions::
                  kMutationAttemptSealed,
          "activation callback cannot be retried");

    for (uint16_t slot = 1;
         slot <= 12;
         ++slot)
        g_operatorRecords[
            (slot - 1) * 0x50 +
            0x12] = 0;
    targetRecord[0x12] = 1;
    std::memcpy(
        g_profileStorage + 0x242,
        targetRecord,
        0x50);
    g_profileStorage[0x242 + 0x12] = 0; // Native profile retains serialized old flag.
    std::memcpy(
        g_operatorReplyRecords,
        g_operatorRecords,
        sizeof(g_operatorReplyRecords));
    bool enabled = false;
    OperatorFlowActions::OperatorRecord active;
    Check(actions.ReadEnabled(
              targetUid,
              &enabled) &&
              enabled &&
              actions.ReadActiveOperator(
                  &active) &&
              active.uid == targetUid &&
              active.operator_id ==
                  kOperatorId &&
              active.slot == 12 &&
              active.enabled == 1,
          "active profile identity agrees with enabled inventory even when serialized profile flag is stale");

    *reinterpret_cast<int32_t*>(
        g_operatorInventory + 0xB8) = 0;
    for (int index = 0;
         index < 12;
         ++index)
        g_operatorRows[index][0x25] = 0;
    g_operatorPageCalls = 0;
    g_operatorActivationCalls = 0;
    OperatorInventoryActions activeActions;
    Check(activeActions.Bind(
              bindings,
              &navigation),
          "already-active facade fixture binds");
    g_operatorActions = &activeActions;
    OperatorFlowActions::InventoryImage
        activeImage;
    bool activeInventoryReady = false;
    for (unsigned attempt = 0;
         attempt < 6 &&
         !activeInventoryReady;
         ++attempt)
        activeInventoryReady =
            OperatorInventoryActions::
                ReadInventory(
                    &activeActions,
                    &activeImage);
    Check(
        activeInventoryReady &&
            activeImage.records.size() == 12,
        "already-active facade enumerates the same bounded native inventory before target navigation");
    *reinterpret_cast<int32_t*>(
        g_operatorInventory + 0xB8) = 0;
    g_operatorPageCalls = 0;
    Check(activeActions.Activate(
              kOperatorId,
              targetUid) ==
                  OperatorInventoryActions::
                      kNavigatingPage &&
              g_operatorPageCalls == 1,
          "already-active target still navigates to its native page");
    Check(activeActions.Activate(
              kOperatorId,
              targetUid) ==
                  OperatorInventoryActions::
                      kAlreadyActive &&
              activeActions.State().
                      row_selected == 1 &&
              activeActions.State().
                      selection_actions == 1 &&
              !activeActions.State().
                   activation_sealed &&
              g_operatorRows[11][0x25] == 1 &&
              g_operatorActivationCalls == 0,
          "already-active target is genuinely selected for display without mutation");
    g_operatorActions = nullptr;

    DestroyNativeString(
        g_operatorRoot + 0x118);
    DestroyNativeString(
        g_operatorList + 0x118);
    DestroyNativeString(
        g_operatorRightOne + 0x118);
    DestroyNativeString(
        g_operatorLeftOne + 0x118);
    DestroyNativeString(
        g_operatorActivate + 0x118);
    DestroyNativeString(
        g_operatorInputRoot + 0x118);
    DestroyNativeString(
        g_operatorInputEdit + 0x118);
    DestroyNativeString(
        g_operatorInputEdit + 0x200);
    DestroyNativeString(
        g_operatorInputButton + 0x118);
}

int RunSuite()
{
    TestSuccessfulPurchase();
    TestActiveConfirmationPurchase();
    TestConfirmationDiscoveryTelemetry();
    TestOptionsValidationRefusesMutation();
    TestConfirmationValidationRefusesMutation();
    TestConfirmationRootNameIsDiagnostic();
    TestControlsAreRevalidatedBeforeCallbacks();
    TestMutationIsNeverRetried();
    TestTransportAndReplyFailures();
    TestAutomaticCommitTimeout();
    TestReadbackMustBeExact();
    TestContextIdentity();
    TestInventoryServiceReadback();
    TestOperatorInventoryFacade();
    CleanupStrings();
    if (g_failures) {
        std::fprintf(
            stderr,
            "OPERATOR FLOW ACTIONS: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf(
        "OPERATOR FLOW ACTIONS OFFLINE CONTRACTS PASSED\n");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 &&
        std::strcmp(argv[1], "--suite") == 0)
        return RunSuite();
    std::fprintf(
        stderr,
        "usage: game_test_operator_flow_actions_test --suite\n");
    return 2;
}
