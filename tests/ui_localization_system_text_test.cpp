#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../mods/ui_localization/system_text.h"

namespace {

using namespace sdgo::ui_localization::system_text;

unsigned g_checks = 0;
const char* g_hookName = nullptr;
void* g_detour = nullptr;

void Check(bool condition, const char* message)
{
    ++g_checks;
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition)
        throw std::runtime_error(message);
}

void WriteId(Record* record, std::uint32_t id)
{
    std::memcpy(record->data(), &id, sizeof(id));
}

void WriteText(Record* record, const wchar_t* text)
{
    std::memset(record->data() + kTextOffset, 0, kTextCharacters * 2);
    for (std::size_t index = 0; text[index]; ++index) {
        const std::uint16_t value = static_cast<std::uint16_t>(text[index]);
        std::memcpy(record->data() + kTextOffset + index * 2, &value,
                    sizeof(value));
    }
}

std::wstring ReadText(const Record& record)
{
    wchar_t text[kTextCharacters] = {};
    for (std::size_t index = 0; index < kTextCharacters; ++index) {
        std::uint16_t value = 0;
        std::memcpy(&value, record.data() + kTextOffset + index * 2,
                    sizeof(value));
        text[index] = static_cast<wchar_t>(value);
        if (!value)
            return text;
    }
    throw std::runtime_error("record text has no terminator");
}

int HookCode(SdgoMod*, const char* name, void* detour, void** original)
{
    g_hookName = name;
    g_detour = detour;
    *original = reinterpret_cast<void*>(1);
    return 1;
}

} // namespace

int main()
{
    try {
        Record source{};
        source.fill(0xA5);
        WriteId(&source, 126956);
        source[4] = 0x5A;
        WriteText(&source, L"Does not match the version. To end the game");
        const std::array<std::uint8_t, 4> style = {0xDE, 0xAD, 0xBE, 0xEF};
        std::memcpy(source.data() + kStyleOffset, style.data(), style.size());
        const Record original = source;

        Record output{};
        output.fill(0xCC);
        Check(TranslateRecord(source.data(), source.size(), 126956, kTableType,
                              &output) == PatchResult::replaced,
              "an exact system text ID and source phrase is translated");
        Check(source == original, "translation never mutates the native row");
        Check(ReadText(output) == L"版本不匹配，游戏即将结束。",
              "the translated row contains the reviewed Chinese phrase");
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (index >= kTextOffset && index < kStyleOffset)
                continue;
            Check(output[index] == source[index],
                  "all bytes outside the fixed text slot are preserved");
        }

        Record sentinel{};
        sentinel.fill(0x7B);
        output = sentinel;
        WriteText(&source, L"Does not match the version. To end the game?");
        Check(TranslateRecord(source.data(), source.size(), 126956, kTableType,
                              &output) == PatchResult::unchanged &&
                  output == sentinel,
              "an unknown source phrase leaves the output untouched");
        Check(TranslateRecord(original.data(), original.size(), 126956, 2,
                              &output) == PatchResult::unchanged &&
                  output == sentinel,
              "non-UI tables are never patched");

        const std::wstring overlong(kTextCharacters, L'长');
        Check(PatchRecord(original.data(), original.size(), 126956, kTableType,
                          overlong, &output) == PatchResult::invalid &&
                  output == sentinel,
              "text that cannot fit the native slot is rejected");
        Check(TranslateRecord(original.data(), original.size(), 126957,
                              kTableType, &output) == PatchResult::invalid &&
                  output == sentinel,
              "a requested ID that disagrees with the row is rejected");

        Record unterminated = original;
        for (std::size_t index = 0; index < kTextCharacters; ++index) {
            const std::uint16_t value = L'X';
            std::memcpy(unterminated.data() + kTextOffset + index * 2, &value,
                        sizeof(value));
        }
        Check(TranslateRecord(unterminated.data(), unterminated.size(), 126956,
                              kTableType, &output) == PatchResult::invalid &&
                  output == sentinel,
              "a row without a bounded text terminator is rejected");

        SdgoApi api{};
        api.size = sizeof(api);
        api.HookCode = &HookCode;
        Check(Install(&api, reinterpret_cast<SdgoMod*>(1)) &&
                  g_hookName && !std::strcmp(g_hookName,
                                             "ui_localization.grid_find") &&
                  g_detour,
              "installation owns only the declared grid-find hook");
        const Stats stats = Snapshot();
        Check(stats.lookups == 0 && stats.replaced == 0 && stats.invalid == 0,
              "pure record tests do not alter runtime hook statistics");

        std::printf("%u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
