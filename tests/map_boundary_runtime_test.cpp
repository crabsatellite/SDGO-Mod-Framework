#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "mods/map_catalog/boundary_runtime.h"
#include "sdgo/map_boundary_contact.h"
#include "sdgo/map_boundary_format.h"

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void U32(std::vector<std::uint8_t>& data, std::size_t at, std::uint32_t value) {
    std::memcpy(data.data() + at, &value, sizeof(value));
}
void F32(std::vector<std::uint8_t>& data, std::size_t at, float value) {
    std::memcpy(data.data() + at, &value, sizeof(value));
}
std::vector<std::uint8_t> BoundaryFile(std::vector<sdgo::map_boundary::Entry> rows) {
    using namespace sdgo::map_boundary;
    std::vector<std::uint8_t> file(kHeaderBytes + rows.size() * kRowBytes);
    std::memcpy(file.data(), kMagic, sizeof(kMagic));
    U32(file, 8, kVersion); U32(file, 12, kHeaderBytes); U32(file, 16, kRowBytes);
    U32(file, 20, static_cast<std::uint32_t>(rows.size()));
    U32(file, 24, static_cast<std::uint32_t>(rows.size() * kRowBytes));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const std::size_t at = kHeaderBytes + i * kRowBytes;
        U32(file, at + 0, row.missionId); U32(file, at + 4, row.phase);
        U32(file, at + 8, row.flags); U32(file, at + 12, row.warningSpeechId);
        F32(file, at + 16, row.left); F32(file, at + 20, row.top);
        F32(file, at + 24, row.right); F32(file, at + 28, row.bottom);
        U32(file, at + 32, row.damageEdgeMask); U32(file, at + 36, row.damagePermille);
        U32(file, at + 40, row.damageCooldownMs);
    }
    U32(file, 28, Fnv1a(file.data() + kHeaderBytes, file.size() - kHeaderBytes));
    return file;
}
}

