#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sdgo::friend_unit_gift {

inline constexpr std::uint16_t kNativeRequestOpcode = 0x09A2;
inline constexpr std::uint16_t kNativeReplyOpcode = 0x07BE;
inline constexpr std::size_t kNativeProbeBytes = 10;
inline constexpr std::size_t kCarrierBytes = 0x07EA;
inline constexpr std::size_t kRequestEnvelopeOffset = 0x0A;
inline constexpr std::size_t kReplyEnvelopeOffset = 0x10;
inline constexpr std::size_t kEnvelopeHeaderBytes = 0x70;
inline constexpr std::size_t kSummaryBytes = 0x58;
inline constexpr std::size_t kDetailPrefixBytes = 0x34;
inline constexpr std::size_t kMaximumNoteCodeUnits = 200;
inline constexpr std::size_t kMaximumPeerNameCodeUnits = 20;
inline constexpr std::size_t kMaximumRows = 20;
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint32_t kProbeActorMarker = 0x31544746u; // "FGT1"
inline constexpr std::array<std::uint8_t, 8> kEnvelopeMagic = {
    'S', 'D', 'G', 'O', 'G', 'I', 'F', 'T'
};

enum class Command : std::uint16_t {
    hello = 1,
    offer = 2,
    list_inbox = 3,
    list_returned = 4,
    detail = 5,
    accept = 6,
    return_to_sender = 7,
    takeback = 8,
    provenance = 9,
    preview_offer = 10,
    receipt = 11,
    inventory_returns = 12,
};

// Extension commands let other Mods ride the same authenticated carrier.
// Their envelope payload is opaque bytes owned by the registered handler; the
// envelope header fields that gift commands use stay zero. Extension commands
// are idempotent by contract (the owning Mod carries its own revision), so
// the transport treats them as read-only and never enters the uncertain state.
inline constexpr std::uint16_t kExtensionCommandFirst = 0x1000;
inline constexpr std::uint16_t kExtensionCommandLast = 0x10FF;
inline constexpr std::size_t kMaximumExtensionPayloadBytes = 1024;

inline bool IsExtensionCommand(Command command) noexcept {
    const auto value = static_cast<std::uint16_t>(command);
    return value >= kExtensionCommandFirst && value <= kExtensionCommandLast;
}

inline bool IsListCommand(Command command) noexcept {
    return command == Command::list_inbox || command == Command::list_returned ||
           command == Command::inventory_returns;
}

enum class WireStatus : std::int32_t {
    success = 0,
    disabled = 1,
    invalid_argument = 2,
    account_unavailable = 3,
    not_mutual_friend = 4,
    sender_not_eligible = 5,
    unit_unavailable = 6,
    unit_ineligible = 7,
    capacity_full = 8,
    not_authorized = 9,
    wrong_state = 10,
    returning_waiting_capacity = 11,
    request_conflict = 12,
    transport_invalid = 100,
    transport_unavailable = 101,
    database_unavailable = 102,
    timed_out = 103,
    send_failed = 104,
    disconnected = 105,
    outcome_unknown = 106,
};

enum EnvelopeFlag : std::uint16_t {
    envelope_agent_validated = 1u << 0,
    envelope_read_only = 1u << 1,
    envelope_expanded_probe = 1u << 2,
};

enum Capability : std::uint32_t {
    capability_protocol = 1u << 0,
    capability_expanded_requests = 1u << 1,
    capability_offer_enabled = 1u << 2,
    capability_resolve_existing = 1u << 3,
    capability_provenance = 1u << 4,
    capability_agent_validated = 1u << 5,
};

enum class GiftState : std::uint8_t {
    pending = 0,
    accepted = 1,
    returning_waiting_capacity = 2,
    returned = 3,
};

enum class ListDirection : std::uint8_t {
    inbox = 0,
    returned = 1,
};

struct RequestId {
    std::array<std::uint8_t, 16> bytes{};
};

inline bool operator==(const RequestId& left,
                       const RequestId& right) noexcept {
    return left.bytes == right.bytes;
}

inline bool operator!=(const RequestId& left,
                       const RequestId& right) noexcept {
    return !(left == right);
}

inline bool Empty(const RequestId& id) noexcept {
    std::uint8_t combined = 0;
    for (const auto value : id.bytes) combined |= value;
    return combined == 0;
}

struct Request {
    Command command = Command::hello;
    std::uint16_t flags = 0;
    RequestId request_id{};
    std::uint64_t gift_id = 0;
    std::uint64_t unit_uid = 0;
    std::uint32_t receiver_id = 0;
    std::uint32_t page = 1;
    std::uint16_t note_code_units = 0;
    std::array<char16_t, kMaximumNoteCodeUnits> note{};
    // Extension commands only.
    std::uint16_t extension_bytes = 0;
    std::array<std::uint8_t, kMaximumExtensionPayloadBytes> extension{};
};

