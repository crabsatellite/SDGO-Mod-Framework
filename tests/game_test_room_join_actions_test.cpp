// OFFLINE ONLY.
//
// Win32 ABI/state-machine regression for RoomJoinActions. The fixture models
// the verified native layouts and original callbacks. It never launches
// GOnline, contacts a server, writes a database, or claims gameplay evidence.

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/room_join_actions.h"

namespace
{
static_assert(
    sizeof(void*) == 4,
    "room-join ABI test must be compiled as Win32");

constexpr uint32_t kRoomId = 0x1234ABCD;
constexpr uint8_t kRoomVariant = 1;
constexpr uint8_t kLocalSlot = 2;
constexpr char kTitle[] = "GT-JOIN-NONCE";

using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using PacketDetour =
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

static_assert(
    offsetof(ServiceNode, service) == 8,
    "service list fixture must match the shipped x86 node");

int g_failures = 0;
RoomJoinActions* g_actions = nullptr;
NetworkSendDetour g_networkSendDetour = nullptr;
PacketDetour g_roomPacketDetour = nullptr;
PacketDetour g_readyReplyDetour = nullptr;

uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x6000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_room[0x100] = {};
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;

uint8_t g_serviceManagerStorage[0x60] = {};
void* g_serviceManager = g_serviceManagerStorage;
ServiceNode g_head = {};
ServiceNode g_roomNode = {};
ServiceNode g_duplicateNode = {};

uint8_t g_roomList[0x180] = {};
uint8_t g_duplicateRoomList[0x180] = {};
void* g_roomListVtable[20] = {};
uint8_t g_roomEntries[0x90 * 10] = {};

std::array<uint8_t, 0x240> g_root = {};
std::array<uint8_t, 0x240> g_makeButton = {};
std::array<std::array<uint8_t, 0x240>, 10> g_rows = {};
const wchar_t* g_controlNames[12] = {};
bool g_hideRequestedRow = false;

uint8_t g_waitOwner[0x20] = {};
uint8_t g_manager[0x80] = {};
uint8_t g_readyCommand[0x20] = {};
uint8_t* g_states[25] = {};
void* g_waitOwnerVtable[12] = {};
void* g_managerVtables[3][16] = {};
void* g_readyCommandVtable[12] = {};

uint8_t g_readyFirst[0x40] = {};
uint8_t g_readyStatus[0x20] = {};
void* g_readyStatusVtable[20] = {};

bool g_networkAccepts = true;
bool g_localIsHost = false;
uint8_t g_nativeReady = 0;
void* g_readyGetterThis = nullptr;
uint32_t g_readyGetterSlot = UINT32_MAX;
unsigned g_readyGetterCalls = 0;
bool g_roomOriginalCalled = false;
bool g_roomPacketFreed = false;
uint32_t g_joinSequenceInsideOriginal = UINT32_MAX;
uint32_t g_joinResultInsideOriginal = UINT32_MAX;
bool g_readyOriginalCalled = false;
bool g_readyPacketFreed = false;
uint32_t g_readySequenceInsideOriginal = UINT32_MAX;
uint32_t g_readyResultInsideOriginal = UINT32_MAX;
unsigned g_joinCallbackCalls = 0;
unsigned g_readyBusinessCalls = 0;
const char* g_rejectedHookSlot = nullptr;

void Check(
    bool condition,
    const char* message)
{
    if (condition) {
        std::printf(
            "PASS: %s\n",
            message);
        return;
    }
    std::fprintf(
        stderr,
        "FAIL: %s\n",
        message);
    ++g_failures;
}

template <typename T>
void Put(
    void* base,
    size_t offset,
    T value)
{
    std::memcpy(
        static_cast<uint8_t*>(
            base) +
            offset,
        &value,
        sizeof(value));
}

template <typename T>
T Get(
    const void* base,
    size_t offset)
{
    T value = {};
    std::memcpy(
        &value,
        static_cast<const uint8_t*>(
            base) +
            offset,
        sizeof(value));
    return value;
}

const wchar_t* NativeString(
    const void* storage)
{
    const auto* bytes =
        static_cast<const uint8_t*>(
            storage);
    const uint32_t capacity =
        Get<uint32_t>(
            bytes,
            0x14);
    return capacity < 8
               ? reinterpret_cast<
                     const wchar_t*>(
                     bytes)
               : Get<const wchar_t*>(
                     bytes,
                     0);
}

void SetNativeString(
    void* storage,
    const wchar_t* value)
{
    std::memset(
        storage,
        0,
        0x18);
    const uint32_t length =
        static_cast<uint32_t>(
            std::wcslen(value));
    if (length < 8) {
        std::memcpy(
            storage,
            value,
            (length + 1) *
                sizeof(wchar_t));
        Put<uint32_t>(
            storage,
            0x14,
            7);
    } else {
        Put<const wchar_t*>(
            storage,
            0,
            value);
        Put<uint32_t>(
            storage,
            0x14,
            length);
    }
    Put<uint32_t>(
        storage,
        0x10,
        length);
}

void SetControlName(
    uint8_t* control,
    const wchar_t* value)
{
    SetNativeString(
        control + 0x118,
        value);
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

char* __cdecl FakePasswordMd5(
    const void*,
    char* output,
    int)
{
    std::memset(
        output,
        '0',
        32);
    output[32] = '\0';
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
    const void*,
    int,
    char)
{
    return g_networkAccepts ? 1 : 0;
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
    return 1;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    SetNativeString(
        self,
        value);
    return self;
}

void __fastcall FakeNativeWStringDtor(
    void*,
    void*)
{
}

void* __fastcall FakeNicknameCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(
    void* self,
    int)
{
    return self;
}

void __fastcall FakeVoidThis(
    void*,
    void*)
{
}

bool __stdcall FakeIsRoomMaster(
    void* room)
{
    return room == g_room &&
           g_localIsHost;
}

void* __fastcall FakeFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    if (root != g_root.data() ||
        !nativeName)
        return nullptr;
    const wchar_t* name =
        NativeString(nativeName);
    if (!name)
        return nullptr;
    if (kind == 4 &&
        std::wcscmp(
            name,
            L"MakeRoomBtn") == 0)
        return g_makeButton.data();
    if (kind != 0x0C ||
        std::wcsncmp(
            name,
            L"GameRoom",
            8) != 0 ||
        name[8] < L'0' ||
        name[8] > L'9' ||
        name[9] != L'\0')
        return nullptr;
    const unsigned index =
        static_cast<unsigned>(
            name[8] - L'0');
    if (g_hideRequestedRow &&
        index == 0)
        return nullptr;
    return g_rows[index].data();
}

int SendNativePacket(
    const void* packet,
    int size)
{
    if (!g_networkSendDetour)
        return 0;
    return g_networkSendDetour(
        g_network,
        nullptr,
        &g_stream,
        packet,
        size,
        0);
}

uint32_t __stdcall FakeRoomJoinCallback(
    void* service,
    uint32_t event,
    const void* controlName,
    void* control)
{
    ++g_joinCallbackCalls;
    if (service != g_roomList ||
        event != 0x303 ||
        !control ||
        controlName !=
            static_cast<uint8_t*>(
                control) +
                0x118)
        return 0;

    int index = -1;
    for (int candidate = 0;
         candidate < 10;
         ++candidate) {
        if (Get<void*>(
                service,
                0x94 +
                    candidate *
                        0x0C) ==
            control) {
            index = candidate;
            break;
        }
    }
    if (index < 0)
        return 0;

    auto* begin =
        Get<uint8_t*>(
            service,
            0x124);
    uint8_t packet[0x32] = {};
    Put<uint16_t>(
        packet,
        0,
        0x0734);
    Put<uint32_t>(
        packet,
        6,
        Get<uint32_t>(
            begin +
                index * 0x90,
            0));
    SendNativePacket(
        packet,
        sizeof(packet));
    return 0xA5A5u;
}

uint8_t __fastcall FakeReadyGetter(
    void* status,
    void*,
    uint32_t localSlot)
{
    g_readyGetterThis = status;
    g_readyGetterSlot = localSlot;
    ++g_readyGetterCalls;
    return g_nativeReady;
}

int __fastcall FakeReadyBusiness(
    void* command,
    void*)
{
    ++g_readyBusinessCalls;
    if (command != g_readyCommand)
        return -1;
    auto* context =
        Get<uint8_t*>(
            command,
            0x0C);
    auto* first =
        context
            ? Get<uint8_t*>(
                  context,
                  8)
            : nullptr;
    auto* status =
        first
            ? Get<uint8_t*>(
                  first,
                  0x14)
            : nullptr;
    const uint32_t localSlot =
        context
            ? Get<uint32_t>(
                  context,
                  0)
            : UINT32_MAX;
    if (status != g_readyStatus ||
        localSlot >= 12)
        return -1;
    const uint8_t currentReady =
        FakeReadyGetter(
            status,
            nullptr,
            localSlot);
    uint8_t packet[8] = {};
    Put<uint16_t>(
        packet,
        0,
        0x05A6);
    packet[6] =
        static_cast<uint8_t>(
            localSlot);
    packet[7] =
        currentReady ? 0 : 1;
    Put<uint32_t>(
        command,
        8,
        2);
    SendNativePacket(
        packet,
        sizeof(packet));
    return 2;
}

int __fastcall FakeRoomPacketOriginal(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void* packet)
{
    g_roomOriginalCalled = true;
    if (g_actions) {
        const auto inside =
            g_actions->JoinState();
        g_joinSequenceInsideOriginal =
            inside.sequence;
        g_joinResultInsideOriginal =
            inside.result;
    }
    g_roomPacketFreed =
        packet &&
        VirtualFree(
            packet,
            0,
            MEM_RELEASE) != 0;
    return 41;
}

int __fastcall FakeReadyReplyOriginal(
    void* self,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    g_readyOriginalCalled = true;
    if (g_actions) {
        const auto inside =
            g_actions->ReadyState();
        g_readySequenceInsideOriginal =
            inside.sequence;
        g_readyResultInsideOriginal =
            inside.result;
    }
    if (self && packet &&
        (opcode & 0xFFFF) == 0x0999) {
        auto* bytes =
            static_cast<uint8_t*>(
                packet);
        if (bytes[6] ==
            g_manager[0x20])
            g_nativeReady =
                bytes[7] != 0;
        Put<uint32_t>(
            self,
            8,
            3);
    }
    g_readyPacketFreed =
        packet &&
        VirtualFree(
            packet,
            0,
            MEM_RELEASE) != 0;
    return 47;
}

void* LookupSymbol(
    const char* name)
{
    if (std::strcmp(
            name,
            "game.app") == 0)
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
            &FakePasswordMd5);
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
            "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameCtor);
    if (std::strcmp(
            name,
            "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(
            &FakeEnqueueUiObject);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_dtor") == 0 ||
        std::strcmp(
            name,
            "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(
            &FakeVoidThis);
    if (std::strcmp(
            name,
            "flow.is_room_master") == 0)
        return reinterpret_cast<void*>(
            &FakeIsRoomMaster);
    if (std::strcmp(
            name,
            "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(
            name,
            "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringDtor);
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeFindControl);
    if (std::strcmp(
            name,
            "flow.native_room_list_vtable") == 0)
        return g_roomListVtable;
    if (std::strcmp(
            name,
            "flow.room_join_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeRoomJoinCallback);
    if (std::strcmp(
            name,
            "flow.three_d_wait_room_vtable") == 0)
        return g_waitOwnerVtable;
    if (std::strcmp(
            name,
            "flow.room_join_ready_command_vtable") == 0)
        return g_readyCommandVtable;
    if (std::strcmp(
            name,
            "flow.room_join_ready_business") == 0)
        return reinterpret_cast<void*>(
            &FakeReadyBusiness);
    if (std::strcmp(
            name,
            "flow.normal_3d_manager_vtable") == 0)
        return g_managerVtables[0];
    if (std::strcmp(
            name,
            "flow.ai_3d_manager_vtable") == 0)
        return g_managerVtables[1];
    if (std::strcmp(
            name,
            "flow.battleship_3d_manager_vtable") == 0)
        return g_managerVtables[2];
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
    void* target =
        LookupSymbol(name);
    if (!target || !detour ||
        !original)
        return 0;
    *original = target;
    if (std::strcmp(
            name,
            "flow.network_send") == 0)
        g_networkSendDetour =
            reinterpret_cast<
                NetworkSendDetour>(
                detour);
    return 1;
}

int ApiHookSlot(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    if (!name || !detour ||
        !original ||
        (g_rejectedHookSlot &&
         std::strcmp(
             name,
             g_rejectedHookSlot) == 0))
        return 0;
    if (std::strcmp(
            name,
            "flow.room_join_list_packet_slot") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakeRoomPacketOriginal);
        g_roomPacketDetour =
            reinterpret_cast<
                PacketDetour>(
                detour);
        return 1;
    }
    if (std::strcmp(
            name,
            "flow.room_join_ready_reply_slot") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakeReadyReplyOriginal);
        g_readyReplyDetour =
            reinterpret_cast<
                PacketDetour>(
                detour);
        return 1;
    }
    return 0;
}

void ResetPublicationReceipts()
{
    g_roomOriginalCalled = false;
    g_roomPacketFreed = false;
    g_joinSequenceInsideOriginal =
        UINT32_MAX;
    g_joinResultInsideOriginal =
        UINT32_MAX;
    g_readyOriginalCalled = false;
    g_readyPacketFreed = false;
    g_readySequenceInsideOriginal =
        UINT32_MAX;
    g_readyResultInsideOriginal =
        UINT32_MAX;
}

void SetupRoomList(
    uint32_t roomId = kRoomId,
    const char* title = kTitle)
{
    std::memset(
        g_app,
        0,
        sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_room,
        0,
        sizeof(g_room));
    std::memset(
        g_serviceManagerStorage,
        0,
        sizeof(g_serviceManagerStorage));
    std::memset(
        g_roomList,
        0,
        sizeof(g_roomList));
    std::memset(
        g_duplicateRoomList,
        0,
        sizeof(g_duplicateRoomList));
    std::memset(
        g_roomEntries,
        0,
        sizeof(g_roomEntries));
    g_root.fill(0);
    g_makeButton.fill(0);
    for (auto& row : g_rows)
        row.fill(0);
    g_head = {};
    g_roomNode = {};
    g_duplicateNode = {};

    Put<uint32_t>(
        g_app,
        0x4A0,
        6);
    Put<void*>(
        g_roomList,
        0,
        g_roomListVtable);
    Put<void*>(
        g_roomList,
        0x84,
        g_makeButton.data());
    Put<uint8_t*>(
        g_roomList,
        0x124,
        g_roomEntries);
    Put<uint8_t*>(
        g_roomList,
        0x128,
        g_roomEntries + 0x90);

    g_root[0x162] = 1;
    g_makeButton[0x162] = 1;
    g_makeButton[0x167] = 1;
    Put<void*>(
        g_makeButton.data(),
        0x15C,
        g_root.data());
    SetControlName(
        g_makeButton.data(),
        L"MakeRoomBtn");

    static const wchar_t* rowNames[10] = {
        L"GameRoom0",
        L"GameRoom1",
        L"GameRoom2",
        L"GameRoom3",
        L"GameRoom4",
        L"GameRoom5",
        L"GameRoom6",
        L"GameRoom7",
        L"GameRoom8",
        L"GameRoom9",
    };
    for (int index = 0;
         index < 10;
         ++index) {
        auto* row =
            g_rows[index].data();
        row[0x162] = 1;
        row[0x167] = 1;
        Put<void*>(
            row,
            0x15C,
            g_root.data());
        Put<void*>(
            row,
            0x21C,
            reinterpret_cast<void*>(
                0x12340000u +
                index));
        SetControlName(
            row,
            rowNames[index]);
        Put<void*>(
            g_roomList,
            0x94 +
                index * 0x0C,
            row);
    }

    Put<uint32_t>(
        g_roomEntries,
        0,
        roomId);
    std::memcpy(
        g_roomEntries + 0x7A,
        title,
        std::strlen(title));

    Put<ServiceNode*>(
        g_serviceManagerStorage,
        0x44,
        &g_head);
    g_head.next = &g_roomNode;
    g_roomNode.next = &g_head;
    g_roomNode.service =
        g_roomList;

    g_hideRequestedRow = false;
    g_networkAccepts = true;
    g_localIsHost = false;
    g_nativeReady = false;
    g_joinCallbackCalls = 0;
    g_readyBusinessCalls = 0;
    ResetPublicationReceipts();
}

void InstallGuestRoom(
    uint32_t roomId = kRoomId)
{
    std::memset(
        g_waitOwner,
        0,
        sizeof(g_waitOwner));
    std::memset(
        g_manager,
        0,
        sizeof(g_manager));
    std::memset(
        g_readyCommand,
        0,
        sizeof(g_readyCommand));
    std::memset(
        g_states,
        0,
        sizeof(g_states));
    std::memset(
        g_readyFirst,
        0,
        sizeof(g_readyFirst));
    std::memset(
        g_readyStatus,
        0,
        sizeof(g_readyStatus));

    Put<uint32_t>(
        g_room,
        0,
        roomId);
    g_room[4] = kRoomVariant;
    g_room[5] = 1;
    Put<uint8_t*>(
        g_profileStorage,
        0xEC,
        g_room);

    Put<uint32_t>(
        g_app,
        0x4A0,
        18);
    Put<uint8_t*>(
        g_app,
        0x4A4,
        g_waitOwner);
    Put<void*>(
        g_waitOwner,
        0,
        g_waitOwnerVtable);
    Put<uint8_t*>(
        g_waitOwner,
        4,
        g_manager);

    Put<void*>(
        g_manager,
        0,
        g_managerVtables[0]);
    Put<uint8_t**>(
        g_manager,
        0x0C,
        g_states);
    g_manager[0x20] =
        kLocalSlot;
    Put<uint8_t*>(
        g_manager,
        0x28,
        g_readyFirst);

    g_states[4] =
        g_readyCommand;
    Put<void*>(
        g_readyCommand,
        0,
        g_readyCommandVtable);
    Put<uint32_t>(
        g_readyCommand,
        4,
        4);
    Put<uint32_t>(
        g_readyCommand,
        8,
        0);
    Put<uint8_t*>(
        g_readyCommand,
        0x0C,
        g_manager + 0x20);

    Put<uint8_t*>(
        g_readyFirst,
        0x14,
        g_readyStatus);
    Put<void*>(
        g_readyStatus,
        0,
        g_readyStatusVtable);
    g_readyGetterThis = nullptr;
    g_readyGetterSlot = UINT32_MAX;
    g_readyGetterCalls = 0;
}

uint8_t* AllocatePacket(
    size_t size)
{
    (void)size;
    auto* packet =
        static_cast<uint8_t*>(
            VirtualAlloc(
                nullptr,
                0x1000,
                MEM_RESERVE |
                    MEM_COMMIT,
                PAGE_READWRITE));
    if (packet)
        std::memset(
            packet,
            0,
            0x1000);
    return packet;
}

int DeliverJoinReply(
    uint32_t roomId,
    uint8_t variant)
{
    auto* packet =
        AllocatePacket(0x20);
    Check(
        packet != nullptr,
        "join reply packet fixture was allocated");
    if (!packet ||
        !g_roomPacketDetour)
        return 0;
    Put<uint32_t>(
        packet,
        6,
        roomId);
    packet[10] = variant;
    return g_roomPacketDetour(
        g_roomList,
        nullptr,
        1,
        2,
        3,
        0x0550,
        packet);
}

int DeliverReadyReply(
    uint8_t slot,
    uint8_t flag)
{
    auto* packet =
        AllocatePacket(0x20);
    Check(
        packet != nullptr,
        "ready reply packet fixture was allocated");
    if (!packet ||
        !g_readyReplyDetour)
        return 0;
    packet[6] = slot;
    packet[7] = flag;
    packet[8] = 0x5A;
    return g_readyReplyDetour(
        g_readyCommand,
        nullptr,
        4,
        5,
        6,
        0x0999,
        packet);
}

bool CompleteGuestJoin(
    RoomJoinActions& actions)
{
    actions.ResetObservations();
    SetupRoomList();
    if (!actions.JoinExactTitle(
            kTitle))
        return false;
    if (DeliverJoinReply(
            kRoomId,
            kRoomVariant) != 41)
        return false;
    InstallGuestRoom();
    return actions.JoinState().result ==
           RoomJoinActions::kJoinAccepted;
}

void TestBindGuards(
    const SdgoApi& api,
    NativeActions& native,
    RoomJoinActions& actions)
{
    g_rejectedHookSlot =
        "flow.room_join_ready_reply_slot";
    RoomJoinActions rejected;
    Check(
        !rejected.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(
                1),
            &native),
        "Bind fails closed when the verified Ready slot cannot be hooked");
    ResetPublicationReceipts();
    Check(
        g_roomPacketDetour(
            g_roomList,
            nullptr,
            0,
            0,
            0,
            0,
            nullptr) == 41 &&
            g_roomOriginalCalled,
        "a partial slot bind remains a transparent pass-through after Bind fails");
    g_rejectedHookSlot = nullptr;

    Check(
        actions.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(
                1),
            &native),
        "RoomJoinActions bound to verified callback identities and slot hooks");
    Check(
        g_roomPacketDetour &&
            g_readyReplyDetour,
        "both original packet-consumer slot detours were captured");
}

