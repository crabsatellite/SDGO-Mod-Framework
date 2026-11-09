#pragma once

#include <windows.h>

#include <cstdint>

#define SDGO_UNIT_CATALOG_LOCALIZATION_BOUNDARY_V1_EXPORT \
    "SdgoUnitCatalogLocalizationBoundaryV1"
#define SDGO_CONTENT_LOCALIZATION_NOT_OWNED 0u
#define SDGO_CONTENT_LOCALIZATION_STOCK_OR_UNKNOWN \
    SDGO_CONTENT_LOCALIZATION_NOT_OWNED
#define SDGO_CONTENT_LOCALIZATION_OWNED 1u
#define SDGO_CONTENT_LOCALIZATION_INDETERMINATE 2u
// The longest protected native field has capacity for 300 UTF-16 code units.
// Longer caller input is outside this V1 contract and returns indeterminate.
#define SDGO_CONTENT_LOCALIZATION_MAX_SOURCE_CHARS 300u

extern "C" {
// recordKind uses the native grid kind (2, 10, 13 or 0x22). Pass 0/0 for an
// exact source-only query when the caller has no stable record identity.
using SdgoUnitCatalogLocalizationBoundaryV1Fn = uint32_t(__cdecl*)(
    uint32_t recordKind,
    uint32_t recordId,
    const wchar_t* source,
    uint32_t sourceChars);
}

namespace sdgo::content_localization_boundary {

// Absence of unit_catalog means no content package claims the text. A loaded
// older/incomplete provider is conservative because it cannot prove ownership.
inline uint32_t QueryProvider(uint32_t recordKind, uint32_t recordId,
                              const wchar_t* source, uint32_t sourceChars) noexcept {
    const HMODULE module = GetModuleHandleW(L"unit_catalog.dll");
    if (!module) return SDGO_CONTENT_LOCALIZATION_STOCK_OR_UNKNOWN;
    const auto query = reinterpret_cast<SdgoUnitCatalogLocalizationBoundaryV1Fn>(
        GetProcAddress(module,
                       SDGO_UNIT_CATALOG_LOCALIZATION_BOUNDARY_V1_EXPORT));
    if (!query) return SDGO_CONTENT_LOCALIZATION_INDETERMINATE;
    __try {
        const uint32_t result =
            query(recordKind, recordId, source, sourceChars);
        return result <= SDGO_CONTENT_LOCALIZATION_INDETERMINATE
                   ? result
                   : SDGO_CONTENT_LOCALIZATION_INDETERMINATE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SDGO_CONTENT_LOCALIZATION_INDETERMINATE;
    }
}

inline uint32_t Query(uint32_t recordKind,uint32_t recordId,
                      const wchar_t* source,uint32_t sourceChars) noexcept {
    const DWORD previous=GetLastError();
    const uint32_t result=QueryProvider(recordKind,recordId,source,sourceChars);
    SetLastError(previous);
    return result;
}

} // namespace sdgo::content_localization_boundary
