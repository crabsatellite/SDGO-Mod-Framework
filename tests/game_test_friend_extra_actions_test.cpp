// OFFLINE-ONLY ABI and boundary tests for FriendExtraActions.
//
// The fixture models the shipped x86 list/object layouts. It never starts
// GOnline, contacts a server, or treats these checks as gameplay evidence.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/friend_extra_actions.h"

namespace
{
int g_failures = 0;
uint8_t g_playerDataStorage[0x900] = {};
uint8_t* g_playerData = g_playerDataStorage;
uint8_t g_messenger[0x100] = {};
void* g_messengerVtable[32] = {};
uint8_t g_serviceManagerObject = 0;
void* g_serviceManager = &g_serviceManagerObject;

uint8_t g_friendHead[12] = {};
uint8_t g_friendNode[12] = {};
uint8_t g_friendRecord[0x180] = {};
uint8_t g_conversationHead[12] = {};
uint8_t g_conversationNode[12] = {};
uint8_t g_conversation[0x80] = {};
uint8_t g_managedProfilePopup[0x210] = {};
uint8_t g_foreignProfilePopup[0x210] = {};
void* g_profilePopupVtable[31] = {};
void* g_foreignProfilePopupVtable[31] = {};

unsigned g_closeCalls = 0;
int g_closeIndex = -1;
unsigned g_profileCtorCalls = 0;
unsigned g_profileDtorCalls = 0;
unsigned g_profileCloneCalls = 0;
unsigned g_profileInitializeCalls = 0;
unsigned g_profileDispatchCalls = 0;
unsigned g_profileScalarDtorCalls = 0;
unsigned g_enqueueCalls = 0;
const void* g_profileSource = nullptr;
int g_profileCompare = -1;
bool g_enqueueSucceeds = true;
bool g_cloneSucceeds = true;
bool g_runInitialize = true;
bool g_corruptCloneIdentity = false;
bool g_corruptCloneName = false;
bool g_replyDuringInitialize = false;
uint32_t g_resultInsideDispatch = UINT32_MAX;

using ProfileCloneDetour =
    void*(__fastcall*)(void*, void*);
using ProfileInitializeDetour =
    void(__fastcall*)(void*, void*, int);
using ProfileDispatchDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);
using ProfileScalarDtorDetour =
    void*(__fastcall*)(void*, void*, uint32_t);

ProfileCloneDetour g_profileCloneDetour = nullptr;
ProfileInitializeDetour g_profileInitializeDetour = nullptr;
ProfileDispatchDetour g_profileDispatchDetour = nullptr;
ProfileScalarDtorDetour g_profileScalarDtorDetour = nullptr;
friends::FriendExtraActions* g_actions = nullptr;

int DeliverProfileReply(
    void* popup,
    uint32_t targetId,
    uint16_t opcode);

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
    if (*reinterpret_cast<uint32_t*>(bytes + 0x14) >= 8) {
        void* allocation = *reinterpret_cast<void**>(bytes);
        if (allocation)
            HeapFree(GetProcessHeap(), 0, allocation);
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
        *reinterpret_cast<uint32_t*>(bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (length + 1) * sizeof(wchar_t);
        auto* allocation = static_cast<wchar_t*>(
            HeapAlloc(
                GetProcessHeap(),
                HEAP_ZERO_MEMORY,
                byteCount));
        if (!allocation)
            return false;
        std::memcpy(allocation, value, byteCount);
        *reinterpret_cast<wchar_t**>(bytes) = allocation;
        *reinterpret_cast<uint32_t*>(bytes + 0x14) =
            static_cast<uint32_t>(length);
    }
    *reinterpret_cast<uint32_t*>(bytes + 0x10) =
        static_cast<uint32_t>(length);
    return true;
}

