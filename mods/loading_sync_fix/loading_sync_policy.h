#pragma once

#include <cstdint>

namespace loading_sync_fix
{
    constexpr std::uint32_t kAiPilotBit = 0x80000000u;

    constexpr bool IsLoadingParticipant(std::uint32_t pilotId)
    {
        return pilotId != 0 && (pilotId & kAiPilotBit) == 0;
    }
}