void TestRoomSelectionGuards(
    RoomJoinActions& actions)
{
    actions.ResetObservations();
    SetupRoomList();
    Check(
        !actions.JoinExactTitle(
            ""),
        "empty exact title is rejected");
    Check(
        !actions.JoinExactTitle(
            "123456789012345678901"),
        "title longer than the native 20-byte field is rejected");
    Check(
        g_joinCallbackCalls == 0,
        "invalid title boundaries never invoke native room business");

    actions.ResetObservations();
    SetupRoomList();
    Put<uint8_t*>(
        g_roomList,
        0x128,
        g_roomEntries +
            0x90 * 2);
    Put<uint32_t>(
        g_roomEntries + 0x90,
        0,
        kRoomId + 1);
    std::memcpy(
        g_roomEntries +
            0x90 +
            0x7A,
        kTitle,
        sizeof(kTitle) - 1);
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "duplicate exact titles fail closed instead of choosing a room id");

    actions.ResetObservations();
    SetupRoomList();
    Put<uint8_t*>(
        g_roomList,
        0x128,
        g_roomEntries + 1);
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "misaligned server room vector is rejected");

    actions.ResetObservations();
    SetupRoomList();
    g_rows[0][0x167] = 0;
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "disabled live GameRoom row is rejected");

    actions.ResetObservations();
    SetupRoomList();
    g_hideRequestedRow = true;
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "cached row must resolve through the actual native UI root");

    actions.ResetObservations();
    SetupRoomList();
    g_roomEntries[0x8F] = 1;
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "passworded room entry is not joined through the passwordless path");

    actions.ResetObservations();
    SetupRoomList();
    g_duplicateNode.next =
        &g_head;
    g_duplicateNode.service =
        g_duplicateRoomList;
    Put<void*>(
        g_duplicateRoomList,
        0,
        g_roomListVtable);
    g_roomNode.next =
        &g_duplicateNode;
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "duplicate live RoomList services fail closed");

    actions.ResetObservations();
    SetupRoomList();
    g_networkAccepts = false;
    Check(
        !actions.JoinExactTitle(
            kTitle),
        "failed original 0x0734 transport is rejected");
    const auto rejected =
        actions.JoinState();
    Check(
        rejected.result ==
            RoomJoinActions::
                kJoinRejected,
        "transport rejection is not exposed as a pending or accepted join");
}

