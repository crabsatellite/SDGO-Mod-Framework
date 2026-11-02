// OFFLINE-ONLY checks for the native friend action adapter.
//
// The fixture models the shipped x86 list layouts. It does not start the
// client, contact a server, or treat transport simulation as gameplay proof.

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <utility>

#include "../include/sdgo/sdgo_api.h"

#define private public
#include "../mods/game_test_client/native_actions.h"
#include "../mods/game_test_client/friend_actions.h"
#undef private
#include "../mods/game_test_client/friend_session.h"

namespace
{
const wchar_t* FixtureName() { return GetACP()==936 ? L"\x6D4B\x8BD5\x597D\x53CB" : L"TestFriend"; }
const wchar_t* FixtureBody() { return GetACP()==936 ? L"\x4F60\x597D\xFF0C\x670B\x53CB" : L"Hello friend"; }
int g_failures = 0;
uint8_t g_playerDataStorage[0x900] = {};
uint8_t* g_playerData = g_playerDataStorage;
uint8_t g_conversationHead[12] = {};
uint8_t g_conversationNode[12] = {};
uint8_t g_conversation[0x80] = {};
uint8_t g_friendRecord[0x80] = {};
uint8_t g_friendHead[12] = {};
uint8_t g_friendNode[12] = {};
uint8_t g_messenger[0x100] = {};
uint8_t g_messengerService[0x60] = {};
uint8_t g_gameApp[0x4A4] = {};
uint8_t g_network[0x60] = {};
uint8_t g_networkStream = 0;
uint8_t g_edit = 0;
uint8_t g_addRoot = 0;
uint8_t g_pendingRoot = 0;
uint8_t g_addOkButton[0x170] = {};
uint8_t g_pendingAcceptButton[0x170] = {};
uint8_t g_pendingRejectButton[0x170] = {};
uint8_t g_friendTree[0x244] = {};
uint8_t g_treeRootNode[0x50] = {};
uint8_t g_treeCategoryNode[0x50] = {};
uint8_t g_treeFriendNode[0x50] = {};

uint8_t g_addService[0xB0] = {};
uint8_t g_pendingService[0xB0] = {};
uint8_t g_duplicatePendingService[0xB0] = {};
void* g_addVtable[16] = {};
void* g_pendingVtable[16] = {};
void* g_messengerVtable[4] = {};
void* g_messengerServiceVtable[4] = {};
uint8_t g_serviceManagerObject[0x80] = {};
uint8_t g_serviceHead[12] = {};
uint8_t g_serviceNodes[3][12] = {};
void* g_serviceManager = g_serviceManagerObject;
wchar_t g_resolvedServiceName[64] = {};
wchar_t g_editText[79] = {};
char g_addTarget[17] = {};
uint8_t g_observedAddPacket[0x30] = {};
int g_observedAddPacketSize = 0;
unsigned g_observedAddPackets = 0;

NativeActions* g_native = nullptr;
friends::FriendActions* g_actions = nullptr;
unsigned g_sendCalls = 0;
unsigned g_dispatchCalls = 0;
unsigned g_initialListCalls = 0;
unsigned g_namedLookupCalls = 0;
unsigned g_openMessengerCalls = 0;
unsigned g_openAddCalls = 0;
unsigned g_acceptCalls = 0;
unsigned g_rejectCalls = 0;
unsigned g_addCallbackCalls = 0;
unsigned g_pendingCallbackCalls = 0;
unsigned g_treeFindCalls = 0;
unsigned g_treeSelectCalls = 0;
unsigned g_openSelectedCalls = 0;
void* g_lastTreeFindRoot = nullptr;
void* g_lastTreeFindRecord = nullptr;
void* g_lastSelectedTree = nullptr;
void* g_lastSelectedNode = nullptr;
bool g_treeForceWrongNode = false;
bool g_popupRegistered = true;
bool g_openMessengerRegisters = true;

uint8_t* __cdecl FakeGameApp()
{
    return g_gameApp;
}

void __cdecl FakeInitialListRequest()
{
    ++g_initialListCalls;
    *reinterpret_cast<uint32_t*>(g_playerDataStorage + 0x788) = 1;
}
unsigned g_addCalls = 0;
unsigned g_editSetCalls = 0;
unsigned g_acceptedValidatorCalls = 0;
unsigned g_pendingValidatorCalls = 0;
bool g_validatorResult = false;

enum AddScenario
{
    kNoValidationBranch,
    kAcceptedDuplicate,
    kAcceptedAllowed,
    kAcceptedMismatchedTarget,
    kPendingDuplicate,
    kSelfTarget,
    kAcceptedThenTransport,
    kAcceptedThenTransportThenRoomList,
};

AddScenario g_addScenario = kNoValidationBranch;
alignas(16) uint8_t g_beforeFx[512] = {};
alignas(16) uint8_t g_afterFx[512] = {};
alignas(16) const uint32_t g_xmmValues[4] = {
    0x11223344,
    0x55667788,
    0x99AABBCC,
    0xDDEEFF00,
};
uint32_t g_afterRegisters[7] = {};
DWORD g_beforeFlags = 0;
DWORD g_afterFlags = 0;
uint8_t g_selfFrame[0x80] = {};

__declspec(naked) void SelfBridgeResume()
{
    __asm { ret }
}

__declspec(naked) void __stdcall InvokeSelfBridge(
    void*,
    uint8_t*)
{
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ebp, [esp + 24]
        fninit
        fld1
        fldpi
        movdqu xmm0, xmmword ptr [g_xmmValues]
        fxsave [g_beforeFx]
        mov eax, 11111111h
        mov ebx, 22222222h
        mov ecx, 33333333h
        mov edx, 44444444h
        mov esi, 11111111h
        mov edi, 77777777h
        stc
        std
        pushfd
        pop dword ptr [g_beforeFlags]
        call dword ptr [esp + 20]
        mov dword ptr [g_afterRegisters + 0], eax
        mov dword ptr [g_afterRegisters + 4], ebx
        mov dword ptr [g_afterRegisters + 8], ecx
        mov dword ptr [g_afterRegisters + 12], edx
        mov dword ptr [g_afterRegisters + 16], esi
        mov dword ptr [g_afterRegisters + 20], edi
        mov dword ptr [g_afterRegisters + 24], ebp
        pushfd
        pop dword ptr [g_afterFlags]
        cld
        fxsave [g_afterFx]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret 8
    }
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

const wchar_t* ReadNativeString(const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes = static_cast<const uint8_t*>(storage);
    return *reinterpret_cast<const uint32_t*>(bytes + 0x14) < 8
        ? reinterpret_cast<const wchar_t*>(bytes)
        : *reinterpret_cast<wchar_t* const*>(bytes);
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    auto* bytes = static_cast<uint8_t*>(self);
    std::memset(bytes, 0, 0x1C);
    *reinterpret_cast<const wchar_t**>(bytes) = value;
    *reinterpret_cast<uint32_t*>(bytes + 0x10) =
        static_cast<uint32_t>(std::wcslen(value));
    *reinterpret_cast<uint32_t*>(bytes + 0x14) = 8;
    return self;
}

void __fastcall FakeNativeWStringDtor(void*, void*) {}

void SetServiceList(
    void* first,
    void* second,
    void* third);

void* __fastcall FakeServiceFindByName(
    void* manager,
    void*,
    const void* descriptor)
{
    ++g_namedLookupCalls;
    const auto* bytes =
        static_cast<const uint8_t*>(descriptor);
    const wchar_t* name =
        bytes
            ? ReadNativeString(bytes + sizeof(uint32_t))
            : nullptr;
    if (name)
        wcsncpy_s(g_resolvedServiceName, name, _TRUNCATE);
    const bool valid =
        manager == g_serviceManager && bytes &&
        *reinterpret_cast<const uint32_t*>(bytes) == 1 &&
        name;
    if (!valid)
        return nullptr;
    if (std::wcscmp(name, L"Messenger") == 0)
        return g_messengerService;
    if (std::wcscmp(name, L"MessengerPopup") == 0)
        return g_popupRegistered ? g_messenger : nullptr;
    return nullptr;
}

void* __fastcall FakeNativeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t type)
{
    const wchar_t* name = ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (root == &g_addRoot) {
        if (type == 8 &&
            std::wcscmp(name, L"IdEdit") == 0)
            return &g_edit;
        if (type == 4 &&
            std::wcscmp(name, L"OkBtn") == 0)
            return g_addOkButton;
    }
    if (root == &g_pendingRoot && type == 4) {
        if (std::wcscmp(name, L"Btn0") == 0)
            return g_pendingAcceptButton;
        if (std::wcscmp(name, L"Btn1") == 0)
            return g_pendingRejectButton;
    }
    if (root == g_friendTree &&
        type == 0x12 &&
        std::wcscmp(name, L"FriendTree") == 0)
        return g_friendTree;
    return nullptr;
}

