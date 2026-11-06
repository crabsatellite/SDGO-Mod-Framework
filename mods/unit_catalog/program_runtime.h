#pragma once
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"

namespace sdgo::unit_programs {
bool Attach(const SdgoApi* api, SdgoMod* mod, const sdgo::unit_catalog::Catalog& catalog,
            const bool* recordsReady);
}
