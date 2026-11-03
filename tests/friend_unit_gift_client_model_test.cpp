#include "../mods/friend_unit_gift_client/client_model.h"

#include <cstdlib>
#include <iostream>

namespace {

using namespace sdgo::friend_unit_gift;
using namespace sdgo::friend_unit_gift::client;

void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}

RequestId Id(std::uint8_t value) {
    RequestId id;
    id.bytes[0] = value;
    id.bytes[15] = static_cast<std::uint8_t>(value ^ 0x5A);
    return id;
}

Reply CapabilitiesReply(
    Command command,
    RequestId id,
    std::uint32_t capabilities,
    std::uint64_t epoch = 1) {
    Reply reply;
    reply.command = command;
    reply.request_id = id;
    reply.capabilities = capabilities;
    reply.server_epoch = epoch;
    return reply;
}

constexpr std::uint32_t kAllCapabilities =
    capability_protocol |
    capability_expanded_requests |
    capability_offer_enabled |
    capability_resolve_existing |
    capability_provenance |
    capability_agent_validated;

} // namespace

int main() {
    Model model;
    model.SetSession(0x100);
    Check(!model.CanOffer(), "offer is hidden before hello capabilities");

    Request hello;
    hello.command = Command::hello;
    hello.request_id = Id(1);
    Check(model.Begin(hello, 10), "hello begins before capabilities");
    auto helloReply =
        CapabilitiesReply(Command::hello, hello.request_id, kAllCapabilities);
    Check(model.Receive(0x100, helloReply), "matching hello reply accepted");
    Check(
        model.CanOffer() &&
            model.CanResolveExisting() &&
            model.CanReadProvenance(),
        "capability gates become available after reply");

    UnitSnapshot selected;
    selected.uid = 0x1122334455667788ull;
    selected.unit_number = 572;
    selected.slot = 4;
    selected.full_record[0] = 0x3C;
    const char16_t note[] = u"给你的礼物🙂";
    Check(
        model.PrepareOffer(
            42, selected, note,
            static_cast<std::uint16_t>(std::size(note) - 1)),
        "Unicode note and exact owned unit enter confirmation");
    Check(model.HasOfferConfirmation(), "offer requires confirmation");
    Check(
        !model.ConfirmOffer(selected, Id(2)).has_value(),
        "local click cannot bypass server preview");

    auto preview = model.PreviewOffer(Id(3));
    Check(preview && model.Begin(*preview, 11), "offer preview begins read-only");
    auto previewReply = CapabilitiesReply(
        Command::preview_offer, preview->request_id, kAllCapabilities);
    previewReply.receiver_id = 42;
    previewReply.unit_uid = selected.uid;
    Check(model.Receive(0x100, previewReply), "server preview is correlated");

    auto changed = selected;
    changed.full_record[0] ^= 1;
    Check(
        !model.ConfirmOffer(changed, Id(4)).has_value() &&
            !model.HasOfferConfirmation(),
        "changed record fails closed and does not auto retry");

    Check(
        model.PrepareOffer(
            42, selected, note,
            static_cast<std::uint16_t>(std::size(note) - 1)),
        "player may explicitly select and confirm again");
    preview = model.PreviewOffer(Id(5));
    Check(preview && model.Begin(*preview, 12), "second preview begins");
    previewReply = CapabilitiesReply(
        Command::preview_offer, preview->request_id, kAllCapabilities);
    previewReply.receiver_id = 42;
    previewReply.unit_uid = selected.uid;
    Check(model.Receive(0x100, previewReply), "second preview succeeds");
    auto offer = model.ConfirmOffer(selected, Id(6));
    Check(
        offer && offer->receiver_id == 42 &&
            offer->unit_uid == selected.uid &&
            offer->note_code_units == std::size(note) - 1,
        "confirmed offer preserves UID and Unicode note");
    Check(model.Begin(*offer, 20), "confirmed offer starts once");
    Check(!model.Begin(*offer, 21), "pending mutation is never duplicated");
    auto offerReply =
        CapabilitiesReply(Command::offer, offer->request_id, kAllCapabilities);
    offerReply.gift_id = 88;
    offerReply.unit_uid = selected.uid;
    Check(model.Receive(0x100, offerReply), "offer commit reply correlates");

    Request list;
    list.command = Command::list_inbox;
    list.request_id = Id(7);
    list.page = 1;
    Check(model.Begin(list, 30), "custom inbox request begins");
    auto listReply =
        CapabilitiesReply(Command::list_inbox, list.request_id, kAllCapabilities);
    listReply.total_count = 1;
    listReply.row_count = 1;
    listReply.rows[0].gift_id = 88;
    listReply.rows[0].unit_uid = selected.uid;
    listReply.rows[0].unit_number = 572;
    listReply.rows[0].peer_name_code_units = 2;
    listReply.rows[0].peer_name[0] = u'好';
    listReply.rows[0].peer_name[1] = u'友';
    Check(model.Receive(0x100, listReply), "custom inbox reply accepted");
    Check(
        model.row_count() == 1 &&
            model.row(0)->gift_id == 88 &&
            model.BlockNativeClaimAll(),
        "custom rows remain separate and block native Claim All only in view");
    model.LeaveCustomStorageView();
    Check(
        !model.BlockNativeClaimAll(),
        "ordinary native gift view retains native Claim All");

    Request accept;
    accept.command = Command::accept;
    accept.request_id = Id(8);
    accept.gift_id = 88;
    Check(model.Begin(accept, 40), "accept begins explicitly");
    Check(model.Timeout(100, 50), "mutation timeout stops request");
    Check(
        model.last_status() == WireStatus::timed_out && !model.pending(),
        "timed out mutation is not automatically resent");
    model.SyncCapabilities(0x100, kAllCapabilities, 1);
    Check(
        model.last_status() == WireStatus::timed_out,
        "periodic capability sync preserves the operation result");

    // Turning new offers off must preserve resolution and provenance.
    Request refresh;
    refresh.command = Command::hello;
    refresh.request_id = Id(9);
    Check(model.Begin(refresh, 110), "capabilities can refresh");
    auto disabled = CapabilitiesReply(
        Command::hello, refresh.request_id,
        kAllCapabilities & ~capability_offer_enabled);
    Check(model.Receive(0x100, disabled), "disabled capability reply accepted");
    Check(
        !model.CanOffer() &&
            model.CanResolveExisting() &&
            model.CanReadProvenance(),
        "feature off forbids only new offers");

    Request provenance;
    provenance.command = Command::provenance;
    provenance.request_id = Id(10);
    provenance.unit_uid = selected.uid;
    Check(model.Begin(provenance, 120), "provenance request remains available");
    auto provenanceReply = CapabilitiesReply(
        Command::provenance, provenance.request_id,
        disabled.capabilities);
    provenanceReply.unit_uid = selected.uid;
    provenanceReply.gift_id = 88;
    provenanceReply.peer_account = 42;
    provenanceReply.note_code_units = 2;
    provenanceReply.note[0] = u'礼';
    provenanceReply.note[1] = u'物';
    Check(
        model.Receive(0x100, provenanceReply) &&
            model.provenance().present &&
            model.provenance().unit_uid == selected.uid,
        "accepted UID provenance is retained");
    provenance.request_id = Id(11);
    Check(
        model.Begin(provenance, 121) &&
            !model.provenance().present,
        "a new provenance lookup clears stale per-UID text");
    Check(
        model.Fail(
            0x100,
            Command::provenance,
            provenance.request_id,
            WireStatus::transport_unavailable) &&
            !model.provenance().present,
        "failed lookup cannot redisplay old provenance");
    model.SyncCapabilities(0x100, disabled.capabilities, 2);
    Check(
        !model.provenance().present &&
            !model.custom_storage_view() &&
            model.server_epoch() == 2,
        "server epoch change invalidates cached rows and provenance");

    model.SetSession(0x200);
    Check(
        !model.CanOffer() &&
            !model.provenance().present &&
            !model.custom_storage_view(),
        "session change clears stale capabilities and metadata");

    std::cout << "friend_unit_gift_client_model_test: PASS\n";
    return 0;
}
