#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace sdgo::unit_forms {
// The supported battle table copies this payload into a 0x7c allocation.
// Padding and intrusive list links at +0x74/+0x78 are not definition bytes.
constexpr std::size_t kBattleDefinitionBytes=0x72;
using BattleDefinition=std::array<std::uint8_t,kBattleDefinitionBytes>;
enum class DefinitionMatch { different, exact, native_default_normalized };
inline std::uint32_t DefinitionWord(const BattleDefinition& definition,std::size_t offset) {
    std::uint32_t value;std::memcpy(&value,definition.data()+offset,4);return value;
}
// Original 4D8320 changes an out-of-range MasteredWeaponIX to 1. Accept only
// that precise native mutation; every other byte must still equal installed
// host expectations. This compares data only, not registry pointer freshness,
// owner/edge permission or actor lifecycle.
inline DefinitionMatch MatchBattleDefinition(const BattleDefinition& expected,const BattleDefinition& actual) {
    if(expected==actual)return DefinitionMatch::exact;
    const auto before=DefinitionWord(expected,0x45),after=DefinitionWord(actual,0x45);
    if((before>=1&&before<=3)||after!=1)return DefinitionMatch::different;
    for(std::size_t i=0;i<expected.size();++i)
        if(!(i>=0x45&&i<0x49)&&expected[i]!=actual[i])return DefinitionMatch::different;
    return DefinitionMatch::native_default_normalized;
}
} // namespace sdgo::unit_forms
