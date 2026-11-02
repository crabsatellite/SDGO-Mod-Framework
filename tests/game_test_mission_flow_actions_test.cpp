// OFFLINE ONLY.
//
// This fixture models the verified Win32 object layouts and invokes the
// production mission adapter through SdgoApi Symbol/HookCode stubs. It never
// launches GOnline, contacts a server, mutates a database, or claims gameplay
// evidence.

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/mission_flow_actions.h"

namespace
{
constexpr size_t kGroupStride = 0x764;
constexpr size_t kChannelStride = 0x1D;
constexpr uint16_t kNormalChannel = 7001;
constexpr uint16_t kMissionChannel = 7002;
constexpr uint32_t kRoomId = 0x12345678;

using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using ChoiceDetour =
    void(__fastcall*)(void*, void*);
using DispatchDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);

struct ServiceNode
{
    ServiceNode* next;
    uint32_t reserved;
    void* service;
};

static_assert(sizeof(void*) == 4,
              "mission ABI test must be compiled as Win32");
static_assert(offsetof(ServiceNode, service) == 8,
              "service-node fixture must match the shipped x86 list");

int g_failures = 0;

uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x6000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_groups[kGroupStride * 2] = {};
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;

uint8_t g_serviceManagerStorage[0x60] = {};
void* g_serviceManager = g_serviceManagerStorage;
ServiceNode g_head = {};
ServiceNode g_topNode = {};
ServiceNode g_lobbyNode = {};
ServiceNode g_dialogNode = {};

uint8_t g_topService[0x100] = {};
uint8_t g_otherTopService[0x100] = {};
uint8_t g_missionLobby[0x900] = {};
uint8_t g_createDialog[0x100] = {};
uint8_t g_topRoot[4] = {};
uint8_t g_lobbyRoot[4] = {};
uint8_t g_dialogRoot[4] = {};

void* g_topVtable[20] = {};
void* g_lobbyVtable[20] = {};
void* g_dialogVtable[20] = {};
void* g_waitOwnerVtable[12] = {};
void* g_managerVtable[12] = {};
void* g_mainVtable[12] = {};
void* g_commandVtable[12] = {};

std::array<uint8_t, 0x180> g_singleControl = {};
std::array<uint8_t, 0x180> g_multiControl = {};
std::array<uint8_t, 0x180> g_makeRoomControl = {};
std::array<uint8_t, 0x180> g_mapRightControl = {};
std::array<uint8_t, 0x180> g_titleControl = {};
std::array<uint8_t, 0x180> g_okControl = {};

uint8_t g_nativeRoom[0x20] = {};
uint8_t g_packedModel[4]{};
void* g_packedModelVtable[12]{};
uint8_t g_waitOwner[0x20] = {};
uint8_t g_manager[0x80] = {};
uint8_t g_mainState[0x20] = {};
uint8_t g_startCommand[0x20] = {};
uint8_t g_otherCommand[0x20] = {};
uint8_t* g_states[13] = {};

NetworkSendDetour g_networkSendDetour = nullptr;
ChoiceDetour g_choiceDetour = nullptr;
DispatchDetour g_createReplyDetour = nullptr;
DispatchDetour g_startReplyDetour = nullptr;

bool g_missionLobbyPresent = false;
bool g_dialogPresent = false;
bool g_joinWritesPending = true;
bool g_emitRoomDetails = true;
bool g_networkAcceptsCreate = true;
bool g_localIsHost = true;
bool g_hasStartState = false;
bool g_addStateSendsImmediately = false;
bool g_replyPacketReleased = false;
void* g_replyAllocation = nullptr;

unsigned g_topCallbackCalls = 0;
unsigned g_choiceFallbackCalls = 0;
unsigned g_joinCalls = 0;
unsigned g_lobbyCallbackCalls = 0;
unsigned g_createCallbackCalls = 0;
unsigned g_addStateCalls = 0;
unsigned g_networkOriginalCalls = 0;
uint16_t g_joinedChannel = 0;
bool g_callbackAbiValid = true;
bool g_addStateAbiValid = true;
wchar_t g_setTitle[32] = {};

uint32_t g_descriptorKeys[2] = {1001, 1002};
uint32_t g_descriptorIds[2] = {0x00400000, 0x00300000};
unsigned g_descriptorIndex = 0;

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
        *reinterpret_cast<const uint32_t*>(bytes + 0x14);
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
            (length + 1) *
                sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (length + 1) *
            sizeof(wchar_t);
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
        *reinterpret_cast<wchar_t**>(
            bytes) = allocation;
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) =
            static_cast<uint32_t>(
                length);
    }
    *reinterpret_cast<uint32_t*>(
        bytes + 0x10) =
        static_cast<uint32_t>(
            length);
    return true;
}

const wchar_t* ReadNativeString(
    const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(
            storage);
    const uint32_t capacity =
        *reinterpret_cast<
            const uint32_t*>(
            bytes + 0x14);
    return capacity < 8
        ? reinterpret_cast<
              const wchar_t*>(bytes)
        : *reinterpret_cast<
              wchar_t* const*>(bytes);
}

void InitializeControl(
    std::array<uint8_t, 0x180>& control,
    const wchar_t* name,
    void* parent)
{
    DestroyNativeString(
        control.data() + 0x118);
    std::memset(
        control.data(),
        0,
        control.size());
    WriteNativeString(
        control.data() + 0x118,
        name);
    *reinterpret_cast<void**>(
        control.data() + 0x15C) =
        parent;
    control[0x162] = 1;
    control[0x167] = 1;
}

void DestroyControls()
{
    DestroyNativeString(
        g_singleControl.data() + 0x118);
    DestroyNativeString(
        g_multiControl.data() + 0x118);
    DestroyNativeString(
        g_makeRoomControl.data() + 0x118);
    DestroyNativeString(
        g_mapRightControl.data() + 0x118);
    DestroyNativeString(
        g_titleControl.data() + 0x118);
    DestroyNativeString(
        g_okControl.data() + 0x118);
}

void RebuildServiceList()
{
    g_head.next = &g_topNode;
    g_head.reserved = 0;
    g_head.service = nullptr;
    g_topNode.service = g_topService;
    if (g_missionLobbyPresent) {
        g_topNode.next = &g_lobbyNode;
        g_lobbyNode.service =
            g_missionLobby;
        if (g_dialogPresent) {
            g_lobbyNode.next =
                &g_dialogNode;
            g_dialogNode.service =
                g_createDialog;
            g_dialogNode.next =
                &g_head;
        } else {
            g_lobbyNode.next =
                &g_head;
        }
    } else {
        g_topNode.next = &g_head;
    }
    *reinterpret_cast<ServiceNode**>(
        g_serviceManagerStorage +
        0x44) = &g_head;
}

uint8_t* Channel(
    unsigned group,
    unsigned index)
{
    return g_groups +
        group * kGroupStride +
        0x24 +
        index * kChannelStride;
}

void PutChannel(
    uint8_t* row,
    uint16_t id,
    uint32_t type,
    uint32_t capacity,
    uint32_t population,
    uint32_t category,
    bool enabled)
{
    std::memset(row, 0, kChannelStride);
    *reinterpret_cast<uint16_t*>(
        row) = id;
    *reinterpret_cast<uint32_t*>(
        row + 0x06) = type;
    *reinterpret_cast<uint32_t*>(
        row + 0x0A) = capacity;
    *reinterpret_cast<uint32_t*>(
        row + 0x0E) = population;
    *reinterpret_cast<uint32_t*>(
        row + 0x16) = category;
    row[0x1C] = enabled ? 1 : 0;
}

