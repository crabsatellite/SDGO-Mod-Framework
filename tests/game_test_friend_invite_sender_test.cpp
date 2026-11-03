// OFFLINE-ONLY ABI checks for FriendInviteSender.
//
// The fixture models the shipped x86 object/list layouts and records calls to
// the supplied original callback/business methods. It does not start the game,
// contact a server, or treat simulated transport as native gameplay proof.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../include/sdgo/sdgo_api.h"

#define private public
#include "../mods/game_test_client/native_actions.h"
#include "../mods/game_test_client/friend_invite_sender.h"
#undef private

namespace
{
int g_failures = 0;
uint8_t g_gameApp[0x600] = {};
uint8_t g_playerDataStorage[0x900] = {};
uint8_t* g_playerData = g_playerDataStorage;
uint8_t g_friendHead[12] = {};
uint8_t g_friendNode[12] = {};
uint8_t g_conversationHead[12] = {};
uint8_t g_conversationNode[12] = {};
uint8_t g_conversation[0x40] = {};
uint8_t g_friendRecord[0x80] = {};
uint8_t g_serviceManagerObject[0x80] = {};
uint8_t g_serviceManagerHead[12] = {};
uint8_t g_serviceManagerServiceNode[12] = {};
uint8_t g_serviceManagerPopupNode[12] = {};
uint8_t g_serviceManagerDuplicateNode[12] = {};
void* g_serviceManager = g_serviceManagerObject;
uint8_t g_messengerService[0x80] = {};
uint8_t g_messenger[0x100] = {};
uint8_t g_duplicateMessenger[0x100] = {};
uint8_t g_serviceControlRoot[0x200] = {};
uint8_t g_uiLoadManagerObject[0x80] = {};
void* g_uiLoadManager = g_uiLoadManagerObject;
uint8_t g_chatRoot[0x200] = {};
uint8_t g_inviteButton[0x200] = {};
void* g_messengerServiceVtable[32] = {};
void* g_threeDMessengerServiceVtable[32] = {};
void* g_messengerVtable[32] = {};
void* g_threeDMessengerVtable[32] = {};
void* g_threeDWaitRoomVtable[16] = {};
void* g_normalThreeDManagerVtable[16] = {};
void* g_normalThreeDInviteStateVtable[16] = {};
uint8_t g_threeDWaitRoomOwner[0x40] = {};
uint8_t g_normalThreeDManager[0x80] = {};
uint8_t* g_normalThreeDStates[0x40] = {};
uint8_t g_normalThreeDInviteState[0x40] = {};
uint8_t g_normalThreeDInviteHead[12] = {};
uint8_t g_normalThreeDInviteNode[12] = {};
uint8_t g_normalThreeDQueuedRecord[0x38] = {};
uint8_t g_threeDEventQueue[0x40] = {};
const wchar_t g_inviteName[] = L"InviteBtn";

NativeActions* g_native = nullptr;
unsigned g_callbackCalls = 0;
unsigned g_businessCalls = 0;
unsigned g_threeDBusinessCalls = 0;
unsigned g_threeDOpenCalls = 0;
bool g_threeDOpenPending = false;
uint32_t g_businessRecipientId = 0;
char g_businessRecipientName[17] = {};
uint16_t g_transportOpcode = 0x077E;
uint32_t g_transportSize = 0x1E;
LONG g_transportResult = 1;
bool g_returnPopup = true;
bool g_returnMessengerService = true;
bool g_returnInviteControl = true;
bool g_threeDQueueRecord = true;
bool g_openResult = true;
bool g_uiLoadBusy = false;
bool g_openFailureStartsUiLoad = false;
unsigned g_uiLoadQueries = 0;

void LinkSingleNode(
    uint8_t* head,
    uint8_t* node,
    void* payload);
void LinkServiceManager(bool duplicatePopup = false);
void PublishTransport(
    NativeActions& native,
    uint16_t opcode,
    uint32_t size,
    LONG result);

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

const wchar_t* NativeStringData(const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(storage);
    return *reinterpret_cast<
                   const uint32_t*>(
                   bytes + 0x14) < 8
        ? reinterpret_cast<const wchar_t*>(
              bytes)
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
    *reinterpret_cast<const wchar_t**>(bytes) =
        value;
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
            ? NativeStringData(
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
            L"Messenger") == 0)
        return g_returnMessengerService
            ? g_messengerService
            : nullptr;
    if (std::wcscmp(
            name,
            L"MessengerPopup") == 0)
        return g_returnPopup
            ? g_messenger
            : nullptr;
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    const wchar_t* name =
        NativeStringData(nativeName);
    return g_returnInviteControl &&
            root == g_chatRoot &&
            kind == 4 &&
            name &&
            std::wcscmp(
                name,
                L"InviteBtn") == 0
        ? g_inviteButton
        : nullptr;
}

uint8_t* __cdecl FakeGameApp()
{
    return g_gameApp;
}

