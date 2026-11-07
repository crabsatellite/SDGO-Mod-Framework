#include "original_ui.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

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

// Native control types of FindControl (and the .uir records).
constexpr int kIcon = 1;
constexpr int kStatic = 3;
constexpr int kButton = 4;
constexpr int kCheck = 10;

// CControl layout used by the upgrade page and the existing native-UI Mods.
constexpr std::size_t kVisible = 0x162;
constexpr std::size_t kVisible2 = 0x163;
constexpr std::size_t kHover = 0x164;
constexpr std::size_t kEnabled = 0x167;
constexpr std::size_t kDebugQuad = 0x168;
constexpr std::size_t kPressed = 0x21C;    // button press state; check box checked flag
constexpr std::size_t kCapture = 0x18C;
constexpr std::size_t kParent = 0x15C;
// CStatic::SetText(const std::wstring&) and CCheckBox::SetCheck(bool, int)
// share vtable slot 0x88 (the upgrade page Init and the room options use them).
constexpr std::size_t kSetTextSlot = 0x88;

using SetTextFn = void(__thiscall*)(void*, const NativeString*);
using SetCheckFn = void(__thiscall*)(void*, int, int);
// Service::LoadDialog(const std::wstring& name, const std::wstring& path):
// creates the dialog, loads the .uir, routes its events to the service and
// files it in the service's dialog map under the name (vtable +0x58).
using LoadDialogFn = void*(__thiscall*)(void*, const NativeString*, const NativeString*);
constexpr std::size_t kLoadDialogSlot = 0x58;
constexpr std::size_t kServiceFromComponent = 0xA8;
constexpr std::size_t kComponentDialog = 0x28;

struct Spec {
    std::uint8_t window;
    const wchar_t* name;
    int type;
    Action action;
};