void SetSelectedChannel(
    uint32_t group,
    uint16_t channel)
{
    *reinterpret_cast<uint16_t*>(
        g_profileStorage + 0x5D0) =
        channel;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0x5D4) =
        group;
}

void SetDescriptor(unsigned index)
{
    g_descriptorIndex = index % 2;
    *reinterpret_cast<uint32_t*>(
        g_createDialog + 0x50) =
        g_descriptorKeys[
            g_descriptorIndex];
    *reinterpret_cast<uint32_t*>(
        g_createDialog + 0x54) =
        g_descriptorKeys[
            g_descriptorIndex];
    *reinterpret_cast<uint32_t*>(
        g_createDialog + 0x58) =
        g_descriptorIds[
            g_descriptorIndex];
}

void ApplyPendingDescriptor()
{
    const uint32_t pending =
        *reinterpret_cast<uint32_t*>(
            g_createDialog + 0x54);
    for (unsigned i = 0; i < 2; ++i) {
        if (g_descriptorKeys[i] ==
            pending) {
            SetDescriptor(i);
            return;
        }
    }
}

bool SendNativePacket(
    uint16_t opcode,
    uint32_t size)
{
    std::array<uint8_t, 0x60> packet = {};
    *reinterpret_cast<uint16_t*>(
        packet.data()) = opcode;
    if (!g_networkSendDetour)
        return false;
    return g_networkSendDetour(
               g_network,
               nullptr,
               &g_stream,
               packet.data(),
               static_cast<int>(size),
               0) != 0;
}

uint8_t* __fastcall FakeRoomInfo(uint8_t* self,void*)
{
    return self==g_packedModel ? g_nativeRoom : nullptr;
}

void InstallNativeRoom(
    uint8_t mode,
    uint32_t roomId)
{
    std::memset(
        g_nativeRoom,
        0,
        sizeof(g_nativeRoom));
    std::memset(
        g_waitOwner,
        0,
        sizeof(g_waitOwner));
    std::memset(
        g_manager,
        0,
        sizeof(g_manager));
    std::memset(
        g_mainState,
        0,
        sizeof(g_mainState));
    std::memset(
        g_startCommand,
        0,
        sizeof(g_startCommand));
    std::memset(
        g_states,
        0,
        sizeof(g_states));

    *reinterpret_cast<uint32_t*>(
        g_nativeRoom) = roomId;
    g_nativeRoom[5] = mode;
    *reinterpret_cast<void***>(g_packedModel)=g_packedModelVtable;
    g_packedModelVtable[7]=reinterpret_cast<void*>(&FakeRoomInfo);
    *reinterpret_cast<uint8_t**>(g_manager+0x38)=g_packedModel;
    *reinterpret_cast<void**>(
        g_profileStorage + 0xEC) =
        g_nativeRoom;

    *reinterpret_cast<void***>(
        g_waitOwner) =
        g_waitOwnerVtable;
    *reinterpret_cast<uint8_t**>(
        g_waitOwner + 4) =
        g_manager;
    *reinterpret_cast<void***>(
        g_manager) =
        g_managerVtable;
    *reinterpret_cast<uint8_t***>(
        g_manager + 0x0C) =
        g_states;
    *reinterpret_cast<void***>(
        g_mainState) =
        g_mainVtable;
    *reinterpret_cast<uint32_t*>(
        g_mainState + 8) = 2;
    *reinterpret_cast<uint8_t**>(
        g_mainState + 0x0C) =
        g_manager + 0x24;
    *reinterpret_cast<uint8_t**>(
        g_mainState + 0x10) =
        g_manager;
    *reinterpret_cast<void***>(
        g_startCommand) =
        g_commandVtable;
    *reinterpret_cast<uint32_t*>(
        g_startCommand + 8) = 2;
    *reinterpret_cast<uint8_t**>(
        g_startCommand + 0x0C) =
        g_manager + 0x24;
    *reinterpret_cast<uint8_t**>(
        g_startCommand + 0x10) =
        g_manager;
    *reinterpret_cast<void***>(
        g_otherCommand) =
        g_commandVtable;
    *reinterpret_cast<uint32_t*>(
        g_otherCommand + 8) = 2;
    *reinterpret_cast<uint8_t**>(
        g_otherCommand + 0x0C) =
        g_manager + 0x24;
    *reinterpret_cast<uint8_t**>(
        g_otherCommand + 0x10) =
        g_manager;
    g_states[0] = g_mainState;
    g_states[12] = g_startCommand;

    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 18;
    *reinterpret_cast<uint8_t**>(
        g_app + 0x4A4) =
        g_waitOwner;
}

void ResetFixture(
    MissionFlowActions& actions,
    unsigned initialDescriptor = 0)
{
    actions.Reset();
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_groups,
        0,
        sizeof(g_groups));
    std::memset(
        g_network,
        0,
        sizeof(g_network));
    std::memset(
        g_topService,
        0,
        sizeof(g_topService));
    std::memset(
        g_otherTopService,
        0,
        sizeof(g_otherTopService));
    std::memset(
        g_missionLobby,
        0,
        sizeof(g_missionLobby));
    std::memset(
        g_createDialog,
        0,
        sizeof(g_createDialog));

    g_profile = g_profileStorage;
    g_serviceManager =
        g_serviceManagerStorage;
    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 6;
    *reinterpret_cast<void**>(
        g_network + 0x14) =
        &g_stream;

    *reinterpret_cast<void***>(
        g_topService) = g_topVtable;
    *reinterpret_cast<void**>(
        g_topService + 0x44) =
        g_topRoot;
    *reinterpret_cast<void**>(
        g_topService + 0x54) =
        g_singleControl.data();
    *reinterpret_cast<uint32_t*>(
        g_topService + 0x6C) = 1;
    *reinterpret_cast<uint32_t*>(
        g_topService + 0x68) = 0;
    *reinterpret_cast<uint32_t*>(
        g_topService + 0x70) = 0;

    *reinterpret_cast<void***>(
        g_missionLobby) =
        g_lobbyVtable;
    *reinterpret_cast<void**>(
        g_missionLobby + 0x60) =
        g_lobbyRoot;
    g_missionLobby[0x64] = 0;

    *reinterpret_cast<void***>(
        g_createDialog) =
        g_dialogVtable;
    *reinterpret_cast<void**>(
        g_createDialog + 0x4C) =
        g_dialogRoot;
    SetDescriptor(initialDescriptor);

    InitializeControl(
        g_singleControl,
        L"SingleBtn",
        g_topRoot);
    InitializeControl(
        g_multiControl,
        L"MultiRadio",
        g_lobbyRoot);
    InitializeControl(
        g_makeRoomControl,
        L"MakeRoomBtn",
        g_lobbyRoot);
    InitializeControl(
        g_mapRightControl,
        L"MapRightBtn",
        g_dialogRoot);
    InitializeControl(
        g_titleControl,
        L"GameRoomNameEdit",
        g_dialogRoot);
    InitializeControl(
        g_okControl,
        L"SettingOkBtn",
        g_dialogRoot);

    *reinterpret_cast<uint8_t**>(
        g_profileStorage + 0x5B0) =
        g_groups;
    *reinterpret_cast<uint8_t**>(
        g_profileStorage + 0x5B4) =
        g_groups + sizeof(g_groups);
    *reinterpret_cast<uint16_t*>(
        g_groups + 0x22) = 1;
    *reinterpret_cast<uint16_t*>(
        g_groups +
        kGroupStride + 0x22) = 2;
    PutChannel(
        Channel(0, 0),
        6001,
        1,
        100,
        1,
        1,
        true);
    PutChannel(
        Channel(1, 0),
        kNormalChannel,
        1,
        100,
        1,
        1,
        true);
    PutChannel(
        Channel(1, 1),
        kMissionChannel,
        1,
        100,
        1,
        2,
        true);
    SetSelectedChannel(0, 6001);

    g_missionLobbyPresent = false;
    g_dialogPresent = false;
    RebuildServiceList();

    g_joinWritesPending = true;
    g_emitRoomDetails = true;
    g_networkAcceptsCreate = true;
    g_localIsHost = true;
    g_hasStartState = false;
    g_addStateSendsImmediately = false;
    g_replyPacketReleased = false;
    g_replyAllocation = nullptr;
    g_topCallbackCalls = 0;
    g_choiceFallbackCalls = 0;
    g_joinCalls = 0;
    g_lobbyCallbackCalls = 0;
    g_createCallbackCalls = 0;
    g_addStateCalls = 0;
    g_networkOriginalCalls = 0;
    g_joinedChannel = 0;
    g_callbackAbiValid = true;
    g_addStateAbiValid = true;
    g_setTitle[0] = L'\0';
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
    ++g_networkOriginalCalls;
    if (!packet || size <= 0)
        return 0;
    const uint16_t opcode =
        *reinterpret_cast<
            const uint16_t*>(packet);
    if (opcode == 0x0640 &&
        !g_networkAcceptsCreate)
        return 0;
    return 1;
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
    return WriteNativeString(
               self,
               value)
        ? self
        : nullptr;
}