void TestJoinReplyAndModel(
    RoomJoinActions& actions)
{
    actions.ResetObservations();
    SetupRoomList();
    Check(
        actions.JoinExactTitle(
            kTitle),
        "exact advertised title invokes the original row callback");
    Check(
        g_joinCallbackCalls == 1,
        "native row callback was invoked exactly once");
    const auto requested =
        actions.JoinState();
    Check(
        requested.result ==
                RoomJoinActions::
                    kJoinPending &&
            requested.requested_room_id ==
                kRoomId,
        "pending join records the room id selected from the real list vector");
    Check(
        !actions.JoinExactTitle(
            kTitle) &&
            g_joinCallbackCalls == 1,
        "pending join cannot be retried into a duplicate request");

    const uint32_t beforeSequence =
        requested.sequence;
    ResetPublicationReceipts();
    const int handled =
        DeliverJoinReply(
            kRoomId,
            kRoomVariant);
    const auto acknowledged =
        actions.JoinState();
    Check(
        handled == 41,
        "room-list slot detour preserved the original int return");
    Check(
        g_roomOriginalCalled &&
            g_roomPacketFreed,
        "original room-list handler consumed its owned packet");
    Check(
        g_joinSequenceInsideOriginal ==
                beforeSequence &&
            g_joinResultInsideOriginal ==
                RoomJoinActions::
                    kJoinPending,
        "join receipt was not published before the original handler completed");
    Check(
        acknowledged.result ==
                RoomJoinActions::
                    kJoinPending &&
            acknowledged.reply_opcode ==
                0x0550 &&
            acknowledged.
                    acknowledged_room_id ==
                kRoomId &&
            acknowledged.
                    original_handler_completed ==
                1,
        "0x0550 acknowledgement remains pending until a real room model exists");

    InstallGuestRoom();
    g_localIsHost = true;
    Check(
        actions.JoinState().result ==
            RoomJoinActions::
                kJoinPending,
        "a host model cannot satisfy guest-join acceptance");
    g_localIsHost = false;
    const auto accepted =
        actions.JoinState();
    Check(
        accepted.result ==
                RoomJoinActions::
                    kJoinAccepted &&
            accepted.model_room_id ==
                kRoomId &&
            accepted.local_is_guest == 1 &&
            accepted.native_app_state == 18,
        "join accepts only the exact guest room installed by the original client");

    actions.ResetObservations();
    SetupRoomList();
    Check(
        actions.JoinExactTitle(
            kTitle),
        "mismatched-ack fixture sent a genuine native join request");
    ResetPublicationReceipts();
    DeliverJoinReply(
        kRoomId + 7,
        kRoomVariant);
    InstallGuestRoom(
        kRoomId);
    const auto mismatch =
        actions.JoinState();
    Check(
        mismatch.result ==
                RoomJoinActions::
                    kJoinRejected &&
            mismatch.
                    acknowledged_room_id ==
                kRoomId + 7,
        "foreign or mismatched room acknowledgement never passes on model state");
}