uint8_t* SelectedRecord()
{
    const int32_t selected =
        *reinterpret_cast<int32_t*>(
            g_playerDataStorage + 0x790);
    const uint32_t count =
        *reinterpret_cast<uint32_t*>(
            g_playerDataStorage + 0x7B4);
    auto* head =
        *reinterpret_cast<uint8_t**>(
            g_playerDataStorage + 0x7B0);
    if (selected < 0 ||
        static_cast<uint32_t>(selected) >=
            count ||
        !head)
        return nullptr;
    auto* node =
        *reinterpret_cast<uint8_t**>(head);
    for (int32_t index = 0;
         index < selected && node &&
         node != head;
         ++index) {
        node =
            *reinterpret_cast<uint8_t**>(
                node);
    }
    auto* payload =
        node && node != head
            ? *reinterpret_cast<uint8_t**>(
                  node + 8)
            : nullptr;
    return payload
        ? *reinterpret_cast<uint8_t**>(
              payload)
        : nullptr;
}

void __fastcall FakeInviteBusiness(
    void* self,
    void*)
{
    ++g_businessCalls;
    if (self != g_messenger ||
        !g_native)
        return;
    uint8_t* record = SelectedRecord();
    if (!record)
        return;
    g_businessRecipientId =
        *reinterpret_cast<uint32_t*>(record);
    std::memset(
        g_businessRecipientName,
        0,
        sizeof(g_businessRecipientName));
    std::memcpy(
        g_businessRecipientName,
        record + 0x0A,
        16);

    PublishTransport(
        *g_native,
        g_transportOpcode,
        g_transportSize,
        g_transportResult);
}

void __fastcall FakeThreeDInviteBusiness(
    void* self,
    void*)
{
    ++g_businessCalls;
    ++g_threeDBusinessCalls;
    if (self != g_messenger)
        return;
}

bool __fastcall FakeOpenThreeDMessenger(
    void* self,
    void*)
{
    ++g_threeDOpenCalls;
    if (self != g_messengerService)
        return false;
    if (!g_openResult &&
        g_openFailureStartsUiLoad)
        g_uiLoadBusy = true;
    g_threeDOpenPending = g_openResult;
    return g_openResult;
}

bool __fastcall FakeUiLoadInProgress(
    void* self,
    void*)
{
    ++g_uiLoadQueries;
    return self == g_uiLoadManager &&
        g_uiLoadBusy;
}

void CompleteThreeDMessengerOpen()
{
    if (!g_threeDOpenPending)
        return;
    g_threeDOpenPending = false;
    g_returnPopup = true;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        g_messenger;
    *reinterpret_cast<void**>(
        g_messenger) =
        g_threeDMessengerVtable;
    *reinterpret_cast<void**>(
        g_messenger + 0xF0) =
        g_threeDEventQueue;
}

void PublishThreeDStateRecord()
{
    if (!g_threeDQueueRecord)
        return;
    uint8_t* record = SelectedRecord();
    if (!record)
        return;

    std::memcpy(
        g_normalThreeDQueuedRecord,
        record,
        sizeof(g_normalThreeDQueuedRecord));
    LinkSingleNode(
        g_normalThreeDInviteHead,
        g_normalThreeDInviteNode,
        g_normalThreeDQueuedRecord);
    *reinterpret_cast<uint32_t*>(
        g_normalThreeDInviteState +
        0x1C) = 1;
}

void PublishTransport(
    NativeActions& native,
    uint16_t opcode,
    uint32_t size,
    LONG result)
{
    const uint32_t sequence =
        static_cast<uint32_t>(
            InterlockedCompareExchange(
                &native.transportSequence_,
                0,
                0)) +
        1;
    auto& slot =
        native.transportHistory_[
            (sequence - 1) %
            NativeActions::
                kTransportHistoryCapacity];
    InterlockedIncrement(&slot.version);
    InterlockedExchange(
        &slot.sequence,
        static_cast<LONG>(sequence));
    InterlockedExchange(
        &slot.opcode,
        opcode);
    InterlockedExchange(
        &slot.size,
        static_cast<LONG>(size));
    InterlockedExchange(
        &slot.result,
        result);
    InterlockedIncrement(&slot.version);

    InterlockedIncrement(
        &native.transportWriteVersion_);
    InterlockedExchange(
        &native.transportOpcode_,
        opcode);
    InterlockedExchange(
        &native.transportSize_,
        static_cast<LONG>(size));
    InterlockedExchange(
        &native.transportResult_,
        result);
    InterlockedExchange(
        &native.transportSequence_,
        static_cast<LONG>(sequence));
    InterlockedIncrement(
        &native.transportWriteVersion_);
}

void __stdcall FakeInviteUiCallback(
    void* popup,
    uint32_t event,
    const void* name,
    void* control)
{
    ++g_callbackCalls;
    Check(
        popup == g_messenger &&
            event == 0x101 &&
            name ==
                g_inviteButton + 0x118 &&
            control == g_inviteButton,
        "sender invoked the exact native InviteBtn callback arguments");
    auto** vtable =
        *reinterpret_cast<void***>(popup);
    auto business =
        reinterpret_cast<
            void(__thiscall*)(void*)>(
            vtable[0x70 / sizeof(void*)]);
    business(popup);
}

