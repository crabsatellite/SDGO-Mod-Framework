// G改 1.0 on the client's native 强化部件 page.
//
// The factory's custom-skill page (CServiceFactoryRoomAttribute with its
// category bar and skill list, layouts uirs\1252/1253/1254, tooltip 1332,
// popups 1293/1189) is the original attribute page. Its data manager
// (CAttributeMgr) loads the categories and entries from the client tables
// lazily; in G改 1.0 mode the Mod's loaders fill it with the server's G改 1.0
// rule set instead (category bar index 0 stays hidden as natively, so the
// eight G改 1.0 types sit at 1..8), the page shows the unit's G改 1.0 loadout
// and its save and reset requests go to the Mod's lobby commands. Everything
// the player sees and clicks stays the native page; leaving the mode (or the
// tab, or the factory) restores the native data.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "original_ui.h"
#include "sdgo/oc_custom_protocol.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::oc_custom::client::g1_native {

bool Bind(const SdgoApi* api, SdgoMod* mod, void (*log)(const char* message), const ui::Bindings* bindings);
bool Bound();

// The attribute page service (CServiceFactoryRoomAttribute) when it exists.
void* FindPage();

// The server's G改 1.0 rule set (category indices from 0) and type names.
void SetRules(const std::vector<G1Rule>& rules, const std::vector<std::wstring>& categories);
bool HasRules();

// Game thread, once per factory frame. page: the attribute page service when
// the 强化部件 tab is on screen, else nullptr. g1_available: the server offers
// G改 1.0. want_g1: the player chose G改 1.0. loadout: the active unit's G改
// 1.0 loadout when known. Draws the page's system switch (G改 2.0 | G改 1.0)
// under its left panel while the tab is on screen.
void Update(void* page, bool g1_available, bool want_g1, const G1Loadout* loadout);
// A click on the attribute page: 1 the G改 2.0 button, 2 the G改 1.0 button,
// 0 not one of the Mod's controls.
int SwitchOf(void* control);
void* SwitchControl(int which);  // GameTest: the G改 2.0 (1) or G改 1.0 (2) button
// GameTest: the native services of the page: 1 category bar, 2 skill list,
// 3 save popup, 4 reset popup, 5 reset confirmation popup (nullptr when absent).
void* NativeService(int which);
bool PageReady();  // the attribute page exists and takes input now
// The bar's first page: bit n set when the category of button n (manager
// index n + 1) has an entry with the NEW flag.
std::uint32_t CategoryNewMask();
void* CategoryBarService();
void SetNotice(const std::wstring& text);
bool Active();  // the native page holds G改 1.0 data now
void Leave();   // restore the native data at once (factory exit)

// Requests the native page made while in G改 1.0 mode.
bool TakeSave(std::array<std::uint16_t, 4>* skills);
bool TakeReset();
// Results, fed back into the native page's own success and failure paths.
void SaveResult(bool ok, const std::array<std::uint16_t, 4>& skills);
void ResetResult(bool ok, std::int32_t charged);
// The G改 1.0 reset cost shown by the native reset popup.
void SetResetCost(std::int32_t cost);

struct Stats {
    std::uint32_t entered = 0;      // switches into G改 1.0
    std::uint32_t left = 0;         // switches back to the native data
    std::uint32_t loads = 0;        // manager loads served with G改 1.0 data
    std::uint32_t saves = 0;        // native save requests answered
    std::uint32_t resets = 0;       // native reset requests answered
    std::uint32_t categories = 0;   // categories in the manager now
    std::uint32_t entries = 0;      // entries in the manager now
    std::array<std::uint16_t, 4> shown{}; // skills the page shows (profile record)
};
Stats GetStats();

} // namespace sdgo::oc_custom::client::g1_native