struct GiftSummary {
    std::uint64_t gift_id = 0;
    std::uint64_t unit_uid = 0;
    std::uint32_t unit_number = 0;
    std::uint64_t offered_unix_millis = 0;
    std::uint32_t peer_account = 0;
    std::uint32_t level = 0;
    GiftState state = GiftState::pending;
    ListDirection direction = ListDirection::inbox;
    std::uint8_t unit_class = 0;
    std::uint8_t class_mark = 0;
    std::uint16_t peer_name_code_units = 0;
    std::array<char16_t, kMaximumPeerNameCodeUnits> peer_name{};
};

struct Reply {
    Command command = Command::hello;
    std::uint16_t flags = 0;
    WireStatus status = WireStatus::success;
    std::uint32_t capabilities = 0;
    RequestId request_id{};
    std::uint64_t gift_id = 0;
    std::uint64_t unit_uid = 0;
    std::uint32_t receiver_id = 0;
    std::uint32_t page = 1;
    std::uint32_t total_count = 0;
    std::uint64_t server_epoch = 0;
    std::uint64_t offered_unix_millis = 0;
    std::uint64_t accepted_unix_millis = 0;
    std::uint32_t unit_number = 0;
    GiftState state = GiftState::pending;
    ListDirection direction = ListDirection::inbox;
    std::uint32_t peer_account = 0;
    std::uint32_t level = 0;
    std::uint8_t unit_class = 0;
    std::uint8_t class_mark = 0;
    std::uint16_t peer_name_code_units = 0;
    std::array<char16_t, kMaximumPeerNameCodeUnits> peer_name{};
    std::uint16_t note_code_units = 0;
    std::array<char16_t, kMaximumNoteCodeUnits> note{};
    std::uint16_t row_count = 0;
    std::array<GiftSummary, kMaximumRows> rows{};
    // Extension commands only.
    std::uint16_t extension_bytes = 0;
    std::array<std::uint8_t, kMaximumExtensionPayloadBytes> extension{};
};

namespace wire {

inline std::uint16_t Read16(const std::uint8_t* value) noexcept {
    return static_cast<std::uint16_t>(value[0]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(value[1]) << 8u);
}

inline std::uint32_t Read32(const std::uint8_t* value) noexcept {
    return static_cast<std::uint32_t>(value[0]) |
           (static_cast<std::uint32_t>(value[1]) << 8u) |
           (static_cast<std::uint32_t>(value[2]) << 16u) |
           (static_cast<std::uint32_t>(value[3]) << 24u);
}

inline std::uint64_t Read64(const std::uint8_t* value) noexcept {
    return static_cast<std::uint64_t>(Read32(value)) |
           (static_cast<std::uint64_t>(Read32(value + 4)) << 32u);
}

inline void Write16(std::uint8_t* output, std::uint16_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8u);
}

inline void Write32(std::uint8_t* output, std::uint32_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8u);
    output[2] = static_cast<std::uint8_t>(value >> 16u);
    output[3] = static_cast<std::uint8_t>(value >> 24u);
}

inline void Write64(std::uint8_t* output, std::uint64_t value) noexcept {
    Write32(output, static_cast<std::uint32_t>(value));
    Write32(output + 4, static_cast<std::uint32_t>(value >> 32u));
}

inline bool ValidUtf16(const char16_t* value,
                       std::size_t codeUnits) noexcept {
    if (!value && codeUnits) return false;
    for (std::size_t index = 0; index < codeUnits; ++index) {
        const auto character = static_cast<std::uint16_t>(value[index]);
        if (character == 0) return false;
        if (character >= 0xD800u && character <= 0xDBFFu) {
            if (++index >= codeUnits) return false;
            const auto low = static_cast<std::uint16_t>(value[index]);
            if (low < 0xDC00u || low > 0xDFFFu) return false;
        } else if (character >= 0xDC00u && character <= 0xDFFFu) {
            return false;
        }
    }
    return true;
}

inline void WriteUtf16(std::uint8_t* output,
                       const char16_t* value,
                       std::size_t codeUnits) noexcept {
    for (std::size_t index = 0; index < codeUnits; ++index)
        Write16(output + index * 2,
                static_cast<std::uint16_t>(value[index]));
}

inline void ReadUtf16(const std::uint8_t* input,
                      char16_t* value,
                      std::size_t codeUnits) noexcept {
    for (std::size_t index = 0; index < codeUnits; ++index)
        value[index] = static_cast<char16_t>(Read16(input + index * 2));
}

inline bool ValidCommand(Command command) noexcept {
    const auto value = static_cast<std::uint16_t>(command);
    return (value >= static_cast<std::uint16_t>(Command::hello) &&
            value <= static_cast<std::uint16_t>(Command::inventory_returns)) ||
           IsExtensionCommand(command);
}

