#pragma once

#include <windows.h>

#include <cstdint>

#include "native_sites.h"
#include "sdgo/operator_reward_filter.h"

namespace sdgo::battle_rewards {

struct NativeRewardAmounts {
    uint16_t pilot_exp = 0;
    uint16_t pilot_event = 0;
    uint16_t unit_exp = 0;
    uint16_t unit_event = 0;
    uint16_t gp = 0;
    uint16_t gp_event = 0;
};

inline uint16_t SaturateReward16(uint32_t value) {
    return static_cast<uint16_t>(value > UINT16_MAX ? UINT16_MAX : value);
}

inline uint16_t AdjustEventBucket(uint16_t event, uint16_t before,
                                  uint16_t after) {
    if (after >= before) {
        const uint32_t adjusted =
            static_cast<uint32_t>(event) + (after - before);
        return SaturateReward16(adjusted);
    }
    const uint16_t reduction = static_cast<uint16_t>(before - after);
    return event > reduction ? static_cast<uint16_t>(event - reduction) : 0;
}

inline bool ReadNativeRewardAmounts(BYTE* frame, unsigned site,
                                    NativeRewardAmounts* result) {
    if (!frame || !result || site >= kSettlementSites.size()) return false;
    const auto& fields = kSettlementSites[site];
    __try {
        NativeRewardAmounts staged;
        staged.pilot_exp =
            *reinterpret_cast<const volatile uint16_t*>(frame + fields.pilot_exp);
        staged.pilot_event =
            *reinterpret_cast<const volatile uint16_t*>(frame + fields.pilot_event);
        staged.unit_exp =
            *reinterpret_cast<const volatile uint16_t*>(frame + fields.unit_exp);
        staged.unit_event =
            *reinterpret_cast<const volatile uint16_t*>(frame + fields.unit_event);
        staged.gp =
            *reinterpret_cast<const volatile uint16_t*>(frame + fields.g_amount);
        staged.gp_event =
            *reinterpret_cast<const volatile uint16_t*>(frame + fields.g_event);
        *result = staged;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool WriteNativeExpTargets(BYTE* frame, unsigned site,
                                  uint32_t write_mask,
                                  uint32_t target_pilot_exp,
                                  uint32_t target_unit_exp) {
    if (!frame || site >= kSettlementSites.size() ||
        (write_mask & ~(SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                        SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP)) != 0) {
        return false;
    }
    const auto& fields = kSettlementSites[site];
    __try {
        if (write_mask & SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP) {
            volatile uint16_t* total =
                reinterpret_cast<volatile uint16_t*>(frame + fields.pilot_exp);
            volatile uint16_t* event =
                reinterpret_cast<volatile uint16_t*>(frame + fields.pilot_event);
            const uint16_t before = *total;
            const uint16_t after = SaturateReward16(target_pilot_exp);
            *event = AdjustEventBucket(*event, before, after);
            *total = after;
        }
        if (write_mask & SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP) {
            volatile uint16_t* total =
                reinterpret_cast<volatile uint16_t*>(frame + fields.unit_exp);
            volatile uint16_t* event =
                reinterpret_cast<volatile uint16_t*>(frame + fields.unit_event);
            const uint16_t before = *total;
            const uint16_t after = SaturateReward16(target_unit_exp);
            *event = AdjustEventBucket(*event, before, after);
            *total = after;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool WriteNativeGpTarget(BYTE* frame, unsigned site,
                                uint32_t target_gp) {
    if (!frame || site >= kSettlementSites.size()) return false;
    const auto& fields = kSettlementSites[site];
    __try {
        volatile uint16_t* total =
            reinterpret_cast<volatile uint16_t*>(frame + fields.g_amount);
        volatile uint16_t* event =
            reinterpret_cast<volatile uint16_t*>(frame + fields.g_event);
        const uint16_t before = *total;
        const uint16_t after = SaturateReward16(target_gp);
        *event = AdjustEventBucket(*event, before, after);
        *total = after;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace sdgo::battle_rewards