// Every control the original layouts declare that the OC flow drives. Names,
// types and windows come from the shipped 0794/0813/0819 records.
const Spec kSpecs[] = {
    {window_over, L"ExBtn", kButton, Action::ex},
    {window_over, L"ExIcon", kIcon, Action::none},
    {window_over, L"CustomIcon", kIcon, Action::none},
    {window_over, L"BackIcon1", kIcon, Action::none},
    {window_over, L"NoticeStatic", kStatic, Action::none},
    {window_over, L"NomalPointStatic", kStatic, Action::none},
    {window_over, L"OverPointStatic", kStatic, Action::none},
    {window_over, L"PlusStatic0", kStatic, Action::none},
    {window_over, L"PlusStatic1", kStatic, Action::none},
    {window_over, L"PlusStatic2", kStatic, Action::none},
    {window_over, L"PlusStatic3", kStatic, Action::none},
    {window_over, L"PlusStatic4", kStatic, Action::none},
    {window_over, L"PlusStatic5", kStatic, Action::none},
    {window_over, L"PlusStatic6", kStatic, Action::none},
    {window_over, L"SpeedUpBtn", kButton, Action::up0},
    {window_over, L"HpUpBtn", kButton, Action::up1},
    {window_over, L"AtkUpBtn", kButton, Action::up2},
    {window_over, L"DefUpBtn", kButton, Action::up3},
    {window_over, L"AgiUpBtn", kButton, Action::up4},
    {window_over, L"SpecialUpBtn", kButton, Action::up5},
    {window_over, L"SpeedStatic", kStatic, Action::none},
    {window_over, L"HpStatic", kStatic, Action::none},
    {window_over, L"AtkStatic", kStatic, Action::none},
    {window_over, L"DefStatic", kStatic, Action::none},
    {window_over, L"AgiStatic", kStatic, Action::none},
    {window_over, L"SpecialStatic", kStatic, Action::none},
    {window_over, L"SpeedPointStatic", kStatic, Action::none},
    {window_over, L"HpPointStatic", kStatic, Action::none},
    {window_over, L"AtkPointStatic", kStatic, Action::none},
    {window_over, L"DefPointStatic", kStatic, Action::none},
    {window_over, L"AgiPointStatic", kStatic, Action::none},
    {window_over, L"SpecialPointStatic", kStatic, Action::none},
    {window_over, L"SpeedOverPointStatic", kStatic, Action::none},
    {window_over, L"HpOverPointStatic", kStatic, Action::none},
    {window_over, L"AtkOverPointStatic", kStatic, Action::none},
    {window_over, L"DefOverPointStatic", kStatic, Action::none},
    {window_over, L"AgiOverPointStatic", kStatic, Action::none},
    {window_over, L"SpecialOverPointStatic", kStatic, Action::none},
    {window_over, L"ResetBtn", kButton, Action::reset},
    {window_challenge, L"TitleStatic", kStatic, Action::none},
    {window_challenge, L"WorkUnitName", kStatic, Action::none},
    {window_challenge, L"WorkStateText", kStatic, Action::none},
    {window_challenge, L"TargetLevelTittle", kStatic, Action::none},
    {window_challenge, L"LevelName", kStatic, Action::none},
    {window_challenge, L"MessageStatic0", kStatic, Action::none},
    {window_challenge, L"OverCustomTittle", kStatic, Action::none},
    {window_challenge, L"MessageStatic1", kStatic, Action::none},
    {window_challenge, L"StartBtn", kButton, Action::challenge_start},
    {window_challenge, L"CloseBtn", kButton, Action::challenge_close},
    {window_challenge, L"LeftBtn", kButton, Action::challenge_left},
    {window_challenge, L"RightBtn", kButton, Action::challenge_right},
    {window_challenge, L"UseItemCheck0", kCheck, Action::item0},
    {window_challenge, L"UseItemCheck1", kCheck, Action::item1},
    {window_challenge, L"UseItemCheck2", kCheck, Action::item2},
    {window_challenge, L"UseItemCheck3", kCheck, Action::item3},
    {window_challenge, L"UseItemCheck4", kCheck, Action::item4},
    {window_challenge, L"UseItemCheck5", kCheck, Action::item5},
    {window_challenge, L"UseItemCheck6", kCheck, Action::item6},
    {window_challenge, L"UseItemCheck7", kCheck, Action::item7},
    {window_challenge, L"UseItemCheck8", kCheck, Action::item8},
    {window_challenge, L"UseItemCheck9", kCheck, Action::item9},
    {window_confirm, L"NoticeStatic0", kStatic, Action::none},
    {window_confirm, L"NoticeStatic1", kStatic, Action::none},
    {window_confirm, L"OvercustomExIcon", kIcon, Action::none},
    {window_confirm, L"OKBtn", kButton, Action::confirm_ok},
    {window_confirm, L"CancelBtn", kButton, Action::confirm_cancel},
};

struct WindowFile {
    const wchar_t* registry; // unique key in the service's dialog map
    const wchar_t* path;
};
const WindowFile kWindows[window_count] = {
    {L"ocdlg0794", L"uirs\\0794.uir"},
    {L"ocdlg0813", L"uirs\\0813.uir"},
    {L"ocdlg0819", L"uirs\\0819.uir"},
};

// Stat order of the protocol (speed, hp, attack, defense, agility, special)
// mapped to the 0794 control name prefixes.
const wchar_t* const kStatPrefix[stat_count] = {L"Speed", L"Hp", L"Atk", L"Def", L"Agi", L"Special"};
// The "+" between normal and Over points (0: remaining points, 1..6: stats).
const wchar_t* const kPlusStatics[] = {L"PlusStatic0", L"PlusStatic1", L"PlusStatic2", L"PlusStatic3",
                                       L"PlusStatic4", L"PlusStatic5", L"PlusStatic6"};

int WindowIndex(const char* window) noexcept {
    if (!window) return -1;
    if (!std::strcmp(window, "over")) return window_over;
    if (!std::strcmp(window, "challenge")) return window_challenge;
    if (!std::strcmp(window, "confirm")) return window_confirm;
    return -1;
}

constexpr std::uint32_t kLabelColor = 0xFFFFFFFF;
constexpr std::uint32_t kShadowColor = 0xFF000000;

} // namespace

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

