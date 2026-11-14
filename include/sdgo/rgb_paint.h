#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sdgo::rgb_paint {

inline constexpr std::size_t kRegions = 6;
struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
};
inline bool operator==(Rgb a, Rgb b) noexcept { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(Rgb a, Rgb b) noexcept { return !(a == b); }
struct Hsv { double h = 0, s = 0, v = 0; }; // hue in degrees; saturation/value in [0,1]
bool FromHsv(Hsv hsv, Rgb& out) noexcept;
Hsv ToHsv(Rgb rgb) noexcept;
bool ParseHex(std::string_view text, Rgb& out) noexcept;
std::string Hex(Rgb rgb);
// The stock client decodes RGB555 by shifting each channel left three bits.
// Zero means restore material, not black. This conversion is for stock data
// only; exact RGB values must never be persisted through RGB555.
std::optional<Rgb> FromNative(std::uint16_t color) noexcept;

// Input model for the native page's hue strip, saturation/value square and
// RGB channels. Achromatic RGB edits retain the selected hue so dragging up
// from black does not unexpectedly switch a blue/pink selection to red.
class Picker {
public:
    void Set(Rgb color) noexcept;
    bool Channel(std::size_t channel, int value) noexcept;
    bool Hue(double normalized) noexcept;
    bool Square(double x, double y) noexcept;
    bool HexInput(std::string_view text) noexcept;
    Rgb color() const noexcept { return rgb_; }
    Hsv hsv() const noexcept { return hsv_; }
private:
    Rgb rgb_{};
    Hsv hsv_{};
};

using Palette = std::array<std::optional<Rgb>, kRegions>;
struct Identity {
    std::uint64_t session = 0, unit = 0;
};
inline bool operator==(Identity a, Identity b) noexcept { return a.session == b.session && a.unit == b.unit; }
inline bool operator!=(Identity a, Identity b) noexcept { return !(a == b); }
struct Snapshot {
    Identity identity;
    std::uint64_t revision = 0;
    Palette colors;
};
enum class State { closed, editing, confirming, saving, uncertain };
struct Save {
    Identity identity;
    std::uint64_t revision = 0, sequence = 0;
    Palette colors;
};

// Pure UI transaction model. Preview never updates the committed snapshot.
// Bind only snapshots from the authoritative adapter, on the game UI thread.
class Editor {
public:
    bool Open(const Snapshot& snapshot) noexcept;
    void Close() noexcept;
    bool Select(std::size_t region) noexcept;
    bool Set(Rgb color) noexcept;
    bool Clear() noexcept;
    bool Restore() noexcept;
    bool Confirm() noexcept;
    bool CancelConfirmation() noexcept;
    std::optional<Save> Submit() noexcept;
    bool Reply(const Save& request, const Snapshot& snapshot, bool accepted) noexcept;
    bool Timeout(const Save& request) noexcept;
    // A fresh read resolves a timed-out write; it is never automatically retried.
    bool Reconcile(const Snapshot& snapshot) noexcept;
    State state() const noexcept { return state_; }
    bool dirty() const noexcept { return draft_ != saved_.colors; }
    std::size_t selected() const noexcept { return selected_; }
    const Palette& preview() const noexcept { return draft_; }
    const Snapshot& saved() const noexcept { return saved_; }
private:
    bool Matches(const Save& request) const noexcept;
    State state_ = State::closed;
    Snapshot saved_{};
    Palette draft_{};
    Save pending_{};
    std::size_t selected_ = 0;
    std::uint64_t sequence_ = 0;
};

} // namespace sdgo::rgb_paint
