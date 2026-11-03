#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace friends {
namespace friend_message_history {

static_assert(
    sizeof(void*) == 4,
    "friend message history models the shipped x86 client");

// Shipped 0x0953 routing fields. The native receiver uses +0x80 to select
// the ordinary online-message path and then copies it to friend +0x1C.
// +0x06 is consumed as a numeric packet identity by native validation and
// notification code, but conversation ownership is resolved by matching the
// packet nickname at +0x0B to the friend record nickname at +0x0A.
constexpr size_t kPacketIdentityOffset = 0x06;
constexpr size_t kPacketOnlineFlagOffset = 0x80;

constexpr uint32_t kMaximumFriends = 100;
constexpr uint32_t kMaximumConversations = 10;
constexpr uint32_t kMaximumLines = 200;
constexpr uint32_t kMaximumTextCharacters = 256;
constexpr uint32_t kMessageBodyStyle = 0xFFFFFFFFu;

enum InspectionStatus : uint32_t
{
    kInspectionOk = 0,
    kInspectionInvalidArgument,
    kInspectionExpectedTextMalformed,
    kInspectionPlayerDataUnreadable,
    kInspectionFriendCountInvalid,
    kInspectionFriendListMalformed,
    kInspectionFriendRecordMalformed,
    kInspectionFriendNotFound,
    kInspectionFriendAmbiguous,
    kInspectionConversationCountInvalid,
    kInspectionConversationListMalformed,
    kInspectionConversationRecordMalformed,
    kInspectionConversationAmbiguous,
    kInspectionLineListMalformed,
    kInspectionNativeStringMalformed,
    kInspectionAccessFault,
};

struct Snapshot
{
    InspectionStatus status;
    uint8_t structure_valid;
    uint8_t conversation_present;
    uint8_t tail_present;
    uint8_t tail_matches_expected;

    uint32_t accepted_friend_id;
    uint32_t friend_presence;
    uint32_t friend_count;
    uint32_t conversation_count;
    uint32_t line_count;
    uint32_t line_limit;
    uint32_t tail_length;
    uint32_t tail_capacity;
    uint32_t tail_style;
    uint32_t expected_length;

    uintptr_t friend_identity;
    uintptr_t conversation_identity;
    uintptr_t tail_node_identity;
    uintptr_t tail_payload_identity;

