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
// text with its system (【OC】, 【G改1】 or 【G改2】), once per native list render.
void MarkRoomList(void* service);

// After the native C3DWaitRoomTitleBar update: a label next to the title
// button shows the current room's system.
void MarkTitleBar(void* title_bar, RoomSystem system);

// Room creation windows (CServiceMakeRoom / CServiceMakeRoomAI): a 体系 row
// whose arrows step the title's system command (none, !oc, !g1).
bool BindCreate(const SdgoApi* api, SdgoMod* mod);
void SetG1Available(bool available);            // the server offers G改 1.0 rooms
void UpdateCreateWindow();                      // per frame while the lobby room list updates
bool HandleCreateEvent(void* service, std::uint32_t event, void* control); // true: the row consumed it
bool ClickCreate(const wchar_t* control);       // GameTest: a click on a row arrow
std::uint32_t CreateShown();                    // create windows that showed the row
std::uint32_t CreateToggles();                  // system changes made with the row
std::int32_t CreateSystem();                    // system of the open window's title (-1 none)
bool CreateOpen();                              // a PvP create window is open and ready

std::uint32_t ListMarks();    // room list rows marked since load
std::uint32_t TitleMarks();   // title bar labels set since load
std::int32_t TitleSystem();   // system of the label last shown (-1 none)

} // namespace sdgo::oc_custom::client::room_marks
