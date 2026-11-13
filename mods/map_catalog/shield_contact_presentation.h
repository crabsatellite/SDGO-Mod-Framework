#pragma once

#include "sdgo/map_boundary_format.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sdgo::map_catalog::shield_contact {

// Preserve only the two stock terrain-contact effects emitted by the native
// wall clamp. Ordinary mech hurt presentation comes from the movement FSM;
// duplicating a body particle or guessing a sound asset makes the contact look
// like a special attack.
inline constexpr char kLuaFormat[] =
    "CreateEffectA('0506.zfx',vector3(%.3f,%.3f,%.3f),vector3(1.5,1.5,1.5),"
    "vector3(%.1f,0,%.1f),vector3(0,1,0))\n"
    "CreateEffectA('0087.zfx',vector3(%.3f,%.3f,%.3f),vector3(1,1,1),"
    "vector3(%.1f,0,%.1f),vector3(0,1,0))";

inline constexpr std::size_t kUnitHealth = 0x38;
inline constexpr std::size_t kUnitMovementFsm = 0x9C8;
inline constexpr std::size_t kUnitHurtState = 0x9D8;
inline constexpr std::size_t kUnitInvulnerable = 0xEB4;
inline constexpr std::size_t kFsmCurrentState = 0x04;
inline constexpr std::size_t kFsmPendingState = 0x08;
inline constexpr std::size_t kMovementHurtState = 0xE8;
inline constexpr std::size_t kMovementProtectedHurtState = 0xFC;
inline constexpr std::size_t kMovementDownState = 0x110;
inline constexpr std::size_t kMovementDirectionX = 0x1B8;
inline constexpr std::size_t kMovementDirectionY = 0x1BC;
inline constexpr std::size_t kMovementDirectionZ = 0x1C0;
inline constexpr std::size_t kMovementHurtStyle = 0x1C4;
inline constexpr std::size_t kMovementMagnitude = 0x1C8;
inline constexpr std::size_t kMovementOwner = 0x21C;
inline constexpr std::uint8_t kOrdinaryHurtStyle = 1;
inline constexpr float kBoundaryImpulse = 0.0f;

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Plan {
    Position wall{};
    float directionX = 0.0f;
    float directionZ = 0.0f;
};

template<class T>
inline T ReadField(const std::uint8_t* base, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template<class T>
inline void WriteField(std::uint8_t* base, std::size_t offset, const T& value) noexcept {
    std::memcpy(base + offset, &value, sizeof(value));
}

inline bool IsMovementState(const std::uint8_t* movement, const std::uint8_t* state) noexcept {
    if (!movement || !state) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(movement);
    const auto address = reinterpret_cast<std::uintptr_t>(state);
    if (address < base + 0x20 || address > base + 0x1A0) return false;
    return ((address - base - 0x20) % 0x14) == 0;
}

// Reproduce only the presentation half of the shipped ordinary-hit consumer.
// The caller has already admitted and applied the authoritative HP receipt.
// Queueing movement+E8 lets the game run hurt_melee Entry/Update/Exit; replaying
// message 0x0C here would subtract HP a second time.
template<class QueueState>
inline bool QueueNativeHurt(std::uint8_t* unit, float directionX, float directionZ,
                            QueueState queueState) noexcept {
    if (!unit || !std::isfinite(directionX) || !std::isfinite(directionZ) ||
        (directionX == 0.0f && directionZ == 0.0f)) return false;
    const std::int32_t health = ReadField<std::int32_t>(unit, kUnitHealth);
    const std::uint32_t hurtState = ReadField<std::uint32_t>(unit, kUnitHurtState);
    const std::uint8_t invulnerable = ReadField<std::uint8_t>(unit, kUnitInvulnerable);
    std::uint8_t* movement = ReadField<std::uint8_t*>(unit, kUnitMovementFsm);
    if (health <= 0 || invulnerable || hurtState == 2 || !movement ||
        ReadField<std::uint8_t*>(movement, kMovementOwner) != unit) return false;

    std::uint8_t* current = ReadField<std::uint8_t*>(movement, kFsmCurrentState);
    std::uint8_t* pending = ReadField<std::uint8_t*>(movement, kFsmPendingState);
    if (!IsMovementState(movement, current) ||
        (pending && !IsMovementState(movement, pending))) return false;
    auto is = [movement](const std::uint8_t* state, std::size_t offset) {
        return state == movement + offset;
    };
    if (is(current, kMovementProtectedHurtState) || is(pending, kMovementProtectedHurtState) ||
        is(current, kMovementDownState) || is(pending, kMovementDownState) ||
        (is(current, kMovementHurtState) && is(pending, kMovementHurtState))) return false;

    const float oldX = ReadField<float>(movement, kMovementDirectionX);
    const float oldY = ReadField<float>(movement, kMovementDirectionY);
    const float oldZ = ReadField<float>(movement, kMovementDirectionZ);
    const std::uint8_t oldStyle = ReadField<std::uint8_t>(movement, kMovementHurtStyle);
    const float oldMagnitude = ReadField<float>(movement, kMovementMagnitude);
    const float zero = 0.0f;
    WriteField(movement, kMovementDirectionX, directionX);
    WriteField(movement, kMovementDirectionY, zero);
    WriteField(movement, kMovementDirectionZ, directionZ);
    WriteField(movement, kMovementHurtStyle, kOrdinaryHurtStyle);
    WriteField(movement, kMovementMagnitude, kBoundaryImpulse);
    if (queueState(movement, movement + kMovementHurtState)) return true;

    WriteField(movement, kMovementDirectionX, oldX);
    WriteField(movement, kMovementDirectionY, oldY);
    WriteField(movement, kMovementDirectionZ, oldZ);
    WriteField(movement, kMovementHurtStyle, oldStyle);
    WriteField(movement, kMovementMagnitude, oldMagnitude);
    return false;
}

inline bool MakePlan(const map_boundary::Entry& entry, Position position, Plan& plan) {
    using namespace map_boundary;
    if ((entry.flags & (kAreaEnabled | kDamageEnabled)) !=
            (kAreaEnabled | kDamageEnabled) ||
        !entry.damageEdgeMask || (entry.damageEdgeMask & ~kKnownEdges) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return false;

    Plan candidate{};
    candidate.wall = {position.x, position.y + 100.0f, position.z};
    if (entry.damageEdgeMask & kEdgeTop) {
        candidate.wall.z = std::min(entry.top, position.z + 100.0f);
        candidate.directionZ = -1.0f;
    } else if (entry.damageEdgeMask & kEdgeBottom) {
        candidate.wall.z = std::max(entry.bottom, position.z - 100.0f);
        candidate.directionZ = 1.0f;
    } else if (entry.damageEdgeMask & kEdgeLeft) {
        candidate.wall.x = std::max(entry.left, position.x - 100.0f);
        candidate.directionX = 1.0f;
    } else if (entry.damageEdgeMask & kEdgeRight) {
        candidate.wall.x = std::min(entry.right, position.x + 100.0f);
        candidate.directionX = -1.0f;
    } else {
        return false;
    }
    if (!std::isfinite(candidate.wall.x) || !std::isfinite(candidate.wall.y) ||
        !std::isfinite(candidate.wall.z)) return false;
    plan = candidate;
    return true;
}

} // namespace sdgo::map_catalog::shield_contact