#ifdef NDEBUG
#undef NDEBUG
#endif

#include <windows.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "mods/map_catalog_server/damage_gate.h"

namespace {
template<class T>
void Put(void* base, ptrdiff_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}

template<class T>
T Get(const void* base, ptrdiff_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
    return value;
}

void Mission(std::vector<uint8_t>& room, uint32_t start = 12345) {
    Put<uint32_t>(room.data(), 0x21, 1);
    Put<uint8_t>(room.data(), 5, 5);
    Put<uint32_t>(room.data(), 0x1B, 88072);
    Put<uint32_t>(room.data(), 0x316A4, 2);
    Put<uint8_t>(room.data(), 0x316F0, 8);
    Put<uint32_t>(room.data(), 0x2EEB4, start);
}

void Pilot(std::vector<uint8_t>& room, uint32_t slot, uint32_t entity,
           uint32_t* npcData, uint32_t health = 100) {
    auto* pilot = room.data() + 0x3CE4 + slot * 0x1F54;
    Put<uint32_t>(pilot, 0, entity);
    Put<uint8_t>(pilot, 0x33C, 0);
    Put<uint32_t>(pilot, 0x340 + 0x34, health);
    Put<void*>(pilot, 0x1CEC, npcData);
}

void Health(std::vector<uint8_t>& room, uint32_t slot, uint32_t health) {
    auto* pilot = room.data() + 0x3CE4 + slot * 0x1F54;
    Put<uint32_t>(pilot, 0x340 + 0x34, health);
}

void Body(sdgo::map_catalog_server::DamageGate& gate, std::vector<uint8_t>& room,
          uint32_t damage, uint32_t force, uint32_t slot = 12) {
    std::vector<uint8_t> storage(0x600);
    auto* frame = storage.data() + 0x500;
    Put<void*>(frame, -0x480, room.data());
    Put<uint32_t>(frame, 0x20, damage);
    Put<uint32_t>(frame, 0x2C, force);
    Put<uint32_t>(frame, 0x34, slot);
    SetLastError(0x1234);
    gate.HandleHitFrame(frame, false);
    assert(GetLastError() == 0x1234);
    assert(Get<uint32_t>(frame, 0x20) == damage);
    assert(Get<uint32_t>(frame, 0x2C) == force);
}

void BlockedBody(sdgo::map_catalog_server::DamageGate& gate,
                 std::vector<uint8_t>& room) {
    std::vector<uint8_t> storage(0x600);
    auto* frame = storage.data() + 0x500;
    Put<void*>(frame, -0x480, room.data());
    Put<uint32_t>(frame, 0x20, 777);
    Put<uint32_t>(frame, 0x2C, 1);
    Put<uint32_t>(frame, 0x34, 12);
    gate.HandleHitFrame(frame, false);
    assert(Get<uint32_t>(frame, 0x20) == 0);
    assert(Get<uint32_t>(frame, 0x2C) == 0);
}

void BlockedPart(sdgo::map_catalog_server::DamageGate& gate,
                 std::vector<uint8_t>& room) {
    std::vector<uint8_t> storage(0x80);
    auto* frame = storage.data() + 0x30;
    Put<void*>(frame, -0x14, room.data());
    Put<uint32_t>(frame, 0x1C, 333);
    Put<uint32_t>(frame, 0x24, 1);
    Put<uint32_t>(frame, 0x30, 12);
    gate.HandleHitFrame(frame, true);
    assert(Get<uint32_t>(frame, 0x1C) == 0);
    assert(Get<uint32_t>(frame, 0x24) == 0);
}

void Loss(sdgo::map_catalog_server::DamageGate& gate, std::vector<uint8_t>& room,
          uint32_t slot, bool body, bool selected = true) {
    std::vector<uint8_t> storage(0x100);
    auto* frame = storage.data() + 0x50;
    auto* pilot = room.data() + 0x3CE4 + slot * 0x1F54;
    Put<void*>(frame, -0x34, room.data());
    Put<void*>(frame, 0x08, pilot);
    Put<void*>(frame, 0x10, body ? pilot + 0x340 + (selected ? 0 : 0x658) : nullptr);
    Put<uint32_t>(frame, 0x14, 500);
    Put<int32_t>(frame, 0x18, 1);
    SetLastError(0x5678);
    gate.HandleLossFrame(frame);
    assert(GetLastError() == 0x5678);
}
}  // namespace

int main() {
    using sdgo::map_catalog_server::DamageGate;
    using sdgo::mission_damage_gate::Entry;

    DamageGate gate;
    gate.Configure({Entry{88072, 8, 85303, 97639, 2}});
    assert(gate.Enabled());

    std::vector<uint8_t> room(0x32900);
    Mission(room);
    uint32_t targetData = 85303;
    uint32_t guardData = 97639;
    Pilot(room, 12, 0x80000001u, &targetData);
    Pilot(room, 13, 0x80000002u, &guardData);

    BlockedBody(gate, room);
    BlockedPart(gate, room);
    assert(gate.Blocked() == 2 && gate.Defeats() == 0);

    Loss(gate, room, 13, false);
    assert(gate.Defeats() == 0);
    Loss(gate, room, 13, true);  // exact guard was not seen by a hit
    assert(gate.Defeats() == 0);
    Body(gate, room, 500, 0, 13);  // observe guard, ordinary damage retained
    Loss(gate, room, 13, true);  // selected native body still has HP
    assert(gate.Defeats() == 0);
    Health(room, 13, 0);
    Loss(gate, room, 13, true, false);  // wrong form unit
    assert(gate.Defeats() == 0);
    Loss(gate, room, 13, true);
    assert(gate.Defeats() == 1);
    BlockedBody(gate, room);

    Loss(gate, room, 13, true);
    assert(gate.Defeats() == 1);
    Pilot(room, 13, 0x80000003u, &guardData, 0);
    Loss(gate, room, 13, true);  // zero HP alone is insufficient without observation
    assert(gate.Defeats() == 1);
    Body(gate, room, 500, 0, 13);
    Loss(gate, room, 13, true);
    assert(gate.Defeats() == 2);
    Body(gate, room, 777, 1);

    Mission(room, 12346);
    BlockedBody(gate, room);

    Put<uint8_t>(room.data(), 0x316F0, 7);
    Body(gate, room, 444, 1);

    // Creating a second session must use a free ledger slot instead of evicting the first.
    sdgo::map_catalog_server::DamageGateTracker tracker;
    std::vector<Entry> rules{Entry{88072, 8, 85303, 97639, 1}};
    tracker.Bind(&rules);
    std::vector<uint8_t> otherRoom(0x32900);
    const sdgo::map_catalog_server::DamageGateContext first{
        room.data(), 1, 88072, 8};
    const sdgo::map_catalog_server::DamageGateContext second{
        otherRoom.data(), 2, 88072, 8};
    assert(tracker.Observe(first, 97639, 0x80001001u));
    assert(tracker.Observe(second, 97639, 0x80001002u));
    assert(tracker.RecordDefeat(first, 97639, 0x80001001u));
    assert(!tracker.Blocks(first, 85303));
    assert(tracker.Blocks(second, 85303));

    return 0;
}