inline bool ReadOnly(Command command) noexcept {
    return IsExtensionCommand(command) ||
           command == Command::hello ||
           command == Command::list_inbox ||
           command == Command::list_returned ||
           command == Command::inventory_returns ||
           command == Command::detail ||
           command == Command::provenance ||
           command == Command::preview_offer ||
           command == Command::receipt;
}

inline bool ValidState(GiftState state) noexcept {
    return state == GiftState::pending ||
           state == GiftState::accepted ||
           state == GiftState::returning_waiting_capacity ||
           state == GiftState::returned;
}

inline bool ValidDirection(ListDirection direction) noexcept {
    return direction == ListDirection::inbox ||
           direction == ListDirection::returned;
}

inline bool ValidStatus(WireStatus status) noexcept {
    switch (status) {
    case WireStatus::success:
    case WireStatus::disabled:
    case WireStatus::invalid_argument:
    case WireStatus::account_unavailable:
    case WireStatus::not_mutual_friend:
    case WireStatus::sender_not_eligible:
    case WireStatus::unit_unavailable:
    case WireStatus::unit_ineligible:
    case WireStatus::capacity_full:
    case WireStatus::not_authorized:
    case WireStatus::wrong_state:
    case WireStatus::returning_waiting_capacity:
    case WireStatus::request_conflict:
    case WireStatus::transport_invalid:
    case WireStatus::transport_unavailable:
    case WireStatus::database_unavailable:
    case WireStatus::timed_out:
    case WireStatus::send_failed:
    case WireStatus::disconnected:
    case WireStatus::outcome_unknown:
        return true;
    }
    return false;
}

inline bool ValidRequest(const Request& request,
                         bool agentGeneratedHello = false) noexcept {
    if (!ValidCommand(request.command) ||
        (request.flags & ~(envelope_agent_validated |
                           envelope_read_only |
                           envelope_expanded_probe)) != 0 ||
        ((request.flags & envelope_expanded_probe) != 0 &&
         request.command != Command::hello) ||
        request.note_code_units > kMaximumNoteCodeUnits ||
        !ValidUtf16(request.note.data(), request.note_code_units))
        return false;
    if (request.command != Command::hello && Empty(request.request_id))
        return false;
    if (request.command == Command::hello &&
        !agentGeneratedHello && Empty(request.request_id))
        return false;
    if (IsExtensionCommand(request.command)) {
        return request.note_code_units == 0 && !request.gift_id &&
               !request.unit_uid && !request.receiver_id &&
               request.page == 1 &&
               request.extension_bytes <= kMaximumExtensionPayloadBytes;
    }
    if (request.extension_bytes != 0) return false;
    switch (request.command) {
    case Command::hello:
        return request.note_code_units == 0;
    case Command::offer:
        return request.receiver_id && request.unit_uid &&
               request.note_code_units <= kMaximumNoteCodeUnits;
    case Command::preview_offer:
        return request.receiver_id && request.unit_uid &&
               request.note_code_units == 0;
    case Command::list_inbox:
    case Command::list_returned:
    case Command::inventory_returns:
        return request.page > 0 && request.note_code_units == 0;
    case Command::detail:
    case Command::accept:
    case Command::return_to_sender:
    case Command::takeback:
        return request.gift_id && request.note_code_units == 0;
    case Command::receipt:
        // Receipt lookup is keyed by the original mutation request ID.
        // gift_id may be zero because a committed mutation can lose its
        // acknowledgement before the client learns the allocated gift ID.
        return request.note_code_units == 0;
    case Command::provenance:
        return request.unit_uid && request.note_code_units == 0;
    }
    return false;
}

inline bool ValidReply(const Reply& reply) noexcept {
    if (!ValidCommand(reply.command) ||
        !ValidStatus(reply.status) ||
        (reply.flags & ~(envelope_agent_validated |
                         envelope_read_only |
                         envelope_expanded_probe)) != 0 ||
        ((reply.flags & envelope_expanded_probe) != 0 &&
         reply.command != Command::hello) ||
        (((reply.flags & envelope_read_only) != 0) !=
         ReadOnly(reply.command)) ||
        !ValidState(reply.state) ||
        !ValidDirection(reply.direction) ||
        reply.note_code_units > kMaximumNoteCodeUnits ||
        reply.peer_name_code_units > kMaximumPeerNameCodeUnits ||
        reply.row_count > kMaximumRows ||
        !ValidUtf16(reply.note.data(), reply.note_code_units) ||
        !ValidUtf16(reply.peer_name.data(),
                    reply.peer_name_code_units))
        return false;
    if (IsExtensionCommand(reply.command)) {
        return reply.row_count == 0 && reply.note_code_units == 0 &&
               reply.peer_name_code_units == 0 &&
               reply.extension_bytes <= kMaximumExtensionPayloadBytes;
    }
    if (reply.extension_bytes != 0) return false;
    for (std::size_t index = 0; index < reply.row_count; ++index) {
        const auto& row = reply.rows[index];
        if (!row.gift_id || !row.unit_uid ||
            !ValidState(row.state) ||
            !ValidDirection(row.direction) ||
            row.peer_name_code_units > kMaximumPeerNameCodeUnits ||
            !ValidUtf16(row.peer_name.data(),
                        row.peer_name_code_units))
            return false;
    }
    const bool list = IsListCommand(reply.command);
    const bool detail = reply.command == Command::detail ||
                        reply.command == Command::provenance ||
                        reply.command == Command::preview_offer ||
                        reply.command == Command::receipt;
    if ((!list && reply.row_count != 0) ||
        (!detail && reply.note_code_units != 0) ||
        (list && reply.note_code_units != 0))
        return false;
    return true;
}