int main() {
    using namespace sdgo::map_catalog::boundary_runtime;
    std::array<std::uint8_t, 8> first{}, second{};
    const std::uint32_t firstId = 17, secondId = 31;
    std::memcpy(first.data() + 4, &firstId, sizeof(firstId));
    std::memcpy(second.data() + 4, &secondId, sizeof(secondId));
    Node head{}, one{}, two{};
    head.next = &one; head.previous = &two;
    one.next = &two; one.previous = &head; one.object = first.data();
    two.next = &head; two.previous = &one; two.object = second.data();
    std::vector<std::uint32_t> ids;
    Check(CollectCounted(&head, 2, ids) && ids == std::vector<std::uint32_t>({17, 31}),
          "counted native list preserves every actor id");
    ids.clear();
    Check(!CollectCounted(&head, 1, ids), "count mismatch rejects a concurrently changed list");
    ids.clear();
    Check(!CollectCounted(&head, 4097, ids), "list hard limit is fail closed");
    two.object = nullptr;
    ids.clear();
    Check(!CollectCounted(&head, 2, ids), "null actor in native list is rejected");
    two.object = second.data();
    ids = {31, 17, 31}; SortUnique(ids);
    Check(ids == std::vector<std::uint32_t>({17, 31}), "actor ids are sorted and deduplicated");

    std::vector<ActorStamp> stamps{{31, second.data()}, {17, first.data()}};
    SortStamps(stamps);
    Check(FindStamp(stamps, 17) && FindStamp(stamps, 17)->object == first.data() && !FindStamp(stamps, 18),
          "canonical actor stamps use exact ids");
    std::vector<TargetCooldown> cooldowns;
    CommitCooldown(cooldowns, 17, 1000);
    CommitCooldown(cooldowns, 31, 1100);
    Check(CoolingDown(cooldowns, 17, 1199, 200) && !CoolingDown(cooldowns, 31, 1300, 200),
          "per-target cooldowns cannot starve a different actor");
    CommitCooldown(cooldowns, 17, 1400);
    Check(cooldowns.size() == 2 && CoolingDown(cooldowns, 17, 1401, 200),
          "committing an existing target updates instead of duplicating it");
    Check(ShouldApplyHealth(900, 1000, 850) &&
          !ShouldApplyHealth(900, 1000, 900) &&
          !ShouldApplyHealth(900, 1000, 950) &&
          !ShouldApplyHealth(0, 1000, 1) &&
          !ShouldApplyHealth(900, 1000, 1001),
          "authoritative boundary health is monotonic and cannot revive or heal");
    std::vector<PendingBoundaryReceipt> receipts;
    int32_t requestedBefore = 0;
    Check(RecordBoundaryRequest(receipts, 17, 900, 1000) &&
          ConsumeBoundaryReceipt(receipts, 17, 850, 1000, 850, 1100, 5000,
                                 requestedBefore) &&
          requestedBefore == 900 && receipts.empty(),
          "a server-committed equal-HP receipt proves the pending request lost health");
    Check(!ConsumeBoundaryReceipt(receipts, 17, 850, 1000, 850, 1101, 5000,
                                  requestedBefore),
          "a duplicate acknowledgement cannot replay contact feedback");
    Check(RecordBoundaryRequest(receipts, 17, 850, 1200) &&
          ConsumeBoundaryReceipt(receipts, 17, 850, 1000, 800, 1300, 5000,
                                 requestedBefore) &&
          requestedBefore == 850,
          "a lower notice may still be applied after matching a fresh request");
    Check(RecordBoundaryRequest(receipts, 17, 800, 1400) &&
          !ConsumeBoundaryReceipt(receipts, 17, 750, 1000, 800, 1500, 5000,
                                  requestedBefore) && receipts.empty(),
          "a late higher receipt cannot heal or present after later combat damage");
    Check(RecordBoundaryRequest(receipts, 17, 750, 1600) &&
          !ConsumeBoundaryReceipt(receipts, 17, 750, 1000, 700, 7001, 5000,
                                  requestedBefore) && receipts.empty(),
          "an expired request cannot authorize delayed feedback");
    Check(RecordBoundaryRequest(receipts, 17, 700, 7100) &&
          !CancelBoundaryRequest(receipts, 17, 701, 7100) && receipts.size() == 1 &&
          CancelBoundaryRequest(receipts, 17, 700, 7100) && receipts.empty(),
          "a failed send cancels only its exact pre-transport reservation");
    const void* missionMode = reinterpret_cast<const void*>(0x1234);
    Check(MissionCombatReleased(22, missionMode, missionMode, false) &&
          !MissionCombatReleased(21, missionMode, missionMode, false) &&
          !MissionCombatReleased(22, nullptr, missionMode, false) &&
          !MissionCombatReleased(22, missionMode, missionMode, true),
          "completed mission update and native demo state gate actor boundaries");
    Check(LocalRoomMaster(true, 0x1234, 0x1234) &&
          !LocalRoomMaster(false, 0x1234, 0x1234) &&
          !LocalRoomMaster(true, 0x1234, 0x5678) &&
          !LocalRoomMaster(true, 0, 0),
          "native mission host flag is bound to the real local pilot identity");
    Check(ContactFeedbackCoolingDown(1000, 1499, 500) &&
          !ContactFeedbackCoolingDown(1000, 1500, 500) &&
          !ContactFeedbackCoolingDown(0, 100, 500),
          "contact feedback throttle expires without suppressing the first effect");
    using sdgo::map_boundary_contact::kMarker;
    Check(RouteMapDamage(0xC0000042u, 0xC0000042u, kMarker) == MapDamageRoute::Native &&
          RouteMapDamage(kMarker | 0x42u, kMarker | 0x42u, kMarker) == MapDamageRoute::Native &&
          RouteMapDamage(kMarker | 0x42u, 0, kMarker) == MapDamageRoute::Boundary &&
          RouteMapDamage(0x42u, 0, kMarker) == MapDamageRoute::Native,
          "native ACE and exact marker-bit pilot identities precede boundary notice decoding");
    const auto phase = Transition(88072, 1, 88072, 2);
    const auto demo = Transition(88072, 2, 88072, 2);
    const auto switched = Transition(88072, 2, 88073, 1);
    const auto exited = Transition(88072, 2, 0, 0);
    Check(!phase.missionExitOrSwitch && phase.phaseChanged &&
          !demo.missionExitOrSwitch && !demo.phaseChanged,
          "phase and demo resets preserve mission warning lifetime");
    Check(switched.missionExitOrSwitch && exited.missionExitOrSwitch,
          "only mission switch or exit clears the warning lifetime");

    using namespace sdgo::map_boundary;
    Entry phase1{88072, 1, kAreaEnabled | kWarningEnabled | kDamageEnabled, 3229324,
                 -1000.0f, 2000.0f, 1000.0f, 500.0f, kEdgeTop, 50, 2000};
    Entry phase2{88072, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto file = BoundaryFile({phase1, phase2});
    std::vector<Entry> parsed;
    std::string error;
    Check(Parse(file.data(), file.size(), parsed, error) && parsed.size() == 2 &&
          Find(parsed, 88072, 1) && Find(parsed, 88072, 2),
          "boundary parser accepts sorted phase rows including an explicit clear");
    Check(ContactEdges(phase1, 0.0f, 2000.0f) == kEdgeTop &&
          ContactEdges(phase1, -1000.0f, 500.0f) == (kEdgeLeft | kEdgeBottom) &&
          ContactEdges(phase1, 0.0f, 2500.0f) == kEdgeTop &&
          ContactEdges(phase1, -1200.0f, 300.0f) == (kEdgeLeft | kEdgeBottom) &&
          ContactEdges(phase1, 0.0f, 1500.0f) == 0,
          "contact detection reports touched and pre-clamp beyond faces only");
    auto corrupt = file; corrupt.back() ^= 1;
    Check(!Parse(corrupt.data(), corrupt.size(), parsed, error), "payload checksum mismatch is rejected");
    auto unsorted = BoundaryFile({phase2, phase1});
    Check(!Parse(unsorted.data(), unsorted.size(), parsed, error), "unsorted mission phases are rejected");
    phase1.flags |= 0x80;
    auto unknown = BoundaryFile({phase1});
    Check(!Parse(unknown.data(), unknown.size(), parsed, error), "unknown boundary flags are rejected");

    if (failures) return 1;
    std::cout << "map boundary runtime tests passed\n";
    return 0;
}
