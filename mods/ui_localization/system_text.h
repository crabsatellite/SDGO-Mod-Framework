#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "sdgo/sdgo_api.h"

namespace sdgo::ui_localization::system_text {

inline constexpr int kTableType = 0x3B;
inline constexpr std::size_t kRecordBytes = 209;
inline constexpr std::size_t kTextOffset = 5;
inline constexpr std::size_t kTextCharacters = 100;
inline constexpr std::size_t kStyleOffset = 205;

using Record = std::array<std::uint8_t, kRecordBytes>;

enum class PatchResult {
    unchanged,
    replaced,
    invalid
};

// Pure record helpers. They never mutate source or output on refusal.
PatchResult PatchRecord(const std::uint8_t* source, std::size_t sourceBytes,
                        std::uint32_t requestedId, int type,
                        std::wstring_view target, Record* output) noexcept;
PatchResult TranslateRecord(const std::uint8_t* source, std::size_t sourceBytes,
                            std::uint32_t requestedId, int type,
                            Record* output) noexcept;

struct Stats {
    std::uint64_t lookups;
    std::uint64_t replaced;
    std::uint64_t invalid;
};

bool Install(const SdgoApi* api, SdgoMod* mod);
Stats Snapshot();

} // namespace sdgo::ui_localization::system_text