bool Bindings::Bind(const SdgoApi* api, SdgoMod* mod) noexcept {
    if (!api || !mod || !api->Symbol) return false;
    add = reinterpret_cast<Add>(api->Symbol(mod, "hangar.ui_add_button"));
    find = reinterpret_cast<Find>(api->Symbol(mod, "hangar.ui_find_control"));
    text = reinterpret_cast<Text>(api->Symbol(mod, "hangar.ui_text_item"));
    skin = reinterpret_cast<Skin>(api->Symbol(mod, "hangar.ui_add_skin"));
    add_icon = reinterpret_cast<AddIcon>(api->Symbol(mod, "friend_unit_gift.ui_add_icon"));
    button_vtable = api->Symbol(mod, "hangar.ui_button_vtable");
    return add && find && text && skin && add_icon && button_vtable;
}

bool OriginalUi::Load(void* component, const Bindings& bindings) {
    Detach();
    last_failure_ = nullptr;
    bindings_ = bindings;
    if (!component || !bindings_.find) {
        last_failure_ = "missing native UI bindings";
        return false;
    }
    void* service = static_cast<std::uint8_t*>(component) - kServiceFromComponent;
    void* page = Read<void*>(component, kComponentDialog);
    if (!page) {
        last_failure_ = "the upgrade page has no dialog";
        return false;
    }
    auto** vtable = Read<void**>(service);
    auto load = reinterpret_cast<LoadDialogFn>(vtable[kLoadDialogSlot / sizeof(void*)]);
    std::array<void*, window_count> dialogs{};
    for (std::size_t index = 0; index < window_count; ++index) {
        const std::wstring name = kWindows[index].registry;
        const std::wstring path = kWindows[index].path;
        const auto native_name = ViewOf(name);
        const auto native_path = ViewOf(path);
        dialogs[index] = load(service, &native_name, &native_path);
        if (!dialogs[index]) {
            last_failure_ = "an original OC layout did not load";
            return false;
        }
        // Loaded dialogs start visible; keep them closed until the flow opens them.
        Put<std::uint8_t>(dialogs[index], kVisible, 0);
    }
    std::array<Bound, 80> bound{};
    std::size_t count = 0;
    for (const auto& spec : kSpecs) {
        const std::wstring name = spec.name;
        const auto native = ViewOf(name);
        void* control = bindings_.find(dialogs[spec.window], &native, spec.type);
        void* parent = control ? Read<void*>(control, kParent) : nullptr;
        // Controls sit on the loaded dialog or on the layout's inner Dlg record.
        const bool owned = parent && (parent == dialogs[spec.window] || Read<void*>(parent, kParent) == dialogs[spec.window]);
        if (!control || !owned || count >= bound.size()) {
            std::snprintf(failure_, sizeof(failure_), "original control %ls (type %d) %s in %ls: control=%p parent=%p up=%p dialog=%p vt=%p",
                          spec.name, spec.type, control ? "has a foreign parent" : "not found", kWindows[spec.window].path,
                          control, parent, parent ? Read<void*>(parent, kParent) : nullptr, dialogs[spec.window],
                          control ? Read<void*>(control) : nullptr);
            last_failure_ = failure_;
            return false;
        }
        auto& entry = bound[count++];
        entry.window = spec.window;
        entry.name = spec.name;
        entry.type = spec.type;
        entry.action = spec.action;
        entry.pointer = control;
        entry.parent = parent;
        entry.width = Read<int>(control, 0x154);
    }
    component_ = component;
    service_ = service;
    page_dialog_ = page;
    dialogs_ = dialogs;
    bound_ = bound;
    bound_count_ = count;
    return true;
}

