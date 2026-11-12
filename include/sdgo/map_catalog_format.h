#pragma once

// catalog.bin of the map_catalog Mod: native grid records produced by the map
// package compiler (owner/maps). Same container layout as the unit catalog with
// its own magic, so a unit catalog can never be loaded as a map catalog.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sdgo::map_catalog {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kBlockObjectType = 0x0E;  // gdrs\0008 named destroyable object
constexpr uint32_t kMapType = 0x0F;          // gdrs\0254 map row
constexpr uint32_t kNpcTemplateType = 0x29;  // NPC template
constexpr uint32_t kGameModeType = 0x2A;     // mission GameMode
constexpr uint32_t kClearType = 0x2F;        // mission Clear mirror
constexpr uint32_t kCatalogueType = 0x1D;    // mission catalogue group
constexpr uint32_t kRouteType = 0x1F;        // mission route (list entry)
constexpr uint32_t kObjectiveType = 0x20;    // mission objective
constexpr uint32_t kSpeechType = 0x36;       // operator speech
constexpr uint32_t kSpawnType = 0x2E;        // mission spawn mirror (walked by the room start check)
constexpr uint32_t kTriggerType = 0x6B;      // mission trigger objects
constexpr uint32_t kUnitType = 0x02;         // client-only unit copy (NPC label / weapons)
constexpr uint32_t kWeaponType = 0x0A;       // client-only weapon copy (NPC damage multiplier)
constexpr uint32_t kSectionClearPolicy = 0x01;
constexpr uint32_t kSectionClearMask = 0x0000FF00;

inline bool ValidFlags(uint32_t kind, uint32_t flags) {
    return flags == 0 || (kind == kGameModeType && (flags & kSectionClearPolicy) &&
                          !(flags & ~(kSectionClearPolicy | kSectionClearMask)));
}

constexpr size_t kMapBytes = 0x34C;
constexpr uint32_t kMapFirstId = 100001;
constexpr uint32_t kMapIdLimit = 101000;     // terrain loader treats >= 101000 as GameMode IDs
constexpr uint32_t kMapNoLimit = 255;        // room map byte
constexpr size_t kMapNoOffset = 5;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

inline uint32_t U32(const void* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline uint32_t Hash32(const void* bytes, size_t size) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 16777619u;
    return value;
}

#pragma pack(push, 1)
struct Header {
    char magic[8];                  // SDGOMP1
    uint32_t version;
    uint32_t headerBytes;
    uint32_t count;
    uint32_t payloadBytes;
    uint32_t payloadHash;           // FNV-1a corruption check, not a signature
    uint32_t reserved;
};

struct EntryHeader {
    uint32_t kind;
    uint32_t id;
    uint32_t byteCount;
    uint32_t recordHash;
    uint32_t flags;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 32);
static_assert(sizeof(EntryHeader) == 24);

inline size_t ExpectedBytes(uint32_t kind) {
    switch (kind) {
    case kBlockObjectType: return 513;
    case kMapType: return kMapBytes;
    case kNpcTemplateType: return 598;
    case kGameModeType: return 1090;
    case kClearType: return 391;
    case kCatalogueType: return 1309;
    case kRouteType: return 34;
    case kObjectiveType: return 634;
    case kSpeechType: return 506;
    case kSpawnType: return 152;
    case kTriggerType: return 395;
    case kUnitType: return 4976;
    case kWeaponType: return 1871;
    default: return 0;
    }
}

struct Entry {
    uint32_t kind = 0;
    uint32_t id = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> bytes;
};

class Catalog {
public:
    std::vector<Entry> records;

    size_t Count(uint32_t kind) const {
        size_t count = 0;
        for (const auto& entry : records) count += entry.kind == kind;
        return count;
    }

    bool Parse(const std::vector<uint8_t>& file, std::string& error) {
        records.clear();
        if (file.size() < sizeof(Header)) return Fail(error, "short map catalog header");
        Header header{};
        std::memcpy(&header, file.data(), sizeof(header));
        if (std::memcmp(header.magic, "SDGOMP1", 8) != 0 || header.version != kFormatVersion ||
            header.headerBytes != sizeof(Header) || header.reserved != 0 || header.count == 0 ||
            header.count > kMaxRecords || header.payloadBytes > kMaxPayloadBytes) {
            return Fail(error, "unsupported map catalog header");
        }
        if (file.size() != static_cast<size_t>(header.headerBytes) + header.payloadBytes ||
            Hash32(file.data() + header.headerBytes, header.payloadBytes) != header.payloadHash) {
            return Fail(error, "map catalog length or payload checksum mismatch");
        }
        size_t cursor = header.headerBytes;
        uint32_t lastMapId = 0;
        for (uint32_t index = 0; index < header.count; ++index) {
            if (file.size() - cursor < sizeof(EntryHeader)) return Fail(error, "truncated map catalog entry");
            EntryHeader entry{};
            std::memcpy(&entry, file.data() + cursor, sizeof(entry));
            cursor += sizeof(entry);
            const size_t expected = ExpectedBytes(entry.kind);
            if (!expected || entry.byteCount != expected || !ValidFlags(entry.kind, entry.flags) || entry.reserved != 0 ||
                file.size() - cursor < entry.byteCount) {
                return Fail(error, "unsupported map catalog record");
            }
            const uint8_t* raw = file.data() + cursor;
            if (Hash32(raw, entry.byteCount) != entry.recordHash || U32(raw) != entry.id || entry.id == 0)
                return Fail(error, "map catalog record identity or checksum mismatch");
            if (entry.kind == kMapType) {
                const uint32_t mapNo = U32(raw + kMapNoOffset);
                if (entry.id < kMapFirstId || entry.id >= kMapIdLimit || mapNo != entry.id - kMapFirstId ||
                    mapNo > kMapNoLimit || (lastMapId && entry.id != lastMapId + 1)) {
                    return Fail(error, "map rows must be contiguous MapNo records up to 255");
                }
                lastMapId = entry.id;
            }
            for (const auto& existing : records)
                if (existing.id == entry.id) return Fail(error, "duplicate map catalog record");
            records.push_back(Entry{entry.kind, entry.id, entry.flags, std::vector<uint8_t>(raw, raw + entry.byteCount)});
            cursor += entry.byteCount;
        }
        if (cursor != file.size()) return Fail(error, "trailing bytes in map catalog");
        return true;
    }

    bool Load(const std::wstring& path, std::string& error) {
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, path.c_str(), L"rb") != 0 || !stream) return Fail(error, "map catalog.bin is missing");
        std::vector<uint8_t> file;
        uint8_t buffer[65536];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
            file.insert(file.end(), buffer, buffer + read);
            if (file.size() > sizeof(Header) + kMaxPayloadBytes) {
                std::fclose(stream);
                return Fail(error, "map catalog.bin is too large");
            }
        }
        std::fclose(stream);
        return Parse(file, error);
    }

private:
    static bool Fail(std::string& error, const char* message) {
        error = message;
        return false;
    }
};

} // namespace sdgo::map_catalog
