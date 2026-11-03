#pragma once

#include "sdgo/sdgo_api.h"

namespace sdgo::unit_preview {

// Installs the private candidate-client adapter for the native Secret Store
// detail dialog. The runtime gate in preview_runtime.cpp is responsible for
// ensuring this is never enabled in a player publication.
bool InstallShop(const SdgoApi* api, SdgoMod* mod);

} // namespace sdgo::unit_preview
