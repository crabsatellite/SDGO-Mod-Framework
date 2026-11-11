#include "sdgo/acquisition_sources_file.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace sdgo::acquisition_sources_file;

namespace {

void Put32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(value));
}

void Finish(std::vector<std::uint8_t>& file, const char (&magic)[9],
            std::uint32_t count, std::uint32_t authority,
            const std::vector<std::uint8_t>& payload) {
    Header header{};
    std::memcpy(header.magic, magic, 8);
    header.version = kFormatVersion;
    header.headerBytes = sizeof(Header);
    header.count = count;
    header.payloadBytes = static_cast<std::uint32_t>(payload.size());
    header.payloadHash = Hash32(payload.data(), payload.size());
    header.authority = authority;
    file.resize(sizeof(header));
    std::memcpy(file.data(), &header, sizeof(header));
    file.insert(file.end(), payload.begin(), payload.end());
}

struct SourceInput {
    std::uint32_t subject;
    std::uint32_t recipe;
    std::string key;
    std::wstring text;
};

std::vector<std::uint8_t> Sources(const std::vector<SourceInput>& rows,
                                  std::uint32_t provider) {
    std::vector<std::uint8_t> payload;
    for (const SourceInput& row : rows) {
        Put32(payload, row.subject);
        Put32(payload, row.recipe);
        Put32(payload, static_cast<std::uint32_t>(row.key.size()));
        Put32(payload, static_cast<std::uint32_t>(row.text.size()));
        payload.insert(payload.end(), row.key.begin(), row.key.end());
        const auto* text = reinterpret_cast<const std::uint8_t*>(row.text.data());
        payload.insert(payload.end(), text, text + row.text.size() * sizeof(wchar_t));
    }
    std::vector<std::uint8_t> file;
    Finish(file, "SDGOAS1\0", static_cast<std::uint32_t>(rows.size()),
           provider, payload);
    return file;
}

std::vector<std::uint8_t> Availability(
    const std::vector<AvailabilityRow>& rows, std::uint32_t catalogHash) {
    std::vector<std::uint8_t> payload;
    for (const AvailabilityRow& row : rows) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&row);
        payload.insert(payload.end(), bytes, bytes + sizeof(row));
    }
    std::vector<std::uint8_t> file;
    Finish(file, "SDGOBP1\0", static_cast<std::uint32_t>(rows.size()),
           catalogHash, payload);
    return file;
}

struct Captured {
    std::vector<std::pair<std::string, std::wstring>> rows;
    bool accept = true;
};

int __cdecl Emit(void* context, const char* key, const wchar_t* text,
                 std::uint32_t chars) {
    auto* captured = static_cast<Captured*>(context);
    if (!captured->accept) return 0;
    captured->rows.emplace_back(key, std::wstring(text, chars));
    return 1;
}

std::uint32_t Query(const SourceFile& source, std::uint32_t subject,
                    std::uint32_t recipe, Captured& captured) {
    const SdgoAcquisitionRequestV1 request{
        sizeof(SdgoAcquisitionRequestV1), 1, subject, recipe};
    const SdgoAcquisitionSinkV1 sink{
        sizeof(SdgoAcquisitionSinkV1), 1, &captured, &Emit};
    return source.Query(&request, &sink);
}

} // namespace

int main() {
    std::string error;
    SourceFile source;
    const auto valid = Sources({
        {SDGO_ACQUISITION_UNIT, 10549, "shop:unit:10549:rental", L"商城租用"},
        {SDGO_ACQUISITION_BLUEPRINT, 209001, "operator:60003:blueprint:209001", L"副官掉落：设计图补给官·希美"},
        {SDGO_ACQUISITION_BLUEPRINT, 209001, "shop:blueprint:209001", L"商城购买"},
        {SDGO_ACQUISITION_BLUEPRINT, 209002, "shop:blueprint:209002", L"商城购买"},
    }, kShopProvider);
    assert(source.Parse(valid, kShopProvider, &error));
    assert(source.status() == SDGO_ACQUISITION_READY);
    Captured captured;
    assert(Query(source, SDGO_ACQUISITION_BLUEPRINT, 209001, captured) ==
           SDGO_ACQUISITION_READY);
    assert(captured.rows.size() == 2);
    assert(captured.rows[0].first == "operator:60003:blueprint:209001");
    assert(captured.rows[1].second == L"商城购买");

    Captured unitCaptured;
    assert(Query(source, SDGO_ACQUISITION_UNIT, 10549, unitCaptured) ==
           SDGO_ACQUISITION_READY);
    assert(unitCaptured.rows.size() == 1 &&
           unitCaptured.rows[0].second == L"商城租用");

    Captured rejected;
    rejected.accept = false;
    assert(Query(source, SDGO_ACQUISITION_BLUEPRINT, 209001, rejected) ==
           SDGO_ACQUISITION_INCOMPLETE);

    auto corrupt = valid;
    corrupt.back() ^= 1;
    assert(!source.Parse(corrupt, kShopProvider, &error));
    assert(source.status() == SDGO_ACQUISITION_INCOMPLETE && source.rows().empty());
    assert(!source.Parse(valid, kOperatorProvider, &error));

    const auto duplicate = Sources({
        {SDGO_ACQUISITION_BLUEPRINT, 209001, "same", L"商城购买"},
        {SDGO_ACQUISITION_BLUEPRINT, 209001, "same", L"商城购买"},
    }, kShopProvider);
    assert(!source.Parse(duplicate, kShopProvider, &error));

    const wchar_t invalidSurrogate[] = {static_cast<wchar_t>(0xD800)};
    const auto invalidUtf16 = Sources({
        {SDGO_ACQUISITION_BLUEPRINT, 209001, "bad", std::wstring(invalidSurrogate, 1)},
    }, kShopProvider);
    assert(!source.Parse(invalidUtf16, kShopProvider, &error));

    const auto missing = std::filesystem::temp_directory_path() /
        (L"sdgo-acquisition-missing-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    DeleteFileW(missing.c_str());
    source.Load(missing.wstring(), kShopProvider, &error);
    assert(source.status() == SDGO_ACQUISITION_UNAVAILABLE);

    constexpr std::uint32_t catalogHash = 0x12345678;
    AvailabilityFile availability;
    const auto states = Availability({
        {209001, 90001, 1},
        {209002, 90002, 0},
    }, catalogHash);
    assert(availability.Parse(states, catalogHash, &error));
    assert(availability.State(209001, 90001) == kBlueprintEnabled);
    assert(availability.State(209002, 90002) == kBlueprintDisabled);
    assert(availability.State(209001, 90002) == kBlueprintIndeterminate);
    assert(availability.State(209003, 90003) == kBlueprintIndeterminate);

    assert(!availability.Parse(states, catalogHash + 1, &error));
    assert(!availability.ready());
    assert(availability.State(209001, 90001) == kBlueprintIndeterminate);

    const auto duplicateAvailability = Availability({
        {209001, 90001, 1},
        {209001, 90001, 0},
    }, catalogHash);
    assert(!availability.Parse(duplicateAvailability, catalogHash, &error));

    availability.Load(missing.wstring(), catalogHash, &error);
    assert(!availability.ready());
    assert(availability.State(209001, 90001) == kBlueprintIndeterminate);
    return 0;
}