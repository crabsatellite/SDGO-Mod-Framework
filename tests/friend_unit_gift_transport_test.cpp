#include "sdgo/friend_unit_gift_client_transport.h"
#include <windows.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace fixture {

using namespace sdgo::friend_unit_gift;
using namespace sdgo::friend_unit_gift::client;

std::array<std::uint8_t, 0x100> session{};
std::vector<std::vector<std::uint8_t>> sent;
std::vector<TransportEvent> events;
std::vector<Reply> event_replies;

std::uint8_t* __cdecl Session() {
    return session.data();
}

int __cdecl CaptureSend(
    void* native_session,
    int connection,
    const std::uint8_t* packet,
    int size,
    int flags) {
    assert(native_session == session.data());
    assert(connection == 7);
    assert(flags == 0);
    assert(packet);
    assert(size == static_cast<int>(kNativeProbeBytes) ||
           size == static_cast<int>(kCarrierBytes));
    sent.emplace_back(packet, packet + size);
    return 1;
}

#if defined(_M_IX86)
int __fastcall NativeSendShim(
    void* native_session,
    void*,
    int connection,
    const std::uint8_t* packet,
    int size,
    int flags) {
    return CaptureSend(
        native_session, connection, packet, size, flags);
}
#else
int NativeSendShim(
    void* native_session,
    int connection,
    const std::uint8_t* packet,
    int size,
    int flags) {
    return CaptureSend(
        native_session, connection, packet, size, flags);
}
#endif

void __cdecl Sink(
    const TransportEvent* event,
    void*) {
    assert(event);
    events.push_back(*event);
    events.back().reply = nullptr;
    if (event->reply) {
        event_replies.push_back(*event->reply);
    }
}

void SetConnection(int value) {
    std::memcpy(session.data() + 0x14, &value, sizeof(value));
}

Request DecodeSentRequest(std::size_t index) {
    assert(index < sent.size());
    assert(sent[index].size() == kCarrierBytes);
    auto bytes = sent[index];
    wire::Write32(bytes.data() + 2, 4001);
    assert(MarkAgentValidated(bytes.data(), bytes.size()));
    Request request;
    assert(DecodeRequest(bytes.data(), bytes.size(), &request));
    return request;
}

std::array<std::uint8_t, kCarrierBytes> ReplyBytes(
    const Reply& reply) {
    std::array<std::uint8_t, kCarrierBytes> bytes{};
    assert(EncodeReply(4001, reply, &bytes));
    return bytes;
}

} // namespace fixture

