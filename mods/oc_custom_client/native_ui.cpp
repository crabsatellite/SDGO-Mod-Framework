#include "native_ui.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace sdgo::oc_custom::client::ui {
namespace {

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(pointer) + offset, sizeof(value));
    return value;
}

template <class T>
void Put(void* pointer, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(pointer) + offset, &value, sizeof(value));
}

NativeString ViewOf(const std::wstring& value) {
    NativeString string;
    string.length = static_cast<std::uint32_t>(value.size());
    if (value.size() < 8) {
        std::memcpy(string.data.small, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
    } else {
        string.data.pointer = value.c_str();
        string.capacity = string.length;
    }
    return string;
}

bool ChildOf(void* child, void* dialog) noexcept {
    __try {
        return child && dialog && Read<void*>(child, 0x15C) == dialog;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsBlankSkin(const void* entry) {
    return entry && !Read<void*>(entry) && Read<std::uint32_t>(entry, 0x14) == 0 &&
           Read<std::uint32_t>(entry, 0x18) == 7 && Read<wchar_t>(entry, 4) == L'\0';
}

const wchar_t* kStatNames[stat_count] = {L"速度", L"HP", L"攻击", L"防御", L"敏捷", L"必杀"};

} // namespace

// The factory upgrade page dialog is a 279 px wide column. The overlay is
// 271 px wide: inner 259 px = name 28 + 2 + value 169 + 4 + minus 26 + 4 + plus 26,
// or two 126 px buttons with a 7 px gap. Height: toggle row 24, inner 290
// (title, points, six 22 px rows, three button rows, two status lines), margins.
constexpr int kOverlayWidth = 271;
constexpr int kOverlayHeight = 324;
constexpr int kInner = 259;
// Natural drawn size of the client's blue button sprites sprs\17530-17532.spr.
constexpr float kButtonSpriteWidth = 82.0f;
constexpr float kButtonSpriteHeight = 28.0f;

bool Bindings::Bind(const SdgoApi* api, SdgoMod* mod) noexcept {
    if (!api || !mod || !api->Symbol) return false;
    add = reinterpret_cast<Add>(api->Symbol(mod, "hangar.ui_add_button"));
    find = reinterpret_cast<Find>(api->Symbol(mod, "hangar.ui_find_control"));
    text = reinterpret_cast<Text>(api->Symbol(mod, "hangar.ui_text_item"));
    skin = reinterpret_cast<Skin>(api->Symbol(mod, "hangar.ui_add_skin"));
    empty_skin = reinterpret_cast<EmptySkin>(api->Symbol(mod, "friend_unit_gift.ui_add_empty_skin"));
    add_icon = reinterpret_cast<AddIcon>(api->Symbol(mod, "friend_unit_gift.ui_add_icon"));
    button_vtable = api->Symbol(mod, "hangar.ui_button_vtable");
    return add && find && text && skin && empty_skin && add_icon && button_vtable;
}

bool Panel::Add(const wchar_t* name, const wchar_t* label, Action action, int x, int y, int w, int h) {
    const std::wstring key = name;
    const auto native = ViewOf(key);
    bindings_.add(dialog_, &native, x, y, w, h, 0);
    auto* child = bindings_.find(dialog_, &native, 4);
    if (!child || Read<void*>(child) != bindings_.button_vtable || !ChildOf(child, dialog_)) return false;
    // Keep every child private until the whole panel is ready; fresh controls
    // default to visible and draw a debug quad (+0x168) until cleared.
    Put<std::uint8_t>(child, 0x162, 0);
    Put<std::uint8_t>(child, 0x163, 0);
    Put<std::uint8_t>(child, 0x167, 0);
    Put<std::uint8_t>(child, 0x168, 0);
    if (action != Action::none && skin_source_) {
        auto** first = Read<std::uint8_t**>(skin_source_, 0x1A4);
        auto** last = Read<std::uint8_t**>(skin_source_, 0x1A8);
        const auto count = first && last && last >= first ? static_cast<std::size_t>(last - first) : 0;
        if (count < 3 || count > 16) return false;
        for (std::size_t index = 0; index < 3; ++index) {
            if (!first[index]) return false;
            if (IsBlankSkin(first[index])) {
                bindings_.empty_skin(child);
                continue;
            }
            if (!Read<void*>(first[index])) return false;
            bindings_.skin(child, reinterpret_cast<const NativeString*>(first[index] + 4),
                           Read<std::uint8_t>(first[index], 0x44), Read<std::uint32_t>(first[index], 0x3C),
                           Read<std::uint32_t>(first[index], 0x40));
        }
    } else if (action != Action::none) {
        // No button to copy on this page: use the client's own blue button
        // sprites (normal, hover with a 4 px inset, pressed).
        for (int index = 0; index < 3; ++index) {
            const std::wstring path = L"sprs\\" + std::to_wstring(17530 + index) + L".spr";
            const auto native_path = ViewOf(path);
            bindings_.skin(child, &native_path, 0, index == 1 ? 4u : 0u, index == 1 ? 4u : 0u);
        }
        // These sprites draw at their natural 82x28 size; scale this
        // control's instances (normal, hover, pressed) to the control size.
        auto** first = Read<std::uint8_t**>(child, 0x1A4);
        auto** last = Read<std::uint8_t**>(child, 0x1A8);
        for (auto** entry = first; first && last && entry < last && entry < first + 3; ++entry) {
            auto* sprite = *entry ? Read<std::uint8_t*>(*entry) : nullptr;
            if (!sprite) continue;
            Put<float>(sprite, 0x94, static_cast<float>(w) / kButtonSpriteWidth);
            Put<float>(sprite, 0x98, static_cast<float>(h) / kButtonSpriteHeight);
            Put<std::uint8_t>(sprite, 0xA0, 1);
        }
    }
    controls_.push_back({child, action, w, {}, 0, false});
    SetText(controls_.back(), label, text_color_);
    return true;
}

bool Panel::Attach(void* service, void* dialog, void* skin_source, const Bindings& bindings, int left, int top) {
    Detach();
    last_failure_ = nullptr;
    bindings_ = bindings;
    if (!service || !dialog || !bindings_.add || !bindings_.find || !bindings_.text || !bindings_.button_vtable) {
        last_failure_ = "missing native UI bindings";
        return false;
    }
    const int width = Read<int>(dialog, 0x154);
    const int height = Read<int>(dialog, 0x158);
    if (width < kOverlayWidth || width > 4096 || height < kOverlayHeight || height > 4096) {
        last_failure_ = "page dialog size outside the supported range";
        return false;
    }
    if (skin_source && (Read<void*>(skin_source) != bindings_.button_vtable || !ChildOf(skin_source, dialog)))
        skin_source = nullptr;
    skin_source_ = skin_source;
    if (skin_source_) {
        auto** begin = Read<std::uint8_t**>(skin_source_, 0x190);
        auto** end = Read<std::uint8_t**>(skin_source_, 0x194);
        if (begin && end && end > begin && *begin) {
            font_ = Read<std::uint32_t>(*begin, 0x6C);
            text_color_ = Read<std::uint32_t>(*begin, 0x58);
            shadow_color_ = Read<std::uint32_t>(*begin, 0x5C);
        }
    }
    service_ = service;
    dialog_ = dialog;
    open_ = false;
    constexpr int kRow = 22, kLabelW = 28, kValueW = 169, kButtonW = 26, kWideW = 126, kLine = 18;
    const int x0 = std::clamp(left, 0, std::max(0, width - kOverlayWidth));
    const int y0 = std::clamp(top, 0, std::max(0, height - kOverlayHeight));
    bool ok = Add(L"ModOcToggle", L"OC 六维强化", Action::panel_toggle, x0 + kOverlayWidth - 110, y0, 110, 20);
    // Opaque backdrop so the overlay reads cleanly over the native item area.
    {
        const std::wstring key = L"ModOcBackground";
        const auto native = ViewOf(key);
        const std::wstring path = L"sprs\\17459.spr";
        const auto native_path = ViewOf(path);
        bindings_.add_icon(dialog_, &native, x0, y0 + 24, kOverlayWidth, kOverlayHeight - 24);
        auto* icon = bindings_.find(dialog_, &native, 1);
        if (icon && ChildOf(icon, dialog_)) {
            Put<std::uint8_t>(icon, 0x162, 0);
            Put<std::uint8_t>(icon, 0x163, 0);
            Put<std::uint8_t>(icon, 0x167, 0);
            Put<std::uint8_t>(icon, 0x168, 0);
            bindings_.skin(icon, &native_path, 0, 0, 0);
            auto** skins = Read<std::uint8_t**>(icon, 0x1A4);
            auto* sprite = skins && *skins ? Read<std::uint8_t*>(*skins) : nullptr;
            if (sprite) {
                // Base sprite is 262x61; only this instance is scaled.
                Put<float>(sprite, 0x94, static_cast<float>(kOverlayWidth) / 262.0f);
                Put<float>(sprite, 0x98, static_cast<float>(kOverlayHeight - 24) / 61.0f);
                Put<std::uint8_t>(sprite, 0xA0, 1);
                background_ = {icon, Action::none, kOverlayWidth, {}, 0, false};
                background_ready_ = true;
            }
        }
    }
    const int x = x0 + 6;
    const int y = y0 + 28;
    ok = ok && Add(L"ModOcTitle", L"", Action::none, x, y, kInner, kLine);
    ok = ok && Add(L"ModOcPoints", L"", Action::none, x, y + 20, kInner, kLine);
    for (std::size_t stat = 0; ok && stat < stat_count; ++stat) {
        const int row_y = y + 44 + kRow * static_cast<int>(stat);
        const std::wstring name = L"ModOcName" + std::to_wstring(stat);
        const std::wstring value = L"ModOcValue" + std::to_wstring(stat);
        const std::wstring minus = L"ModOcMinus" + std::to_wstring(stat);
        const std::wstring plus = L"ModOcPlus" + std::to_wstring(stat);
        ok = Add(name.c_str(), kStatNames[stat], Action::none, x, row_y, kLabelW, kLine) &&
             Add(value.c_str(), L"", Action::none, x + kLabelW + 2, row_y, kValueW, kLine) &&
             Add(minus.c_str(), L"-", static_cast<Action>(static_cast<int>(Action::minus0) + static_cast<int>(stat)),
                 x + kLabelW + kValueW + 6, row_y, kButtonW, kLine) &&
             Add(plus.c_str(), L"+", static_cast<Action>(static_cast<int>(Action::plus0) + static_cast<int>(stat)),
                 x + kLabelW + kValueW + kButtonW + 10, row_y, kButtonW, kLine);
    }
    const int buttons_y = y + 44 + kRow * 6 + 4;
    ok = ok && Add(L"ModOcOverToggle", L"", Action::over_toggle, x, buttons_y, kWideW, 20) &&
         Add(L"ModOcApply", L"", Action::apply, x + kWideW + 7, buttons_y, kWideW, 20) &&
         Add(L"ModOcReset", L"", Action::reset, x, buttons_y + 24, kWideW, 20) &&
         Add(L"ModOcChallenge", L"", Action::challenge, x + kWideW + 7, buttons_y + 24, kWideW, 20) &&
         Add(L"ModOcRefresh", L"", Action::refresh, x, buttons_y + 48, kWideW, 20) &&
         Add(L"ModOcStatus", L"", Action::none, x, buttons_y + 72, kInner, kLine) &&
         Add(L"ModOcStatus2", L"", Action::none, x, buttons_y + 92, kInner, kLine);
    if (!ok) {
        for (auto& control : controls_) Put<std::uint8_t>(control.pointer, 0x162, 0);
        if (background_ready_) Put<std::uint8_t>(background_.pointer, 0x162, 0);
        Detach();
        last_failure_ = "a native button could not be created on the page dialog";
        return false;
    }
    std::size_t index = 0;
    toggle_ = &controls_[index++];
    title_ = &controls_[index++];
    points_ = &controls_[index++];
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        ++index; // name
        value_labels_[stat] = &controls_[index++];
        minus_buttons_[stat] = &controls_[index++];
        plus_buttons_[stat] = &controls_[index++];
    }
    over_toggle_ = &controls_[index++];
    apply_ = &controls_[index++];
    reset_ = &controls_[index++];
    challenge_ = &controls_[index++];
    refresh_ = &controls_[index++];
    status_ = &controls_[index++];
    status2_ = &controls_[index++];
    return true;
}

void Panel::Detach() noexcept {
    service_ = dialog_ = skin_source_ = nullptr;
    controls_.clear();
    background_ = {};
    background_ready_ = false;
    open_ = false;
    toggle_ = nullptr;
    value_labels_ = {};
    minus_buttons_ = {};
    plus_buttons_ = {};
    title_ = points_ = status_ = status2_ = apply_ = reset_ = challenge_ = refresh_ = over_toggle_ = nullptr;
}

bool Panel::Alive() const noexcept {
    if (!dialog_ || !service_ || controls_.empty()) return false;
    __try {
        return Read<void*>(service_, 0x28) == dialog_ && Read<void*>(controls_[0].pointer, 0x15C) == dialog_ &&
               Read<void*>(controls_[0].pointer) == bindings_.button_vtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Action Panel::Click(void* control) const noexcept {
    if (!control || !Alive()) return Action::none;
    for (const auto& entry : controls_) {
        if (entry.pointer != control) continue;
        __try {
            if (Read<std::uint8_t>(control, 0x162) && Read<std::uint8_t>(control, 0x167)) return entry.action;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Action::none;
        }
        return Action::none;
    }
    return Action::none;
}

void Panel::SetText(Control& control, const std::wstring& value, std::uint32_t color) {
    if (control.set && control.text == value && control.color == color) return;
    const auto native = ViewOf(value);
    const int align = control.action == Action::none ? 1 : 2;
    bindings_.text(control.pointer, 0, &native, 2, 3, std::max(1, control.width - 4), 14, color, shadow_color_, 0,
                   font_, align);
    control.text = value;
    control.color = color;
    control.set = true;
}

void Panel::SetState(Control& control, bool visible, bool enabled) noexcept {
    __try {
        if (!visible || !enabled) {
            Put<std::uint16_t>(control.pointer, 0x21C, 0);
            Put<std::uint8_t>(control.pointer, 0x164, 0);
            Put<std::uint32_t>(control.pointer, 0x18C, 0);
        }
        Put<std::uint8_t>(control.pointer, 0x162, visible ? 1 : 0);
        Put<std::uint8_t>(control.pointer, 0x163, visible ? 1 : 0);
        Put<std::uint8_t>(control.pointer, 0x167, enabled ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Panel::Render(const View& view) {
    if (!Alive()) return;
    const std::uint32_t body = 0xFFE3EEF6;
    const std::uint32_t dim = 0xFF70808A;
    const std::uint32_t accent = 0xFFFFD27A;
    const std::uint32_t warn = 0xFFFF8A7A;
    const bool interactive = view.server_oc_mode && view.profile_present && !view.busy;
    SetText(*toggle_, open_ ? L"收起 OC" : L"OC 六维强化", text_color_);
    SetState(*toggle_, true, true);
    if (background_ready_) SetState(background_, open_, false);
    if (!open_) {
        for (auto& control : controls_)
            if (&control != toggle_) SetState(control, false, false);
        return;
    }
    SetText(*title_, view.unit_caption.empty() ? L"OC 六维强化" : L"OC 六维强化 · " + view.unit_caption, accent);
    std::wstring points;
    if (!view.server_oc_mode) {
        points = L"服务器未启用 OC 模式";
    } else if (!view.profile_present) {
        points = L"OC 档未读取";
    } else {
        points = L"OC Lv" + std::to_wstring(view.oc_level) + L" 机体 Lv" + std::to_wstring(view.native_level) +
                 L"  普通 " + std::to_wstring(view.normal_points_left) + L"  Over " +
                 std::to_wstring(view.over_points_left) + (view.over_mode ? L"  [Over]" : L"  [普通]");
        if (view.dirty) points += L" 余" + std::to_wstring(view.pending_left);
    }
    SetText(*points_, points, body);
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        std::wstring value;
        bool over_cap = false;
        if (view.profile_present) {
            const int normal = view.over_mode ? view.normal[stat] : view.pending_normal[stat];
            const int over = view.over_mode ? view.pending_over[stat] : view.over[stat];
            const int from_normal = normal * view.normal_gain[stat];
            const int from_over = over * view.over_gain[stat];
            const int total = view.base[stat] + from_normal + from_over;
            over_cap = total > kStatCaps[stat];
            // points normal|over, then base + normal gain + over gain = total
            value = std::to_wstring(normal) + L"|" + std::to_wstring(over) + L"  " + std::to_wstring(view.base[stat]) +
                    L"+" + std::to_wstring(from_normal) + L"+" + std::to_wstring(from_over) + L"=" +
                    std::to_wstring(total);
        }
        SetText(*value_labels_[stat], value, over_cap ? warn : body);
        const auto current = view.over_mode ? view.pending_over[stat] : view.pending_normal[stat];
        const bool can_minus = interactive && current > 0;
        const bool can_plus = interactive && view.pending_left > 0;
        SetText(*minus_buttons_[stat], L"-", can_minus ? text_color_ : dim);
        SetText(*plus_buttons_[stat], L"+", can_plus ? text_color_ : dim);
        SetState(*minus_buttons_[stat], true, can_minus);
        SetState(*plus_buttons_[stat], true, can_plus);
    }
    SetText(*over_toggle_, view.over_mode ? L"编辑普通点" : L"编辑 Over 点", interactive ? text_color_ : dim);
    SetState(*over_toggle_, true, interactive);
    SetText(*apply_, L"确认加点", interactive && view.dirty ? text_color_ : dim);
    SetState(*apply_, true, interactive && view.dirty);
    SetText(*reset_, view.reset_caption.empty() ? L"洗点" : view.reset_caption, interactive ? text_color_ : dim);
    SetState(*reset_, true, interactive);
    SetText(*challenge_, view.challenge_caption.empty() ? L"OC 挑战" : view.challenge_caption,
            interactive && view.challenge_available ? text_color_ : dim);
    SetState(*challenge_, true, interactive && view.challenge_available);
    SetText(*refresh_, L"刷新", !view.busy ? text_color_ : dim);
    SetState(*refresh_, true, !view.busy);
    std::wstring status = view.status;
    std::wstring second;
    if (status.size() > 22) {
        second = status.substr(22);
        status.resize(22);
    }
    SetText(*status_, status, body);
    SetText(*status2_, second, body);
    SetState(*title_, true, false);
    SetState(*points_, true, false);
    SetState(*status_, true, false);
    SetState(*status2_, true, false);
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        SetState(controls_[3 + stat * 4], true, false);
        SetState(*value_labels_[stat], true, false);
    }
}

} // namespace sdgo::oc_custom::client::ui
