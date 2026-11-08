// Room systems for the over-custom (OC) Mods in "room" mode.
//
// One server keeps several enhancement systems for every unit: the current
// custom-skill attributes (G改 2.0, tbl_UserUnits UnitSkill_1..4), the OC
// six-stat profile and, when the server owner installed a G改 1.0 rule set, a
// separate G改 1.0 attribute loadout. In "room" mode each room plays one of
// them. The room title carries the choice through the shared room-command
// grammar (room_commands.h), so the native room records, long room titles and
// every title-aware Mod see the same rule:
//
//   !oc      OC: six-stat deltas apply, custom skills are ignored
//   !g1      G改 1.0: the unit's G改 1.0 loadout replaces its custom skills
//   !g2      G改 2.0 named explicitly (the same as no command)
//   (none)   G改 2.0, the current custom-skill system; OC deltas do not apply
//
// The commands are read, never consumed or rewritten. When a title carries
// both, !oc wins; an unknown command keeps the default.
#pragma once

#include <cstdint>

#include "sdgo/room_commands.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::oc_custom {

enum class RoomSystem : std::uint8_t {
    parts = 0, // G改 2.0: the current custom-skill attributes
    oc = 1,    // OC six-stat enhancement
    g1 = 2,    // G改 1.0 attribute loadout
};

inline constexpr char kOcRoomCommand[] = "oc";
// Command names are letters; the generation is the command's count: !g1 is
// G改 1.0, !g2 names the default G改 2.0 explicitly.
inline constexpr char kGenerationRoomCommand[] = "g";

inline RoomSystem RoomSystemOfTitle(const SdgoApi* api, const char* title, std::uint32_t length) {
    SdgoRoomCommand command{};
    if (title && length && sdgo::room_commands::Find(api, title, length, kOcRoomCommand, 0, &command))
        return RoomSystem::oc;
    if (title && length &&
        sdgo::room_commands::Find(api, title, length, kGenerationRoomCommand, SDGO_ROOM_COMMAND_ALLOW_COUNT, &command) &&
        command.has_count && command.count == 1)
        return RoomSystem::g1;
    return RoomSystem::parts;
}

// Log names.
inline const char* RoomSystemName(RoomSystem system) {
    return system == RoomSystem::oc ? "oc" : system == RoomSystem::g1 ? "g1" : "g2";
}

// Display names.
inline const wchar_t* RoomSystemLabel(RoomSystem system) {
    return system == RoomSystem::oc ? L"OC" : system == RoomSystem::g1 ? L"G改 1.0" : L"G改 2.0";
}

} // namespace sdgo::oc_custom
