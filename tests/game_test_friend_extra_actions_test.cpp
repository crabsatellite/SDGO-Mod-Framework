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

unsigned g_closeCalls = 0;
int g_closeIndex = -1;
unsigned g_profileCtorCalls = 0;
unsigned g_profileDtorCalls = 0;
unsigned g_enqueueCalls = 0;
const void* g_profileSource = nullptr;
int g_profileCompare = -1;
bool g_enqueueSucceeds = true;

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
    std::memset(g_playerDataStorage, 0, sizeof(g_playerDataStorage));
    std::memset(g_messenger, 0, sizeof(g_messenger));
    std::memset(g_friendHead, 0, sizeof(g_friendHead));
    std::memset(g_friendNode, 0, sizeof(g_friendNode));
    std::memset(g_friendRecord, 0, sizeof(g_friendRecord));
    std::memset(g_conversationHead, 0, sizeof(g_conversationHead));
    std::memset(g_conversationNode, 0, sizeof(g_conversationNode));
    std::memset(g_conversation, 0, sizeof(g_conversation));
    g_playerData = g_playerDataStorage;
    g_serviceManager = &g_serviceManagerObject;
    *reinterpret_cast<void***>(g_messenger) = g_messengerVtable;

    *reinterpret_cast<uint32_t*>(g_friendRecord) = 4242;
    std::memcpy(g_friendRecord + 0x0A, "FriendAlpha", 12);
    g_friendRecord[0x1B] = 7;
    *reinterpret_cast<uint32_t*>(g_friendRecord + 0x1C) = 1;
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
    g_enqueueCalls = 0;
    g_profileSource = nullptr;
    g_profileCompare = -1;
    g_enqueueSucceeds = true;
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
        name && std::wcscmp(name, L"Messenger") == 0;
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
    std::memset(self, 0x5A, 0x214);
}

void __fastcall FakeProfilePopupDtor(void* self, void*)
{
    ++g_profileDtorCalls;
    std::memset(self, 0, 0x214);
}

void* __cdecl FakeEnqueueUiObject(void* object, int clone)
{
    ++g_enqueueCalls;
    return object && clone == 1 && g_enqueueSucceeds
        ? object
        : nullptr;
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
    if (std::strcmp(name, "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(&FakeEnqueueUiObject);
    return nullptr;
}

void* ApiSymbol(SdgoMod*, const char* name)
{
    return LookupSymbol(name);
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
    const auto state = actions.State();
    Check(
        result == friends::FriendActions::kReady,
        "accepted friend profile opened through native UI cloning");
    Check(
        g_profileCtorCalls == 1 &&
            g_profileSource == g_friendRecord + 0x38 &&
            g_profileCompare == 0,
        "profile constructor received the native friend profile subrecord");
    Check(
        g_enqueueCalls == 1 && g_profileDtorCalls == 1,
        "profile object was cloned before its stack object was destroyed");
    Check(
        state.profile_constructed == 1 &&
            state.profile_enqueued == 1 &&
            state.profile_enqueue_count == 1,
        "profile observation recorded the real UI enqueue");
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
}
}

int main()
{
    SdgoApi api = {};
    api.Symbol = &ApiSymbol;
    auto* mod = reinterpret_cast<SdgoMod*>(1);
    friends::FriendExtraActions actions;
    Check(
        actions.Bind(&api, mod),
        "extra friend actions bound all exact native symbols");
    TestCloseSelected(actions);
    TestCloseGuards(actions);
    TestOpenProfile(actions);
    TestProfileGuards(actions);
    return g_failures == 0 ? 0 : 1;
}
