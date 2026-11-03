// OFFLINE-ONLY structural tests for native friend-message history proof.
//
// These fixtures model shipped x86 list, conversation and native-wstring
// layouts. They do not start GOnline, contact a server, or treat a raw packet
// observation as proof that the original client applied a message.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/friend_message_history.h"

namespace
{
using namespace friends::friend_message_history;

int g_failures = 0;

struct ListNode
{
    ListNode* next;
    ListNode* previous;
    void* payload;
};

struct NativeLine
{
    uint8_t bytes[0x20];
};

struct Fixture
{
    uint8_t player[0x900];
    ListNode friendHead;
    ListNode friendNode[2];
    uint8_t friendRecord[2][0x80];
    ListNode conversationHead;
    ListNode conversationNode[2];
    uint8_t conversation[2][0x50];
    ListNode lineHead[2];
    ListNode lineNode[4];
    NativeLine line[4];
};

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void InitializeEmptyList(ListNode* head)
{
    head->next = head;
    head->previous = head;
    head->payload = nullptr;
}

void LinkNodes(
    ListNode* head,
    ListNode* nodes,
    void* const* payloads,
    uint32_t count)
{
    InitializeEmptyList(head);
    for (uint32_t index = 0; index < count; ++index) {
        nodes[index].previous =
            index == 0 ? head : &nodes[index - 1];
        nodes[index].next =
            index + 1 == count
                ? head
                : &nodes[index + 1];
        nodes[index].payload = payloads[index];
    }
    if (count != 0) {
        head->next = &nodes[0];
        head->previous = &nodes[count - 1];
    }
}

void WriteFriend(
    uint8_t* record,
    uint32_t id,
    const char* name,
    uint32_t presence)
{
    std::memset(record, 0, 0x80);
    *reinterpret_cast<uint32_t*>(record) = id;
    strncpy_s(
        reinterpret_cast<char*>(record + 0x0A),
        17,
        name,
        _TRUNCATE);
    *reinterpret_cast<uint32_t*>(record + 0x1C) =
        presence;
}

void DestroyNativeString(NativeLine* line)
{
    if (!line)
        return;
    auto* bytes = line->bytes;
    const uint32_t capacity =
        *reinterpret_cast<uint32_t*>(bytes + 0x14);
    if (capacity >= 8) {
        void* allocation =
            *reinterpret_cast<void**>(bytes);
        if (allocation)
            HeapFree(GetProcessHeap(), 0, allocation);
    }
    std::memset(bytes, 0, 0x20);
}

bool WriteNativeString(
    NativeLine* line,
    const wchar_t* text,
    uint32_t style)
{
    if (!line || !text)
        return false;
    DestroyNativeString(line);
    const uint32_t length =
        static_cast<uint32_t>(std::wcslen(text));
    auto* bytes = line->bytes;
    if (length < 8) {
        std::memcpy(
            bytes,
            text,
            (static_cast<size_t>(length) + 1) *
                sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (static_cast<size_t>(length) + 1) *
            sizeof(wchar_t);
        auto* allocation =
            static_cast<wchar_t*>(
                HeapAlloc(
                    GetProcessHeap(),
                    HEAP_ZERO_MEMORY,
                    byteCount));
        if (!allocation)
            return false;
        std::memcpy(allocation, text, byteCount);
        *reinterpret_cast<wchar_t**>(bytes) =
            allocation;
        *reinterpret_cast<uint32_t*>(bytes + 0x14) =
            length;
    }
    *reinterpret_cast<uint32_t*>(bytes + 0x10) =
        length;
    *reinterpret_cast<uint32_t*>(bytes + 0x1C) =
        style;
    return true;
}

void InitializeConversation(
    Fixture* fixture,
    uint32_t conversationIndex,
    uint32_t friendIndex,
    const wchar_t* const* lines,
    const uint32_t* styles,
    uint32_t lineCount)
{
    auto* conversation =
        fixture->conversation[conversationIndex];
    std::memset(conversation, 0, 0x50);
    *reinterpret_cast<uint8_t**>(conversation) =
        fixture->friendRecord[friendIndex];
    InitializeEmptyList(
        &fixture->lineHead[conversationIndex]);
    *reinterpret_cast<ListNode**>(
        conversation + 0x04) =
        &fixture->lineHead[conversationIndex];
    *reinterpret_cast<uint32_t*>(
        conversation + 0x08) = lineCount;
    *reinterpret_cast<uint32_t*>(
        conversation + 0x10) = lineCount;
    *reinterpret_cast<uint32_t*>(
        conversation + 0x14) = kMaximumLines;

    void* payloads[2] = {};
    for (uint32_t index = 0;
         index < lineCount;
         ++index) {
        const uint32_t storageIndex =
            conversationIndex * 2 + index;
        Check(
            WriteNativeString(
                &fixture->line[storageIndex],
                lines[index],
                styles[index]),
            "fixture wrote native chat-line string");
        payloads[index] =
            fixture->line[storageIndex].bytes;
    }
    LinkNodes(
        &fixture->lineHead[conversationIndex],
        &fixture->lineNode[conversationIndex * 2],
        payloads,
        lineCount);
}

void ResetFixture(Fixture* fixture)
{
    for (auto& line : fixture->line)
        DestroyNativeString(&line);
    std::memset(fixture, 0, sizeof(*fixture));

    WriteFriend(
        fixture->friendRecord[0],
        4242,
        "FriendAlpha",
        1);
    void* friendPayloads[1] = {
        fixture->friendRecord[0],
    };
    LinkNodes(
        &fixture->friendHead,
        fixture->friendNode,
        friendPayloads,
        1);
    *reinterpret_cast<ListNode**>(
        fixture->player + 0x7A4) =
        &fixture->friendHead;
    *reinterpret_cast<uint32_t*>(
        fixture->player + 0x7A8) = 1;

    InitializeEmptyList(&fixture->conversationHead);
    *reinterpret_cast<ListNode**>(
        fixture->player + 0x7B0) =
        &fixture->conversationHead;
    *reinterpret_cast<uint32_t*>(
        fixture->player + 0x7B4) = 0;
}

void SetConversations(
    Fixture* fixture,
    uint32_t count)
{
    void* payloads[2] = {
        fixture->conversation[0],
        fixture->conversation[1],
    };
    LinkNodes(
        &fixture->conversationHead,
        fixture->conversationNode,
        payloads,
        count);
    *reinterpret_cast<uint32_t*>(
        fixture->player + 0x7B4) = count;
}

void TestNewConversationProof()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"MSG0001";

    const Snapshot before = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        before.structure_valid &&
            !before.conversation_present,
        "valid before snapshot permits no existing conversation");

