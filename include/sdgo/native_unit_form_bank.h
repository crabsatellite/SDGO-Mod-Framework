#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace sdgo::unit_forms {

// Layout of the supported x86 native server. These borrowed pointers and
// weapon states belong to one process/catalog lifetime; never serialize them
// into a content project or reuse them after a room/player is reconstructed.
constexpr std::size_t kSlotBytes = 0x658;
constexpr std::size_t kBankBytes = 0x2f0;
constexpr std::size_t kStatsBytes = 0x0c;
constexpr std::size_t kBankOffset = 0x64;
constexpr std::size_t kStatsOffset = 0x0e;
constexpr std::size_t kActiveBankOffset = 0x60;
using Slot = std::array<std::uint8_t, kSlotBytes>;

inline std::uint32_t Read32(const std::uint8_t* value) {
    std::uint32_t result = 0;
    std::memcpy(&result, value, sizeof(result));
    return result;
}

struct Bank {
    std::array<std::uint8_t, kBankBytes> bytes{};
    std::array<std::uint8_t, kStatsBytes> stats{};
    std::uint32_t id() const { return Read32(bytes.data()); }
    bool has_definition() const { return Read32(bytes.data() + 8) != 0; }
};

inline bool Capture(const Slot& slot, unsigned bank, Bank& result) {
    if (bank > 1) return false;
    Bank captured;
    std::memcpy(captured.bytes.data(), slot.data() + kBankOffset + bank * kBankBytes, kBankBytes);
    std::memcpy(captured.stats.data(), slot.data() + kStatsOffset + bank * kStatsBytes, kStatsBytes);
    result = captured;
    return true;
}

enum class StageResult { invalid, already_cached, prepared };

// Prepare a separate slot image only. This grants no graph/owner authority,
// takes no room lock and never commits to a live actor. A native caller must
// separately validate the directed edge, generation, definitions and lifecycle
// before entering its existing authorized/locked native transform flow.
// Replacing a live active bank would reset ammo/cooldowns, so it is refused.
// already_cached preserves the existing inactive weapon state. Its immutable
// header, weapon pointers and complete stat slice must match the replacement;
// caller-owned generation/lifecycle checks remain required.
inline StageResult StageInactive(const Slot& original, std::uint32_t inventoryRoot,
                                 std::uint32_t currentForm, const Bank& replacement,
                                 Slot& prepared) {
    const auto validId = [](std::uint32_t id) { return id >= 10001 && id <= 30000; };
    const auto active = Read32(original.data() + kActiveBankOffset);
    if (&original == &prepared || !validId(inventoryRoot) || !validId(currentForm) || active > 1 ||
        Read32(original.data()) != inventoryRoot || !validId(replacement.id()) ||
        !replacement.has_definition() || replacement.id() == currentForm)
        return StageResult::invalid;
    Bank current, inactive;
    Capture(original, active, current);
    Capture(original, active ^ 1, inactive);
    if (current.id() != currentForm || !current.has_definition()) return StageResult::invalid;
    if (inactive.id() == replacement.id()) {
        if (std::memcmp(inactive.bytes.data(), replacement.bytes.data(), 12) != 0 ||
            inactive.stats != replacement.stats) return StageResult::invalid;
        for (unsigned weapon = 0; weapon < 5; ++weapon)
            if (Read32(inactive.bytes.data() + 12 + weapon * 0x94) !=
                Read32(replacement.bytes.data() + 12 + weapon * 0x94)) return StageResult::invalid;
        return StageResult::already_cached;
    }
    Slot staged = original;
    const auto destination = active ^ 1;
    std::memcpy(staged.data() + kBankOffset + destination * kBankBytes, replacement.bytes.data(), kBankBytes);
    std::memcpy(staged.data() + kStatsOffset + destination * kStatsBytes, replacement.stats.data(), kStatsBytes);
    prepared = staged;
    return StageResult::prepared;
}

// A narrower staging option for forms whose freshly composed complete banks
// and stat slices are byte-identical except for unit ID/definition pointer.
// Retain every live runtime byte (including all five weapon records, SP and
// effect-modified stats); change only the inactive identity and definition.
// Fresh compositions must come from the same captured seed/current registries.
// This does NOT prove unit/skill semantic equivalence, authorize an edge or
// retire a live form. The native caller still owns those proofs, the original
// room lock, lifecycle checks, notification and death/deck restoration.
// Only prepared fills the output; invalid/already_cached leave it unchanged.
inline StageResult StageEquivalentInactive(const Slot& original, std::uint32_t inventoryRoot,
                                           std::uint32_t currentForm, const Bank& initialSource,
                                           const Bank& initialTarget, Slot& prepared) {
    const auto validId=[](std::uint32_t id){return id>=10001&&id<=30000;};
    const auto active=Read32(original.data()+kActiveBankOffset);
    if(&original==&prepared||!validId(inventoryRoot)||!validId(currentForm)||active>1||
       Read32(original.data())!=inventoryRoot||!validId(initialSource.id())||!validId(initialTarget.id())||
       initialSource.id()==initialTarget.id()||initialTarget.id()==currentForm||
       !initialSource.has_definition()||!initialTarget.has_definition()||
       Read32(initialSource.bytes.data()+8)==Read32(initialTarget.bytes.data()+8)||
       initialSource.stats!=initialTarget.stats)return StageResult::invalid;
    // Compare complete fresh banks, including all five complete weapon states.
    for(std::size_t i=4;i<kBankBytes;++i)
        if(!(i>=8&&i<12)&&initialSource.bytes[i]!=initialTarget.bytes[i])return StageResult::invalid;
    Bank current,inactive;Capture(original,active,current);Capture(original,active^1,inactive);
    if(current.id()!=currentForm||!current.has_definition())return StageResult::invalid;
    const bool cached=inactive.id()==initialTarget.id();
    const auto& expected=cached?initialTarget:initialSource;
    if(inactive.id()!=expected.id()||Read32(inactive.bytes.data()+8)!=Read32(expected.bytes.data()+8))
        return StageResult::invalid;
    for(unsigned weapon=0;weapon<5;++weapon)
        if(Read32(inactive.bytes.data()+12+weapon*0x94)!=Read32(expected.bytes.data()+12+weapon*0x94))
            return StageResult::invalid;
    if(cached)return StageResult::already_cached;
    Slot staged=original;const auto at=kBankOffset+(active^1)*kBankBytes;
    std::memcpy(staged.data()+at,initialTarget.bytes.data(),4);
    std::memcpy(staged.data()+at+8,initialTarget.bytes.data()+8,4);
    prepared=staged;return StageResult::prepared;
}

} // namespace sdgo::unit_forms