inline void WriteRequestId(std::uint8_t* output,
                           const RequestId& id) noexcept {
    std::memcpy(output, id.bytes.data(), id.bytes.size());
}

inline void ReadRequestId(const std::uint8_t* input,
                          RequestId* id) noexcept {
    std::memcpy(id->bytes.data(), input, id->bytes.size());
}

inline bool WriteEnvelopeHeader(std::uint8_t* envelope,
                                Command command,
                                std::uint16_t flags,
                                std::uint32_t payloadBytes,
                                std::int32_t status,
                                std::uint32_t capabilities,
                                const RequestId& requestId,
                                std::uint64_t giftId,
                                std::uint64_t unitUid,
                                std::uint32_t receiverId,
                                std::uint32_t page,
                                std::uint32_t totalCount,
                                std::uint16_t rowCount,
                                std::uint16_t noteCodeUnits,
                                std::uint64_t serverEpoch,
                                std::uint64_t offeredUnixMillis,
                                std::uint64_t acceptedUnixMillis,
                                std::uint32_t unitNumber,
                                GiftState state,
                                ListDirection direction) noexcept {
    if (!envelope || !ValidCommand(command)) return false;
    std::memcpy(envelope, kEnvelopeMagic.data(), kEnvelopeMagic.size());
    Write16(envelope + 0x08, kProtocolVersion);
    Write16(envelope + 0x0A,
            static_cast<std::uint16_t>(kEnvelopeHeaderBytes));
    Write16(envelope + 0x0C, static_cast<std::uint16_t>(command));
    Write16(envelope + 0x0E, flags);
    Write32(envelope + 0x10, payloadBytes);
    Write32(envelope + 0x14, static_cast<std::uint32_t>(status));
    Write32(envelope + 0x18, capabilities);
    Write32(envelope + 0x1C, 0);
    WriteRequestId(envelope + 0x20, requestId);
    Write64(envelope + 0x30, giftId);
    Write64(envelope + 0x38, unitUid);
    Write32(envelope + 0x40, receiverId);
    Write32(envelope + 0x44, page);
    Write32(envelope + 0x48, totalCount);
    Write16(envelope + 0x4C, rowCount);
    Write16(envelope + 0x4E, noteCodeUnits);
    Write64(envelope + 0x50, serverEpoch);
    Write64(envelope + 0x58, offeredUnixMillis);
    Write64(envelope + 0x60, acceptedUnixMillis);
    Write32(envelope + 0x68, unitNumber);
    envelope[0x6C] = static_cast<std::uint8_t>(state);
    envelope[0x6D] = static_cast<std::uint8_t>(direction);
    envelope[0x6E] = 0;
    envelope[0x6F] = 0;
    return true;
}