void __fastcall FakeNativeWStringDtor(
    void* self,
    void*)
{
    DestroyNativeString(self);
}

void* __fastcall FakeNicknameDialogCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(
    void* object,
    int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(
    void*,
    void*)
{
}

void __fastcall FakeNicknameDialogConfirm(
    void*,
    void*)
{
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
    const bool valid =
        manager == g_serviceManager &&
        bytes &&
        *reinterpret_cast<
            const uint32_t*>(bytes) == 1 &&
        name;
    g_callbackAbiValid =
        g_callbackAbiValid && valid;
    if (!valid)
        return nullptr;
    return std::wcscmp(
               name,
               L"upmenu") == 0
        ? g_topService
        : nullptr;
}

void* ControlFor(
    void* root,
    const wchar_t* name,
    uint32_t kind)
{
    if (!root || !name)
        return nullptr;
    if (root == g_topRoot &&
        kind == 4 &&
        std::wcscmp(
            name,
            L"SingleBtn") == 0)
        return g_singleControl.data();
    if (root == g_lobbyRoot) {
        if (kind == 0x0C &&
            std::wcscmp(
                name,
                L"MultiRadio") == 0)
            return g_multiControl.data();
        if (kind == 4 &&
            std::wcscmp(
                name,
                L"MakeRoomBtn") == 0)
            return g_makeRoomControl.data();
    }
    if (root == g_dialogRoot) {
        if (kind == 4 &&
            std::wcscmp(
                name,
                L"MapRightBtn") == 0)
            return g_mapRightControl.data();
        if (kind == 8 &&
            std::wcscmp(
                name,
                L"GameRoomNameEdit") == 0)
            return g_titleControl.data();
        if (kind == 4 &&
            std::wcscmp(
                name,
                L"SettingOkBtn") == 0)
            return g_okControl.data();
    }
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    return ControlFor(
        root,
        ReadNativeString(nativeName),
        kind);
}

void __fastcall FakeEditSetText(
    void* self,
    void*,
    const void* nativeText,
    uint32_t zero)
{
    const wchar_t* value =
        ReadNativeString(nativeText);
    g_callbackAbiValid =
        g_callbackAbiValid &&
        self == g_titleControl.data() &&
        zero == 0 &&
        value;
    if (value)
        wcscpy_s(g_setTitle, value);
}

uint32_t __stdcall FakeTopCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_topCallbackCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    g_callbackAbiValid =
        g_callbackAbiValid &&
        service == g_topService &&
        event == 0x101 &&
        control ==
            g_singleControl.data() &&
        nativeName ==
            g_singleControl.data() +
            0x118 &&
        name &&
        std::wcscmp(
            name,
            L"SingleBtn") == 0;
    if (g_callbackAbiValid) {
        *reinterpret_cast<uint32_t*>(
            g_topService + 0x70) = 2;
        *reinterpret_cast<uint32_t*>(
            g_topService + 0x68) = 2;
    }
    return 71;
}

void __fastcall FakeChoiceOriginal(
    void*,
    void*)
{
    ++g_choiceFallbackCalls;
}

void __fastcall FakeJoinChannel(
    void* self,
    void*,
    uint16_t channel)
{
    ++g_joinCalls;
    g_joinedChannel = channel;
    if (self != g_topService)
        return;
    uint8_t packet[0x0E] = {};
    *reinterpret_cast<uint16_t*>(
        packet) = 0x04FA;
    *reinterpret_cast<uint16_t*>(
        packet + 0x0A) = channel;
    if (g_networkSendDetour) {
        g_networkSendDetour(
            g_network,
            nullptr,
            &g_stream,
            packet,
            sizeof(packet),
            0);
    }
    if (g_joinWritesPending) {
        *reinterpret_cast<uint32_t*>(
            g_topService + 0x68) = 4;
    }
}

uint32_t __stdcall FakeMissionLobbyCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_lobbyCallbackCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    g_callbackAbiValid =
        g_callbackAbiValid &&
        service == g_missionLobby &&
        nativeName ==
            static_cast<uint8_t*>(
                control) +
            0x118 &&
        name;
    if (!name)
        return 0;
    if (event == 0x301 &&
        control ==
            g_multiControl.data() &&
        std::wcscmp(
            name,
            L"MultiRadio") == 0) {
        g_missionLobby[0x64] = 3;
        return 81;
    }
    if (event == 0x101 &&
        control ==
            g_makeRoomControl.data() &&
        std::wcscmp(
            name,
            L"MakeRoomBtn") == 0) {
        g_dialogPresent = true;
        RebuildServiceList();
        return 82;
    }
    g_callbackAbiValid = false;
    return 0;
}

uint32_t __stdcall FakeCreateCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_createCallbackCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    g_callbackAbiValid =
        g_callbackAbiValid &&
        service == g_createDialog &&
        event == 0x101 &&
        nativeName ==
            static_cast<uint8_t*>(
                control) +
            0x118 &&
        name;
    if (!name)
        return 0;
    if (control ==
            g_mapRightControl.data() &&
        std::wcscmp(
            name,
            L"MapRightBtn") == 0) {
        const unsigned next =
            (g_descriptorIndex + 1) % 2;
        *reinterpret_cast<uint32_t*>(
            g_createDialog + 0x54) =
            g_descriptorKeys[next];
        return 91;
    }
    if (control ==
            g_okControl.data() &&
        std::wcscmp(
            name,
            L"SettingOkBtn") == 0) {
        std::array<uint8_t, 0x53> packet = {};
        *reinterpret_cast<uint16_t*>(
            packet.data()) = 0x0640;
        if (g_networkSendDetour) {
            g_networkSendDetour(
                g_network,
                nullptr,
                &g_stream,
                packet.data(),
                static_cast<int>(
                    packet.size()),
                0);
        }
        return 92;
    }
    g_callbackAbiValid = false;
    return 0;
}

void __fastcall FakeCreateUpdate(
    void*,
    void*)
{
    ApplyPendingDescriptor();
}

int __fastcall FakeMissionPacketDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 29;
}

