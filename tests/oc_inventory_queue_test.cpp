#include "ai_test_support.h"
#include "../mods/oc_custom_client/oc_custom_client.cpp"

namespace {
int carrier_state = 1;
bool accept_send = true;
std::vector<Command> sent;
unsigned native_drags = 0;
void* drag_service = nullptr;
void* drag_message = nullptr;
void __fastcall NativeDrag(void* service, void*, void* message) {
    ++native_drags; drag_service = service; drag_message = message;
}
int __cdecl CarrierState() { return carrier_state; }
int __cdecl SendInventoryRequest(std::uint16_t command, const std::uint8_t* payload, std::uint32_t bytes,
                                sdgo::friend_unit_gift::ClientExtensionCallbackFn, void*) {
    UnitRequest request;
    CHECK(ReadUnitRequest(payload, bytes, &request) && request.unit_uid == 42);
    if (!accept_send) return 0;
    sent.push_back(static_cast<Command>(command));
    return 1;
}
void ResetInventoryFixture() {
    g_state_data = {};
    g_g1 = {};
    g_state_data.unit_uid = 42;
    g_state_data.mode_known = true;
    g_state_data.mode_reply_seen = true;
    g_state_data.server_oc_mode = true;
    g_send = &SendInventoryRequest;
    g_state = &CarrierState;
    carrier_state = 1;
    accept_send = true;
    sent.clear();
}
void ReplyInventory(const ChallengeItemsReply& reply, WireStatus status = WireStatus::success, bool truncate = false) {
    std::array<std::uint8_t, kChallengeItemsReplyBytes> payload{};
    WriteChallengeItemsReply(payload.data(), reply);
    OnReply(nullptr, static_cast<std::uint16_t>(Command::owned_items), static_cast<std::int32_t>(status),
            payload.data(), static_cast<std::uint32_t>(payload.size() - (truncate ? 1 : 0)));
}
}

int main() {
    ai_test::scenario = "retained success card uses a compact effect in narrow inventory cells";
    ChallengeItem card{};card.item=3240004;card.count=7;card.probability_plus=10000;
    CHECK(ItemLabel(card,true)==L"+100%");
    card.probability_plus=0;card.lv_prevention=1;
    CHECK(ItemLabel(card,true)==L"等级保护");
    ai_test::scenario = "hidden native inventory cannot drag through OC holdings";
    g_inventory_drag_start_original = reinterpret_cast<InventoryDragStart>(&NativeDrag);
    int service{}, message{};
    for (int cycle = 0; cycle < 3; ++cycle) {
        g_oc_page_shown = true;
        InventoryDragStartHook(&service, nullptr, &message);
        CHECK(native_drags == static_cast<unsigned>(cycle));
        g_oc_page_shown = false;
        InventoryDragStartHook(&service, nullptr, &message);
        CHECK(native_drags == static_cast<unsigned>(cycle + 1));
        CHECK(drag_service == &service && drag_message == &message);
    }
    ResetInventoryFixture();
    auto& s = g_state_data;
    ai_test::scenario = "background holdings survive a foreground reply scheduling another read";
    s.outstanding = Pending::mode;
    s.owned_wanted = true;
    PumpQueue();
    CHECK(s.owned_wanted && s.queued == Pending::none && sent.empty());
    // The mode/profile reply may enqueue the next foreground query. It must
    // not replace an already-consumed background inventory reservation.
    s.outstanding = Pending::none;
    s.queued = Pending::profile;
    PumpQueue();
    CHECK(sent.size() == 1 && sent.back() == Command::get_profile && s.owned_wanted);
    s.outstanding = Pending::none;
    PumpQueue();
    CHECK(sent.size() == 2 && sent.back() == Command::owned_items && !s.owned_wanted);
    CHECK(s.owned_load == InventoryLoad::loading);
    PumpQueue();
    CHECK(sent.size() == 2);

    ai_test::scenario = "busy carrier and rejected sends retain the refresh";
    ResetInventoryFixture();
    s.owned_wanted = true;
    carrier_state = 2;
    PumpQueue();
    CHECK(s.owned_wanted && s.queued == Pending::none && sent.empty());
    carrier_state = 1;
    accept_send = false;
    PumpQueue();
    CHECK(s.owned_wanted && s.outstanding == Pending::none && sent.empty());
    accept_send = true;
    PumpQueue();
    CHECK(sent.size() == 1 && sent.back() == Command::owned_items);

    ai_test::scenario = "empty inventory differs from failed or malformed inventory";
    ChallengeItemsReply items{};
    ReplyInventory(items);
    CHECK(s.owned_load == InventoryLoad::ready && s.owned.count == 0);
    items.count = 1;
    items.items[0].item = 3240005;
    items.items[0].count = 2;
    ReplyInventory(items);
    CHECK(s.owned_load == InventoryLoad::ready && s.owned.count == 1 && s.owned.items[0].count == 2);
    ReplyInventory(items, WireStatus::success, true);
    CHECK(s.owned_load == InventoryLoad::failed && s.owned.count == 0);
    ReplyInventory(items, WireStatus::database_unavailable);
    CHECK(s.owned_load == InventoryLoad::failed && s.owned.count == 0);
    items.ret_val = 1;
    ReplyInventory(items);
    CHECK(s.owned_load == InventoryLoad::failed && s.owned.count == 0);
    items.ret_val = 0;
    ReplyInventory(items);
    CHECK(s.owned_load == InventoryLoad::ready && s.owned.count == 1);

    ai_test::scenario = "page entry progresses continuously and cancels without drift";
    ui::PageSlide slide;
    CHECK(!slide.Moving() && slide.Offset(-300) == -300 && slide.Offset(400) == 400);
    for (int cycle = 0; cycle < 3; ++cycle) {
        slide.Show(true);
        CHECK(slide.Moving() && slide.Offset(-300) == -300 && slide.Offset(400) == 400);
        float previous = slide.Offset(400);
        for (int frame = 0; frame < 20; ++frame) {
            slide.Show(true); // a reply/timer render must not restart entry
            slide.Advance();
            CHECK(slide.Offset(400) < previous && slide.Offset(400) >= 0);
            previous = slide.Offset(400);
        }
        CHECK(!slide.Moving() && slide.Offset(400) == 0);
        slide.Show(false);
        CHECK(slide.Offset(-300) == -300 && slide.Offset(400) == 400);
        slide.Advance(); // hidden native updates must not bring panels on screen
        CHECK(slide.Offset(-300) == -300 && slide.Offset(400) == 400);
    }
    slide.Show(true);
    slide.Advance();
    slide.Show(false);
    CHECK(!slide.Moving() && slide.Offset(-300) == -300);
    return ai_test::Finish();
}
