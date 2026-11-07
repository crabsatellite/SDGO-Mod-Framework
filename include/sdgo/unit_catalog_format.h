#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sdgo::unit_catalog {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kUnitType = 2;
constexpr uint32_t kWeaponType = 10;
constexpr uint32_t kFunnelType = 11;
constexpr uint32_t kSkillType = 13;
constexpr uint32_t kAffiliationType = 20;
constexpr uint32_t kBlueprintType = 0x21;
constexpr uint32_t kItemCategoryType = 0x22;
constexpr uint32_t kUnitIndexType = 0x55;
constexpr size_t kUnitBytes = 4976;
constexpr size_t kWeaponBytes = 1871;
constexpr size_t kFunnelBytes = 732;
constexpr size_t kSkillBytes = 1132;
constexpr size_t kAffiliationBytes = 106;
constexpr uint32_t kAffiliationFirstKey = 125501;
constexpr size_t kBlueprintBytes = 1197;
constexpr size_t kItemCategoryBytes = 614;
constexpr size_t kUnitIndexBytes = 614;
constexpr uint32_t kUnitIndexFirstId = 0x375BE1;
constexpr size_t kUnitIndexRefOffset = 6;
constexpr size_t kUnitIndexBlueprintRefOffset = 10;
constexpr uint32_t kMaxId = 9999999;
constexpr uint32_t kMaxRecords = 4096;
constexpr uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;

constexpr size_t kUnitNameOffset = 5;
constexpr size_t kUnitNameBytes = 100;
constexpr size_t kUnitGradeOffset = 308;
constexpr size_t kUnitSubgradeOffset = 309;
constexpr size_t kUnitModelPathOffset = 612;
constexpr size_t kUnitModelPathBytes = 60;
constexpr size_t kUnitPreviewPathOffsets[] = {4654, 4754, 4854};
constexpr size_t kUnitPreviewPathBytes = 100;
constexpr size_t kUnitTransformOffset = 316;
constexpr size_t kUnitWeaponOffsets[] = {585, 589, 593, 597};
constexpr size_t kUnitSkillOffsets[] = {575, 579};
constexpr size_t kWeaponFunnelOffset = 1565;
constexpr size_t kSkillLinkedSkillOffset = 354;
constexpr size_t kItemCategoryNameOffset = 5;
constexpr size_t kItemCategoryNameBytes = 100;
constexpr size_t kItemCategoryRefOffset = 105;
constexpr uint32_t kItemCategoryClassDivisor = 100000;
constexpr uint32_t kBlueprintItemClass = 15;
constexpr size_t kBlueprintMaterialCountOffset = 105;
constexpr size_t kBlueprintOutputOffset = 229;
constexpr size_t kBlueprintMaterialOffset = 233;
constexpr size_t kBlueprintMaterialLevelOffset = 261;
constexpr size_t kBlueprintMaterialSlots = 7;
constexpr uint32_t kBlueprintMinimumMaterials = 3;
constexpr uint32_t kBlueprintMaximumMaterials = 7;
constexpr uint32_t kBlueprintMaximumMaterialLevel = 255;

inline uint32_t U32(const void* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline void Put32(void* bytes, uint32_t value) {
    std::memcpy(bytes, &value, sizeof(value));
}

inline uint32_t Hash32(const void* bytes, size_t size) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 16777619u;
    return value;
}

#pragma pack(push, 1)
struct Header {
    char magic[8];                  // SDGOUN1
    uint32_t version;
    uint32_t headerBytes;
    uint32_t count;
    uint32_t payloadBytes;
    uint32_t payloadHash;           // FNV-1a corruption check, not a signature
    uint32_t reserved;
};

struct EntryHeader {
    uint32_t kind;                  // 2 unit, 10 weapon, 11 funnel, 13 skill, 0x21 blueprint, 0x22 item, 0x55 index
    uint32_t id;
    uint32_t byteCount;
    uint32_t recordHash;            // FNV-1a over the following native record
    uint32_t flags;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 32);
static_assert(sizeof(EntryHeader) == 24);

inline size_t ExpectedBytes(uint32_t kind) {
    switch (kind) {
    case kUnitType: return kUnitBytes;
    case kWeaponType: return kWeaponBytes;
    case kFunnelType: return kFunnelBytes;
    case kSkillType: return kSkillBytes;
    case kAffiliationType: return kAffiliationBytes;
    case kBlueprintType: return kBlueprintBytes;
    case kItemCategoryType: return kItemCategoryBytes;
    case kUnitIndexType: return kUnitIndexBytes;
    default: return 0;
    }
}

struct Entry {
    uint32_t kind = 0;
    uint32_t id = 0;
    uint32_t recordHash = 0;
    std::vector<uint8_t> bytes;
};

class Catalog {
public:
    std::vector<Entry> records;