const wchar_t* ReadNativeString(const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes = static_cast<const uint8_t*>(storage);
    return *reinterpret_cast<const uint32_t*>(
               bytes + 0x14) < 8
        ? reinterpret_cast<const wchar_t*>(bytes)
        : *reinterpret_cast<wchar_t* const*>(bytes);
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

void ResetFixture()
{
    DestroyNativeString(g_friendRecord + 0x38);
    DestroyNativeString(g_managedProfilePopup + 0x50);
    DestroyNativeString(g_foreignProfilePopup + 0x50);
    std::memset(g_playerDataStorage, 0, sizeof(g_playerDataStorage));
    std::memset(g_messenger, 0, sizeof(g_messenger));
    std::memset(g_friendHead, 0, sizeof(g_friendHead));
    std::memset(g_friendNode, 0, sizeof(g_friendNode));
    std::memset(g_friendRecord, 0, sizeof(g_friendRecord));
    std::memset(g_conversationHead, 0, sizeof(g_conversationHead));
    std::memset(g_conversationNode, 0, sizeof(g_conversationNode));
    std::memset(g_conversation, 0, sizeof(g_conversation));
    std::memset(
        g_managedProfilePopup,
        0,
        sizeof(g_managedProfilePopup));
    std::memset(
        g_foreignProfilePopup,
        0,
        sizeof(g_foreignProfilePopup));
    g_playerData = g_playerDataStorage;
    g_serviceManager = &g_serviceManagerObject;
    *reinterpret_cast<void***>(g_messenger) = g_messengerVtable;

    *reinterpret_cast<uint32_t*>(g_friendRecord) = 4242;
    std::memcpy(g_friendRecord + 0x0A, "FriendAlpha", 12);
    g_friendRecord[0x1B] = 7;
    *reinterpret_cast<uint32_t*>(g_friendRecord + 0x1C) = 1;
    Check(
        WriteNativeString(
            g_friendRecord + 0x38,
            L"FriendAlpha"),
        "friend fixture created the native profile target string");
    LinkSingleNode(g_friendHead, g_friendNode, g_friendRecord);
    *reinterpret_cast<uint8_t**>(
        g_playerDataStorage + 0x7A4) = g_friendHead;
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
        g_playerDataStorage + 0x7B0) = g_conversationHead;
    *reinterpret_cast<uint32_t*>(
        g_playerDataStorage + 0x7B4) = 1;

    g_closeCalls = 0;
    g_closeIndex = -1;
    g_profileCtorCalls = 0;
    g_profileDtorCalls = 0;
    g_profileCloneCalls = 0;
    g_profileInitializeCalls = 0;
    g_profileDispatchCalls = 0;
    g_profileScalarDtorCalls = 0;
    g_enqueueCalls = 0;
    g_profileSource = nullptr;
    g_profileCompare = -1;
    g_enqueueSucceeds = true;
    g_cloneSucceeds = true;
    g_runInitialize = true;
    g_corruptCloneIdentity = false;
    g_corruptCloneName = false;
    g_replyDuringInitialize = false;
    g_resultInsideDispatch = UINT32_MAX;
    *reinterpret_cast<void**>(
        g_foreignProfilePopup) =
        g_foreignProfilePopupVtable;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    return WriteNativeString(self, value) ? self : nullptr;
}

void __fastcall FakeNativeWStringDtor(void* self, void*)
{
    DestroyNativeString(self);
}

void* __fastcall FakeServiceFindByName(
    void* manager,
    void*,
    const void* descriptor)
{
    const auto* bytes =
        static_cast<const uint8_t*>(descriptor);
    const wchar_t* name =
        bytes
            ? ReadNativeString(bytes + sizeof(uint32_t))
            : nullptr;
    const bool valid =
        manager == g_serviceManager && bytes &&
        *reinterpret_cast<const uint32_t*>(bytes) == 1 &&
        name && std::wcscmp(name, L"MessengerPopup") == 0;
    Check(
        valid,
        "extra actions resolved the verified Messenger service");
    return valid ? g_messenger : nullptr;
}

void __fastcall FakeCloseConversation(
    void* self,
    void*,
    int index)
{
    ++g_closeCalls;
    g_closeIndex = index;
    if (self == g_messenger && index == 0) {
        *reinterpret_cast<uint32_t*>(
            g_playerDataStorage + 0x7B4) = 0;
        *reinterpret_cast<int32_t*>(
            g_playerDataStorage + 0x790) = -1;
    }
}

