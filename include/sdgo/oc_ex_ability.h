#pragma once

#include "sdgo/oc_custom_protocol.h"

namespace sdgo::oc_custom {

// The EX attack and defense IDs are part of the original OC battle contract.
enum class ExAbility : std::uint8_t {
    none = 0, attack = 1, defense = 2, mobility = 3, boost = 4,
    movement = 5, quickstep = 6, lock_on = 7, boost_recovery = 8,
};
inline constexpr std::uint8_t kExAbilityCount = 8;
inline constexpr std::uint8_t kExLevel = 15;

// Native mode 4 suppresses OC EX. The caller resolves the room's system before
// using this result; G1/G2 rooms keep their own attributes and never use EX.
inline std::uint8_t ActiveExAbility(const Carrier& carrier, bool oc_room,
                                   std::uint8_t native_mode) noexcept {
    if (!(carrier.flags & carrier_profile) || carrier.oc_level != kExLevel ||
        carrier.extra_bonus > kExAbilityCount || native_mode == 4 ||
        !((carrier.flags & carrier_oc_mode) || ((carrier.flags & carrier_room_rule) && oc_room)))
        return 0;
    return carrier.extra_bonus;
}

inline std::int32_t ExBaseStatGain(std::uint8_t ability, Stat stat, std::uint16_t base) noexcept {
    if (!((ability == static_cast<std::uint8_t>(ExAbility::attack) && stat == stat_attack) ||
          (ability == static_cast<std::uint8_t>(ExAbility::defense) && stat == stat_defense)))
        return 0;
    // Original x87 fild / fmul(float32 0x3CF5C28F) / __ftol sequence. Keep the
    // float32 coefficient widened to double: integer 3/100 or 0.03 changes
    // boundaries such as a base value of 100 (the original grants 2, not 3).
    return static_cast<std::int32_t>(static_cast<double>(base) * static_cast<double>(0.03f));
}

inline std::uint16_t ExEnhancedStat(std::uint16_t native, std::int16_t allocated,
                                    std::uint16_t base, std::uint8_t ability,
                                    Stat stat, std::int32_t cap) noexcept {
    std::int64_t value = static_cast<std::int64_t>(native) + allocated + ExBaseStatGain(ability, stat, base);
    if (cap > 0 && value > cap) value = cap;
    if (value < 0) value = 0;
    if (value > 65535) value = 65535;
    return static_cast<std::uint16_t>(value);
}

} // namespace sdgo::oc_custom
