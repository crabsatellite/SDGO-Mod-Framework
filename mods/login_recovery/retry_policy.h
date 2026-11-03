#pragma once
namespace sdgo::login_recovery {
struct Attempt {
    bool used = false;
    template<class Notify> bool AfterNativeCleanup(bool sameRequest, int account, Notify notify) {
        if (!sameRequest || account <= 0 || used) return false;
        used = true;
        return notify();
    }
};
}