int __fastcall FakeMissionReplyOriginal(
    void*,
    void*,
    int,
    int,
    int,
    int opcode,
    void*)
{
    if ((opcode & 0xFFFF) ==
            0x0A33 &&
        g_emitRoomDetails)
        SendNativePacket(0x0621, 10);
    if (g_replyAllocation) {
        g_replyPacketReleased =
            VirtualFree(
                g_replyAllocation,
                0,
                MEM_RELEASE) != 0;
        g_replyAllocation = nullptr;
    }
    return 37;
}

bool __fastcall FakeHasState(
    void* self,
    void*,
    int state,
    int extra)
{
    g_addStateAbiValid =
        g_addStateAbiValid &&
        self == g_manager &&
        state == 12 &&
        extra == 0;
    return g_hasStartState;
}

void __fastcall FakeAddState(
    void* self,
    void*,
    int state,
    void* payload,
    int queue,
    int extra)
{
    ++g_addStateCalls;
    g_addStateAbiValid =
        g_addStateAbiValid &&
        self == g_manager &&
        state == 12 &&
        payload == nullptr &&
        queue == 1 &&
        extra == 0;
    g_hasStartState = true;
    if (g_addStateSendsImmediately)
        SendNativePacket(0x08A7, 8);
}

bool __stdcall FakeIsRoomMaster(
    void* room)
{
    return room == g_nativeRoom &&
           g_localIsHost;
}

int __fastcall FakeStartReplyOriginal(
    void* self,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    if (self &&
        ((opcode & 0xFFFF) == 0x06C3 ||
         ((opcode & 0xFFFF)==0x07B5 && packet &&
          *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(packet)+6)!=0))) {
        auto* command =
            static_cast<uint8_t*>(self);
        uint32_t* stage =
            reinterpret_cast<uint32_t*>(
                command + 8);
        if (*stage == 2)
            ++*stage;
    }
    if (g_replyAllocation) {
        g_replyPacketReleased =
            VirtualFree(
                g_replyAllocation,
                0,
                MEM_RELEASE) != 0;
        g_replyAllocation = nullptr;
    }
    return 41;
}

