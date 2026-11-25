#include "original_ui.h"
#include "radar.h"

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
constexpr int kRender = 5; // 3DRender
constexpr int kCheck = 10;
const wchar_t* const kRemoveNames[kInfoItems] = {L"OcRemoveItem0", L"OcRemoveItem1", L"OcRemoveItem2"};

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
    {window_ex, L"ExIcon0", kIcon, Action::none},
    {window_ex, L"ExIcon1", kIcon, Action::none},
    {window_ex, L"ExIcon2", kIcon, Action::none},
    {window_ex, L"ExIcon3", kIcon, Action::none},
    {window_ex, L"ExIcon4", kIcon, Action::none},
    {window_ex, L"ExIcon5", kIcon, Action::none},
    {window_ex, L"ExIcon6", kIcon, Action::none},
    {window_ex, L"ExIcon7", kIcon, Action::none},
    {window_ex, L"ExBtn0", kButton, Action::ex0},
    {window_ex, L"ExBtn1", kButton, Action::ex1},
    {window_ex, L"ExBtn2", kButton, Action::ex2},
    {window_ex, L"ExBtn3", kButton, Action::ex3},
    {window_ex, L"ExBtn4", kButton, Action::ex4},
    {window_ex, L"ExBtn5", kButton, Action::ex5},
    {window_ex, L"ExBtn6", kButton, Action::ex6},
    {window_ex, L"ExBtn7", kButton, Action::ex7},
    {window_ex, L"CloseBtn", kButton, Action::ex_close},
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
    // 0800: OVER CUSTOM level track (BackIcon0, sprs 8472) with the lit level
    // icons (sprs 8997..9003) and the ITEM box (BackIcon1, sprs 8485).
    {window_info, L"BackIcon0", kIcon, Action::none},
    {window_info, L"BackIcon1", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon0", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon1", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon2", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon3", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon4", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon5", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon6", kIcon, Action::none},
    {window_info, L"OverCustomSlotIcon7", kIcon, Action::none},
    {window_info, L"NoneIcon0", kIcon, Action::none},
    {window_info, L"NoneIcon1", kIcon, Action::none},
    {window_info, L"NoneIcon2", kIcon, Action::none},
    {window_info, L"ItemNameStatic0", kStatic, Action::none},
    {window_info, L"ItemNameStatic1", kStatic, Action::none},
    {window_info, L"ItemNameStatic2", kStatic, Action::none},
    {window_info, L"Item3DRender0", kRender, Action::none},
    {window_info, L"Item3DRender1", kRender, Action::none},
    {window_info, L"Item3DRender2", kRender, Action::none},
    // Original factory inventory: two columns, four rows, native item click events.
    {window_items, L"SlotIcon0", kIcon, Action::none},
    {window_items, L"Item3DRender0", kRender, Action::owned0},
    {window_items, L"ItemStatic0", kStatic, Action::none},
    {window_items, L"NumberStatic0", kStatic, Action::none},
    {window_items, L"SlotIcon1", kIcon, Action::none},
    {window_items, L"Item3DRender1", kRender, Action::owned1},
    {window_items, L"ItemStatic1", kStatic, Action::none},
    {window_items, L"NumberStatic1", kStatic, Action::none},
    {window_items, L"SlotIcon2", kIcon, Action::none},
    {window_items, L"Item3DRender2", kRender, Action::owned2},
    {window_items, L"ItemStatic2", kStatic, Action::none},
    {window_items, L"NumberStatic2", kStatic, Action::none},
    {window_items, L"SlotIcon3", kIcon, Action::none},
    {window_items, L"Item3DRender3", kRender, Action::owned3},
    {window_items, L"ItemStatic3", kStatic, Action::none},
    {window_items, L"NumberStatic3", kStatic, Action::none},
    {window_items, L"SlotIcon4", kIcon, Action::none},
    {window_items, L"Item3DRender4", kRender, Action::owned4},
    {window_items, L"ItemStatic4", kStatic, Action::none},
    {window_items, L"NumberStatic4", kStatic, Action::none},
    {window_items, L"SlotIcon5", kIcon, Action::none},
    {window_items, L"Item3DRender5", kRender, Action::owned5},
    {window_items, L"ItemStatic5", kStatic, Action::none},
    {window_items, L"NumberStatic5", kStatic, Action::none},
    {window_items, L"SlotIcon6", kIcon, Action::none},
    {window_items, L"Item3DRender6", kRender, Action::owned6},
    {window_items, L"ItemStatic6", kStatic, Action::none},
    {window_items, L"NumberStatic6", kStatic, Action::none},
    {window_items, L"SlotIcon7", kIcon, Action::none},
    {window_items, L"Item3DRender7", kRender, Action::owned7},
    {window_items, L"ItemStatic7", kStatic, Action::none},
    {window_items, L"NumberStatic7", kStatic, Action::none},
    {window_items, L"LeftBtn", kButton, Action::items_previous},
    {window_items, L"RightBtn", kButton, Action::items_next},
    {window_items, L"PageStatic", kStatic, Action::none},
};

struct WindowFile {
    const wchar_t* registry; // unique key in the service's dialog map
    const wchar_t* path;
};
const WindowFile kWindows[window_count] = {
    {L"ocdlg0794", L"uirs\\0794.uir"},
    {L"ocdlg0813", L"uirs\\0813.uir"},
    {L"ocdlg0819", L"uirs\\0819.uir"},
    {L"ocdlg0800", L"uirs\\0800.uir"},
    {L"ocdlg0792", L"uirs\\0792.uir"}, // native factory item inventory
    {L"ocdlg0822", L"uirs\\0822.uir"},
    {L"ocdlg1252", L"uirs\\1252.uir"}, // G改 1.0 page: the package's attribute page layout
};

// G改 1.0 page controls of 1252 that the Mod drives. The page is optional: when
// the layout or a control is missing the OC tab keeps working without it.
const Spec kG1Specs[] = {
    {window_g1, L"SaveBtn", kButton, Action::g1_save},
    {window_g1, L"SelectSkillResetBtn", kButton, Action::g1_revert},
    {window_g1, L"AllSkillResetBtn", kButton, Action::g1_reset},
    {window_g1, L"UnitLevelStatic", kStatic, Action::none},
    {window_g1, L"TypeStatic", kStatic, Action::none},
};

