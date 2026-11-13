#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sdgo::map_catalog::boundary_runtime {

struct Node {
    Node* next;
    Node* previous;
    uint8_t* object;
};

struct ActorStamp {
    uint32_t id;
    uint8_t* object;
};

struct TargetCooldown {
    uint32_t target;
    uint64_t at;
};

struct PendingBoundaryReceipt {
    uint32_t target;
    int32_t before;
    uint64_t at;
};

struct BoundaryTransition {
    bool missionExitOrSwitch;
    bool phaseChanged;
};

enum class MapDamageRoute {
    Native,
    Boundary,
};

// CMissionMode::Update has completed before the boundary tick. Native mission
// control is released while the same mission mode remains current and its
// CScriptControlMode demo byte is clear. Player input state belongs to the
// replaceable local unit and must not release or disable every actor boundary.
inline bool MissionCombatReleased(uint32_t appState, const void* currentMode,
                                  const void* mode, bool demoPlaying) {
    return appState == 22 && mode && currentMode == mode && !demoPlaying;
}

inline bool LocalRoomMaster(bool nativeMaster, uint32_t roomLocalPilot,
                           uint32_t unitPilot) {
    return nativeMaster && roomLocalPilot && roomLocalPilot == unitPilot;
}


inline bool ContactFeedbackCoolingDown(uint64_t last, uint64_t now,
                                       uint32_t duration) {
    return last && now >= last && now - last < duration;
}

// Existing native identities own the native 0793 namespace even if a future
// or legacy pilot id happens to contain the extension marker bit.
inline MapDamageRoute RouteMapDamage(uint32_t wirePilot, uint32_t exactNativePilot,
                                     uint32_t marker) {
    if (wirePilot && exactNativePilot == wirePilot) return MapDamageRoute::Native;
    return (wirePilot & marker) ? MapDamageRoute::Boundary : MapDamageRoute::Native;
}

inline BoundaryTransition Transition(uint32_t currentMission, uint32_t currentPhase,
                                     uint32_t nextMission, uint32_t nextPhase) {
    const bool missionExitOrSwitch = currentMission != 0 && currentMission != nextMission;
    return {missionExitOrSwitch,
            currentMission == nextMission && currentMission != 0 && currentPhase != nextPhase};
}

inline bool CollectCounted(Node* head, uint32_t expected, std::vector<uint32_t>& ids,
                           uint32_t hardLimit = 4096) {
    if (!head || expected > hardLimit) return false;
    Node* node = head->next;
    uint32_t seen = 0;
    while (node && node != head && seen < expected) {
        Node* next = node->next;
        if (!node->object) return false;
        const uint32_t id = *reinterpret_cast<const uint32_t*>(node->object + 4);
        if (!id) return false;
        ids.push_back(id);
        node = next;
        ++seen;
    }
    return node == head && seen == expected;
}

inline bool CollectBounded(Node* head, std::vector<uint32_t>& ids, uint32_t hardLimit = 4096) {
    if (!head) return true;
    Node* node = head->next;
    uint32_t seen = 0;
    while (node && node != head && seen < hardLimit) {
        Node* next = node->next;
        if (!node->object) return false;
        const uint32_t id = *reinterpret_cast<const uint32_t*>(node->object + 4);
        if (!id) return false;
        ids.push_back(id);
        node = next;
        ++seen;
    }
    return node == head;
}

inline void SortUnique(std::vector<uint32_t>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

inline const ActorStamp* FindStamp(const std::vector<ActorStamp>& stamps, uint32_t id) {
    const auto found = std::lower_bound(stamps.begin(), stamps.end(), id,
        [](const ActorStamp& stamp, uint32_t value) { return stamp.id < value; });
    return found != stamps.end() && found->id == id ? &*found : nullptr;
}

inline void SortStamps(std::vector<ActorStamp>& stamps) {
    std::sort(stamps.begin(), stamps.end(),
              [](const ActorStamp& left, const ActorStamp& right) { return left.id < right.id; });
}

inline bool CoolingDown(const std::vector<TargetCooldown>& cooldowns, uint32_t target,
                        uint64_t now, uint32_t duration) {
    const auto found = std::find_if(cooldowns.begin(), cooldowns.end(),
        [target](const TargetCooldown& entry) { return entry.target == target; });
    return found != cooldowns.end() && now >= found->at && now - found->at < duration;
}

inline void CommitCooldown(std::vector<TargetCooldown>& cooldowns, uint32_t target, uint64_t now) {
    const auto found = std::find_if(cooldowns.begin(), cooldowns.end(),
        [target](const TargetCooldown& entry) { return entry.target == target; });
    if (found != cooldowns.end()) found->at = now;
    else cooldowns.push_back({target, now});
}

inline bool RecordBoundaryRequest(std::vector<PendingBoundaryReceipt>& receipts,
                                  uint32_t target, int32_t before, uint64_t now,
                                  size_t hardLimit = 4096) {
    if (!target || before <= 0) return false;
    const auto found = std::find_if(receipts.begin(), receipts.end(),
        [target](const PendingBoundaryReceipt& entry) { return entry.target == target; });
    if (found != receipts.end()) {
        *found = {target, before, now};
        return true;
    }
    if (receipts.size() >= hardLimit) return false;
    receipts.push_back({target, before, now});
    return true;
}

inline bool CancelBoundaryRequest(std::vector<PendingBoundaryReceipt>& receipts,
                                  uint32_t target, int32_t before, uint64_t now) {
    const auto found = std::find_if(receipts.begin(), receipts.end(),
        [target, before, now](const PendingBoundaryReceipt& entry) {
            return entry.target == target && entry.before == before && entry.at == now;
        });
    if (found == receipts.end()) return false;
    receipts.erase(found);
    return true;
}

// The server commits boundary HP before broadcasting its receipt. Native health
// synchronization may therefore reach the client first. Match the receipt to a
// request recorded with a strictly higher pre-contact HP instead of requiring
// this callback itself to perform the decrease. Consuming the record also makes
// a duplicate acknowledgement silent.
inline bool ConsumeBoundaryReceipt(std::vector<PendingBoundaryReceipt>& receipts,
                                   uint32_t target, int32_t current,
                                   int32_t maximum, uint32_t notice,
                                   uint64_t now, uint32_t lifetime,
                                   int32_t& requestedBefore) {
    requestedBefore = 0;
    const auto found = std::find_if(receipts.begin(), receipts.end(),
        [target](const PendingBoundaryReceipt& entry) { return entry.target == target; });
    if (found == receipts.end()) return false;
    const PendingBoundaryReceipt receipt = *found;
    receipts.erase(found);
    requestedBefore = receipt.before;
    return target && current > 0 && maximum > 0 && notice >= 1 &&
           notice <= static_cast<uint32_t>(maximum) &&
           notice <= static_cast<uint32_t>(current) &&
           notice < static_cast<uint32_t>(receipt.before) &&
           now >= receipt.at && now - receipt.at <= lifetime;
}

// Boundary health synchronization may only lower a living unit to a
// server-admitted, nonzero value. A late duplicate cannot heal or revive a unit
// after other ordered combat traffic has already changed its local HP.
inline bool ShouldApplyHealth(int32_t current, int32_t maximum, uint32_t notice) {
    return current > 0 && maximum > 0 && notice >= 1 &&
           notice <= static_cast<uint32_t>(maximum) &&
           notice < static_cast<uint32_t>(current);
}

}  // namespace sdgo::map_catalog::boundary_runtime
