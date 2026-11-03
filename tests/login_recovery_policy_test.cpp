#include <cstdio>
#include "../mods/login_recovery/retry_policy.h"
int main() {
    using sdgo::login_recovery::Attempt;
    int notifications = 0;
    auto notify = [&] { ++notifications; return true; };
    Attempt first;
    if (first.AfterNativeCleanup(false, 1, notify) ||
        first.AfterNativeCleanup(true, 0, notify) || notifications) return 1;
    if (!first.AfterNativeCleanup(true, 1, notify) || notifications != 1) return 2;
    if (first.AfterNativeCleanup(true, 1, notify) || notifications != 1) return 3;
    Attempt failed;
    if (failed.AfterNativeCleanup(true, 1, [&] { ++notifications; return false; }) ||
        failed.AfterNativeCleanup(true, 1, notify) || notifications != 2) return 4;
    Attempt next;
    if (!next.AfterNativeCleanup(true, 1, notify) || notifications != 3) return 5;
    std::puts("PASS same request, positive identity, one notification and retry, failed notification fallback, independent next request");
    return 0;
}
