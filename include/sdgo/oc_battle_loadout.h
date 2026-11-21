#pragma once

#include "sdgo/oc_custom_protocol.h"
#include "sdgo/oc_room_system.h"

namespace sdgo::oc_custom {
// Choose a loadout before either native side builds its parts-effect cache.
// G2/unknown rooms leave the destination untouched; the owned record is read-only.
inline bool SelectBattleSkillIds(const std::uint8_t* record, bool room_known,
                                 RoomSystem system, std::array<std::uint16_t, 4>* skills) noexcept {
    Carrier carrier{};
    if (!skills || !record || !ReadCarrier(record, &carrier)) return false;
    if (carrier.flags & carrier_oc_mode) system = RoomSystem::oc;
    else if (!(carrier.flags & carrier_room_rule) || !room_known) return false;
    if (system == RoomSystem::parts) return false;
    *skills = system == RoomSystem::g1 && (carrier.flags & carrier_g1_profile)
        ? carrier.g1_skills : std::array<std::uint16_t, 4>{};
    return true;
}

// The client copy ends with four {u16 skill, u8 active} entries.
inline bool SelectBattleLoadout(std::uint8_t* block, const std::uint8_t* record,
                                bool room_known, RoomSystem system) noexcept {
    std::array<std::uint16_t, 4> skills{};
    if (!block || !SelectBattleSkillIds(record, room_known, system, &skills)) return false;
    for (std::size_t slot = 0; slot < kCustomSkillCount; ++slot) {
        const std::uint16_t skill = skills[slot];
        std::memcpy(block + 0x26 + slot * 3, &skill, sizeof(skill));
        block[0x28 + slot * 3] = skill ? 1 : 0;
    }
    return true;
}
} // namespace sdgo::oc_custom