bool OriginalUi::AttachEntry() {
    if (!component_ || entry_button_) return entry_button_ != nullptr;
    if (!bindings_.add || !bindings_.add_icon || !bindings_.skin || !bindings_.button_vtable) {
        last_failure_ = "missing native control constructors";
        return false;
    }
    // OC-era uirs\0799.uir: BackIcon1 (22,217 84x24, sprs\8525.spr: the dim
    // OVER CUSTOM label) under OverCustomBtn (18,213 205x30, sprs\8526-8528).
    const std::wstring label_name = L"BackIcon1";
    const auto native_label = ViewOf(label_name);
    bindings_.add_icon(page_dialog_, &native_label, 22, 217, 84, 24);
    void* label = bindings_.find(page_dialog_, &native_label, kIcon);
    const std::wstring button_name = L"OverCustomBtn";
    const auto native_button = ViewOf(button_name);
    bindings_.add(page_dialog_, &native_button, 18, 213, 205, 30, 0);
    void* button = bindings_.find(page_dialog_, &native_button, kButton);
    if (!label || !button || Read<void*>(label, kParent) != page_dialog_ || Read<void*>(button, kParent) != page_dialog_ ||
        Read<void*>(button) != bindings_.button_vtable) {
        last_failure_ = "the OVER CUSTOM entry could not be created";
        return false;
    }
    for (void* control : {label, button}) {
        Put<std::uint8_t>(control, kVisible, 0);
        Put<std::uint8_t>(control, kVisible2, 0);
        Put<std::uint8_t>(control, kEnabled, 0);
        Put<std::uint8_t>(control, kDebugQuad, 0);
    }
    const std::wstring label_sprite = L"sprs\\8525.spr";
    const auto native_label_sprite = ViewOf(label_sprite);
    bindings_.skin(label, &native_label_sprite, 0, 0, 0);
    for (int state = 0; state < 3; ++state) {
        const std::wstring sprite = L"sprs\\" + std::to_wstring(8526 + state) + L".spr";
        const auto native_sprite = ViewOf(sprite);
        bindings_.skin(button, &native_sprite, 0, 0, 0);
    }
    entry_label_ = label;
    entry_button_ = button;
    entry_shown_ = false;
    return true;
}

void OriginalUi::Detach() noexcept {
    component_ = service_ = page_dialog_ = nullptr;
    dialogs_ = {};
    bound_ = {};
    bound_count_ = 0;
    entry_button_ = entry_label_ = nullptr;
    shown_ = {};
    entry_shown_ = false;
}

