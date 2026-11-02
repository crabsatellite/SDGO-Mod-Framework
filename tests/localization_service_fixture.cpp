#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "sdgo/localization.h"

namespace {
const wchar_t* Translation(uint32_t kind, uint32_t stableId, const std::wstring& source)
{
    if (kind == SDGO_LOCALIZATION_UNIT_NAME && stableId == 42 && source == L"Gundam")
        return L"高达";
    if (kind == SDGO_LOCALIZATION_ITEM_NAME && stableId == 200 &&
        source == L"EXP Capsule 200 (Full)")
        return L"经验胶囊 200（满）";
    if (kind == SDGO_LOCALIZATION_UI_TEXT && stableId == 0 &&
        source == L"The room title could not be sent.")
        return L"无法发送房间标题。";
    return nullptr;
}
}

extern "C" __declspec(dllexport) uint32_t __cdecl SdgoLocalizationLookupV1(
    uint32_t kind, uint32_t stableId, const wchar_t* source, uint32_t sourceChars,
    wchar_t* output, uint32_t outputChars)
{
    if (!source || sourceChars >= SDGO_LOCALIZATION_MAX_CAPACITY)
        return 0;
    const std::wstring sourceText(source, sourceChars);
    if (sourceText.find(L'\0') != std::wstring::npos)
        return 0;
    const wchar_t* text = Translation(kind, stableId, sourceText);
    if (!text)
        return 0;
    const uint32_t required = static_cast<uint32_t>(wcslen(text) + 1);
    if (!output || outputChars < required)
        return required;
    std::memcpy(output, text, required * sizeof(wchar_t));
    return required;
}
