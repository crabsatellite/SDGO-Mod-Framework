// OFFLINE-ONLY ABI, identity, ownership, and transition tests for
// CapsuleDrawActions. The fixture models the shipped x86 activity-machine
// branch: derived dialog 0x00E8C67C, request 0x05D3, result 0x04E1 and
// acknowledgement 0x09C6. It never starts GOnline, contacts a server, writes a
// database, grants currency, or publishes an inventory item.

#include <windows.h>

#include <process.h>

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../mods/game_test_client/capsule_draw_actions.h"

namespace
{
constexpr uint32_t kMachineId = 990001;
constexpr uint32_t kPrice = 100000;
constexpr size_t kStride = 0xB3;
constexpr size_t kTargetIndex = 10;

using NetworkDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using CloneDetour =
    void*(__fastcall*)(void*, void*);
using ReplyDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);

int g_failures = 0;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x7000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_network[0x80] = {};
uint8_t g_stream = 0;
uint8_t g_managerObject[0x40] = {};
void* g_manager = g_managerObject;

uint8_t g_machine[0x300] = {};
uint8_t g_machineRoot[0x40] = {};
uint8_t g_buying[0x180] = {};
uint8_t g_buyingController[0xB0] = {};
uint8_t g_resultRoot[0x40] = {};
uint8_t g_dialogSource[0x100] = {};
uint8_t g_dialogLive[0x180] = {};
uint8_t g_dialogRoot[0x40] = {};
uint8_t g_dialogRecord[0xB3] = {};
uint8_t g_wrongService[0x300] = {};

uint8_t g_radio[0x180] = {};
uint8_t g_left[0x180] = {};
uint8_t g_right[0x180] = {};
uint8_t g_drop[0x180] = {};
uint8_t g_point[8][0x180] = {};
uint8_t g_event[8][0x180] = {};
uint8_t g_buy[0x180] = {};
uint8_t g_capsuleClick[0x180] = {};

void* g_machineVtable[32] = {};
void* g_dialogVtable[32] = {};
void* g_buyingVtable[32] = {};
void* g_controllerVtable[4] = {};
void* g_wrongVtable[32] = {};

std::vector<uint8_t> g_rows;
bool g_machineAvailable = true;
bool g_buyingAvailable = false;
bool g_delayDialogInit = false;
bool g_consumeAck = true;
bool g_removeBuyingOnClose = true;
bool g_wrongMachineService = false;
bool g_badCallbackAbi = false;
bool g_useDialogRecord = false;
bool g_dialogBuyReady = true;
bool g_emitTransport = true;
bool g_buyFault = false;
uint16_t g_emittedOpcode = 0x05D3;
int g_emittedSize = 10;
int g_networkResult = 1;

unsigned g_machineCallbacks = 0;
unsigned g_cloneCalls = 0;
unsigned g_buyCalls = 0;
unsigned g_networkCalls = 0;
unsigned g_replyCalls = 0;
unsigned g_resultCalls = 0;
uint16_t g_lastOpcode = 0;
uint32_t g_lastPacketMachine = 0;
void* g_ownedPacket = nullptr;
uint32_t g_replySequenceInsideOriginal = UINT32_MAX;

NetworkDetour g_networkDetour = nullptr;
CloneDetour g_cloneDetour = nullptr;
ReplyDetour g_replyDetour = nullptr;
CapsuleDrawActions* g_actions = nullptr;

void* ApiSymbol(SdgoMod*, const char*);
int ApiHookCode(
    SdgoMod*,
    const char*,
    void*,
    void**);

SdgoApi& TestApi()
{
    static SdgoApi api = [] {
        SdgoApi value = {};
        value.size = sizeof(value);
        value.version = SDGO_API_VERSION;
        value.Symbol = &ApiSymbol;
        value.HookCode = &ApiHookCode;
        return value;
    }();
    return api;
}

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

bool WriteNativeString(
    void* storage,
    const wchar_t* value)
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

const wchar_t* ReadNativeString(
    const void* storage)
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

void InitializeControl(
    uint8_t* control,
    void* root,
    const wchar_t* name,
    bool ready = true)
{
    std::memset(control, 0, 0x180);
    WriteNativeString(control + 0x118, name);
    *reinterpret_cast<void**>(
        control + 0x15C) = root;
    control[0x162] = ready ? 1 : 0;
    control[0x167] = ready ? 1 : 0;
}

void DestroyControls()
{
    DestroyNativeString(g_radio + 0x118);
    DestroyNativeString(g_left + 0x118);
    DestroyNativeString(g_right + 0x118);
    DestroyNativeString(g_drop + 0x118);
    DestroyNativeString(g_buy + 0x118);
    DestroyNativeString(g_capsuleClick + 0x118);
    for (unsigned index = 0; index < 8; ++index) {
        DestroyNativeString(
            g_point[index] + 0x118);
        DestroyNativeString(
            g_event[index] + 0x118);
    }
}

uint32_t ReadU32(const void* source)
{
    uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

void WriteU32(void* destination, uint32_t value)
{
    std::memcpy(
        destination,
        &value,
        sizeof(value));
}

uint8_t* TargetRow()
{
    return g_rows.data() +
        kTargetIndex * kStride;
}

void SetRow(
    size_t index,
    uint32_t machine,
    uint8_t currency,
    uint32_t price,
    bool event = false)
{
    auto* row =
        g_rows.data() + index * kStride;
    WriteU32(row, 145001 +
        static_cast<uint32_t>(index));
    WriteU32(row + 5, machine);
    row[9] = currency;
    row[10] = event ? 1 : 0;
    WriteU32(row + 15, price);
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeLoginPacketCtor(
    void* self,
    void*,
    const char*,
    const char*)
{
    return self;
}

char* __cdecl FakePasswordMd5Hex(
    const void*,
    char* output,
    int)
{
    if (output)
        std::memset(output, '0', 32);
    return output;
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void*,
    void*,
    void*,
    const void* packet,
    int size,
    char)
{
    ++g_networkCalls;
    if (packet && size >= 10) {
        g_lastOpcode =
            *static_cast<
                const uint16_t*>(packet);
        g_lastPacketMachine =
            ReadU32(
                static_cast<
                    const uint8_t*>(packet) +
                6);
    }
    return g_networkResult;
}

int __fastcall FakeLoginDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 0;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    return WriteNativeString(self, value)
        ? self
        : nullptr;
}

void __fastcall FakeNativeWStringDtor(
    void* self,
    void*)
{
    DestroyNativeString(self);
}

void* __fastcall FakeIdentityCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueue(void* object, int)
{
    return object;
}

void __fastcall FakeVoidThiscall(void*, void*)
{
}

void* __fastcall FakeServiceFind(
    void* manager,
    void*,
    const void* descriptor)
{
    if (manager != g_manager || !descriptor)
        return nullptr;
    const wchar_t* name =
        ReadNativeString(
            static_cast<
                const uint8_t*>(descriptor) +
            4);
    if (!name)
        return nullptr;
    const wchar_t* machineName =
        ReadNativeString(g_machine + 0x0C);
    if (machineName &&
        std::wcscmp(name, machineName) == 0) {
        if (!g_machineAvailable)
            return nullptr;
        return g_wrongMachineService
            ? static_cast<void*>(g_wrongService)
            : static_cast<void*>(g_machine);
    }
    const wchar_t* buyingName =
        ReadNativeString(g_buying + 0x0C);
    if (buyingName &&
        std::wcscmp(name, buyingName) == 0)
        return g_buyingAvailable
            ? static_cast<void*>(g_buying)
            : nullptr;
    return nullptr;
}

void* FindMachineControl(
    const wchar_t* name,
    uint32_t kind)
{
    if (kind == 0x0C &&
        std::wcscmp(
            name,
            L"MachineRadio2") == 0)
        return g_radio;
    if (kind == 4 &&
        std::wcscmp(
            name,
            L"CapsuleLeftBtn") == 0)
        return g_left;
    if (kind == 4 &&
        std::wcscmp(
            name,
            L"CapsuleRightBtn") == 0)
        return g_right;
    if (kind == 4 &&
        std::wcscmp(
            name,
            L"CapsuleDropBtn") == 0)
        return g_drop;
    for (unsigned index = 0;
         index < 8;
         ++index) {
        wchar_t point[32] = {};
        wchar_t event[32] = {};
        swprintf_s(
            point,
            L"PointMachineRadio%u",
            index);
        swprintf_s(
            event,
            L"EventMachineRadio%u",
            index);
        if (kind == 0x0C &&
            std::wcscmp(name, point) == 0)
            return g_point[index];
        if (kind == 0x0C &&
            std::wcscmp(name, event) == 0)
            return g_event[index];
    }
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (root == g_machineRoot)
        return FindMachineControl(
            name,
            kind);
    if (root == g_dialogRoot &&
        kind == 4 &&
        std::wcscmp(name, L"BuyBtn") == 0)
        return g_buy;
    if (root == g_resultRoot &&
        kind == 1 &&
        std::wcscmp(
            name,
            L"CapsuleClick") == 0)
        return g_capsuleClick;
    return nullptr;
}

void* __fastcall FakeFindNamedChild(
    void* self,
    void*,
    const void* nativeName)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (self == g_dialogLive &&
        std::wcscmp(name, L"f_buy_dlg") == 0)
        return g_dialogRoot;
    if (self == g_buying &&
        std::wcscmp(name, L"machine") == 0)
        return g_resultRoot;
    return nullptr;
}