    const wchar_t* lines[] = {
        L"FriendAlpha:",
        expected,
    };
    const uint32_t styles[] = {
        0x11223344,
        kMessageBodyStyle,
    };
    InitializeConversation(
        &fixture,
        0,
        0,
        lines,
        styles,
        2);
    SetConversations(&fixture, 1);
    *reinterpret_cast<uint32_t*>(
        fixture.friendRecord[0] + 0x1C) = 5;

    const Snapshot after = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    const ApplicationProof proof =
        Compare(before, after, 5, 4242);
    Check(
        proof.applied &&
            proof.status == kProofApplied &&
            proof.conversation_created &&
            proof.after_line_count == 2,
        "new conversation requires exact owner, title/body count and nonce tail");

    DestroyNativeString(&fixture.line[0]);
    DestroyNativeString(&fixture.line[1]);
}

void TestExistingConversationProof()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"\x4E2D\x6587\x6D88\x606F";
    const wchar_t* oldLines[] = {
        L"old",
    };
    const uint32_t oldStyles[] = {
        kMessageBodyStyle,
    };
    InitializeConversation(
        &fixture,
        0,
        0,
        oldLines,
        oldStyles,
        1);
    SetConversations(&fixture, 1);

    const Snapshot before = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        4);

    const wchar_t* newLines[] = {
        L"header",
        expected,
    };
    const uint32_t newStyles[] = {
        0xA5A5A5A5,
        kMessageBodyStyle,
    };
    DestroyNativeString(&fixture.line[0]);
    InitializeConversation(
        &fixture,
        0,
        0,
        newLines,
        newStyles,
        2);
    *reinterpret_cast<uint32_t*>(
        fixture.conversation[0] + 0x08) = 3;
    *reinterpret_cast<uint32_t*>(
        fixture.conversation[0] + 0x10) = 3;

    // Add the retained old line followed by the native title/body pair.
    Check(
        WriteNativeString(
            &fixture.line[2],
            L"old",
            kMessageBodyStyle),
        "fixture retained prior chat line");
    Check(
        WriteNativeString(
            &fixture.line[0],
            L"header",
            0xA5A5A5A5),
        "fixture appended sender title");
    Check(
        WriteNativeString(
            &fixture.line[1],
            expected,
            kMessageBodyStyle),
        "fixture appended expected body");
    ListNode nodes[3] = {};
    void* payloads[3] = {
        fixture.line[2].bytes,
        fixture.line[0].bytes,
        fixture.line[1].bytes,
    };
    LinkNodes(
        &fixture.lineHead[0],
        nodes,
        payloads,
        3);
    *reinterpret_cast<uint32_t*>(
        fixture.friendRecord[0] + 0x1C) = 9;

    const Snapshot after = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        4);
    const ApplicationProof proof =
        Compare(before, after, 9, 4242);
    Check(
        proof.applied &&
            !proof.conversation_created &&
            proof.before_line_count == 1 &&
            proof.after_line_count == 3,
        "existing conversation proves one title plus one body line");

    DestroyNativeString(&fixture.line[0]);
    DestroyNativeString(&fixture.line[1]);
    DestroyNativeString(&fixture.line[2]);
}

