#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sdgo::map_boundary {

constexpr char kMagic[8] = {'S', 'D', 'G', 'O', 'B', 'N', 'D', '1'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 32;
constexpr uint32_t kRowBytes = 44;
constexpr uint32_t kMaxRows = 16384;
constexpr uint32_t kMaxPhases = 8;

constexpr uint32_t kAreaEnabled = 1u << 0;
constexpr uint32_t kWarningEnabled = 1u << 1;
constexpr uint32_t kDamageEnabled = 1u << 2;
constexpr uint32_t kKnownFlags = kAreaEnabled | kWarningEnabled | kDamageEnabled;

constexpr uint32_t kEdgeLeft = 1u << 0;
constexpr uint32_t kEdgeTop = 1u << 1;
constexpr uint32_t kEdgeRight = 1u << 2;
constexpr uint32_t kEdgeBottom = 1u << 3;
constexpr uint32_t kKnownEdges = kEdgeLeft | kEdgeTop | kEdgeRight | kEdgeBottom;

struct Entry {
    uint32_t missionId = 0;
    uint32_t phase = 0;
    uint32_t flags = 0;
    uint32_t warningSpeechId = 0;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    uint32_t damageEdgeMask = 0;
    uint32_t damagePermille = 0;
    uint32_t damageCooldownMs = 0;
};

inline uint32_t ReadU32(const uint8_t* raw) {
    uint32_t value = 0;
    std::memcpy(&value, raw, sizeof(value));
    return value;
}

inline float ReadF32(const uint8_t* raw) {
    float value = 0.0f;
    std::memcpy(&value, raw, sizeof(value));
    return value;
}

inline uint32_t Fnv1a(const uint8_t* raw, size_t size) {
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < size; ++index) value = (value ^ raw[index]) * 16777619u;
    return value;
}

inline bool KeyLess(const Entry& left, const Entry& right) {
    return left.missionId < right.missionId ||
           (left.missionId == right.missionId && left.phase < right.phase);
}

inline bool Valid(const Entry& entry) {
    if (!entry.missionId || !entry.phase || entry.phase > kMaxPhases || (entry.flags & ~kKnownFlags)) return false;
    if (entry.flags & kAreaEnabled) {
        if (!std::isfinite(entry.left) || !std::isfinite(entry.top) || !std::isfinite(entry.right) ||
            !std::isfinite(entry.bottom) || !(entry.left < entry.right) || !(entry.bottom < entry.top)) return false;
    } else if ((entry.flags & (kWarningEnabled | kDamageEnabled)) != 0) {
        return false;
    }
    if ((entry.flags & kWarningEnabled) != 0) {
        if (!entry.warningSpeechId) return false;
    } else if (entry.warningSpeechId) {
        return false;
    }
    if ((entry.flags & kDamageEnabled) != 0) {
        if (!entry.damageEdgeMask || (entry.damageEdgeMask & ~kKnownEdges) || !entry.damagePermille ||
            entry.damagePermille >= 1000 || entry.damageCooldownMs < 250 || entry.damageCooldownMs > 60000) return false;
    } else if (entry.damageEdgeMask || entry.damagePermille || entry.damageCooldownMs) {
        return false;
    }
    return true;
}

inline bool Parse(const uint8_t* raw, size_t size, std::vector<Entry>& entries, std::string& error) {
    entries.clear();
    error.clear();
    if (!raw || size < kHeaderBytes) {
        error = "boundary declaration has a short header";
        return false;
    }
    if (std::memcmp(raw, kMagic, sizeof(kMagic)) != 0 || ReadU32(raw + 8) != kVersion ||
        ReadU32(raw + 12) != kHeaderBytes || ReadU32(raw + 16) != kRowBytes) {
        error = "boundary declaration has an unsupported header";
        return false;
    }
    const uint32_t count = ReadU32(raw + 20);
    const uint32_t payloadBytes = ReadU32(raw + 24);
    const uint32_t checksum = ReadU32(raw + 28);
    if (count > kMaxRows || payloadBytes != count * kRowBytes || size != kHeaderBytes + payloadBytes) {
        error = "boundary declaration length is inconsistent";
        return false;
    }
    const uint8_t* payload = raw + kHeaderBytes;
    if (Fnv1a(payload, payloadBytes) != checksum) {
        error = "boundary declaration checksum does not match";
        return false;
    }
    entries.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t* row = payload + index * kRowBytes;
        Entry entry{};
        entry.missionId = ReadU32(row + 0);
        entry.phase = ReadU32(row + 4);
        entry.flags = ReadU32(row + 8);
        entry.warningSpeechId = ReadU32(row + 12);
        entry.left = ReadF32(row + 16);
        entry.top = ReadF32(row + 20);
        entry.right = ReadF32(row + 24);
        entry.bottom = ReadF32(row + 28);
        entry.damageEdgeMask = ReadU32(row + 32);
        entry.damagePermille = ReadU32(row + 36);
        entry.damageCooldownMs = ReadU32(row + 40);
        if (!Valid(entry) || (!entries.empty() && !KeyLess(entries.back(), entry))) {
            error = "boundary declaration contains an invalid or unsorted row";
            entries.clear();
            return false;
        }
        entries.push_back(entry);
    }
    return true;
}

inline const Entry* Find(const std::vector<Entry>& entries, uint32_t missionId, uint32_t phase) {
    Entry key{};
    key.missionId = missionId;
    key.phase = phase;
    const auto found = std::lower_bound(entries.begin(), entries.end(), key, KeyLess);
    return found != entries.end() && found->missionId == missionId && found->phase == phase ? &*found : nullptr;
}

inline uint32_t ContactEdges(const Entry& entry, float x, float z, float epsilon = 0.5f) {
    if ((entry.flags & kAreaEnabled) == 0 || !std::isfinite(x) || !std::isfinite(z) || epsilon < 0.0f) return 0;
    uint32_t edges = 0;
    if (x <= entry.left + epsilon) edges |= kEdgeLeft;
    if (z >= entry.top - epsilon) edges |= kEdgeTop;
    if (x >= entry.right - epsilon) edges |= kEdgeRight;
    if (z <= entry.bottom + epsilon) edges |= kEdgeBottom;
    return edges;
}

}  // namespace sdgo::map_boundary