void __fastcall FakeUiEditSetText(
    void* edit,
    void*,
    const void* text,
    int)
{
    ++g_editSetCalls;
    const wchar_t* value = ReadNativeString(text);
    if (edit == &g_edit && value) {
        wcsncpy_s(g_editText, value, _TRUNCATE);
        BOOL usedDefault = FALSE;
        WideCharToMultiByte(
            CP_ACP,
            GetACP() == CP_UTF8
                ? WC_ERR_INVALID_CHARS
                : WC_NO_BEST_FIT_CHARS,
            value,
            -1,
            g_addTarget,
            sizeof(g_addTarget),
            nullptr,
            GetACP() == CP_UTF8
                ? nullptr
                : &usedDefault);
    }
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void* network,
    void*,
    void* stream,
    const void* packet,
    int size,
    char)
{
    if (network != g_network ||
        stream != &g_networkStream ||
        !packet ||
        size < static_cast<int>(sizeof(uint16_t)))
        return 0;
    const uint16_t opcode =
        *static_cast<const uint16_t*>(packet);
    if (opcode == 0x0829 &&
        size == static_cast<int>(
                    sizeof(g_observedAddPacket))) {
        std::memcpy(
            g_observedAddPacket,
            packet,
            sizeof(g_observedAddPacket));
        g_observedAddPacketSize = size;
        ++g_observedAddPackets;
    }
    return 1;
}

void RecordPacketTransport(const void* packet, int size)
{
    if (!g_native || !packet || size <= 0)
        return;
    NativeActions::active_ = g_native;
    NativeActions::ObserveNetworkSend(
        g_network,
        nullptr,
        &g_networkStream,
        packet,
        size,
        0);
}

void RecordTransport(uint16_t opcode, LONG size)
{
    if (size < static_cast<LONG>(sizeof(opcode)) ||
        size > 0x100)
        return;
    uint8_t packet[0x100] = {};
    *reinterpret_cast<uint16_t*>(packet) = opcode;
    RecordPacketTransport(packet, size);
}

void RecordAddRequestTransport()
{
    uint8_t packet[0x30] = {};
    *reinterpret_cast<uint16_t*>(packet) = 0x0829;
    std::memcpy(packet + 0x0E, "Owner", sizeof("Owner"));
    std::memcpy(
        packet + 0x1F,
        g_addTarget,
        sizeof(g_addTarget));
    RecordPacketTransport(packet, sizeof(packet));
}

void __fastcall FakeSendMessage(void* self, void*)
{
    ++g_sendCalls;
    if (self != g_messenger || !g_native)
        return;
    RecordTransport(0x0560, 0x95);
}

bool __fastcall FakeAcceptedValidator(
    void*,
    void*,
    const void*)
{
    ++g_acceptedValidatorCalls;
    return g_validatorResult;
}

bool __fastcall FakePendingValidator(
    void*,
    void*,
    const char*)
{
    ++g_pendingValidatorCalls;
    return g_validatorResult;
}

bool __fastcall FakeAddFriend(void* self, void*)
{
    ++g_addCalls;
    if (self != g_addService || !g_actions)
        return false;
    alignas(4) uint8_t nativeTarget[0x1C] = {};
    switch (g_addScenario) {
    case kAcceptedDuplicate:
    case kAcceptedAllowed:
    case kAcceptedThenTransport:
    case kAcceptedThenTransportThenRoomList:
        FakeNativeWStringCtor(
            nativeTarget,
            nullptr,
            g_editText);
        {
            const bool result =
                friends::FriendActions::
                    ObserveAcceptedDuplicate(
                        self,
                        nullptr,
                        nativeTarget);
            if (g_addScenario == kAcceptedThenTransport ||
                g_addScenario ==
                    kAcceptedThenTransportThenRoomList) {
                RecordAddRequestTransport();
            }
            if (g_addScenario ==
                kAcceptedThenTransportThenRoomList)
                RecordTransport(0x07FB, 0x16);
            return result;
        }
    case kAcceptedMismatchedTarget:
        FakeNativeWStringCtor(
            nativeTarget,
            nullptr,
            L"Different");
        return friends::FriendActions::
            ObserveAcceptedDuplicate(
                self,
                nullptr,
                nativeTarget);
    case kPendingDuplicate:
        return friends::FriendActions::
            ObservePendingDuplicate(
                self,
                nullptr,
                g_addTarget);
    case kSelfTarget:
        friends::FriendActions::
            ObserveSelfComparisonMatch(
                g_addTarget,
                g_addTarget);
        return false;
    default:
        return false;
    }
}

void __fastcall FakeOpenAddDialog(void*, void*)
{
    ++g_openAddCalls;
}

void __fastcall FakeOpenMessenger(void* self, void*)
{
    ++g_openMessengerCalls;
    if (self == g_messengerService &&
        g_openMessengerRegisters) {
        g_popupRegistered = true;
        *reinterpret_cast<void**>(
            g_messengerService + 0x48) =
                g_messenger;
    }
}

void __fastcall FakeAcceptPending(void* self, void*)
{
    ++g_acceptCalls;
    if (self == g_pendingService ||
        self == g_duplicatePendingService)
        RecordTransport(0x0645, 0x32);
}

void __fastcall FakeRejectPending(void* self, void*)
{
    ++g_rejectCalls;
    if (self == g_pendingService ||
        self == g_duplicatePendingService)
        RecordTransport(0x0737, 0x32);
}

uint32_t __stdcall FakeAddDialogCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_addCallbackCalls;
    const wchar_t* name = ReadNativeString(nativeName);
    if (service == g_addService &&
        event == 0x101 &&
        name &&
        std::wcscmp(name, L"OkBtn") == 0 &&
        control == g_addOkButton) {
        (void)FakeAddFriend(service, nullptr);
        SetServiceList(
            g_pendingService,
            nullptr,
            nullptr);
    }
    return 0;
}

uint32_t __stdcall FakePendingDialogCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_pendingCallbackCalls;
    const wchar_t* name = ReadNativeString(nativeName);
    if (event != 0x101 || !name)
        return 0;
    if (std::wcscmp(name, L"Btn0") == 0 &&
        control == g_pendingAcceptButton)
        FakeAcceptPending(service, nullptr);
    else if (std::wcscmp(name, L"Btn1") == 0 &&
             control == g_pendingRejectButton)
        FakeRejectPending(service, nullptr);
    else
        return 0;
    SetServiceList(g_addService, nullptr, nullptr);
    return 0;
}