// The current system's controls of 1252 (weapon effects, the slot list, its
// tabs and totals), hidden on the G改 1.0 page.
const wchar_t* const kG1HiddenNames[] = {
    L"EmphasizeSkill2Icon0", L"EmphasizeSkill2Icon1", L"EmphasizeSkill2Icon2", L"EmphasizeSkill2Icon3",
    L"EmphasizeSkillIcon0", L"EmphasizeSkillIcon1", L"EmphasizeSkillIcon2", L"EmphasizeSkillIcon3",
    L"BeforeBgIcon0", L"BeforeBgIcon1", L"BeforeBgIcon2", L"BeforeBgIcon3",
    L"AfterBgIcon0", L"AfterBgIcon1", L"AfterBgIcon2", L"AfterBgIcon3", L"TotalBgIcon",
    L"BeforeIcon0", L"BeforeIcon1", L"BeforeIcon2", L"BeforeIcon3",
    L"AfterIcon0", L"AfterIcon1", L"AfterIcon2", L"AfterIcon3",
    L"SelectSkillSlotIcon0", L"SelectSkillSlotIcon1", L"SelectSkillSlotIcon2", L"SelectSkillSlotIcon3",
    L"SelectSkillSlotOutlineIcon0", L"SelectSkillSlotOutlineIcon1", L"SelectSkillSlotOutlineIcon2",
    L"SelectSkillSlotOutlineIcon3", L"AfterBlindIcon",
    L"MouseCheckIcon_0", L"MouseCheckIcon_1", L"MouseCheckIcon_2", L"MouseCheckIcon_3",
    L"XIcon0", L"XIcon1", L"XIcon2", L"XIcon3", L"GBlindIcon", L"GBlindStatic",
    L"SkillSlotNameStatic", L"BeforeStatic", L"AfterStatic", L"TabRadio0", L"TabRadio1",
    L"SelectSkill1Static0", L"SelectSkill1Static1", L"SelectSkill1Static2", L"SelectSkill1Static3",
    L"SelectSkill1Static4", L"SelectSkill1Static5", L"SelectSkill1Static6", L"SelectSkill1Static7",
    L"SelectSkill2Static0", L"SelectSkill2Static1", L"SelectSkill2Static2", L"SelectSkill2Static3",
    L"SelectSkill2Static4", L"SelectSkill2Static5", L"SelectSkill2Static6", L"SelectSkill2Static7",
    L"SelectSkill3Static0", L"SelectSkill3Static1", L"SelectSkill3Static2", L"SelectSkill3Static3",
    L"SelectSkill3Static4", L"SelectSkill3Static5", L"SelectSkill3Static6", L"SelectSkill3Static7",
    L"SelectSkill4Static0", L"SelectSkill4Static1", L"SelectSkill4Static2", L"SelectSkill4Static3",
    L"SelectSkill4Static4", L"SelectSkill4Static5", L"SelectSkill4Static6", L"SelectSkill4Static7",
    L"TotalStaticL0", L"TotalStaticL1", L"TotalStaticL2", L"TotalStaticL3", L"TotalStaticL4", L"TotalStaticL5",
    L"TotalStaticL6", L"TotalStaticL7", L"TotalStaticL8", L"TotalStaticL9", L"TotalStaticL10", L"TotalStaticL11",
    L"TotalStaticL12", L"TotalStaticL13", L"TotalStaticL14", L"TotalStaticL15",
    L"TotalStaticR0", L"TotalStaticR1", L"TotalStaticR2", L"TotalStaticR3", L"TotalStaticR4", L"TotalStaticR5",
    L"TotalStaticR6", L"TotalStaticR7", L"TotalStaticR8", L"TotalStaticR9", L"TotalStaticR10", L"TotalStaticR11",
    L"TotalStaticR12", L"TotalStaticR13", L"TotalStaticR14", L"TotalStaticR15",
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
    if (!std::strcmp(window, "info")) return window_info;
    if (!std::strcmp(window, "items")) return window_items;
    if (!std::strcmp(window, "ex")) return window_ex;
    if (!std::strcmp(window, "g1")) return window_g1;
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
    add_radio = reinterpret_cast<AddRadio>(api->Symbol(mod, "oc_custom.ui_add_radio"));
    clear_skins = reinterpret_cast<Refresh>(api->Symbol(mod, "friend_unit_gift.ui_clear_skins"));
    refresh = reinterpret_cast<Refresh>(api->Symbol(mod, "friend_unit_gift.ui_refresh_geometry"));
    render_lock = reinterpret_cast<RenderVoid>(api->Symbol(mod, "oc_custom.client_render_lock"));
    render_clear = reinterpret_cast<RenderVoid>(api->Symbol(mod, "oc_custom.client_render_clear"));
    render_load = reinterpret_cast<RenderLoad>(api->Symbol(mod, "oc_custom.client_render_load"));
    render_rotation = reinterpret_cast<RenderRotation>(api->Symbol(mod, "oc_custom.client_render_rotation"));
    render_model = reinterpret_cast<RenderModel>(api->Symbol(mod, "oc_custom.client_render_model"));
    model_animation = reinterpret_cast<ModelAnimation>(api->Symbol(mod, "oc_custom.client_model_animation"));
    render_unlock = reinterpret_cast<RenderVoid>(api->Symbol(mod, "oc_custom.client_render_unlock"));
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
    for (std::size_t index = 0; index < window_g1; ++index) {
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
    std::array<Bound, 192> bound{};
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
    // G改 1.0 lives on the native 强化部件 page (g1_native.h); the OC page no
    // longer loads a copy of its layout.
    component_ = component;
    service_ = service;
    page_dialog_ = page;
    dialogs_ = dialogs;
    bound_ = bound;
    bound_count_ = count;
    // The OC-era allocation sheet starts 36 px above the factory's left
    // information sheet. Move its root so artwork, labels, hit targets and
    // the dialog-relative radar stay together and both sheets align.
    if (bindings_.refresh) {
        auto* over = dialogs_[window_over];
        Put<float>(over, 0x14C, Read<float>(over, 0x14C) + 36.0f);
        bindings_.refresh(over);
    }
    // Capture the authored positions before parking the hidden windows. The
    // native service may show its dialog map before our first active update.
    for (const auto window : {window_info, window_over, window_items})
        if (dialogs_[window]) page_x_[window] = Read<float>(dialogs_[window], 0x148);
    page_positioned_ = true;
    PositionPage();
    return true;
}

bool OriginalUi::AttachEntry() {
    if (!component_ || entry_button_) return entry_button_ != nullptr;
    if (!bindings_.add || !bindings_.add_icon || !bindings_.skin || !bindings_.button_vtable) {
        last_failure_ = "missing native control constructors";
        return false;
    }
    // The OC challenge entry: OverCustomBtn on the client's text-free glass
    // button (sprs\17454 normal, 17457 hover and pressed, 180x46; the
    // category bar's own button art) with a Chinese caption, where the OC-era
    // OVER CUSTOM banner (sprs\8526-8528, English art) stood. The label icon
    // stays empty.
    // A dual-system factory keeps the 经验值转换 page native, so the entry sits
    // on the OC tab's own 0800 page, between the level track and the ITEM box.
    void* info = dialogs_[window_info];
    const std::wstring label_name = L"OverCustomBackIcon";
    const auto native_label = ViewOf(label_name);
    bindings_.add_icon(info, &native_label, 41, 254, 84, 24);
    void* label = bindings_.find(info, &native_label, kIcon);
    const std::wstring button_name = L"OverCustomBtn";
    const auto native_button = ViewOf(button_name);
    bindings_.add(info, &native_button, 49, 242, 180, 46, 0);
    void* button = bindings_.find(info, &native_button, kButton);
    if (!label || !button || Read<void*>(label, kParent) != info || Read<void*>(button, kParent) != info ||
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
    static const int kEntrySprites[3] = {17454, 17457, 17457};
    for (int state = 0; state < 3; ++state) {
        const std::wstring sprite = L"sprs\\" + std::to_wstring(kEntrySprites[state]) + L".spr";
        const auto native_sprite = ViewOf(sprite);
        bindings_.skin(button, &native_sprite, 0, 0, 0);
    }
    entry_label_ = label;
    entry_button_ = button;
    entry_shown_ = false;
    // 0800 OverCustomSlotIcon0..6 (sprs 8997..9003, 30x30 from 3,120 in 35 px
    // steps) and NoneIcon0..2 (sprs 8486, 34x74 at 27/108/186,331).
    static const wchar_t* const kLevelNames[kLevelIcons] = {L"OcLevelIcon0", L"OcLevelIcon1", L"OcLevelIcon2", L"OcLevelIcon3",
                                                             L"OcLevelIcon4", L"OcLevelIcon5", L"OcLevelIcon6"};
    static const wchar_t* const kLevelSprites[kLevelIcons] = {L"sprs\\8997.spr", L"sprs\\8998.spr", L"sprs\\8999.spr", L"sprs\\9000.spr",
                                                               L"sprs\\9001.spr", L"sprs\\9002.spr", L"sprs\\9003.spr"};
    for (std::size_t level = 0; level < kLevelIcons; ++level)
        level_icons_[level] = CreateIcon(info, kLevelNames[level], 3 + 35 * static_cast<int>(level), 120, 30, 30, kLevelSprites[level]);
    static const wchar_t* const kNoneNames[kInfoItems] = {L"OcNoneIcon0", L"OcNoneIcon1", L"OcNoneIcon2"};
    // An empty ITEM slot: the 强化部件 page's own empty-slot frame (sprs\16827,
    // 54x54) centred on the slot's 3D render, with 无 (the OC-era NONE art,
    // sprs\8486, is English).
    static const int kNoneX[kInfoItems] = {15, 95, 175};
    static const wchar_t* const kNoneTexts[kInfoItems] = {L"OcNoneText0", L"OcNoneText1", L"OcNoneText2"};
    for (std::size_t slot = 0; slot < kInfoItems; ++slot) {
        none_icons_[slot] = CreateIcon(info, kNoneNames[slot], kNoneX[slot], 341, 54, 54, L"sprs\\16827.spr");
        none_texts_[slot] = CreateButton(window_info, kNoneTexts[slot], kNoneX[slot], 359, 54, 18, nullptr, 0, Action::none);
        SetCreatedText(kNoneTexts[slot], L"无", 2);
        if (!CreateButton(window_info, kRemoveNames[slot], kNoneX[slot], 397, 54, 18,
                          nullptr, 0, static_cast<Action>(static_cast<int>(Action::remove0) + slot))) return false;
        SetCreatedText(kRemoveNames[slot], L"移除", 2);
    }
    // Keep both original layouts intact; each has one switch to the other view.
    if (!CreateButton(window_over, L"OcItemListBtn", 246, 29, 138, 22, L"sprs\\", 16819, Action::item_list) ||
        !CreateButton(window_items, L"OcItemsRadarBtn", 87, 5, 138, 22, L"sprs\\", 16819, Action::item_list)) return false;
    // These sprites paint their frame inside the logical button origin.
    // Centre the captions on that frame, including the native font baseline.
    SetCreatedText(L"OcItemListBtn", L"强化道具", 2, 5, 5);
    SetCreatedText(L"OcItemsRadarBtn", L"六维图", 2, 5, 5);
    if (!CreateButton(window_items, L"OcItemsStatus", 8, 42, 290, 48, nullptr, 0, Action::none)) return false;
    AttachG1(); // optional
    return true;
}

// A button the Mod adds to a loaded dialog: with sprites (three states) it is a
// clickable button, without them a caption holder drawn with its text item.
void* OriginalUi::CreateButton(std::uint8_t window, const wchar_t* name, int x, int y, int width, int height,
                               const wchar_t* sprite_base, int first_sprite, Action action) {
    void* dialog = dialogs_[window];
    if (!dialog || created_count_ >= created_.size()) return nullptr;
    const std::wstring control_name = name;
    const auto native_name = ViewOf(control_name);
    void* control = bindings_.find(dialog, &native_name, kButton);
    if (!control) {
        bindings_.add(dialog, &native_name, x, y, width, height, 0);
        control = bindings_.find(dialog, &native_name, kButton);
    }
    if (!control || Read<void*>(control, kParent) != dialog || Read<void*>(control) != bindings_.button_vtable) return nullptr;
    Put<std::uint8_t>(control, kVisible, 0);
    Put<std::uint8_t>(control, kVisible2, 0);
    Put<std::uint8_t>(control, kEnabled, 0);
    Put<std::uint8_t>(control, kDebugQuad, 0);
    if (sprite_base) {
        for (int state = 0; state < 3; ++state) {
            const std::wstring sprite = std::wstring(sprite_base) + std::to_wstring(first_sprite + state) + L".spr";
            const auto native_sprite = ViewOf(sprite);
            bindings_.skin(control, &native_sprite, 0, 0, 0);
        }
    }
    if (bindings_.refresh) {
        bindings_.refresh(control);
        bindings_.refresh(dialog);
    }
    auto& created = created_[created_count_++];
    created.name = name;
    created.pointer = control;
    created.action = action;
    created.window = window;
    created.width = width;
    created.height = height;
    return control;
}

void OriginalUi::SetCreatedText(const wchar_t* name, const std::wstring& value, int align, int offset_x, int offset_y) {
    for (std::size_t index = 0; index < created_count_; ++index) {
        auto& created = created_[index];
        if (std::wcscmp(created.name, name) || (created.text_set && created.text == value)) continue;
        const int height = value.find(L'\n') == std::wstring::npos ? 14 : 28;
        SetCaption(created.pointer, value, 2 + offset_x, std::max(0, (created.height - height) / 2) + offset_y,
                   std::max(1, created.width - 4), height, align);
        created.text = value;
        created.text_set = true;
    }
}

void OriginalUi::SetCreatedState(const wchar_t* name, bool visible, bool enabled) {
    for (std::size_t index = 0; index < created_count_; ++index)
        if (!std::wcscmp(created_[index].name, name)) SetState(created_[index].pointer, visible, enabled);
}

// The G改 1.0 page's own rows, laid out between the 1252 header statics and
// its buttons: the style button on 0800 (below the ITEM box), then on 1252 a
// category row and four slot rows (the create window's selector arrows,
// sprs\17915-17920) and a notice line, and the way back to the OC page above
// 设置保存 (the page's own button sprites, sprs\16819-16821).
bool OriginalUi::AttachG1() {
    if (g1_attached_ || !dialogs_[window_g1] || !bindings_.add || !bindings_.skin || !bindings_.text) return g1_attached_;
    auto* save = Find(window_g1, L"SaveBtn");
    auto* level = Find(window_g1, L"UnitLevelStatic");
    auto* type = Find(window_g1, L"TypeStatic");
    if (!save || !level || !type) return false;
    const int left = static_cast<int>(Read<float>(level->pointer, 0x148));
    int width = Read<int>(level->pointer, 0x154);
    if (width < 200 || width > 320) width = 280;
    const int save_y = static_cast<int>(Read<float>(save->pointer, 0x14C));
    const int arrow = 19;
    if (!CreateButton(window_info, L"OcG1StyleBtn", 20, 430, 138, 22, L"sprs\\", 16819, Action::g1_style)) return false;
    // The header's type line is the 类型 selector; the notice and the way back
    // to the OC page sit on the two rows above the 强化槽 frame (the layout's
    // weapon-effect rows, empty on this page).
    const int type_y = static_cast<int>(Read<float>(type->pointer, 0x14C));
    const int type_height = Read<int>(type->pointer, 0x158);
    const int arrow_y = type_y + std::max(0, (type_height - arrow) / 2);
    if (!CreateButton(window_g1, L"OcG1CategoryLeftBtn", left + 4, arrow_y, arrow, arrow, L"sprs\\", 17915,
                      Action::g1_category_prev) ||
        !CreateButton(window_g1, L"OcG1CategoryRightBtn", left + width - arrow - 4, arrow_y, arrow, arrow, L"sprs\\", 17918,
                      Action::g1_category_next))
        return false;
    if (!CreateButton(window_g1, L"OcG1Notice", left, save_y - 326, width, 18, nullptr, 0, Action::none)) return false;
    if (!CreateButton(window_g1, L"OcStyleBackBtn", left + (width - 138) / 2, save_y - 281, 138, 22, L"sprs\\", 16819,
                      Action::oc_style))
        return false;
    // Inside the frame, next to its slot numbers 1..4.
    static const int kSlotOffsets[4] = {200, 148, 95, 42};
    static const wchar_t* const kSlots[4][4] = {
        {L"OcG1Slot0Label", L"OcG1Slot0LeftBtn", L"OcG1Slot0Value", L"OcG1Slot0RightBtn"},
        {L"OcG1Slot1Label", L"OcG1Slot1LeftBtn", L"OcG1Slot1Value", L"OcG1Slot1RightBtn"},
        {L"OcG1Slot2Label", L"OcG1Slot2LeftBtn", L"OcG1Slot2Value", L"OcG1Slot2RightBtn"},
        {L"OcG1Slot3Label", L"OcG1Slot3LeftBtn", L"OcG1Slot3Value", L"OcG1Slot3RightBtn"},
    };
    const int slot_arrow_x = left + 56;
    const int slot_value_x = slot_arrow_x + arrow + 2;
    const int slot_value_width = left + width - arrow - 4 - slot_value_x;
    for (int slot = 0; slot < 4; ++slot) {
        const int y = save_y - kSlotOffsets[slot];
        if (!CreateButton(window_g1, kSlots[slot][0], left, y, 50, 18, nullptr, 0, Action::none) ||
            !CreateButton(window_g1, kSlots[slot][1], slot_arrow_x, y - 1, arrow, arrow, L"sprs\\", 17915,
                          static_cast<Action>(static_cast<int>(Action::g1_slot_prev0) + slot)) ||
            !CreateButton(window_g1, kSlots[slot][2], slot_value_x, y, slot_value_width, 18, nullptr, 0, Action::none) ||
            !CreateButton(window_g1, kSlots[slot][3], slot_value_x + slot_value_width + 2, y - 1, arrow, arrow, L"sprs\\",
                          17918, static_cast<Action>(static_cast<int>(Action::g1_slot_next0) + slot)))
            return false;
    }
    g1_attached_ = true;
    return true;
}

void OriginalUi::HideG1Native() {
    if (g1_native_hidden_ || !dialogs_[window_g1]) return;
    // Controls sit on the loaded dialog or on the layout's inner Dlg record.
    void* dialog = dialogs_[window_g1];
    for (const auto* name : kG1HiddenNames) {
        const std::wstring control_name = name;
        const auto native = ViewOf(control_name);
        for (int type = 0; type <= 20; ++type) {
            void* control = bindings_.find(dialog, &native, type);
            void* parent = control ? Read<void*>(control, kParent) : nullptr;
            if (control && parent && (parent == dialog || Read<void*>(parent, kParent) == dialog)) SetState(control, false, false);
        }
    }
    g1_native_hidden_ = true;
}

void OriginalUi::Detach() noexcept {
    radar::ClearChallengeSkins();
    challenge_frame_={};challenge_rows_=-1;
    component_ = service_ = page_dialog_ = nullptr;
    dialogs_ = {};
    bound_ = {};
    bound_count_ = 0;
    entry_button_ = entry_label_ = nullptr;
    shown_ = {};
    page_slide_ = {};
    page_x_ = {};
    page_positioned_ = false;
    entry_shown_ = false;
    page_hidden_ = false;
    page_visible_ = 1;
    level_icons_ = {};
    none_icons_ = {};
    inventory_models_ = {};
    models_shown_ = {};
    renders_prepared_ = false;
    created_ = {};
    created_count_ = 0;
    g1_attached_ = false;
    g1_native_hidden_ = false;
}

bool OriginalUi::Alive() const noexcept {
    if (!component_ || !page_dialog_) return false;
    __try {
        if (Read<void*>(component_, kComponentDialog) != page_dialog_) return false;
        for (std::size_t index = 0; index < bound_count_; index += 16)
            if (Read<void*>(bound_[index].pointer, kParent) != bound_[index].parent) return false;
        return !entry_button_ || Read<void*>(entry_button_, kParent) == dialogs_[window_info];
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
    if (page_slide_.Moving()) return Action::none;
    __try {
        if (control == entry_button_)
            return Read<std::uint8_t>(control, kVisible) && Read<std::uint8_t>(control, kEnabled) ? Action::entry
                                                                                                   : Action::none;
        for (std::size_t index = 0; index < created_count_; ++index) {
            const auto& created = created_[index];
            if (created.pointer != control || created.action == Action::none) continue;
            if (!shown_[created.window] || !Read<std::uint8_t>(control, kVisible) || !Read<std::uint8_t>(control, kEnabled)) {
                if (bindings_.log) {
                    char line[160];
                    std::snprintf(line, sizeof(line), "g1 page control %ls refused: shown=%u visible=%u enabled=%u", created.name,
                                  shown_[created.window] ? 1u : 0u, Read<std::uint8_t>(control, kVisible),
                                  Read<std::uint8_t>(control, kEnabled));
                    bindings_.log(line);
                }
                return Action::none;
            }
            return created.action;
        }
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

// 0800 item names use centred native text in 200 px wide statics. Centre the
// control itself over each 80 px item slot, independent of caption length.
void OriginalUi::SetItemName(std::size_t slot, const std::wstring& value) {
    const std::wstring index = std::to_wstring(slot);
    auto* control = Find(window_info, (L"ItemNameStatic" + index).c_str());
    auto* frame = Find(window_info, (L"NoneIcon" + index).c_str());
    if (!control || (control->text_set && control->text == value)) return;
    SetStatic(window_info, (L"ItemNameStatic" + index).c_str(), value);
    if (!frame || !bindings_.refresh) return;
    const float centre = 42.0f + 80.0f * static_cast<float>(slot);
    Put<float>(control->pointer, 0x148, centre - static_cast<float>(control->width) / 2.0f);
    bindings_.refresh(control->pointer);
}

void OriginalUi::SetLabel(Bound& control, const std::wstring& value, int align, int caption_height) {
    if (control.text_set && control.text == value) return;
    const int height = Read<int>(control.pointer, 0x158);
    const int left = control.type == kCheck ? 20 : 2;
    const int width = control.type == kCheck ? 360 : std::max(1, control.width - 4);
    SetCaption(control.pointer, value, left, std::max(0, (height - caption_height) / 2), width, caption_height, align);
    control.text = value;
    control.text_set = true;
}

void OriginalUi::SetCaption(void* control, const std::wstring& value, int left, int top, int width, int height, int align) {
    // Native buttons/check boxes can select a different text item for hover,
    // press or disabled states. The shipped challenge button includes Text1
    // placeholders with different geometry; replacing only item zero exposes
    // them on hover. Keep every existing state on the same caption and box.
    const auto begin = Read<std::uintptr_t>(control, 0x190);
    const auto end = Read<std::uintptr_t>(control, 0x194);
    const auto count = begin && end >= begin && end - begin <= 64 * sizeof(void*) &&
        (end - begin) % sizeof(void*) == 0 ? (end - begin) / sizeof(void*) : 0;
    const auto native = ViewOf(value);
    for (std::size_t index = 0; index < std::max<std::size_t>(1, count); ++index)
        bindings_.text(control, static_cast<int>(index), &native, left, top, width, height,
                       kLabelColor, kShadowColor, 0, 1, align);
}

void OriginalUi::LayoutChallenge(const ChallengeView& view) {
    int rows=0;for(bool shown:view.item_visible)if(shown)++rows;
    if(rows==challenge_rows_)return;
    auto* root=dialogs_[window_challenge];
    if(!root || !bindings_.clear_skins || !bindings_.refresh)return;
    const int height=330+22*rows;
    const auto move=[&](void* p,int x,int y,int w,int h) {
        Put<float>(p,0x148,static_cast<float>(x));Put<float>(p,0x14C,static_cast<float>(y));
        Put<int>(p,0x154,w);Put<int>(p,0x158,h);bindings_.refresh(p);
    };
    move(root,292,(768-height)/2-18,440,height);
    const auto place=[&](const wchar_t* name,int x,int y,int w,int h) {
        if(auto* b=Find(window_challenge,name)) {
            move(b->pointer,x,y,w,h);b->width=w;
            auto** items=Read<std::uint8_t**>(b->pointer,0x190);
            auto** end=Read<std::uint8_t**>(b->pointer,0x194);
            if(b->type==kStatic && items && items<end && *items) {
                const auto empty=ViewOf(std::wstring());
                bindings_.text(b->pointer,0,&empty,0,0,w,h,Read<unsigned>(*items,0x58),
                    Read<unsigned>(*items,0x5C),Read<std::uint8_t>(*items,0x64),
                    Read<unsigned>(*items,0x6C),Read<int>(*items,0x68));
            }
            b->text_set=false;
        }
    };
    place(L"TitleStatic",40,17,348,22);place(L"CloseBtn",400,14,25,25);
    place(L"WorkUnitName",28,51,384,20);
    place(L"TargetLevelTittle",28,80,100,18);place(L"LevelName",150,80,250,18);
    place(L"WorkStateText",28,106,384,18);place(L"MessageStatic0",28,130,384,28);
    place(L"OverCustomTittle",28,164,384,20);
    int row=0;
    for(std::size_t i=0;i<kItemSlots;++i)if(view.item_visible[i])
        place((L"UseItemCheck"+std::to_wstring(i)).c_str(),28,190+22*row++,16,18);
    place(L"MessageStatic1",28,208+22*rows,384,46);
    // The native skin's visible face is 130 px wide, inset by 4 px. A
    // 156 px control centered its caption to the right of that face. Match
    // the hit area to the skin and center the whole button in the modal.
    place(L"StartBtn",151,height-60,138,30);
    if(!challenge_frame_[0]) {
        bindings_.clear_skins(root);
        auto* head=Read<void*>(root,0x23C);auto* first=Read<void*>(head);
        for(std::size_t i=0;i<challenge_frame_.size();++i) {
            const auto name=L"OcChallengeFrame"+std::to_wstring(i);const auto key=ViewOf(name);
            bindings_.add(root,&key,0,0,368,152,0);
            auto* p=bindings_.find(root,&key,kButton);if(!p)return;
            const std::wstring art=L"sprs\\8460.spr";const auto skin=ViewOf(art);
            bindings_.skin(p,&skin,0,0,0);SetState(p,true,false);Put<std::uint8_t>(p,kDebugQuad,0);
            challenge_frame_[i]=p;
            // Backgrounds precede the original controls, without changing hit targets.
            for(auto* link=Read<void*>(head);link!=head;link=Read<void*>(link))if(Read<void*>(link,8)==p) {
                auto* next=Read<void*>(link);auto* prev=Read<void*>(link,4);
                Put<void*>(prev,0,next);Put<void*>(next,4,prev);prev=Read<void*>(first,4);
                Put<void*>(link,0,first);Put<void*>(link,4,prev);Put<void*>(prev,0,link);Put<void*>(first,4,link);break;
            }
        }
    }
    // Nine slices of the original confirmation frame; retain the corner/edge
    // pixels and use its blank blue interior without stretching printed art.
    const radar::Rect source[11]={{0,0,20,20},{20,0,328,20},{348,0,20,20},
        {0,20,20,112},{20,80,4,4},{348,20,20,112},
        {0,132,20,20},{20,132,328,20},{348,132,20,20},
        {83,24,261,52},{83,24,261,52}};
    const radar::Rect target[11]={{0,0,20,20},{20,0,400,20},{420,0,20,20},
        {0,20,20,static_cast<float>(height-40)},{20,20,400,static_cast<float>(height-40)},
        {420,20,20,static_cast<float>(height-40)},
        {0,static_cast<float>(height-20),20,20},{20,static_cast<float>(height-20),400,20},
        {420,static_cast<float>(height-20),20,20},
        {20,44,400,112},{20,static_cast<float>(200+22*rows),400,56}};
    radar::ClearChallengeSkins();
    for(std::size_t i=0;i<challenge_frame_.size();++i) {
        auto* p=challenge_frame_[i];if(!p)return;
        const float sx=target[i].width/source[i].width,sy=target[i].height/source[i].height;
        auto** skins=Read<std::uint8_t**>(p,0x1A4);auto* sprite=Read<void*>(*skins);
        move(p,static_cast<int>(target[i].x-source[i].x*sx),static_cast<int>(target[i].y-source[i].y*sy),
             static_cast<int>(368*sx),static_cast<int>(152*sy));
        Put<float>(sprite,0x94,sx);Put<float>(sprite,0x98,sy);Put<std::uint8_t>(sprite,0xA0,1);
        radar::InstallChallengeSkin(p,root,target[i]);
    }
    challenge_rows_=rows;
}

void OriginalUi::ShowIcon(void* control, bool visible) noexcept {
    __try {
        Put<std::uint8_t>(control, kVisible, visible ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// An icon created on a loaded dialog with its sprite, hidden until shown (the
// same native calls as the restored OVER CUSTOM entry).
void* OriginalUi::CreateIcon(void* dialog, const wchar_t* name, int x, int y, int width, int height, const wchar_t* sprite) {
    const std::wstring icon_name = name;
    const auto native_name = ViewOf(icon_name);
    bindings_.add_icon(dialog, &native_name, x, y, width, height);
    void* icon = bindings_.find(dialog, &native_name, kIcon);
    if (!icon || Read<void*>(icon, kParent) != dialog) return nullptr;
    Put<std::uint8_t>(icon, kVisible, 0);
    Put<std::uint8_t>(icon, kVisible2, 0);
    Put<std::uint8_t>(icon, kEnabled, 0);
    Put<std::uint8_t>(icon, kDebugQuad, 0);
    const std::wstring sprite_path = sprite;
    const auto native_sprite = ViewOf(sprite_path);
    bindings_.skin(icon, &native_sprite, 0, 0, 0);
    return icon;
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

namespace {
// Keep the native camera axis and clipping planes, with room around coupon
// models for the inventory effect caption and the selected-slot remove button.
void PutCamera(void* control) noexcept {
    __try {
        Put<float>(control, 0x234, 0.0f);
        Put<float>(control, 0x238, 0.0f);
        Put<float>(control, 0x23C, -45.0f);
        Put<float>(control, 0x240, 0.0f);
        Put<float>(control, 0x244, 0.0f);
        Put<float>(control, 0x248, 0.0f);
        Put<float>(control, 0x25C, 1.0f);
        Put<float>(control, 0x260, 500.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
} // namespace

void OriginalUi::PrepareRenders() noexcept {
    if (renders_prepared_) return;
    renders_prepared_ = true;
    static const wchar_t* const kRenders[kInfoItems] = {L"Item3DRender0", L"Item3DRender1", L"Item3DRender2"};
    for (const auto* name : kRenders) {
        auto* render = Find(window_info, name);
        if (render && render->pointer) PutCamera(render->pointer);
    }
}

namespace {
bool LoadModelLocked(const Bindings& bindings, void* render, const NativeString* path, const NativeString* animation) noexcept {
    void* model = nullptr;
    __try {
        bindings.render_clear(render);
        bindings.render_load(render, path, 0);
        bindings.render_rotation(render, 0.0f, 0.0f, 0.0f);
        model = bindings.render_model(render);
        if (model) {
            bindings.model_animation(model, animation, 1, 1.0f, 0.1f, 2, 1, 0);
            Put<float>(model, 0x88, 0.0f);
            Put<float>(model, 0x8C, -11.0f);
            Put<float>(model, 0x90, 0.0f);
            Put<std::uint8_t>(model, 0xA0, 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        model = nullptr;
    }
    return model != nullptr;
}
} // namespace

// FUN_00873B40's sequence: under the control's lock, clear, load the model,
// reset the rotation and start its "default" animation, then refresh.
bool OriginalUi::LoadModel(void* render, const NativeString* path) noexcept {
    NativeString animation{};
    static constexpr wchar_t kDefault[] = L"default";
    std::memcpy(animation.data.small, kDefault, sizeof(kDefault));
    animation.length = 7;
    animation.capacity = 7;
    bool loaded = false;
    __try {
        bindings_.render_lock(render);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    loaded = LoadModelLocked(bindings_, render, path, &animation);
    __try {
        bindings_.render_unlock(render);
        if (loaded) {
            auto** vtable = Read<void**>(render);
            reinterpret_cast<Bindings::RenderVoid>(vtable[0x1C / sizeof(void*)])(render);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        loaded = false;
    }
    return loaded;
}

void OriginalUi::HideAll() noexcept {
    __try {
        for (std::size_t index = 0; index < window_count; ++index) {
            if (dialogs_[index]) Put<std::uint8_t>(dialogs_[index], kVisible, 0);
            shown_[index] = false;
        }
        page_slide_.Show(false);
        PositionPage();
        if (entry_button_) SetState(entry_button_, false, false);
        if (entry_label_) SetState(entry_label_, false, false);
        entry_shown_ = false;
        if (page_hidden_ && page_dialog_) Put<std::uint8_t>(page_dialog_, kVisible, page_visible_);
        page_hidden_ = false;
        models_visible_ = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

float OriginalUi::PageOffset(Window window) const noexcept {
    if (!dialogs_[window]) return 0.0f;
    if (window != window_info && window != window_over && window != window_items) return 0.0f;
    const float width = static_cast<float>(Read<int>(dialogs_[window], 0x154));
    return page_slide_.Offset(window == window_info ? -width : width);
}

void OriginalUi::PositionPage() noexcept {
    if (!page_positioned_ || !bindings_.refresh) return;
    __try {
        for (const auto window : {window_info, window_over, window_items}) {
            void* dialog = dialogs_[window];
            if (!dialog) continue;
            const float x = page_x_[window] + PageOffset(window);
            if (Read<float>(dialog, 0x148) != x) {
                Put<float>(dialog, 0x148, x);
                bindings_.refresh(dialog);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void OriginalUi::AdvancePageSlide() noexcept {
    if (!page_slide_.Moving() || !Alive()) return;
    page_slide_.Advance();
    PositionPage();
}

void OriginalUi::Render(const EntryView& entry, const OverView& over, const InfoView& info,
                        const ChallengeView& challenge, const ConfirmView& confirm, const G1View& g1) {
    if (!Alive()) return;
    if (!page_positioned_) {
        for (const auto window : {window_info, window_over, window_items})
            if (dialogs_[window]) page_x_[window] = Read<float>(dialogs_[window], 0x148);
        page_positioned_ = true;
    }
    if (!info.visible)
        for (const auto window : {window_info, window_over, window_items})
            if (dialogs_[window]) Put<std::uint8_t>(dialogs_[window], kVisible, 0);
    page_slide_.Show(info.visible && bindings_.refresh);
    PositionPage();
    // 0800 (or the G改 1.0 page) takes the upgrade page's place (same frame at
    // 0,100) while the OC tab is shown; the page dialog keeps its own
    // visibility for the way back.
    const bool g1_page = g1.visible && dialogs_[window_g1] && g1_attached_;
    if (info.visible || g1_page) {
        if (!page_hidden_) {
            page_visible_ = Read<std::uint8_t>(page_dialog_, kVisible);
            page_hidden_ = true;
        }
        Put<std::uint8_t>(page_dialog_, kVisible, 0);
    }
    if (info.visible) {
        // The layout's own level icons and NONE frames stay hidden; the
        // created ones at the same places show the state.
        for (std::size_t level = 0; level < kLevelIcons; ++level) {
            const std::wstring name = L"OverCustomSlotIcon" + std::to_wstring(level);
            if (auto* icon = Find(window_info, name.c_str())) ShowIcon(icon->pointer, false);
            if (level_icons_[level]) SetState(level_icons_[level], info.level[level], false);
        }
        if (auto* spare = Find(window_info, L"OverCustomSlotIcon7")) ShowIcon(spare->pointer, false);
        models_visible_ = 0;
        for (std::size_t slot = 0; slot < kInfoItems; ++slot) {
            const std::wstring index = std::to_wstring(slot);
            SetItemName(slot, info.item[slot] ? info.item_text[slot] : L"");
            if (auto* none = Find(window_info, (L"NoneIcon" + index).c_str())) ShowIcon(none->pointer, false);
            // Item3DRender0..2 show the item's own model, as the factory pages do.
            const bool model = info.item[slot] && !info.item_model[slot].empty() && bindings_.ItemModels();
            bool shown = false;
            if (auto* render = Find(window_info, (L"Item3DRender" + index).c_str())) {
                if (model && models_shown_[slot] != info.item_model[slot]) {
                    PrepareRenders();
                    const auto native_model = ViewOf(info.item_model[slot]);
                    const bool loaded = LoadModel(render->pointer, &native_model);
                    if (loaded) {
                        models_shown_[slot] = info.item_model[slot];
                        ++models_loaded_;
                    } else {
                        models_shown_[slot].clear();
                    }
                    if (bindings_.log) {
                        char line[160];
                        std::snprintf(line, sizeof(line), "oc item model slot %u %ls %s", static_cast<unsigned>(slot),
                                      info.item_model[slot].c_str(), loaded ? "loaded" : "did not load");
                        bindings_.log(line);
                    }
                }
                shown = model && models_shown_[slot] == info.item_model[slot];
                SetState(render->pointer, shown, false);
                if (shown) ++models_visible_;
            }
            if (none_icons_[slot]) SetState(none_icons_[slot], !info.item[slot], false);
            if (none_texts_[slot]) SetState(none_texts_[slot], !info.item[slot], false);
            SetCreatedState(kRemoveNames[slot], info.item[slot], info.items_enabled && !confirm.visible);
        }
    }
    if (!info.visible && !g1_page) {
        models_visible_ = 0;
        if (page_hidden_) {
            Put<std::uint8_t>(page_dialog_, kVisible, page_visible_);
            page_hidden_ = false;
        }
    }
    if (dialogs_[window_info]) Put<std::uint8_t>(dialogs_[window_info], kVisible, info.visible ? 1 : 0);
    shown_[window_info] = info.visible;
    // The style button on 0800 opens the G改 1.0 page.
    if (g1_attached_) {
        SetCreatedText(L"OcG1StyleBtn", g1.style_label, 2);
        SetCreatedState(L"OcG1StyleBtn", info.visible && g1.offered, info.visible && g1.offered && !confirm.visible);
    }
    // The G改 1.0 page (1252 with the Mod's rows).
    if (g1_page) {
        HideG1Native();
        SetStatic(window_g1, L"UnitLevelStatic", g1.header);
        SetStatic(window_g1, L"TypeStatic", g1.type);
        SetCreatedText(L"OcStyleBackBtn", g1.back_label, 2);
        SetCreatedState(L"OcStyleBackBtn", true, !confirm.visible);
        SetCreatedText(L"OcG1CategoryLabel", L"类型", 0);
        SetCreatedState(L"OcG1CategoryLabel", true, false);
        SetCreatedText(L"OcG1CategoryValue", g1.category, 2);
        SetCreatedState(L"OcG1CategoryValue", true, false);
        SetCreatedState(L"OcG1CategoryLeftBtn", true, g1.category_arrows && !confirm.visible);
        SetCreatedState(L"OcG1CategoryRightBtn", true, g1.category_arrows && !confirm.visible);
        for (std::size_t slot = 0; slot < kG1Slots; ++slot) {
            const std::wstring index = std::to_wstring(slot);
            SetCreatedText((L"OcG1Slot" + index + L"Label").c_str(), g1.slot_label[slot], 0);
            SetCreatedState((L"OcG1Slot" + index + L"Label").c_str(), true, false);
            SetCreatedText((L"OcG1Slot" + index + L"Value").c_str(), g1.slot_value[slot], 2);
            SetCreatedState((L"OcG1Slot" + index + L"Value").c_str(), true, false);
            SetCreatedState((L"OcG1Slot" + index + L"LeftBtn").c_str(), true, g1.slot_arrows[slot] && !confirm.visible);
            SetCreatedState((L"OcG1Slot" + index + L"RightBtn").c_str(), true, g1.slot_arrows[slot] && !confirm.visible);
        }
        SetCreatedText(L"OcG1Notice", g1.notice, 2);
        SetCreatedState(L"OcG1Notice", true, false);
        if (auto* save = Find(window_g1, L"SaveBtn")) {
            SetLabel(*save, L"设置保存", 2);
            SetState(save->pointer, true, g1.save_enabled && !confirm.visible);
        }
        if (auto* revert = Find(window_g1, L"SelectSkillResetBtn")) {
            SetLabel(*revert, L"还原", 2);
            SetState(revert->pointer, true, g1.revert_enabled && !confirm.visible);
        }
        if (auto* reset = Find(window_g1, L"AllSkillResetBtn")) {
            SetLabel(*reset, L"强化槽初始化", 2);
            SetState(reset->pointer, true, g1.reset_enabled && !confirm.visible);
        }
    }
    if (dialogs_[window_g1]) Put<std::uint8_t>(dialogs_[window_g1], kVisible, g1_page ? 1 : 0);
    shown_[window_g1] = g1_page;
    if (entry_button_) {
        // The button is shown with the page; it is enabled (and its caption
        // lit) once the challenge can start.
        SetState(entry_label_, false, false);
        SetState(entry_button_, entry.visible, entry.visible && entry.lit);
        if (entry.visible && (!entry_caption_set_ || entry_caption_lit_ != entry.lit)) {
            const std::wstring caption = L"终极强化挑战";
            const auto native = ViewOf(caption);
            bindings_.text(entry_button_, 0, &native, 2, 16, 176, 14, entry.lit ? 0xFFFFE680 : 0xFF8FA7B8, kShadowColor, 0, 1, 2);
            entry_caption_set_ = true;
            entry_caption_lit_ = entry.lit;
        }
        entry_shown_ = entry.visible;
    }
    // 0792 is the client's factory inventory, with its original cell art,
    // 3D renders, quantity/name statics and paging controls.
    const bool inventory_visible = over.visible && over.item_list;
    SetCreatedState(L"OcItemListBtn", over.visible && !over.item_list, over.items_enabled && !confirm.visible);
    SetCreatedState(L"OcItemsRadarBtn", inventory_visible, over.items_enabled && !confirm.visible);
    SetCreatedText(L"OcItemsStatus", over.item_status, 2);
    SetCreatedState(L"OcItemsStatus", inventory_visible && !over.item_status.empty(), false);
    if (inventory_visible) {
        for (std::size_t cell = 0; cell < kInventoryCells; ++cell) {
            const auto slot = over.item_page * kInventoryCells + cell;
            const auto suffix = std::to_wstring(cell);
            const bool occupied = slot < kItemSlots && over.item_visible[slot];
            SetStatic(window_items, (L"ItemStatic" + suffix).c_str(), occupied ? over.item_text[slot] : L"");
            SetStatic(window_items, (L"NumberStatic" + suffix).c_str(), occupied ? over.item_count[slot] : L"");
            for (const auto* prefix : {L"ItemStatic", L"NumberStatic"})
                if (auto* label = Find(window_items, (std::wstring(prefix) + suffix).c_str()))
                    SetState(label->pointer, occupied, false);
            if (auto* icon = Find(window_items, (L"SlotIcon" + suffix).c_str()))
                ShowIcon(icon->pointer, occupied && over.item_selected[slot]);
            if (auto* render = Find(window_items, (L"Item3DRender" + suffix).c_str())) {
                const bool model = occupied && !over.item_model[slot].empty() && bindings_.ItemModels();
                if (model && inventory_models_[cell] != over.item_model[slot]) {
                    PutCamera(render->pointer);
                    const auto native = ViewOf(over.item_model[slot]);
                    if (LoadModel(render->pointer, &native)) inventory_models_[cell] = over.item_model[slot];
                    else inventory_models_[cell].clear();
                }
                const bool loaded = model && inventory_models_[cell] == over.item_model[slot];
                SetState(render->pointer, loaded, loaded && over.item_enabled[slot] && !confirm.visible);
            }
        }
        SetStatic(window_items, L"PageStatic", std::to_wstring(over.item_page + 1) + L" / " + std::to_wstring(over.item_pages));
        if (auto* previous = Find(window_items, L"LeftBtn"))
            SetState(previous->pointer, true, over.items_enabled && over.item_page > 0 && !confirm.visible);
        if (auto* next = Find(window_items, L"RightBtn"))
            SetState(next->pointer, true, over.items_enabled && over.item_page + 1 < over.item_pages && !confirm.visible);
    }
    Put<std::uint8_t>(dialogs_[window_items], kVisible, inventory_visible ? 1 : 0);
    shown_[window_items] = inventory_visible;
    // 0794: allocation window.
    if (over.visible && !over.item_list) {
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
        // The legacy equipped-EX overlay shares ExBtn's hit area and intercepts
        // native mouse input. The button opens our EX selector in every state;
        // the selector itself displays the equipped ability.
        if (auto* icon = Find(window_over, L"ExIcon")) SetState(icon->pointer, false, false);
        // The layout's light sheet (BackIcon1, sprs\8536) lit a fixed corner
        // over the chart; the OC chart lights only the Over gain (radar.h).
        if (auto* sheet = Find(window_over, L"BackIcon1")) ShowIcon(sheet->pointer, false);
    }
    if (dialogs_[window_over]) Put<std::uint8_t>(dialogs_[window_over], kVisible, over.visible && !over.item_list ? 1 : 0);
    shown_[window_over] = over.visible && !over.item_list;
    // 0813: challenge window.
    if (challenge.visible) {
        LayoutChallenge(challenge);
        SetStatic(window_challenge, L"TitleStatic", challenge.title);
        SetStatic(window_challenge, L"WorkUnitName", challenge.unit);
        SetStatic(window_challenge, L"WorkStateText", challenge.state);
        SetStatic(window_challenge, L"TargetLevelTittle", challenge.target_title);
        SetStatic(window_challenge, L"LevelName", challenge.target);
        SetStatic(window_challenge, L"MessageStatic0", challenge.message0);
        SetStatic(window_challenge, L"OverCustomTittle", challenge_rows_ ? L"强化道具" : L"强化道具：无");
        SetStatic(window_challenge, L"MessageStatic1", challenge.message1);
        if (auto* start = Find(window_challenge, L"StartBtn")) {
            // Font 1 paints 12 px glyphs; center those inside the button,
            // rather than centering a 14 px line box with blank descent.
            SetLabel(*start, challenge.start, 2, 12);
            SetState(start->pointer, true, challenge.start_enabled && !confirm.visible);
        }
        if (auto* close = Find(window_challenge, L"CloseBtn")) SetState(close->pointer, true, !confirm.visible);
        if (auto* left = Find(window_challenge, L"LeftBtn")) SetState(left->pointer, false, false);
        if (auto* right = Find(window_challenge, L"RightBtn")) SetState(right->pointer, false, false);
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
    const bool ex_visible = over.ex_visible && !confirm.visible;
    if (ex_visible) {
        static const int sprites[8] = {9004, 9005, 9008, 9007, 9006, 9009, 9011, 9010};
        for (std::size_t index = 0; index < 8; ++index) {
            if (auto* button = Find(window_ex, (L"ExBtn" + std::to_wstring(index)).c_str())) {
                SetLabel(*button, over.ex_labels[index] + (over.ex_selected == index + 1 ? L"（已装备）" : L""), 2);
                SetState(button->pointer, true, over.ex_choices_enabled && over.ex_selected != index + 1);
            }
            if (auto* icon = Find(window_ex, (L"ExIcon" + std::to_wstring(index)).c_str())) {
                if (!icon->text_set && bindings_.skin && bindings_.clear_skins) {
                    const auto path = L"sprs\\" + std::to_wstring(sprites[index]) + L".spr";
                    const auto native = ViewOf(path);
                    // The shipped layout supplies an empty frame already.
                    // AddSkin appends; remove that frame so the EX art is used.
                    bindings_.clear_skins(icon->pointer);
                    bindings_.skin(icon->pointer, &native, 0, 0, 0);
                    icon->text_set = true;
                }
                ShowIcon(icon->pointer, true);
            }
        }
        if (auto* close = Find(window_ex, L"CloseBtn")) SetState(close->pointer, true, true);
    }
    if (dialogs_[window_ex]) Put<std::uint8_t>(dialogs_[window_ex], kVisible, ex_visible ? 1 : 0);
    shown_[window_ex] = ex_visible;
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
    if (control) return control->pointer;
    for (std::size_t created = 0; created < created_count_; ++created)
        if (created_[created].window == index && !std::wcscmp(created_[created].name, name)) return created_[created].pointer;
    return nullptr;
}

bool OriginalUi::Visible(const char* window) const noexcept {
    if (!Alive()) return false;
    if (window && !std::strcmp(window, "entry")) {
        __try {
            return entry_button_ && shown_[window_info] && Read<std::uint8_t>(entry_button_, kVisible) &&
                   Read<std::uint8_t>(dialogs_[window_info], kVisible);
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
