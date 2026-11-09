// Chinese for the English left on the factory pages the room systems use (the
// 终极强化 OC page, the 强化部件 page in G改 2.0 and G改 1.0, the factory
// header). Text the client formats from its text table is translated in the
// table; English baked into sprites is covered with the client's own
// text-free art (the header's twin sprite, flat panel sprites) and a native
// text control on top, ordered with the controls' z values. No game resource
// is replaced or redistributed.
#pragma once

#include <cstdint>

#include "original_ui.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::oc_custom::client::l10n {

bool Bind(const SdgoApi* api, SdgoMod* mod, const ui::Bindings* bindings, void (*log)(const char* message),
          std::uint8_t** grid);

// Once the client's text table is loaded: "%s RANK" (the unit's grade line)
// becomes "%s级" and "%dLv" (custom-skill level tags) "%d级".
void PatchTexts();

// Factory header (uirs\0804), every header frame: the text-free twin of its
// art (sprs\13875) under the page title and 整备室 in place of FACTORY.
void Header(void* title_control);

// The OC tab's left page (uirs\0800 as the Mod loads it), after each render:
// 机体信息, 终极强化等级 and 道具 over the art's English labels.
void InfoPage(void* dialog, bool shown);

// Category bar (uirs\1254), every page frame: 新 instead of the NEW badges.
// new_mask: bit n set when button n's category has a NEW entry.
void CategoryBar(void* bar, std::uint32_t new_mask);

// The factory's exit button (uirs\0802 ExitBtn): its text-free art.
void ExitButton(void* control);

struct Stats {
    std::uint32_t texts = 0;    // text table entries translated
    std::uint32_t header = 0;   // header dialogs given the Chinese title art
    std::uint32_t info = 0;     // OC pages given Chinese labels
    std::uint32_t badges = 0;   // NEW badges shown as 新
};
Stats GetStats();

} // namespace sdgo::oc_custom::client::l10n