    wchar_t expected_text[kMaximumTextCharacters + 1];
    wchar_t tail_text[kMaximumTextCharacters + 1];
};

enum ProofStatus : uint32_t
{
    kProofApplied = 0,
    kProofBeforeInvalid,
    kProofAfterInvalid,
    kProofExpectedTextChanged,
    kProofPacketOffline,
    kProofFriendChanged,
    kProofFriendIdMismatch,
    kProofPresenceMismatch,
    kProofFriendModelChanged,
    kProofStaleExpectedTail,
    kProofConversationMissing,
    kProofConversationCapReached,
    kProofConversationCountMismatch,
    kProofConversationIdentityChanged,
    kProofLineLimitMismatch,
    kProofLineCountMismatch,
    kProofTailTextMismatch,
    kProofTailStyleMismatch,
    kProofTailUnchanged,
};

struct ApplicationProof
{
    ProofStatus status;
    uint8_t applied;
    uint8_t conversation_created;
    uint8_t tail_identity_changed;
    uint8_t tail_content_changed;
    uint32_t accepted_friend_id;
    uint32_t before_line_count;
    uint32_t after_line_count;
};

namespace detail {

inline bool Readable(const void* address, size_t size) noexcept
{
    if (!address || size == 0)
        return false;

    uintptr_t cursor =
        reinterpret_cast<uintptr_t>(address);
    if (cursor + size < cursor)
        return false;
    const uintptr_t end = cursor + size;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION information = {};
        if (VirtualQuery(
                reinterpret_cast<const void*>(cursor),
                &information,
                sizeof(information)) != sizeof(information))
            return false;
        if (information.State != MEM_COMMIT ||
            (information.Protect & PAGE_GUARD) != 0 ||
            (information.Protect & PAGE_NOACCESS) != 0)
            return false;

        const uintptr_t regionStart =
            reinterpret_cast<uintptr_t>(
                information.BaseAddress);
        const uintptr_t regionEnd =
            regionStart + information.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = regionEnd < end ? regionEnd : end;
    }
    return true;
}

template<class T>
inline bool Read(
    const void* address,
    T* output) noexcept
{
    if (!output || !Readable(address, sizeof(T)))
        return false;
    std::memcpy(output, address, sizeof(T));
    return true;
}

inline bool SameAnsiName(
    const uint8_t* record,
    const char* expected) noexcept
{
    if (!Readable(record + 0x0A, 17))
        return false;
    for (uint32_t index = 0; index < 17; ++index) {
        const char actual =
            static_cast<char>(record[0x0A + index]);
        if (actual != expected[index])
            return false;
        if (actual == '\0')
            return true;
    }
    return false;
}

inline bool ValidAnsiName(
    const uint8_t* record) noexcept
{
    if (!Readable(record + 0x0A, 17))
        return false;
    for (uint32_t index = 0; index < 17; ++index) {
        if (record[0x0A + index] == '\0')
            return index != 0;
    }
    return false;
}

inline bool PointerSeen(
    const void* const* pointers,
    uint32_t count,
    const void* value) noexcept
{
    for (uint32_t index = 0; index < count; ++index) {
        if (pointers[index] == value)
            return true;
    }
    return false;
}

struct NativeString
{
    uint32_t length;
    uint32_t capacity;
    const wchar_t* characters;
};

inline bool ReadNativeString(
    const uint8_t* storage,
    NativeString* output) noexcept
{
    if (!output || !Readable(storage, 0x1C))
        return false;

    NativeString result = {};
    if (!Read(storage + 0x10, &result.length) ||
        !Read(storage + 0x14, &result.capacity))
        return false;
    if (result.length > kMaximumTextCharacters ||
        result.length > result.capacity)
        return false;

    if (result.capacity < 8) {
        if (result.capacity != 7 || result.length > 7)
            return false;
        result.characters =
            reinterpret_cast<const wchar_t*>(storage);
    } else {
        if (result.capacity > 0x10000)
            return false;
        if (!Read(storage, &result.characters) ||
            !result.characters)
            return false;
    }

    const size_t byteCount =
        (static_cast<size_t>(result.length) + 1) *
        sizeof(wchar_t);
    if (!Readable(result.characters, byteCount))
        return false;
    for (uint32_t index = 0;
         index < result.length;
         ++index) {
        if (result.characters[index] == L'\0')
            return false;
    }
    if (result.characters[result.length] != L'\0')
        return false;

    *output = result;
    return true;
}

struct LineList
{
    uint32_t count;
    uint32_t limit;
    uintptr_t tail_node;
    uintptr_t tail_payload;
    NativeString tail_string;
    uint32_t tail_style;
};

inline bool ValidateLineList(
    const uint8_t* conversation,
    LineList* output,
    InspectionStatus* error) noexcept
{
    if (!conversation || !output || !error ||
        !Readable(conversation, 0x18)) {
        if (error)
            *error = kInspectionConversationRecordMalformed;
        return false;
    }

    uint8_t* sentinel = nullptr;
    uint32_t count = 0;
    uint32_t mirroredCount = 0;
    uint32_t limit = 0;
    if (!Read(conversation + 0x04, &sentinel) ||
        !Read(conversation + 0x08, &count) ||
        !Read(conversation + 0x10, &mirroredCount) ||
        !Read(conversation + 0x14, &limit) ||
        !sentinel || count != mirroredCount ||
        limit != kMaximumLines || count > limit ||
        !Readable(sentinel, 12)) {
        *error = kInspectionLineListMalformed;
        return false;
    }

    uint8_t* node = nullptr;
    uint8_t* previous = sentinel;
    if (!Read(sentinel, &node) || !node) {
        *error = kInspectionLineListMalformed;
        return false;
    }

    const void* nodes[kMaximumLines] = {};
    const void* payloads[kMaximumLines] = {};
    NativeString tailString = {};
    uint32_t tailStyle = 0;
    uintptr_t tailNode = 0;
    uintptr_t tailPayload = 0;

    for (uint32_t index = 0; index < count; ++index) {
        if (node == sentinel ||
            !Readable(node, 12) ||
            PointerSeen(nodes, index, node)) {
            *error = kInspectionLineListMalformed;
            return false;
        }
        nodes[index] = node;

        uint8_t* backward = nullptr;
        uint8_t* next = nullptr;
        uint8_t* payload = nullptr;
        if (!Read(node + 4, &backward) ||
            backward != previous ||
            !Read(node, &next) || !next ||
            !Readable(next, 12)) {
            *error = kInspectionLineListMalformed;
            return false;
        }
        uint8_t* nextBackward = nullptr;
        if (!Read(next + 4, &nextBackward) ||
            nextBackward != node ||
            !Read(node + 8, &payload) || !payload ||
            PointerSeen(payloads, index, payload) ||
            !Readable(payload, 0x20)) {
            *error = kInspectionLineListMalformed;
            return false;
        }
        payloads[index] = payload;

        NativeString text = {};
        if (!ReadNativeString(payload, &text)) {
            *error = kInspectionNativeStringMalformed;
            return false;
        }
        uint32_t style = 0;
        if (!Read(payload + 0x1C, &style)) {
            *error = kInspectionLineListMalformed;
            return false;
        }

        previous = node;
        node = next;
        tailNode = reinterpret_cast<uintptr_t>(previous);
        tailPayload = reinterpret_cast<uintptr_t>(payload);
        tailString = text;
        tailStyle = style;
    }

    uint8_t* sentinelPrevious = nullptr;
    if (node != sentinel ||
        !Read(sentinel + 4, &sentinelPrevious) ||
        sentinelPrevious != previous) {
        *error = kInspectionLineListMalformed;
        return false;
    }

    output->count = count;
    output->limit = limit;
    output->tail_node = tailNode;
    output->tail_payload = tailPayload;
    output->tail_string = tailString;
    output->tail_style = tailStyle;
    return true;
}

inline bool TextEqual(
    const wchar_t* left,
    uint32_t leftLength,
    const wchar_t* right,
    uint32_t rightLength) noexcept
{
    return leftLength == rightLength &&
        (leftLength == 0 ||
         std::memcmp(
             left,
             right,
             static_cast<size_t>(leftLength) *
                 sizeof(wchar_t)) == 0);
}

} // namespace detail