inline bool ReadEnvelopeHeader(const std::uint8_t* envelope,
                               std::size_t capacity,
                               Command* command,
                               std::uint16_t* flags,
                               std::uint32_t* payloadBytes,
                               std::int32_t* status,
                               std::uint32_t* capabilities,
                               RequestId* requestId,
                               std::uint64_t* giftId,
                               std::uint64_t* unitUid,
                               std::uint32_t* receiverId,
                               std::uint32_t* page,
                               std::uint32_t* totalCount,
                               std::uint16_t* rowCount,
                               std::uint16_t* noteCodeUnits,
                               std::uint64_t* serverEpoch,
                               std::uint64_t* offeredUnixMillis,
                               std::uint64_t* acceptedUnixMillis,
                               std::uint32_t* unitNumber,
                               GiftState* state,
                               ListDirection* direction) noexcept {
    if (!envelope || capacity < kEnvelopeHeaderBytes ||
        std::memcmp(envelope, kEnvelopeMagic.data(),
                    kEnvelopeMagic.size()) != 0 ||
        Read16(envelope + 0x08) != kProtocolVersion ||
        Read16(envelope + 0x0A) != kEnvelopeHeaderBytes ||
        Read32(envelope + 0x1C) != 0 ||
        Read16(envelope + 0x6E) != 0)
        return false;
    const auto decoded =
        static_cast<Command>(Read16(envelope + 0x0C));
    if (!ValidCommand(decoded)) return false;
    const auto payload = Read32(envelope + 0x10);
    if (payload > capacity - kEnvelopeHeaderBytes) return false;
    *command = decoded;
    *flags = Read16(envelope + 0x0E);
    *payloadBytes = payload;
    *status = static_cast<std::int32_t>(Read32(envelope + 0x14));
    *capabilities = Read32(envelope + 0x18);
    ReadRequestId(envelope + 0x20, requestId);
    *giftId = Read64(envelope + 0x30);
    *unitUid = Read64(envelope + 0x38);
    *receiverId = Read32(envelope + 0x40);
    *page = Read32(envelope + 0x44);
    *totalCount = Read32(envelope + 0x48);
    *rowCount = Read16(envelope + 0x4C);
    *noteCodeUnits = Read16(envelope + 0x4E);
    *serverEpoch = Read64(envelope + 0x50);
    *offeredUnixMillis = Read64(envelope + 0x58);
    *acceptedUnixMillis = Read64(envelope + 0x60);
    *unitNumber = Read32(envelope + 0x68);
    const auto decodedState =
        static_cast<GiftState>(envelope[0x6C]);
    const auto decodedDirection =
        static_cast<ListDirection>(envelope[0x6D]);
    if (!ValidState(decodedState) ||
        !ValidDirection(decodedDirection))
        return false;
    *state = decodedState;
    *direction = decodedDirection;
    return true;
}

} // namespace wire

inline bool EncodeProbe(std::array<std::uint8_t, kNativeProbeBytes>* output)
    noexcept {
    if (!output) return false;
    output->fill(0);
    wire::Write16(output->data(), kNativeRequestOpcode);
    wire::Write32(output->data() + 2, kProbeActorMarker);
    wire::Write32(output->data() + 6, 1);
    return true;
}

inline bool IsProbeBeforeAuthentication(const std::uint8_t* data,
                                        std::size_t size) noexcept {
    return data && size == kNativeProbeBytes &&
           wire::Read16(data) == kNativeRequestOpcode &&
           wire::Read32(data + 2) == kProbeActorMarker &&
           wire::Read32(data + 6) == 1;
}

inline bool EncodeRequest(
    const Request& request,
    std::array<std::uint8_t, kCarrierBytes>* output,
    bool agentGeneratedHello = false) noexcept {
    if (!output ||
        (!agentGeneratedHello &&
         (request.flags & envelope_agent_validated) != 0) ||
        !wire::ValidRequest(request, agentGeneratedHello))
        return false;
    output->fill(0);
    wire::Write16(output->data(), kNativeRequestOpcode);
    wire::Write32(output->data() + 2, 0); // Agent always replaces this.
    wire::Write32(output->data() + 6, 1);
    auto flags = request.flags;
    if (wire::ReadOnly(request.command))
        flags = static_cast<std::uint16_t>(
            flags | envelope_read_only);
    const bool extension = IsExtensionCommand(request.command);
    const auto payloadBytes = extension
        ? static_cast<std::uint32_t>(request.extension_bytes)
        : static_cast<std::uint32_t>(request.note_code_units * 2u);
    auto* envelope = output->data() + kRequestEnvelopeOffset;
    if (!wire::WriteEnvelopeHeader(
            envelope, request.command, flags, payloadBytes,
            static_cast<std::int32_t>(WireStatus::success), 0,
            request.request_id, request.gift_id, request.unit_uid,
            request.receiver_id, request.page, 0, 0,
            request.note_code_units, 0, 0, 0, 0,
            GiftState::pending, ListDirection::inbox))
        return false;
    if (extension)
        std::memcpy(envelope + kEnvelopeHeaderBytes,
                    request.extension.data(), request.extension_bytes);
    else
        wire::WriteUtf16(envelope + kEnvelopeHeaderBytes,
                         request.note.data(), request.note_code_units);
    return true;
}

