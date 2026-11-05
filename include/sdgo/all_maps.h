#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sdgo::all_maps {

constexpr std::size_t kMapDomainSize = 256;

struct Entry {
    uint32_t map = 0;
    std::array<uint8_t, 8> modes{};
};

inline bool PvpMode(uint32_t mode) {
    return mode == 1 || mode == 3 || mode == 6 || mode == 7 || mode == 9;
}

inline bool PvpEntry(const Entry& entry) {
    const bool hasPvpFlag =
        entry.modes[0] || entry.modes[1] || entry.modes[2] ||
        entry.modes[3] || entry.modes[5];
    return entry.map > 0 && entry.map < kMapDomainSize && hasPvpFlag;
}

inline std::vector<uint32_t> Pool(const std::vector<Entry>& entries) {
    std::array<bool, kMapDomainSize> included{};
    std::vector<uint32_t> pool;
    for (const auto& entry : entries) {
        if (PvpEntry(entry) && !included[entry.map]) {
            included[entry.map] = true;
            pool.push_back(entry.map);
        }
    }
    return pool;
}

inline Entry ServerEntry(const uint8_t* row) {
    Entry entry{};
    if (!row) {
        return entry;
    }
    std::memcpy(&entry.map, row + 8, sizeof(entry.map));
    std::memcpy(entry.modes.data(), row + 0xC, entry.modes.size());
    return entry;
}

}  // namespace sdgo::all_maps

#define SDGO_ALL_MAPS_SNAPSHOT_EXPORT "SdgoAllMapsGetSnapshotV1"

struct SdgoAllMapsSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t pool_count;
    uint32_t draws;
    uint32_t last_map;
    uint32_t mode;
    uint32_t selection;
    uint32_t error;
    uint32_t maps[sdgo::all_maps::kMapDomainSize];
};

using SdgoAllMapsGetSnapshotV1Fn =
    int(__cdecl*)(SdgoAllMapsSnapshotV1* output);
