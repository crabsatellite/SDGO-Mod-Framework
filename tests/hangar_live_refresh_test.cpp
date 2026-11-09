// Offline contract of the hangar live refresh (mods/hangar_manager/live_refresh.h).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../mods/hangar_manager/live_refresh.h"

namespace live = sdgo::hangar::live;

namespace {

struct Fake {
    std::map<int, uint32_t> cache;  // slot -> unit type (the native brief)
    std::vector<uint32_t> requestedPages;
    std::vector<uint16_t> cleared;
    int userInfo = 0;
    bool sendOk = true;
};

live::Io MakeIo(Fake& fake) {
    live::Io io;
    io.user = &fake;
    io.send = [](void* user, const uint8_t* bytes, int size) {
        auto& f = *static_cast<Fake*>(user);
        assert(size == 10 && bytes[0] == 0xFF && bytes[1] == 0x04);
        uint32_t zero = 1, page = 0;
        std::memcpy(&zero, bytes + 2, 4);
        std::memcpy(&page, bytes + 6, 4);
        assert(zero == 0);
        f.requestedPages.push_back(page);
        return f.sendOk;
    };
    io.write = [](void* user, const uint8_t* record) {
        auto& f = *static_cast<Fake*>(user);
        uint32_t type = 0;
        uint16_t slot = 0;
        std::memcpy(&type, record, 4);
        std::memcpy(&slot, record + 4, 2);
        f.cache[slot] = type;
    };
    io.occupied = [](void* user, uint16_t slot) {
        auto& f = *static_cast<Fake*>(user);
        auto it = f.cache.find(slot);
        return it != f.cache.end() && it->second != 0;
    };
    io.clear = [](void* user, uint16_t slot) {
        auto& f = *static_cast<Fake*>(user);
        f.cache.erase(slot);
        f.cleared.push_back(slot);
    };
    io.user_info = [](void* user) {
        ++static_cast<Fake*>(user)->userInfo;
        return true;
    };
    return io;
}

live::Packet Ack(uint16_t opcode) {
    uint8_t bytes[8] = {};
    std::memcpy(bytes, &opcode, 2);
    live::Packet p;
    assert(live::Capture(opcode, bytes, &p));
    return p;
}

// A 0x08F2 page carrying (slot, type) records.
struct Rec {
    uint16_t slot;
    uint32_t type;
};

live::Packet Page(const std::vector<Rec>& records) {
    std::vector<uint8_t> bytes(7 + records.size() * live::kRecordBytes, 0);
    const uint16_t op = live::kPageReply;
    std::memcpy(bytes.data(), &op, 2);
    bytes[6] = static_cast<uint8_t>(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        uint8_t* r = bytes.data() + 7 + i * live::kRecordBytes;
        std::memcpy(r, &records[i].type, 4);
        std::memcpy(r + 4, &records[i].slot, 2);
    }
    live::Packet p;
    assert(live::Capture(op, bytes.data(), &p));
    return p;
}

void TestCapture() {
    uint8_t bytes[16] = {};
    live::Packet p;
    assert(!live::Capture(0x0918, bytes, &p));
    bytes[6] = 7;  // more than six records is not a slot page
    assert(!live::Capture(live::kPageReply, bytes, &p));
    bytes[6] = 0;
    assert(live::Capture(live::kPageReply, bytes, &p) && p.size == 7);
    assert(!live::Capture(live::kSendOk, nullptr, &p));
}

void TestSortRefresh() {
    Fake f;
    // Before: slots 1,2,5 occupied; slot 13 beyond the 12-slot capacity is impossible.
    f.cache = {{1, 100}, {2, 200}, {5, 300}};
    auto io = MakeIo(f);
    live::Refresh r;
    r.Begin(1, 0, 15000, 20000);
    r.SendsDone();
    assert(!r.Tick(10, 12, {}, io) && r.state() == live::State::AwaitAck);
    assert(!r.Tick(20, 12, {Ack(live::kSendOk)}, io) && r.state() == live::State::Reload);
    assert(r.pages() == 2 && f.requestedPages == std::vector<uint32_t>({0, 1}));
    // Server order after the sort: slots 1..3 hold 300, 200, 100; page 1 is empty.
    assert(!r.Tick(30, 12, {Page({{1, 300}, {2, 200}, {3, 100}})}, io));
    assert(r.Tick(40, 12, {Page({})}, io));
    assert(r.result() == live::Result::Refreshed && r.state() == live::State::Idle && r.generation() == 1);
    assert(f.cache.size() == 3 && f.cache[1] == 300 && f.cache[2] == 200 && f.cache[3] == 100);
    assert(f.cleared == std::vector<uint16_t>({5}) && r.cleared() == 1 && f.userInfo == 1);
}

void TestRefusedAndTimeout() {
    Fake f;
    auto io = MakeIo(f);
    live::Refresh r;
    r.Begin(3, 0, 1000, 2000);
    assert(!r.Tick(1, 12, {Ack(live::kSendOk)}, io));
    assert(r.Tick(2, 12, {Ack(live::kSendRefused)}, io) && r.result() == live::Result::Refused);
    assert(f.requestedPages.empty() && f.userInfo == 0);

    // All acknowledgements but the sends never finished: waits, then times out.
    r.Begin(1, 0, 1000, 2000);
    assert(!r.Tick(1, 12, {Ack(live::kSendOk)}, io) && r.state() == live::State::AwaitAck);
    assert(r.Tick(1000, 12, {}, io) && r.result() == live::Result::Unknown);

    // Page replies missing: the reload times out without clearing anything.
    f.cache = {{1, 100}};
    r.Begin(1, 0, 1000, 2000);
    r.SendsDone();
    r.Tick(1, 12, {Ack(live::kSendOk)}, io);
    assert(!r.Tick(2, 12, {Page({})}, io));
    assert(r.Tick(5000, 12, {}, io) && r.result() == live::Result::Unknown);
    assert(f.cleared.empty() && f.cache.size() == 1);

    // A page error keeps the old cache (re-login path).
    r.Begin(1, 0, 1000, 2000);
    r.SendsDone();
    r.Tick(1, 6, {Ack(live::kSendOk)}, io);
    assert(r.Tick(2, 6, {Ack(live::kPageError)}, io) && r.result() == live::Result::Unknown);

    // A failing send ends the refresh.
    f.sendOk = false;
    r.Begin(1, 0, 1000, 2000);
    r.SendsDone();
    assert(r.Tick(1, 12, {Ack(live::kSendOk)}, io) && r.result() == live::Result::Unknown);
}

void TestCapacityAndDuplicates() {
    Fake f;
    auto io = MakeIo(f);
    live::Refresh r;
    // A record beyond the client's allocated slots cannot be written natively.
    r.Begin(1, 0, 1000, 2000);
    r.SendsDone();
    r.Tick(1, 6, {Ack(live::kSendOk)}, io);
    assert(r.Tick(2, 6, {Page({{7, 100}})}, io) && r.result() == live::Result::Unknown);
    assert(f.cache.empty());

    // A native lazy load of an already answered page does not complete the reload early.
    f = Fake{};
    io = MakeIo(f);
    r.Begin(1, 0, 1000, 2000);
    r.SendsDone();
    r.Tick(1, 18, {Ack(live::kSendOk)}, io);
    assert(r.pages() == 3);
    assert(!r.Tick(2, 18, {Page({{1, 100}}), Page({{1, 100}}), Page({{7, 200}})}, io));
    assert(r.Tick(3, 18, {Page({{13, 300}})}, io) && r.result() == live::Result::Refreshed);

    // GB batch: one answer per message, the reload waits for all of them.
    f = Fake{};
    f.cache = {{1, 100}, {2, 100}, {3, 100}};
    io = MakeIo(f);
    r.Begin(3, 0, 1000, 2000);
    assert(!r.Tick(1, 6, {Ack(live::kSendOk), Ack(live::kSendOk)}, io));
    r.SendsDone();
    assert(!r.Tick(2, 6, {}, io) && r.state() == live::State::AwaitAck);
    assert(!r.Tick(3, 6, {Ack(live::kSendOk)}, io) && r.state() == live::State::Reload);
    assert(r.Tick(4, 6, {Page({{1, 100}, {2, 100}})}, io) && r.result() == live::Result::Refreshed);
    assert(f.cache.size() == 2 && f.cleared == std::vector<uint16_t>({3}));
}

} // namespace

int main() {
    TestCapture();
    TestSortRefresh();
    TestRefusedAndTimeout();
    TestCapacityAndDuplicates();
    std::puts("hangar live refresh offline contract passed");
    return 0;
}