void* LookupSymbol(const char* name)
{
    if(!std::strcmp(name,"flow.mission_room_info"))return reinterpret_cast<void*>(&FakeRoomInfo);
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
            &FakeNicknameDialogCtor);
    if (std::strcmp(
            name,
            "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(
            &FakeEnqueueUiObject);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogConfirm);
    if (std::strcmp(
            name,
            "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(
            name,
            "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(
            &FakeServiceFindByName);
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeUiFindControl);
    if (std::strcmp(
            name,
            "flow.native_edit_set_text") == 0)
        return reinterpret_cast<void*>(
            &FakeEditSetText);
    if (std::strcmp(
            name,
            "flow.lobby_ui_vtable") == 0)
        return g_topVtable;
    if (std::strcmp(
            name,
            "flow.lobby_ui_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeTopCallback);
    if (std::strcmp(
            name,
            "flow.lobby_channel_list_choice") == 0)
        return reinterpret_cast<void*>(
            &FakeChoiceOriginal);
    if (std::strcmp(
            name,
            "flow.lobby_join_channel_business") == 0)
        return reinterpret_cast<void*>(
            &FakeJoinChannel);
    if (std::strcmp(
            name,
            "flow.mission_lobby_vtable") == 0)
        return g_lobbyVtable;
    if (std::strcmp(
            name,
            "flow.mission_lobby_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeMissionLobbyCallback);
    if (std::strcmp(
            name,
            "flow.mission_lobby_packet_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeMissionPacketDispatch);
    if (std::strcmp(
            name,
            "flow.mission_lobby_reply_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeMissionReplyOriginal);
    if (std::strcmp(
            name,
            "flow.mission_create_vtable") == 0)
        return g_dialogVtable;
    if (std::strcmp(
            name,
            "flow.mission_create_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeCreateCallback);
    if (std::strcmp(
            name,
            "flow.mission_create_update") == 0)
        return reinterpret_cast<void*>(
            &FakeCreateUpdate);
    if (std::strcmp(
            name,
            "flow.three_d_wait_room_vtable") == 0)
        return g_waitOwnerVtable;
    if (std::strcmp(
            name,
            "flow.three_d_start_command_vtable") == 0)
        return g_commandVtable;
    if (std::strcmp(
            name,
            "flow.three_d_room_has_state") == 0)
        return reinterpret_cast<void*>(
            &FakeHasState);
    if (std::strcmp(
            name,
            "flow.three_d_room_add_state") == 0)
        return reinterpret_cast<void*>(
            &FakeAddState);
    if (std::strcmp(
            name,
            "flow.mission_3d_manager_vtable") == 0)
        return g_managerVtable;
    if (std::strcmp(
            name,
            "flow.mission_3d_main_vtable") == 0)
        return g_mainVtable;
    if (std::strcmp(
            name,
            "flow.mission_3d_start_reply_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeStartReplyOriginal);
    if (std::strcmp(
            name,
            "flow.mission_is_room_master") == 0)
        return reinterpret_cast<void*>(
            &FakeIsRoomMaster);
    return nullptr;
}

void* ApiSymbol(
    SdgoMod*,
    const char* name)
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
            "flow.network_send") == 0) {
        g_networkSendDetour =
            reinterpret_cast<
                NetworkSendDetour>(
                detour);
    } else if (std::strcmp(
                   name,
                   "flow.lobby_channel_list_choice") ==
               0) {
        g_choiceDetour =
            reinterpret_cast<
                ChoiceDetour>(detour);
    } else if (std::strcmp(
                   name,
                   "flow.mission_lobby_reply_dispatch") ==
               0) {
        g_createReplyDetour =
            reinterpret_cast<
                DispatchDetour>(detour);
    } else if (std::strcmp(
                   name,
                   "flow.mission_3d_start_reply_dispatch") ==
               0) {
        g_startReplyDetour =
            reinterpret_cast<
                DispatchDetour>(detour);
    }
    return 1;
}

uint8_t* AllocateGuardedReply(
    uint16_t opcode,
    uint32_t roomId)
{
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    const size_t page =
        info.dwPageSize;
    auto* allocation =
        static_cast<uint8_t*>(
            VirtualAlloc(
                nullptr,
                page * 2,
                MEM_RESERVE |
                    MEM_COMMIT,
                PAGE_READWRITE));
    if (!allocation)
        return nullptr;
    DWORD oldProtect = 0;
    if (!VirtualProtect(
            allocation + page,
            page,
            PAGE_NOACCESS,
            &oldProtect)) {
        VirtualFree(
            allocation,
            0,
            MEM_RELEASE);
        return nullptr;
    }
    uint8_t* packet =
        allocation + page - 10;
    std::memset(packet, 0, 10);
    *reinterpret_cast<uint16_t*>(
        packet) = opcode;
    *reinterpret_cast<uint32_t*>(
        packet + 6) = roomId;
    g_replyAllocation = allocation;
    g_replyPacketReleased = false;
    return packet;
}

bool EnterMissionLobby(
    MissionFlowActions& actions,
    uint16_t mode)
{
    if (actions.EnsureRoom(
            mode,
            "mission-ai") !=
        MissionFlowActions::
            kEnteringMissionChannel)
        return false;
    if (!g_choiceDetour)
        return false;
    g_choiceDetour(
        g_topService,
        nullptr);
    SetSelectedChannel(
        1,
        kMissionChannel);
    g_missionLobbyPresent = true;
    RebuildServiceList();
    return true;
}

bool OpenCreateDialog(
    MissionFlowActions& actions,
    uint16_t mode)
{
    if (!EnterMissionLobby(
            actions,
            mode))
        return false;
    if (actions.EnsureRoom(
            mode,
            "mission-ai") !=
        MissionFlowActions::
            kSelectingMultiplayer)
        return false;
    return actions.EnsureRoom(
               mode,
               "mission-ai") ==
           MissionFlowActions::
               kOpeningCreateDialog;
}

bool SubmitMatchingMission(
    MissionFlowActions& actions,
    uint16_t mode)
{
    if (!OpenCreateDialog(
            actions,
            mode))
        return false;
    MissionFlowActions::Result result =
        actions.EnsureRoom(
            mode,
            "mission-ai");
    if (result ==
        MissionFlowActions::
            kCyclingMission) {
        ApplyPendingDescriptor();
        result = actions.EnsureRoom(
            mode,
            "mission-ai");
    }
    if (result !=
        MissionFlowActions::
            kSettingRoomTitle)
        return false;
    return actions.EnsureRoom(
               mode,
               "mission-ai") ==
           MissionFlowActions::
               kCreateSubmitted;
}

void TestBoundaryAndTopEntry(
    MissionFlowActions& actions)
{
    ResetFixture(actions);
    Check(
        actions.EnsureRoom(
            10,
            "mission-ai") ==
            MissionFlowActions::
                kInvalidMode &&
            actions.EnsureRoom(
                11,
                "mission-ai") ==
                MissionFlowActions::
                    kInvalidMode &&
            actions.EnsureRoom(
                12,
                "mission-ai") ==
                MissionFlowActions::
                    kInvalidMode,
        "single mission and native AI modes cannot masquerade as modes 5/8");
    Check(
        actions.EnsureRoom(
            5,
            "") ==
            MissionFlowActions::
                kInvalidTitle &&
            actions.EnsureRoom(
                5,
                "123456789012345678901") ==
                MissionFlowActions::
                    kInvalidTitle,
        "room title validation fails before any native callback");

    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    Check(
        g_choiceFallbackCalls == 1,
        "unarmed channel chooser forwards unchanged to the original");

    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kEnteringMissionChannel,
        "mode 5 enters through the original top-menu SingleBtn callback");
    const auto state =
        actions.PollSnapshot();
    Check(
        g_topCallbackCalls == 1 &&
            g_callbackAbiValid &&
            state.channel_choice_armed &&
            !state.channel_list_ready &&
            !state.channel_scan_valid &&
            state.channel_scan_status ==
                MissionFlowActions::
                    kChannelScanWaitingForReply &&
            state.channel_wait_polls == 0 &&
            *reinterpret_cast<
                uint32_t*>(
                g_topService + 0x70) == 2 &&
            *reinterpret_cast<
                uint32_t*>(
                g_topService + 0x68) == 2,
        "top callback used stdcall4 ABI and initialized native mission page state");
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kEnteringMissionChannel &&
            g_topCallbackCalls == 1,
        "pending channel discovery is one-shot and never re-clicks SingleBtn");

    *reinterpret_cast<void***>(
        g_otherTopService) = g_topVtable;
    *reinterpret_cast<uint32_t*>(
        g_otherTopService + 0x68) = 2;
    *reinterpret_cast<uint32_t*>(
        g_otherTopService + 0x70) = 2;
    if (g_choiceDetour)
        g_choiceDetour(
            g_otherTopService,
            nullptr);
    Check(
        g_choiceFallbackCalls == 2 &&
            g_joinCalls == 0 &&
            actions.PollSnapshot().
                channel_choice_armed,
        "same-vtable channel callback for another top owner is forwarded without consuming this run");

    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    const auto requested =
        actions.PollSnapshot();
    Check(
        g_joinCalls == 1 &&
            g_joinedChannel ==
                kMissionChannel &&
            requested.target_channel ==
                kMissionChannel &&
            requested.target_channel_group ==
                1 &&
            requested.channel_request_sent &&
            requested.channel_list_ready &&
            requested.channel_scan_valid &&
            requested.channel_choice_callbacks == 1 &&
            requested.channel_page_mode == 2 &&
            requested.channel_request_state == 2 &&
            requested.advertised_group_count == 2 &&
            requested.advertised_raw_count == 2 &&
            requested.advertised_mode2_count == 1 &&
            requested.advertised_eligible_count == 1 &&
            requested.rejected_non_mode2_count == 1,
        "gated chooser used the native Mode field and published ready-list diagnostics");
    Check(
        g_networkOriginalCalls == 1 &&
            *reinterpret_cast<
                uint32_t*>(
                g_topService + 0x68) == 4,
        "original JoinChannel built 0x04FA and entered native pending state");
}

void TestCreationDialogHandoff(MissionFlowActions& actions)
{
    ResetFixture(actions, 0);
    Check(OpenCreateDialog(actions, 5), "native mission admission opens the dialog before typed-input handoff");
    Check(actions.EnsureRoom(5, "mission-ai", false) == MissionFlowActions::kCyclingMission,
          "typed-input preparation still selects the requested real mission mode");
    ApplyPendingDescriptor();
    Check(actions.EnsureRoom(5, "mission-ai", false) == MissionFlowActions::kCreateDialogReady &&
              g_setTitle[0] == L'\0' && !actions.PollSnapshot().create_submitted,
          "dialog handoff neither assigns the title nor submits a request before keyboard tests");
    Check(actions.EnsureRoom(5, "mission-ai", false) == MissionFlowActions::kCreateDialogReady &&
              g_setTitle[0] == L'\0',
          "repeated preparation keeps the original mission dialog ready for real character input");
    Check(actions.EnsureRoom(5, "mission-ai") == MissionFlowActions::kSettingRoomTitle,
          "the existing complete mission flow retains its default submission behavior");
}

void TestCompleteMode5Flow(
    MissionFlowActions& actions)
{
    ResetFixture(actions, 0);
    Check(
        OpenCreateDialog(
            actions,
            5),
        "real channel and Mission Lobby callbacks opened the create dialog");
    Check(
        g_lobbyCallbackCalls == 2 &&
            g_missionLobby[0x64] == 3 &&
            g_dialogPresent,
        "MultiRadio and MakeRoom each ran once through stdcall4 ABI");

    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kCyclingMission,
        "wrong-mode listed mission advances through MapRightBtn");
    Check(
        *reinterpret_cast<uint32_t*>(
            g_createDialog + 0x50) ==
                g_descriptorKeys[0] &&
            *reinterpret_cast<uint32_t*>(
                g_createDialog + 0x54) ==
                g_descriptorKeys[1],
        "adapter waits for native Update to commit pending descriptor");
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kWaitingForMissionRefresh,
        "pending mission key cannot be submitted before native refresh");
    ApplyPendingDescriptor();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kSettingRoomTitle &&
            std::wcscmp(
                g_setTitle,
                L"mission-ai") == 0,
        "matching actual descriptor receives title through native edit setter");
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kCreateSubmitted,
        "SettingOk generated the native 0x0640/0x53 transport");
    auto submitted =
        actions.PollSnapshot();
    Check(
        submitted.create_submitted &&
            submitted.create_dialog_observed &&
            submitted.create_opcode ==
                0x0640 &&
            submitted.selected_mission_id ==
                g_descriptorIds[1] &&
            g_createCallbackCalls == 2,
        "submission observation retains the actual listed mission identity");

    uint8_t* reply =
        AllocateGuardedReply(
            0x0A33,
            kRoomId);
    Check(
        reply != nullptr,
        "guarded 0x0A33 packet fixture was allocated");
    if (!reply ||
        !g_createReplyDetour)
        return;
    const int handled =
        g_createReplyDetour(
            g_topService,
            nullptr,
            1,
            2,
            3,
            0x0A33,
            reply);
    Check(
        handled == 37 &&
            !actions.PollSnapshot().
                create_reply_consumed,
        "reply for a different native owner is forwarded but cannot satisfy the scenario");
    if (g_replyAllocation) {
        VirtualFree(
            g_replyAllocation,
            0,
            MEM_RELEASE);
        g_replyAllocation = nullptr;
    }

    reply = AllocateGuardedReply(
        0x0A33,
        kRoomId);
    Check(
        reply != nullptr,
        "owned guarded 0x0A33 fixture was allocated");
    if (!reply)
        return;
    const uint32_t sequenceBefore =
        actions.PollSnapshot().
            create_reply_sequence;
    const int ownedHandled =
        g_createReplyDetour(
            g_missionLobby,
            nullptr,
            1,
            2,
            3,
            0x0A33,
            reply);
    const auto consumed =
        actions.PollSnapshot();
    Check(
        ownedHandled == 37 &&
            g_replyPacketReleased &&
            consumed.create_reply_sequence ==
                sequenceBefore + 1 &&
            consumed.create_reply_consumed &&
            consumed.requested_room_id ==
                kRoomId,
        "room id was copied before original ownership release and published after native 0x0621");

    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kWaitingForNativeRoom,
        "0x0A33 transport is not mistaken for a constructed 3D room");
    InstallNativeRoom(8, kRoomId);
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kNativeRoomMismatch,
        "live room must preserve the requested mode and genuine room id");
    InstallNativeRoom(5, kRoomId);
    *reinterpret_cast<uint32_t*>(g_nativeRoom)=0;g_nativeRoom[5]=0;
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kWaitingForNativeRoom,
        "native state construction can precede the actual packed room publication without becoming an admission error");
    *reinterpret_cast<uint32_t*>(g_nativeRoom)=kRoomId;g_nativeRoom[5]=5;
    g_localIsHost = false;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kLocalPilotNotHost,
        "original room model must identify the local pilot as host");
    g_localIsHost = true;
    *reinterpret_cast<void**>(g_profileStorage+0xEC)=nullptr;
    *reinterpret_cast<uint32_t*>(g_mainState+8)=1;
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kWaitingForNativeRoom,
        "a structurally identified native mission main state waits for its real ready transition");
    *reinterpret_cast<uint32_t*>(g_mainState+8)=2;
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kRoomReady &&
        !*reinterpret_cast<void**>(g_profileStorage+0xEC),
        "the actual verified 3D room getter works without fabricating the absent legacy profile room");
    uint8_t unrelatedLegacy[0x20]{};unrelatedLegacy[5]=140;
    *reinterpret_cast<void**>(g_profileStorage+0xEC)=unrelatedLegacy;
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kRoomReady &&
        *reinterpret_cast<void**>(g_profileStorage+0xEC)==unrelatedLegacy,
        "a non-null unrelated legacy publication cannot override the actual 3D room or be rewritten");
    *reinterpret_cast<void**>(g_profileStorage+0xEC)=nullptr;
    uint8_t* savedCommandContext =
        *reinterpret_cast<uint8_t**>(
            g_startCommand + 0x0C);
    *reinterpret_cast<uint8_t**>(
        g_startCommand + 0x0C) = nullptr;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kNativeRoomMismatch,
        "start command must retain the native manager context and back-pointer");
    *reinterpret_cast<uint8_t**>(
        g_startCommand + 0x0C) =
        savedCommandContext;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kRoomReady,
        "exact mission owner/manager/main/command graph completes room readiness");

    g_addStateSendsImmediately = false;
    Check(
        actions.Start() ==
            MissionFlowActions::
                kStartQueued &&
            g_addStateCalls == 1 &&
            g_addStateAbiValid,
        "Start queues native state 12 once with the verified x86 arguments");
    Check(
        actions.Start() ==
            MissionFlowActions::
                kStartQueued &&
            g_addStateCalls == 1,
        "polling a queued start never enqueues a duplicate state");

    uint8_t packet[8] = {};
    *reinterpret_cast<uint16_t*>(
        packet) = 0x06C3;
    Check(
        g_startReplyDetour != nullptr,
        "mission start-command reply detour was captured");
    if (!g_startReplyDetour)
        return;
    Check(
        g_startReplyDetour(
            g_startCommand,
            nullptr,
            0,
            0,
            0,
            0x06C3,
            packet) == 41 &&
            !actions.PollSnapshot().
                battle_start_consumed,
        "0x06C3 cannot complete before the original 0x08A7 send is observed");
    *reinterpret_cast<uint32_t*>(
        g_startCommand + 8) = 2;

    Check(
        SendNativePacket(
            0x08A7,
            8),
        "the original 0x08A7 send can be followed by a same-update reply before another Start poll");

    Check(
        g_startReplyDetour(
            g_otherCommand,
            nullptr,
            0,
            0,
            0,
            0x06C3,
            packet) == 41 &&
            !actions.PollSnapshot().
                battle_start_consumed,
        "0x06C3 for another command owner cannot complete this start");
    *reinterpret_cast<uint32_t*>(
        g_startCommand + 8) = 1;
    Check(
        g_startReplyDetour(
            g_startCommand,
            nullptr,
            0,
            0,
            0,
            0x06C3,
            packet) == 41 &&
            !actions.PollSnapshot().
                battle_start_consumed,
        "owned 0x06C3 requires the native command to be in stage 2");
    *reinterpret_cast<uint32_t*>(
        g_startCommand + 8) = 2;
    *reinterpret_cast<uint8_t**>(
        g_startCommand + 0x0C) = nullptr;
    Check(
        g_startReplyDetour(
            g_startCommand,
            nullptr,
            0,
            0,
            0,
            0x06C3,
            packet) == 41 &&
            !actions.PollSnapshot().
                battle_start_consumed,
        "owned 0x06C3 requires the original command context graph");
    *reinterpret_cast<uint8_t**>(
        g_startCommand + 0x0C) =
        savedCommandContext;
    *reinterpret_cast<uint32_t*>(
        g_startCommand + 8) = 2;
    Check(
        g_startReplyDetour(
            g_startCommand,
            nullptr,
            0,
            0,
            0,
            0x06C3,
            packet) == 41 &&
            actions.Start() ==
                MissionFlowActions::
                    kBattleStartConsumed,
        "genuine 0x06C3 is published only after the original command consumer");
    const auto final =
        actions.PollSnapshot();
    Check(
        final.start_request_sent &&
            final.battle_start_consumed &&
            !final.natural_result_observed &&
            !final.natural_exit_observed,
        "battle start is proven while natural result and exit remain explicitly OPEN");
}

