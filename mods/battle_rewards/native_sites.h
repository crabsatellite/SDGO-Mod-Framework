// Verified post-bonus boundaries in BattleServer f74d1603...7b63a4.
// These sites are six bytes after daily_rewards' boundaries, so the two mods
// never own the same instruction. Mission pilot EXP and G have a second final
// phase after the native Win/Lose operator additions.
#pragma once
#include <array>
#include <cstddef>

namespace sdgo::battle_rewards {
struct SettlementSite {
    const char* symbol;
    ptrdiff_t pilot_exp;
    ptrdiff_t g_amount;
    ptrdiff_t unit_exp;
    ptrdiff_t pilot_event;
    ptrdiff_t g_event;
    ptrdiff_t unit_event;
    ptrdiff_t g_display_base;
    std::array<ptrdiff_t, 3> g_display_bonuses;
};
// Sources of result packet +18 (total), +1E (base), +24/+2A/+30 (legacy
// bonuses), and +36 (event) in the supported BattleServer. The native client's
// normal animation uses base + bonuses, rather than the total field used by
// wallet delivery and the skip-animation path.
inline constexpr std::array<SettlementSite, 10> kSettlementSites = {{
    {"battle.rewards.augment.personal", -0x770, -0x28, -0x89C, -0x18, -0x87C, -0x844, -0x758, {{-0x10, -0x76C, -0x744}}},
    {"battle.rewards.augment.team", -0x758, -0x20, -0x904, -0x10, -0x834, -0x82C, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.leader", -0x758, -0x20, -0x904, -0x10, -0x834, -0x82C, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.ai", -0x760, -0x24, -0x908, -0x14, -0x83C, -0x834, -0x74C, {{-0x08, -0x75C, -0x73C}}},
    {"battle.rewards.augment.ai_ship", -0x764, -0x24, -0x918, -0x14, -0x844, -0x83C, -0x74C, {{-0x08, -0x760, -0x73C}}},
    {"battle.rewards.augment.mode13", -0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834, -0x74C, {{-0x08, -0x75C, -0x73C}}},
    {"battle.rewards.augment.mode14", -0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834, -0x74C, {{-0x08, -0x75C, -0x73C}}},
    {"battle.rewards.augment.tag", -0x758, -0x20, -0x904, -0x10, -0x834, -0x82C, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.death_grid", -0x758, -0x20, -0x90C, -0x10, -0x834, -0x82C, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.mission", -0x760, -0x20, -0x860, -0x10, -0x840, -0x834, -0x74C, {{-0x08, -0x75C, -0x73C}}},
}};
}
