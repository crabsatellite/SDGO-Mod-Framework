// Room-system marks for "room" mode: the PvP room list shows each room's
// enhancement system in front of its title, and the waiting room's title bar
// gets a label with the room's system. The marks are the Mod's own text drawn
// with the client's native controls; the room title itself is not changed.
#pragma once

#include <cstdint>

#include "sdgo/oc_room_system.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::oc_custom::client::room_marks {

bool Bind(const SdgoApi* api, SdgoMod* mod, void (*log)(const char* message));

// After the native CServiceRoomList update: prefix every listed room's title
// text with its system (【OC】 or 【G改】), once per native list render.
void MarkRoomList(void* service);

// After the native C3DWaitRoomTitleBar update: a label next to the title
// button shows the current room's system.
void MarkTitleBar(void* title_bar, RoomSystem system);

std::uint32_t ListMarks();    // room list rows marked since load
std::uint32_t TitleMarks();   // title bar labels set since load
std::int32_t TitleSystem();   // system of the label last shown (-1 none)

} // namespace sdgo::oc_custom::client::room_marks