void LinkSingleNode(
    uint8_t* head,
    uint8_t* node,
    void* payload)
{
    *reinterpret_cast<uint8_t**>(head) =
        node;
    *reinterpret_cast<uint8_t**>(
        head + 4) = node;
    *reinterpret_cast<uint8_t**>(node) =
        head;
    *reinterpret_cast<uint8_t**>(
        node + 4) = head;
    *reinterpret_cast<void**>(
        node + 8) = payload;
}

void LinkServiceManager(bool duplicatePopup)
{
    auto* tail =
        duplicatePopup
            ? g_serviceManagerDuplicateNode
            : g_serviceManagerPopupNode;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerHead) =
        g_serviceManagerServiceNode;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerHead + 4) = tail;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerServiceNode) =
        g_serviceManagerPopupNode;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerServiceNode + 4) =
        g_serviceManagerHead;
    *reinterpret_cast<void**>(
        g_serviceManagerServiceNode + 8) =
        g_messengerService;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerPopupNode) =
        duplicatePopup
            ? g_serviceManagerDuplicateNode
            : g_serviceManagerHead;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerPopupNode + 4) =
        g_serviceManagerServiceNode;
    *reinterpret_cast<void**>(
        g_serviceManagerPopupNode + 8) =
        g_messenger;
    if (duplicatePopup) {
        *reinterpret_cast<uint8_t**>(
            g_serviceManagerDuplicateNode) =
            g_serviceManagerHead;
        *reinterpret_cast<uint8_t**>(
            g_serviceManagerDuplicateNode + 4) =
            g_serviceManagerPopupNode;
        *reinterpret_cast<void**>(
            g_serviceManagerDuplicateNode + 8) =
            g_duplicateMessenger;
    }
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerObject + 0x44) =
        g_serviceManagerHead;
}

void WriteControlName(
    uint8_t* control,
    const wchar_t* value)
{
    auto* nativeName =
        control + 0x118;
    std::memset(nativeName, 0, 0x1C);
    *reinterpret_cast<
        const wchar_t**>(nativeName) =
        value;
    *reinterpret_cast<uint32_t*>(
        nativeName + 0x10) =
        static_cast<uint32_t>(
            std::wcslen(value));
    *reinterpret_cast<uint32_t*>(
        nativeName + 0x14) = 8;
}

