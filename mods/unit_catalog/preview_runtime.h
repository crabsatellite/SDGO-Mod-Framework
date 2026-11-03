#pragma once
#include <cstdint>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"

// This interface is enabled only by the Manager's private candidate client.
// Player publications never contain native-preview.ini or its receipts.
namespace sdgo::unit_preview {
bool Target(uint32_t* unit, uint32_t* category);
void Seen(const char* role, uint32_t unit, const char* renderer);
void Failed(const char* role, const char* reason);
bool Attach(const SdgoApi* api, SdgoMod* mod, const unit_catalog::Catalog& catalog);
bool InstallShop(const SdgoApi* api, SdgoMod* mod);
bool InstallCapsule(const SdgoApi* api, SdgoMod* mod);
}