int main() {
    using namespace fixture;

    SetConnection(7);
    Transport transport;
    assert(transport.BindNative(
        &Session,
        reinterpret_cast<Transport::NativeSend>(&NativeSendShim)));
    transport.SetSink(&Sink, nullptr);
    assert(transport.State() == ConnectionState::disconnected);

    assert(transport.BeginProbeAt(100));
    assert(sent.size() == 1);
    assert(sent[0].size() == kNativeProbeBytes);
    assert(IsProbeBeforeAuthentication(
        sent[0].data(), sent[0].size()));
    assert(transport.State() == ConnectionState::probing_native);
    assert(transport.NativeProbeSendAttempts() == 1);
    assert(transport.NativeProbeSendSuccesses() == 1);
    assert(transport.NativeProbeLastSendResult() == 1);
    assert(transport.NativeProbeLastPacketSize() ==
           kNativeProbeBytes);
    assert(transport.NativeProbeLastSession() != 0);
    assert(transport.NativeProbeReplyCount() == 0);
    assert(transport.NativeProbeLastReason() ==
           NativeProbeReason::awaiting_reply);

    std::array<std::uint8_t, kCarrierBytes> native_reply{};
    wire::Write16(native_reply.data(), kNativeReplyOpcode);
    assert(!transport.OnStoragePacket(
        kNativeReplyOpcode,
        native_reply.data(),
        static_cast<std::uint32_t>(native_reply.size())));
    assert(transport.NativeProbeLastReason() ==
           NativeProbeReason::stock_not_extension);

    constexpr std::uint32_t capabilities =
        capability_protocol |
        capability_expanded_requests |
        capability_offer_enabled |
        capability_resolve_existing |
        capability_provenance |
        capability_agent_validated;
    Reply hello;
    hello.command = Command::hello;
    hello.capabilities = capabilities;
    hello.server_epoch = 77;
    auto bytes = ReplyBytes(hello);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(transport.NativeProbeReplyCount() == 1);
    assert(transport.NativeProbeLastReason() ==
           NativeProbeReason::capability_accepted);
    assert(sent.size() == 2);
    assert(transport.State() ==
           ConnectionState::probing_expanded);
    const auto expanded = DecodeSentRequest(1);
    assert(expanded.command == Command::hello);
    assert(!Empty(expanded.request_id));
    assert(expanded.flags & envelope_expanded_probe);
    // A real first-use policy installation exceeded the three-second native
    // discovery window. The confirmed extension must still accept its reply.
    transport.TickAt(GetTickCount64() + 5000);
    assert(transport.State() == ConnectionState::probing_expanded);

    hello.request_id = expanded.request_id;
    hello.flags = envelope_expanded_probe;
    bytes = ReplyBytes(hello);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(transport.State() == ConnectionState::ready);
    assert(transport.Capabilities() == capabilities);
    assert(transport.ServerEpoch() == 77);

    Request list;
    list.command = Command::list_inbox;
    list.page = 1;
    assert(transport.SendAt(list, 1000));
    assert(sent.size() == 3);
    const auto sent_list = DecodeSentRequest(2);
    Reply list_reply;
    list_reply.command = Command::list_inbox;
    list_reply.request_id = sent_list.request_id;
    list_reply.capabilities = capabilities;
    list_reply.total_count = 0;
    list_reply.server_epoch = 78;
    bytes = ReplyBytes(list_reply);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(!transport.HasOutstanding());
    assert(transport.ServerEpoch() == 78);

    Request offer;
    offer.command = Command::offer;
    offer.receiver_id = 4002;
    offer.unit_uid = 0x1122334455667788ull;
    assert(transport.SendAt(offer, 2000));
    assert(sent.size() == 4);
    transport.TickAt(12000);
    assert(transport.State() == ConnectionState::uncertain);
    assert(transport.MutationUncertain());
    assert(sent.size() == 4);
    assert(!transport.SendAt(offer, 12001));

    const auto sent_offer = DecodeSentRequest(3);
    assert(transport.UncertainRequestId() ==
           sent_offer.request_id);
    assert(transport.UncertainCommand() == Command::offer);
    Request receipt;
    receipt.command = Command::receipt;
    assert(transport.SendAt(receipt, 12002));
    assert(sent.size() == 5);
    const auto sent_receipt = DecodeSentRequest(4);
    assert(sent_receipt.request_id == sent_offer.request_id);
    assert(sent_receipt.gift_id == 0);
    Reply receipt_reply;
    receipt_reply.command = Command::receipt;
    receipt_reply.request_id = sent_receipt.request_id;
    receipt_reply.capabilities = capabilities;
    receipt_reply.status = WireStatus::outcome_unknown;
    receipt_reply.unit_uid = sent_offer.unit_uid;
    receipt_reply.server_epoch = 79;
    bytes = ReplyBytes(receipt_reply);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(transport.State() == ConnectionState::uncertain);
    assert(transport.MutationUncertain());
    assert(transport.UncertainRequestId() ==
           sent_offer.request_id);

    // A receipt query reuses the original mutation GUID. It never creates a
    // second mutation identity while the committed outcome is unresolved.
    assert(transport.SendAt(receipt, 12003));
    assert(sent.size() == 6);
    const auto repeated_receipt = DecodeSentRequest(5);
    assert(repeated_receipt.request_id == sent_offer.request_id);
    receipt_reply.request_id = repeated_receipt.request_id;
    receipt_reply.status = WireStatus::success;
    receipt_reply.gift_id = 9001;
    receipt_reply.server_epoch = 80;
    bytes = ReplyBytes(receipt_reply);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(transport.State() == ConnectionState::ready);
    assert(!transport.MutationUncertain());
    assert(Empty(transport.UncertainRequestId()));

    // The server can also acknowledge that execution was issued while its
    // post-commit result/receipt read remains unknown. Treat that explicit
    // reply exactly like an acknowledgement lost in transit.
    Request second_offer = offer;
    second_offer.unit_uid = 0x8877665544332211ull;
    assert(transport.SendAt(second_offer, 12500));
    assert(sent.size() == 7);
    const auto sent_second_offer = DecodeSentRequest(6);
    Reply uncertain_reply;
    uncertain_reply.command = Command::offer;
    uncertain_reply.status = WireStatus::outcome_unknown;
    uncertain_reply.request_id = sent_second_offer.request_id;
    uncertain_reply.capabilities = capabilities;
    uncertain_reply.unit_uid = sent_second_offer.unit_uid;
    uncertain_reply.server_epoch = 81;
    bytes = ReplyBytes(uncertain_reply);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(transport.State() == ConnectionState::uncertain);
    assert(transport.MutationUncertain());
    assert(transport.UncertainRequestId() ==
           sent_second_offer.request_id);

    Request second_receipt;
    second_receipt.command = Command::receipt;
    assert(transport.SendAt(second_receipt, 12501));
    assert(sent.size() == 8);
    const auto sent_second_receipt = DecodeSentRequest(7);
    assert(sent_second_receipt.request_id ==
           sent_second_offer.request_id);
    receipt_reply.request_id = sent_second_receipt.request_id;
    receipt_reply.status = WireStatus::success;
    receipt_reply.gift_id = 9002;
    receipt_reply.unit_uid = sent_second_offer.unit_uid;
    receipt_reply.server_epoch = 82;
    bytes = ReplyBytes(receipt_reply);
    assert(transport.OnStoragePacket(
        kNativeReplyOpcode,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())));
    assert(transport.State() == ConnectionState::ready);
    assert(!transport.MutationUncertain());

    Request preview;
    preview.command = Command::preview_offer;
    preview.receiver_id = 4002;
    preview.unit_uid = offer.unit_uid;
    auto without_offer = hello;
    without_offer.request_id = {};
    without_offer.flags = 0;
    without_offer.capabilities =
        capabilities & ~capability_offer_enabled;

    // A session change clears capability and requires a fresh safe probe.
    SetConnection(8);
    transport.TickAt(13000);
    assert(transport.State() == ConnectionState::idle);
    assert(!transport.SendAt(preview, 13001));

    bool saw_timeout = false;
    for (const auto& event : events) {
        if (event.kind == EventKind::request_failed &&
            event.status == WireStatus::timed_out &&
            event.command == Command::offer)
            saw_timeout = true;
    }
    assert(saw_timeout);

    SetConnection(7);
    Transport vanilla;
    assert(vanilla.BindNative(
        &Session,
        reinterpret_cast<Transport::NativeSend>(&NativeSendShim)));
    assert(vanilla.BeginProbeAt(20000));
    assert(vanilla.NativeProbeSendSuccesses() == 1);
    vanilla.TickAt(23000);
    assert(vanilla.State() == ConnectionState::unavailable);
    assert(vanilla.NativeProbeReplyCount() == 0);
    assert(vanilla.NativeProbeLastReason() ==
           NativeProbeReason::reply_timeout);

    Transport unresponsive_extension;
    assert(unresponsive_extension.BindNative(
        &Session, reinterpret_cast<Transport::NativeSend>(&NativeSendShim)));
    assert(unresponsive_extension.BeginProbeAt(30000));
    Reply extension_hello;
    extension_hello.command = Command::hello;
    extension_hello.capabilities = capabilities;
    extension_hello.server_epoch = 90;
    bytes = ReplyBytes(extension_hello);
    assert(unresponsive_extension.OnStoragePacket(
        kNativeReplyOpcode, bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    assert(unresponsive_extension.State() == ConnectionState::probing_expanded);
    unresponsive_extension.TickAt(GetTickCount64() + 180000);
    assert(unresponsive_extension.State() == ConnectionState::unavailable);
    assert(!unresponsive_extension.Ready());

    std::cout << "friend unit gift transport tests passed\n";
    return 0;
}
