#include "ai_test_support.h"
#include "../mods/battle_ai/ace_profile_registry.h"

int main() {
    using battle_ai::ace::UnitProfileRegistry;
    int unitA = 0;
    int unitB = 0;
    UnitProfileRegistry registry;

    SCENARIO("queued native Init then endpoint then first Think preserves the endpoint profile");
    registry.BeginBattle(1);
    auto queued = registry.NoteNativeInit(&unitA);
    CHECK(registry.Apply(&unitA, 2, 1));
    CHECK(registry.Reset(queued));
    CHECK(registry.Get(&unitA, 0) == 2 && registry.Size() == 1);

    SCENARIO("a later native Init clears the old profile until a new endpoint restores it");
    auto later = registry.NoteNativeInit(&unitA);
    CHECK(!registry.Reset(later));
    CHECK(registry.Get(&unitA, 1) == 1 && registry.Size() == 0);
    CHECK(registry.Apply(&unitA, 0, 2));
    CHECK(registry.Get(&unitA, 1) == 0);
    CHECK(!registry.Apply(&unitA, 2, 1));
    CHECK(registry.Get(&unitA, 1) == 0);

    SCENARIO("new battle cleanup rejects stale queued Init and pointer reuse");
    auto stale = registry.NoteNativeInit(&unitB);
    registry.BeginBattle(2);
    CHECK(registry.BattleGeneration() == 2 && registry.Size() == 0);
    auto current = registry.NoteNativeInit(&unitB);
    CHECK(registry.Apply(&unitB, 2, 3));
    bool currentBattle = true;
    CHECK(!registry.Reset(stale, &currentBattle));
    CHECK(!currentBattle);
    CHECK(registry.Get(&unitB, 0) == 2);
    currentBattle = false;
    CHECK(registry.Reset(current, &currentBattle));
    CHECK(currentBattle && registry.Get(&unitB, 0) == 2);
    registry.BeginBattle(3);
    CHECK(registry.Get(&unitB, 1) == 1 && registry.Size() == 0);
    const auto currentWithoutOverride = registry.NoteNativeInit(&unitB);
    currentBattle = false;
    CHECK(!registry.Reset(currentWithoutOverride, &currentBattle));
    CHECK(currentBattle);
    return ai_test::Finish();
}
