#pragma once

#include "sdgo/sdgo_api.h"

namespace sdgo::shop_localization {

bool BindSearchHooks(
    const SdgoApi* api, SdgoMod* mod, const bool* searchEnabled,
    const bool* interfaceEnabled);

} // namespace sdgo::shop_localization