void TestChannelListReadiness(
    MissionFlowActions& actions)
{
    ResetFixture(actions);
    *reinterpret_cast<uint16_t*>(
        g_groups +
        kGroupStride + 0x22) = 0;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kEnteringMissionChannel,
        "an empty pre-reply profile remains an asynchronous native-list wait");
    const auto waiting =
        actions.PollSnapshot();
    Check(
        !waiting.channel_list_ready &&
            waiting.channel_scan_status ==
                MissionFlowActions::
                    kChannelScanWaitingForReply &&
            waiting.advertised_raw_count == 0,
        "pre-reply state is not mislabeled as a ready empty list");
    *reinterpret_cast<uint16_t*>(
        g_groups +
        kGroupStride + 0x22) = 2;
    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    Check(
        g_joinCalls == 1 &&
            g_joinedChannel ==
                kMissionChannel &&
            actions.PollSnapshot().
                advertised_eligible_count == 1,
        "the native reply callback may populate the real list before selection");

    ResetFixture(actions);
    *reinterpret_cast<uint16_t*>(
        g_groups +
        kGroupStride + 0x22) = 0;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kEnteringMissionChannel,
        "ready-empty fixture entered the native list flow");
    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    const auto emptyReady =
        actions.PollSnapshot();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kNoAdvertisedMissionChannel &&
            emptyReady.channel_list_ready &&
            emptyReady.channel_scan_valid &&
            emptyReady.advertised_raw_count == 0 &&
            emptyReady.advertised_mode2_count == 0,
        "a native callback with zero rows is a truly empty ready list");

    ResetFixture(actions);
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kEnteringMissionChannel,
        "bounded-wait fixture entered the native list flow");
    bool stayedPending = true;
    for (unsigned i = 0; i < 600; ++i) {
        stayedPending =
            stayedPending &&
            actions.EnsureRoom(
                5,
                "mission-ai") ==
                MissionFlowActions::
                    kEnteringMissionChannel;
    }
    const auto timedOut =
        actions.EnsureRoom(
            5,
            "mission-ai");
    const auto timeoutState =
        actions.PollSnapshot();
    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    Check(
        stayedPending &&
            timedOut ==
                MissionFlowActions::
                    kChannelRequestRejected &&
            timeoutState.channel_scan_status ==
                MissionFlowActions::
                    kChannelScanReplyTimedOut &&
            timeoutState.channel_wait_polls == 601 &&
            !timeoutState.channel_choice_armed &&
            g_choiceFallbackCalls == 0 &&
            g_joinCalls == 0,
        "missing native list replies fail after a bounded wait and suppress a late fallback");
}