inline Snapshot Inspect(
    const void* playerData,
    const char* senderName,
    const wchar_t* expectedBody,
    uint32_t expectedLength) noexcept
{
    Snapshot result = {};
    result.status = kInspectionInvalidArgument;
    if (!playerData || !senderName || !expectedBody ||
        expectedLength == 0 ||
        expectedLength > kMaximumTextCharacters)
        return result;

    __try {
        char boundedName[17] = {};
        uint32_t senderLength = 0;
        for (; senderLength < 17; ++senderLength) {
            if (!detail::Readable(
                    senderName + senderLength,
                    sizeof(char)))
                return result;
            boundedName[senderLength] =
                senderName[senderLength];
            if (boundedName[senderLength] == '\0')
                break;
        }
        if (senderLength == 0 || senderLength > 16 ||
            senderLength == 17) {
            result.status = kInspectionInvalidArgument;
            return result;
        }

        const size_t expectedBytes =
            (static_cast<size_t>(expectedLength) + 1) *
            sizeof(wchar_t);
        if (!detail::Readable(expectedBody, expectedBytes)) {
            result.status =
                kInspectionExpectedTextMalformed;
            return result;
        }
        for (uint32_t index = 0;
             index < expectedLength;
             ++index) {
            if (expectedBody[index] == L'\0') {
                result.status =
                    kInspectionExpectedTextMalformed;
                return result;
            }
        }
        if (expectedBody[expectedLength] != L'\0') {
            result.status =
                kInspectionExpectedTextMalformed;
            return result;
        }
        result.expected_length = expectedLength;
        std::memcpy(
            result.expected_text,
            expectedBody,
            expectedBytes);

        const auto* player =
            static_cast<const uint8_t*>(playerData);
        if (!detail::Readable(player, 0x7B8)) {
            result.status =
                kInspectionPlayerDataUnreadable;
            return result;
        }

        uint8_t* friendHead = nullptr;
        uint32_t friendCount = 0;
        if (!detail::Read(player + 0x7A4, &friendHead) ||
            !detail::Read(player + 0x7A8, &friendCount) ||
            friendCount > kMaximumFriends) {
            result.status =
                kInspectionFriendCountInvalid;
            return result;
        }
        result.friend_count = friendCount;
        if (!friendHead ||
            !detail::Readable(friendHead, 12)) {
            result.status =
                kInspectionFriendListMalformed;
            return result;
        }

        const void* friendNodes[kMaximumFriends] = {};
        const void* friendRecords[kMaximumFriends] = {};
        uint8_t* friendNode = nullptr;
        uint8_t* previousFriendNode = friendHead;
        if (!detail::Read(friendHead, &friendNode) ||
            !friendNode) {
            result.status =
                kInspectionFriendListMalformed;
            return result;
        }

        uint32_t friendMatches = 0;
        const uint8_t* acceptedFriend = nullptr;
        for (uint32_t index = 0;
             index < friendCount;
             ++index) {
            if (friendNode == friendHead ||
                !detail::Readable(friendNode, 12) ||
                detail::PointerSeen(
                    friendNodes,
                    index,
                    friendNode)) {
                result.status =
                    kInspectionFriendListMalformed;
                return result;
            }
            friendNodes[index] = friendNode;

            uint8_t* backward = nullptr;
            uint8_t* next = nullptr;
            uint8_t* record = nullptr;
            if (!detail::Read(
                    friendNode + 4,
                    &backward) ||
                backward != previousFriendNode ||
                !detail::Read(friendNode, &next) ||
                !next || !detail::Readable(next, 12)) {
                result.status =
                    kInspectionFriendListMalformed;
                return result;
            }
            uint8_t* nextBackward = nullptr;
            if (!detail::Read(
                    next + 4,
                    &nextBackward) ||
                nextBackward != friendNode) {
                result.status =
                    kInspectionFriendListMalformed;
                return result;
            }
            if (!detail::Read(
                    friendNode + 8,
                    &record) ||
                !record ||
                !detail::Readable(record, 0x20) ||
                detail::PointerSeen(
                    friendRecords,
                    index,
                    record)) {
                result.status =
                    kInspectionFriendRecordMalformed;
                return result;
            }
            friendRecords[index] = record;

            uint32_t id = 0;
            if (!detail::Read(record, &id) || id == 0 ||
                !detail::ValidAnsiName(record)) {
                result.status =
                    kInspectionFriendRecordMalformed;
                return result;
            }
            if (detail::SameAnsiName(
                    record,
                    boundedName)) {
                ++friendMatches;
                acceptedFriend = record;
                result.accepted_friend_id = id;
                if (!detail::Read(
                        record + 0x1C,
                        &result.friend_presence)) {
                    result.status =
                        kInspectionFriendRecordMalformed;
                    return result;
                }
            }

            previousFriendNode = friendNode;
            friendNode = next;
        }

        uint8_t* friendTail = nullptr;
        if (friendNode != friendHead ||
            !detail::Read(friendHead + 4, &friendTail) ||
            friendTail != previousFriendNode) {
            result.status =
                kInspectionFriendListMalformed;
            return result;
        }
        if (friendMatches == 0) {
            result.status = kInspectionFriendNotFound;
            return result;
        }
        if (friendMatches != 1) {
            result.status = kInspectionFriendAmbiguous;
            return result;
        }
        result.friend_identity =
            reinterpret_cast<uintptr_t>(acceptedFriend);

        uint8_t* conversationHead = nullptr;
        uint32_t conversationCount = 0;
        if (!detail::Read(
                player + 0x7B0,
                &conversationHead) ||
            !detail::Read(
                player + 0x7B4,
                &conversationCount) ||
            conversationCount > kMaximumConversations) {
            result.status =
                kInspectionConversationCountInvalid;
            return result;
        }
        result.conversation_count =
            conversationCount;
        if (!conversationHead ||
            !detail::Readable(conversationHead, 12)) {
            result.status =
                kInspectionConversationListMalformed;
            return result;
        }

        const void*
            conversationNodes[kMaximumConversations] = {};
        const void*
            conversations[kMaximumConversations] = {};
        const void*
            conversationFriends[kMaximumConversations] = {};
        uint8_t* conversationNode = nullptr;
        uint8_t* previousConversationNode =
            conversationHead;
        if (!detail::Read(
                conversationHead,
                &conversationNode) ||
            !conversationNode) {
            result.status =
                kInspectionConversationListMalformed;
            return result;
        }

        uint32_t conversationMatches = 0;
        for (uint32_t index = 0;
             index < conversationCount;
             ++index) {
            if (conversationNode == conversationHead ||
                !detail::Readable(
                    conversationNode,
                    12) ||
                detail::PointerSeen(
                    conversationNodes,
                    index,
                    conversationNode)) {
                result.status =
                    kInspectionConversationListMalformed;
                return result;
            }
            conversationNodes[index] = conversationNode;

            uint8_t* backward = nullptr;
            uint8_t* next = nullptr;
            uint8_t* conversation = nullptr;
            if (!detail::Read(
                    conversationNode + 4,
                    &backward) ||
                backward != previousConversationNode ||
                !detail::Read(
                    conversationNode,
                    &next) ||
                !next || !detail::Readable(next, 12)) {
                result.status =
                    kInspectionConversationListMalformed;
                return result;
            }
            uint8_t* nextBackward = nullptr;
            if (!detail::Read(
                    next + 4,
                    &nextBackward) ||
                nextBackward != conversationNode) {
                result.status =
                    kInspectionConversationListMalformed;
                return result;
            }
            if (!detail::Read(
                    conversationNode + 8,
                    &conversation) ||
                !conversation ||
                !detail::Readable(conversation, 0x18) ||
                detail::PointerSeen(
                    conversations,
                    index,
                    conversation)) {
                result.status =
                    kInspectionConversationRecordMalformed;
                return result;
            }
            conversations[index] = conversation;

            uint8_t* owner = nullptr;
            if (!detail::Read(conversation, &owner) ||
                !owner ||
                !detail::PointerSeen(
                    friendRecords,
                    friendCount,
                    owner) ||
                detail::PointerSeen(
                    conversationFriends,
                    index,
                    owner)) {
                result.status =
                    kInspectionConversationRecordMalformed;
                return result;
            }
            conversationFriends[index] = owner;

            detail::LineList lines = {};
            InspectionStatus lineError =
                kInspectionLineListMalformed;
            if (!detail::ValidateLineList(
                    conversation,
                    &lines,
                    &lineError)) {
                result.status = lineError;
                return result;
            }

            if (owner == acceptedFriend) {
                ++conversationMatches;
                result.conversation_present = 1;
                result.conversation_identity =
                    reinterpret_cast<uintptr_t>(
                        conversation);
                result.line_count = lines.count;
                result.line_limit = lines.limit;
                if (lines.count != 0) {
                    result.tail_present = 1;
                    result.tail_node_identity =
                        lines.tail_node;
                    result.tail_payload_identity =
                        lines.tail_payload;
                    result.tail_length =
                        lines.tail_string.length;
                    result.tail_capacity =
                        lines.tail_string.capacity;
                    result.tail_style =
                        lines.tail_style;
                    std::memcpy(
                        result.tail_text,
                        lines.tail_string.characters,
                        (static_cast<size_t>(
                             lines.tail_string.length) +
                         1) * sizeof(wchar_t));
                    result.tail_matches_expected =
                        detail::TextEqual(
                            result.tail_text,
                            result.tail_length,
                            result.expected_text,
                            result.expected_length)
                        ? 1
                        : 0;
                }
            }

            previousConversationNode =
                conversationNode;
            conversationNode = next;
        }

        uint8_t* conversationTail = nullptr;
        if (conversationNode != conversationHead ||
            !detail::Read(
                conversationHead + 4,
                &conversationTail) ||
            conversationTail !=
                previousConversationNode) {
            result.status =
                kInspectionConversationListMalformed;
            return result;
        }
        if (conversationMatches > 1) {
            result.status =
                kInspectionConversationAmbiguous;
            return result;
        }

        result.status = kInspectionOk;
        result.structure_valid = 1;
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.status = kInspectionAccessFault;
        result.structure_valid = 0;
        return result;
    }
}