void* __fastcall FakeTreeFindByData(
    void* node,
    void*,
    void* record)
{
    if (g_treeFindCalls++ == 0) {
        g_lastTreeFindRoot = node;
        g_lastTreeFindRecord = record;
    }
    if (g_treeForceWrongNode && node == g_treeRootNode)
        return g_treeCategoryNode;
    if (*reinterpret_cast<void**>(
            static_cast<uint8_t*>(node) + 0x34) ==
        record)
        return node;
    if (node == g_treeRootNode)
        return FakeTreeFindByData(
            g_treeCategoryNode,
            nullptr,
            record);
    if (node == g_treeCategoryNode)
        return FakeTreeFindByData(
            g_treeFriendNode,
            nullptr,
            record);
    return nullptr;
}

void* __fastcall FakeTreeItemData(
    void* node,
    void*)
{
    return node
        ? *reinterpret_cast<void**>(
              static_cast<uint8_t*>(node) + 0x34)
        : nullptr;
}

void __fastcall FakeTreeSelect(
    void* tree,
    void*,
    void* node)
{
    ++g_treeSelectCalls;
    g_lastSelectedTree = tree;
    g_lastSelectedNode = node;
}

void __fastcall FakeOpenSelectedFriend(
    void* messenger,
    void*)
{
    if (messenger == g_messenger)
        ++g_openSelectedCalls;
}

int __fastcall FakeMessengerDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    ++g_dispatchCalls;
    return 73;
}

bool EncodeNative(
    const wchar_t* value,
    char* output,
    int capacity)
{
    BOOL usedDefault = FALSE;
    const int written = WideCharToMultiByte(
        CP_ACP,
        GetACP()==CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
        value,
        -1,
        output,
        capacity,
        nullptr,
        GetACP()==CP_UTF8 ? nullptr : &usedDefault);
    return written > 1 && !usedDefault;
}

void LinkSingleNode(
    uint8_t* head,
    uint8_t* node,
    void* payload)
{
    *reinterpret_cast<uint8_t**>(head) = node;
    *reinterpret_cast<uint8_t**>(head + 4) = node;
    *reinterpret_cast<uint8_t**>(node) = head;
    *reinterpret_cast<uint8_t**>(node + 4) = head;
    *reinterpret_cast<void**>(node + 8) = payload;
}

void SetServiceList(
    void* first,
    void* second = nullptr,
    void* third = nullptr)
{
    std::memset(g_serviceHead, 0, sizeof(g_serviceHead));
    std::memset(g_serviceNodes, 0, sizeof(g_serviceNodes));
    std::memset(
        g_serviceManagerObject,
        0,
        sizeof(g_serviceManagerObject));
    *reinterpret_cast<uint8_t**>(g_serviceHead) =
        g_serviceHead;
    *reinterpret_cast<uint8_t**>(g_serviceHead + 4) =
        g_serviceHead;
    *reinterpret_cast<uint8_t**>(
        g_serviceManagerObject + 0x44) =
            g_serviceHead;

    void* services[] = {first, second, third};
    uint8_t* previous = g_serviceHead;
    for (size_t index = 0; index < 3; ++index) {
        if (!services[index])
            continue;
        uint8_t* node = g_serviceNodes[index];
        *reinterpret_cast<uint8_t**>(node + 4) = previous;
        *reinterpret_cast<void**>(node + 8) =
            services[index];
        *reinterpret_cast<uint8_t**>(previous) = node;
        previous = node;
    }
    *reinterpret_cast<uint8_t**>(previous) =
        g_serviceHead;
    *reinterpret_cast<uint8_t**>(g_serviceHead + 4) =
        previous;
}

friends::FriendActions::Command MakeSendCommand()
{
    friends::FriendActions::Command command = {};
    command.sequence = 41;
    command.operation = friends::FriendActions::kSendMessage;
    command.target_id = 4242;
    EncodeNative(
        FixtureName(),
        command.target_name,
        sizeof(command.target_name));
    EncodeNative(
        FixtureBody(),
        command.body,
        sizeof(command.body));
    return command;
}

friends::FriendActions::Command MakeAddCommand()
{
    friends::FriendActions::Command command = {};
    command.sequence = 42;
    command.operation = friends::FriendActions::kAddByName;
    EncodeNative(
        FixtureName(),
        command.target_name,
        sizeof(command.target_name));
    return command;
}

friends::FriendActions::Command MakePendingCommand(
    uint32_t operation)
{
    friends::FriendActions::Command command = {};
    command.sequence = 43;
    command.operation = operation;
    command.target_id = 4242;
    EncodeNative(
        FixtureName(),
        command.target_name,
        sizeof(command.target_name));
    return command;
}