void TestStartRejectAndLatchedSend(MissionFlowActions& actions)
{
    ResetFixture(actions);
    Check(SubmitMatchingMission(actions,5),"start rejection follows an actual listed create request");
    auto* reply=AllocateGuardedReply(0x0A33,kRoomId);
    Check(reply!=nullptr,"start rejection receives an owned create acknowledgement");
    if(!reply)return;
    g_createReplyDetour(g_missionLobby,nullptr,0,0,0,0x0A33,reply);
    InstallNativeRoom(5,kRoomId);
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kRoomReady,
        "start rejection uses the validated native room graph");
    g_addStateSendsImmediately=true;
    Check(actions.Start()==MissionFlowActions::kStartRequestSent,"the original command sends before unrelated traffic");
    SendNativePacket(0x04CC,10);
    Check(actions.Start()==MissionFlowActions::kStartRequestSent && g_addStateCalls==1,
        "unrelated native traffic cannot erase the observed start send or enqueue another start");
    uint8_t rejected[8]{};*reinterpret_cast<uint16_t*>(rejected)=0x07B5;
    *reinterpret_cast<uint16_t*>(rejected+6)=0x0FB1;
    const int result=g_startReplyDetour(g_startCommand,nullptr,0,0,0,0x07B5,rejected);
    const auto state=actions.PollSnapshot();
    Check(result==41 && actions.Start()==MissionFlowActions::kStartServerRejected &&
        state.last_start_reply_opcode==0x07B5 && state.last_start_reply_status==0x0FB1 &&
        !state.battle_start_consumed,
        "a genuine owned start rejection preserves original handling and reports its exact native code");
}

void TestMode8AndMissionWrap(
    MissionFlowActions& actions)
{
    ResetFixture(actions, 1);
    Check(
        OpenCreateDialog(
            actions,
            8),
        "mode 8 uses the same normal multiplayer mission entry");
    Check(
        actions.EnsureRoom(
            8,
            "mission-ai") ==
            MissionFlowActions::
                kCyclingMission,
        "mode 8 cycles away from a listed mode-5 descriptor");
    ApplyPendingDescriptor();
    Check(
        actions.EnsureRoom(
            8,
            "mission-ai") ==
            MissionFlowActions::
                kSettingRoomTitle,
        "mode 8 accepts only an actual id above the native threshold");

    ResetFixture(actions, 0);
    g_descriptorIds[0] = 0x00400000;
    g_descriptorIds[1] = 0x00400001;
    Check(
        OpenCreateDialog(
            actions,
            5),
        "mission-wrap fixture reached the original create dialog");
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kCyclingMission,
        "first unavailable descriptor advances");
    ApplyPendingDescriptor();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kCyclingMission,
        "second unavailable descriptor advances");
    ApplyPendingDescriptor();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kRequestedMissionUnavailable,
        "wrapping to the original key fails without inventing a mission");
    g_descriptorIds[0] = 0x00400000;
    g_descriptorIds[1] = 0x00300000;
}

