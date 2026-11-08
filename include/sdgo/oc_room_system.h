// Room systems for the over-custom (OC) Mods in "room" mode.
//
// One server keeps both enhancement systems for every unit: the OC six-stat
// profile and the current custom-skill parts (G改 1.0, tbl_UserUnits
// UnitSkill_1..4). In "room" mode each room plays one of them. The room title
// carries the choice through the shared room-command grammar (room_commands.h),
// so the native room records, long room titles and every title-aware Mod see
// the same rule:
//
//   !oc      OC: six-stat deltas apply, custom skills are ignored
//   (none)   current custom-skill system; OC deltas do not apply
//
// The command is read, never consumed or rewritten. A later system adds its own
// command here; an unknown command keeps the default.
#pragma once

#include <cstdint>

#include "sdgo/room_commands.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::oc_custom {

enum class RoomSystem : std::uint8_t {
    parts = 0, // current custom-skill parts (G改 1.0)
    oc = 1,    // OC six-stat enhancement
};

inline constexpr char kOcRoomCommand[] = "oc";

inline RoomSystem RoomSystemOfTitle(const SdgoApi* api, const char* title, std::uint32_t length) {
    SdgoRoomCommand command{};
    if (title && length && sdgo::room_commands::Find(api, title, length, kOcRoomCommand, 0, &command))
        return RoomSystem::oc;
    return RoomSystem::parts;
}

inline const char* RoomSystemName(RoomSystem system) {
    return system == RoomSystem::oc ? "oc" : "parts";
}

} // namespace sdgo::oc_custom
