// Optional read-only damage observation ABI for BattleServer processes where
// battle_rewards already owns battle.rewards.record_damage.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION 1
#define SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_EXPORT \
    "SdgoBattleRewardsRegisterDamageObserverV1"

#pragma pack(push, 4)

typedef struct SdgoBattleRewardsDamageEventV1 {
    uint32_t size;
    uint32_t version;
    void* room;
    void* victim_pilot;
    void* attacker_pilot;
    void* victim_unit;
    uint32_t effective_loss;
    int32_t death_flag;
} SdgoBattleRewardsDamageEventV1;

// Called synchronously from the existing native record_damage detour. The
// pointers are valid only for this callback. Copy a bounded read-only snapshot,
// return immediately, and never call back into battle_rewards.
typedef void (__cdecl* SdgoBattleRewardsDamageObserverCallbackV1)(
    const SdgoBattleRewardsDamageEventV1* event,
    void* user);

typedef struct SdgoBattleRewardsDamageObserverSinkV1 {
    uint32_t size;
    uint32_t version;
    SdgoBattleRewardsDamageObserverCallbackV1 callback;
    void* user;
} SdgoBattleRewardsDamageObserverSinkV1;

#pragma pack(pop)

typedef int (__cdecl* SdgoBattleRewardsRegisterDamageObserverV1Fn)(
    const SdgoBattleRewardsDamageObserverSinkV1* sink);

#ifdef __cplusplus
static_assert(
    offsetof(SdgoBattleRewardsDamageEventV1, effective_loss) ==
        8 + 4 * sizeof(void*),
    "battle reward damage observer pointer layout changed");
static_assert(
    sizeof(SdgoBattleRewardsDamageEventV1) ==
        16 + 4 * sizeof(void*),
    "battle reward damage observer event layout changed");
static_assert(
    sizeof(SdgoBattleRewardsDamageObserverSinkV1) ==
        8 + sizeof(SdgoBattleRewardsDamageObserverCallbackV1) +
            sizeof(void*),
    "battle reward damage observer sink layout changed");
#endif