void ResetFixture(
    friends::FriendActions& actions,
    NativeActions& native)
{
    std::memset(
        g_playerDataStorage,
        0,
        sizeof(g_playerDataStorage));
    std::memset(g_conversationHead, 0, sizeof(g_conversationHead));
    std::memset(g_conversationNode, 0, sizeof(g_conversationNode));
    std::memset(g_conversation, 0, sizeof(g_conversation));
    std::memset(g_friendRecord, 0, sizeof(g_friendRecord));
    std::memset(g_friendHead, 0, sizeof(g_friendHead));
    std::memset(g_friendNode, 0, sizeof(g_friendNode));
    std::memset(g_messenger, 0, sizeof(g_messenger));
    std::memset(
        g_messengerService,
        0,
        sizeof(g_messengerService));
    std::memset(g_gameApp, 0, sizeof(g_gameApp));
    std::memset(g_network, 0, sizeof(g_network));
    std::memset(g_addOkButton, 0, sizeof(g_addOkButton));
    std::memset(
        g_pendingAcceptButton,
        0,
        sizeof(g_pendingAcceptButton));
    std::memset(
        g_pendingRejectButton,
        0,
        sizeof(g_pendingRejectButton));
    std::memset(g_friendTree, 0, sizeof(g_friendTree));
    std::memset(g_treeRootNode, 0, sizeof(g_treeRootNode));
    std::memset(
        g_treeCategoryNode,
        0,
        sizeof(g_treeCategoryNode));
    std::memset(
        g_treeFriendNode,
        0,
        sizeof(g_treeFriendNode));
    std::memset(g_addService, 0, sizeof(g_addService));
    std::memset(g_pendingService, 0, sizeof(g_pendingService));
    std::memset(
        g_duplicatePendingService,
        0,
        sizeof(g_duplicatePendingService));
    std::memset(g_resolvedServiceName, 0, sizeof(g_resolvedServiceName));
    std::memset(g_editText, 0, sizeof(g_editText));
    std::memset(g_addTarget, 0, sizeof(g_addTarget));
    std::memset(
        g_observedAddPacket,
        0,
        sizeof(g_observedAddPacket));
    g_observedAddPacketSize = 0;
    g_observedAddPackets = 0;
    g_playerData = g_playerDataStorage;
    g_serviceManager = &g_serviceManagerObject;
    g_native = &native;
    g_actions = &actions;
    g_sendCalls = 0;
    g_dispatchCalls = 0;
    g_addCalls = 0;
    g_editSetCalls = 0;
    g_namedLookupCalls = 0;
    g_openMessengerCalls = 0;
    g_openAddCalls = 0;
    g_acceptCalls = 0;
    g_rejectCalls = 0;
    g_addCallbackCalls = 0;
    g_pendingCallbackCalls = 0;
    g_treeFindCalls = 0;
    g_treeSelectCalls = 0;
    g_openSelectedCalls = 0;
    g_lastTreeFindRoot = nullptr;
    g_lastTreeFindRecord = nullptr;
    g_lastSelectedTree = nullptr;
    g_lastSelectedNode = nullptr;
    g_treeForceWrongNode = false;
    g_acceptedValidatorCalls = 0;
    g_pendingValidatorCalls = 0;
    g_validatorResult = false;
    g_addScenario = kNoValidationBranch;
    g_popupRegistered = true;
    g_openMessengerRegisters = true;

    native.transportWriteVersion_ = 0;
    native.transportSequence_ = 0;
    native.transportOpcode_ = 0;
    native.transportSize_ = 0;
    native.transportResult_ = 0;
    native.transportPublishGuard_ = 0;
    std::memset(
        native.transportHistory_,
        0,
        sizeof(native.transportHistory_));
    native.gameApp_ = &FakeGameApp;
    native.network_ = &FakeNetwork;
    native.networkSend_ = reinterpret_cast<
        NativeActions::NetworkSendFn>(
            &FakeNetworkSend);
    native.networkObserverInstalled_ = true;
    NativeActions::active_ = &native;
    *reinterpret_cast<uint32_t*>(
        g_gameApp + 0x4A0) = 6;
    *reinterpret_cast<void**>(g_network + 0x14) =
        &g_networkStream;

    *reinterpret_cast<void**>(g_messenger) =
        g_messengerVtable;
    *reinterpret_cast<void**>(g_messengerService) =
        g_messengerServiceVtable;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
            g_messenger;
    *reinterpret_cast<void**>(g_addService) =
        g_addVtable;
    *reinterpret_cast<void**>(g_pendingService) =
        g_pendingVtable;
    *reinterpret_cast<void**>(g_duplicatePendingService) =
        g_pendingVtable;
    *reinterpret_cast<void**>(g_addService + 0x4C) =
        &g_addRoot;
    *reinterpret_cast<void**>(g_addService + 0x50) =
        &g_edit;
    *reinterpret_cast<void**>(g_addService + 0x58) =
        g_addOkButton;
    *reinterpret_cast<void**>(g_pendingService + 0x4C) =
        &g_pendingRoot;
    *reinterpret_cast<void**>(
        g_duplicatePendingService + 0x4C) =
            &g_pendingRoot;
    g_addVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeAddDialogCallback);
    g_pendingVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakePendingDialogCallback);
    *reinterpret_cast<void**>(
        g_addOkButton + 0x15C) = &g_addRoot;
    g_addOkButton[0x162] = 1;
    g_addOkButton[0x163] = 1;
    g_addOkButton[0x167] = 1;
    *reinterpret_cast<void**>(
        g_pendingAcceptButton + 0x15C) =
            &g_pendingRoot;
    g_pendingAcceptButton[0x162] = 1;
    g_pendingAcceptButton[0x163] = 1;
    g_pendingAcceptButton[0x167] = 1;
    *reinterpret_cast<void**>(
        g_pendingRejectButton + 0x15C) =
            &g_pendingRoot;
    g_pendingRejectButton[0x162] = 1;
    g_pendingRejectButton[0x163] = 1;
    g_pendingRejectButton[0x167] = 1;
    SetServiceList(g_addService, g_pendingService);
    *reinterpret_cast<uint32_t*>(g_friendRecord) = 4242;
    EncodeNative(
        FixtureName(),
        reinterpret_cast<char*>(g_friendRecord + 0x0A),
        17);
    LinkSingleNode(
        g_friendHead,
        g_friendNode,
        g_friendRecord);
    *reinterpret_cast<uint8_t**>(
        g_playerDataStorage + 0x7A4) =
            g_friendHead;
    *reinterpret_cast<uint32_t*>(
        g_playerDataStorage + 0x7A8) = 1;
    *reinterpret_cast<uint8_t**>(g_conversation) =
        g_friendRecord;
    LinkSingleNode(
        g_conversationHead,
        g_conversationNode,
        g_conversation);
    *reinterpret_cast<int32_t*>(
        g_playerDataStorage + 0x790) = 0;
    *reinterpret_cast<uint8_t**>(
        g_playerDataStorage + 0x7B0) =
            g_conversationHead;
    *reinterpret_cast<uint32_t*>(
        g_playerDataStorage + 0x7B4) = 1;
    *reinterpret_cast<void**>(g_messenger + 0x84) = &g_edit;
    *reinterpret_cast<void**>(g_messenger + 0x50) =
        g_friendTree;
    *reinterpret_cast<void**>(
        g_friendTree + 0x21C) =
            g_treeRootNode;
    *reinterpret_cast<void**>(
        g_treeFriendNode + 0x34) =
            g_friendRecord;

    actions.Reset();
    actions.bound_ = true;
    actions.native_ = &native;
    actions.playerDataPointer_ = &g_playerData;
    actions.nativeWStringCtor_ = reinterpret_cast<
        friends::FriendActions::NativeWStringCtorFn>(
            &FakeNativeWStringCtor);
    actions.nativeWStringDtor_ = reinterpret_cast<
        friends::FriendActions::NativeWStringDtorFn>(
            &FakeNativeWStringDtor);
    actions.uiEditSetText_ = reinterpret_cast<
        friends::FriendActions::UiEditSetTextFn>(
            &FakeUiEditSetText);
    actions.sendMessage_ = reinterpret_cast<
        friends::FriendActions::SimpleBusinessFn>(
            &FakeSendMessage);
    actions.openMessenger_ = reinterpret_cast<
        friends::FriendActions::SimpleBusinessFn>(
            &FakeOpenMessenger);
    actions.openAddDialog_ = reinterpret_cast<
        friends::FriendActions::SimpleBusinessFn>(
            &FakeOpenAddDialog);
    actions.acceptedDuplicateValidator_ =
        reinterpret_cast<
            friends::FriendActions::
                NativeWStringValidatorFn>(
                    &FakeAcceptedValidator);
    actions.pendingDuplicateValidator_ =
        reinterpret_cast<
            friends::FriendActions::AnsiValidatorFn>(
                &FakePendingValidator);
    actions.nativeUiFindControl_ = reinterpret_cast<
        friends::FriendActions::NativeUiFindControlFn>(
            &FakeNativeUiFindControl);
    actions.treeFindByData_ = reinterpret_cast<
        friends::FriendActions::TreeFindByDataFn>(
            &FakeTreeFindByData);
    actions.treeItemData_ = reinterpret_cast<
        friends::FriendActions::TreeItemDataFn>(
            &FakeTreeItemData);
    actions.treeSelect_ = reinterpret_cast<
        friends::FriendActions::TreeSelectFn>(
            &FakeTreeSelect);
    actions.openSelectedFriend_ = reinterpret_cast<
        friends::FriendActions::SimpleBusinessFn>(
            &FakeOpenSelectedFriend);
    actions.serviceManagedPointer_ = &g_serviceManager;
    actions.serviceFindByName_ = reinterpret_cast<
        friends::FriendActions::ServiceFindByNameFn>(
            &FakeServiceFindByName);
    actions.messengerVtable_ = g_messengerVtable;
    actions.messengerServiceVtable_ =
        g_messengerServiceVtable;
    actions.addDialogVtable_ = g_addVtable;
    actions.pendingDialogVtable_ = g_pendingVtable;
    *reinterpret_cast<uint16_t*>(actions.pendingPacket_) =
        0x0829;
    *reinterpret_cast<uint32_t*>(
        actions.pendingPacket_ + 2) = 9001;
    *reinterpret_cast<uint32_t*>(
        actions.pendingPacket_ + 6) = 4242;
    EncodeNative(
        FixtureName(),
        reinterpret_cast<char*>(
            actions.pendingPacket_ + 0x0E),
        17);
    EncodeNative(
        L"Owner",
        reinterpret_cast<char*>(
            actions.pendingPacket_ + 0x1F),
        17);
    std::memcpy(
        g_pendingService + 0x60,
        actions.pendingPacket_,
        sizeof(actions.pendingPacket_));
    std::memcpy(
        g_duplicatePendingService + 0x60,
        actions.pendingPacket_,
        sizeof(actions.pendingPacket_));
    actions.observation_.pending.available = 1;
    actions.observation_.pending.sender_id = 4242;
    actions.observation_.pending.recipient_id = 9001;
    EncodeNative(
        FixtureName(),
        actions.observation_.pending.sender,
        sizeof(actions.observation_.pending.sender));
    EncodeNative(
        L"Owner",
        actions.observation_.pending.recipient,
        sizeof(actions.observation_.pending.recipient));
    friends::FriendActions::active_ = &actions;
}

