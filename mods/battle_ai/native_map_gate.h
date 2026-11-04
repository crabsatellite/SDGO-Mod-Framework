#pragma once

#include <cstddef>
#include <cstdint>

namespace battle_ai::native {

constexpr std::uint16_t kGateDamageRequestOpcode = 0x06DD;
constexpr std::uint16_t kGateCountdownNoticeOpcode = 0x0534;
constexpr std::uint16_t kGateCompletionRequestOpcode = 0x0A19;
constexpr std::uint16_t kGateDestroyedNoticeOpcode = 0x0835;
constexpr int kDestroyableBlockKind = 0x12;
constexpr std::uint32_t kNpcPilotBit = 0x80000000u;

constexpr std::size_t kGateDamageCapacity = 8;
constexpr std::size_t kGateUnitVictimCapacity = 12;
constexpr std::size_t kGateBlockVictimCapacity = 8;

#pragma pack(push, 1)
// The native sender initializes routeId to zero. The authenticated transport
// supplies the connection pilot before BattleServer dispatch. BattleServer's
// 0x006DD handler at 0x004C6AF0 reads attackerPilotId independently.
struct GateDamageRequest06DD {
    std::uint16_t opcode = kGateDamageRequestOpcode;
    std::uint32_t routeId = 0;
    std::uint32_t attackerPilotId = 0;
    std::uint32_t reserved0A = 0;
    std::uint8_t blockCount = 0;
    std::uint32_t blockRuntimeIds[kGateDamageCapacity]{};
    std::uint32_t damage[kGateDamageCapacity]{};
};

// BattleServer emits this prefix after a lethal hit selects authored delayed
// destruction. GOnline 0x004C6240 resolves attackerPilotId, dispatches object
// message 36 to every block, and starts each native countdown.
struct GateCountdownNotice0534Prefix {
    std::uint16_t opcode = kGateCountdownNoticeOpcode;
    std::uint32_t routeId = 0;
    std::uint32_t attackerPilotId = 0;
    std::uint8_t blockCount = 0;
    std::uint32_t firstBlockRuntimeId = 0;
};

// GOnline's CDestroyableBlock update sends this only when its native countdown
// reaches zero and the message-36 authority byte was accepted. BattleServer's
// 0x004C96A0 validates the room, actor, block and prior zero-HP state before
// broadcasting 0x0835.
struct GateCompletionRequest0A19 {
    std::uint16_t opcode = kGateCompletionRequestOpcode;
    std::uint32_t routeId = 0;
    std::uint32_t attackerPilotId = 0;
    std::uint32_t primaryBlockRuntimeId = 0;
    std::uint8_t unitVictimCount = 0;
    std::uint32_t unitVictimIds[kGateUnitVictimCapacity]{};
    std::uint32_t unitDamage[kGateUnitVictimCapacity]{};
    std::uint16_t unitHitValues[kGateUnitVictimCapacity]{};
    std::uint8_t unitHitFlags[kGateUnitVictimCapacity]{};
    std::uint8_t blockVictimCount = 0;
    std::uint32_t blockVictimIds[kGateBlockVictimCapacity]{};
    std::uint32_t blockDamage[kGateBlockVictimCapacity]{};
};

// GOnline 0x004C63D0 consumes this notice. It dispatches message 2 to the
// primary block, which performs the authoritative removal/replacement.
struct GateDestroyedNotice0835 {
    std::uint16_t opcode = kGateDestroyedNoticeOpcode;
    std::uint32_t routeId = 0;
    std::uint32_t primaryBlockRuntimeId = 0;
    std::uint8_t unitVictimCount = 0;
    std::uint32_t unitVictimIds[kGateUnitVictimCapacity]{};
    std::uint32_t unitDamage[kGateUnitVictimCapacity]{};
    std::uint8_t unitHitFlags[kGateUnitVictimCapacity]{};
    std::uint8_t blockVictimCount = 0;
    std::uint32_t blockVictimIds[kGateBlockVictimCapacity]{};
    std::uint32_t blockDamage[kGateBlockVictimCapacity]{};
};
#pragma pack(pop)

static_assert(sizeof(GateDamageRequest06DD) == 0x4F,
              "GOnline gate-damage request ABI changed");
static_assert(offsetof(GateDamageRequest06DD, attackerPilotId) == 0x06,
              "GOnline gate-damage attacker field moved");
static_assert(offsetof(GateDamageRequest06DD, blockCount) == 0x0E,
              "GOnline gate-damage count field moved");
static_assert(offsetof(GateDamageRequest06DD, blockRuntimeIds) == 0x0F,
              "GOnline gate-damage ID array moved");
static_assert(offsetof(GateDamageRequest06DD, damage) == 0x2F,
              "GOnline gate-damage value array moved");

static_assert(offsetof(GateCountdownNotice0534Prefix, attackerPilotId) == 0x06,
              "GOnline gate-countdown attacker field moved");
static_assert(offsetof(GateCountdownNotice0534Prefix, blockCount) == 0x0A,
              "GOnline gate-countdown count field moved");
static_assert(offsetof(GateCountdownNotice0534Prefix, firstBlockRuntimeId) == 0x0B,
              "GOnline gate-countdown ID array moved");

static_assert(sizeof(GateCompletionRequest0A19) == 0xD4,
              "GOnline gate-completion request ABI changed");
static_assert(offsetof(GateCompletionRequest0A19, attackerPilotId) == 0x06,
              "GOnline gate-completion attacker field moved");
static_assert(offsetof(GateCompletionRequest0A19, primaryBlockRuntimeId) == 0x0A,
              "GOnline gate-completion primary block field moved");
static_assert(offsetof(GateCompletionRequest0A19, unitVictimCount) == 0x0E,
              "GOnline gate-completion unit count moved");
static_assert(offsetof(GateCompletionRequest0A19, unitVictimIds) == 0x0F,
              "GOnline gate-completion unit IDs moved");
static_assert(offsetof(GateCompletionRequest0A19, unitDamage) == 0x3F,
              "GOnline gate-completion unit damage moved");
static_assert(offsetof(GateCompletionRequest0A19, unitHitValues) == 0x6F,
              "GOnline gate-completion hit values moved");
static_assert(offsetof(GateCompletionRequest0A19, unitHitFlags) == 0x87,
              "GOnline gate-completion hit flags moved");
static_assert(offsetof(GateCompletionRequest0A19, blockVictimCount) == 0x93,
              "GOnline gate-completion block count moved");
static_assert(offsetof(GateCompletionRequest0A19, blockVictimIds) == 0x94,
              "GOnline gate-completion block IDs moved");
static_assert(offsetof(GateCompletionRequest0A19, blockDamage) == 0xB4,
              "GOnline gate-completion block damage moved");

static_assert(sizeof(GateDestroyedNotice0835) == 0xB8,
              "GOnline gate-destroyed notice ABI changed");
static_assert(offsetof(GateDestroyedNotice0835, primaryBlockRuntimeId) == 0x06,
              "GOnline gate-destroyed primary block field moved");
static_assert(offsetof(GateDestroyedNotice0835, unitVictimCount) == 0x0A,
              "GOnline gate-destroyed unit count moved");
static_assert(offsetof(GateDestroyedNotice0835, unitVictimIds) == 0x0B,
              "GOnline gate-destroyed unit IDs moved");
static_assert(offsetof(GateDestroyedNotice0835, unitDamage) == 0x3B,
              "GOnline gate-destroyed unit damage moved");
static_assert(offsetof(GateDestroyedNotice0835, unitHitFlags) == 0x6B,
              "GOnline gate-destroyed hit flags moved");
static_assert(offsetof(GateDestroyedNotice0835, blockVictimCount) == 0x77,
              "GOnline gate-destroyed block count moved");
static_assert(offsetof(GateDestroyedNotice0835, blockVictimIds) == 0x78,
              "GOnline gate-destroyed block IDs moved");
static_assert(offsetof(GateDestroyedNotice0835, blockDamage) == 0x98,
              "GOnline gate-destroyed block damage moved");

constexpr bool IsNpcPilot(std::uint32_t pilotId) noexcept {
    return (pilotId & kNpcPilotBit) != 0;
}

constexpr bool IsDestroyableBlockKind(int kind) noexcept {
    return kind == kDestroyableBlockKind;
}

// Type 0x12 is explicitly admitted by the ordinary melee result function at
// 0x006B1CF0. Unit-only hurt/movement protection reads must be skipped for this
// object kind; native vtable+0x28 eligibility, damage, message C and 0x06DD
// remain authoritative.
constexpr bool BypassesUnitDamageProtectionForGate(int kind) noexcept {
    return IsDestroyableBlockKind(kind);
}

// Exact BattleServer rule shared by 0x06DD and 0x0A19 after the sender has
// already been resolved to a live room:
//   sender == attacker, or
//   sender == room host and attacker is a reserved high-bit NPC pilot.
constexpr bool NativeGateServerAuthority(std::uint32_t senderPilotId,
                                         std::uint32_t attackerPilotId,
                                         std::uint32_t roomHostPilotId) noexcept {
    return senderPilotId == attackerPilotId ||
           (senderPilotId == roomHostPilotId && IsNpcPilot(attackerPilotId));
}

using GateOwnedLocalNpcFn =
    bool (*)(void* context, std::uint32_t attackerPilotId,
             std::uint32_t attackerObjectId);

// GOnline 0x004C6240 already grants countdown ownership to the local human.
// Extend only that one decision for an exact locally-owned NPC. The callback
// owns current bot registration, room slot, local-control and generation
// validation. Missing or stale authority fails closed. The caller must still
// let native message 36, countdown, 0x0A19 and server 0x0835 run unchanged.
inline bool OwnsGateCountdown(bool nativeLocalHumanAuthority,
                              std::uint32_t attackerPilotId,
                              std::uint32_t attackerObjectId,
                              std::uint32_t localHumanObjectId,
                              GateOwnedLocalNpcFn ownedLocalNpc,
                              void* context) noexcept {
    if (nativeLocalHumanAuthority) return true;
    if (!ownedLocalNpc || !IsNpcPilot(attackerPilotId) ||
        attackerObjectId == 0 || attackerObjectId == localHumanObjectId) {
        return false;
    }
    return ownedLocalNpc(context, attackerPilotId, attackerObjectId);
}

}  // namespace battle_ai::native