    bool Parse(const std::vector<uint8_t>& file, std::string& error) {
        records.clear();
        if (file.size() < sizeof(Header)) return Fail(error, "short unit catalog header");

        Header header{};
        std::memcpy(&header, file.data(), sizeof(header));
        if (std::memcmp(header.magic, "SDGOUN1", 8) != 0 ||
            header.version != kFormatVersion ||
            header.headerBytes != sizeof(Header) ||
            header.reserved != 0 ||
            header.count == 0 || header.count > kMaxRecords ||
            header.payloadBytes > kMaxPayloadBytes) {
            return Fail(error, "unsupported unit catalog header");
        }
        if (file.size() != static_cast<size_t>(header.headerBytes) + header.payloadBytes ||
            Hash32(file.data() + header.headerBytes, header.payloadBytes) != header.payloadHash) {
            return Fail(error, "unit catalog length or payload checksum mismatch");
        }

        std::vector<Entry> parsed;
        parsed.reserve(header.count);
        size_t cursor = header.headerBytes;
        uint32_t previousId = 0;
        for (uint32_t index = 0; index < header.count; ++index) {
            if (cursor > file.size() || file.size() - cursor < sizeof(EntryHeader))
                return Fail(error, "short unit catalog entry header");
            EntryHeader native{};
            std::memcpy(&native, file.data() + cursor, sizeof(native));
            cursor += sizeof(native);

            const size_t expected = ExpectedBytes(native.kind);
            if (!expected || native.byteCount != expected ||
                !native.id || native.id > kMaxId || native.id <= previousId ||
                native.flags != 0 || native.reserved != 0 ||
                cursor > file.size() || file.size() - cursor < expected) {
                return Fail(error, "invalid, duplicate or unsorted unit catalog entry");
            }

            Entry entry{};
            entry.kind = native.kind;
            entry.id = native.id;
            entry.recordHash = native.recordHash;
            entry.bytes.assign(file.begin() + cursor, file.begin() + cursor + expected);
            cursor += expected;
            previousId = entry.id;

            if (Hash32(entry.bytes.data(), entry.bytes.size()) != entry.recordHash ||
                U32(entry.bytes.data()) != entry.id || entry.bytes[4] != 0) {
                return Fail(error, "native record identity, security byte or checksum mismatch");
            }
            if (!ValidateRecord(entry, error)) {
                records.clear();
                return false;
            }
            parsed.push_back(std::move(entry));
        }
        if (cursor != file.size()) return Fail(error, "unit catalog has trailing payload bytes");
        records.swap(parsed);
        return true;
    }

    bool Load(const std::wstring& path, std::string& error) {
        FILE* stream = nullptr;
        if (_wfopen_s(&stream, path.c_str(), L"rb") || !stream)
            return Fail(error, "catalog.bin is missing or unreadable");
        if (std::fseek(stream, 0, SEEK_END)) {
            std::fclose(stream);
            return Fail(error, "cannot size catalog.bin");
        }
        const long length = std::ftell(stream);
        if (length < static_cast<long>(sizeof(Header)) ||
            length > static_cast<long>(sizeof(Header) + kMaxPayloadBytes)) {
            std::fclose(stream);
            return Fail(error, "catalog.bin size is outside the supported bound");
        }
        std::rewind(stream);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        const size_t read = std::fread(bytes.data(), 1, bytes.size(), stream);
        std::fclose(stream);
        return read == bytes.size() ? Parse(bytes, error) : Fail(error, "short catalog.bin read");
    }

    size_t Find(uint32_t id) const {
        const auto found = std::lower_bound(records.begin(), records.end(), id,
            [](const Entry& entry, uint32_t value) { return entry.id < value; });
        return found != records.end() && found->id == id
            ? static_cast<size_t>(found - records.begin()) : records.size();
    }

    bool Contains(uint32_t id, uint32_t kind) const {
        const size_t index = Find(id);
        return index != records.size() && records[index].kind == kind;
    }

private:
    static bool ValidateUtf16Path(const uint8_t* bytes, size_t size, const char* prefix, bool required) {
        if (size < 2 || (size & 1)) return false;
        size_t characters = 0;
        bool terminated = false;
        size_t segmentLength = 0;
        bool segmentDotsOnly = true;
        for (; characters < size / 2; ++characters) {
            const uint16_t c = static_cast<uint16_t>(bytes[characters * 2]) |
                               (static_cast<uint16_t>(bytes[characters * 2 + 1]) << 8);
            if (!c) {
                terminated = true;
                break;
            }
            if (c < 32 || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
                c == L'<' || c == L'>' || c == L'|' || c == L'/') return false;
            if (c == L'\\') {
                if (!segmentLength || (segmentDotsOnly && segmentLength <= 2)) return false;
                segmentLength = 0;
                segmentDotsOnly = true;
            } else {
                ++segmentLength;
                segmentDotsOnly = segmentDotsOnly && c == L'.';
            }
        }
        if (!terminated || (required && characters == 0)) return false;
        if (characters == 0) return true;
        if (!segmentLength || (segmentDotsOnly && segmentLength <= 2)) return false;

        const size_t prefixLength = std::strlen(prefix);
        if (characters < prefixLength) return false;
        for (size_t i = 0; i < prefixLength; ++i) {
            const uint16_t c = static_cast<uint16_t>(bytes[i * 2]) |
                               (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8);
            char narrowed = c <= 0x7f ? static_cast<char>(c) : '\0';
            if (narrowed >= 'A' && narrowed <= 'Z') narrowed = static_cast<char>(narrowed - 'A' + 'a');
            char expected = prefix[i];
            if (expected >= 'A' && expected <= 'Z') expected = static_cast<char>(expected - 'A' + 'a');
            if (narrowed != expected) return false;
        }
        return true;
    }

