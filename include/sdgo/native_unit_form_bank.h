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

} // namespace sdgo::unit_forms