void TestHeapStringInspection()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] =
        L"Heap-backed message body";
    const wchar_t* lines[] = {expected};
    const uint32_t styles[] = {kMessageBodyStyle};
    InitializeConversation(
        &fixture,
        0,
        0,
        lines,
        styles,
        1);
    SetConversations(&fixture, 1);

    const Snapshot snapshot = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        static_cast<uint32_t>(
            std::wcslen(expected)));
    Check(
        snapshot.structure_valid &&
            snapshot.tail_matches_expected &&
            snapshot.tail_capacity >= 8,
        "inspection validates bounded heap-backed native strings");

    DestroyNativeString(&fixture.line[0]);
}

void TestOfflineAndStaleRejections()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"MSG0002";
    const wchar_t* lines[] = {expected};
    const uint32_t styles[] = {kMessageBodyStyle};
    InitializeConversation(
        &fixture,
        0,
        0,
        lines,
        styles,
        1);
    SetConversations(&fixture, 1);

    const Snapshot snapshot = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        Compare(snapshot, snapshot, 0, 4242).status ==
            kProofPacketOffline,
        "online flag zero cannot prove ordinary body application");
    Check(
        Compare(snapshot, snapshot, 1, 4242).status ==
            kProofStaleExpectedTail,
        "a nonce already present before dispatch is rejected as stale");

    DestroyNativeString(&fixture.line[0]);
}

void TestWrongFriendAndMalformedLists()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"MSG0003";

    WriteFriend(
        fixture.friendRecord[1],
        4343,
        "FriendBeta",
        1);
    void* friendPayloads[2] = {
        fixture.friendRecord[0],
        fixture.friendRecord[1],
    };
    LinkNodes(
        &fixture.friendHead,
        fixture.friendNode,
        friendPayloads,
        2);
    *reinterpret_cast<uint32_t*>(
        fixture.player + 0x7A8) = 2;

    const wchar_t* lines[] = {expected};
    const uint32_t styles[] = {kMessageBodyStyle};
    InitializeConversation(
        &fixture,
        0,
        1,
        lines,
        styles,
        1);
    SetConversations(&fixture, 1);

    const Snapshot noConversation = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        noConversation.structure_valid &&
            !noConversation.conversation_present,
        "a different friend's conversation cannot satisfy sender ownership");

    fixture.friendNode[1].previous =
        &fixture.friendHead;
    const Snapshot malformed = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        !malformed.structure_valid &&
            malformed.status ==
                kInspectionFriendListMalformed,
        "broken friend-list backlinks are rejected");

    DestroyNativeString(&fixture.line[0]);
}

void TestAmbiguousAndMalformedHistory()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"MSG0004";

    WriteFriend(
        fixture.friendRecord[1],
        4343,
        "FriendAlpha",
        1);
    void* friendPayloads[2] = {
        fixture.friendRecord[0],
        fixture.friendRecord[1],
    };
    LinkNodes(
        &fixture.friendHead,
        fixture.friendNode,
        friendPayloads,
        2);
    *reinterpret_cast<uint32_t*>(
        fixture.player + 0x7A8) = 2;
    const Snapshot ambiguous = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        ambiguous.status == kInspectionFriendAmbiguous,
        "duplicate matching friend names are rejected");

    ResetFixture(&fixture);
    const wchar_t* lines[] = {L"old"};
    const uint32_t styles[] = {kMessageBodyStyle};
    InitializeConversation(
        &fixture,
        0,
        0,
        lines,
        styles,
        1);
    SetConversations(&fixture, 1);
    fixture.lineNode[0].previous = nullptr;
    const Snapshot malformedHistory = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        malformedHistory.status ==
            kInspectionLineListMalformed,
        "broken chat-history backlinks are rejected");

    DestroyNativeString(&fixture.line[0]);
}

