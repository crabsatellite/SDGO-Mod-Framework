#pragma once
#include "sdgo/human_unit_capture.h"

namespace sdgo::unit_forms {
struct NativeTransformContext {
    const void* room = nullptr;
    const void* source = nullptr;
    const void* player = nullptr;
    const void* slot = nullptr;
    std::uint32_t owner = 0, root = 0, deck = 0, mode = 0, active = 0;
    std::uint32_t current = 0, target = 0, hp = 0;
    std::uint64_t serial = 0;
    const void* definitions[2]{};
    std::uint32_t forms[2]{}, targets[2]{};
};

// Supported original x86 host's paired-bank profile only. Call after a
// successful constructor and the original sender, room, alive and BOTH
// non-null definition checks, while its original room critical section is
// held. A single-bank actor is deliberately outside this boundary's profile.
// This bounded read grants no authorization, performs no native calls and
// changes no game data. Failure leaves output unchanged. All returned pointers
// are borrowed for that same locked invocation; do not retain and dereference
// them later. Non-null pointers and readable IDs do not prove registry freshness.
bool ReadNativeTransformContext(const void* room, std::uint32_t human_slot,
                                const void* expected_slot, std::uint32_t target,
                                NativeTransformContext& output) noexcept;

// Identity comparison only. A caller must also recheck capture.Current under
// the same native room lock, graph.resolveTarget, the current registry records,
// and skill/weapon/respawn state before any mutation. Captured root definitions
// need not remain in one of the two active native cache positions.
bool MatchesCapture(const NativeTransformContext&, const HumanUnitCaptureSnapshot&) noexcept;
} // namespace sdgo::unit_forms
