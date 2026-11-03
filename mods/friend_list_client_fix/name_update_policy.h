#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace friend_list_client_fix {
inline constexpr const char* kNameUpdateSymbol = "friend_list.client.update_record";
inline constexpr std::size_t kUpdateRowBytes = 0x30;
inline constexpr std::size_t kNameOffset = 0x0A;
inline constexpr std::size_t kNameBytes = 17;

// The native presence row and cached friend have the same identity/name prefix.
// A lossy ANSI status reply must not replace an already known, complete name.
// All remaining status, location and relationship fields still come from it.
inline bool PreserveKnownName(const void* cached, const void* incoming) noexcept {
    if (!cached || !incoming) return false;
    std::uint32_t oldId = 0, newId = 0;
    std::memcpy(&oldId,cached,sizeof(oldId));
    std::memcpy(&newId,incoming,sizeof(newId));
    if (!oldId || oldId != newId) return false;
    const auto* oldName = static_cast<const char*>(cached) + kNameOffset;
    const auto* newName = static_cast<const char*>(incoming) + kNameOffset;
    const auto* oldEnd = static_cast<const char*>(std::memchr(oldName,0,kNameBytes));
    const auto* newEnd = static_cast<const char*>(std::memchr(newName,0,kNameBytes));
    return oldEnd && oldEnd != oldName && newEnd && newEnd != newName &&
        !std::memchr(oldName,'?',static_cast<std::size_t>(oldEnd-oldName)) &&
        std::memchr(newName,'?',static_cast<std::size_t>(newEnd-newName));
}
}
