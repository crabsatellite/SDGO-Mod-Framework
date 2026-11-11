#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "sdgo/sdgo_api.h"

namespace sdgo::ui_localization::acquisition_text {
inline constexpr int kTableType = 85;
inline constexpr std::size_t kRecordBytes = 614;
inline constexpr std::size_t kUnitOffset = 6;
inline constexpr std::size_t kTextOffset = 214;
inline constexpr std::size_t kTextCharacters = 100;
using Record = std::array<std::uint8_t, kRecordBytes>;

// Prepare a display copy only for a reviewed stock record.
// The source row and every non-text field remain unchanged.
bool TranslateRecord(const std::uint8_t* source, std::size_t bytes,
                     std::uint32_t id, Record* output) noexcept;
bool Install(const SdgoApi* api, SdgoMod* mod);
void Activate() noexcept;
void Deactivate() noexcept;
} // namespace sdgo::ui_localization::acquisition_text