void ResetFixture(
    friends::FriendInviteSender& sender,
    NativeActions& native)
{
    std::memset(
        g_gameApp,
        0,
        sizeof(g_gameApp));
    std::memset(
        g_playerDataStorage,
        0,
        sizeof(g_playerDataStorage));
    std::memset(
        g_friendHead,
        0,
        sizeof(g_friendHead));
    std::memset(
        g_friendNode,
        0,
        sizeof(g_friendNode));
    std::memset(
        g_conversationHead,
        0,
        sizeof(g_conversationHead));
    std::memset(
        g_conversationNode,
        0,
        sizeof(g_conversationNode));
    std::memset(
        g_conversation,
        0,
        sizeof(g_conversation));
    std::memset(
        g_friendRecord,
        0,
        sizeof(g_friendRecord));
    std::memset(
        g_serviceManagerObject,
        0,
        sizeof(g_serviceManagerObject));
    std::memset(
        g_serviceManagerHead,
        0,
        sizeof(g_serviceManagerHead));
    std::memset(
        g_serviceManagerServiceNode,
        0,
        sizeof(g_serviceManagerServiceNode));
    std::memset(
        g_serviceManagerPopupNode,
        0,
        sizeof(g_serviceManagerPopupNode));
    std::memset(
        g_serviceManagerDuplicateNode,
        0,
        sizeof(g_serviceManagerDuplicateNode));
    std::memset(
        g_messengerService,
        0,
        sizeof(g_messengerService));
    std::memset(
        g_messenger,
        0,
        sizeof(g_messenger));
    std::memset(
        g_duplicateMessenger,
        0,
        sizeof(g_duplicateMessenger));
    std::memset(
        g_serviceControlRoot,
        0,
        sizeof(g_serviceControlRoot));
    std::memset(
        g_uiLoadManagerObject,
        0,
        sizeof(g_uiLoadManagerObject));
    std::memset(
        g_chatRoot,
        0,
        sizeof(g_chatRoot));
    std::memset(
        g_inviteButton,
        0,
        sizeof(g_inviteButton));
    std::memset(
        g_messengerServiceVtable,
        0,
        sizeof(g_messengerServiceVtable));
    std::memset(
        g_threeDMessengerServiceVtable,
        0,
        sizeof(g_threeDMessengerServiceVtable));
    std::memset(
        g_messengerVtable,
        0,
        sizeof(g_messengerVtable));
    std::memset(
        g_threeDMessengerVtable,
        0,
        sizeof(g_threeDMessengerVtable));
    std::memset(
        g_threeDWaitRoomVtable,
        0,
        sizeof(g_threeDWaitRoomVtable));
    std::memset(
        g_normalThreeDManagerVtable,
        0,
        sizeof(g_normalThreeDManagerVtable));
    std::memset(
        g_normalThreeDInviteStateVtable,
        0,
        sizeof(g_normalThreeDInviteStateVtable));
    std::memset(
        g_threeDWaitRoomOwner,
        0,
        sizeof(g_threeDWaitRoomOwner));
    std::memset(
        g_normalThreeDManager,
        0,
        sizeof(g_normalThreeDManager));
    std::memset(
        g_normalThreeDStates,
        0,
        sizeof(g_normalThreeDStates));
    std::memset(
        g_normalThreeDInviteState,
        0,
        sizeof(g_normalThreeDInviteState));
    std::memset(
        g_normalThreeDInviteHead,
        0,
        sizeof(g_normalThreeDInviteHead));
    std::memset(
        g_normalThreeDInviteNode,
        0,
        sizeof(g_normalThreeDInviteNode));
    std::memset(
        g_normalThreeDQueuedRecord,
        0,
        sizeof(g_normalThreeDQueuedRecord));
    std::memset(
        g_threeDEventQueue,
        0,
        sizeof(g_threeDEventQueue));

    g_playerData = g_playerDataStorage;
    g_serviceManager =
        g_serviceManagerObject;
    g_uiLoadManager =
        g_uiLoadManagerObject;
    g_native = &native;
    g_callbackCalls = 0;
    g_businessCalls = 0;
    g_threeDBusinessCalls = 0;
    g_threeDOpenCalls = 0;
    g_threeDOpenPending = false;
    g_businessRecipientId = 0;
    std::memset(
        g_businessRecipientName,
        0,
        sizeof(g_businessRecipientName));
    g_transportOpcode = 0x077E;
    g_transportSize = 0x1E;
    g_transportResult = 1;
    g_returnPopup = true;
    g_returnMessengerService = true;
    g_returnInviteControl = true;
    g_threeDQueueRecord = true;
    g_openResult = true;
    g_uiLoadBusy = false;
    g_openFailureStartsUiLoad = false;
    g_uiLoadQueries = 0;

    *reinterpret_cast<uint32_t*>(
        g_gameApp + 0x4A0) = 7;
    native.gameApp_ = &FakeGameApp;
    native.transportWriteVersion_ = 0;
    native.transportSequence_ = 0;
    native.transportOpcode_ = 0;
    native.transportSize_ = 0;
    native.transportResult_ = 0;
    std::memset(
        native.transportHistory_,
        0,
        sizeof(native.transportHistory_));

    *reinterpret_cast<void**>(
        g_messengerService) =
        g_messengerServiceVtable;
    g_threeDMessengerServiceVtable[
        0x68 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeOpenThreeDMessenger);
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        g_messenger;
    *reinterpret_cast<void**>(
        g_messengerService + 0x50) =
        g_serviceControlRoot;
    *reinterpret_cast<void**>(
        g_messenger) =
        g_messengerVtable;
    *reinterpret_cast<void**>(
        g_duplicateMessenger) =
        g_messengerVtable;
    LinkServiceManager();
    g_messengerVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeInviteUiCallback);
    g_messengerVtable[
        0x70 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeInviteBusiness);
    g_threeDMessengerVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeInviteUiCallback);
    g_threeDMessengerVtable[
        0x70 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeThreeDInviteBusiness);
    *reinterpret_cast<void**>(
        g_messenger + 0x54) =
        g_chatRoot;

    *reinterpret_cast<void**>(
        g_threeDWaitRoomOwner) =
        g_threeDWaitRoomVtable;
    *reinterpret_cast<void**>(
        g_threeDWaitRoomOwner + 4) =
        g_normalThreeDManager;
    *reinterpret_cast<void**>(
        g_normalThreeDManager) =
        g_normalThreeDManagerVtable;
    *reinterpret_cast<uint8_t***>(
        g_normalThreeDManager + 0x0C) =
        g_normalThreeDStates;
    g_normalThreeDStates[0x2B] =
        g_normalThreeDInviteState;
    *reinterpret_cast<void**>(
        g_normalThreeDInviteState) =
        g_normalThreeDInviteStateVtable;
    *reinterpret_cast<uint32_t*>(
        g_normalThreeDInviteState + 4) =
        0x2B;
    *reinterpret_cast<uint8_t**>(
        g_normalThreeDInviteState + 0x0C) =
        g_normalThreeDManager + 0x24;
    *reinterpret_cast<uint8_t**>(
        g_normalThreeDInviteState + 0x10) =
        g_normalThreeDManager;
    *reinterpret_cast<uint8_t**>(
        g_normalThreeDInviteState + 0x18) =
        g_normalThreeDInviteHead;
    *reinterpret_cast<uint8_t**>(
        g_normalThreeDInviteHead) =
        g_normalThreeDInviteHead;
    *reinterpret_cast<uint8_t**>(
        g_normalThreeDInviteHead + 4) =
        g_normalThreeDInviteHead;
    *reinterpret_cast<uint8_t**>(
        g_gameApp + 0x4A4) =
        g_threeDWaitRoomOwner;

    WriteControlName(
        g_inviteButton,
        g_inviteName);
    *reinterpret_cast<void**>(
        g_inviteButton + 0x15C) =
        g_chatRoot;
    g_chatRoot[0x162] = 1;
    g_inviteButton[0x162] = 1;
    g_inviteButton[0x163] = 1;
    g_inviteButton[0x167] = 1;

    *reinterpret_cast<uint32_t*>(
        g_friendRecord) = 4242;
    std::memcpy(
        g_friendRecord + 0x0A,
        "FriendAlpha",
        12);
    g_friendRecord[0x1B] = 7;
    LinkSingleNode(
        g_friendHead,
        g_friendNode,
        g_friendRecord);
    *reinterpret_cast<uint8_t**>(
        g_playerDataStorage + 0x7A4) =
        g_friendHead;
    *reinterpret_cast<uint32_t*>(
        g_playerDataStorage + 0x7A8) = 1;

    *reinterpret_cast<uint8_t**>(
        g_conversation) =
        g_friendRecord;
    LinkSingleNode(
        g_conversationHead,
        g_conversationNode,
        g_conversation);
    *reinterpret_cast<int32_t*>(
        g_playerDataStorage + 0x790) = 0;
    *reinterpret_cast<uint32_t*>(
        g_playerDataStorage + 0x788) = 2;
    *reinterpret_cast<uint8_t**>(
        g_playerDataStorage + 0x7B0) =
        g_conversationHead;
    *reinterpret_cast<uint32_t*>(
        g_playerDataStorage + 0x7B4) = 1;

    sender.Reset();
}