void __fastcall FakeProfilePopupCtor(
    void* self,
    void*,
    const void* source,
    int compare)
{
    ++g_profileCtorCalls;
    g_profileSource = source;
    g_profileCompare = compare;
    std::memset(self, 0, 0x210);
    *reinterpret_cast<void**>(self) =
        g_profilePopupVtable;
    const wchar_t* name = ReadNativeString(source);
    Check(
        name &&
            WriteNativeString(
                static_cast<uint8_t*>(self) + 0x50,
                name),
        "profile constructor copied the native target string to +0x50");
    std::memset(
        static_cast<uint8_t*>(self) + 0x70,
        0,
        0x10D);
}

void __fastcall FakeProfilePopupDtor(void* self, void*)
{
    ++g_profileDtorCalls;
    DestroyNativeString(
        static_cast<uint8_t*>(self) + 0x50);
    std::memset(self, 0, 0x210);
}

void* __fastcall FakeProfilePopupClone(
    void* self,
    void*)
{
    ++g_profileCloneCalls;
    if (!self || !g_cloneSucceeds)
        return nullptr;
    DestroyNativeString(g_managedProfilePopup + 0x50);
    std::memset(
        g_managedProfilePopup,
        0,
        sizeof(g_managedProfilePopup));
    *reinterpret_cast<void**>(
        g_managedProfilePopup) =
        g_profilePopupVtable;
    const wchar_t* name =
        ReadNativeString(
            static_cast<uint8_t*>(self) + 0x50);
    Check(
        name &&
            WriteNativeString(
                g_managedProfilePopup + 0x50,
                g_corruptCloneName
                    ? L"OtherPilot"
                    : name),
        "profile clone deep-copied its native target string");
    if (g_corruptCloneIdentity) {
        *reinterpret_cast<void**>(
            g_managedProfilePopup) =
            g_foreignProfilePopupVtable;
    }
    return g_managedProfilePopup;
}

void __fastcall FakeProfilePopupInitialize(
    void* self,
    void*,
    int compare)
{
    Check(
        self == g_managedProfilePopup &&
            compare == 0,
        "managed profile clone entered the original initialize path");
    ++g_profileInitializeCalls;
    if (g_replyDuringInitialize)
        DeliverProfileReply(self, 4242, 0x084B);
}

int __fastcall FakeProfilePopupDispatch(
    void* self,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    ++g_profileDispatchCalls;
    g_resultInsideDispatch =
        g_actions
            ? g_actions->State().result
            : UINT32_MAX;
    if (self && packet &&
        static_cast<uint16_t>(opcode) == 0x084B) {
        const auto* payload =
            static_cast<const uint8_t*>(packet) + 6;
        std::memcpy(
            static_cast<uint8_t*>(self) + 0x70,
            payload,
            0x10D);
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(self) + 0x180) =
            *reinterpret_cast<const uint32_t*>(
                payload);
    }
    return 73;
}

void* __fastcall FakeProfilePopupScalarDtor(
    void* self,
    void*,
    uint32_t)
{
    ++g_profileScalarDtorCalls;
    if (self) {
        DestroyNativeString(
            static_cast<uint8_t*>(self) + 0x50);
        std::memset(self, 0, 0x210);
    }
    return self;
}

void* __cdecl FakeEnqueueUiObject(void* object, int clone)
{
    ++g_enqueueCalls;
    if (!object || clone != 1 || !g_enqueueSucceeds)
        return nullptr;
    void* managed =
        g_profileCloneDetour
            ? g_profileCloneDetour(
                  object,
                  nullptr)
            : nullptr;
    if (managed && g_runInitialize &&
        g_profileInitializeDetour) {
        g_profileInitializeDetour(
            managed,
            nullptr,
            0);
    }
    // The shipped enqueue returns its manager-owned queue entry, not proof
    // that cloning or initialization succeeded.
    return object;
}

