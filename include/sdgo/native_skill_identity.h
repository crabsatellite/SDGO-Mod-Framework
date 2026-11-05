#pragma once
#include "skill_identity.h"
#include "sdgo_api.h"

namespace sdgo::skill_identity {
// Queue both exact original dispatcher sites during SdgoModLoad. A failure
// requires declining the whole Mod so the core rolls back its queued hooks.
// Rejected owned contexts return false from the native supplemental dispatcher.
// The lookup object and bindings remain valid for the entire process lifetime.
bool InstallNative(const SdgoApi* api, SdgoMod* mod,
    std::shared_ptr<const Bindings> bindings, const Lookup* lookup, std::string& error);
}