void InitializeDialog()
{
    *reinterpret_cast<void**>(
        g_dialogLive) = g_dialogVtable;
    WriteU32(g_dialogLive + 0x50, kMachineId);
    WriteU32(g_dialogLive + 0x60, 3);
    *reinterpret_cast<void**>(
        g_dialogLive + 0x90) =
        g_useDialogRecord
            ? static_cast<void*>(
                  g_dialogRecord)
            : static_cast<void*>(
                  TargetRow());
    g_buy[0x162] =
        g_dialogBuyReady ? 1 : 0;
    g_buy[0x167] =
        g_dialogBuyReady ? 1 : 0;
}

void* __fastcall FakeActivityDialogClone(
    void* source,
    void*)
{
    ++g_cloneCalls;
    std::memset(
        g_dialogLive,
        0,
        sizeof(g_dialogLive));
    std::memcpy(
        g_dialogLive,
        source,
        0x94);
    *reinterpret_cast<void**>(
        g_dialogLive) = g_dialogVtable;
    if (!g_delayDialogInit)
        InitializeDialog();
    return g_dialogLive;
}

uint32_t __stdcall FakeMachineCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_machineCallbacks;
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (service != g_machine ||
        !name ||
        nativeName !=
            static_cast<uint8_t*>(
                control) +
                0x118) {
        g_badCallbackAbi = true;
        return 0;
    }
    if (event == 0x301 &&
        control == g_radio &&
        std::wcscmp(
            name,
            L"MachineRadio2") == 0) {
        WriteU32(g_machine + 0x1A0, 2);
        WriteU32(
            g_machine + 0x7C,
            ReadU32(g_machine + 0x1DC));
        WriteU32(g_machine + 0x78, 0);
        WriteU32(g_machine + 0x80, 0);
        WriteU32(g_machine + 0x84, 0);
    } else if (
        event == 0x101 &&
        control == g_right &&
        std::wcscmp(
            name,
            L"CapsuleRightBtn") == 0) {
        ++*reinterpret_cast<
            uint32_t*>(g_machine + 0x78);
    } else if (
        event == 0x101 &&
        control == g_left &&
        std::wcscmp(
            name,
            L"CapsuleLeftBtn") == 0) {
        --*reinterpret_cast<
            uint32_t*>(g_machine + 0x78);
    } else if (event == 0x301) {
        const unsigned slot =
            static_cast<unsigned>(
                name[17] - L'0');
        WriteU32(
            g_machine + 0x84,
            slot);
        WriteU32(
            g_machine + 0x80,
            ReadU32(g_machine + 0x78) *
                8 +
                slot);
    } else if (
        event == 0x101 &&
        control == g_drop &&
        std::wcscmp(
            name,
            L"CapsuleDropBtn") == 0) {
        std::memset(
            g_dialogSource,
            0,
            sizeof(g_dialogSource));
        *reinterpret_cast<void**>(
            g_dialogSource) =
            g_dialogVtable;
        WriteU32(
            g_dialogSource + 0x50,
            kMachineId);
        std::memset(
            g_buying,
            0,
            sizeof(g_buying));
        *reinterpret_cast<void**>(
            g_buying) = g_buyingVtable;
        WriteNativeString(
            g_buying + 0x0C,
            L"machine");
        WriteU32(
            g_buying + 0x70,
            kMachineId);
        *reinterpret_cast<void**>(
            g_buying + 0x44) =
            g_buyingController;
        g_buyingAvailable = true;
        if (g_cloneDetour)
            g_cloneDetour(
                g_dialogSource,
                nullptr);
    }
    return 0;
}

uint32_t __stdcall FakeActivityBuyAction(
    void* dialog,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_buyCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (dialog != g_dialogLive ||
        event != 0x101 ||
        control != g_buy ||
        nativeName != g_buy + 0x118 ||
        !name ||
        std::wcscmp(name, L"BuyBtn") != 0 ||
        ReadU32(g_dialogLive + 0x50) !=
            kMachineId ||
        ReadU32(g_dialogLive + 0x60) != 3) {
        g_badCallbackAbi = true;
        return 0;
    }
    if (g_buyFault)
        RaiseException(
            0xE00005D3,
            0,
            0,
            nullptr);
    uint8_t packet[10] = {};
    *reinterpret_cast<uint16_t*>(packet) =
        g_emittedOpcode;
    WriteU32(packet + 6, kMachineId);
    if (g_emitTransport &&
        g_networkDetour) {
        g_networkDetour(
            g_network,
            nullptr,
            &g_stream,
            packet,
            g_emittedSize,
            0);
    }
    g_buy[0x167] = 0;
    return 0;
}

int __fastcall FakeReplyDispatch(
    void* self,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    ++g_replyCalls;
    if (g_actions) {
        const auto before =
            g_actions->State();
        g_replySequenceInsideOriginal =
            (opcode & 0xFFFF) == 0x09C6
                ? before.acknowledgement_sequence
                : before.result_reply_sequence;
    }
    if (self == g_buying &&
        (opcode & 0xFFFF) == 0x09C6 &&
        packet &&
        g_consumeAck) {
        WriteU32(
            g_buying + 0x88,
            ReadU32(
                static_cast<uint8_t*>(
                    packet) +
                0x0E));
        WriteU32(
            g_buying + 0x8C,
            ReadU32(
                static_cast<uint8_t*>(
                    packet) +
                0x12));
        *reinterpret_cast<uint16_t*>(
            g_buying + 0x48) = 2;
    }
    if (packet == g_ownedPacket) {
        VirtualFree(
            g_ownedPacket,
            0,
            MEM_RELEASE);
        g_ownedPacket = nullptr;
    }
    return 83;
}

