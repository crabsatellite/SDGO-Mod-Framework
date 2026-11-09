#include "sdgo/content_localization_boundary.h"

#include <windows.h>

#include <cstdint>

namespace {

volatile LONG g_sourceOnly =
    static_cast<LONG>(SDGO_CONTENT_LOCALIZATION_INDETERMINATE);
volatile LONG g_identity =
    static_cast<LONG>(SDGO_CONTENT_LOCALIZATION_INDETERMINATE);

std::uint32_t Response(volatile LONG* value) {
    const LONG current = InterlockedCompareExchange(value, 0, 0);
    if (current < static_cast<LONG>(SDGO_CONTENT_LOCALIZATION_NOT_OWNED) ||
        current > static_cast<LONG>(
                      SDGO_CONTENT_LOCALIZATION_INDETERMINATE)) {
        return SDGO_CONTENT_LOCALIZATION_INDETERMINATE;
    }
    return static_cast<std::uint32_t>(current);
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl
SdgoShopLocalizationOriginFixtureSet(std::uint32_t sourceOnly,
                                     std::uint32_t identity) {
    InterlockedExchange(&g_sourceOnly, static_cast<LONG>(sourceOnly));
    InterlockedExchange(&g_identity, static_cast<LONG>(identity));
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
SdgoUnitCatalogLocalizationBoundaryV1(std::uint32_t recordKind,
                                      std::uint32_t recordId,
                                      const wchar_t*,
                                      std::uint32_t) {
    return recordKind == 0 && recordId == 0
               ? Response(&g_sourceOnly)
               : Response(&g_identity);
}