void SetThreeDRoomFixture()
{
    *reinterpret_cast<uint32_t*>(
        g_gameApp + 0x4A0) = 18;
    *reinterpret_cast<void**>(
        g_messenger) =
        g_threeDMessengerVtable;
    *reinterpret_cast<void**>(
        g_messengerService) =
        g_threeDMessengerServiceVtable;
    *reinterpret_cast<void**>(
        g_messengerService + 0x60) =
        g_threeDEventQueue;
    *reinterpret_cast<void**>(
        g_messenger + 0xF0) =
        g_threeDEventQueue;
}

void* LookupSymbol(const char* name)
{
    if (std::strcmp(
            name,
            "friends.player_data_pointer") == 0)
        return &g_playerData;
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
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeUiFindControl);
    if (std::strcmp(
            name,
            "game.app") == 0)
        return reinterpret_cast<void*>(
            &FakeGameApp);
    if (std::strcmp(
            name,
            "flow.three_d_wait_room_vtable") == 0)
        return g_threeDWaitRoomVtable;
    if (std::strcmp(
            name,
            "flow.normal_3d_manager_vtable") == 0)
        return g_normalThreeDManagerVtable;
    if (std::strcmp(
            name,
            "friends.messenger_vtable") == 0)
        return g_messengerVtable;
    if (std::strcmp(
            name,
            "friends.three_d_messenger_vtable") == 0)
        return g_threeDMessengerVtable;
    if (std::strcmp(
            name,
            "friends.messenger_service_vtable") == 0)
        return g_messengerServiceVtable;
    if (std::strcmp(
            name,
            "friends.three_d_messenger_service_vtable") ==
        0)
        return g_threeDMessengerServiceVtable;
    if (std::strcmp(
            name,
            "friends.open_three_d_messenger") == 0)
        return reinterpret_cast<void*>(
            &FakeOpenThreeDMessenger);
    if (std::strcmp(
            name,
            "friends.ui_load_manager_pointer") == 0)
        return &g_uiLoadManager;
    if (std::strcmp(
            name,
            "friends.ui_load_in_progress") == 0)
        return reinterpret_cast<void*>(
            &FakeUiLoadInProgress);
    if (std::strcmp(
            name,
            "friends.invite_ui_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeInviteUiCallback);
    if (std::strcmp(
            name,
            "friends.invite_sender_business") == 0)
        return reinterpret_cast<void*>(
            &FakeInviteBusiness);
    if (std::strcmp(
            name,
            "friends.three_d_invite_sender_business") ==
        0)
        return reinterpret_cast<void*>(
            &FakeThreeDInviteBusiness);
    if (std::strcmp(
            name,
            "friends.normal_3d_invite_state_vtable") ==
        0)
        return g_normalThreeDInviteStateVtable;
    return nullptr;
}

void* ApiSymbol(
    SdgoMod*,
    const char* name)
{
    return LookupSymbol(name);
}

void TestSuccessfulNativePath(
    friends::FriendInviteSender& sender,
    NativeActions& native)
{
    ResetFixture(sender, native);
    const auto result =
        sender.Send(
            4242,
            "FriendAlpha");
    const auto state = sender.State();
    Check(
        result ==
            friends::FriendInviteSender::
                kRequestSent,
        "accepted selected friend reached the original invitation path");
    Check(
        g_callbackCalls == 1 &&
            g_businessCalls == 1,
        "sender used the original callback and its vtable business method exactly once");
    Check(
        g_businessRecipientId == 4242 &&
            std::strcmp(
                g_businessRecipientName,
                "FriendAlpha") == 0,
        "original business method consumed the exact selected recipient record");
    Check(
        state.native_app_state == 7 &&
            state.selected_conversation == 0 &&
            state.conversation_count == 1 &&
            state.recipient_id == 4242 &&
            std::strcmp(
                state.recipient_name,
                "FriendAlpha") == 0 &&
            state.relationship == 7,
        "state retained the exact native context and accepted recipient identity");
    Check(
        state.messenger_identity_matched == 1 &&
            state.selected_target_matched == 1 &&
            state.accepted_friend_matched == 1 &&
            state.invite_control_found == 1 &&
            state.invite_control_name_matched == 1 &&
            state.invite_control_parent_matched == 1 &&
            state.invite_control_visible == 1 &&
            state.invite_control_enabled == 1 &&
            state.invite_control_hit_testable == 1 &&
            state.callback_slot_matched == 1 &&
            state.business_slot_matched == 1,
        "state proves the live MessengerPopup and InviteBtn identities");
    Check(
        state.transport_sequence_before == 0 &&
            state.transport_sequence_after == 1 &&
            state.request_opcode == 0x077E &&
            state.request_size == 0x1E &&
            state.transport_result == 1 &&
            state.request_sent == 1 &&
            state.recipient_correlated == 1,
        "existing transport observation correlated the selected recipient with the original 0x077E request");
}

void TestStateAndRecipientGuards(
    friends::FriendInviteSender& sender,
    NativeActions& native)
{
    ResetFixture(sender, native);
    *reinterpret_cast<uint32_t*>(
        g_gameApp + 0x4A0) = 20;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kInviteContextUnavailable &&
            g_callbackCalls == 0,
        "non-invitation native app state cannot bypass the shipped invitation gates");

    ResetFixture(sender, native);
    Check(
        sender.Send(
            4242,
            "OtherPilot") ==
                friends::FriendInviteSender::
                    kSelectedTargetMismatch &&
            g_callbackCalls == 0,
        "selected conversation name must match the requested recipient");

    ResetFixture(sender, native);
    g_friendRecord[0x1B] = 6;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kFriendNotAccepted &&
            g_callbackCalls == 0,
        "non-accepted relationship cannot be invited");

    ResetFixture(sender, native);
    *reinterpret_cast<uint8_t**>(
        g_playerDataStorage + 0x7A4) =
        nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kFriendNotAccepted &&
            g_callbackCalls == 0,
        "stale conversation record must also exist in the accepted friend list");
}

