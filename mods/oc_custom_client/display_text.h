#pragma once

#include <cstdint>
#include <string>

#include "sdgo/localization.h"

namespace sdgo::oc_custom::client::display {

// Expand the client's stored line-break notation without changing its wording.
inline std::wstring FixedChinese(const wchar_t* text)
{
    if (!text)
        return {};
    std::wstring result(text);
    for (std::size_t at = result.find(L"\\n"); at != std::wstring::npos;
         at = result.find(L"\\n", at))
        result.replace(at, 2, L"\n");
    return result;
}

inline std::wstring ReviewedUiText(const std::wstring& native,const wchar_t* fallback)
{
    bool chinese=false,thai=false;
    for(const auto value:native) {
        chinese=chinese || (value>=0x3400 && value<=0x9FFF);
        thai=thai || (value>=0x0E00 && value<=0x0E7F);
    }
    // Preserve existing localized prose; use the reviewed fallback for missing
    // or untranslated native rows used by these fixed Mod controls.
    return FixedChinese(!native.empty() && chinese && !thai ? native.c_str():fallback);
}

// Dynamic names use the optional shared dictionary and otherwise remain exact.
inline std::wstring LocalizedName(localization::Kind kind, std::uint32_t stableId,
                                  const std::wstring& source)
{
    if (source.empty())
        return source;
    std::wstring translated;
    if (localization::TryLookup(kind, stableId, source, &translated) &&
        !translated.empty())
        return translated;
    return source;
}

} // namespace sdgo::oc_custom::client::display
