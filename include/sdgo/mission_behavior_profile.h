#pragma once

// Owner-generated mission behavior data. No original mission tables are bundled
// with the framework. Public runtime code consumes this bounded, checked delta.
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sdgo::mission_behavior {
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kBindingsGeneration = 1;
constexpr std::uint32_t kBossBindings = 1;
constexpr std::uint32_t kNpcOverrides = 2;
constexpr std::uint32_t kMaxOverrides = 4096;
constexpr std::uint32_t kChangeNpc = 1u << 0;
constexpr std::uint32_t kChangeX = 1u << 1;
constexpr std::uint32_t kChangeY = 1u << 2;
constexpr std::uint32_t kChangeZ = 1u << 3;
constexpr std::uint32_t kChangeDelay = 1u << 4;
constexpr std::uint32_t kChangeLimit = 1u << 5;
constexpr std::uint32_t kAllowedMask = 63;

enum class Profile { Oc, Gco };
inline bool ParseProfile(const std::string& value, Profile& out) {
    if (value == "oc") { out = Profile::Oc; return true; }
    if (value == "gco") { out = Profile::Gco; return true; }
    return false;
}

#pragma pack(push, 1)
struct Header {
    char magic[8];                 // SDGOMBP1
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t flags;
    std::uint32_t entry_bytes;
    std::uint32_t entry_count;
    std::uint32_t payload_bytes;
    std::uint32_t payload_crc32;
    std::uint32_t bindings_generation;
    std::uint8_t source_digest[32];
    std::uint8_t baseline_digest[32];
    char label[24];
};
struct NpcRow {
    std::uint32_t id, mission, difficulty, part, npc;
    std::int32_t x, y, z;
    std::uint32_t after_time, abs_time, manual, party, create_limit, zone;
};
struct NpcOverride {
    std::uint32_t mask;
    NpcRow baseline;
    NpcRow oc;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 128);
static_assert(sizeof(NpcRow) == 56);
static_assert(sizeof(NpcOverride) == 116);

struct Bundle {
    Header header{};
    std::vector<NpcOverride> entries;
};

inline std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = ~0u;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
inline bool ValidRow(const NpcRow& row) {
    return row.id && row.mission && row.npc && row.difficulty <= 255 &&
        row.part <= 255 && row.party <= 255 && row.after_time <= 65535 &&
        row.abs_time <= 65535 && row.create_limit <= 255 && row.zone <= 255 &&
        row.x >= -10000000 && row.x <= 10000000 &&
        row.y >= -10000000 && row.y <= 10000000 &&
        row.z >= -10000000 && row.z <= 10000000;
}
inline bool SameRow(const NpcRow& a, const NpcRow& b) {
    return a.id == b.id && a.mission == b.mission &&
        a.difficulty == b.difficulty && a.part == b.part && a.npc == b.npc &&
        a.x == b.x && a.y == b.y && a.z == b.z &&
        a.after_time == b.after_time && a.abs_time == b.abs_time &&
        a.manual == b.manual && a.party == b.party &&
        a.create_limit == b.create_limit && a.zone == b.zone;
}
inline bool ValidEntry(const NpcOverride& entry) {
    if (!entry.mask || (entry.mask & ~kAllowedMask) ||
        !ValidRow(entry.baseline) || !ValidRow(entry.oc)) return false;
    auto changed = entry.baseline;
    if (entry.mask & kChangeNpc) changed.npc = entry.oc.npc;
    if (entry.mask & kChangeX) changed.x = entry.oc.x;
    if (entry.mask & kChangeY) changed.y = entry.oc.y;
    if (entry.mask & kChangeZ) changed.z = entry.oc.z;
    if (entry.mask & kChangeDelay) changed.after_time = entry.oc.after_time;
    if (entry.mask & kChangeLimit) changed.create_limit = entry.oc.create_limit;
    std::uint32_t actual = 0;
    if (entry.baseline.npc != entry.oc.npc) actual |= kChangeNpc;
    if (entry.baseline.x != entry.oc.x) actual |= kChangeX;
    if (entry.baseline.y != entry.oc.y) actual |= kChangeY;
    if (entry.baseline.z != entry.oc.z) actual |= kChangeZ;
    if (entry.baseline.after_time != entry.oc.after_time) actual |= kChangeDelay;
    if (entry.baseline.create_limit != entry.oc.create_limit) actual |= kChangeLimit;
    return SameRow(changed, entry.oc) && actual == entry.mask;
}
inline bool ParseBundle(const std::uint8_t* data, std::size_t size,
                        Bundle& out, std::string& error) {
    auto fail = [&](const char* reason) { error = reason; return false; };
    if (!data || size < sizeof(Header)) return fail("profile header missing");
    Header header{};
    std::memcpy(&header, data, sizeof(header));
    if (std::memcmp(header.magic, "SDGOMBP1", 8) || header.version != kVersion ||
        header.header_bytes != sizeof(Header) ||
        header.entry_bytes != sizeof(NpcOverride) ||
        header.bindings_generation != kBindingsGeneration ||
        !(header.flags & kBossBindings) ||
        (header.flags & ~(kBossBindings | kNpcOverrides)) ||
        !header.entry_count || header.entry_count > kMaxOverrides ||
        header.payload_bytes != header.entry_count * sizeof(NpcOverride) ||
        size != sizeof(Header) + header.payload_bytes ||
        bool(header.flags & kNpcOverrides) != bool(header.entry_count))
        return fail("profile format or generation mismatch");
    const auto* payload = data + sizeof(Header);
    if (Crc32(payload, header.payload_bytes) != header.payload_crc32)
        return fail("profile payload checksum mismatch");
    Bundle candidate;
    candidate.header = header;
    candidate.entries.resize(header.entry_count);
    if (header.payload_bytes)
        std::memcpy(candidate.entries.data(), payload, header.payload_bytes);
    std::uint32_t previous = 0;
    for (const auto& entry : candidate.entries) {
        if (entry.baseline.id <= previous || !ValidEntry(entry))
            return fail("profile rows are invalid, duplicated or unsupported");
        previous = entry.baseline.id;
    }
    out = std::move(candidate);
    error.clear();
    return true;
}
inline bool LoadProfileFile(const std::filesystem::path& path,
                            Bundle& out, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { error = "owner task profile missing"; return false; }
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) >
        sizeof(Header) + kMaxOverrides * sizeof(NpcOverride)) {
        error = "profile size exceeds limit"; return false;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), data.size())) {
        error = "profile read incomplete"; return false;
    }
    return ParseBundle(data.data(), data.size(), out, error);
}
inline const NpcOverride* FindOverride(const Bundle& bundle, std::uint32_t id) {
    const auto found = std::lower_bound(bundle.entries.begin(), bundle.entries.end(), id,
        [](const NpcOverride& entry, std::uint32_t value) { return entry.baseline.id < value; });
    return found != bundle.entries.end() && found->baseline.id == id ? &*found : nullptr;
}
inline bool ApplyOcOverride(const NpcOverride& entry, const NpcRow& live, NpcRow& out) {
    if (!SameRow(live, entry.baseline)) return false;
    out = entry.oc;
    return true;
}
} // namespace sdgo::mission_behavior
