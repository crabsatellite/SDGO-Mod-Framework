#pragma once

namespace sdgo::oc_custom::client::ui {

// The native factory's horizontal entry advances by 0.05 per logic update
// (FUN_007E55F0). Keep progression on the game update, never the 250 ms timer.
class PageSlide {
public:
    void Show(bool visible) noexcept {
        if (visible && !visible_) frames_ = 20;
        if (!visible) frames_ = 0;
        visible_ = visible;
    }
    void Advance() noexcept { if (frames_) --frames_; }
    bool Moving() const noexcept { return frames_ != 0; }
    // A native service can expose its dialogs before the OC page's update.
    // Hidden panels must therefore stay at the entry edge, never at the
    // settled position where that first exposure would flash the whole panel.
    float Offset(float distance) const noexcept {
        return distance * (visible_ ? static_cast<float>(frames_) / 20.0f : 1.0f);
    }
private:
    bool visible_ = false;
    unsigned frames_ = 0;
};

} // namespace sdgo::oc_custom::client::ui
