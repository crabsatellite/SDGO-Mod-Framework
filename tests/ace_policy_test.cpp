#include "ai_test_support.h"
#include "../mods/battle_ai/ace_policy.h"

int main() {
    using battle_ai::Difficulty;
    using namespace battle_ai::ace;

    SCENARIO("difficulty-specific defaults and probability boundaries");
    Settings settings{};
    auto easy = Select(settings, Difficulty::Easy, 4);
    CHECK(easy.ace && easy.effective == Difficulty::Medium && easy.hpPercent == 100 && easy.attackPercent == 100);
    auto medium = Select(settings, Difficulty::Medium, 4);
    CHECK(medium.ace && medium.effective == Difficulty::Hard && medium.hpPercent == 100 && medium.attackPercent == 100);
    auto hard = Select(settings, Difficulty::Hard, 4);
    CHECK(hard.ace && hard.effective == Difficulty::Hard && hard.hpPercent == 120 && hard.attackPercent == 100);
    CHECK(!Select(settings, Difficulty::Hard, 5).ace);
    settings.byDifficulty[Index(Difficulty::Easy)].chancePercent = 0;
    CHECK(!Select(settings, Difficulty::Easy, 0).ace);
    settings.byDifficulty[Index(Difficulty::Medium)].chancePercent = 100;
    CHECK(Select(settings, Difficulty::Medium, 99).ace);
    settings.enabled = false;
    CHECK(!Select(settings, Difficulty::Medium, 0).ace);

    SCENARIO("server-authored pilot identity carries only Ace and effective difficulty");
    settings.enabled = true;
    Outcome selected{};
    selected.ace = true;
    selected.base = Difficulty::Hard;
    selected.effective = Difficulty::Hard;
    selected.hpPercent = 120;
    uint32_t encoded = 0;
    CHECK(EncodePilotId(kAiPilotBit | 123u, selected, &encoded));
    CHECK((encoded & kAcePilotBit) != 0 && (encoded & kAiPilotBit) != 0);
    auto decoded = DecodePilotId(encoded, Difficulty::Hard);
    CHECK(decoded.ace && decoded.base == Difficulty::Hard && decoded.effective == Difficulty::Hard);
    Outcome ordinary{};
    ordinary.base = Difficulty::Medium;
    ordinary.effective = Difficulty::Medium;
    CHECK(EncodePilotId(kAiPilotBit | 124u, ordinary, &encoded));
    CHECK(encoded == (kAiPilotBit | 124u) && !DecodePilotId(encoded, Difficulty::Medium).ace);
    CHECK(!EncodePilotId(kAiPilotBit | kAcePilotBit | 125u, selected, &encoded));
    CHECK(!EncodePilotId(126u, selected, &encoded));

    SCENARIO("stat scaling is rounded once and saturates");
    CHECK(Scale(15000, 120) == 18000);
    CHECK(Scale(1, 150) == 2);
    CHECK(Scale(0xffffffffu, 1000) == 0xffffffffu);
    return ai_test::Finish();
}
