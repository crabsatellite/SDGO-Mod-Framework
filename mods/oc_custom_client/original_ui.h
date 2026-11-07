// The original OC windows that still ship in the client package, driven on the
// native factory upgrade page.
//
// The current GOnline.exe no longer creates the OC services, but the package
// keeps their layouts: uirs\0794.uir (six-stat allocation window),
// uirs\0813.uir (OC challenge window) and uirs\0819.uir (OC confirmation
// popup). The page service loads them with its own loader (vtable +0x58, the
// call the upgrade page uses for uirs\0799.uir), so they are native dialogs
// owned, drawn and destroyed by the service, and their button events reach the
// service's OnUIEvent. The OVER CUSTOM entry of the OC-era upgrade page
// (OverCustomBtn and its BackIcon1 label, removed from the current 0799) is
// re-created on the page with the same position and sprites.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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

NativeString ViewOf(const std::wstring& value);

inline constexpr std::size_t kItemSlots = 10;

enum class Action : std::uint8_t {
    none,
    entry,                                  // OVER CUSTOM on the upgrade page
    up0, up1, up2, up3, up4, up5,           // <stat>UpBtn, protocol stat order
    reset,                                  // ResetBtn
    ex,                                     // ExBtn (OC EX ability)
    challenge_start, challenge_close, challenge_left, challenge_right,
    item0, item1, item2, item3, item4, item5, item6, item7, item8, item9,
    confirm_ok, confirm_cancel,
};

inline bool IsUp(Action action) noexcept { return action >= Action::up0 && action <= Action::up5; }
inline std::size_t UpStat(Action action) noexcept {
    return static_cast<std::size_t>(action) - static_cast<std::size_t>(Action::up0);
}
inline bool IsItem(Action action) noexcept { return action >= Action::item0 && action <= Action::item9; }
inline std::size_t ItemIndex(Action action) noexcept {
    return static_cast<std::size_t>(action) - static_cast<std::size_t>(Action::item0);
}

struct Bindings {
    using Add = std::uint32_t(__thiscall*)(void*, const NativeString*, int, int, int, int, std::uint32_t);
    using Find = void*(__thiscall*)(void*, const NativeString*, int);
    using Text = void(__thiscall*)(void*, int, const NativeString*, int, int, int, int, std::uint32_t,
                                   std::uint32_t, std::uint8_t, std::uint32_t, int);
    using Skin = void(__thiscall*)(void*, const NativeString*, std::uint8_t, std::uint32_t, std::uint32_t);
    using AddIcon = std::uint32_t(__thiscall*)(void*, const NativeString*, int, int, int, int);
    Add add = nullptr;
    Find find = nullptr;
    Text text = nullptr;
    Skin skin = nullptr;
    AddIcon add_icon = nullptr;
    void* button_vtable = nullptr;
    bool Bind(const SdgoApi* api, SdgoMod* mod) noexcept;
};

enum Window : std::uint8_t { window_over = 0, window_challenge, window_confirm, window_count };

struct EntryView {
    bool visible = false;
    bool lit = false; // the challenge can start: the glowing OVER CUSTOM button
};

struct OverView {
    bool visible = false;
    std::wstring notice;                       // NoticeStatic
    std::wstring normal_left, over_left;       // NomalPointStatic, OverPointStatic
    std::array<std::wstring, stat_count> label;
    std::array<std::wstring, stat_count> normal;
    std::array<std::wstring, stat_count> over;
    std::array<bool, stat_count> up{};
    std::wstring reset;
    bool reset_enabled = false;
    bool ex_enabled = false;
};

struct ChallengeView {
    bool visible = false;
    std::wstring title, unit, state, target_title, target, message0, over_title, message1;
    bool start_enabled = false;
    bool left_enabled = false;
    bool right_enabled = false;
    std::array<std::wstring, kItemSlots> item_text;
    std::array<bool, kItemSlots> item_visible{};
    std::array<bool, kItemSlots> item_checked{};
};

struct ConfirmView {
    bool visible = false;
    std::wstring line0, line1;
    bool cancel = true;
};

class OriginalUi {
public:
    // Loader thread, right after the page's own Init: loads the OC windows into
    // the page service and binds every named control. On any miss nothing is
    // kept (the loaded dialogs stay hidden and are released with the service).
    bool Load(void* component, const Bindings& bindings);
    // Game thread: re-creates the OVER CUSTOM entry on the upgrade page dialog.
    bool AttachEntry();
    void Detach() noexcept;
    bool Loaded() const noexcept { return component_ != nullptr; }
    bool Alive() const noexcept;
    void* component() const noexcept { return component_; }
    Action Click(void* control) const noexcept;
    void Render(const EntryView& entry, const OverView& over, const ChallengeView& challenge,
                const ConfirmView& confirm);
    void HideAll() noexcept;
    // GameTest: a bound control by window ("entry", "over", "challenge",
    // "confirm") and original control name.
    void* Control(const char* window, const wchar_t* name) const noexcept;
    bool Visible(const char* window) const noexcept;
    bool entry_ready() const noexcept { return entry_button_ != nullptr; }
    void* page_dialog() const noexcept { return page_dialog_; }
    void* window_dialog(Window window) const noexcept { return dialogs_[window]; }
    bool window_shown(Window window) const noexcept { return shown_[window]; }
    const char* last_failure() const noexcept { return last_failure_; }

private:
    struct Bound {
        std::uint8_t window = 0;
        const wchar_t* name = nullptr;
        int type = 0;
        Action action = Action::none;
        void* pointer = nullptr;
        void* parent = nullptr; // the loaded dialog or its inner Dlg record
        std::wstring text;
        bool text_set = false;
        int width = 0;
    };
    Bound* Find(std::uint8_t window, const wchar_t* name) noexcept;
    const Bound* Find(std::uint8_t window, const wchar_t* name) const noexcept;
    void SetStatic(std::uint8_t window, const wchar_t* name, const std::wstring& value);
    void SetLabel(Bound& control, const std::wstring& value, int align);
    void SetState(void* control, bool visible, bool enabled) noexcept;
    void SetCheck(void* control, bool checked) noexcept;

    Bindings bindings_;
    void* component_ = nullptr;
    void* service_ = nullptr;
    void* page_dialog_ = nullptr;
    std::array<void*, window_count> dialogs_{};
    std::array<Bound, 80> bound_{};
    std::size_t bound_count_ = 0;
    void* entry_button_ = nullptr;
    void* entry_label_ = nullptr;
    std::array<bool, window_count> shown_{};
    bool entry_shown_ = false;
    const char* last_failure_ = nullptr;
    char failure_[256]{};
};

} // namespace sdgo::oc_custom::client::ui
