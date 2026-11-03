#pragma once
#include <windows.h>
#include <cstdint>

namespace sdgo::friend_unit_gift {
// A generation identifies this loaded server runtime, not an individual reply.
// Changing it for every request invalidates the client's pending gift preview.
inline std::uint64_t ServerRuntimeEpoch() noexcept {
    static const auto epoch=[]() noexcept {
        FILETIME now{};GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER stamp{};stamp.LowPart=now.dwLowDateTime;stamp.HighPart=now.dwHighDateTime;
        return stamp.QuadPart ^ (static_cast<std::uint64_t>(GetCurrentProcessId())<<16);
    }();
    return epoch ? epoch : 1;
}
}
