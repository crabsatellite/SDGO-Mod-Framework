#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "mods/map_catalog/damage_gate_client.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    using namespace sdgo::map_catalog::damage_gate_client;
    constexpr uint32_t marker = 0x20000000u;
    constexpr uint32_t target = 0x80000042u;
    const uint32_t marked = target | marker;

    Check(RouteNotice(marked, 0, marker, true, false) == NoticeRoute::Boundary &&
          RouteNotice(marked, 0, marker, true, true) == NoticeRoute::Boundary,
          "the active damaging boundary owns its marked target regardless of carrier +2");
    Check(RouteNotice(marked, 0, marker, false, true) == NoticeRoute::Protected,
          "an active protected target retains the generic damage-gate route");
    Check(RouteNotice(marked, marked, marker, true, true) == NoticeRoute::Native,
          "an exact marked native pilot keeps native ownership");
    Check(RouteNotice(marked, 0, marker, false, false) == NoticeRoute::Native &&
          RouteNotice(target, target, marker, true, true) == NoticeRoute::Native &&
          RouteNotice(target, 0, marker, true, true) == NoticeRoute::Native,
          "unowned marked packets and unmarked packets remain native");
    Check(NoticeTarget(marked, marker) == target && NoticeTarget(marker, marker) == 0,
          "only a complete marked identity decodes");
    Check(IsLocalWarning(17, 17) && !IsLocalWarning(18, 17) && !IsLocalWarning(0, 0),
          "warning ownership follows the exact local recipient");

    std::vector<GuardObservation> guards;
    Check(!ObserveGuard(guards, 101, 0) && Locked(1, guards),
          "a dead-only or absent guard cannot unlock the target");
    Check(!ObserveGuard(guards, 101, 500) && Locked(1, guards),
          "observing a living guard keeps the target locked");
    Check(ObserveGuard(guards, 101, 0) && !Locked(1, guards),
          "the same observed living guard unlocks only after exact zero HP");
    Check(!ObserveGuard(guards, 101, 0) && DefeatCount(guards) == 1,
          "duplicate zero-HP observations do not double count");
    Check(!ObserveGuard(guards, 102, 300) && Locked(2, guards) &&
          ObserveGuard(guards, 102, 0) && !Locked(2, guards),
          "multiple required defeats use distinct observed entities");
    std::vector<TargetHealthObservation> targetHealth;
    Check(!ObserveAcceptedTargetDamage(targetHealth, target, 50000, true, true) &&
          !ObserveAcceptedTargetDamage(targetHealth, target, 49900, true, true),
          "ordinary damage while a guard is still alive cannot unlock visuals");
    Check(!ObserveAcceptedTargetDamage(targetHealth, target, 49900, true, false),
          "guard absence without accepted target damage remains locked");
    Check(ObserveAcceptedTargetDamage(targetHealth, target, 49800, true, false),
          "a native target HP decrease confirms server unlock after the observed guard disappears");
    std::vector<TargetHealthObservation> unseenGuardTarget;
    Check(!ObserveAcceptedTargetDamage(unseenGuardTarget, target, 50000, false, false) &&
          !ObserveAcceptedTargetDamage(unseenGuardTarget, target, 49000, false, false),
          "target damage cannot replace the requirement to have observed the guard alive");

    std::cout << "protected notices stay separate from HP receipts and guard absence\n";
    return 0;
}