void TestMessengerOpenLifecycle(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    g_popupRegistered = false;
    g_openMessengerRegisters = false;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) = nullptr;
    friends::FriendActions::Command add = {};
    add.operation = friends::FriendActions::kAddByName;
    EncodeNative(FixtureName(), add.target_name, sizeof(add.target_name));
    Check(
        actions.Execute(add) == friends::FriendActions::kWaitingForMessenger &&
            g_openMessengerCalls == 1,
        "an Add command reopens a missing MessengerPopup through its original service");
    Check(
        actions.Execute(add) == friends::FriendActions::kWaitingForMessenger &&
            g_openMessengerCalls == 1,
        "an Add command waits for the native UI clone without repeating its opener");

    g_popupRegistered = true;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
            g_messenger;
    Check(
        actions.ResolveOrOpenMessenger(true) ==
                g_messenger &&
            g_openMessengerCalls == 1,
        "registered MessengerPopup is accepted only through the owning service pointer");

    ResetFixture(actions, native);
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) =
            g_addService;
    Check(
        actions.ResolveOrOpenMessenger(false) == nullptr &&
            g_openMessengerCalls == 0,
        "a named popup that differs from Messenger service +0x48 is rejected");

    ResetFixture(actions, native);
    g_popupRegistered = false;
    *reinterpret_cast<void**>(
        g_messengerService + 0x48) = nullptr;
    Check(
        actions.ResolveOrOpenMessenger(true) ==
                g_messenger &&
            g_openMessengerCalls == 1,
        "the source-proven opener registers and returns the owned MessengerPopup");
}

void TestDialogIdentityAndUniqueness(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    auto pending = MakePendingCommand(
        friends::FriendActions::kAcceptPending);
    Check(
        actions.ResolveAddDialog() == g_addService &&
            actions.ResolvePendingDialog(pending) ==
                g_pendingService,
        "unnamed add and pending dialogs resolve by their distinct exact classes");
    auto guards = actions.State();
    Check(
        guards.add_dialog_list_valid == 1 &&
            guards.add_dialog_class_matches == 1 &&
            guards.add_dialog_root_ready == 1 &&
            guards.add_dialog_edit_ready == 1 &&
            guards.pending_dialog_list_valid == 1 &&
            guards.pending_dialog_class_matches == 1 &&
            guards.pending_dialog_packet_matches == 1 &&
            guards.pending_dialog_target_matches == 1 &&
            guards.pending_dialog_root_ready == 1 &&
            guards.pending_dialog_sender_id == 4242 &&
            guards.pending_dialog_recipient_id == 9001,
        "dialog guard diagnostics expose bounded list, class, packet, target, readiness, and IDs");
    Check(
        g_namedLookupCalls == 0 &&
            g_resolvedServiceName[0] == L'\0',
        "dialog lookup does not treat UIR panel or message-target text as a service name");

    SetServiceList(
        g_addService,
        g_pendingService,
        g_duplicatePendingService);
    Check(
        actions.ResolvePendingDialog(pending) == nullptr,
        "ambiguous matching pending dialogs are rejected");
    guards = actions.State();
    Check(
        guards.pending_dialog_list_valid == 1 &&
            guards.pending_dialog_class_matches == 2 &&
            guards.pending_dialog_packet_matches == 0,
        "ambiguous pending-dialog diagnostics stop before packet matching");

    SetServiceList(g_addService, g_pendingService);
    *reinterpret_cast<uint8_t**>(
        g_serviceNodes[1] + 4) =
            g_serviceHead;
    Check(
        actions.ResolvePendingDialog(pending) == nullptr,
        "a structurally invalid manager list cannot yield a dialog");
    guards = actions.State();
    Check(
        guards.pending_dialog_list_valid == 0,
        "invalid manager-list diagnostics remain fail closed");
}

void TestAddDialogOneShotLifecycle(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    SetServiceList(g_pendingService);
    auto command = MakeAddCommand();
    Check(
        actions.AddFriend(g_messenger, command) ==
                friends::FriendActions::kWaitingForMessenger &&
            g_openAddCalls == 1,
        "missing add dialog invokes its native opener once");
    Check(
        actions.AddFriend(g_messenger, command) ==
                friends::FriendActions::kWaitingForMessenger &&
            g_openAddCalls == 1,
        "asynchronous add-dialog registration waits without duplicate opens");

    SetServiceList(g_addService, g_pendingService);
    g_addScenario = kAcceptedThenTransport;
    Check(
        actions.AddFriend(g_messenger, command) ==
                friends::FriendActions::kRequestSent &&
            g_openAddCalls == 1 &&
            g_addCalls == 1 &&
            g_addCallbackCalls == 1 &&
            actions.ResolveAddDialog() == nullptr,
        "the registered ready add dialog uses OkBtn callback transport and native close lifecycle");
}

void TestAddCompletesAtRequestSent(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    g_addScenario = kAcceptedThenTransport;
    const auto command = MakeAddCommand();
    Check(
        actions.Submit(command) ==
            friends::FriendActions::kWaitingForMessenger,
        "add command enters the bounded main-thread adapter");
    actions.Tick();
    auto state = actions.State();
    Check(
        state.result == friends::FriendActions::kRequestSent &&
            state.request_sent == 1 &&
            state.request_opcode == 0x0829 &&
            g_addCallbackCalls == 1 &&
            !actions.commandPending_ &&
            !actions.commandStarted_,
        "add completes and releases its command lock at verified 0829 transport");
    Check(
        friends::IsFriendCommandTerminal(state),
        "friend session publishes add RequestSent without waiting for peer decision");

    uint8_t lateReply[0x32] = {};
    *reinterpret_cast<uint16_t*>(lateReply + 0x30) = 0x1407;
    actions.messengerDispatch_ = reinterpret_cast<
        friends::FriendActions::MessengerDispatchFn>(
            &FakeMessengerDispatch);
    friends::FriendActions::active_ = &actions;
    friends::FriendActions::ObserveMessengerDispatch(
        g_messenger,
        nullptr,
        0,
        0,
        0,
        0x0737,
        lateReply);
    state = actions.State();
    Check(
        state.result == friends::FriendActions::kRequestSent &&
            state.reply_opcode == 0x0737 &&
            state.reply_status == 0x1407 &&
            state.reply_result == 73 &&
            state.reply_consumed == 0,
        "late peer decision remains latched without changing RequestSent into acceptance");

    auto next = MakeSendCommand();
    ++next.sequence;
    Check(
        actions.Submit(next) ==
                friends::FriendActions::kWaitingForMessenger &&
            actions.State().reply_opcode == 0 &&
            actions.State().reply_status == 0 &&
            actions.State().reply_result == 0,
        "the next command clears the previously latched late reply");
}

