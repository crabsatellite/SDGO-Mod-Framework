#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sdgo/mission_retreat_format.h"

namespace sdgo::map_catalog::retreat_client {

constexpr std::uint16_t kAuthoritativeCleanupOpcode = 0x0905;
constexpr std::size_t kPacketIdsOffset = 15;
constexpr std::size_t kMaximumPacketIds = 10;
constexpr std::uint16_t kNativeRemovalMessage = 2;

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float scale = 0.0f;
};

struct Candidate {
    std::uint32_t listedId = 0;
    std::uint32_t npcId = 0;
    void* unit = nullptr;
    std::int32_t health = 0;
    Position position{};
    bool hasPosition = false;
};

struct DispatchUnit {
    std::uint32_t listedId = 0;
    std::uint32_t npcId = 0;
    void* unit = nullptr;
    const void* message = nullptr;
    Position position{};
    bool hasPosition = false;
};

struct DispatchContext {
    std::array<DispatchUnit, kMaximumPacketIds> units{};
    std::size_t count = 0;
    std::uint32_t mission = 0;
    std::uint32_t phase = 0;
};

inline bool IsAuthoritativeCleanup(std::uint16_t opcode, std::uint8_t count) noexcept {
    return opcode == kAuthoritativeCleanupOpcode && count <= kMaximumPacketIds;
}

inline const sdgo::mission_retreat::Entry* Find(
    const std::vector<sdgo::mission_retreat::Entry>& entries,
    std::uint32_t mission,
    std::uint32_t phase,
    std::uint32_t npcId) noexcept {
    const sdgo::mission_retreat::Entry key{mission, phase, npcId};
    const auto found = std::lower_bound(entries.begin(), entries.end(), key,
        [](const auto& left, const auto& right) {
            return sdgo::mission_retreat::Key(left) < sdgo::mission_retreat::Key(right);
        });
    return found != entries.end() && found->missionId == mission &&
        found->phase == phase && found->npcId == npcId ? &*found : nullptr;
}

inline bool Eligible(
    const std::vector<sdgo::mission_retreat::Entry>& entries,
    std::uint32_t mission,
    std::uint32_t phase,
    const Candidate& candidate) noexcept {
    return candidate.listedId != 0 && candidate.npcId != 0 &&
        candidate.unit != nullptr && candidate.health > 0 &&
        Find(entries, mission, phase, candidate.npcId) != nullptr;
}

inline bool Add(DispatchContext& context, const Candidate& candidate) noexcept {
    if (context.count >= context.units.size()) return false;
    for (std::size_t i = 0; i < context.count; ++i) {
        if (context.units[i].listedId == candidate.listedId ||
            context.units[i].unit == candidate.unit) return false;
    }
    context.units[context.count++] = {
        candidate.listedId, candidate.npcId, candidate.unit, nullptr,
        candidate.position, candidate.hasPosition};
    return true;
}

inline bool ArmPresentation(
    DispatchContext* context,
    const void* unit,
    const void* message,
    std::uint16_t messageType) noexcept {
    if (!context || !unit || !message || messageType != kNativeRemovalMessage) return false;
    for (std::size_t i = 0; i < context->count; ++i) {
        auto& entry = context->units[i];
        if (entry.unit != unit) continue;
        if (!entry.message) entry.message = message;
        return entry.message == message;
    }
    return false;
}

}  // namespace sdgo::map_catalog::retreat_client
