#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace sdgo::oc_custom {
struct BattleAttributes {
    std::array<std::uint16_t, 4> skills{};
    std::uint32_t unit = 0, weapon = 0;
    float medium_damage = 0, medium_attack_speed = 0, medium_reload = 0;
    std::int32_t medium_range = 0;
    std::int32_t maximum_hp = 0;
    std::int16_t medium_cooldown = 0, dash = 0, motive = 0;
};
inline bool SameBattleAttributes(const BattleAttributes& actual, const BattleAttributes& expected) noexcept {
    const auto close = [](float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) < 0.0001f; };
    return actual.skills == expected.skills && actual.unit == expected.unit && actual.weapon == expected.weapon &&
        close(actual.medium_damage, expected.medium_damage) && close(actual.medium_attack_speed, expected.medium_attack_speed) &&
        close(actual.medium_reload, expected.medium_reload) && actual.medium_range == expected.medium_range &&
        actual.maximum_hp == expected.maximum_hp && actual.medium_cooldown == expected.medium_cooldown &&
        actual.dash == expected.dash && actual.motive == expected.motive;
}
} // namespace sdgo::oc_custom
