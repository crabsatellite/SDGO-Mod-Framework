#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sdgo::friend_unit_gift {

inline constexpr std::int32_t kBaseUnitSlots = 24;
inline constexpr std::int32_t kMaximumNativeUnitSlot = 1200;
inline constexpr std::size_t kMaximumNoteCharacters = 200;
inline constexpr std::uint8_t kGradeTierCount = 12;

struct EligibilityPolicy {
    bool enabled = false;
    std::int32_t minimum_battles = 20;
    std::int32_t minimum_player_kills = 1;
    std::uint8_t required_tiers = 12;
    std::uint16_t minimum_distinct_per_tier = 2;
};

inline constexpr EligibilityPolicy kManagerDefaults{};

enum class OfferState : std::uint8_t {
    pending = 0,
    accepted = 1,
    returning_waiting_capacity = 2,
    returned = 3,
};

enum class ResolutionKind : std::uint8_t {
    none = 0,
    recipient_return = 1,
};

enum class Operation : std::uint8_t {
    offer = 1,
    accept = 2,
    return_to_sender = 3,
    claim_returned = 4,
};

enum class Result : std::int32_t {
    success = 0,
    disabled = 1,
    invalid_argument = 2,
    account_unavailable = 3,
    not_mutual_friend = 4,
    sender_not_eligible = 5,
    unit_unavailable = 6,
    unit_ineligible = 7,
    capacity_full = 8,
    not_authorized = 9,
    wrong_state = 10,
    returning_waiting_capacity = 11,
    request_conflict = 12,
};

struct UnitInvariantSnapshot {
    std::int32_t owner_account = 0;
    std::int64_t user_unit_number = 0;
    std::int64_t active_user_unit_number = 0;
    std::int64_t assist_user_unit_number = 0;
    std::uint8_t lock_state = 0;
    std::int16_t battle_ship_count = 0;
    bool referenced_by_ship_or_leader = false;
    std::uint8_t time_limit = 0;
    std::uint8_t run_out = 0;
    std::uint8_t forbidden = 0;
    bool has_end_date = false;
};

inline bool ValidPolicy(const EligibilityPolicy& policy) noexcept {
    return policy.minimum_battles >= 0 &&
           policy.minimum_player_kills >= 0 &&
           policy.required_tiers <= kGradeTierCount &&
           policy.minimum_distinct_per_tier > 0 &&
           policy.minimum_distinct_per_tier <=
               static_cast<std::uint16_t>(kMaximumNativeUnitSlot);
}

inline bool ValidNote(std::wstring_view note) noexcept {
    if (note.size() > kMaximumNoteCharacters) return false;
    for (const wchar_t character : note) {
        if (character == L'\0') return false;
    }
    return true;
}

inline bool EligibleSender(const EligibilityPolicy& policy,
                           std::int64_t battles,
                           std::int64_t playerKills,
                           std::int64_t tiersMet) noexcept {
    return ValidPolicy(policy) &&
           battles >= policy.minimum_battles &&
           playerKills >= policy.minimum_player_kills &&
           tiersMet >= policy.required_tiers;
}

inline bool EligibleUnit(const UnitInvariantSnapshot& unit,
                         std::int32_t authenticatedSender) noexcept {
    return authenticatedSender > 0 &&
           unit.owner_account == authenticatedSender &&
           unit.user_unit_number > 0 &&
           unit.user_unit_number != unit.active_user_unit_number &&
           unit.user_unit_number != unit.assist_user_unit_number &&
           unit.lock_state == 0 &&
           unit.battle_ship_count == 0 &&
           !unit.referenced_by_ship_or_leader &&
           unit.time_limit == 0 &&
           unit.run_out == 0 &&
           unit.forbidden == 0 &&
           !unit.has_end_date;
}

inline bool CanCreateOffer(const EligibilityPolicy& policy) noexcept {
    return ValidPolicy(policy) && policy.enabled;
}

inline bool ResolutionAvailable(bool schemaExists,
                                Operation operation) noexcept {
    if (!schemaExists) return false;
    return operation == Operation::accept ||
           operation == Operation::return_to_sender ||
           operation == Operation::claim_returned;
}

inline bool ValidCapacity(std::int32_t expandSlotCount) noexcept {
    return expandSlotCount >= 0 &&
           expandSlotCount <= kMaximumNativeUnitSlot - kBaseUnitSlots;
}

inline std::int32_t NativeCapacity(std::int32_t expandSlotCount) noexcept {
    return ValidCapacity(expandSlotCount)
        ? kBaseUnitSlots + expandSlotCount
        : 0;
}

} // namespace sdgo::friend_unit_gift
