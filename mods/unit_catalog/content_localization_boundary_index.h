#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include "sdgo/content_localization_boundary.h"
#include "sdgo/unit_catalog_format.h"

namespace sdgo::unit_catalog {

class ContentLocalizationBoundaryIndex {
public:
    bool Build(const Catalog& catalog) {
        std::set<uint64_t> records;
        std::set<std::wstring> texts;
        for (const auto& entry : catalog.records) {
            if (!SupportedKind(entry.kind)) continue;
            if (!entry.id || entry.bytes.size() != ExpectedBytes(entry.kind)) {
                Clear();
                return false;
            }
            records.insert(RecordKey(entry.kind, entry.id));
            bool valid = false;
            switch (entry.kind) {
            case kUnitType:
                valid = AddField(entry, 5, 50, texts) &&
                        AddField(entry, 105, 50, texts) &&
                        AddField(entry, 205, 50, texts);
                break;
            case kWeaponType:
                // Only the name is text. From offset 105 a weapon row holds
                // numeric data (special feature at 132) and effect paths.
                valid = AddField(entry, 5, 50, texts);
                break;
            case kSkillType:
                valid = AddField(entry, 5, 50, texts) &&
                        AddField(entry, 0x213, 300, texts);
                break;
            case kItemCategoryType:
                valid = AddField(entry, 5, 50, texts);
                break;
            default:
                break;
            }
            if (!valid) {
                Clear();
                return false;
            }
        }
        records_.swap(records);
        texts_.swap(texts);
        return true;
    }

    void Clear() noexcept {
        records_.clear();
        texts_.clear();
    }

    bool ContainsRecord(uint32_t kind, uint32_t id) const noexcept {
        return SupportedKind(kind) && id &&
               records_.find(RecordKey(kind, id)) != records_.end();
    }

    bool ContainsText(const wchar_t* source, size_t chars) const {
        if (!source || !chars ||
            chars > SDGO_CONTENT_LOCALIZATION_MAX_SOURCE_CHARS) {
            return false;
        }
        return texts_.find(std::wstring(source, chars)) != texts_.end();
    }

    static bool SupportedKind(uint32_t kind) noexcept {
        return kind == kUnitType || kind == kWeaponType ||
               kind == kSkillType || kind == kItemCategoryType;
    }

    size_t RecordCount() const noexcept { return records_.size(); }
    size_t TextCount() const noexcept { return texts_.size(); }

private:
    static_assert(sizeof(wchar_t) == sizeof(uint16_t),
                  "native catalog text requires 16-bit wchar_t");

    static uint64_t RecordKey(uint32_t kind, uint32_t id) noexcept {
        return (static_cast<uint64_t>(kind) << 32) | id;
    }

    static std::wstring DisplayText(const std::wstring& raw) {
        std::wstring display;
        display.reserve(raw.size());
        for (size_t index = 0; index < raw.size(); ++index) {
            if (raw[index] == L'\\' && index + 1 < raw.size()) {
                if (raw[index + 1] == L'n') {
                    display.push_back(L'\n');
                    ++index;
                    continue;
                }
                if (raw[index + 1] == L't') {
                    display.push_back(L'\t');
                    ++index;
                    continue;
                }
            }
            display.push_back(raw[index]);
        }
        return display;
    }

    static bool ReadField(const Entry& entry, size_t offset, size_t capacity,
                          std::wstring& output) {
        output.clear();
        if (!capacity || offset > entry.bytes.size() ||
            capacity > (entry.bytes.size() - offset) / sizeof(uint16_t)) {
            return false;
        }
        output.reserve(capacity);
        for (size_t index = 0; index < capacity; ++index) {
            uint16_t codeUnit = 0;
            std::memcpy(&codeUnit,
                        entry.bytes.data() + offset +
                            index * sizeof(uint16_t),
                        sizeof(codeUnit));
            if (!codeUnit) return true;
            output.push_back(static_cast<wchar_t>(codeUnit));
        }
        output.clear();
        return false;
    }

    static bool AddField(const Entry& entry, size_t offset, size_t capacity,
                         std::set<std::wstring>& texts) {
        std::wstring raw;
        if (!ReadField(entry, offset, capacity, raw)) return false;
        if (raw.empty()) return true;
        texts.insert(raw);
        texts.insert(DisplayText(raw));
        return true;
    }

    std::set<uint64_t> records_;
    std::set<std::wstring> texts_;
};

} // namespace sdgo::unit_catalog
