#include <cstdint>
#include <cstdio>

#include "../mods/battle_ai/native_map_gate.h"

namespace {

using battle_ai::native::GateCompletionRequest0A19;
using battle_ai::native::GateCountdownNotice0534Prefix;
using battle_ai::native::GateDamageRequest06DD;
using battle_ai::native::GateDestroyedNotice0835;

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

struct AuthorityFixture {
    std::uint32_t pilot = 0x80000021u;
    std::uint32_t object = 1007;
    bool registered = true;
    bool slotHeld = true;
    bool localControl = true;
    bool currentGeneration = true;
    int calls = 0;
};

bool OwnedLocalNpc(void* context, std::uint32_t pilot,
                   std::uint32_t object) {
    auto& fixture = *static_cast<AuthorityFixture*>(context);
    ++fixture.calls;
    return pilot == fixture.pilot && object == fixture.object &&
           fixture.registered && fixture.slotHeld &&
           fixture.localControl && fixture.currentGeneration;
}

void TestWireLayouts() {
    GateDamageRequest06DD damage{};
    damage.attackerPilotId = 0x80000021u;
    damage.blockCount = 1;
    damage.blockRuntimeIds[0] = 0x11223344u;
    damage.damage[0] = 900;
    const auto* damageBytes =
        reinterpret_cast<const std::uint8_t*>(&damage);
    Check(sizeof(damage) == 0x4F &&
              damage.opcode == 0x06DD &&
              *reinterpret_cast<const std::uint32_t*>(damageBytes + 0x06) ==
                  0x80000021u &&
              damageBytes[0x0E] == 1 &&
              *reinterpret_cast<const std::uint32_t*>(damageBytes + 0x0F) ==
                  0x11223344u &&
              *reinterpret_cast<const std::uint32_t*>(damageBytes + 0x2F) ==
                  900,
          "0x06DD preserves the native attacker, block and damage offsets");

    GateCountdownNotice0534Prefix countdown{};
    countdown.attackerPilotId = damage.attackerPilotId;
    countdown.blockCount = 1;
    countdown.firstBlockRuntimeId = damage.blockRuntimeIds[0];
    const auto* countdownBytes =
        reinterpret_cast<const std::uint8_t*>(&countdown);
    Check(countdown.opcode == 0x0534 &&
              *reinterpret_cast<const std::uint32_t*>(
                  countdownBytes + 0x06) == damage.attackerPilotId &&
              countdownBytes[0x0A] == 1 &&
              *reinterpret_cast<const std::uint32_t*>(
                  countdownBytes + 0x0B) == damage.blockRuntimeIds[0],
          "0x0534 exposes the native attacker before countdown ownership");

    GateCompletionRequest0A19 completion{};
    completion.attackerPilotId = damage.attackerPilotId;
    completion.primaryBlockRuntimeId = damage.blockRuntimeIds[0];
    completion.unitVictimCount = 1;
    completion.unitVictimIds[0] = 55;
    completion.unitDamage[0] = 600;
    completion.unitHitValues[0] = 7;
    completion.unitHitFlags[0] = 2;
    completion.blockVictimCount = 1;
    completion.blockVictimIds[0] = 77;
    completion.blockDamage[0] = 500;
    const auto* completionBytes =
        reinterpret_cast<const std::uint8_t*>(&completion);
    Check(sizeof(completion) == 0xD4 &&
              completion.opcode == 0x0A19 &&
              *reinterpret_cast<const std::uint32_t*>(
                  completionBytes + 0x0A) == damage.blockRuntimeIds[0] &&
              completionBytes[0x0E] == 1 &&
              *reinterpret_cast<const std::uint32_t*>(
                  completionBytes + 0x0F) == 55 &&
              *reinterpret_cast<const std::uint32_t*>(
                  completionBytes + 0x3F) == 600 &&
              *reinterpret_cast<const std::uint16_t*>(
                  completionBytes + 0x6F) == 7 &&
              completionBytes[0x87] == 2 &&
              completionBytes[0x93] == 1 &&
              *reinterpret_cast<const std::uint32_t*>(
                  completionBytes + 0x94) == 77 &&
              *reinterpret_cast<const std::uint32_t*>(
                  completionBytes + 0xB4) == 500,
          "0x0A19 preserves the native completion and collateral layout");

    GateDestroyedNotice0835 destroyed{};
    destroyed.primaryBlockRuntimeId = damage.blockRuntimeIds[0];
    destroyed.unitVictimCount = 1;
    destroyed.unitVictimIds[0] = 55;
    destroyed.unitDamage[0] = 600;
    destroyed.unitHitFlags[0] = 2;
    destroyed.blockVictimCount = 1;
    destroyed.blockVictimIds[0] = 77;
    destroyed.blockDamage[0] = 500;
    const auto* destroyedBytes =
        reinterpret_cast<const std::uint8_t*>(&destroyed);
    Check(sizeof(destroyed) == 0xB8 &&
              destroyed.opcode == 0x0835 &&
              *reinterpret_cast<const std::uint32_t*>(
                  destroyedBytes + 0x06) == damage.blockRuntimeIds[0] &&
              destroyedBytes[0x0A] == 1 &&
              *reinterpret_cast<const std::uint32_t*>(
                  destroyedBytes + 0x0B) == 55 &&
              *reinterpret_cast<const std::uint32_t*>(
                  destroyedBytes + 0x3B) == 600 &&
              destroyedBytes[0x6B] == 2 &&
              destroyedBytes[0x77] == 1 &&
              *reinterpret_cast<const std::uint32_t*>(
                  destroyedBytes + 0x78) == 77 &&
              *reinterpret_cast<const std::uint32_t*>(
                  destroyedBytes + 0x98) == 500,
          "0x0835 preserves the authoritative removal and collateral layout");
}

void TestServerAuthority() {
    using battle_ai::native::NativeGateServerAuthority;

    Check(NativeGateServerAuthority(44, 44, 99),
          "the native server accepts an actor sending for itself");
    Check(NativeGateServerAuthority(99, 0x80000021u, 99),
          "the native server accepts the room host for a high-bit NPC");
    Check(!NativeGateServerAuthority(98, 0x80000021u, 99),
          "a non-host connection cannot submit for a reserved NPC");
    Check(!NativeGateServerAuthority(99, 21, 99),
          "the host exception does not cover an ordinary pilot");
}

void TestCountdownAuthority() {
    using battle_ai::native::OwnsGateCountdown;

    AuthorityFixture fixture;
    Check(OwnsGateCountdown(true, 21, 12, 12, nullptr, nullptr) &&
              fixture.calls == 0,
          "native local-human countdown authority remains unchanged");

    Check(OwnsGateCountdown(false, fixture.pilot, fixture.object, 12,
                            &OwnedLocalNpc, &fixture) &&
              fixture.calls == 1,
          "an exact live locally-owned high-bit NPC may own the countdown");

    fixture.currentGeneration = false;
    Check(!OwnsGateCountdown(false, fixture.pilot, fixture.object, 12,
                             &OwnedLocalNpc, &fixture),
          "a stale NPC generation cannot own a gate countdown");

    fixture.currentGeneration = true;
    Check(!OwnsGateCountdown(false, 21, fixture.object, 12,
                             &OwnedLocalNpc, &fixture),
          "an ordinary pilot cannot enter the NPC authority extension");
    Check(!OwnsGateCountdown(false, fixture.pilot, fixture.object, 12,
                             nullptr, &fixture),
          "missing live ownership validation fails closed");
    Check(!OwnsGateCountdown(false, fixture.pilot, 12, 12,
                             &OwnedLocalNpc, &fixture),
          "the extension cannot relabel the local human object as an NPC");
}

void TestMeleeTypeBoundary() {
    using battle_ai::native::IsDestroyableBlockKind;
    using battle_ai::native::BypassesUnitDamageProtectionForGate;

    Check(IsDestroyableBlockKind(0x12) &&
              BypassesUnitDamageProtectionForGate(0x12) &&
              !IsDestroyableBlockKind(12),
          "native melee kind 0x12 bypasses unit-only protection reads, not decimal 12");
    Check(!IsDestroyableBlockKind(7) &&
              !BypassesUnitDamageProtectionForGate(7),
          "ordinary unit targets retain unit damage-protection checks");
    Check(!IsDestroyableBlockKind(0x20) &&
              !BypassesUnitDamageProtectionForGate(0x20),
          "other native melee object types are not relabeled as gates");
}

}  // namespace

int main() {
    TestWireLayouts();
    TestServerAuthority();
    TestCountdownAuthority();
    TestMeleeTypeBoundary();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::puts("all native map gate tests passed");
    return 0;
}
