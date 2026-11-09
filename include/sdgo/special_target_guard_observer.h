#pragma once
#include <cstdint>

#define SDGO_SPECIAL_TARGET_GUARD_STATUS_EXPORT \
    "SdgoSpecialTargetGuardGetStatusV1"

struct SdgoSpecialTargetGuardStatusV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t guarded_invalid_counts;
    std::uint32_t forwarded_calls;
    std::int32_t last_invalid_count;
};

using SdgoSpecialTargetGuardGetStatusV1Fn =
    int (__cdecl*)(SdgoSpecialTargetGuardStatusV1*);