uint32_t __stdcall FakeResultCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_resultCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (service != g_buying ||
        event != 0x801 ||
        control != g_capsuleClick ||
        nativeName !=
            g_capsuleClick + 0x118 ||
        !name ||
        std::wcscmp(
            name,
            L"CapsuleClick") != 0) {
        g_badCallbackAbi = true;
        return 0;
    }
    *reinterpret_cast<uint16_t*>(
        g_buying + 0x48) = 4;
    if (g_removeBuyingOnClose)
        g_buyingAvailable = false;
    return 0;
}

void* LookupSymbol(const char* name)
{
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(
            &FakeGameApp);
    if (std::strcmp(
            name,
            "flow.login_packet_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginPacketCtor);
    if (std::strcmp(
            name,
            "flow.password_md5_hex") == 0)
        return reinterpret_cast<void*>(
            &FakePasswordMd5Hex);
    if (std::strcmp(
            name,
            "flow.network") == 0)
        return reinterpret_cast<void*>(
            &FakeNetwork);
    if (std::strcmp(
            name,
            "flow.network_send") == 0)
        return reinterpret_cast<void*>(
            &FakeNetworkSend);
    if (std::strcmp(
            name,
            "flow.login_service_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginDispatch);
    if (std::strcmp(
            name,
            "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(
            name,
            "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringCtor);
    if (std::strcmp(
            name,
            "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeIdentityCtor);
    if (std::strcmp(
            name,
            "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(
            &FakeEnqueue);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_dtor") == 0 ||
        std::strcmp(
            name,
            "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(
            &FakeVoidThiscall);
    if (std::strcmp(
            name,
            "flow.service_managed_pointer") == 0)
        return &g_manager;
    if (std::strcmp(
            name,
            "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(
            &FakeServiceFind);
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeUiFindControl);
    if (std::strcmp(
            name,
            "flow.capsule_machine_vtable") == 0)
        return g_machineVtable;
    if (std::strcmp(
            name,
            "flow.capsule_machine_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeMachineCallback);
    if (std::strcmp(
            name,
            "flow.capsule_activity_dialog_vtable") ==
        0)
        return g_dialogVtable;
    if (std::strcmp(
            name,
            "flow.capsule_activity_dialog_clone") ==
        0)
        return reinterpret_cast<void*>(
            &FakeActivityDialogClone);
    if (std::strcmp(
            name,
            "flow.capsule_activity_buy_action") == 0)
        return reinterpret_cast<void*>(
            &FakeActivityBuyAction);
    if (std::strcmp(
            name,
            "flow.capsule_reply_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeReplyDispatch);
    if (std::strcmp(
            name,
            "flow.capsule_buying_vtable") == 0)
        return g_buyingVtable;
    if (std::strcmp(
            name,
            "flow.capsule_result_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeResultCallback);
    return nullptr;
}

void* ApiSymbol(SdgoMod*, const char* name)
{
    return LookupSymbol(name);
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    void* target = LookupSymbol(name);
    if (!target || !detour || !original)
        return 0;
    *original = target;
    if (std::strcmp(
            name,
            "flow.network_send") == 0)
        g_networkDetour =
            reinterpret_cast<
                NetworkDetour>(detour);
    else if (std::strcmp(
                 name,
                 "flow.capsule_activity_dialog_clone") ==
             0)
        g_cloneDetour =
            reinterpret_cast<
                CloneDetour>(detour);
    else if (std::strcmp(
                 name,
                 "flow.capsule_reply_dispatch") == 0)
        g_replyDetour =
            reinterpret_cast<
                ReplyDetour>(detour);
    return 1;
}

void InitializeVtables()
{
    std::memset(
        g_machineVtable,
        0,
        sizeof(g_machineVtable));
    std::memset(
        g_dialogVtable,
        0,
        sizeof(g_dialogVtable));
    std::memset(
        g_buyingVtable,
        0,
        sizeof(g_buyingVtable));
    std::memset(
        g_controllerVtable,
        0,
        sizeof(g_controllerVtable));
    g_machineVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeMachineCallback);
    g_dialogVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeActivityBuyAction);
    g_dialogVtable[0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeActivityDialogClone);
    g_dialogVtable[0x5C / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeFindNamedChild);
    g_buyingVtable[0x28 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeReplyDispatch);
    g_buyingVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeResultCallback);
    g_buyingVtable[0x5C / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeFindNamedChild);
}

void ResetFixture()
{
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_network,
        0,
        sizeof(g_network));
    std::memset(
        g_machine,
        0,
        sizeof(g_machine));
    std::memset(
        g_buying,
        0,
        sizeof(g_buying));
    std::memset(
        g_buyingController,
        0,
        sizeof(g_buyingController));
    std::memset(
        g_dialogSource,
        0,
        sizeof(g_dialogSource));
    std::memset(
        g_dialogLive,
        0,
        sizeof(g_dialogLive));
    std::memset(
        g_dialogRecord,
        0,
        sizeof(g_dialogRecord));
    std::memset(
        g_wrongService,
        0,
        sizeof(g_wrongService));
    *reinterpret_cast<void**>(
        g_network + 0x14) = &g_stream;
    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 6;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0x138) = 9229;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0xBC) = 2;
    *reinterpret_cast<void**>(
        g_machine) = g_machineVtable;
    WriteNativeString(
        g_machine + 0x0C,
        L"roommgr");
    *reinterpret_cast<void**>(
        g_machine + 0x60) =
        g_machineRoot;
    *reinterpret_cast<void**>(
        g_wrongService) = g_wrongVtable;
    *reinterpret_cast<void**>(
        g_buyingController) =
        g_controllerVtable;

    g_rows.assign(16 * kStride, 0);
    for (size_t index = 0;
         index < 16;
         ++index)
        SetRow(
            index,
            800000 +
                static_cast<uint32_t>(
                    index),
            0,
            1000);
    SetRow(
        kTargetIndex,
        kMachineId,
        0,
        kPrice);
    *reinterpret_cast<void**>(
        g_machine + 0x1A4) =
        g_rows.data();
    *reinterpret_cast<void**>(
        g_machine + 0x1A8) =
        g_rows.data() +
        g_rows.size();
    *reinterpret_cast<void**>(
        g_machine + 0x1AC) =
        g_rows.data() +
        g_rows.size();
    // +0x7C describes the currently selected machine type. Before
    // MachineRadio2 is selected it deliberately differs from the activity
    // vector's authoritative +0x1DC page count.
    WriteU32(g_machine + 0x7C, 7);
    WriteU32(g_machine + 0x1DC, 2);

    InitializeControl(
        g_radio,
        g_machineRoot,
        L"MachineRadio2");
    InitializeControl(
        g_left,
        g_machineRoot,
        L"CapsuleLeftBtn");
    InitializeControl(
        g_right,
        g_machineRoot,
        L"CapsuleRightBtn");
    InitializeControl(
        g_drop,
        g_machineRoot,
        L"CapsuleDropBtn");
    for (unsigned index = 0;
         index < 8;
         ++index) {
        wchar_t point[32] = {};
        wchar_t event[32] = {};
        swprintf_s(
            point,
            L"PointMachineRadio%u",
            index);
        swprintf_s(
            event,
            L"EventMachineRadio%u",
            index);
        InitializeControl(
            g_point[index],
            g_machineRoot,
            point);
        InitializeControl(
            g_event[index],
            g_machineRoot,
            event);
        *reinterpret_cast<void**>(
            g_machine + 0x94 +
            index * sizeof(void*)) =
            g_point[index];
        *reinterpret_cast<void**>(
            g_machine + 0xB4 +
            index * sizeof(void*)) =
            g_event[index];
    }
    *reinterpret_cast<void**>(
        g_machine + 0xDC) = g_radio;
    InitializeControl(
        g_buy,
        g_dialogRoot,
        L"BuyBtn",
        false);
    InitializeControl(
        g_capsuleClick,
        g_resultRoot,
        L"CapsuleClick",
        false);

    g_machineAvailable = true;
    g_buyingAvailable = false;
    g_delayDialogInit = false;
    g_consumeAck = true;
    g_removeBuyingOnClose = true;
    g_wrongMachineService = false;
    g_badCallbackAbi = false;
    g_useDialogRecord = false;
    g_dialogBuyReady = true;
    g_emitTransport = true;
    g_buyFault = false;
    g_emittedOpcode = 0x05D3;
    g_emittedSize = 10;
    g_networkResult = 1;
    g_machineCallbacks = 0;
    g_cloneCalls = 0;
    g_buyCalls = 0;
    g_networkCalls = 0;
    g_replyCalls = 0;
    g_resultCalls = 0;
    g_lastOpcode = 0;
    g_lastPacketMachine = 0;
    g_ownedPacket = nullptr;
    g_replySequenceInsideOriginal =
        UINT32_MAX;
}

bool Bind(
    NativeActions& native,
    CapsuleDrawActions& actions)
{
    SdgoApi& api = TestApi();
    auto* mod =
        reinterpret_cast<SdgoMod*>(1);
    const NativeActions::Config config{
        "offline-user",
        "offline-password",
        "offline-room",
        1,
        1,
    };
    if (!native.Bind(
            &api,
            mod,
            config))
        return false;
    g_actions = &actions;
    return actions.Bind(
        &api,
        mod,
        &native);
}

CapsuleDrawActions::Result DrivePreview(
    CapsuleDrawActions& actions)
{
    CapsuleDrawActions::Result result =
        CapsuleDrawActions::kUnbound;
    for (unsigned tick = 0;
         tick < 16;
         ++tick) {
        result = actions.EnsurePreview();
        if (result ==
                CapsuleDrawActions::
                    kPreviewReady ||
            result >=
                CapsuleDrawActions::
                    kMachineServiceMismatch)
            return result;
    }
    return result;
}

uint8_t* AllocatePacket(size_t bytes)
{
    g_ownedPacket =
        VirtualAlloc(
            nullptr,
            bytes,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE);
    if (g_ownedPacket)
        std::memset(
            g_ownedPacket,
            0,
            bytes);
    return static_cast<uint8_t*>(
        g_ownedPacket);
}

bool SendResult(
    int opcode,
    uint16_t status,
    uint32_t category = 0,
    uint32_t amount = 0,
    uint32_t remainingGp = 0,
    uint8_t currency = 0,
    uint64_t dropBoxSerial =
        UINT64_C(0x0123456789ABCDEF))
{
    uint8_t* packet =
        AllocatePacket(0x40);
    if (!packet || !g_replyDetour)
        return false;
    *reinterpret_cast<uint16_t*>(
        packet + 6) = status;
    if ((opcode & 0xFFFF) == 0x09C6) {
        std::memcpy(
            packet + 6,
            &dropBoxSerial,
            sizeof(dropBoxSerial));
    }
    WriteU32(
        packet + 0x0E,
        category);
    WriteU32(
        packet + 0x12,
        amount);
    WriteU32(
        packet + 0x1E,
        remainingGp);
    packet[0x22] = currency;
    const int result =
        g_replyDetour(
            g_buying,
            nullptr,
            1,
            2,
            3,
            opcode,
            packet);
    return result == 83 &&
        g_ownedPacket == nullptr;
}

void RevealResultControl()
{
    g_capsuleClick[0x162] = 1;
    g_capsuleClick[0x167] = 1;
}

bool ReadFile(
    const char* path,
    std::vector<uint8_t>& output)
{
    output.clear();
    if (!path || !*path)
        return false;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 ||
        !file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long length = ftell(file);
    if (length <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    output.resize(
        static_cast<size_t>(length));
    const size_t read =
        fread(
            output.data(),
            1,
            output.size(),
            file);
    fclose(file);
    return read == output.size();
}

bool RvaToOffset(
    const std::vector<uint8_t>& image,
    uint32_t rva,
    size_t bytes,
    size_t& output)
{
    output = 0;
    if (image.size() <
        sizeof(IMAGE_DOS_HEADER))
        return false;
    const auto* dos =
        reinterpret_cast<
            const IMAGE_DOS_HEADER*>(
            image.data());
    if (dos->e_magic !=
            IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0)
        return false;
    const size_t ntOffset =
        static_cast<size_t>(
            dos->e_lfanew);
    if (ntOffset >
            image.size() ||
        image.size() - ntOffset <
            sizeof(IMAGE_NT_HEADERS32))
        return false;
    const auto* nt =
        reinterpret_cast<
            const IMAGE_NT_HEADERS32*>(
            image.data() + ntOffset);
    if (nt->Signature !=
            IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic !=
            IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    if (rva <
        nt->OptionalHeader.SizeOfHeaders) {
        if (static_cast<size_t>(rva) >
                image.size() ||
            bytes >
                image.size() -
                    static_cast<size_t>(
                        rva))
            return false;
        output = rva;
        return true;
    }
    const auto* section =
        IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0;
         index <
             nt->FileHeader.NumberOfSections;
         ++index) {
        const uint32_t span =
            section[index].
                    Misc.VirtualSize >
                section[index].
                    SizeOfRawData
                ? section[index].
                      Misc.VirtualSize
                : section[index].
                      SizeOfRawData;
        if (rva <
                section[index].
                    VirtualAddress ||
            rva -
                    section[index].
                        VirtualAddress >=
                span)
            continue;
        const size_t delta =
            static_cast<size_t>(
                rva -
                section[index].
                    VirtualAddress);
        if (delta >
                section[index].
                    SizeOfRawData ||
            bytes >
                section[index].
                    SizeOfRawData -
                    delta)
            return false;
        const size_t offset =
            static_cast<size_t>(
                section[index].
                    PointerToRawData) +
            delta;
        if (offset > image.size() ||
            bytes > image.size() - offset)
            return false;
        output = offset;
        return true;
    }
    return false;
}

bool MatchRva(
    const std::vector<uint8_t>& image,
    uint32_t rva,
    const uint8_t* expected,
    size_t expectedSize)
{
    size_t offset = 0;
    return expected && expectedSize &&
        RvaToOffset(
            image,
            rva,
            expectedSize,
            offset) &&
        std::memcmp(
            image.data() + offset,
            expected,
            expectedSize) == 0;
}

bool ReadRvaU32(
    const std::vector<uint8_t>& image,
    uint32_t rva,
    uint32_t& output)
{
    size_t offset = 0;
    output = 0;
    if (!RvaToOffset(
            image,
            rva,
            sizeof(output),
            offset))
        return false;
    std::memcpy(
        &output,
        image.data() + offset,
        sizeof(output));
    return true;
}

void TestNativeContract(const char* path)
{
    std::vector<uint8_t> image;
    Check(
        ReadFile(path, image),
        "supported GOnline image is available for read-only contract verification");
    if (image.empty())
        return;
    const auto* dos =
        reinterpret_cast<
            const IMAGE_DOS_HEADER*>(
            image.data());
    const auto* nt =
        reinterpret_cast<
            const IMAGE_NT_HEADERS32*>(
            image.data() +
            static_cast<size_t>(
                dos->e_lfanew));
    Check(
        nt->FileHeader.Machine ==
                IMAGE_FILE_MACHINE_I386 &&
            nt->FileHeader.TimeDateStamp ==
                0x54BE1568 &&
            nt->OptionalHeader.ImageBase ==
                0x00400000 &&
            nt->OptionalHeader.SizeOfImage ==
                0x00C97000,
        "native contract is tied to the supported x86 PE identity");

    const uint8_t roomManagerRegistration[] = {
        0x6A, 0x07, 0x66, 0x89, 0x4D,
        0xD4, 0x68, 0x40, 0x1F, 0xE8,
        0x00, 0x8D, 0x4D, 0xD4, 0xC7,
        0x45, 0xE8, 0x07, 0x00, 0x00,
        0x00, 0x89, 0x5D, 0xE4, 0xE8,
        0x15, 0x9C, 0xCF, 0xFF, 0x8D,
        0x8D, 0x44, 0xFD, 0xFF, 0xFF,
        0xC7, 0x45, 0xFC, 0x0F, 0x00,
        0x00, 0x00, 0xE8, 0x13, 0x4B,
        0x09, 0x00,
    };
    const uint8_t buyingSceneName[] = {
        0x6A, 0x01, 0x56, 0x33, 0xC0,
        0x68, 0xE0, 0x20, 0xE5, 0x00,
        0x8D, 0x4D, 0xD4, 0x89, 0x75,
        0xE8, 0x89, 0x5D, 0xE4, 0x66,
        0x89, 0x45, 0xD4, 0xE8, 0x6B,
        0xB5, 0xC5, 0xFF,
    };
    const uint8_t buyingCtorRegistration[] = {
        0x8B, 0x95, 0x80, 0xFE, 0xFF,
        0xFF, 0x8B, 0x85, 0x84, 0xFE,
        0xFF, 0xFF, 0x52, 0xB3, 0x05,
        0x50, 0x8D, 0x8D, 0x88, 0xFE,
        0xFF, 0xFF, 0x88, 0x5D, 0xFC,
        0xE8, 0x96, 0x5F, 0xFF, 0xFF,
        0x8D, 0x4D, 0xB4, 0x51, 0x50,
        0xC6, 0x45, 0xFC, 0x06, 0xE8,
        0xF8, 0x55, 0xF5, 0xFF,
    };
    Check(
        MatchRva(
            image,
            0x00308ABE,
            roomManagerRegistration,
            sizeof(roomManagerRegistration)) &&
        MatchRva(
            image,
            0x003A7169,
            buyingSceneName,
            sizeof(buyingSceneName)) &&
        MatchRva(
            image,
            0x003A71CC,
            buyingCtorRegistration,
            sizeof(buyingCtorRegistration)),
        "native registrations name the room manager roommgr and the transient buying service machine");

    const uint8_t controllerInstall[] = {
        0xE8, 0xFA, 0x7A, 0x34, 0x00,
        0x83, 0xC4, 0x04, 0x89, 0x85,
        0x2C, 0xFF, 0xFF, 0xFF, 0xC6,
        0x45, 0xFC, 0x0B, 0x3B, 0xC3,
        0x74, 0x09, 0x8B, 0xC8, 0xE8,
        0xA6, 0x02, 0xFE, 0xFF, 0xEB,
        0x02, 0x33, 0xC0, 0x57, 0x8B,
        0xC8, 0xC6, 0x45, 0xFC, 0x06,
        0x89, 0x47, 0x44, 0xE8, 0x43,
        0xF4, 0xFF, 0xFF, 0x8B, 0x4F,
        0x44, 0x8B, 0x11, 0x8B, 0x42,
        0x04, 0x53, 0xFF, 0xD0,
    };
    const uint8_t controllerParent[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45,
        0x08, 0x89, 0x41, 0x20,
    };
    const uint8_t buyingUpdate[] = {
        0x56, 0x8B, 0xF1, 0x8B, 0x4E,
        0x44, 0x8B, 0x01, 0x8B, 0x40,
        0x04, 0x8D, 0x56, 0x48, 0x52,
        0xFF, 0xD0,
    };
    const uint8_t resultRootLookup[] = {
        0x8B, 0x4E, 0x20, 0x8B, 0x11,
        0x8B, 0x52, 0x5C, 0x8D, 0x45,
        0xAC, 0x50, 0x89, 0x7D, 0xFC,
        0xFF, 0xD2,
    };
    Check(
        MatchRva(
            image,
            0x003ACD1D,
            controllerInstall,
            sizeof(controllerInstall)) &&
        MatchRva(
            image,
            0x003AC190,
            controllerParent,
            sizeof(controllerParent)) &&
        MatchRva(
            image,
            0x004AEC90,
            buyingUpdate,
            sizeof(buyingUpdate)) &&
        MatchRva(
            image,
            0x00390E71,
            resultRootLookup,
            sizeof(resultRootLookup)),
        "native buying service owns a controller at +0x44 while the controller resolves machine through its bound service");

    uint32_t machineCallback = 0;
    uint32_t dialogBuy = 0;
    uint32_t dialogClone = 0;
    uint32_t replyDispatch = 0;
    uint32_t resultCallback = 0;
    Check(
        ReadRvaU32(
            image,
            0x00A8D36C + 0x38,
            machineCallback) &&
            machineCallback == 0x007A9230 &&
        ReadRvaU32(
            image,
            0x00A8C67C + 0x38,
            dialogBuy) &&
            dialogBuy == 0x007A1260 &&
        ReadRvaU32(
            image,
            0x00A8C67C + 0x54,
            dialogClone) &&
            dialogClone == 0x007AC570 &&
        ReadRvaU32(
            image,
            0x00A8D2B4 + 0x28,
            replyDispatch) &&
            replyDispatch == 0x0078F930 &&
        ReadRvaU32(
            image,
            0x00A8D2B4 + 0x38,
            resultCallback) &&
            resultCallback == 0x00A01470,
        "shipped vtables bind the exact machine, activity, reply, and close callbacks");

    const uint8_t requestBytes[] = {
        0xB9, 0xD3, 0x05, 0x00, 0x00,
        0x66, 0x89, 0x8D, 0x7C, 0xFF,
        0xFF, 0xFF, 0x89, 0x9D, 0x7E,
        0xFF, 0xFF, 0xFF, 0x89, 0x45,
        0x82,
    };
    const uint8_t cloneBytes[] = {
        0xE8, 0x96, 0xF3, 0xFF, 0xFF,
        0xC7, 0x06, 0x7C, 0xC6, 0xE8,
        0x00, 0x8B, 0x87, 0x90, 0x00,
        0x00, 0x00, 0x89, 0x86, 0x90,
        0x00, 0x00, 0x00,
    };
    const uint8_t pageCountBytes[] = {
        0x89, 0x83, 0xDC, 0x01, 0x00,
        0x00,
    };
    const uint8_t ackStateBytes[] = {
        0x66, 0x89, 0x47, 0x48, 0x38,
        0x5E, 0x22,
    };
    const uint8_t ackGpBytes[] = {
        0x8B, 0x4E, 0x1E, 0x89, 0x88,
        0x8F, 0x01, 0x00, 0x00,
    };
    const uint8_t ackCategoryBytes[] = {
        0x8B, 0x4E, 0x0E, 0x89, 0x8F,
        0x88, 0x00, 0x00, 0x00,
    };
    const uint8_t ackAmountBytes[] = {
        0x89, 0x97, 0x8C,
        0x00, 0x00, 0x00,
    };
    Check(
        MatchRva(
            image,
            0x003A1395,
            requestBytes,
            sizeof(requestBytes)),
        "original Buy branch serializes opcode 0x05D3, four zero bytes, and MachineID");
    Check(
        MatchRva(
            image,
            0x003AC5B5,
            cloneBytes,
            sizeof(cloneBytes)),
        "original activity clone installs vtable E8C67C and copies record pointer +0x90");
    Check(
        MatchRva(
            image,
            0x003A6CE8,
            pageCountBytes,
            sizeof(pageCountBytes)),
        "original activity loader stores its page count at service +0x1DC");
    Check(
        MatchRva(
            image,
            0x00390409,
            ackStateBytes,
            sizeof(ackStateBytes)) &&
        MatchRva(
            image,
            0x00390417,
            ackGpBytes,
            sizeof(ackGpBytes)) &&
        MatchRva(
            image,
            0x003904BB,
            ackCategoryBytes,
            sizeof(ackCategoryBytes)) &&
        MatchRva(
            image,
            0x003904CD,
            ackAmountBytes,
            sizeof(ackAmountBytes)),
        "original 0x09C6 branch consumes currency +0x22, GP +0x1E, category +0x0E, and amount +0x12");

    const uint8_t machineReturn[] = {
        0x8B, 0xE5, 0x5D, 0xC2, 0x10,
        0x00,
    };
    const uint8_t buyReturn[] = {
        0x8B, 0xE5, 0x5D, 0xC2, 0x10,
        0x00,
    };
    const uint8_t cloneReturn[] = {
        0x8B, 0xE5, 0x5D, 0xC3,
    };
    const uint8_t replyReturn[] = {
        0x8B, 0xE5, 0x5D, 0xC2, 0x14,
        0x00,
    };
    const uint8_t closeReturn[] = {
        0x5D, 0xC2, 0x10, 0x00,
    };
    Check(
        MatchRva(
            image,
            0x003A938A,
            machineReturn,
            sizeof(machineReturn)) &&
        MatchRva(
            image,
            0x003A15A6,
            buyReturn,
            sizeof(buyReturn)) &&
        MatchRva(
            image,
            0x003AC5DB,
            cloneReturn,
            sizeof(cloneReturn)) &&
        MatchRva(
            image,
            0x00390566,
            replyReturn,
            sizeof(replyReturn)) &&
        MatchRva(
            image,
            0x006014A7,
            closeReturn,
            sizeof(closeReturn)),
        "machine and Buy callbacks ret16, clone ret0, reply ret20, and close ret16");
}

void TestBindMismatch()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    void* saved =
        g_dialogVtable[
            0x38 / sizeof(void*)];
    g_dialogVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeResultCallback);
    SdgoApi& api = TestApi();
    const NativeActions::Config config{
        "offline-user",
        "offline-password",
        "offline-room",
        1,
        1,
    };
    auto* mod =
        reinterpret_cast<SdgoMod*>(1);
    Check(
        native.Bind(
            &api,
            mod,
            config),
        "transport fixture bound");
    Check(
        !actions.Bind(
            &api,
            mod,
            &native),
        "activity dialog vtable mismatch rejects binding");
    g_dialogVtable[
        0x38 / sizeof(void*)] =
        saved;
}

void TestConfiguredTarget()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(actions.ConfigureTarget({7,13,0}),"explicit preview target is accepted before binding");
    Check(!actions.ConfigureTarget({0,13,0}) && !actions.ConfigureTarget({7,0,0}) &&
          !actions.ConfigureTarget({7,13,3}),"invalid target values cannot replace the accepted profile");
    Check(Bind(native,actions),"configured target uses the same native ABI bindings");
    Check(!actions.ConfigureTarget({kMachineId,kPrice,0}),"an armed native target cannot be replaced");
    const auto result=DrivePreview(actions);
    Check(result==CapsuleDrawActions::kTargetMachineMismatch && actions.State().target_machine==7 &&
          !actions.State().buy_attempted && g_networkCalls==0 && g_buyCalls==0,
          "a different native machine is rejected instead of falling back or purchasing");
}

void TestPreview()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        Bind(native, actions),
        "activity capsule ABI hooks bound");
    g_machineAvailable = false;
    Check(
        actions.EnsurePreview() ==
            CapsuleDrawActions::
                kWaitingForMachine,
        "preview waits for the real machine service");
    g_machineAvailable = true;
    g_delayDialogInit = true;
    CapsuleDrawActions::Result result =
        CapsuleDrawActions::kUnbound;
    for (unsigned tick = 0;
         tick < 8;
         ++tick)
        result = actions.EnsurePreview();
    Check(
        result ==
            CapsuleDrawActions::
                kWaitingForPreview &&
            g_cloneCalls == 1 &&
            g_networkCalls == 0 &&
            g_buyCalls == 0,
        "preview pages, selects, and opens without buying");
    const auto waiting = actions.State();
    Check(
        waiting.target_machine ==
                kMachineId &&
            waiting.target_price ==
                kPrice &&
            waiting.target_index ==
                kTargetIndex &&
            waiting.target_page == 1 &&
            waiting.target_slot == 2 &&
            waiting.activity_slots == 16,
        "dynamic activity scan found the unique collection row");
    InitializeDialog();
    Check(
        actions.EnsurePreview() ==
                CapsuleDrawActions::
                    kPreviewReady &&
            actions.State().preview_ready &&
            !g_badCallbackAbi &&
            g_networkCalls == 0,
        "derived activity dialog becomes ready only after native init");
    Check(
        !actions.Bind(
            nullptr,
            nullptr,
            nullptr),
        "one adapter instance cannot reset its purchase gate by rebinding");
}

void TestEventTile()
{
    ResetFixture();
    SetRow(
        kTargetIndex,
        kMachineId,
        0,
        kPrice,
        true);
    NativeActions native;
    CapsuleDrawActions actions;
    Check(Bind(native, actions), "event tile fixture bound");
    Check(
        DrivePreview(actions) ==
            CapsuleDrawActions::
                kPreviewReady,
        "nonzero native row flag selects EventMachineRadio");
    Check(
        !g_badCallbackAbi &&
            ReadU32(g_machine + 0x80) ==
                kTargetIndex,
        "event tile callback selected the expected absolute row");
}

void TestTargetMismatch(
    const char* scenario)
{
    ResetFixture();
    if (std::strcmp(
            scenario,
            "currency") == 0) {
        TargetRow()[9] = 2;
    } else if (std::strcmp(
                   scenario,
                   "price") == 0) {
        WriteU32(
            TargetRow() + 15,
            kPrice - 1);
    } else {
        SetRow(
            3,
            kMachineId,
            0,
            kPrice);
    }
    NativeActions native;
    CapsuleDrawActions actions;
    Check(Bind(native, actions), "mismatch fixture bound");
    Check(
        actions.EnsurePreview() ==
                CapsuleDrawActions::
                    kTargetMachineMismatch &&
            g_machineCallbacks == 0 &&
            g_networkCalls == 0,
        "wrong or duplicate collection row fails before native UI action");
}

void TestWrongService()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(Bind(native, actions), "wrong service fixture bound");
    g_wrongMachineService = true;
    Check(
        actions.EnsurePreview() ==
                CapsuleDrawActions::
                    kMachineServiceMismatch &&
            g_machineCallbacks == 0,
        "same-name service with another vtable fails closed");
}

void TestMalformedActivity(const char* scenario)
{
    ResetFixture();
    if (std::strcmp(
            scenario,
            "activity-pages") == 0) {
        WriteU32(g_machine + 0x1DC, 3);
    } else {
        *reinterpret_cast<void**>(
            g_machine + 0x1A8) =
            g_rows.data() +
            g_rows.size() -
            1;
    }
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        Bind(native, actions),
        "malformed activity fixture bound");
    Check(
        actions.EnsurePreview() ==
                CapsuleDrawActions::
                    kActivityDataMismatch &&
            g_machineCallbacks == 0 &&
            g_networkCalls == 0,
        "malformed vector bounds or activity page count fail before native UI actions");
}

void TestDialogBoundary(const char* scenario)
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    if (std::strcmp(
            scenario,
            "dialog-hidden") == 0) {
        g_dialogBuyReady = false;
        Check(
            Bind(native, actions),
            "hidden Buy fixture bound");
        Check(
            DrivePreview(actions) ==
                    CapsuleDrawActions::
                        kWaitingForPreview &&
                g_buyCalls == 0 &&
                g_networkCalls == 0,
            "hidden or disabled native Buy control remains non-purchasing");
        return;
    }

    Check(
        Bind(native, actions),
        "dialog identity fixture bound");
    g_delayDialogInit = true;
    CapsuleDrawActions::Result result =
        CapsuleDrawActions::kUnbound;
    for (unsigned tick = 0;
         tick < 12;
         ++tick) {
        result = actions.EnsurePreview();
        if (result ==
            CapsuleDrawActions::
                kWaitingForPreview)
            break;
    }
    Check(
        result ==
                CapsuleDrawActions::
                    kWaitingForPreview &&
            g_cloneCalls == 1,
        "activity clone can wait for its original delayed Init");
    std::memcpy(
        g_dialogRecord,
        TargetRow(),
        sizeof(g_dialogRecord));
    g_dialogRecord[9] = 2;
    g_useDialogRecord = true;
    InitializeDialog();
    Check(
        actions.EnsurePreview() ==
                CapsuleDrawActions::
                    kDialogMismatch &&
            g_buyCalls == 0 &&
            g_networkCalls == 0,
        "live dialog record must retain collection machine currency and price");
}

bool PreparePurchase(
    NativeActions& native,
    CapsuleDrawActions& actions)
{
    if (!Bind(native, actions))
        return false;
    return DrivePreview(actions) ==
        CapsuleDrawActions::kPreviewReady;
}

void TestTransportFailure(const char* scenario)
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions),
        "transport rejection fixture reached genuine activity preview");
    CapsuleDrawActions::Result expected =
        CapsuleDrawActions::
            kBuyTransportRejected;
    if (std::strcmp(
            scenario,
            "transport-none") == 0) {
        g_emitTransport = false;
    } else if (std::strcmp(
                   scenario,
                   "transport-opcode") == 0) {
        g_emittedOpcode = 0x0514;
    } else if (std::strcmp(
                   scenario,
                   "transport-size") == 0) {
        g_emittedSize = 9;
    } else if (std::strcmp(
                   scenario,
                   "transport-result") == 0) {
        g_networkResult = 0;
    } else {
        g_buyFault = true;
        expected =
            CapsuleDrawActions::kNativeFault;
    }
    Check(
        actions.DrawOnce() == expected &&
            actions.State().buy_attempted &&
            !actions.State().buy_submitted,
        "missing, malformed, rejected, or faulting native transport is not accepted");
    Check(
        actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuyAlreadyAttempted &&
            g_buyCalls == 1,
        "failed Buy callback cannot be retried by the same adapter");
}

