#pragma once

#include <cstdint>
#include "sdgo/sdgo_api.h"

namespace sdgo::unit_name_display {

struct ContextView {
    std::uint32_t unit_id;
    wchar_t primary_name[51];
    std::uint32_t primary_chars;
};

// Returns a copied stable unit identity and primary record name only while one
// of the three native unit-name helpers is running for an audited UI caller,
// and only at that helper's exact internal string-assignment return address.
// The output is untouched on false.
bool CurrentUnitNameContext(const void* assign_return,
                            ContextView* output) noexcept;

bool Attach(const SdgoApi* api, SdgoMod* mod);

} // namespace sdgo::unit_name_display