inline ApplicationProof Compare(
    const Snapshot& before,
    const Snapshot& after,
    uint32_t onlineFlag,
    uint32_t expectedFriendId = 0) noexcept
{
    ApplicationProof proof = {};
    proof.status = kProofBeforeInvalid;
    if (!before.structure_valid ||
        before.status != kInspectionOk)
        return proof;
    if (!after.structure_valid ||
        after.status != kInspectionOk) {
        proof.status = kProofAfterInvalid;
        return proof;
    }

    proof.accepted_friend_id =
        after.accepted_friend_id;
    proof.before_line_count = before.line_count;
    proof.after_line_count = after.line_count;

    if (!detail::TextEqual(
            before.expected_text,
            before.expected_length,
            after.expected_text,
            after.expected_length)) {
        proof.status = kProofExpectedTextChanged;
        return proof;
    }
    if (onlineFlag == 0) {
        proof.status = kProofPacketOffline;
        return proof;
    }
    if (before.accepted_friend_id !=
            after.accepted_friend_id ||
        before.friend_identity !=
            after.friend_identity) {
        proof.status = kProofFriendChanged;
        return proof;
    }
    if (expectedFriendId != 0 &&
        after.accepted_friend_id !=
            expectedFriendId) {
        proof.status = kProofFriendIdMismatch;
        return proof;
    }
    if (after.friend_presence != onlineFlag) {
        proof.status = kProofPresenceMismatch;
        return proof;
    }
    if (before.friend_count != after.friend_count) {
        proof.status = kProofFriendModelChanged;
        return proof;
    }
    if (before.tail_matches_expected) {
        proof.status = kProofStaleExpectedTail;
        return proof;
    }
    if (!after.conversation_present ||
        !after.tail_present) {
        proof.status =
            !before.conversation_present &&
                    before.conversation_count >=
                        kMaximumConversations
                ? kProofConversationCapReached
                : kProofConversationMissing;
        return proof;
    }
    if (!after.tail_matches_expected) {
        proof.status = kProofTailTextMismatch;
        return proof;
    }
    if (after.tail_style != kMessageBodyStyle) {
        proof.status = kProofTailStyleMismatch;
        return proof;
    }
    if (after.line_limit != kMaximumLines) {
        proof.status = kProofLineLimitMismatch;
        return proof;
    }

    if (before.conversation_present) {
        if (before.conversation_count !=
                after.conversation_count) {
            proof.status =
                kProofConversationCountMismatch;
            return proof;
        }
        if (before.conversation_identity !=
                after.conversation_identity) {
            proof.status =
                kProofConversationIdentityChanged;
            return proof;
        }
        if (before.line_limit != kMaximumLines) {
            proof.status = kProofLineLimitMismatch;
            return proof;
        }
        const uint32_t expectedCount =
            before.line_count >
                    kMaximumLines - 2
                ? kMaximumLines
                : before.line_count + 2;
        if (after.line_count != expectedCount) {
            proof.status = kProofLineCountMismatch;
            return proof;
        }
    } else {
        if (before.conversation_count >=
            kMaximumConversations) {
            proof.status =
                kProofConversationCapReached;
            return proof;
        }
        if (after.conversation_count !=
            before.conversation_count + 1) {
            proof.status =
                kProofConversationCountMismatch;
            return proof;
        }
        if (after.line_count != 2) {
            proof.status = kProofLineCountMismatch;
            return proof;
        }
        proof.conversation_created = 1;
    }

    proof.tail_identity_changed =
        (!before.tail_present ||
         before.tail_node_identity !=
             after.tail_node_identity ||
         before.tail_payload_identity !=
             after.tail_payload_identity)
        ? 1
        : 0;
    proof.tail_content_changed =
        (!before.tail_present ||
         before.tail_style != after.tail_style ||
         !detail::TextEqual(
             before.tail_text,
             before.tail_length,
             after.tail_text,
             after.tail_length))
        ? 1
        : 0;
    if (!proof.tail_identity_changed &&
        !proof.tail_content_changed) {
        proof.status = kProofTailUnchanged;
        return proof;
    }

    proof.status = kProofApplied;
    proof.applied = 1;
    return proof;
}

} // namespace friend_message_history
} // namespace friends
