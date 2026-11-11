#include "acquisition_text.h"
#include "acquisition.generated.h"
#include "sdgo/content_localization_boundary.h"
#include "sdgo/acquisition_sources.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace sdgo::ui_localization::acquisition_text {
namespace {
static_assert(kTextOffset + kTextCharacters * sizeof(wchar_t) == 414);
using BuildEvents = void(__thiscall*)(void*);
using GridData = void*(__cdecl*)();
using GridFind = bool(__thiscall*)(void*, const std::uint8_t**, std::uint32_t, int, char);
using Assign = void*(__thiscall*)(void*, const wchar_t*, std::uint32_t);
BuildEvents originalBuild = nullptr;
GridData gridData = nullptr;
GridFind gridFind = nullptr;
Assign assign = nullptr;
bool active = false;

std::uint32_t Read32(const std::uint8_t* source) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

const AcquisitionPhrase* Phrase(std::uint32_t id) noexcept {
    const auto* end = kAcquisitionPhrases + kAcquisitionPhraseCount;
    const auto* found = std::lower_bound(kAcquisitionPhrases, end, id,
        [](const AcquisitionPhrase& row, std::uint32_t value) { return row.id < value; });
    return found != end && found->id == id ? found : nullptr;
}

bool CopyRecord(const std::uint8_t* source, Record* output) noexcept {
    if (!source) return false;
    __try { std::memcpy(output->data(), source, output->size()); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Context(void* self, std::uint32_t* unit, std::uintptr_t* first,
             std::uintptr_t* last) noexcept {
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(self);
        std::memcpy(unit, bytes + 0x44, 4);
        std::memcpy(first, bytes + 0x178, 4);
        std::memcpy(last, bytes + 0x17C, 4);
        return *unit && *first && *last >= *first && *last - *first <= 28 * 256 &&
               (*last - *first) % 28 == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CopiedText(std::uintptr_t at, wchar_t* text, std::size_t* count) noexcept {
    __try {
        const auto* value = reinterpret_cast<const std::uint8_t*>(at);
        const auto length = Read32(value + 16), capacity = Read32(value + 20);
        if (!length || length >= kTextCharacters || length > capacity || (capacity < 8 && length > 7)) return false;
        const wchar_t* source = reinterpret_cast<const wchar_t*>(value);
        if (capacity >= 8) std::memcpy(&source, value, 4);
        if (!source) return false;
        std::memcpy(text, source, length * 2);
        text[length] = 0;
        *count = length;
        return std::wmemchr(text, 0, length) == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring Field(const Record& record) {
    wchar_t text[kTextCharacters]{};
    std::memcpy(text, record.data() + kTextOffset, sizeof(text));
    return std::wstring(text);
}

void TranslateEvents(void* self) {
    std::uint32_t unit = 0;
    std::uintptr_t first = 0, last = 0;
    if (!Context(self, &unit, &first, &last)) return;
    for (const auto& phrase : kAcquisitionPhrases) {
        if (phrase.unit != unit) continue;
        const std::uint8_t* row = nullptr;
        Record original{}, patched{};
        auto* grid = gridData();
        if (!grid || !gridFind(grid, &row, phrase.id, kTableType, 0) ||
            !CopyRecord(row, &original) ||
            !TranslateRecord(original.data(), original.size(), phrase.id, &patched)) continue;
        const auto before = Field(original), after = Field(patched);
        for (auto at = first; at < last; at += 28) {
            wchar_t text[kTextCharacters]{};
            std::size_t count = 0;
            if (CopiedText(at, text, &count) && std::wstring_view(text, count) == before)
                assign(reinterpret_cast<void*>(at), after.data(), static_cast<std::uint32_t>(after.size()));
        }
    }
}

void __fastcall Build(void* self, void*) {
    originalBuild(self);
    const auto error = GetLastError();
    try { TranslateEvents(self); } catch (...) {}
    SetLastError(error);
}
} // namespace

bool TranslateRecord(const std::uint8_t* source, std::size_t bytes,
                     std::uint32_t id, Record* output) noexcept {
    const auto* phrase = Phrase(id);
    if (!phrase || !source || !output || bytes != kRecordBytes ||
        Read32(source) != id || Read32(source + kUnitOffset) != phrase->unit) return false;
    wchar_t text[kTextCharacters]{};
    std::size_t length = 0;
    for (; length < kTextCharacters; ++length) {
        std::memcpy(text + length, source + kTextOffset + length * 2, 2);
        if (!text[length]) break;
    }
    const std::wstring_view full(text, length), original(phrase->source);
    if (length == kTextCharacters || full.size() < original.size() ||
        full.substr(0, original.size()) != original) return false;
    const auto suffix = full.substr(original.size());
    // Owner-authored acquisition hints can follow the unchanged stock prose.
    // Preserve that suffix verbatim; admit only a real native line separator.
    if (!suffix.empty() && suffix.front() != L'\n' && suffix.substr(0, 2) != L"\\n") return false;
    if (
        sdgo::content_localization_boundary::Query(2, phrase->unit, text,
            static_cast<std::uint32_t>(length)) != SDGO_CONTENT_LOCALIZATION_NOT_OWNED) return false;
    const std::wstring_view target(phrase->target);
    if (target.size() + suffix.size() >= kTextCharacters) return false;
    Record patched{};
    std::memcpy(patched.data(), source, patched.size());
    std::memset(patched.data() + kTextOffset, 0, kTextCharacters * 2);
    std::memcpy(patched.data() + kTextOffset, target.data(), target.size() * 2);
    std::memcpy(patched.data() + kTextOffset + target.size() * 2, suffix.data(), suffix.size() * 2);
    *output = patched;
    return true;
}

bool Install(const SdgoApi* api, SdgoMod* mod) {
    if (!api || !api->Symbol || !api->HookCode) return false;
    gridData = reinterpret_cast<GridData>(api->Symbol(mod, "ui_localization.grid_data"));
    gridFind = reinterpret_cast<GridFind>(api->Symbol(mod, "ui_localization.grid_find"));
    assign = reinterpret_cast<Assign>(api->Symbol(mod, "ui_localization.assign"));
    if (!gridData || !gridFind || !assign) return false;
    const auto consumer = GetModuleHandleW(L"acquisition_sources.dll");
    const auto installed = consumer ? reinterpret_cast<SdgoAcquisitionDisplayInstalledV1Fn>(
        GetProcAddress(consumer, SDGO_ACQUISITION_DISPLAY_EXPORT)) : nullptr;
    if (sdgo::acquisition_sources::DisplayInstalled(installed)) return true;
    if (api->HookCode(mod, "ui_localization.acquisition_build_events",
        reinterpret_cast<void*>(&Build), reinterpret_cast<void**>(&originalBuild)) && originalBuild) return true;
    if (api->Log) api->Log(mod, SDGO_LOG_WARN,
        "acquisition display localization unavailable; native layout captions remain enabled");
    return true;
}
void Activate() noexcept { active = true; }
void Deactivate() noexcept { active = false; }
} // namespace sdgo::ui_localization::acquisition_text

extern "C" __declspec(dllexport) void __cdecl SdgoUiLocalizationAcquisitionEventsV1(void* self) {
    if (!sdgo::ui_localization::acquisition_text::active) return;
    const auto error = GetLastError();
    try { sdgo::ui_localization::acquisition_text::TranslateEvents(self); } catch (...) {}
    SetLastError(error);
}
