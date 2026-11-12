#pragma once
#include <algorithm>
#include <cstdint>

namespace sdgo::map_boundary_contact {

// A fixed-width extension of the native map-contact carrier. Requests keep
// bit 31 clear, so an unextended host cannot mistake one for an NPC death.
// The native handler rejects the marked target because it is neither the
// authenticated source nor a host-controlled NPC. Ordinary IDs are untouched.
// Bit 30 is already used by ACE pilot identities; retain it as payload.
// Adapters also give a real exact native identity precedence over this marker,
// and the server refuses to emit a notice that would collide with its roster.
constexpr std::uint32_t kMarker = 0x20000000u;
constexpr std::uint32_t kAce = 0x40000000u;
constexpr std::uint32_t kNpc = 0x80000000u;
constexpr std::uint32_t kIdentityMask = 0x1fffffffu;
constexpr std::uint32_t kRequestPayloadMask = kIdentityMask | kAce;
constexpr std::uint32_t kMaximumHealth = 0x00ffffffu;

inline bool IsRequest(std::uint32_t source, std::uint32_t marked) noexcept {
    return source != 0 && source <= kIdentityMask &&
           (marked & ~kRequestPayloadMask) == kMarker;
}

inline std::uint32_t EncodeRequest(std::uint32_t source, std::uint32_t target) noexcept {
    if (!source || source > kIdentityMask || !target || (target & kMarker)) return 0;
    if (!(target & kNpc) && target != source) return 0;
    return kMarker | (target & kRequestPayloadMask);
}

// A sender may report its own contact. Only the room's real host may report
// a contact for an existing native NPC; resolution/admission stays server-side.
inline std::uint32_t ResolveRequest(std::uint32_t source, std::uint32_t host,
                                    std::uint32_t marked, std::uint32_t actualTarget) noexcept {
    // The native-unused final request DWORD carries the full target identity;
    // this distinguishes an NPC whose low ID equals the reporting human.
    if (!IsRequest(source, marked) || !actualTarget || EncodeRequest(source, actualTarget) != marked) return 0;
    if (actualTarget & kNpc) return source == host ? actualTarget : 0;
    return actualTarget == source ? source : 0;
}

inline bool IsNotice(std::uint32_t marked) noexcept {
    return (marked & kMarker) != 0 && (marked ^ kMarker) != 0;
}

inline std::uint32_t NoticeTarget(std::uint32_t marked) noexcept {
    return IsNotice(marked) ? marked ^ kMarker : 0;
}

inline bool EncodeHealth(std::uint32_t health, std::uint8_t bytes[3]) noexcept {
    if (!bytes || !health || health > kMaximumHealth) return false;
    bytes[0] = static_cast<std::uint8_t>(health);
    bytes[1] = static_cast<std::uint8_t>(health >> 8);
    bytes[2] = static_cast<std::uint8_t>(health >> 16);
    return true;
}

inline std::uint32_t DecodeHealth(const std::uint8_t bytes[3]) noexcept {
    if (!bytes) return 0;
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16);
}

// Boundary feedback is deliberately nonlethal. It spends no life, grants no
// kill/reward and cannot advance a mission goal by eliminating a unit.
inline std::uint32_t ShockHealth(std::uint32_t current, std::uint32_t maximum,
                                 std::uint32_t permille) noexcept {
    if (!current || !maximum || current > maximum || maximum > kMaximumHealth ||
        !permille || permille > 1000) return current;
    const auto cost = (std::max)(std::uint32_t(1), static_cast<std::uint32_t>(
        (std::uint64_t(maximum) * permille + 999) / 1000));
    return cost >= current ? 1 : current - cost;
}

} // namespace sdgo::map_boundary_contact
