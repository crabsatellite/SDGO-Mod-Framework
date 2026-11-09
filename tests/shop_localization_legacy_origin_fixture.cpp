#include <cstdint>

// This intentionally represents a loaded provider from before the optional
// content-origin contract existed.
extern "C" __declspec(dllexport) std::uint32_t __cdecl
SdgoShopLocalizationLegacyOriginFixtureV0() {
    return 0;
}