    static bool ValidateUtf16Text(const uint8_t* bytes, size_t size) {
        if (size < 2 || (size & 1)) return false;
        for (size_t i = 0; i < size; i += 2) {
            const uint16_t c = static_cast<uint16_t>(bytes[i]) |
                               (static_cast<uint16_t>(bytes[i + 1]) << 8);
            if (!c) return i != 0;
            if (c < 32) return false;
        }
        return false;
    }

    static bool ValidateRecord(const Entry& entry, std::string& error) {
        if (entry.kind == kUnitType) {
            if (!ValidateUtf16Path(entry.bytes.data() + kUnitModelPathOffset,
                                   kUnitModelPathBytes, "mdrs\\", true)) {
                return Fail(error, "unit model path is missing, unsafe or outside mdrs");
            }
            for (const size_t offset : kUnitPreviewPathOffsets) {
                if (!ValidateUtf16Path(entry.bytes.data() + offset,
                                       kUnitPreviewPathBytes, "sprs\\", false)) {
                    return Fail(error, "unit preview path is unsafe or outside sprs");
                }
            }
        } else if (entry.kind == kSkillType) {
            // native-type13-1132-v1 preserves its source record and inserts
            // these exact target-layout gaps. Other native layouts need a
            // separate profile rather than treating arbitrary bytes as this one.
            if (!ValidateUtf16Text(entry.bytes.data()+5,100) || entry.bytes[1131]>3 ||
                !std::all_of(entry.bytes.begin()+105,entry.bytes.begin()+155,
                             [](uint8_t value){return value==0;}) ||
                !std::all_of(entry.bytes.begin()+931,entry.bytes.begin()+1131,
                             [](uint8_t value){return value==0;}))
                return Fail(error,"skill record does not match native-type13-1132-v1");
        } else if (entry.kind == kAffiliationType) {
            // Affiliation dictionary rows carry a fixed computed global key
            // (first key plus the influence number) and a localized name.
            if (entry.id < kAffiliationFirstKey || entry.id > kAffiliationFirstKey + 1000 ||
                !ValidateUtf16Text(entry.bytes.data() + 5, 100))
                return Fail(error, "affiliation key or name is invalid");
        } else if (entry.kind == kItemCategoryType) {
            if (!ValidateUtf16Text(entry.bytes.data() + kItemCategoryNameOffset,
                                   kItemCategoryNameBytes) ||
                !U32(entry.bytes.data() + kItemCategoryRefOffset)) {
                return Fail(error, "item category name or referenced unit is invalid");
            }
        } else if (entry.kind == kBlueprintType) {
            const uint32_t count =
                U32(entry.bytes.data() + kBlueprintMaterialCountOffset);
            const uint32_t output = U32(entry.bytes.data() + kBlueprintOutputOffset);
            if (count < kBlueprintMinimumMaterials ||
                count > kBlueprintMaximumMaterials ||
                !output || output > kMaxId) {
                return Fail(error, "blueprint material count or output unit is invalid");
            }
            uint32_t materials[kBlueprintMaterialSlots] = {};
            for (size_t slot = 0; slot < kBlueprintMaterialSlots; ++slot) {
                const uint32_t material =
                    U32(entry.bytes.data() + kBlueprintMaterialOffset + slot * sizeof(uint32_t));
                const uint32_t level =
                    U32(entry.bytes.data() + kBlueprintMaterialLevelOffset +
                        slot * sizeof(uint32_t));
                materials[slot] = material;
                if (level > kBlueprintMaximumMaterialLevel ||
                    (slot < count && (!material || material > kMaxId || !level)) ||
                    (slot >= count && material)) {
                    return Fail(error, "blueprint material or level slot is invalid");
                }
                if (slot < count && material == output) {
                    return Fail(error, "blueprint output cannot also be a material");
                }
                for (size_t previous = 0; previous < slot && slot < count; ++previous) {
                    if (materials[previous] == material) {
                        return Fail(error, "blueprint materials must be unique");
                    }
                }
            }
        } else if (entry.kind == kUnitIndexType) {
            if (entry.id<kUnitIndexFirstId || entry.bytes[5]>1 ||
                    !U32(entry.bytes.data()+kUnitIndexRefOffset) ||
                    !ValidateUtf16Path(entry.bytes.data()+14,100,"sprs\\",true) ||
                    !ValidateUtf16Path(entry.bytes.data()+114,100,"sprs\\",true))
                return Fail(error,"unit index reference, enablement or native sprite is invalid");
        }
        return true;
    }

    static bool Fail(std::string& error, const char* message) {
        error = message;
        return false;
    }
};

} // namespace sdgo::unit_catalog
