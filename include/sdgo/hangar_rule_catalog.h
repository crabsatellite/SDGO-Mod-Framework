#pragma once

#include "hangar_model.h"
#include "unit_catalog_format.h"

#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sdgo::hangar {

struct CatalogRuleStats {
    size_t catalogUnits = 0;
    size_t added = 0;
    size_t explicitRules = 0;
    size_t unknownGrades = 0;
    size_t ambiguousGrades = 0;
};

inline bool SameReward(const Rule& left, const Rule& right) {
    return left.gb == right.gb && left.item == right.item &&
           left.amount == right.amount;
}

inline bool UnitLocalizedName(
    const unit_catalog::Entry& entry, std::wstring& output) {
    if (entry.kind != unit_catalog::kUnitType ||
        entry.bytes.size() != unit_catalog::kUnitBytes) {
        return false;
    }
    output.clear();
    const auto* raw = entry.bytes.data() + unit_catalog::kUnitNameOffset;
    for (size_t index = 0; index < unit_catalog::kUnitNameBytes / 2; ++index) {
        const uint16_t character = static_cast<uint16_t>(raw[index * 2]) |
            (static_cast<uint16_t>(raw[index * 2 + 1]) << 8);
        if (!character) return !output.empty();
        if (character < 32) return false;
        if (character >= 0xd800 && character <= 0xdbff) {
            if (++index >= unit_catalog::kUnitNameBytes / 2) return false;
            const uint16_t low = static_cast<uint16_t>(raw[index * 2]) |
                (static_cast<uint16_t>(raw[index * 2 + 1]) << 8);
            if (low < 0xdc00 || low > 0xdfff) return false;
            output.push_back(static_cast<wchar_t>(character));
            output.push_back(static_cast<wchar_t>(low));
            continue;
        }
        if (character >= 0xdc00 && character <= 0xdfff) return false;
        output.push_back(static_cast<wchar_t>(character));
    }
    return false;
}

inline CatalogRuleStats MergeCatalogRules(
    Rules& rules, const unit_catalog::Catalog& catalog) {
    using Grade = std::pair<int, int>;
    std::map<Grade, Rule> templates;
    std::set<Grade> ambiguous;
    for (const auto& item : rules) {
        const Rule& rule = item.second;
        const Grade grade(rule.rank, rule.subrank);
        if (ambiguous.count(grade)) continue;
        const auto existing = templates.find(grade);
        if (existing == templates.end()) {
            templates.emplace(grade, rule);
        } else if (!SameReward(existing->second, rule)) {
            templates.erase(existing);
            ambiguous.insert(grade);
        }
    }

    Rules candidate = rules;
    CatalogRuleStats stats;
    std::set<uint32_t> catalogIds;
    for (const auto& entry : catalog.records) {
        if (entry.kind != unit_catalog::kUnitType) continue;
        ++stats.catalogUnits;
        if (entry.bytes.size() != unit_catalog::kUnitBytes ||
            !entry.id || entry.id > unit_catalog::kMaxId ||
            unit_catalog::U32(entry.bytes.data()) != entry.id ||
            entry.bytes[4] != 0 ||
            unit_catalog::Hash32(entry.bytes.data(), entry.bytes.size()) !=
                entry.recordHash ||
            !catalogIds.insert(entry.id).second) {
            throw std::invalid_argument("invalid or conflicting imported unit rule");
        }
        if (candidate.count(entry.id)) {
            ++stats.explicitRules;
            continue;
        }
        const int rank = entry.bytes[unit_catalog::kUnitGradeOffset];
        const int subrank = entry.bytes[unit_catalog::kUnitSubgradeOffset];
        const Grade grade(rank, subrank);
        if (rank < 1 || rank > 4 || subrank < 1 || subrank > 5) {
            ++stats.unknownGrades;
            continue;
        }
        if (ambiguous.count(grade)) {
            ++stats.ambiguousGrades;
            continue;
        }
        const auto source = templates.find(grade);
        if (source == templates.end()) {
            ++stats.unknownGrades;
            continue;
        }
        std::wstring name;
        if (!UnitLocalizedName(entry, name)) {
            throw std::invalid_argument("invalid imported unit localized name");
        }
        Rule added = source->second;
        added.unit = entry.id;
        added.rank = rank;
        added.subrank = subrank;
        added.name = std::move(name);
        candidate.emplace(added.unit, std::move(added));
        ++stats.added;
    }
    rules.swap(candidate);
    return stats;
}

} // namespace sdgo::hangar