inline bool DecodeRequest(const std::uint8_t* data,
                          std::size_t size,
                          Request* output,
                          std::uint32_t* authenticatedActor = nullptr,
                          bool requireAgentValidation = true) noexcept {
    if (!data || !output || size != kCarrierBytes ||
        wire::Read16(data) != kNativeRequestOpcode ||
        wire::Read32(data + 6) != 1)
        return false;
    const auto actor = wire::Read32(data + 2);
    if (!actor || actor > 0x7FFFFFFFu) return false;
    Request request;
    std::uint32_t payloadBytes = 0;
    std::int32_t ignoredStatus = 0;
    std::uint32_t ignoredCapabilities = 0, ignoredTotal = 0;
    std::uint16_t ignoredRows = 0;
    std::uint64_t ignoredEpoch = 0, ignoredOffered = 0;
    std::uint64_t ignoredAccepted = 0;
    std::uint32_t ignoredUnitNumber = 0;
    GiftState ignoredState{};
    ListDirection ignoredDirection{};
    if (!wire::ReadEnvelopeHeader(
            data + kRequestEnvelopeOffset,
            kCarrierBytes - kRequestEnvelopeOffset,
            &request.command, &request.flags, &payloadBytes,
            &ignoredStatus,
            &ignoredCapabilities, &request.request_id,
            &request.gift_id, &request.unit_uid,
            &request.receiver_id, &request.page, &ignoredTotal,
            &ignoredRows, &request.note_code_units, &ignoredEpoch,
            &ignoredOffered, &ignoredAccepted, &ignoredUnitNumber,
            &ignoredState, &ignoredDirection) ||
        (requireAgentValidation &&
         !(request.flags & envelope_agent_validated)) ||
        ignoredStatus !=
            static_cast<std::int32_t>(WireStatus::success) ||
        ignoredCapabilities != 0 || ignoredTotal != 0 ||
        ignoredRows != 0 || ignoredEpoch != 0 ||
        ignoredOffered != 0 || ignoredAccepted != 0 ||
        ignoredUnitNumber != 0 ||
        ignoredState != GiftState::pending ||
        ignoredDirection != ListDirection::inbox ||
        (((request.flags & envelope_read_only) != 0) !=
         wire::ReadOnly(request.command)) ||
        request.note_code_units > kMaximumNoteCodeUnits)
        return false;
    if (IsExtensionCommand(request.command)) {
        if (request.note_code_units != 0 ||
            payloadBytes > kMaximumExtensionPayloadBytes)
            return false;
        request.extension_bytes = static_cast<std::uint16_t>(payloadBytes);
        std::memcpy(request.extension.data(),
                    data + kRequestEnvelopeOffset + kEnvelopeHeaderBytes,
                    payloadBytes);
    } else {
        if (payloadBytes !=
            static_cast<std::uint32_t>(request.note_code_units * 2u))
            return false;
        wire::ReadUtf16(
            data + kRequestEnvelopeOffset + kEnvelopeHeaderBytes,
            request.note.data(), request.note_code_units);
    }
    const bool generatedHello =
        request.command == Command::hello &&
        Empty(request.request_id) &&
        (request.flags & envelope_agent_validated);
    if (!wire::ValidRequest(request, generatedHello)) return false;
    *output = request;
    if (authenticatedActor) *authenticatedActor = actor;
    return true;
}

inline bool MarkAgentValidated(
    std::uint8_t* data,
    std::size_t size) noexcept {
    if (!data || size != kCarrierBytes ||
        wire::Read16(data) != kNativeRequestOpcode ||
        wire::Read32(data + 2) == 0)
        return false;
    auto* flags = data + kRequestEnvelopeOffset + 0x0E;
    const auto original = wire::Read16(flags);
    if (original & envelope_agent_validated)
        return false;
    wire::Write16(flags, static_cast<std::uint16_t>(
        original | envelope_agent_validated));
    Request request;
    std::uint32_t actor = 0;
    if (!DecodeRequest(data, size, &request, &actor, true)) {
        wire::Write16(flags, original);
        return false;
    }
    return actor != 0;
}

