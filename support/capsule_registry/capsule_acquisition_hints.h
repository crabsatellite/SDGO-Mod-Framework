#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace capsule_catalog {
struct CapsuleAcquisitionHints {
    struct Text { std::wstring full, brief; };
    std::unordered_map<uint32_t, Text> units;

    bool Decode(const std::vector<uint8_t>& raw) {
        if (raw.size() < 16 || std::memcmp(raw.data(), "SDGOCAS1", 8)) return false;
        auto u32 = [&](size_t at) { uint32_t n; std::memcpy(&n, raw.data() + at, 4); return n; };
        const uint32_t count = u32(12);
        if (u32(8) != 1 || count > 20000) return false;
        CapsuleAcquisitionHints result;
        size_t cursor = 16;
        uint32_t previous = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (raw.size() - cursor < 12) return false;
            const uint32_t unit = u32(cursor), full = u32(cursor + 4), brief = u32(cursor + 8);
            cursor += 12;
            if (unit <= previous || unit > 9999999 || !full || full >= 100 ||
                !brief || brief > full || raw.size() - cursor < 2u * (full + brief)) return false;
            Text text;
            text.full.resize(full); text.brief.resize(brief);
            std::memcpy(&text.full[0], raw.data() + cursor, 2u * full);
            cursor += 2u * full;
            std::memcpy(&text.brief[0], raw.data() + cursor, 2u * brief);
            cursor += 2u * brief;
            if (text.full.find(L'\0') != std::wstring::npos ||
                text.brief.find(L'\0') != std::wstring::npos) return false;
            result.units.emplace(unit, std::move(text)); previous = unit;
        }
        if (cursor != raw.size()) return false;
        *this = std::move(result); return true;
    }

    const wchar_t* Expected(uint32_t unit) const {
        const auto found = units.find(unit);
        return found == units.end() ? nullptr : found->second.full.c_str();
    }

    bool Apply(void* record, uint32_t size) const {
        if (!record || size != 614) return false;
        auto* row = static_cast<uint8_t*>(record);
        uint32_t unit = 0; std::memcpy(&unit, row + 6, 4);
        const auto found = units.find(unit);
        if (row[4] != 0 || row[5] != 1 || found == units.end()) return false;
        wchar_t buffer[100]{}; std::memcpy(buffer, row + 214, sizeof(buffer));
        size_t length = 0; while (length < 100 && buffer[length]) ++length;
        if (length == 100) return false;
        const std::wstring original(buffer, length);
        const auto& hint = found->second;
        if (original.find(hint.full) != std::wstring::npos) return true;
        const wchar_t* separator = original.empty() ? L"" : L"\n";
        std::wstring value = original + separator + hint.full;
        if (value.size() >= 100) {
            if (original.size() >= hint.brief.size() && original.compare(
                original.size() - hint.brief.size(), hint.brief.size(), hint.brief) == 0) return true;
            value = original + separator + hint.brief;
        }
        if (value.size() >= 100) return false;
        std::memset(row + 214, 0, 200);
        std::memcpy(row + 214, value.data(), value.size() * 2);
        return true;
    }
};
}