void TestAddTransportHistorySelection(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    g_addScenario =
        kAcceptedThenTransportThenRoomList;
    g_validatorResult = false;
    const auto command = MakeAddCommand();
    const auto result =
        actions.AddFriend(g_messenger, command);
    const auto state = actions.State();
    const auto latest = native.TransportState();
    const auto request =
        native.FindTransportSince(0, 0x0829);
    Check(
        result == friends::FriendActions::kRequestSent &&
            state.request_sent == 1 &&
            state.request_opcode == 0x0829 &&
            state.request_transport_sequence ==
                request.send_sequence &&
            request.packet_size == 0x30 &&
            request.result == 1 &&
            latest.opcode == 0x07FB &&
            latest.send_sequence >
                request.send_sequence,
        "add selects the observed 0829 request when an independent 07FB send becomes latest");
    Check(
        state.local_validation_rejected == 0 &&
            state.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationNone,
        "the matching 0829 history receipt suppresses the staged duplicate rejection");
    Check(
        g_observedAddPackets == 1 &&
            g_observedAddPacketSize == 0x30 &&
            *reinterpret_cast<const uint16_t*>(
                g_observedAddPacket) == 0x0829 &&
            std::strcmp(
                reinterpret_cast<const char*>(
                    g_observedAddPacket + 0x1F),
                command.target_name) == 0,
        "the source-shaped 0829 packet carries the requested friend in its recipient field");
}

void TestUiButtonGuards(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    g_addScenario = kAcceptedThenTransport;
    g_addOkButton[0x163] = 0;
    const auto add = MakeAddCommand();
    Check(
        actions.AddFriend(
            g_messenger,
            add) ==
                friends::FriendActions::
                    kWaitingForMessenger &&
            g_addCallbackCalls == 0 &&
            g_addCalls == 0 &&
            g_editSetCalls == 1 &&
            actions.State().add_button_match == 1 &&
            actions.State().add_button_parent == 1 &&
            actions.State().add_button_visible == 1 &&
            actions.State().add_button_enabled == 0 &&
            actions.State().add_button_hit_test == 1 &&
            actions.State().add_callback_invoked == 0,
        "disabled Add OkBtn waits after one text assignment and reports native guards");
    g_addOkButton[0x163] = 1;
    Check(
        actions.AddFriend(g_messenger, add) ==
                friends::FriendActions::kRequestSent &&
            g_editSetCalls == 1 &&
            g_addCallbackCalls == 1 &&
            g_addCalls == 1 &&
            actions.State().add_button_enabled == 1 &&
            actions.State().add_button_hit_test == 1 &&
            actions.State().add_callback_invoked == 1,
        "restoring the real enabled guard invokes the original callback without retyping");

    ResetFixture(actions, native);
    g_pendingRejectButton[0x167] = 0;
    Check(
        actions.RespondToPending(
            MakePendingCommand(
                friends::FriendActions::kRejectPending),
            false) ==
                friends::FriendActions::
                    kWaitingForMessenger &&
            g_pendingCallbackCalls == 0 &&
            g_rejectCalls == 0 &&
            actions.State().pending_button_match == 1 &&
            actions.State().pending_button_parent == 1 &&
            actions.State().pending_button_visible == 1 &&
            actions.State().pending_button_enabled == 1 &&
            actions.State().pending_button_hit_test == 0 &&
            actions.State().pending_callback_invoked == 0,
        "a pending Btn1 without mouse hit testing waits and reports separate native guards");

    ResetFixture(actions, native);
    *reinterpret_cast<void**>(
        g_pendingAcceptButton + 0x15C) =
            &g_addRoot;
    Check(
        actions.RespondToPending(
            MakePendingCommand(
                friends::FriendActions::kAcceptPending),
            true) ==
                friends::FriendActions::
                    kWaitingForMessenger &&
            g_pendingCallbackCalls == 0 &&
            g_acceptCalls == 0 &&
            actions.State().pending_button_match == 1 &&
            actions.State().pending_button_parent == 0 &&
            actions.State().pending_callback_invoked == 0,
        "a pending Btn0 outside the dialog root waits fail closed");
}

void TestPendingPacketSemantics(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    uint8_t packet[0x30] = {};
    std::memcpy(packet, actions.pendingPacket_, sizeof(packet));
    actions.observation_.pending = {};
    actions.messengerDispatch_ = reinterpret_cast<
        friends::FriendActions::MessengerDispatchFn>(
            &FakeMessengerDispatch);
    friends::FriendActions::active_ = &actions;
    friends::FriendActions::ObserveMessengerDispatch(
        g_messenger,
        nullptr,
        0,
        0,
        0,
        0x0829,
        packet);
    const auto state = actions.State();
    Check(
        state.pending.available == 1 &&
            state.pending.sender_id == 4242 &&
            state.pending.recipient_id == 9001 &&
            std::memcmp(
                actions.pendingPacket_,
                packet,
                sizeof(packet)) == 0,
        "0829 observer maps +2 recipient and +6 sender while preserving raw bytes");
    static_assert(
        sizeof(actions.pendingPacket_) == 0x30,
        "0829 dialog identity is the shipped twelve-DWORD payload");
    Check(
        std::strcmp(
            state.pending.sender,
            reinterpret_cast<const char*>(packet + 0x0E)) == 0 &&
            std::strcmp(
                state.pending.recipient,
                reinterpret_cast<const char*>(packet + 0x1F)) == 0,
        "0829 sender and recipient names retain their native packet fields");
}

void TestPendingTargetValidation(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    auto command = MakePendingCommand(
        friends::FriendActions::kAcceptPending);
    Check(
        actions.RespondToPending(command, true) ==
                friends::FriendActions::kRequestSent &&
            g_acceptCalls == 1 &&
            g_rejectCalls == 0 &&
            g_pendingCallbackCalls == 1 &&
            !actions.commandPending_ &&
            !actions.commandStarted_ &&
            actions.State().pending.available == 1 &&
            actions.ResolvePendingDialog(command) == nullptr &&
            friends::IsFriendCommandTerminal(actions.State()),
        "accept uses Btn0 callback transport and removes the closed native dialog");

    ResetFixture(actions, native);
    command = MakePendingCommand(
        friends::FriendActions::kRejectPending);
    Check(
        actions.RespondToPending(command, false) ==
                friends::FriendActions::kRequestSent &&
            g_acceptCalls == 0 &&
            g_rejectCalls == 1 &&
            g_pendingCallbackCalls == 1 &&
            !actions.commandPending_ &&
            !actions.commandStarted_ &&
            actions.State().pending.available == 1 &&
            actions.ResolvePendingDialog(command) == nullptr &&
            friends::IsFriendCommandTerminal(actions.State()),
        "reject uses Btn1 callback transport, closes the dialog, and retains request state until server reply");

    uint8_t decisionReply[0x32] = {};
    actions.messengerDispatch_ = reinterpret_cast<
        friends::FriendActions::MessengerDispatchFn>(
            &FakeMessengerDispatch);
    friends::FriendActions::active_ = &actions;
    friends::FriendActions::ObserveMessengerDispatch(
        g_messenger,
        nullptr,
        0,
        0,
        0,
        0x0737,
        decisionReply);
    Check(
        actions.State().pending.available == 0,
        "a real native decision callback clears the retained pending request");

    uint8_t secondRequest[0x30] = {};
    std::memcpy(
        secondRequest,
        actions.pendingPacket_,
        sizeof(secondRequest));
    *reinterpret_cast<uint32_t*>(
        secondRequest + 6) = 5252;
    std::memset(secondRequest + 0x0E, 0, 17);
    std::memcpy(
        secondRequest + 0x0E,
        "SecondFriend",
        sizeof("SecondFriend"));
    std::memcpy(
        g_duplicatePendingService + 0x60,
        secondRequest,
        sizeof(secondRequest));
    friends::FriendActions::ObserveMessengerDispatch(
        g_messenger,
        nullptr,
        0,
        0,
        0,
        0x0829,
        secondRequest);
    SetServiceList(
        g_addService,
        g_duplicatePendingService);
    command = {};
    command.sequence = 44;
    command.operation =
        friends::FriendActions::kRejectPending;
    command.target_id = 5252;
    std::memcpy(
        command.target_name,
        "SecondFriend",
        sizeof("SecondFriend"));
    const unsigned callbacksBefore =
        g_pendingCallbackCalls;
    Check(
        actions.RespondToPending(command, false) ==
                friends::FriendActions::kRequestSent &&
            g_pendingCallbackCalls ==
                callbacksBefore + 1 &&
            g_rejectCalls == 2 &&
            actions.State()
                    .pending_dialog_class_matches == 1,
        "a second incoming request resolves one new dialog after the first native close");

    ResetFixture(actions, native);
    command = MakePendingCommand(
        friends::FriendActions::kRejectPending);
    command.target_id = 9999;
    Check(
        actions.RespondToPending(command, false) ==
                friends::FriendActions::kInvalidTarget &&
            g_acceptCalls == 0 &&
            g_rejectCalls == 0,
        "pending response rejects a command for a different sender");

    ResetFixture(actions, native);
    command = MakePendingCommand(
        friends::FriendActions::kRejectPending);
    g_pendingService[0x60 + 0x0E] ^= 1;
    Check(
        actions.RespondToPending(command, false) ==
                friends::FriendActions::kWaitingForMessenger &&
            g_rejectCalls == 0,
        "pending response waits instead of acting on a stale dialog packet");
}

