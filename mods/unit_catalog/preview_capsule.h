#pragma once

#include "sdgo/sdgo_api.h"

namespace sdgo::unit_preview {

// Installs the private fixed/custom capsule receipt hooks. The caller only
// invokes this when native-preview.ini has selected a validated catalog unit.
bool InstallCapsule(const SdgoApi* api, SdgoMod* mod);

} // namespace sdgo::unit_preview
