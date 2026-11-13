#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>
#include "sdgo/mission_damage_gate_format.h"

namespace sdgo::mission_retreat {
constexpr size_t kHeaderBytes = 32, kRowBytes = 12;
constexpr uint32_t kMaxRows = 16384;
struct Entry { uint32_t missionId = 0, phase = 0, npcId = 0; };
inline auto Key(const Entry& entry) { return std::make_tuple(entry.missionId, entry.phase, entry.npcId); }

inline bool Parse(const void* source, size_t size, std::vector<Entry>& result, std::string& error) {
    using mission_damage_gate::U32;
    result.clear(); error.clear();
    const auto* bytes = static_cast<const uint8_t*>(source);
    if (!bytes || size < kHeaderBytes || std::memcmp(bytes, "SDGORET1", 8) != 0 ||
        U32(bytes + 8) != 1 || U32(bytes + 12) != kHeaderBytes || U32(bytes + 16) != kRowBytes) {
        error = "invalid retreat header"; return false;
    }
    const uint32_t count = U32(bytes + 20), payload = U32(bytes + 24);
    if (count > kMaxRows || payload != count * kRowBytes || size != kHeaderBytes + payload ||
        mission_damage_gate::Checksum(bytes + kHeaderBytes, payload) != U32(bytes + 28)) {
        error = "invalid retreat length or checksum"; return false;
    }
    std::vector<Entry> parsed;
    parsed.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* row = bytes + kHeaderBytes + size_t(i) * kRowBytes;
        Entry entry{U32(row), U32(row + 4), U32(row + 8)};
        if (!entry.missionId || entry.phase < 1 || entry.phase > 8 || !entry.npcId ||
            (!parsed.empty() && !(Key(parsed.back()) < Key(entry)))) {
            error = "invalid or unordered retreat row"; return false;
        }
        parsed.push_back(entry);
    }
    result.swap(parsed); return true;
}
} // namespace sdgo::mission_retreat
