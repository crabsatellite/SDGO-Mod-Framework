#include "sdgo/friend_unit_gift_protocol.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using namespace sdgo::friend_unit_gift;

RequestId Id(std::uint8_t seed) {
    RequestId id;
    for (std::size_t index = 0; index < id.bytes.size(); ++index)
        id.bytes[index] =
            static_cast<std::uint8_t>(seed + index);
    return id;
}

void SetText(
    char16_t* output,
    std::uint16_t* count,
    const char16_t* value) {
    *count = static_cast<std::uint16_t>(
        std::char_traits<char16_t>::length(value));
    std::copy(value, value + *count, output);
}

} // namespace

int main() {
    using namespace sdgo::friend_unit_gift;

    std::array<std::uint8_t, kNativeProbeBytes> probe{};
    assert(EncodeProbe(&probe));
    assert(IsProbeBeforeAuthentication(probe.data(), probe.size()));
    probe[9] ^= 1;
    assert(!IsProbeBeforeAuthentication(probe.data(), probe.size()));

    Request offer;
    offer.command = Command::offer;
    offer.request_id = Id(1);
    offer.receiver_id = 4002;
    offer.unit_uid = 0x1122334455667788ull;
    SetText(
        offer.note.data(),
        &offer.note_code_units,
        u"祝你出击顺利");

    std::array<std::uint8_t, kCarrierBytes> request_bytes{};
    assert(EncodeRequest(offer, &request_bytes));
    assert(wire::Read32(request_bytes.data() + 2) == 0);
    wire::Write32(request_bytes.data() + 2, 4001);
    assert(MarkAgentValidated(
        request_bytes.data(), request_bytes.size()));

    Request decoded_request;
    std::uint32_t actor = 0;
    assert(DecodeRequest(
        request_bytes.data(),
        request_bytes.size(),
        &decoded_request,
        &actor));
    assert(actor == 4001);
    assert(decoded_request.command == Command::offer);
    assert(decoded_request.request_id == offer.request_id);
    assert(decoded_request.receiver_id == offer.receiver_id);
    assert(decoded_request.unit_uid == offer.unit_uid);
    assert(decoded_request.note_code_units ==
           offer.note_code_units);
    assert(std::equal(
        offer.note.begin(),
        offer.note.begin() + offer.note_code_units,
        decoded_request.note.begin()));

    Request receipt;
    receipt.command = Command::receipt;
    receipt.request_id = offer.request_id;
    assert(receipt.gift_id == 0);
    assert(EncodeRequest(receipt, &request_bytes));
    wire::Write32(request_bytes.data() + 2, 4001);
    assert(MarkAgentValidated(
        request_bytes.data(), request_bytes.size()));
    assert(DecodeRequest(
        request_bytes.data(),
        request_bytes.size(),
        &decoded_request,
        &actor));
    assert(decoded_request.command == Command::receipt);
    assert(decoded_request.request_id == offer.request_id);
    assert(decoded_request.gift_id == 0);

    receipt.request_id = {};
    assert(!EncodeRequest(receipt, &request_bytes));

    assert(EncodeRequest(offer, &request_bytes));
    wire::Write32(request_bytes.data() + 2, 4001);
    assert(MarkAgentValidated(
        request_bytes.data(), request_bytes.size()));

    auto forged = request_bytes;
    assert(!MarkAgentValidated(forged.data(), forged.size()));
    forged = request_bytes;
    forged[kRequestEnvelopeOffset + 0x1C] = 1;
    assert(!DecodeRequest(
        forged.data(), forged.size(), &decoded_request));
    forged = request_bytes;
    forged[kRequestEnvelopeOffset + 0x0E] |= 0x80;
    assert(!DecodeRequest(
        forged.data(), forged.size(), &decoded_request));

    Reply list;
    list.command = Command::list_inbox;
    list.status = WireStatus::success;
    list.capabilities =
        capability_protocol | capability_expanded_requests |
        capability_offer_enabled | capability_resolve_existing |
        capability_provenance | capability_agent_validated;
    list.request_id = Id(33);
    list.page = 2;
    list.total_count = 37;
    list.server_epoch = 0x123456789ABCDEF0ull;
    list.row_count = static_cast<std::uint16_t>(kMaximumRows);
    for (std::size_t index = 0; index < kMaximumRows; ++index) {
        auto& row = list.rows[index];
        row.gift_id = 9000 + index;
        row.unit_uid = 0xA000000000000000ull + index;
        row.unit_number = 100 + static_cast<std::uint32_t>(index);
        row.offered_unix_millis = 1900000000000ull + index;
        row.peer_account = 5000 + static_cast<std::uint32_t>(index);
        row.level = 20 + static_cast<std::uint32_t>(index);
        row.state = GiftState::pending;
        row.direction = ListDirection::inbox;
        row.unit_class = static_cast<std::uint8_t>(index % 4);
        row.class_mark = static_cast<std::uint8_t>(index % 3);
        SetText(
            row.peer_name.data(),
            &row.peer_name_code_units,
            u"好友");
    }

    std::array<std::uint8_t, kCarrierBytes> reply_bytes{};
    assert(EncodeReply(4001, list, &reply_bytes));
    Reply decoded_reply;
    assert(DecodeReply(
        reply_bytes.data(), reply_bytes.size(), &decoded_reply));
    assert(decoded_reply.command == Command::list_inbox);
    assert(decoded_reply.request_id == list.request_id);
    assert(decoded_reply.row_count == kMaximumRows);
    assert(decoded_reply.rows[19].gift_id ==
           list.rows[19].gift_id);
    assert(decoded_reply.rows[19].peer_name_code_units == 2);

    auto invalid_padding = reply_bytes;
    invalid_padding[
        kReplyEnvelopeOffset + kEnvelopeHeaderBytes + 0x54] = 1;
    assert(!DecodeReply(
        invalid_padding.data(),
        invalid_padding.size(),
        &decoded_reply));

    Reply detail;
    detail.command = Command::receipt;
    detail.status = WireStatus::outcome_unknown;
    detail.capabilities = list.capabilities;
    detail.request_id = Id(77);
    detail.gift_id = 9001;
    detail.unit_uid = 0x8877665544332211ull;
    detail.unit_number = 321;
    detail.state = GiftState::returned;
    detail.direction = ListDirection::returned;
    detail.peer_account = 5002;
    detail.level = 45;
    detail.unit_class = 3;
    detail.class_mark = 2;
    SetText(
        detail.peer_name.data(),
        &detail.peer_name_code_units,
        u"旧友");
    SetText(
        detail.note.data(),
        &detail.note_code_units,
        u"原始赠言仍保留");
    assert(EncodeReply(4001, detail, &reply_bytes));
    assert(DecodeReply(
        reply_bytes.data(), reply_bytes.size(), &decoded_reply));
    assert(decoded_reply.status == WireStatus::outcome_unknown);
    assert(decoded_reply.note_code_units ==
           detail.note_code_units);
    assert(decoded_reply.peer_name_code_units == 2);

    detail.flags = envelope_expanded_probe;
    assert(!EncodeReply(4001, detail, &reply_bytes));

    Request inventory;
    inventory.command=Command::inventory_returns;
    inventory.request_id=Id(90);
    assert(wire::ReadOnly(inventory.command));
    assert(EncodeRequest(inventory,&request_bytes));
    wire::Write32(request_bytes.data()+2,4001);
    assert(MarkAgentValidated(request_bytes.data(),request_bytes.size()));
    assert(DecodeRequest(request_bytes.data(),request_bytes.size(),&decoded_request,&actor));
    assert(decoded_request.command==Command::inventory_returns && actor==4001);
    Reply returned=list;
    returned.command=Command::inventory_returns;
    returned.row_count=returned.total_count=1;
    returned.direction=ListDirection::returned;
    returned.rows[0].state=GiftState::returned;
    returned.rows[0].direction=ListDirection::returned;
    assert(EncodeReply(4001,returned,&reply_bytes));
    assert(DecodeReply(reply_bytes.data(),reply_bytes.size(),&decoded_reply));
    assert(decoded_reply.command==Command::inventory_returns && decoded_reply.rows[0].state==GiftState::returned);

    std::cout << "friend unit gift protocol tests passed\n";
    return 0;
}
