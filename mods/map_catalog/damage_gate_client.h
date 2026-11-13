#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sdgo::map_catalog::damage_gate_client {

enum class NoticeRoute {
    Native,
    Protected,
    Boundary,
};

// Protected-target and boundary receipts share one marked pilot carrier. The
// native room broadcast overwrites packet +2 with each recipient pilot, so that
// field cannot classify the producer. Resolve the marked target against the
// active mission/phase declarations instead. A live exact native identity still
// owns the native namespace, and an active damaging boundary takes precedence.
inline NoticeRoute RouteNotice(uint32_t wirePilot, uint32_t exactNativePilot,
                               uint32_t marker, bool activeBoundaryTarget,
                               bool activeProtectedTarget) {
    if (!wirePilot || (exactNativePilot && exactNativePilot == wirePilot))
        return NoticeRoute::Native;
    if ((wirePilot & marker) == 0 || (wirePilot ^ marker) == 0)
        return NoticeRoute::Native;
    if (activeBoundaryTarget) return NoticeRoute::Boundary;
    if (activeProtectedTarget) return NoticeRoute::Protected;
    return NoticeRoute::Native;
}

inline uint32_t NoticeTarget(uint32_t wirePilot, uint32_t marker) {
    return (wirePilot & marker) && (wirePilot ^ marker) ? wirePilot ^ marker : 0;
}

inline bool IsLocalWarning(uint32_t recipient, uint32_t localPilot) {
    return recipient != 0 && recipient == localPilot;
}

struct GuardObservation {
    uint32_t pilot = 0;
    bool aliveObserved = false;
    bool defeated = false;
};
struct TargetHealthObservation {
    uint32_t pilot = 0;
    int32_t health = 0;
};

// Absence is deliberately not a defeat.  A guard must first be observed alive,
// then the same native entity must be observed with exact zero HP.
inline bool ObserveGuard(std::vector<GuardObservation>& observations,
                         uint32_t pilot, int32_t health) {
    if (!pilot || health < 0) return false;
    const auto found = std::find_if(observations.begin(), observations.end(),
        [pilot](const GuardObservation& entry) { return entry.pilot == pilot; });
    if (health > 0) {
        if (found == observations.end()) observations.push_back({pilot, true, false});
        else found->aliveObserved = true;
        return false;
    }
    if (found == observations.end() || !found->aliveObserved || found->defeated) return false;
    found->defeated = true;
    return true;
}

inline bool ObserveAcceptedTargetDamage(
    std::vector<TargetHealthObservation>& observations, uint32_t pilot,
    int32_t health, bool guardWasObserved, bool guardAliveNow) {
    if (!pilot || health < 0) return false;
    const auto found = std::find_if(observations.begin(), observations.end(),
        [pilot](const TargetHealthObservation& entry) { return entry.pilot == pilot; });
    if (found == observations.end()) {
        observations.push_back({pilot, health});
        return false;
    }
    const bool decreased = health < found->health;
    found->health = health;
    return guardWasObserved && !guardAliveNow && decreased;
}
inline uint32_t DefeatCount(const std::vector<GuardObservation>& observations) {
    return static_cast<uint32_t>(std::count_if(observations.begin(), observations.end(),
        [](const GuardObservation& entry) { return entry.defeated; }));
}

inline bool Locked(uint32_t requiredDefeats,
                   const std::vector<GuardObservation>& observations) {
    return requiredDefeats != 0 && DefeatCount(observations) < requiredDefeats;
}

}  // namespace sdgo::map_catalog::damage_gate_client
