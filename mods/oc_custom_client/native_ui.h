// Host-allocated OC allocation panel on the native factory upgrade page.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sdgo/oc_custom_protocol.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::oc_custom::client::ui {

struct NativeString {
    union {
        wchar_t small[8];
        const wchar_t* pointer;
    } data{};
    std::uint32_t length = 0;
    std::uint32_t capacity = 7;
};
static_assert(sizeof(NativeString) == 24, "native string ABI");

enum class Action : std::uint8_t {
    none,
    minus0, minus1, minus2, minus3, minus4, minus5,
    plus0, plus1, plus2, plus3, plus4, plus5,
    apply,
    reset,
    challenge,
    refresh,
    over_toggle,
    panel_toggle,
};

inline bool IsMinus(Action action) noexcept {
    return action >= Action::minus0 && action <= Action::minus5;
}
inline bool IsPlus(Action action) noexcept {
    return action >= Action::plus0 && action <= Action::plus5;
}
inline std::size_t StatOf(Action action) noexcept {
    if (IsMinus(action)) return static_cast<std::size_t>(action) - static_cast<std::size_t>(Action::minus0);
    if (IsPlus(action)) return static_cast<std::size_t>(action) - static_cast<std::size_t>(Action::plus0);
    return 0;
}

struct Bindings {
    using Add = std::uint32_t(__thiscall*)(void*, const NativeString*, int, int, int, int, std::uint32_t);
    using Find = void*(__thiscall*)(void*, const NativeString*, int);
    using Text = void(__thiscall*)(void*, int, const NativeString*, int, int, int, int, std::uint32_t,
                                   std::uint32_t, std::uint8_t, std::uint32_t, int);
    using Skin = void(__thiscall*)(void*, const NativeString*, std::uint8_t, std::uint32_t, std::uint32_t);
    using EmptySkin = void(__thiscall*)(void*);
    using AddIcon = std::uint32_t(__thiscall*)(void*, const NativeString*, int, int, int, int);
    Add add = nullptr;
    Find find = nullptr;
    Text text = nullptr;
    Skin skin = nullptr;
    EmptySkin empty_skin = nullptr;
    AddIcon add_icon = nullptr;
    void* button_vtable = nullptr;
    bool Bind(const SdgoApi* api, SdgoMod* mod) noexcept;
};

// What the panel shows. The owner fills it from the server profile and the
// pending local allocation; the panel only renders.
struct View {
    bool server_oc_mode = false;
    bool profile_present = false;
    bool busy = false;
    bool over_mode = false; // editing over points instead of normal points
    std::wstring status;
    std::wstring unit_caption;
    std::uint8_t oc_level = 0;
    std::uint8_t native_level = 0;
    std::uint8_t normal_points_left = 0;
    std::uint8_t over_points_left = 0;
    std::array<std::int32_t, stat_count> base{};
    std::array<std::int32_t, stat_count> normal_gain{};
    std::array<std::int32_t, stat_count> over_gain{};
    std::array<std::uint8_t, stat_count> normal{};
    std::array<std::uint8_t, stat_count> over{};
    std::array<std::uint8_t, stat_count> pending_normal{};
    std::array<std::uint8_t, stat_count> pending_over{};
    int pending_left = 0;
    bool dirty = false;
    bool challenge_available = false;
    std::wstring challenge_caption;
    std::wstring reset_caption;
};

class Panel {
public:
    bool Attach(void* service, void* dialog, void* skin_source, const Bindings& bindings, int left, int top);
    void Detach() noexcept;
    bool Ready() const noexcept { return dialog_ != nullptr; }
    void* service() const noexcept { return service_; }
    void* dialog() const noexcept { return dialog_; }
    Action Click(void* control) const noexcept;
    void Render(const View& view);
    bool Alive() const noexcept;
    const char* last_failure() const noexcept { return last_failure_; }
    // The OC page opens over the native item area only on request; the
    // toggle button stays on the page.
    bool open() const noexcept { return open_; }
    void Toggle() noexcept { open_ = !open_; }

private:
    struct Control {
        void* pointer = nullptr;
        Action action = Action::none;
        int width = 0;
        std::wstring text;
        std::uint32_t color = 0;
        bool set = false;
    };
    bool Add(const wchar_t* name, const wchar_t* label, Action action, int x, int y, int w, int h);
    void SetText(Control& control, const std::wstring& value, std::uint32_t color);
    void SetState(Control& control, bool visible, bool enabled) noexcept;

    Bindings bindings_;
    void* service_ = nullptr;
    void* dialog_ = nullptr;
    void* skin_source_ = nullptr;
    std::vector<Control> controls_;
    const char* last_failure_ = nullptr;
    bool open_ = false;
    Control background_{};
    bool background_ready_ = false;
    std::uint32_t font_ = 1;
    std::uint32_t text_color_ = 0xFF9CB8C8;
    std::uint32_t shadow_color_ = 0xFF000000;
    std::array<Control*, stat_count> value_labels_{};
    std::array<Control*, stat_count> minus_buttons_{};
    std::array<Control*, stat_count> plus_buttons_{};
    Control* title_ = nullptr;
    Control* points_ = nullptr;
    Control* status_ = nullptr;
    Control* status2_ = nullptr;
    Control* apply_ = nullptr;
    Control* reset_ = nullptr;
    Control* challenge_ = nullptr;
    Control* refresh_ = nullptr;
    Control* over_toggle_ = nullptr;
    Control* toggle_ = nullptr;
};

} // namespace sdgo::oc_custom::client::ui