void TestReadyReplyAndModel(
    RoomJoinActions& actions)
{
    Check(
        CompleteGuestJoin(
            actions),
        "Ready fixture completed a guest join through ACK and native model");
    Put<uint32_t>(
        g_manager,
        0x20,
        0x100u +
            kLocalSlot);
    Check(
        !actions.Ready() &&
            g_readyGetterCalls == 0 &&
            g_readyBusinessCalls == 0,
        "Ready validates the full native local-slot dword before calling its accessor");
    Put<uint32_t>(
        g_manager,
        0x20,
        kLocalSlot);

    g_nativeReady = 2;
    Check(
        !actions.Ready() &&
            g_readyBusinessCalls == 0,
        "non-boolean native readiness fails closed before Ready business");

    g_nativeReady = 1;
    Check(
        !actions.Ready() &&
            g_readyBusinessCalls == 0,
        "Ready refuses to toggle an already-ready native pilot off");
    Check(
        g_readyGetterThis ==
                g_readyStatus &&
            g_readyGetterSlot ==
                kLocalSlot &&
            g_readyGetterCalls == 2,
        "native Ready getter receives the shipped status object and full local slot");

    g_nativeReady = 0;
    Check(
        actions.Ready(),
        "Ready invokes the original guest 3D command");
    Check(
        g_readyBusinessCalls == 1,
        "original Ready business ran exactly once");
    const auto pending =
        actions.ReadyState();
    Check(
        pending.result ==
                RoomJoinActions::
                    kReadyPending &&
            pending.room_id == kRoomId,
        "Ready remains pending after real 0x05A6 transport");
    Check(
        !actions.Ready() &&
            g_readyBusinessCalls == 1,
        "pending Ready cannot issue a second toggle");

    ResetPublicationReceipts();
    const int foreignHandled =
        DeliverReadyReply(
            kLocalSlot + 1,
            1);
    const auto afterForeign =
        actions.ReadyState();
    Check(
        foreignHandled == 47 &&
            afterForeign.result ==
                RoomJoinActions::
                    kReadyPending,
        "0x0999 for another slot runs the original but cannot satisfy local Ready");

    Put<uint32_t>(
        g_readyCommand,
        8,
        2);
    ResetPublicationReceipts();
    const uint32_t beforeSequence =
        afterForeign.sequence;
    const int handled =
        DeliverReadyReply(
            kLocalSlot,
            1);
    const auto accepted =
        actions.ReadyState();
    Check(
        handled == 47,
        "Ready slot detour preserved the original int return");
    Check(
        g_readyOriginalCalled &&
            g_readyPacketFreed,
        "original local Ready consumer updated state and consumed its packet");
    Check(
        g_readySequenceInsideOriginal ==
                beforeSequence &&
            g_readyResultInsideOriginal ==
                RoomJoinActions::
                    kReadyPending,
        "Ready acceptance was not published before the original consumer completed");
    Check(
        accepted.result ==
                RoomJoinActions::
                    kReadyAccepted &&
            accepted.reply_opcode ==
                0x0999 &&
            accepted.local_slot ==
                kLocalSlot &&
            accepted.ready_flag == 1 &&
            accepted.
                    original_handler_completed ==
                1 &&
            accepted.model_ready == 1,
        "local 0x0999 passes only after native readiness reads back true");

    Check(
        CompleteGuestJoin(
            actions),
        "Ready rejection fixture completed a fresh guest join");
    g_nativeReady = false;
    Check(
        actions.Ready(),
        "Ready rejection fixture sent the original 0x05A6");
    Put<uint32_t>(
        g_readyCommand,
        8,
        2);
    ResetPublicationReceipts();
    DeliverReadyReply(
        kLocalSlot,
        0);
    const auto rejected =
        actions.ReadyState();
    Check(
        rejected.result ==
                RoomJoinActions::
                    kReadyRejected &&
            rejected.ready_flag == 0 &&
            rejected.model_ready == 0,
        "server unready notification is not reported as Ready success");

    Check(
        CompleteGuestJoin(
            actions),
        "Ready host-gate fixture completed a fresh guest join");
    g_localIsHost = true;
    Check(
        !actions.Ready(),
        "Ready refuses the host branch and cannot send battle-start by mistake");
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: RoomJoinActions Win32 ABI/state regression; no game, server, or database");

    g_roomListVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeRoomJoinCallback);
    g_readyCommandVtable[
        0x08 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeReadyBusiness);
    g_readyStatusVtable[
        0x28 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeReadyGetter);

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;
    api.HookSlot = &ApiHookSlot;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "offline-room";
    config.room_mode = 1;
    config.map_id = 0;

    NativeActions native;
    Check(
        native.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(
                1),
            config),
        "NativeActions transport observer bound to offline stubs");
    Check(
        g_networkSendDetour != nullptr,
        "NativeActions captured original transport for request receipts");

    RoomJoinActions actions;
    g_actions = &actions;
    TestBindGuards(
        api,
        native,
        actions);
    TestRoomSelectionGuards(
        actions);
    TestJoinReplyAndModel(
        actions);
    TestReadyReplyAndModel(
        actions);

    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE ROOM-JOIN FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts(
        "OFFLINE ROOM-JOIN PASS");
    return 0;
}