void TestAccepted()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions),
        "accepted fixture reached genuine activity preview");
    Check(
        actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted &&
            g_buyCalls == 1 &&
            g_networkCalls == 1 &&
            g_lastOpcode == 0x05D3 &&
            g_lastPacketMachine ==
                kMachineId &&
            !g_badCallbackAbi,
        "explicit draw used original 0x05D3 ten-byte activity request");
    Check(
        actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuyAlreadyAttempted &&
            g_buyCalls == 1 &&
            g_networkCalls == 1,
        "one genuine draw cannot be submitted twice");

    uint8_t unrelated[8] = {};
    const int unrelatedResult =
        g_replyDetour(
            g_buying,
            nullptr,
            1,
            2,
            3,
            0x0907,
            unrelated);
    Check(
        unrelatedResult == 83 &&
            actions.State().
                acknowledgement_sequence ==
                0,
        "unrelated normal-capsule ACK is ignored");

    Check(
        SendResult(
            0x09C6,
            0,
            1104242,
            1,
            5600,
            0),
        "original activity ACK retained packet ownership and return");
    const auto accepted = actions.State();
    Check(
        g_replySequenceInsideOriginal == 0 &&
            accepted.result_reply_sequence ==
                0 &&
            accepted.
                acknowledgement_sequence ==
                1 &&
            accepted.acknowledgement_opcode ==
                0x09C6 &&
            accepted.dropbox_serial ==
                UINT64_C(
                    0x0123456789ABCDEF) &&
            accepted.reward_item_category ==
                1104242 &&
            accepted.reward_amount == 1 &&
            accepted.remaining_gp == 5600 &&
            actions.Poll() ==
                CapsuleDrawActions::
                    kWaitingForResult,
        "ACK-only 0x09C6 success requires native delivery identity and waits for the original reveal animation");
    RevealResultControl();
    Check(
        actions.Poll() ==
            CapsuleDrawActions::
                kResultReady,
        "visible CapsuleClick completes the native result-readiness gate while service state remains two");
    g_removeBuyingOnClose=false;
    Check(
        actions.CloseResult() ==
                CapsuleDrawActions::
                    kClosingResult &&
            g_resultCalls == 1 &&
            !g_badCallbackAbi,
        "visible CapsuleClick uses the shipped 0x0801 close callback");
    Check(
        actions.Poll() ==
            CapsuleDrawActions::
                kComplete,
        "the native close state completes while the persistent buying service remains registered");
}

