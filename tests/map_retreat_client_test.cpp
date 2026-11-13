#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#include <vector>

#include "mods/map_catalog/retreat_client.h"

int main() {
    using namespace sdgo::map_catalog::retreat_client;
    using sdgo::mission_retreat::Entry;

    const std::vector<Entry> policies{
        {70001, 5, 91001},
        {70001, 6, 91002},
        {70002, 1, 91001},
    };

    assert(!IsAuthoritativeCleanup(0x0570, 1));
    assert(!IsAuthoritativeCleanup(kAuthoritativeCleanupOpcode, 11));
    assert(IsAuthoritativeCleanup(kAuthoritativeCleanupOpcode, 0));
    assert(IsAuthoritativeCleanup(kAuthoritativeCleanupOpcode, 10));

    std::uint32_t unitA = 0;
    std::uint32_t unitB = 0;
    Candidate eligible{0x80000011u, 91002, &unitA, 150};
    assert(Eligible(policies, 70001, 6, eligible));
    assert(!Eligible(policies, 70001, 5, eligible));
    assert(!Eligible(policies, 70002, 6, eligible));
    Candidate zeroHealth = eligible;
    zeroHealth.health = 0;
    assert(!Eligible(policies, 70001, 6, zeroHealth));
    Candidate unrelatedNpc = eligible;
    unrelatedNpc.npcId = 99999;
    assert(!Eligible(policies, 70001, 6, unrelatedNpc));
    Candidate unresolved = eligible;
    unresolved.unit = nullptr;
    assert(!Eligible(policies, 70001, 6, unresolved));

    DispatchContext context{};
    assert(Add(context, eligible));
    assert(context.count == 1);
    assert(!context.units[0].hasPosition);
    assert(!Add(context, eligible));
    Candidate sameUnit{0x80000012u, 91002, &unitA, 90};
    assert(!Add(context, sameUnit));
    Candidate second{0x80000012u, 91002, &unitB, 90};
    second.position = {1.0f, 2.0f, 3.0f, 0.5f};
    second.hasPosition = true;
    assert(Add(context, second));
    assert(context.count == 2);
    assert(context.units[1].hasPosition);

    std::uint16_t removalA = kNativeRemovalMessage;
    std::uint16_t removalB = kNativeRemovalMessage;
    std::uint16_t ordinary = 0x1C;
    assert(!ArmPresentation(nullptr, &unitA, &removalA, removalA));
    assert(!ArmPresentation(&context, &unitA, &ordinary, ordinary));
    assert(!ArmPresentation(&context, &unitA, &removalA, 1));
    assert(!ArmPresentation(&context, reinterpret_cast<void*>(1), &removalA, removalA));
    assert(ArmPresentation(&context, &unitA, &removalA, removalA));
    assert(ArmPresentation(&context, &unitA, &removalA, removalA));
    assert(!ArmPresentation(&context, &unitA, &removalB, removalB));
    assert(ArmPresentation(&context, &unitB, &removalB, removalB));

    DispatchContext nested{};
    assert(Add(nested, eligible));
    assert(ArmPresentation(&nested, &unitA, &removalB, removalB));
    assert(ArmPresentation(&context, &unitA, &removalA, removalA));
    return 0;
}
