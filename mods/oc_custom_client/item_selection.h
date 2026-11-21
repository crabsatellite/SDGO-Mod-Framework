#pragma once
#include "sdgo/oc_custom_protocol.h"

namespace sdgo::oc_custom::client {
// Selection follows item identity across a fresh server inventory response.
// Removed or exhausted types must never migrate to another row's item.
inline std::array<bool, kChallengeItemMax> ReconcileItems(
    const ChallengeItemsReply& before, const std::array<bool, kChallengeItemMax>& selected,
    const ChallengeItemsReply& after) {
    std::array<bool, kChallengeItemMax> result{};
    if (after.ret_val != 0) return result;
    std::size_t placed = 0;
    for (std::size_t row = 0; row < after.count && row < kChallengeItemMax && placed < 3; ++row) {
        if (after.items[row].count <= 0 || after.items[row].item <= 0) continue;
        for (std::size_t old = 0; old < before.count && old < kChallengeItemMax; ++old)
            if (selected[old] && before.items[old].item == after.items[row].item) {
                bool duplicate = false;
                for (std::size_t i = 0; i < row; ++i)
                    duplicate |= result[i] && after.items[i].item == after.items[row].item;
                if (!duplicate) { result[row] = true; ++placed; }
                break;
            }
    }
    return result;
}
}
