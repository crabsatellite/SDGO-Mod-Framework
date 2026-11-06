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

bool Bindings::Bind(const SdgoApi* api, SdgoMod* mod) noexcept {
    if (!api || !mod || !api->Symbol) return false;
    add = reinterpret_cast<Add>(api->Symbol(mod, "hangar.ui_add_button"));
    find = reinterpret_cast<Find>(api->Symbol(mod, "hangar.ui_find_control"));
    text = reinterpret_cast<Text>(api->Symbol(mod, "hangar.ui_text_item"));
    skin = reinterpret_cast<Skin>(api->Symbol(mod, "hangar.ui_add_skin"));
    empty_skin = reinterpret_cast<EmptySkin>(api->Symbol(mod, "friend_unit_gift.ui_add_empty_skin"));
    button_vtable = api->Symbol(mod, "hangar.ui_button_vtable");
    return add && find && text && skin && empty_skin && button_vtable;
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
    }
    controls_.push_back({child, action, w, {}, 0, false});
    SetText(controls_.back(), label, text_color_);
    return true;
}

bool Panel::Attach(void* service, void* dialog, void* skin_source, const Bindings& bindings, int left, int top) {
    Detach();
    bindings_ = bindings;
    if (!service || !dialog || !bindings_.add || !bindings_.find || !bindings_.text || !bindings_.button_vtable)
        return false;
    const int width = Read<int>(dialog, 0x154);
    const int height = Read<int>(dialog, 0x158);
    if (width < 400 || width > 4096 || height < 300 || height > 4096) return false;
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
    constexpr int kRow = 24, kLabelW = 48, kValueW = 196, kButtonW = 26, kWideW = 118;
    const int x = std::clamp(left, 0, std::max(0, width - 440));
    const int y = std::clamp(top, 0, std::max(0, height - 300));
    bool ok = Add(L"ModOcTitle", L"OC 六维强化", Action::none, x, y, 420, 20);
    ok = ok && Add(L"ModOcPoints", L"", Action::none, x, y + kRow, 420, 20);
    for (std::size_t stat = 0; ok && stat < stat_count; ++stat) {
        const int row_y = y + kRow * (2 + static_cast<int>(stat));
        const std::wstring name = L"ModOcName" + std::to_wstring(stat);
        const std::wstring value = L"ModOcValue" + std::to_wstring(stat);
        const std::wstring minus = L"ModOcMinus" + std::to_wstring(stat);
        const std::wstring plus = L"ModOcPlus" + std::to_wstring(stat);
        ok = Add(name.c_str(), kStatNames[stat], Action::none, x, row_y, kLabelW, 20) &&
             Add(value.c_str(), L"", Action::none, x + kLabelW + 4, row_y, kValueW, 20) &&
             Add(minus.c_str(), L"-", static_cast<Action>(static_cast<int>(Action::minus0) + static_cast<int>(stat)),
                 x + kLabelW + kValueW + 12, row_y, kButtonW, 20) &&
             Add(plus.c_str(), L"+", static_cast<Action>(static_cast<int>(Action::plus0) + static_cast<int>(stat)),
                 x + kLabelW + kValueW + kButtonW + 18, row_y, kButtonW, 20);
    }
    const int buttons_y = y + kRow * 8 + 4;
    ok = ok && Add(L"ModOcOverToggle", L"切换 Over", Action::over_toggle, x, buttons_y, kWideW, 22) &&
         Add(L"ModOcApply", L"确认加点", Action::apply, x + kWideW + 8, buttons_y, kWideW, 22) &&
         Add(L"ModOcReset", L"洗点", Action::reset, x + 2 * (kWideW + 8), buttons_y, kWideW, 22) &&
         Add(L"ModOcChallenge", L"OC 挑战", Action::challenge, x, buttons_y + 28, kWideW, 22) &&
         Add(L"ModOcRefresh", L"刷新", Action::refresh, x + kWideW + 8, buttons_y + 28, kWideW, 22) &&
         Add(L"ModOcStatus", L"", Action::none, x, buttons_y + 56, 420, 20) &&
         Add(L"ModOcStatus2", L"", Action::none, x, buttons_y + 78, 420, 20);
    if (!ok) {
        for (auto& control : controls_) Put<std::uint8_t>(control.pointer, 0x162, 0);
        Detach();
        return false;
    }
    std::size_t index = 0;
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
    const bool interactive = view.server_oc_mode && view.profile_present && !view.busy;
    const bool clickable = skin_source_ != nullptr;
    SetText(*title_, view.unit_caption.empty() ? L"OC 六维强化" : L"OC 六维强化 · " + view.unit_caption, accent);
    std::wstring points;
    if (!view.server_oc_mode) {
        points = L"服务器未启用 OC 模式";
    } else if (!view.profile_present) {
        points = L"OC 档未读取";
    } else {
        points = L"OC 等级 " + std::to_wstring(view.oc_level) + L"（机体等级 " + std::to_wstring(view.native_level) +
                 L"）  普通点 " + std::to_wstring(view.normal_points_left) + L"  Over 点 " +
                 std::to_wstring(view.over_points_left) +
                 (view.over_mode ? L"  [编辑 Over]" : L"  [编辑普通]") +
                 (view.dirty ? L"  待分配 " + std::to_wstring(view.pending_left) : L"");
    }
    SetText(*points_, points, body);
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        std::wstring value;
        if (view.profile_present) {
            const auto normal = view.over_mode ? view.normal[stat] : view.pending_normal[stat];
            const auto over = view.over_mode ? view.pending_over[stat] : view.over[stat];
            const auto total = view.base[stat] + normal * view.normal_gain[stat] + over * view.over_gain[stat];
            value = std::to_wstring(view.base[stat]) + L" + " + std::to_wstring(normal) + L"×" +
                    std::to_wstring(view.normal_gain[stat]) + L" + " + std::to_wstring(over) + L"×" +
                    std::to_wstring(view.over_gain[stat]) + L" = " + std::to_wstring(total);
            if (total > kStatCaps[stat]) value += L" (超上限 " + std::to_wstring(kStatCaps[stat]) + L")";
        }
        SetText(*value_labels_[stat], value, body);
        const auto current = view.over_mode ? view.pending_over[stat] : view.pending_normal[stat];
        const bool can_minus = interactive && current > 0;
        const bool can_plus = interactive && view.pending_left > 0;
        SetText(*minus_buttons_[stat], L"-", can_minus ? text_color_ : dim);
        SetText(*plus_buttons_[stat], L"+", can_plus ? text_color_ : dim);
        SetState(*minus_buttons_[stat], true, can_minus && clickable);
        SetState(*plus_buttons_[stat], true, can_plus && clickable);
    }
    SetText(*over_toggle_, view.over_mode ? L"编辑普通点" : L"编辑 Over 点", interactive ? text_color_ : dim);
    SetState(*over_toggle_, true, interactive && clickable);
    SetText(*apply_, L"确认加点", interactive && view.dirty ? text_color_ : dim);
    SetState(*apply_, true, interactive && view.dirty && clickable);
    SetText(*reset_, view.reset_caption.empty() ? L"洗点" : view.reset_caption, interactive ? text_color_ : dim);
    SetState(*reset_, true, interactive && clickable);
    SetText(*challenge_, view.challenge_caption.empty() ? L"OC 挑战" : view.challenge_caption,
            interactive && view.challenge_available ? text_color_ : dim);
    SetState(*challenge_, true, interactive && view.challenge_available && clickable);
    SetText(*refresh_, L"刷新", !view.busy ? text_color_ : dim);
    SetState(*refresh_, true, !view.busy && clickable);
    std::wstring status = view.status;
    std::wstring second;
    if (status.size() > 30) {
        second = status.substr(30);
        status.resize(30);
    }
    SetText(*status_, status, body);
    SetText(*status2_, second, body);
    SetState(*title_, true, false);
    SetState(*points_, true, false);
    SetState(*status_, true, false);
    SetState(*status2_, true, false);
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        SetState(controls_[2 + stat * 4], true, false);
        SetState(*value_labels_[stat], true, false);
    }
}

} // namespace sdgo::oc_custom::client::ui
