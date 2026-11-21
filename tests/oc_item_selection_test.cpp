#include "../mods/oc_custom_client/item_selection.h"
#include <cstdlib>

using namespace sdgo::oc_custom;
using namespace sdgo::oc_custom::client;
int main() {
    ChallengeItemsReply before{};
    before.count = 3;
    for (int i = 0; i < 3; ++i) { before.items[i].item = 100 + i; before.items[i].count = 5; }
    std::array<bool, kChallengeItemMax> selected{};
    selected[0] = selected[2] = true;
    auto after = before;
    std::swap(after.items[0], after.items[1]);
    auto result = ReconcileItems(before, selected, after);
    if (result[0] || !result[1] || !result[2]) return EXIT_FAILURE;
    after.items[2].count = 0;
    result = ReconcileItems(before, selected, after);
    if (result[0] || !result[1] || result[2]) return EXIT_FAILURE;
    after.items[1].item = 999;
    result = ReconcileItems(before, selected, after);
    for (bool value : result) if (value) return EXIT_FAILURE;
    after = before; after.ret_val = 1;
    result = ReconcileItems(before, selected, after);
    for (bool value : result) if (value) return EXIT_FAILURE;
    after = before; after.count = 4; after.items[3] = before.items[0];
    result = ReconcileItems(before, selected, after);
    if (!result[0] || !result[2] || result[3]) return EXIT_FAILURE;
    before = after; before.items[3].item = 103; after = before;
    selected.fill(true);
    result = ReconcileItems(before, selected, after);
    if (!result[0] || !result[1] || !result[2] || result[3]) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
