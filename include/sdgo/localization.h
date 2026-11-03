// Optional process-local localization service. Consumers never load the
// provider and retain their exact source text when it is absent or declines.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#define SDGO_LOCALIZATION_LOOKUP_V1_EXPORT "SdgoLocalizationLookupV1"
#define SDGO_LOCALIZATION_UNIT_NAME 1u
#define SDGO_LOCALIZATION_ITEM_NAME 2u
#define SDGO_LOCALIZATION_UI_TEXT 3u
#define SDGO_LOCALIZATION_MAX_CAPACITY 8193u

extern "C" {
using SdgoLocalizationLookupV1Fn = uint32_t(__cdecl*)(
    uint32_t kind,
    uint32_t stableId,
    const wchar_t* source,
    uint32_t sourceChars,
    wchar_t* output,
    uint32_t outputChars);
}

namespace sdgo::localization {

enum class Kind : uint32_t {
    UnitName = SDGO_LOCALIZATION_UNIT_NAME,
    ItemName = SDGO_LOCALIZATION_ITEM_NAME,
    UiText = SDGO_LOCALIZATION_UI_TEXT
};

inline SdgoLocalizationLookupV1Fn FindProvider() noexcept
{
    const HMODULE module = GetModuleHandleW(L"shop_localization.dll");
    return module ? reinterpret_cast<SdgoLocalizationLookupV1Fn>(
                        GetProcAddress(module, SDGO_LOCALIZATION_LOOKUP_V1_EXPORT))
                  : nullptr;
}

inline bool TryLookup(Kind kind, uint32_t stableId, const wchar_t* source,
                      uint32_t sourceChars, std::wstring* output) noexcept
{
    if (!source || !output || sourceChars >= SDGO_LOCALIZATION_MAX_CAPACITY)
        return false;
    const auto lookup = FindProvider();
    if (!lookup)
        return false;
    try {
        const uint32_t required =
            lookup(static_cast<uint32_t>(kind), stableId, source, sourceChars, nullptr, 0);
        if (!required || required > SDGO_LOCALIZATION_MAX_CAPACITY)
            return false;
        std::vector<wchar_t> buffer(required, L'\0');
        if (lookup(static_cast<uint32_t>(kind), stableId, source, sourceChars,
                   buffer.data(), required) != required ||
            buffer.back() != L'\0')
            return false;
        size_t textChars = 0;
        while (textChars + 1 < buffer.size() && buffer[textChars])
            ++textChars;
        if (textChars + 1 != buffer.size())
            return false;
        std::wstring translated(buffer.data(), textChars);
        output->swap(translated);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool TryLookup(Kind kind, uint32_t stableId, const std::wstring& source,
                      std::wstring* output) noexcept
{
    if (source.size() >= SDGO_LOCALIZATION_MAX_CAPACITY)
        return false;
    return TryLookup(kind, stableId, source.data(), static_cast<uint32_t>(source.size()), output);
}

inline bool TryLookup(Kind kind, uint32_t stableId, const wchar_t* source,
                      std::wstring* output) noexcept
{
    if (!source)
        return false;
    uint32_t length = 0;
    while (length + 1 < SDGO_LOCALIZATION_MAX_CAPACITY && source[length])
        ++length;
    if (source[length])
        return false;
    return TryLookup(kind, stableId, source, length, output);
}

} // namespace sdgo::localization