void TestStatusReply()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions) &&
            actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted,
        "status-reply fixture submitted one genuine-form request");
    Check(
        SendResult(0x04E1, 0),
        "original status-reply handler retained packet ownership and return");
    const auto reply = actions.State();
    Check(
        g_replySequenceInsideOriginal == 0 &&
            reply.result_reply_sequence == 1 &&
            reply.result_reply_opcode ==
                0x04E1 &&
            reply.result_reply_status == 0 &&
            reply.acknowledgement_sequence ==
                0 &&
            reply.original_reply_completed &&
            actions.Poll() ==
                CapsuleDrawActions::
                    kReplyAccepted,
        "zero-status 0x04E1 remains an observed status reply and does not manufacture a success acknowledgement");
}

void TestRejected()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions) &&
            actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted,
        "rejection fixture submitted one genuine-form request");
    Check(
        SendResult(0x04E1, 0x34),
        "rejection packet stayed owned by original handler");
    Check(
        actions.Poll() ==
                CapsuleDrawActions::
                    kServerRejected &&
            actions.CloseResult() ==
                CapsuleDrawActions::
                    kWaitingForResult &&
            g_resultCalls == 0,
        "nonzero activity result cannot become a success or close action");
}

void TestUnconsumedAck()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions) &&
            actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted,
        "unconsumed fixture submitted one genuine-form request");
    g_consumeAck = false;
    Check(
        SendResult(
            0x09C6,
            0,
            1104242,
            1,
            5600,
            0),
        "unconsumed ACK remained original-owned");
    Check(
        actions.State().
                acknowledgement_sequence ==
                0 &&
            actions.Poll() ==
                CapsuleDrawActions::
                    kWaitingForReply,
        "packet capture alone is not acceptance without native state readback");
}