void TestFriendTreePayloadLookup(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    friends::FriendActions::Command command = {};
    command.sequence = 45;
    command.operation =
        friends::FriendActions::kOpenConversation;
    command.target_id = 4242;
    EncodeNative(
        FixtureName(),
        command.target_name,
        sizeof(command.target_name));
    actions.observation_.friend_count = 0;
    Check(
        actions.OpenConversation(
            g_messenger,
            command) ==
                friends::FriendActions::kReady &&
            g_treeFindCalls == 3 &&
            g_lastTreeFindRoot == g_treeRootNode &&
            g_lastTreeFindRecord == g_friendRecord &&
            g_treeSelectCalls == 1 &&
            g_lastSelectedTree == g_friendTree &&
            g_lastSelectedNode == g_treeFriendNode &&
            g_openSelectedCalls == 1,
        "conversation open finds a nested FriendTree node by payload without a visible-row or friend-count loop");

    ResetFixture(actions, native);
    g_treeForceWrongNode = true;
    Check(
        actions.OpenConversation(
            g_messenger,
            command) ==
                friends::FriendActions::kFriendNotFound &&
            g_treeSelectCalls == 0 &&
            g_openSelectedCalls == 0,
        "FriendTree lookup validates the returned node payload before selection");

    ResetFixture(actions, native);
    *reinterpret_cast<void**>(
        g_friendTree + 0x21C) = nullptr;
    Check(
        actions.OpenConversation(
            g_messenger,
            command) ==
                friends::FriendActions::kFriendNotFound &&
            g_treeFindCalls == 0,
        "FriendTree lookup fails closed when the native root node is absent");
}

void TestListPageMatchedReplyBaseline(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    actions.observation_.reply_sequence = 138;
    actions.matchedReplySequence_ = 3;
    friends::FriendActions::Command command = {};
    command.sequence = 46;
    command.operation =
        friends::FriendActions::kRequestListPage;
    command.page = 2;
    Check(
        actions.Submit(command) ==
            friends::FriendActions::kWaitingForMessenger,
        "list-page command enters the main-thread adapter");
    actions.Tick();
    Check(
        actions.State().result ==
                friends::FriendActions::kRequestSent &&
            actions.State().request_sent == 1 &&
            actions.State().request_opcode == 0x0697 &&
            actions.replyBaseline_ == 3,
        "0697 arms its baseline from matched replies even when total reply count is much larger");

    actions.messengerDispatch_ = reinterpret_cast<
        friends::FriendActions::MessengerDispatchFn>(
            &FakeMessengerDispatch);
    friends::FriendActions::active_ = &actions;
    friends::FriendActions::ObserveMessengerDispatch(
        g_messenger,
        nullptr,
        0,
        0,
        0,
        0x9999,
        nullptr);
    actions.Tick();
    Check(
        actions.State().result ==
                friends::FriendActions::kWaitingForReply &&
            actions.matchedReplySequence_ == 3,
        "an unrelated reply does not complete the active list-page request");

    uint8_t pageReply[11] = {};
    pageReply[6] = 1;
    *reinterpret_cast<uint32_t*>(
        pageReply + 7) = 1;
    friends::FriendActions::ObserveMessengerDispatch(
        g_messenger,
        nullptr,
        0,
        0,
        0,
        0x04B3,
        pageReply);
    actions.Tick();
    Check(
        actions.State().result ==
                friends::FriendActions::kReplyObserved &&
            actions.State().reply_consumed == 1 &&
            actions.State().list_page_replies == 1 &&
            actions.matchedReplySequence_ == 4 &&
            !actions.commandPending_,
        "a genuine 04B3 completes refresh despite the unrelated reply-count lead");
}

void TestSelectedConversationSend(
    friends::FriendActions& actions)
{
    auto command = MakeSendCommand();
    actions.commandPending_ = true;
    const auto result =
        actions.SendMessage(g_messenger, command);
    const auto state = actions.State();
    Check(
        result == friends::FriendActions::kRequestSent &&
            state.request_sent == 1 &&
            state.request_opcode == 0x0560,
        "send completes from the observed native transport");
    Check(
        !actions.commandPending_ &&
            !actions.commandStarted_ &&
            g_sendCalls == 1,
        "send clears command state without waiting for an ACK");
    Check(
        std::wcscmp(
            g_editText,
            FixtureBody()) == 0,
        "native ANSI message bytes reach the Unicode edit control");

    command.target_id = 9999;
    actions.commandPending_ = true;
    Check(
        actions.SendMessage(g_messenger, command) ==
                friends::FriendActions::kFriendNotFound &&
            g_sendCalls == 1,
        "send rejects a different selected native conversation");
}

void TestPassiveOfflineReply(
    friends::FriendActions& actions)
{
    uint8_t packet[8] = {};
    *reinterpret_cast<uint16_t*>(packet + 6) = 0x1407;
    const unsigned dispatchesBefore = g_dispatchCalls;
    actions.messengerDispatch_ = reinterpret_cast<
        friends::FriendActions::MessengerDispatchFn>(
            &FakeMessengerDispatch);
    friends::FriendActions::active_ = &actions;
    const int result =
        friends::FriendActions::ObserveMessengerDispatch(
            g_messenger,
            nullptr,
            0,
            0,
            0,
            0x0A45,
            packet);
    Check(
        result == 73 &&
            g_dispatchCalls == dispatchesBefore + 1,
        "passive observer preserves the original dispatcher result");
    const auto state = actions.State();
    Check(
        state.reply_consumed == 1 &&
            state.reply_opcode == 0x0A45 &&
            state.reply_status == 0x1407,
        "passive observer records the consumed offline rejection");
    friends::FriendActions::active_ = nullptr;
}