inline bool EncodeReply(
    std::uint32_t authenticatedActor,
    const Reply& reply,
    std::array<std::uint8_t, kCarrierBytes>* output) noexcept {
    Reply normalized = reply;
    if (wire::ReadOnly(normalized.command))
        normalized.flags = static_cast<std::uint16_t>(
            normalized.flags | envelope_read_only);
    else
        normalized.flags = static_cast<std::uint16_t>(
            normalized.flags & ~envelope_read_only);
    if (!authenticatedActor || !output ||
        !wire::ValidReply(normalized))
        return false;
    output->fill(0);
    wire::Write16(output->data(), kNativeReplyOpcode);
    wire::Write32(output->data() + 6, authenticatedActor);
    wire::Write32(output->data() + 12, 0);
    std::uint32_t payloadBytes = 0;
    if (IsExtensionCommand(normalized.command)) {
        payloadBytes = normalized.extension_bytes;
    } else if (IsListCommand(normalized.command)) {
        payloadBytes =
            static_cast<std::uint32_t>(
                normalized.row_count * kSummaryBytes);
    } else if (normalized.command == Command::detail ||
               normalized.command == Command::provenance ||
               normalized.command == Command::preview_offer ||
               normalized.command == Command::receipt) {
        payloadBytes = static_cast<std::uint32_t>(
            kDetailPrefixBytes +
            normalized.note_code_units * 2u);
    }
    auto* envelope = output->data() + kReplyEnvelopeOffset;
    if (!wire::WriteEnvelopeHeader(
            envelope, normalized.command, normalized.flags,
            payloadBytes,
            static_cast<std::int32_t>(normalized.status),
            normalized.capabilities, normalized.request_id,
            normalized.gift_id, normalized.unit_uid,
            normalized.receiver_id, normalized.page,
            normalized.total_count, normalized.row_count,
            normalized.note_code_units, normalized.server_epoch,
            normalized.offered_unix_millis,
            normalized.accepted_unix_millis,
            normalized.unit_number, normalized.state,
            normalized.direction))
        return false;
    auto* payload = envelope + kEnvelopeHeaderBytes;
    if (IsExtensionCommand(normalized.command)) {
        std::memcpy(payload, normalized.extension.data(),
                    normalized.extension_bytes);
    } else if (IsListCommand(normalized.command)) {
        for (std::size_t index = 0;
             index < normalized.row_count;
             ++index) {
            const auto& row = normalized.rows[index];
            auto* encoded = payload + index * kSummaryBytes;
            wire::Write64(encoded + 0x00, row.gift_id);
            wire::Write64(encoded + 0x08, row.unit_uid);
            wire::Write32(encoded + 0x10, row.unit_number);
            wire::Write64(encoded + 0x14, row.offered_unix_millis);
            wire::Write32(encoded + 0x1C, row.peer_account);
            wire::Write32(encoded + 0x20, row.level);
            encoded[0x24] = static_cast<std::uint8_t>(row.state);
            encoded[0x25] = static_cast<std::uint8_t>(row.direction);
            encoded[0x26] = row.unit_class;
            encoded[0x27] = row.class_mark;
            wire::Write16(encoded + 0x28,
                          row.peer_name_code_units);
            wire::Write16(encoded + 0x2A, 0);
            wire::WriteUtf16(encoded + 0x2C,
                             row.peer_name.data(),
                             row.peer_name_code_units);
        }
    } else if (normalized.command == Command::detail ||
               normalized.command == Command::provenance ||
               normalized.command == Command::preview_offer ||
               normalized.command == Command::receipt) {
        wire::Write32(payload + 0x00, normalized.peer_account);
        wire::Write32(payload + 0x04, normalized.level);
        payload[0x08] = normalized.unit_class;
        payload[0x09] = normalized.class_mark;
        wire::Write16(payload + 0x0A,
                      normalized.peer_name_code_units);
        wire::WriteUtf16(payload + 0x0C,
                         normalized.peer_name.data(),
                         normalized.peer_name_code_units);
        wire::WriteUtf16(payload + kDetailPrefixBytes,
                         normalized.note.data(),
                         normalized.note_code_units);
    }
    return true;
}