void TestWrongReplyIdentity()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions) &&
            actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted,
        "reply identity fixture submitted one genuine-form request");
    WriteU32(g_buying + 0x70, 9301);
    Check(
        SendResult(0x04E1, 0),
        "other activity-machine reply remained original-owned");
    Check(
        actions.State().
                result_reply_sequence ==
                0,
        "same handler reply for another machine is not correlated");
}

void TestInvalidAcknowledgement(
    const char* scenario)
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions) &&
            actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted,
        "invalid ACK fixture submitted one genuine-form request");

    uint32_t category = 1104242;
    uint32_t amount = 1;
    uint8_t currency = 0;
    uint64_t serial =
        UINT64_C(0x0123456789ABCDEF);
    if (std::strcmp(
            scenario,
            "ack-category") == 0)
        category = 1099999;
    else if (std::strcmp(
                 scenario,
                 "ack-amount") == 0)
        amount = 2;
    else if (std::strcmp(
                 scenario,
                 "ack-currency") == 0)
        currency = 2;
    else if (std::strcmp(
                 scenario,
                 "ack-negative") == 0)
        serial = UINT64_MAX;
    else
        serial = 0;

    Check(
        SendResult(
            0x09C6,
            0,
            category,
            amount,
            5600,
            currency,
            serial),
        "invalid ACK remained owned by the original reply handler");
    const auto state = actions.State();
    Check(
        state.acknowledgement_sequence ==
                0 &&
            state.dropbox_serial == 0 &&
            state.reward_item_category ==
                0 &&
            actions.Poll() ==
                CapsuleDrawActions::
                    kWaitingForReply,
        "delivery needs a positive drop-box serial, unit category, amount one, and GP currency");
}

