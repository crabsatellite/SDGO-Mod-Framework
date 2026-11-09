#pragma once
#include <cstdint>

namespace capsule_catalog {
// Only the ordinary-machine formatter calls this. Other identities and
// localization arguments retain their native representation.
inline uint32_t DisplayMachineNumber(uint32_t identity) {
    return identity >= 101 && identity <= 989901 && identity % 100 == 1
        ? identity / 100 : identity;
}
}
