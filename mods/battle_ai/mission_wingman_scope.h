#pragma once

#include <cstdint>

#include "sdgo/mission_ai_client_observer.h"

namespace battle_ai::mission {

inline bool AdmitsWingman(
    const SdgoMissionAiClientSnapshotV1& snapshot,
    std::uint32_t modeObject,
    std::uint32_t unitManagerObject,
    std::uint16_t roomMode,
    std::uint32_t objectId,
    std::uint32_t pilotId) noexcept
{
    if (!snapshot.active ||
        !snapshot.exact_plain_mission ||
        snapshot.mode_class !=
            SDGO_MISSION_AI_MODE_CLASS_PLAIN_MISSION ||
        !snapshot.local_is_host ||
        snapshot.local_is_observer ||
        !snapshot.network_game ||
        !snapshot.room_records_valid ||
        !snapshot.find_object_available ||
        snapshot.room_mode != roomMode ||
        snapshot.mode_object != modeObject ||
        snapshot.unit_manager_object != unitManagerObject ||
        !objectId || !(pilotId & 0x80000000u)) {
        return false;
    }
    for (std::uint32_t index = 0;
         index < snapshot.ai_record_count && index < 12;
         ++index) {
        const auto& participant =
            snapshot.participants[index];
        if (participant.object_id == objectId &&
            participant.pilot_id == pilotId &&
            participant.unit_server_id == pilotId &&
            participant.spawned &&
            participant.valid_registered_object &&
            participant.mapping_valid &&
            participant.local_control &&
            !participant.remote_copy &&
            participant.master_npc &&
            participant.ai_enabled) {
            return true;
        }
    }
    return false;
}

} // namespace battle_ai::mission
