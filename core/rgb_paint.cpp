#include "sdgo/rgb_paint.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sdgo::rgb_paint {
bool FromHsv(Hsv x, Rgb& out) noexcept {
    if (!std::isfinite(x.h) || !std::isfinite(x.s) || !std::isfinite(x.v) ||
        x.h < 0 || x.h > 360 || x.s < 0 || x.s > 1 || x.v < 0 || x.v > 1) return false;
    const double h = (x.h == 360 ? 0 : x.h) / 60;
    const double chroma = x.v * x.s;
    const double middle = chroma * (1 - std::abs(std::fmod(h, 2) - 1));
    const double m = x.v - chroma;
    double r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
    case 0: r = chroma; g = middle; break;
    case 1: r = middle; g = chroma; break;
    case 2: g = chroma; b = middle; break;
    case 3: g = middle; b = chroma; break;
    case 4: r = middle; b = chroma; break;
    default: r = chroma; b = middle; break;
    }
    const auto byte = [m](double v) { return static_cast<std::uint8_t>(std::lround((v + m) * 255)); };
    out = {byte(r), byte(g), byte(b)};
    return true;
}
Hsv ToHsv(Rgb rgb) noexcept {
    const double r = rgb.r / 255., g = rgb.g / 255., b = rgb.b / 255.;
    const double high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
    Hsv out{0, high == 0 ? 0 : delta / high, high};
    if (delta == 0) return out;
    if (high == r) out.h = 60 * std::fmod((g - b) / delta, 6);
    else if (high == g) out.h = 60 * ((b - r) / delta + 2);
    else out.h = 60 * ((r - g) / delta + 4);
    if (out.h < 0) out.h += 360;
    return out;
}
bool ParseHex(std::string_view text, Rgb& out) noexcept {
    if (text.size() == 7 && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6) return false;
    std::uint32_t value = 0;
    for (char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    out = {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return true;
}
std::string Hex(Rgb rgb) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "#000000";
    const std::array<std::uint8_t, 3> channels{rgb.r, rgb.g, rgb.b};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        out[1 + i * 2] = digits[channels[i] >> 4];
        out[2 + i * 2] = digits[channels[i] & 15];
    }
    return out;
}
std::optional<Rgb> FromNative(std::uint16_t color) noexcept {
    if (!color) return std::nullopt;
    return Rgb{static_cast<std::uint8_t>((color >> 7) & 248),
               static_cast<std::uint8_t>((color >> 2) & 248),
               static_cast<std::uint8_t>((color << 3) & 248)};
}
void Picker::Set(Rgb color) noexcept {
    auto next = ToHsv(color);
    if (next.s == 0) next.h = hsv_.h;
    if (next.v == 0) next.s = hsv_.s;
    rgb_ = color; hsv_ = next;
}
bool Picker::Channel(std::size_t channel, int value) noexcept {
    if (channel > 2 || value < 0 || value > 255) return false;
    Rgb next = rgb_;
    if (channel == 0) next.r = static_cast<std::uint8_t>(value);
    else if (channel == 1) next.g = static_cast<std::uint8_t>(value);
    else next.b = static_cast<std::uint8_t>(value);
    Set(next); return true;
}
bool Picker::Hue(double normalized) noexcept {
    if (!std::isfinite(normalized) || normalized < 0 || normalized > 1) return false;
    auto next = hsv_; next.h = normalized * 360;
    if (!FromHsv(next, rgb_)) return false;
    hsv_ = next; return true;
}
bool Picker::Square(double x, double y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return false;
    auto next = hsv_; next.s = x; next.v = 1 - y;
    if (!FromHsv(next,rgb_)) return false;
    hsv_ = next; return true;
}
bool Picker::HexInput(std::string_view text) noexcept {
    Rgb next;
    if (!ParseHex(text,next)) return false;
    Set(next); return true;
}
bool Editor::Open(const Snapshot& snapshot) noexcept {
    if (!snapshot.identity.session || !snapshot.identity.unit || state_ != State::closed) return false;
    saved_ = snapshot; draft_ = snapshot.colors; selected_ = 0; pending_ = {};
    state_ = State::editing;
    return true;
}
void Editor::Close() noexcept { saved_ = {}; draft_ = {}; pending_ = {}; state_ = State::closed; }
bool Editor::Select(std::size_t region) noexcept {
    if (state_ != State::editing || region >= kRegions) return false;
    selected_ = region; return true;
}
bool Editor::Set(Rgb color) noexcept {
    if (state_ != State::editing) return false;
    draft_[selected_] = color; return true;
}
bool Editor::Clear() noexcept {
    if (state_ != State::editing) return false;
    draft_[selected_].reset(); return true;
}
bool Editor::Restore() noexcept {
    if (state_ != State::editing) return false;
    draft_ = saved_.colors; return true;
}
bool Editor::Confirm() noexcept {
    if (state_ != State::editing || !dirty()) return false;
    state_ = State::confirming; return true;
}
bool Editor::CancelConfirmation() noexcept {
    if (state_ != State::confirming) return false;
    state_ = State::editing; return true;
}
std::optional<Save> Editor::Submit() noexcept {
    if (state_ != State::confirming || sequence_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    pending_ = {saved_.identity, saved_.revision, ++sequence_, draft_};
    state_ = State::saving; return pending_;
}
bool Editor::Matches(const Save& request) const noexcept {
    return request.identity == pending_.identity && request.sequence == pending_.sequence &&
        request.revision == pending_.revision && request.colors == pending_.colors;
}
bool Editor::Reply(const Save& request, const Snapshot& snapshot, bool accepted) noexcept {
    if ((state_ != State::saving && state_ != State::uncertain) || !Matches(request) ||
        snapshot.identity != saved_.identity || snapshot.revision < saved_.revision) return false;
    if (snapshot.revision == saved_.revision && snapshot.colors != saved_.colors) return false;
    if (accepted && (snapshot.colors != pending_.colors || snapshot.revision <= pending_.revision)) return false;
    saved_ = snapshot;
    if (accepted) draft_ = snapshot.colors;
    state_ = State::editing; pending_ = {}; return true;
}
bool Editor::Timeout(const Save& request) noexcept {
    if (state_ != State::saving || !Matches(request)) return false;
    state_ = State::uncertain; return true;
}
bool Editor::Reconcile(const Snapshot& snapshot) noexcept {
    if (state_ != State::uncertain || snapshot.identity != saved_.identity || snapshot.revision < saved_.revision) return false;
    if (snapshot.revision == saved_.revision && snapshot.colors != saved_.colors) return false;
    saved_ = snapshot; draft_ = snapshot.colors; pending_ = {}; state_ = State::editing; return true;
}
} // namespace sdgo::rgb_paint