bool OriginalUi::Alive() const noexcept {
    if (!component_ || !page_dialog_) return false;
    __try {
        if (Read<void*>(component_, kComponentDialog) != page_dialog_) return false;
        for (std::size_t index = 0; index < bound_count_; index += 16)
            if (Read<void*>(bound_[index].pointer, kParent) != bound_[index].parent) return false;
        return !entry_button_ || Read<void*>(entry_button_, kParent) == page_dialog_;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

OriginalUi::Bound* OriginalUi::Find(std::uint8_t window, const wchar_t* name) noexcept {
    for (std::size_t index = 0; index < bound_count_; ++index)
        if (bound_[index].window == window && !std::wcscmp(bound_[index].name, name)) return &bound_[index];
    return nullptr;
}

const OriginalUi::Bound* OriginalUi::Find(std::uint8_t window, const wchar_t* name) const noexcept {
    for (std::size_t index = 0; index < bound_count_; ++index)
        if (bound_[index].window == window && !std::wcscmp(bound_[index].name, name)) return &bound_[index];
    return nullptr;
}

Action OriginalUi::Click(void* control) const noexcept {
    if (!control || !Alive()) return Action::none;
    __try {
        if (control == entry_button_)
            return Read<std::uint8_t>(control, kVisible) && Read<std::uint8_t>(control, kEnabled) ? Action::entry
                                                                                                   : Action::none;
        for (std::size_t index = 0; index < bound_count_; ++index) {
            const auto& entry = bound_[index];
            if (entry.pointer != control || entry.action == Action::none) continue;
            if (!shown_[entry.window]) return Action::none;
            if (!Read<std::uint8_t>(control, kVisible) || !Read<std::uint8_t>(control, kEnabled)) return Action::none;
            return entry.action;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return Action::none;
}

void OriginalUi::SetStatic(std::uint8_t window, const wchar_t* name, const std::wstring& value) {
    auto* control = Find(window, name);
    if (!control || (control->text_set && control->text == value)) return;
    const auto native = ViewOf(value);
    auto** vtable = Read<void**>(control->pointer);
    reinterpret_cast<SetTextFn>(vtable[kSetTextSlot / sizeof(void*)])(control->pointer, &native);
    control->text = value;
    control->text_set = true;
}

void OriginalUi::SetLabel(Bound& control, const std::wstring& value, int align) {
    if (control.text_set && control.text == value) return;
    // Text item 0 of a button or check box (the same call the native pages
    // use for captions drawn over sprites).
    const auto native = ViewOf(value);
    const int height = Read<int>(control.pointer, 0x158);
    const int left = control.type == kCheck ? 20 : 2;
    const int width = control.type == kCheck ? 360 : std::max(1, control.width - 4);
    bindings_.text(control.pointer, 0, &native, left, std::max(0, (height - 14) / 2), width, 14, kLabelColor,
                   kShadowColor, 0, 1, align);
    control.text = value;
    control.text_set = true;
}

void OriginalUi::SetState(void* control, bool visible, bool enabled) noexcept {
    __try {
        if (!visible || !enabled) {
            if (Read<void*>(control) == bindings_.button_vtable) Put<std::uint16_t>(control, kPressed, 0);
            Put<std::uint8_t>(control, kHover, 0);
            Put<std::uint32_t>(control, kCapture, 0);
        }
        Put<std::uint8_t>(control, kVisible, visible ? 1 : 0);
        Put<std::uint8_t>(control, kVisible2, visible ? 1 : 0);
        Put<std::uint8_t>(control, kEnabled, enabled ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void OriginalUi::SetCheck(void* control, bool checked) noexcept {
    __try {
        if ((Read<std::uint8_t>(control, kPressed) != 0) == checked) return;
        auto** vtable = Read<void**>(control);
        reinterpret_cast<SetCheckFn>(vtable[kSetTextSlot / sizeof(void*)])(control, checked ? 1 : 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void OriginalUi::HideAll() noexcept {
    __try {
        for (std::size_t index = 0; index < window_count; ++index) {
            if (dialogs_[index]) Put<std::uint8_t>(dialogs_[index], kVisible, 0);
            shown_[index] = false;
        }
        if (entry_button_) SetState(entry_button_, false, false);
        if (entry_label_) SetState(entry_label_, false, false);
        entry_shown_ = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void OriginalUi::Render(const EntryView& entry, const OverView& over, const ChallengeView& challenge,
                        const ConfirmView& confirm) {
    if (!Alive()) return;
    if (entry_button_) {
        SetState(entry_label_, entry.visible, false);
        SetState(entry_button_, entry.visible && entry.lit, entry.visible && entry.lit);
        entry_shown_ = entry.visible;
    }
    // 0794: allocation window.
    if (over.visible) {
        SetStatic(window_over, L"NoticeStatic", over.notice);
        SetStatic(window_over, L"NomalPointStatic", over.normal_left);
        SetStatic(window_over, L"OverPointStatic", over.over_left);
        for (const wchar_t* plus : kPlusStatics) SetStatic(window_over, plus, L"+");
        for (std::size_t stat = 0; stat < stat_count; ++stat) {
            const std::wstring prefix = kStatPrefix[stat];
            SetStatic(window_over, (prefix + L"Static").c_str(), over.label[stat]);
            SetStatic(window_over, (prefix + L"PointStatic").c_str(), over.normal[stat]);
            SetStatic(window_over, (prefix + L"OverPointStatic").c_str(), over.over[stat]);
            if (auto* button = Find(window_over, (prefix + L"UpBtn").c_str()))
                SetState(button->pointer, true, over.up[stat] && !confirm.visible);
        }
        if (auto* reset = Find(window_over, L"ResetBtn")) {
            SetLabel(*reset, over.reset, 2);
            SetState(reset->pointer, true, over.reset_enabled && !confirm.visible);
        }
        if (auto* ex = Find(window_over, L"ExBtn")) SetState(ex->pointer, true, over.ex_enabled && !confirm.visible);
    }
    if (dialogs_[window_over]) Put<std::uint8_t>(dialogs_[window_over], kVisible, over.visible ? 1 : 0);
    shown_[window_over] = over.visible;
    // 0813: challenge window.
    if (challenge.visible) {
        SetStatic(window_challenge, L"TitleStatic", challenge.title);
        SetStatic(window_challenge, L"WorkUnitName", challenge.unit);
        SetStatic(window_challenge, L"WorkStateText", challenge.state);
        SetStatic(window_challenge, L"TargetLevelTittle", challenge.target_title);
        SetStatic(window_challenge, L"LevelName", challenge.target);
        SetStatic(window_challenge, L"MessageStatic0", challenge.message0);
        SetStatic(window_challenge, L"OverCustomTittle", challenge.over_title);
        SetStatic(window_challenge, L"MessageStatic1", challenge.message1);
        if (auto* start = Find(window_challenge, L"StartBtn"))
            SetState(start->pointer, true, challenge.start_enabled && !confirm.visible);
        if (auto* close = Find(window_challenge, L"CloseBtn")) SetState(close->pointer, true, !confirm.visible);
        if (auto* left = Find(window_challenge, L"LeftBtn")) SetState(left->pointer, true, challenge.left_enabled);
        if (auto* right = Find(window_challenge, L"RightBtn")) SetState(right->pointer, true, challenge.right_enabled);
        for (std::size_t slot = 0; slot < kItemSlots; ++slot) {
            const std::wstring name = L"UseItemCheck" + std::to_wstring(slot);
            auto* check = Find(window_challenge, name.c_str());
            if (!check) continue;
            if (challenge.item_visible[slot]) SetLabel(*check, challenge.item_text[slot], 1);
            SetState(check->pointer, challenge.item_visible[slot], challenge.item_visible[slot] && !confirm.visible);
            SetCheck(check->pointer, challenge.item_visible[slot] && challenge.item_checked[slot]);
        }
    }
    if (dialogs_[window_challenge]) Put<std::uint8_t>(dialogs_[window_challenge], kVisible, challenge.visible ? 1 : 0);
    shown_[window_challenge] = challenge.visible;
    // 0819: confirmation popup.
    if (confirm.visible) {
        SetStatic(window_confirm, L"NoticeStatic0", confirm.line0);
        SetStatic(window_confirm, L"NoticeStatic1", confirm.line1);
        if (auto* ok = Find(window_confirm, L"OKBtn")) SetState(ok->pointer, true, true);
        if (auto* cancel = Find(window_confirm, L"CancelBtn")) SetState(cancel->pointer, confirm.cancel, confirm.cancel);
    }
    if (dialogs_[window_confirm]) Put<std::uint8_t>(dialogs_[window_confirm], kVisible, confirm.visible ? 1 : 0);
    shown_[window_confirm] = confirm.visible;
}

void* OriginalUi::Control(const char* window, const wchar_t* name) const noexcept {
    if (!name) return nullptr;
    if (window && !std::strcmp(window, "entry")) return !std::wcscmp(name, L"OverCustomBtn") ? entry_button_ : nullptr;
    const int index = WindowIndex(window);
    if (index < 0) return nullptr;
    const auto* control = Find(static_cast<std::uint8_t>(index), name);
    return control ? control->pointer : nullptr;
}

bool OriginalUi::Visible(const char* window) const noexcept {
    if (!Alive()) return false;
    if (window && !std::strcmp(window, "entry")) {
        __try {
            return entry_button_ && Read<std::uint8_t>(entry_button_, kVisible) &&
                   Read<std::uint8_t>(page_dialog_, kVisible);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    const int index = WindowIndex(window);
    if (index < 0 || !dialogs_[index]) return false;
    __try {
        return shown_[index] && Read<std::uint8_t>(dialogs_[index], kVisible);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace sdgo::oc_custom::client::ui