void* LookupSymbol(const char* name)
{
    if (std::strcmp(name, "friends.player_data_pointer") == 0)
        return &g_playerData;
    if (std::strcmp(name, "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(name, "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(&FakeServiceFindByName);
    if (std::strcmp(name, "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringCtor);
    if (std::strcmp(name, "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringDtor);
    if (std::strcmp(name, "friends.messenger_vtable") == 0)
        return g_messengerVtable;
    if (std::strcmp(
            name,
            "friends.extra.close_selected_conversation") == 0)
        return reinterpret_cast<void*>(&FakeCloseConversation);
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_ctor") == 0)
        return reinterpret_cast<void*>(&FakeProfilePopupCtor);
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_dtor") == 0)
        return reinterpret_cast<void*>(&FakeProfilePopupDtor);
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_vtable") == 0)
        return g_profilePopupVtable;
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_clone") == 0)
        return reinterpret_cast<void*>(&FakeProfilePopupClone);
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_initialize") == 0)
        return reinterpret_cast<void*>(
            &FakeProfilePopupInitialize);
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeProfilePopupDispatch);
    if (std::strcmp(
            name,
            "friends.extra.profile_popup_scalar_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeProfilePopupScalarDtor);
    if (std::strcmp(name, "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(&FakeEnqueueUiObject);
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
            "friends.extra.profile_popup_clone") == 0) {
        g_profileCloneDetour =
            reinterpret_cast<ProfileCloneDetour>(
                detour);
    } else if (std::strcmp(
                   name,
                   "friends.extra.profile_popup_initialize") == 0) {
        g_profileInitializeDetour =
            reinterpret_cast<ProfileInitializeDetour>(
                detour);
    } else if (std::strcmp(
                   name,
                   "friends.extra.profile_popup_dispatch") == 0) {
        g_profileDispatchDetour =
            reinterpret_cast<ProfileDispatchDetour>(
                detour);
    } else if (std::strcmp(
                   name,
                   "friends.extra.profile_popup_scalar_dtor") == 0) {
        g_profileScalarDtorDetour =
            reinterpret_cast<ProfileScalarDtorDetour>(
                detour);
    }
    return 1;
}

friends::FriendActions::Command Command(
    uint32_t operation,
    uint32_t sequence = 1)
{
    friends::FriendActions::Command command = {};
    command.sequence = sequence;
    command.operation = operation;
    command.target_id = 4242;
    std::memcpy(command.target_name, "FriendAlpha", 12);
    return command;
}

int DeliverProfileReply(
    void* popup,
    uint32_t targetId,
    uint16_t opcode = 0x084B)
{
    uint8_t packet[6 + 0x10D] = {};
    for (size_t index = 0; index < 0x10D; ++index) {
        packet[6 + index] =
            static_cast<uint8_t>(
                0x70 + index % 0x30);
    }
    *reinterpret_cast<uint32_t*>(
        packet + 6) = targetId;
    return g_profileDispatchDetour
        ? g_profileDispatchDetour(
              popup,
              nullptr,
              1,
              2,
              3,
              opcode,
              packet)
        : 0;
}

void TestCloseSelected(
    friends::FriendExtraActions& actions)
{
    ResetFixture();
    const auto result = actions.Submit(
        Command(
            friends::FriendExtraActions::
                kCloseSelectedConversation,
            10));
    const auto state = actions.State();
    Check(
        result == friends::FriendActions::kReady,
        "selected native conversation close completed");
    Check(
        g_closeCalls == 1 && g_closeIndex == 0,
        "close used the native selected tab index");
    Check(
        state.command_sequence == 10 &&
            state.selected_conversation == 0 &&
            state.conversation_count_before == 1 &&
            state.conversation_count_after == 0 &&
            state.selected_target_matched == 1,
        "close published exact before/after local tab state");
}

void TestCloseGuards(
    friends::FriendExtraActions& actions)
{
    ResetFixture();
    *reinterpret_cast<int32_t*>(
        g_playerDataStorage + 0x790) = -1;
    auto result = actions.Submit(
        Command(
            friends::FriendExtraActions::
                kCloseSelectedConversation));
    Check(
        result == friends::FriendActions::kFriendNotFound &&
            g_closeCalls == 0,
        "close rejected a missing selected conversation");

    ResetFixture();
    auto wrong = Command(
        friends::FriendExtraActions::
            kCloseSelectedConversation);
    wrong.target_id = 9999;
    std::memset(wrong.target_name, 0, sizeof(wrong.target_name));
    result = actions.Submit(wrong);
    Check(
        result == friends::FriendActions::kFriendNotFound &&
            g_closeCalls == 0,
        "close did not close a different selected friend");
}

void TestOpenProfile(
    friends::FriendExtraActions& actions)
{
    ResetFixture();
    const auto result = actions.Submit(
        Command(
            friends::FriendExtraActions::kOpenProfile,
            20));
    const auto queued = actions.State();
    Check(
        result == friends::FriendActions::kWaitingForReply,
        "profile remains pending after the native request starts");
    Check(
        g_profileCtorCalls == 1 &&
            g_profileSource == g_friendRecord + 0x38 &&
            g_profileCompare == 0,
        "profile constructor received the native friend profile subrecord");
    Check(
        g_enqueueCalls == 1 &&
            g_profileCloneCalls == 1 &&
            g_profileInitializeCalls == 1 &&
            g_profileDtorCalls == 1,
        "profile clone initialized before its stack source was destroyed");
    Check(
        queued.profile_constructed == 1 &&
            queued.profile_enqueued == 1 &&
            queued.profile_enqueue_count == 1 &&
            queued.profile_clone_created == 1 &&
            queued.profile_clone_identity_matched == 1 &&
            queued.profile_initialized == 1 &&
            queued.profile_request_started == 1 &&
            queued.profile_reply_consumed == 0,
        "profile receipt requires the manager-owned exact-target clone and initialize path");
    Check(
        actions.Submit(
            Command(
                friends::FriendExtraActions::kOpenProfile,
                21)) ==
            friends::FriendActions::kBusy,
        "pending profile request cannot be replaced by another command");

    const int foreignResult =
        DeliverProfileReply(
            g_foreignProfilePopup,
            4242);
    Check(
        foreignResult == 73 &&
            actions.State().result ==
                friends::FriendActions::kWaitingForReply,
        "a foreign profile popup reply is passed through but ignored");
    const int otherOpcodeResult =
        DeliverProfileReply(
            g_managedProfilePopup,
            4242,
            0x093D);
    Check(
        otherOpcodeResult == 73 &&
            actions.State().result ==
                friends::FriendActions::kWaitingForReply,
        "an unrelated opcode on the matched popup cannot complete profile");

    const int handled =
        DeliverProfileReply(
            g_managedProfilePopup,
            4242);
    const auto completed = actions.State();
    Check(
        handled == 73,
        "profile dispatch detour preserved the original return value");
    Check(
        g_resultInsideDispatch ==
            friends::FriendActions::kWaitingForReply,
        "profile success was not published before the original consumer ran");
    Check(
        completed.result ==
                friends::FriendActions::kReplyObserved &&
            completed.profile_reply_consumed == 1 &&
            completed.profile_reply_opcode == 0x084B &&
            completed.profile_reply_target_id == 4242 &&
            completed.profile_reply_target_matched == 1,
        "profile completes only after the exact clone consumes its exact-target 0x084B reply");
}

void TestProfileGuards(
    friends::FriendExtraActions& actions)
{
    ResetFixture();
    g_friendRecord[0x1B] = 6;
    auto result = actions.Submit(
        Command(friends::FriendExtraActions::kOpenProfile));
    Check(
        result == friends::FriendActions::kFriendNotFound &&
            g_profileCtorCalls == 0,
        "profile opening rejected a non-accepted relation");

    ResetFixture();
    g_enqueueSucceeds = false;
    result = actions.Submit(
        Command(friends::FriendExtraActions::kOpenProfile));
    Check(
        result ==
                friends::FriendActions::kNativeRequestNotSent &&
            g_profileCtorCalls == 1 &&
            g_profileDtorCalls == 1,
        "profile enqueue failure still destroyed the native stack object");

    ResetFixture();
    g_cloneSucceeds = false;
    result = actions.Submit(
        Command(friends::FriendExtraActions::kOpenProfile));
    const auto cloneFailed = actions.State();
    Check(
        result ==
                friends::FriendActions::kNativeRequestNotSent &&
            cloneFailed.profile_enqueued == 1 &&
            cloneFailed.profile_clone_created == 0,
        "a non-null enqueue return cannot replace a missing managed clone");

    ResetFixture();
    g_corruptCloneIdentity = true;
    result = actions.Submit(
        Command(friends::FriendExtraActions::kOpenProfile));
    Check(
        result ==
                friends::FriendActions::kNativeRequestNotSent &&
            actions.State().profile_clone_created == 1 &&
            actions.State().
                    profile_clone_identity_matched == 0,
        "a foreign clone identity cannot satisfy profile creation");

    ResetFixture();
    g_corruptCloneName = true;
    result = actions.Submit(
        Command(friends::FriendExtraActions::kOpenProfile));
    Check(
        result ==
                friends::FriendActions::kNativeRequestNotSent &&
            actions.State().profile_clone_created == 1 &&
            actions.State().
                    profile_clone_identity_matched == 0,
        "a clone for another native profile target cannot satisfy profile creation");

    ResetFixture();
    g_runInitialize = false;
    result = actions.Submit(
        Command(friends::FriendExtraActions::kOpenProfile));
    Check(
        result ==
                friends::FriendActions::kNativeRequestNotSent &&
            actions.State().
                    profile_clone_identity_matched == 1 &&
            actions.State().profile_initialized == 0,
        "an uninitialized clone cannot satisfy profile opening");

    ResetFixture();
    auto wrongName =
        Command(friends::FriendExtraActions::kOpenProfile);
    std::memset(
        wrongName.target_name,
        0,
        sizeof(wrongName.target_name));
    std::memcpy(
        wrongName.target_name,
        "OtherFriend",
        12);
    result = actions.Submit(wrongName);
    Check(
        result == friends::FriendActions::kFriendNotFound &&
            g_profileCtorCalls == 0,
        "id and name must identify the same accepted friend");
}

void TestProfileReplyGuards(
    friends::FriendExtraActions& actions)
{
    ResetFixture();
    g_replyDuringInitialize = true;
    Check(
        actions.Submit(
            Command(
                friends::FriendExtraActions::kOpenProfile)) ==
                friends::FriendActions::kReplyObserved &&
            actions.State().profile_reply_consumed == 1 &&
            actions.State().profile_reply_target_matched == 1,
        "a synchronous exact-target reply cannot be overwritten by the enqueue completion path");

    ResetFixture();
    Check(
        actions.Submit(
            Command(
                friends::FriendExtraActions::kOpenProfile)) ==
            friends::FriendActions::kWaitingForReply,
        "mismatch fixture started a native profile request");
    DeliverProfileReply(
        g_managedProfilePopup,
        9999);
    const auto mismatch = actions.State();
    Check(
        mismatch.result ==
                friends::FriendActions::kReplyRejected &&
            mismatch.profile_reply_consumed == 1 &&
            mismatch.profile_reply_target_id == 9999 &&
            mismatch.profile_reply_target_matched == 0,
        "a consumed reply for another pilot is rejected");

    ResetFixture();
    Check(
        actions.Submit(
            Command(
                friends::FriendExtraActions::kOpenProfile)) ==
            friends::FriendActions::kWaitingForReply,
        "destruction fixture started a native profile request");
    Check(
        g_profileScalarDtorDetour != nullptr,
        "managed profile scalar-dtor detour was captured");
    if (g_profileScalarDtorDetour) {
        g_profileScalarDtorDetour(
            g_foreignProfilePopup,
            nullptr,
            1);
        Check(
            actions.State().result ==
                friends::FriendActions::kWaitingForReply,
            "foreign popup destruction cannot cancel the matched request");
        g_profileScalarDtorDetour(
            g_managedProfilePopup,
            nullptr,
            1);
    }
    const auto destroyed = actions.State();
    Check(
        destroyed.result ==
                friends::FriendActions::kReplyRejected &&
            destroyed.profile_destroyed == 1 &&
            g_profileScalarDtorCalls == 2,
        "matched popup destruction before reply terminates the request");
}
}

int main()
{
    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;
    auto* mod = reinterpret_cast<SdgoMod*>(1);
    friends::FriendExtraActions actions;
    g_actions = &actions;
    Check(
        actions.Bind(&api, mod),
        "extra friend actions bound all exact native symbols");
    TestCloseSelected(actions);
    TestCloseGuards(actions);
    TestOpenProfile(actions);
    TestProfileGuards(actions);
    TestProfileReplyGuards(actions);
    return g_failures == 0 ? 0 : 1;
}