void TestMessengerAndControlGuards(
    friends::FriendInviteSender& sender,
    NativeActions& native)
{
    ResetFixture(sender, native);
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kMessengerUnavailable &&
            g_callbackCalls == 0,
        "background Messenger must own the exact named MessengerPopup");

    ResetFixture(sender, native);
    *reinterpret_cast<void**>(
        g_inviteButton + 0x15C) =
        g_messenger;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kInviteControlMismatch &&
            g_callbackCalls == 0,
        "InviteBtn with a foreign parent is rejected");

    ResetFixture(sender, native);
    g_inviteButton[0x163] = 0;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kInviteControlNotReady &&
            g_callbackCalls == 0,
        "disabled InviteBtn is not invoked");

    ResetFixture(sender, native);
    g_inviteButton[0x167] = 0;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kInviteControlNotReady &&
            g_callbackCalls == 0,
        "non-hit-testable InviteBtn is not invoked");

    ResetFixture(sender, native);
    g_messengerVtable[
        0x70 / sizeof(void*)] = nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kMessengerUnavailable &&
            g_callbackCalls == 0,
        "changed invitation business slot fails closed");
}

void TestThreeDRoomNativePath(
    friends::FriendInviteSender& sender,
    NativeActions& native)
{
    ResetFixture(sender,native);
    SetThreeDRoomFixture();
    g_inviteButton[0x162]=0;
    Check(sender.Send(4242,"FriendAlpha")==friends::FriendInviteSender::kRequestPending && g_callbackCalls==0,
          "a newly opened 3D popup waits for its own native visibility update without forcing hidden controls");
    g_inviteButton[0x162]=1;
    Check(sender.Poll()==friends::FriendInviteSender::kRequestPending && g_callbackCalls==1,
          "native visibility admits exactly one original 3D invitation callback");
    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    g_uiLoadBusy = true;
    const auto pending =
        sender.Send(
            4242,
            "FriendAlpha");
    const auto pendingState = sender.State();
    Check(
        pending ==
            friends::FriendInviteSender::
                kRequestPending,
        "state-18 InviteBtn waits for the native 3D event dispatcher");
    Check(
        g_callbackCalls == 1 &&
            g_businessCalls == 1 &&
            g_threeDBusinessCalls == 1,
        "state-18 path used the shared native callback and 3D popup business method");
    Check(
        pendingState.native_app_state == 18 &&
            pendingState.three_d_room_path == 1 &&
            pendingState.three_d_queue_matched == 1 &&
            pendingState.request_queued == 0 &&
            pendingState.queued_record_matched == 0 &&
            pendingState.request_sent == 0 &&
            pendingState.ui_load_busy == 1 &&
            pendingState.
                    messenger_waited_for_ui_loader == 0 &&
            pendingState.
                    messenger_waiting_for_ui_loader ==
                0 &&
            pendingState.messenger_open_attempts == 0,
        "state-18 receipt keeps the exact manager guard without inventing a synchronous state record");
    Check(
        *reinterpret_cast<uint32_t*>(
            g_normalThreeDInviteState +
            0x1C) == 0,
        "3D business event has not yet been misreported as the later state queue");
    Check(
        sender.Poll() ==
            friends::FriendInviteSender::
                kRequestPending,
        "3D invitation remains pending until the native room-state tick sends it");

    PublishThreeDStateRecord();
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            sender.State().request_queued == 1 &&
            sender.State().
                    queued_record_matched == 1,
        "poll records the later byte-exact state-owned friend copy when it is observable");

    PublishTransport(
        native,
        0x077E,
        0x1E,
        1);
    PublishTransport(
        native,
        0x067B,
        8,
        1);
    const auto sent = sender.Poll();
    const auto sentState = sender.State();
    Check(
        sent ==
            friends::FriendInviteSender::
                kRequestSent &&
            sentState.request_sent == 1 &&
            sentState.recipient_correlated == 1 &&
            sentState.request_opcode == 0x077E &&
            sentState.request_size == 0x1E,
        "transport history finds the deferred native 0x077E even after unrelated traffic");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    g_returnPopup = false;
    g_uiLoadBusy = true;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 0 &&
            g_uiLoadQueries == 1 &&
            sender.State().
                    ui_load_manager_present == 1 &&
            sender.State().ui_load_busy == 1 &&
            sender.State().
                    messenger_waited_for_ui_loader == 1 &&
            sender.State().
                    messenger_waiting_for_ui_loader ==
                1 &&
            sender.State().
                    messenger_open_attempts == 0,
        "busy native UI loader defers the original opener without mutating the invitation path");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 0 &&
            sender.State().
                    messenger_open_attempts == 0,
        "poll observes a still-busy loader without repeatedly opening Messenger");
    g_uiLoadBusy = false;
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_threeDOpenPending &&
            sender.State().ui_load_busy == 0 &&
            sender.State().
                    messenger_waited_for_ui_loader == 1 &&
            sender.State().
                    messenger_waiting_for_ui_loader ==
                0 &&
            sender.State().
                    messenger_open_attempts == 1 &&
            sender.State().messenger_open_result == 1,
        "idle transition invokes the original opener exactly once after the native loader completes");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_callbackCalls == 0,
        "successful opener waits for managed registration without a double-open");
    CompleteThreeDMessengerOpen();
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_callbackCalls == 1,
        "busy-to-idle registration resumes the preserved invitation target once");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    g_returnPopup = false;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        nullptr;
    const auto opening =
        sender.Send(
            4242,
            "FriendAlpha");
    const auto openingState = sender.State();
    Check(
        opening ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_threeDOpenPending &&
            g_callbackCalls == 0 &&
            openingState.
                    messenger_service_found == 1 &&
            openingState.
                    messenger_service_class_matched ==
                1 &&
            openingState.messenger_popup_found == 0 &&
            openingState.
                    messenger_open_requested == 1 &&
            openingState.messenger_open_result == 1 &&
            openingState.player_data_found == 1 &&
            openingState.player_ui_phase == 2 &&
            openingState.selected_conversation == 0 &&
            openingState.conversation_count == 1 &&
            openingState.
                    messenger_manager_list_valid == 1 &&
            openingState.
                    structural_popup_matches == 1 &&
            openingState.structural_popup_address ==
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(
                        g_messenger)) &&
            openingState.messenger_owner_popup == 0 &&
            openingState.
                    three_d_service_event_queue ==
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(
                        g_threeDEventQueue)) &&
            openingState.
                    messenger_service_control_root ==
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(
                        g_serviceControlRoot)),
        "missing named state-18 popup records the original opener result, player gate, structural manager scan and raw service fields");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_callbackCalls == 0,
        "poll waits for delayed managed popup registration without reopening");
    CompleteThreeDMessengerOpen();
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_callbackCalls == 1 &&
            sender.State().recipient_id == 4242 &&
            sender.State().
                    messenger_popup_found == 1 &&
            sender.State().
                    messenger_popup_class_matched == 1 &&
            sender.State().
                    messenger_owner_popup_matched == 1 &&
            sender.State().
                    three_d_event_queue_matched == 1,
        "delayed popup ownership resumes the preserved target before the native callback");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    g_returnPopup = false;
    g_openResult = false;
    g_openFailureStartsUiLoad = true;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            sender.State().
                    messenger_open_result == 0 &&
            sender.State().ui_load_busy == 1 &&
            sender.State().
                    messenger_waited_for_ui_loader == 1 &&
            sender.State().
                    messenger_waiting_for_ui_loader ==
                1 &&
            sender.State().
                    messenger_open_attempts == 1,
        "loader race after a false factory return enables one bounded native opener retry");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1,
        "loader race remains pending without reopening while the native worker is active");
    g_uiLoadBusy = false;
    g_openResult = true;
    g_openFailureStartsUiLoad = false;
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 2 &&
            sender.State().
                    messenger_open_attempts == 2 &&
            sender.State().messenger_open_result == 1,
        "loader race retries the original opener once after the worker becomes idle");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 2,
        "bounded loader recovery never performs a third opener call");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    g_returnPopup = false;
    g_openResult = false;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
        nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kMessengerUnavailable &&
            g_threeDOpenCalls == 1 &&
            sender.State().
                    messenger_open_requested == 1 &&
            sender.State().
                    messenger_open_result == 0 &&
            sender.State().
                    ui_load_manager_present == 1 &&
            sender.State().ui_load_busy == 0 &&
            sender.State().
                    messenger_waited_for_ui_loader == 0 &&
            sender.State().
                    messenger_waiting_for_ui_loader ==
                0 &&
            sender.State().
                    messenger_open_attempts == 1,
        "a false original opener result fails immediately instead of entering an impossible registration wait");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kMessengerUnavailable &&
            g_threeDOpenCalls == 1 &&
            sender.State().
                    messenger_open_result == 0,
        "poll preserves the terminal false opener result without repeating the original entry");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    *reinterpret_cast<void**>(
        g_duplicateMessenger) =
        g_threeDMessengerVtable;
    LinkServiceManager(true);
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kRequestPending &&
            sender.State().
                    messenger_manager_list_valid == 1 &&
            sender.State().
                    structural_popup_matches == 2 &&
            sender.State().
                    structural_popup_address == 0,
        "bounded structural scan reports duplicate 3D popup identities without selecting either one");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    *reinterpret_cast<void**>(
        g_messengerService) =
        g_messengerServiceVtable;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kMessengerUnavailable &&
            g_threeDOpenCalls == 0 &&
            g_callbackCalls == 0,
        "state-18 path rejects the ordinary Messenger service identity");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    *reinterpret_cast<void**>(
        g_messenger + 0xF0) =
        nullptr;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_callbackCalls == 0 &&
            sender.State().
                    three_d_event_queue_matched ==
                0,
        "partially registered 3D popup waits for its service-owned event queue");
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_threeDOpenCalls == 1 &&
            g_callbackCalls == 0,
        "incomplete 3D popup ownership does not repeat the native opener");
    *reinterpret_cast<void**>(
        g_messenger + 0xF0) =
        g_threeDEventQueue;
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_callbackCalls == 1 &&
            sender.State().
                    three_d_event_queue_matched ==
                1,
        "poll resumes only after the 3D popup event queue is fully owned");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    *reinterpret_cast<void**>(
        g_normalThreeDInviteState) =
        g_messengerVtable;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kThreeDQueueUnavailable &&
            g_callbackCalls == 0,
        "changed normal-3D InviteFriend state identity fails before the UI callback");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    g_threeDQueueRecord = false;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kRequestPending &&
            g_callbackCalls == 1 &&
            g_threeDBusinessCalls == 1,
        "3D callback remains pending while its asynchronous state record is not yet observable");
    PublishTransport(
        native,
        0x077E,
        0x1E,
        1);
    Check(
        sender.Poll() ==
                friends::FriendInviteSender::
                    kRequestSent &&
            sender.State().request_queued == 0 &&
            sender.State().
                    queued_record_matched == 0 &&
            sender.State().
                    recipient_correlated == 1,
        "real 0x077E transport completes the exact native callback lineage without treating an event as sent");

    ResetFixture(sender, native);
    SetThreeDRoomFixture();
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
            friends::FriendInviteSender::
                kRequestPending,
        "second 3D fixture reached the deferred native state");
    PublishTransport(
        native,
        0x077E,
        29,
        1);
    Check(
        sender.Poll() ==
            friends::FriendInviteSender::
                kNativeRequestNotSent &&
            sender.State().request_size == 29,
        "deferred 3D receipt rejects a malformed native invitation size");
}

