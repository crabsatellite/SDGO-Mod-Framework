#include "../mods/ui_localization/acquisition_text.h"
#include "../mods/ui_localization/acquisition.generated.h"
#include <windows.h>
#include <cassert>
#include <cstring>
#include <string_view>

using namespace sdgo::ui_localization;
using namespace sdgo::ui_localization::acquisition_text;

Record Source(const AcquisitionPhrase& phrase) {
    Record result{};
    result.fill(0x35);
    std::memcpy(result.data(), &phrase.id, 4);
    std::memcpy(result.data() + kUnitOffset, &phrase.unit, 4);
    std::memset(result.data() + kTextOffset, 0, kTextCharacters * 2);
    const std::wstring_view text(phrase.source);
    std::memcpy(result.data() + kTextOffset, text.data(), text.size() * 2);
    return result;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    for (const auto& phrase : kAcquisitionPhrases) {
        const auto source = Source(phrase);
        const auto before = source;
        Record output{};
        assert(TranslateRecord(source.data(), source.size(), phrase.id, &output));
        assert(source == before);
        assert(!std::memcmp(source.data(), output.data(), kTextOffset));
        const std::wstring_view target(phrase.target);
        assert(!std::memcmp(output.data() + kTextOffset, target.data(), target.size() * 2));
        const auto textEnd = kTextOffset + kTextCharacters * 2;
        for (auto i = kTextOffset + target.size() * 2; i < textEnd; ++i) assert(output[i] == 0);
        assert(!std::memcmp(source.data() + textEnd, output.data() + textEnd, output.size() - textEnd));
        Record sentinel{};
        sentinel.fill(0x7D);
        output = sentinel;
        auto changed = source;
        changed[kTextOffset] ^= 1;
        assert(!TranslateRecord(changed.data(), changed.size(), phrase.id, &output) && output == sentinel);
        changed = source;
        changed[kUnitOffset] ^= 1;
        assert(!TranslateRecord(changed.data(), changed.size(), phrase.id, &output) && output == sentinel);
        assert(!TranslateRecord(source.data(), source.size() - 1, phrase.id, &output) && output == sentinel);
        assert(!TranslateRecord(source.data(), source.size(), 1, &output) && output == sentinel);
    }
    const auto source = Source(kAcquisitionPhrases[0]);
    Record output{};
    auto suffixed = source;
    const std::wstring_view original(kAcquisitionPhrases[0].source), translated(kAcquisitionPhrases[0].target);
    const std::wstring_view suffix(L"\\n作者说明 100000 GB");
    std::memcpy(suffixed.data() + kTextOffset + original.size() * 2, suffix.data(), (suffix.size() + 1) * 2);
    assert(TranslateRecord(suffixed.data(), suffixed.size(), kAcquisitionPhrases[0].id, &output));
    assert(!std::memcmp(output.data() + kTextOffset + translated.size() * 2, suffix.data(), (suffix.size() + 1) * 2));
    suffixed[kTextOffset + original.size() * 2] = 'x';
    assert(!TranslateRecord(suffixed.data(), suffixed.size(), kAcquisitionPhrases[0].id, &output));
    // A previously translated stock copy must not bypass a later content claim.
    const auto provider = LoadLibraryA(argv[1]);
    assert(provider);
    using Set = void(__cdecl*)(unsigned, unsigned);
    const auto set = reinterpret_cast<Set>(GetProcAddress(provider, "SdgoShopLocalizationOriginFixtureSet"));
    assert(set);
    for (const auto status : {1u, 2u}) {
        set(0, status);
        assert(!TranslateRecord(source.data(), source.size(), kAcquisitionPhrases[0].id, &output));
    }
    set(0, 0);
    assert(TranslateRecord(source.data(), source.size(), kAcquisitionPhrases[0].id, &output));
    FreeLibrary(provider);
    const auto legacy = LoadLibraryA(argv[2]);
    assert(legacy && !TranslateRecord(source.data(), source.size(), kAcquisitionPhrases[0].id, &output));
    FreeLibrary(legacy);
    return 0;
}