inline bool DecodeReply(const std::uint8_t* data,
                        std::size_t size,
                        Reply* output) noexcept {
    if (!data || !output || size != kCarrierBytes ||
        wire::Read16(data) != kNativeReplyOpcode ||
        wire::Read32(data + 2) != 0 ||
        wire::Read16(data + 10) != 0)
        return false;
    Reply reply;
    std::uint32_t payloadBytes = 0;
    std::int32_t status = 0;
    if (!wire::ReadEnvelopeHeader(
            data + kReplyEnvelopeOffset,
            kCarrierBytes - kReplyEnvelopeOffset,
            &reply.command, &reply.flags, &payloadBytes, &status,
            &reply.capabilities, &reply.request_id, &reply.gift_id,
            &reply.unit_uid, &reply.receiver_id, &reply.page,
            &reply.total_count, &reply.row_count,
            &reply.note_code_units, &reply.server_epoch,
            &reply.offered_unix_millis,
            &reply.accepted_unix_millis, &reply.unit_number,
            &reply.state, &reply.direction))
        return false;
    reply.status = static_cast<WireStatus>(status);
    const auto* payload =
        data + kReplyEnvelopeOffset + kEnvelopeHeaderBytes;
    if (IsExtensionCommand(reply.command)) {
        if (reply.row_count != 0 || reply.note_code_units != 0 ||
            payloadBytes > kMaximumExtensionPayloadBytes)
            return false;
        reply.extension_bytes = static_cast<std::uint16_t>(payloadBytes);
        std::memcpy(reply.extension.data(), payload, payloadBytes);
    } else if (IsListCommand(reply.command)) {
        if (reply.row_count > kMaximumRows ||
            reply.note_code_units != 0 ||
            payloadBytes !=
                static_cast<std::uint32_t>(
                    reply.row_count * kSummaryBytes))
            return false;
        for (std::size_t index = 0; index < reply.row_count; ++index) {
            auto& row = reply.rows[index];
            const auto* encoded = payload + index * kSummaryBytes;
            row.gift_id = wire::Read64(encoded + 0x00);
            row.unit_uid = wire::Read64(encoded + 0x08);
            row.unit_number = wire::Read32(encoded + 0x10);
            row.offered_unix_millis =
                wire::Read64(encoded + 0x14);
            row.peer_account = wire::Read32(encoded + 0x1C);
            row.level = wire::Read32(encoded + 0x20);
            row.state = static_cast<GiftState>(encoded[0x24]);
            row.direction =
                static_cast<ListDirection>(encoded[0x25]);
            row.unit_class = encoded[0x26];
            row.class_mark = encoded[0x27];
            row.peer_name_code_units =
                wire::Read16(encoded + 0x28);
            if (row.peer_name_code_units >
                    kMaximumPeerNameCodeUnits ||
                wire::Read16(encoded + 0x2A) != 0 ||
                wire::Read32(encoded + 0x54) != 0)
                return false;
            wire::ReadUtf16(encoded + 0x2C,
                            row.peer_name.data(),
                            row.peer_name_code_units);
        }
    } else if (reply.command == Command::detail ||
               reply.command == Command::provenance ||
               reply.command == Command::preview_offer ||
               reply.command == Command::receipt) {
        if (reply.note_code_units > kMaximumNoteCodeUnits ||
            payloadBytes !=
                static_cast<std::uint32_t>(
                    kDetailPrefixBytes +
                    reply.note_code_units * 2u))
            return false;
        reply.peer_account = wire::Read32(payload + 0x00);
        reply.level = wire::Read32(payload + 0x04);
        reply.unit_class = payload[0x08];
        reply.class_mark = payload[0x09];
        reply.peer_name_code_units =
            wire::Read16(payload + 0x0A);
        if (reply.peer_name_code_units >
            kMaximumPeerNameCodeUnits)
            return false;
        wire::ReadUtf16(payload + 0x0C,
                        reply.peer_name.data(),
                        reply.peer_name_code_units);
        wire::ReadUtf16(payload + kDetailPrefixBytes,
                        reply.note.data(),
                        reply.note_code_units);
    } else if (payloadBytes != 0 || reply.row_count != 0 ||
               reply.note_code_units != 0) {
        return false;
    }
    if (!wire::ValidReply(reply)) return false;
    *output = reply;
    return true;
}

static_assert(kMaximumRows * kSummaryBytes <=
                  kCarrierBytes - kReplyEnvelopeOffset -
                      kEnvelopeHeaderBytes,
              "fixed reply carrier row capacity exceeds carrier");
static_assert(kMaximumExtensionPayloadBytes <=
                  kCarrierBytes - kReplyEnvelopeOffset -
                      kEnvelopeHeaderBytes,
              "extension payload exceeds the reply carrier");
static_assert(kMaximumExtensionPayloadBytes <=
                  kCarrierBytes - kRequestEnvelopeOffset -
                      kEnvelopeHeaderBytes,
              "extension payload exceeds the request carrier");

// Extension registration on the World host (friend_unit_gift_server.dll).
// Handlers run on the native request thread with the request's own database
// sessions; they return a WireStatus value and fill the reply payload.
using ExtensionHandlerFn = std::int32_t(__cdecl*)(
    void* user,
    std::uint32_t authenticated_actor,
    std::uint16_t command,
    const std::uint8_t* payload,
    std::uint32_t payload_bytes,
    void* member_database,
    void* game_database,
    std::uint8_t* reply_payload,
    std::uint32_t reply_capacity,
    std::uint32_t* reply_bytes);
using RegisterExtensionFn = int(__cdecl*)(
    std::uint16_t first_command,
    std::uint16_t last_command,
    ExtensionHandlerFn handler,
    void* user);
#define SDGO_FRIEND_UNIT_GIFT_REGISTER_EXTENSION_EXPORT \
    "SdgoFriendUnitGiftRegisterExtensionV1"

// Extension sending on the client host (friend_unit_gift_client.dll). The
// callback runs on the game thread once, with the reply payload on success or
// a transport WireStatus (and no payload) on failure.
using ClientExtensionCallbackFn = void(__cdecl*)(
    void* user,
    std::uint16_t command,
    std::int32_t status,
    const std::uint8_t* payload,
    std::uint32_t payload_bytes);
using ClientExtensionSendFn = int(__cdecl*)(
    std::uint16_t command,
    const std::uint8_t* payload,
    std::uint32_t payload_bytes,
    ClientExtensionCallbackFn callback,
    void* user);
// 0 transport not ready, 1 ready and idle, 2 ready with a request outstanding.
using ClientExtensionStateFn = int(__cdecl*)();
#define SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT \
    "SdgoFriendUnitGiftClientExtensionSendV1"
#define SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT \
    "SdgoFriendUnitGiftClientExtensionStateV1"

} // namespace sdgo::friend_unit_gift
