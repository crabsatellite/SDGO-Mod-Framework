#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace sdgo::mission_damage_gate {
constexpr size_t kHeaderBytes = 32;
constexpr size_t kRowBytes = 20;
constexpr uint32_t kMaxRows = 16384;

struct Entry {
    uint32_t missionId = 0, phase = 0, targetNpcId = 0, guardNpcId = 0, requiredDefeats = 0;
};

inline uint32_t U32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

inline uint32_t Checksum(const uint8_t* bytes, size_t count) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

inline auto Key(const Entry& entry) {
    return std::make_tuple(entry.missionId, entry.phase, entry.targetNpcId, entry.guardNpcId);
}

inline bool Parse(const void* source, size_t size, std::vector<Entry>& result, std::string& error) {
    result.clear(); error.clear();
    const auto* bytes = static_cast<const uint8_t*>(source);
    if (!bytes || size < kHeaderBytes || std::memcmp(bytes, "SDGODMG1", 8) != 0 ||
        U32(bytes + 8) != 1 || U32(bytes + 12) != kHeaderBytes || U32(bytes + 16) != kRowBytes) {
        error = "invalid damage gate header"; return false;
    }
    const uint32_t count = U32(bytes + 20), payload = U32(bytes + 24);
    if (count > kMaxRows || payload != count * kRowBytes || size != kHeaderBytes + payload ||
        Checksum(bytes + kHeaderBytes, payload) != U32(bytes + 28)) {
        error = "invalid damage gate length or checksum"; return false;
    }
    std::vector<Entry> parsed;
    parsed.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* row = bytes + kHeaderBytes + size_t(i) * kRowBytes;
        Entry entry{U32(row), U32(row + 4), U32(row + 8), U32(row + 12), U32(row + 16)};
        if (!entry.missionId || entry.phase < 1 || entry.phase > 8 || !entry.targetNpcId || !entry.guardNpcId ||
            entry.targetNpcId == entry.guardNpcId || entry.requiredDefeats < 1 || entry.requiredDefeats > 255 ||
            (!parsed.empty() && !(Key(parsed.back()) < Key(entry)))) {
            error = "invalid or unordered damage gate row"; return false;
        }
        parsed.push_back(entry);
    }
    result.swap(parsed);
    return true;
}
} // namespace sdgo::mission_damage_gate