void TestContentChangeAllowsReusedIdentity()
{
    Snapshot before = {};
    before.status = kInspectionOk;
    before.structure_valid = 1;
    before.conversation_present = 1;
    before.tail_present = 1;
    before.accepted_friend_id = 4242;
    before.friend_presence = 1;
    before.friend_count = 1;
    before.conversation_count = 1;
    before.line_count = kMaximumLines;
    before.line_limit = kMaximumLines;
    before.friend_identity = 0x1000;
    before.conversation_identity = 0x2000;
    before.tail_node_identity = 0x3000;
    before.tail_payload_identity = 0x4000;
    before.tail_length = 3;
    before.tail_style = kMessageBodyStyle;
    before.expected_length = 7;
    wcscpy_s(before.tail_text, L"old");
    wcscpy_s(before.expected_text, L"MSG0005");

    Snapshot after = before;
    after.friend_presence = 7;
    after.tail_length = 7;
    wcscpy_s(after.tail_text, L"MSG0005");
    after.tail_matches_expected = 1;

    const ApplicationProof proof =
        Compare(before, after, 7, 4242);
    Check(
        proof.applied &&
            !proof.tail_identity_changed &&
            proof.tail_content_changed,
        "exact changed tail content permits allocator identity reuse at line cap");
}

void TestProofRejectsUnappliedAndWrongId()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"MSG0006";
    const wchar_t* oldLines[] = {L"old"};
    const uint32_t oldStyles[] = {kMessageBodyStyle};
    InitializeConversation(
        &fixture,
        0,
        0,
        oldLines,
        oldStyles,
        1);
    SetConversations(&fixture, 1);
    const Snapshot unchanged = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);

    Check(
        Compare(unchanged, unchanged, 1, 4242).status ==
            kProofTailTextMismatch,
        "recognized packet without expected tail is not application proof");
    Check(
        Compare(unchanged, unchanged, 1, 9999).status ==
            kProofFriendIdMismatch,
        "optional raw packet identity must match model-resolved friend id");

    DestroyNativeString(&fixture.line[0]);
}

void TestUnreadablePointersAndNamesRejected()
{
    Fixture fixture = {};
    ResetFixture(&fixture);
    constexpr wchar_t expected[] = L"MSG0007";

    std::memset(
        fixture.friendRecord[0] + 0x0A,
        'A',
        17);
    const Snapshot malformedName = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        malformedName.status ==
            kInspectionFriendRecordMalformed,
        "unterminated native friend names are rejected");

    ResetFixture(&fixture);
    void* inaccessible = VirtualAlloc(
        nullptr,
        0x1000,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_NOACCESS);
    Check(
        inaccessible != nullptr,
        "fixture allocated an inaccessible guard page");
    if (!inaccessible)
        return;

    *reinterpret_cast<void**>(
        fixture.player + 0x7A4) = inaccessible;
    const Snapshot unreadableList = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        unreadableList.status ==
            kInspectionFriendListMalformed,
        "unreadable friend-list pointers fail closed");

    ResetFixture(&fixture);
    const wchar_t* lines[] = {L"old"};
    const uint32_t styles[] = {kMessageBodyStyle};
    InitializeConversation(
        &fixture,
        0,
        0,
        lines,
        styles,
        1);
    SetConversations(&fixture, 1);
    DestroyNativeString(&fixture.line[0]);
    *reinterpret_cast<void**>(
        fixture.line[0].bytes) = inaccessible;
    *reinterpret_cast<uint32_t*>(
        fixture.line[0].bytes + 0x10) = 8;
    *reinterpret_cast<uint32_t*>(
        fixture.line[0].bytes + 0x14) = 8;
    *reinterpret_cast<uint32_t*>(
        fixture.line[0].bytes + 0x1C) =
        kMessageBodyStyle;
    const Snapshot unreadableText = Inspect(
        fixture.player,
        "FriendAlpha",
        expected,
        7);
    Check(
        unreadableText.status ==
            kInspectionNativeStringMalformed,
        "unreadable heap-backed native strings fail closed");

    std::memset(fixture.line[0].bytes, 0, 0x20);
    VirtualFree(inaccessible, 0, MEM_RELEASE);
}

} // namespace

int main()
{
    TestNewConversationProof();
    TestExistingConversationProof();
    TestHeapStringInspection();
    TestOfflineAndStaleRejections();
    TestWrongFriendAndMalformedLists();
    TestAmbiguousAndMalformedHistory();
    TestContentChangeAllowsReusedIdentity();
    TestProofRejectsUnappliedAndWrongId();
    TestUnreadablePointersAndNamesRejected();
    return g_failures == 0 ? 0 : 1;
}