void TestTransportGuards(
    friends::FriendInviteSender& sender,
    NativeActions& native)
{
    ResetFixture(sender, native);
    g_transportOpcode = 0x067B;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kNativeRequestNotSent &&
            g_callbackCalls == 1 &&
            g_businessCalls == 1 &&
            sender.State().
                    recipient_correlated == 0,
        "another native opcode cannot satisfy the room invitation receipt");

    ResetFixture(sender, native);
    g_transportSize = 29;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kNativeRequestNotSent &&
            sender.State().request_size == 29,
        "wrong transport size preserves the observed failure");

    ResetFixture(sender, native);
    g_transportResult = 0;
    Check(
        sender.Send(
            4242,
            "FriendAlpha") ==
                friends::FriendInviteSender::
                    kNativeRequestNotSent &&
            sender.State().
                    transport_result == 0,
        "native transport rejection is not converted into success");
}
} // namespace

int main()
{
    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    auto* mod =
        reinterpret_cast<SdgoMod*>(1);
    NativeActions native;
    friends::FriendInviteSender sender;
    Check(
        sender.Bind(
            &api,
            mod,
            &native),
        "friend invitation sender bound all exact native symbols");
    TestSuccessfulNativePath(
        sender,
        native);
    TestStateAndRecipientGuards(
        sender,
        native);
    TestMessengerAndControlGuards(
        sender,
        native);
    TestThreeDRoomNativePath(
        sender,
        native);
    TestTransportGuards(
        sender,
        native);
    return g_failures == 0 ? 0 : 1;
}