void TestFailClosedBoundaries(
    MissionFlowActions& actions)
{
    ResetFixture(actions);
    Check(OpenCreateDialog(actions,5),"delayed native modal controls follow a real create callback");
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kCyclingMission,
        "delayed edit fixture selects its actual matching listed mission first");
    ApplyPendingDescriptor();
    g_titleControl[0x162]=0;
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kWaitingForCreateDialog &&
        actions.PollSnapshot().create_control_kind==2 && !g_setTitle[0],
        "an identified but not yet visible native edit waits without setting a title");
    g_titleControl[0x162]=1;
    Check(actions.EnsureRoom(5,"mission-ai")==MissionFlowActions::kSettingRoomTitle && g_setTitle[0],
        "the same modal proceeds through its original title method once interactive");

    ResetFixture(actions);
    g_missionLobbyPresent = true;
    RebuildServiceList();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kChannelSelectionMismatch &&
            g_lobbyCallbackCalls == 0,
        "a pre-existing mission lobby cannot bypass this run's advertised Mode-2 selection");

    ResetFixture(actions);
    Check(
        EnterMissionLobby(
            actions,
            5),
        "selected-channel mismatch fixture completed the real join request");
    SetSelectedChannel(0, 6001);
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kChannelSelectionMismatch &&
            g_lobbyCallbackCalls == 0,
        "mission lobby actions require the exact channel selected by this run");

    ResetFixture(actions);
    Check(
        EnterMissionLobby(
            actions,
            5),
        "post-join advertised-row fixture completed the real join request");
    *reinterpret_cast<uint32_t*>(
        Channel(1, 1) + 0x0E) = 100;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kSelectingMultiplayer,
        "a channel that filled after the accepted join remains the same advertised Mode-2 identity");

    ResetFixture(actions);
    Check(
        EnterMissionLobby(
            actions,
            5),
        "post-join row-identity fixture completed the real join request");
    *reinterpret_cast<uint32_t*>(
        Channel(1, 1) + 0x16) = 3;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kChannelSelectionMismatch &&
            g_lobbyCallbackCalls == 0,
        "mission lobby actions stop if the selected advertised row is no longer Mode-2");

    ResetFixture(actions);
    Check(
        EnterMissionLobby(
            actions,
            5),
        "pre-existing create-dialog fixture completed the real join request");
    g_missionLobby[0x64] = 3;
    g_dialogPresent = true;
    RebuildServiceList();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
                MissionFlowActions::
                    kCreateDialogMismatch &&
            g_lobbyCallbackCalls == 0 &&
            !actions.PollSnapshot().
                create_dialog_observed,
        "a create dialog is accepted only after this run invokes original MakeRoomBtn");

    ResetFixture(actions);
    *reinterpret_cast<void**>(
        g_singleControl.data() +
        0x15C) = g_lobbyRoot;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kTopMenuControlMismatch &&
            g_topCallbackCalls == 0,
        "wrong SingleBtn parent prevents the top-menu callback");

    const auto currentChannelListResult =
        [&actions]() {
            if (actions.EnsureRoom(
                    5,
                    "mission-ai") !=
                MissionFlowActions::
                    kEnteringMissionChannel)
                return MissionFlowActions::
                    kUnbound;
            if (!g_choiceDetour)
                return MissionFlowActions::
                    kUnbound;
            g_choiceDetour(
                g_topService,
                nullptr);
            return actions.EnsureRoom(
                5,
                "mission-ai");
        };

    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(
        Channel(1, 1) + 0x16) = 1;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kEnteringMissionChannel,
        "missing Mode-2 row is discovered only after the native list flow begins");
    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kNoAdvertisedMissionChannel &&
            g_joinCalls == 0,
        "no advertised Mode-2 row fails without normal-channel fallback");

    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(
        Channel(1, 1) + 0x0E) = 100;
    Check(
        currentChannelListResult() ==
            MissionFlowActions::
                kNoAdvertisedMissionChannel &&
            actions.PollSnapshot().
                rejected_full_count == 1,
        "a full Mode-2 row is never selected");

    ResetFixture(actions);
    Channel(1, 1)[0x1C] = 0;
    Check(
        currentChannelListResult() ==
            MissionFlowActions::
                kNoAdvertisedMissionChannel &&
            actions.PollSnapshot().
                rejected_disabled_count == 1,
        "a disabled Mode-2 row is never selected");

    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(
        Channel(1, 1) + 0x0A) = 0;
    Check(
        currentChannelListResult() ==
                MissionFlowActions::
                    kNoAdvertisedMissionChannel &&
            actions.PollSnapshot().
                rejected_zero_capacity_count ==
                1,
        "a zero-capacity Mode-2 row is diagnosed and never selected");

    ResetFixture(actions);
    *reinterpret_cast<uint16_t*>(
        Channel(1, 1)) = 0;
    Check(
        currentChannelListResult() ==
                MissionFlowActions::
                    kNoAdvertisedMissionChannel &&
            actions.PollSnapshot().
                rejected_zero_id_count == 1,
        "a zero-id Mode-2 row is diagnosed and never selected");

    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(
        Channel(1, 1) + 0x16) = 3;
    Check(
        currentChannelListResult() ==
            MissionFlowActions::
                kNoAdvertisedMissionChannel &&
            actions.PollSnapshot().
                advertised_mode2_count == 0 &&
            actions.PollSnapshot().
                rejected_non_mode2_count == 2,
        "non-Mode-2 rows are diagnosed and cannot be used as fallback");

    ResetFixture(actions);
    *reinterpret_cast<uint16_t*>(
        g_groups +
        kGroupStride + 0x22) = 65;
    Check(
        currentChannelListResult() ==
            MissionFlowActions::
                kChannelRequestRejected &&
            actions.PollSnapshot().
                channel_scan_status ==
                MissionFlowActions::
                    kChannelScanChannelCountInvalid,
        "an oversized native channel list fails closed");

    ResetFixture(actions);
    *reinterpret_cast<uint8_t**>(
        g_profileStorage + 0x5B4) =
        g_groups + 1;
    Check(
        currentChannelListResult() ==
            MissionFlowActions::
                kChannelRequestRejected &&
            actions.PollSnapshot().
                channel_scan_status ==
                MissionFlowActions::
                    kChannelScanVectorMalformed,
        "a malformed native group-vector span fails closed");

    ResetFixture(actions);
    g_joinWritesPending = false;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kEnteringMissionChannel,
        "join-state failure fixture entered the native chooser");
    if (g_choiceDetour)
        g_choiceDetour(
            g_topService,
            nullptr);
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kChannelRequestRejected,
        "0x04FA transport alone cannot pass without native state 4");

    ResetFixture(actions, 0);
    Check(
        OpenCreateDialog(
            actions,
            5),
        "create transport rejection fixture reached the dialog");
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kCyclingMission,
        "transport rejection fixture selected the requested mode normally");
    ApplyPendingDescriptor();
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kSettingRoomTitle,
        "transport rejection fixture set title normally");
    g_networkAcceptsCreate = false;
    Check(
        actions.EnsureRoom(
            5,
            "mission-ai") ==
            MissionFlowActions::
                kCreateTransportRejected,
        "failed 0x0640 transport is not retried or treated as a room");

    ResetFixture(actions, 0);
    Check(
        SubmitMatchingMission(
            actions,
            5),
        "reply-mismatch fixture submitted a native mission create");
    g_emitRoomDetails = false;
    uint8_t* reply =
        AllocateGuardedReply(
            0x0A33,
            kRoomId);
    Check(
        reply != nullptr,
        "reply-mismatch guarded packet was allocated");
    if (reply &&
        g_createReplyDetour) {
        g_createReplyDetour(
            g_missionLobby,
            nullptr,
            0,
            0,
            0,
            0x0A33,
            reply);
        Check(
            actions.EnsureRoom(
                5,
                "mission-ai") ==
                MissionFlowActions::
                    kCreateReplyMismatch,
            "0x0A33 cannot pass unless the original handler emits native 0x0621");
    }

    ResetFixture(actions, 0);
    Check(
        SubmitMatchingMission(
            actions,
            5),
        "reset-correlation fixture submitted a native mission create");
    actions.Reset();
    reply = AllocateGuardedReply(
        0x0A33,
        kRoomId);
    Check(
        reply != nullptr,
        "stale 0x0A33 guarded packet was allocated");
    if (reply &&
        g_createReplyDetour) {
        g_createReplyDetour(
            g_missionLobby,
            nullptr,
            0,
            0,
            0,
            0x0A33,
            reply);
        const auto resetState =
            actions.PollSnapshot();
        Check(
            !resetState.create_reply_consumed &&
                resetState.requested_room_id == 0,
            "Reset invalidates prior create-reply ownership before a stale 0x0A33 arrives");
    }
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: MissionFlowActions Win32 ABI/state regression; no game, server, or database");

    g_topVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeTopCallback);
    g_lobbyVtable[0x28 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeMissionPacketDispatch);
    g_lobbyVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeMissionLobbyCallback);
    g_dialogVtable[0x30 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeCreateUpdate);
    g_dialogVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeCreateCallback);
    g_commandVtable[0x18 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeStartReplyOriginal);

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "mission-ai";
    config.room_mode = 5;
    config.map_id = 0;

    NativeActions native;
    Check(
        native.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            config),
        "NativeActions transport observer bound to offline symbols");

    void* savedCallback =
        g_dialogVtable[
            0x38 / sizeof(void*)];
    g_dialogVtable[
        0x38 / sizeof(void*)] = nullptr;
    MissionFlowActions rejected;
    Check(
        !rejected.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            &native),
        "Bind rejects a create-dialog vtable with the wrong callback slot");
    g_dialogVtable[
        0x38 / sizeof(void*)] =
        savedCallback;

    MissionFlowActions actions;
    Check(
        actions.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            &native),
        "MissionFlowActions bound after all code/vtable identities matched");
    Check(
        g_networkSendDetour &&
            g_choiceDetour &&
            g_createReplyDetour &&
            g_startReplyDetour,
        "transport, channel-choice, create-reply and start-reply hooks were captured");

    TestBoundaryAndTopEntry(actions);
    TestChannelListReadiness(actions);
    TestCreationDialogHandoff(actions);
    TestCompleteMode5Flow(actions);
    TestStartRejectAndLatchedSend(actions);
    TestMode8AndMissionWrap(actions);
    TestFailClosedBoundaries(actions);

    DestroyControls();
    if (g_replyAllocation) {
        VirtualFree(
            g_replyAllocation,
            0,
            MEM_RELEASE);
        g_replyAllocation = nullptr;
    }
    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE MISSION FLOW FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts(
        "OFFLINE MISSION FLOW PASS");
    return 0;
}
