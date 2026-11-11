#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sdgo::map_catalog_server {

constexpr std::size_t kMaximumImportedMissions = 256;

struct MissionAllowlist {
    std::array<std::uint32_t, kMaximumImportedMissions> ids{};
    std::size_t count = 0;

    bool Contains(std::uint32_t mission_id) const noexcept {
        return std::binary_search(ids.begin(), ids.begin() + count, mission_id);
    }
};

inline bool IsSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

inline bool ParseMissionIds(const char* text, MissionAllowlist& output) noexcept {
    output = {};
    if (!text) return false;
    const char* cursor = text;
    while (IsSpace(*cursor)) ++cursor;
    if (*cursor == '\0') return true;

    for (;;) {
        while (IsSpace(*cursor)) ++cursor;
        if (*cursor < '0' || *cursor > '9') return false;
        std::uint32_t value = 0;
        do {
            const auto digit = static_cast<std::uint32_t>(*cursor - '0');
            if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
            value = value * 10 + digit;
            ++cursor;
        } while (*cursor >= '0' && *cursor <= '9');
        if (value == 0 || output.count == output.ids.size()) return false;
        output.ids[output.count++] = value;

        while (IsSpace(*cursor)) ++cursor;
        if (*cursor == '\0') break;
        if (*cursor != ',' && *cursor != ';') return false;
        ++cursor;
        const char* next = cursor;
        while (IsSpace(*next)) ++next;
        if (*next == '\0' || *next == ',' || *next == ';') return false;
        cursor = next;
    }

    std::sort(output.ids.begin(), output.ids.begin() + output.count);
    const auto unique_end = std::unique(output.ids.begin(), output.ids.begin() + output.count);
    output.count = static_cast<std::size_t>(unique_end - output.ids.begin());
    return true;
}

inline std::uint32_t AcceleratedDeadline(
    std::uint32_t native_deadline,
    std::uint32_t mission_id,
    const MissionAllowlist& allowlist) noexcept
{
    if (!allowlist.Contains(mission_id)) return native_deadline;
    // The verified native site has already computed now + 2. Moving the
    // deadline back by three seconds makes it strictly older than now while
    // avoiding unsigned wrap during the first two process seconds.
    return native_deadline >= 3 ? native_deadline - 3 : 0;
}

}  // namespace sdgo::map_catalog_server
