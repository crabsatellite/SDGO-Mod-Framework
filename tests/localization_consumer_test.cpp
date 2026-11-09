#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>

#include "../mods/oc_custom_client/display_text.h"
#include "sdgo/localization.h"

namespace {
void Check(bool condition, const char* message)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition)
        throw std::runtime_error(message);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return 2;
    try {
        std::wstring translated = L"unchanged";
        Check(!sdgo::localization::TryLookup(sdgo::localization::Kind::UnitName, 42,
                                             L"Gundam", &translated) &&
                  translated == L"unchanged",
              "missing optional provider preserves caller output");
        Check(sdgo::oc_custom::client::display::FixedChinese(L"开始") == L"开始" &&
                  sdgo::oc_custom::client::display::FixedChinese(L"第一行\\n第二行") ==
                      L"第一行\n第二行",
              "OC fixed controls retain reviewed Chinese and normalize line breaks");
        Check(sdgo::oc_custom::client::display::ReviewedUiText(
                  L"原生中文\\n详细说明",L"简短后备") == L"原生中文\n详细说明",
              "OC keeps the existing localized native prose rather than replacing good wording");
        Check(sdgo::oc_custom::client::display::ReviewedUiText(L"ยกเลิก",L"取消") == L"取消",
              "untranslated native text uses the reviewed Chinese fallback");
        Check(sdgo::oc_custom::client::display::LocalizedName(
                  sdgo::localization::Kind::UnitName, 42, L"Gundam") == L"Gundam",
              "OC dynamic names retain native text without the optional provider");

        const HMODULE fixture = LoadLibraryW(argv[1]);
        Check(fixture != nullptr, "test fixture loaded explicitly by the harness");
        Check(GetModuleHandleW(L"shop_localization.dll") == fixture,
              "fixture has the provider basename used by consumers");

        Check(sdgo::localization::TryLookup(sdgo::localization::Kind::UnitName, 42,
                                            L"Gundam", &translated) &&
                  translated == L"高达",
              "unit ID lookup reaches the optional provider");
        Check(sdgo::localization::TryLookup(sdgo::localization::Kind::ItemName, 200,
                                            L"EXP Capsule 200 (Full)", &translated) &&
                  translated == L"经验胶囊 200（满）",
              "item category lookup reaches the optional provider");
        Check(sdgo::oc_custom::client::display::LocalizedName(
                  sdgo::localization::Kind::UnitName, 42, L"Gundam") == L"高达" &&
                  sdgo::oc_custom::client::display::LocalizedName(
                      sdgo::localization::Kind::ItemName, 200,
                      L"EXP Capsule 200 (Full)") == L"经验胶囊 200（满）",
              "OC dynamic unit and item names reuse the optional provider");
        Check(sdgo::oc_custom::client::display::LocalizedName(
                  sdgo::localization::Kind::UnitName, 43, L"Unknown") == L"Unknown",
              "OC dynamic names preserve native text when the provider declines");
        Check(sdgo::localization::TryLookup(sdgo::localization::Kind::UiText, 0,
                                            L"The room title could not be sent.", &translated) &&
                  translated == L"无法发送房间标题。",
              "fixed UI text lookup reaches the optional provider");

        translated = L"sentinel";
        Check(!sdgo::localization::TryLookup(sdgo::localization::Kind::UnitName, 43,
                                             L"Gundam", &translated) &&
                  translated == L"sentinel",
              "unknown stable ID preserves exact caller state");

        const auto raw = reinterpret_cast<SdgoLocalizationLookupV1Fn>(
            GetProcAddress(fixture, SDGO_LOCALIZATION_LOOKUP_V1_EXPORT));
        Check(raw != nullptr, "fixture exports the stable C ABI name");
        constexpr wchar_t source[] = L"Gundam";
        const uint32_t required =
            raw(SDGO_LOCALIZATION_UNIT_NAME, 42, source, 6, nullptr, 0);
        Check(required == 3, "size query includes the terminating NUL");
        wchar_t shortBuffer[2] = {L'X', L'Y'};
        Check(raw(SDGO_LOCALIZATION_UNIT_NAME, 42, source, 6, shortBuffer, 2) == required &&
                  shortBuffer[0] == L'X' && shortBuffer[1] == L'Y',
              "insufficient output reports size without writing");
        wchar_t exactBuffer[3] = {};
        Check(raw(SDGO_LOCALIZATION_UNIT_NAME, 42, source, 6, exactBuffer, 3) == required &&
                  std::wstring(exactBuffer) == L"高达",
              "exact output capacity receives the complete translation");

        std::wstring tooLong(SDGO_LOCALIZATION_MAX_CAPACITY, L'A');
        translated = L"bounded";
        Check(!sdgo::localization::TryLookup(sdgo::localization::Kind::UiText, 0,
                                             tooLong, &translated) &&
                  translated == L"bounded",
              "consumer rejects text outside the bounded ABI");
        std::puts("LOCALIZATION OPTIONAL CONSUMER PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