void TestObservedLocalValidation(
    friends::FriendActions& actions,
    NativeActions& native)
{
    const auto run =
        [&](AddScenario scenario, bool validatorResult) {
            ResetFixture(actions, native);
            g_addScenario = scenario;
            g_validatorResult = validatorResult;
            const auto command = MakeAddCommand();
            const auto result =
                actions.AddFriend(g_messenger, command);
            return std::pair<
                friends::FriendActions::Result,
                friends::FriendActions::Observation>(
                    result,
                    actions.State());
        };

    auto outcome = run(kAcceptedDuplicate, false);
    Check(
        outcome.first ==
                friends::FriendActions::
                    kNativeRequestNotSent &&
            outcome.second.local_validation_rejected == 1 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationAcceptedDuplicate &&
            g_acceptedValidatorCalls == 1 &&
            g_addCalls == 1,
        "accepted-friend duplicate requires the observed native false return");

    outcome = run(kPendingDuplicate, false);
    Check(
        outcome.first ==
                friends::FriendActions::
                    kNativeRequestNotSent &&
            outcome.second.local_validation_rejected == 1 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationPendingDuplicate &&
            g_pendingValidatorCalls == 1,
        "pending-name duplicate requires the distinct native validator");

    outcome = run(kSelfTarget, false);
    Check(
        outcome.first ==
                friends::FriendActions::
                    kNativeRequestNotSent &&
            outcome.second.local_validation_rejected == 1 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationSelfTarget,
        "self-add receipt requires the observed native equality branch");

    outcome = run(kNoValidationBranch, false);
    Check(
        outcome.first ==
                friends::FriendActions::
                    kNativeRequestNotSent &&
            outcome.second.local_validation_rejected == 0 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationNone,
        "generic native no-send is not classified as local validation");

    outcome = run(kAcceptedMismatchedTarget, false);
    Check(
        outcome.second.local_validation_rejected == 0 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationNone &&
            g_acceptedValidatorCalls == 1,
        "validator rejection for a different target is not attributed");

    outcome = run(kAcceptedAllowed, true);
    Check(
        outcome.second.local_validation_rejected == 0 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationNone &&
            g_acceptedValidatorCalls == 1,
        "native validator true return is preserved without a rejection receipt");

    outcome = run(kAcceptedThenTransport, false);
    Check(
        outcome.first ==
                friends::FriendActions::kRequestSent &&
            outcome.second.request_sent == 1 &&
            outcome.second.local_validation_rejected == 0 &&
            outcome.second.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationNone,
        "transport advancement suppresses a staged local rejection");
}

void TestRemoveWaitsAfterCacheRemoval(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    friends::FriendActions::Command remove = {};
    remove.operation = friends::FriendActions::kRemoveFriend;
    remove.target_id = 4242;
    actions.command_ = remove;
    actions.commandPending_ = true;
    actions.commandStarted_ = true;
    actions.replyBaseline_ = 50;
    actions.matchedReplySequence_ = 50;
    *reinterpret_cast<uint32_t*>(g_playerDataStorage + 0x7A8) = 0;
    Check(actions.Remove(remove) == friends::FriendActions::kWaitingForReply && g_sendCalls == 0,
          "a sent deletion waits for its reply after the native friend cache removes the row");
    uint8_t reply[8] = {};
    *reinterpret_cast<uint16_t*>(reply) = 0x04DE;
    friends::FriendActions::ObserveMessengerDispatch(g_messenger, nullptr, 0, 0, 0, 0x04DE, reply);
    Check(actions.Remove(remove) == friends::FriendActions::kReplyObserved &&
              actions.State().reply_consumed == 1 && g_sendCalls == 0,
          "the fresh native delete reply completes once without finding or resending the removed row");
}

void TestSelfBridgePreservesState(
    friends::FriendActions& actions,
    NativeActions& native)
{
    ResetFixture(actions, native);
    std::memset(g_selfFrame, 0, sizeof(g_selfFrame));
    std::memset(g_beforeFx, 0, sizeof(g_beforeFx));
    std::memset(g_afterFx, 0, sizeof(g_afterFx));
    std::memset(
        g_afterRegisters,
        0,
        sizeof(g_afterRegisters));
    auto command = MakeAddCommand();
    uint8_t* frame = g_selfFrame + 0x40;
    std::memcpy(
        frame - 0x32,
        command.target_name,
        sizeof(command.target_name));
    std::memcpy(
        frame - 0x21,
        command.target_name,
        sizeof(command.target_name));
    actions.BeginLocalValidationScope(
        g_addService,
        command);
    friends::friend_validation_bridge::callback =
        &friends::FriendActions::
            ObserveSelfComparisonMatch;
    friends::friend_validation_bridge::resume =
        reinterpret_cast<void*>(&SelfBridgeResume);
    const auto before = native.TransportState();
    InvokeSelfBridge(
        reinterpret_cast<void*>(
            &friends::friend_validation_bridge::
                ObserveSelfComparison),
        frame);
    const auto after = native.TransportState();
    actions.EndLocalValidationScope();
    actions.CommitLocalValidationReceipt(before, after);

    Check(
        g_afterRegisters[0] == 0x11111111 &&
            g_afterRegisters[1] == 0x22222222 &&
            g_afterRegisters[2] == 0x33333333 &&
            g_afterRegisters[3] == 0x44444444 &&
            g_afterRegisters[4] == 0x11111111 &&
            g_afterRegisters[5] == 0x77777777 &&
            g_afterRegisters[6] ==
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(frame)) &&
            g_beforeFlags == g_afterFlags &&
            std::memcmp(
                g_beforeFx,
                g_afterFx,
                sizeof(g_beforeFx)) == 0,
        "self-comparison HookSite bridge preserves native machine state");
    const auto state = actions.State();
    Check(
        state.local_validation_rejected == 1 &&
            state.local_validation_code ==
                friends::FriendActions::
                    kLocalValidationSelfTarget,
        "self-comparison HookSite bridge records only the matching active target");
}
} // namespace

int main()
{
    static_assert(
        sizeof(void*) == 4,
        "friend action fixture requires x86");
    std::printf("native ANSI code page: %u; CP936 text assertions: %s\n",GetACP(),GetACP()==936?"enabled":"ASCII fallback");

    NativeActions native;
    friends::FriendActions actions;
    ResetFixture(actions, native);
    friends::FriendActions::Command ensure = {};
    ensure.operation = friends::FriendActions::kEnsureInitialized;
    actions.initialListRequest_ = &FakeInitialListRequest;
    Check(actions.EnsureMessenger(reinterpret_cast<void*>(1), ensure) == friends::FriendActions::kWaitingForReply &&
              g_initialListCalls == 1,
          "cache initialization uses native no-argument request without touching unopened UI");
    Check(actions.EnsureMessenger(nullptr, ensure) == friends::FriendActions::kWaitingForReply &&
              g_initialListCalls == 1,
          "pending native cache load is not restarted while layout is absent");
    *reinterpret_cast<uint32_t*>(g_playerDataStorage + 0x788) = 2;
    Check(actions.EnsureMessenger(nullptr, ensure) == friends::FriendActions::kReady,
          "server-loaded cache readiness does not invoke the one-shot layout initializer");
    ResetFixture(actions, native);
    TestMessengerOpenLifecycle(actions, native);
    TestDialogIdentityAndUniqueness(actions, native);
    TestAddDialogOneShotLifecycle(actions, native);
    TestAddCompletesAtRequestSent(actions, native);
    TestAddTransportHistorySelection(actions, native);
    TestUiButtonGuards(actions, native);
    TestPendingPacketSemantics(actions, native);
    TestPendingTargetValidation(actions, native);
    TestFriendTreePayloadLookup(actions, native);
    TestListPageMatchedReplyBaseline(actions, native);
    TestRemoveWaitsAfterCacheRemoval(actions, native);
    TestSelectedConversationSend(actions);
    TestPassiveOfflineReply(actions);
    TestObservedLocalValidation(actions, native);
    TestSelfBridgePreservesState(actions, native);
    friends::FriendActions::active_ = nullptr;
    NativeActions::active_ = nullptr;
    return g_failures == 0 ? 0 : 1;
}