void TestResultControlMismatch()
{
    ResetFixture();
    NativeActions native;
    CapsuleDrawActions actions;
    Check(
        PreparePurchase(native, actions) &&
            actions.DrawOnce() ==
                CapsuleDrawActions::
                    kBuySubmitted &&
            SendResult(
                0x09C6,
                0,
                1104242,
                1,
                5600,
                0),
        "result-control fixture reached native delivery acknowledgement");
    RevealResultControl();
    *reinterpret_cast<void**>(
        g_capsuleClick + 0x15C) =
        nullptr;
    Check(
        actions.Poll() ==
                CapsuleDrawActions::
                    kResultControlMismatch &&
            actions.CloseResult() ==
                CapsuleDrawActions::
                    kResultControlMismatch &&
            g_resultCalls == 0,
        "mismatched result control cannot invoke the native close callback");
}

void RunScenario(
    const char* scenario,
    const char* nativePath)
{
    InitializeVtables();
    ResetFixture();
    if (std::strcmp(
            scenario,
            "native-contract") == 0)
        TestNativeContract(nativePath);
    else if (std::strcmp(
            scenario,
            "bind") == 0)
        TestBindMismatch();
    else if (std::strcmp(
                 scenario,
                 "preview") == 0)
        TestPreview();
    else if (std::strcmp(scenario,"configured-target") == 0)
        TestConfiguredTarget();
    else if (std::strcmp(
                 scenario,
                 "event") == 0)
        TestEventTile();
    else if (std::strcmp(
                 scenario,
                 "currency") == 0 ||
             std::strcmp(
                 scenario,
                 "price") == 0 ||
             std::strcmp(
                 scenario,
                 "duplicate") == 0)
        TestTargetMismatch(scenario);
    else if (std::strcmp(
                 scenario,
                 "service") == 0)
        TestWrongService();
    else if (std::strcmp(
                 scenario,
                 "activity-pages") == 0 ||
             std::strcmp(
                 scenario,
                 "activity-vector") == 0)
        TestMalformedActivity(scenario);
    else if (std::strcmp(
                 scenario,
                 "dialog-record") == 0 ||
             std::strcmp(
                 scenario,
                 "dialog-hidden") == 0)
        TestDialogBoundary(scenario);
    else if (std::strncmp(
                 scenario,
                 "transport-",
                 10) == 0)
        TestTransportFailure(scenario);
    else if (std::strcmp(
                 scenario,
                 "accepted") == 0)
        TestAccepted();
    else if (std::strcmp(
                 scenario,
                  "status-reply") == 0)
        TestStatusReply();
    else if (std::strcmp(
                  scenario,
                 "rejected") == 0)
        TestRejected();
    else if (std::strcmp(
                 scenario,
                 "unconsumed") == 0)
        TestUnconsumedAck();
    else if (std::strcmp(
                 scenario,
                 "identity") == 0)
        TestWrongReplyIdentity();
    else if (std::strncmp(
                 scenario,
                 "ack-",
                 4) == 0)
        TestInvalidAcknowledgement(
            scenario);
    else if (std::strcmp(
                 scenario,
                 "result-control") == 0)
        TestResultControlMismatch();
    else
        Check(false, "known scenario");
    DestroyControls();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 &&
        std::strcmp(
            argv[1],
            "--suite") != 0) {
        RunScenario(
            argv[1],
            argc >= 3 ? argv[2] : nullptr);
        std::printf(
            "SCENARIO %s: %s\n",
            argv[1],
            g_failures ? "FAIL" : "PASS");
        return g_failures ? 1 : 0;
    }

    const char* nativePath =
        argc >= 3 &&
                std::strcmp(
                    argv[1],
                    "--suite") == 0
            ? argv[2]
            : std::getenv(
                  "SDGO_NATIVE_GONLINE");
    const char* scenarios[] = {
        "native-contract",
        "bind",
        "preview",
        "configured-target",
        "event",
        "currency",
        "price",
        "duplicate",
        "service",
        "activity-pages",
        "activity-vector",
        "dialog-record",
        "dialog-hidden",
        "transport-none",
        "transport-opcode",
        "transport-size",
        "transport-result",
        "transport-fault",
        "accepted",
        "status-reply",
        "rejected",
        "unconsumed",
        "identity",
        "ack-category",
        "ack-amount",
        "ack-currency",
        "ack-negative",
        "ack-serial",
        "result-control",
    };
    int failures = 0;
    for (const char* scenario : scenarios) {
        if (std::strcmp(
                scenario,
                "native-contract") == 0 &&
            (!nativePath || !*nativePath)) {
            std::printf(
                "SCENARIO native-contract: SKIP (pass --suite GOnline.exe or set SDGO_NATIVE_GONLINE)\n");
            continue;
        }
        const char* arguments[] = {
            argv[0],
            scenario,
            nativePath,
            nullptr,
        };
        const intptr_t result =
            _spawnv(
                _P_WAIT,
                argv[0],
                arguments);
        if (result != 0) {
            std::fprintf(
                stderr,
                "CHILD FAIL: %s (%lld)\n",
                scenario,
                static_cast<long long>(
                    result));
            ++failures;
        }
    }
    std::printf(
        "SUMMARY: %s (%d failed scenario%s)\n",
        failures ? "FAIL" : "PASS",
        failures,
        failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
