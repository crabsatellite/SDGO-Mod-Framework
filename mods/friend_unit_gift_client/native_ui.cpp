#include "native_ui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstring>

namespace sdgo::friend_unit_gift::client::ui {
namespace {

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(
        &value,
        static_cast<const std::uint8_t*>(pointer) + offset,
        sizeof(value));
    return value;
}

template <class T>
void Put(void* pointer, std::size_t offset, T value) {
    std::memcpy(
        static_cast<std::uint8_t*>(pointer) + offset,
        &value,
        sizeof(value));
}

NativeString View(const std::wstring& value) {
    NativeString string;
    string.length = static_cast<std::uint32_t>(value.size());
    if (value.size() < 8) {
        std::memcpy(
            string.data.small,
            value.c_str(),
            (value.size() + 1) * sizeof(wchar_t));
    } else {
        string.data.pointer = value.c_str();
        string.capacity = string.length;
    }
    return string;
}

NativeString View(const wchar_t* value) {
    NativeString string;
    if (!value) return string;
    string.length =
        static_cast<std::uint32_t>(std::wcslen(value));
    if (string.length < 8) {
        std::memcpy(
            string.data.small,
            value,
            (string.length + 1) * sizeof(wchar_t));
    } else {
        string.data.pointer = value;
        string.capacity = string.length;
    }
    return string;
}

bool ChildOf(void* child, void* dialog) {
    return child && dialog && Read<void*>(child, 0x15C) == dialog;
}

bool Interactive(void* control) noexcept {
    if (!control) return false;
    __try {
        return Read<std::uint8_t>(control, 0x162) != 0 &&
               Read<std::uint8_t>(control, 0x163) != 0 &&
               Read<std::uint8_t>(control, 0x167) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsBlankSkin(const void* entry) {
    return entry && !Read<void*>(entry) &&
        Read<std::uint32_t>(entry,0x14) == 0 &&
        Read<std::uint32_t>(entry,0x18) == 7 &&
        Read<wchar_t>(entry,4) == L'\0';
}

bool HasThreeButtonSkins(void* control) noexcept {
    if (!control) return false;
    __try {
        auto** first =
            Read<std::uint8_t**>(control, 0x1A4);
        auto** last =
            Read<std::uint8_t**>(control, 0x1A8);
        const auto firstAddress =
            reinterpret_cast<std::uintptr_t>(first);
        const auto lastAddress =
            reinterpret_cast<std::uintptr_t>(last);
        if (!first || !last ||
            lastAddress < firstAddress ||
            lastAddress - firstAddress !=
                3 * sizeof(void*)) {
            return false;
        }
        for (std::size_t index = 0; index < 3; ++index) {
            if (!first[index] ||
                (!Read<void*>(first[index]) && !IsBlankSkin(first[index]))) {
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring Utf16(
    const std::array<char16_t, kMaximumPeerNameCodeUnits>& value,
    std::uint16_t length) {
    std::wstring output;
    const auto count = std::min<std::size_t>(length, value.size());
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        output.push_back(static_cast<wchar_t>(value[index]));
    }
    return output;
}

const wchar_t* GiftStateText(GiftState state) {
    switch (state) {
    case GiftState::pending:
        return L"待处理";
    case GiftState::accepted:
        return L"已接收";
    case GiftState::returning_waiting_capacity:
        return L"退回托管中";
    case GiftState::returned:
        return L"已退回";
    }
    return L"未知";
}

} // namespace

bool Bindings::Bind(const SdgoApi* api, SdgoMod* mod) noexcept {
    if (!api || !mod || !api->Symbol) return false;
    add = reinterpret_cast<Add>(
        api->Symbol(mod, "friend_unit_gift.ui_add_button"));
    find = reinterpret_cast<Find>(
        api->Symbol(mod, "friend_unit_gift.ui_find_control"));
    text = reinterpret_cast<Text>(
        api->Symbol(mod, "friend_unit_gift.ui_text_item"));
    skin = reinterpret_cast<Skin>(
        api->Symbol(mod, "friend_unit_gift.ui_add_skin"));
    empty_skin = reinterpret_cast<EmptySkin>(
        api->Symbol(mod, "friend_unit_gift.ui_add_empty_skin"));
    refresh_geometry = reinterpret_cast<RefreshGeometry>(
        api->Symbol(mod, "friend_unit_gift.ui_refresh_geometry"));
    add_icon = reinterpret_cast<AddIcon>(api->Symbol(mod,"friend_unit_gift.ui_add_icon"));
    place_popup = reinterpret_cast<PlacePopup>(api->Symbol(mod,"friend_unit_gift.ui_place_popup"));
    button_vtable =
        api->Symbol(mod, "friend_unit_gift.ui_button_vtable");
    static_vtable=api->Symbol(mod,"friend_unit_gift.ui_static_vtable");
    static_caption=reinterpret_cast<Caption>(api->Symbol(mod,"friend_unit_gift.ui_static_caption"));
    edit_vtable = api->Symbol(mod, "friend_unit_gift.ui_edit_vtable");
    input_limit = reinterpret_cast<InputLimit>(
        api->Symbol(mod, "friend_unit_gift.ui_input_limit"));
    input_context = reinterpret_cast<void**>(
        api->Symbol(mod, "friend_unit_gift.ui_input_context"));
    input_lock = api->Symbol(mod, "friend_unit_gift.ui_input_lock");
    return add && find && text && skin && empty_skin && refresh_geometry && add_icon && place_popup && button_vtable && static_vtable && static_caption &&
           edit_vtable && input_limit && input_context && input_lock;
}

bool Bindings::SetNoteLimit(void* edit) const noexcept {
    if (!edit || !edit_vtable || !input_limit ||
        !input_context || !input_lock) return false;
    __try {
        if (Read<void*>(edit) != edit_vtable) return false;
        Put<std::uint32_t>(edit, 0x2A0,
            kMaximumNoteCodeUnits * sizeof(wchar_t));
        if (!Read<std::uint8_t>(edit, 0x165)) return true;
        auto* lock = static_cast<CRITICAL_SECTION*>(input_lock);
        EnterCriticalSection(lock);
        __try {
            auto* input = *input_context;
            if (!input) return false;
            // Native focus already copied the old limit into this shared
            // input. clear=0 preserves its text and caret while changing it.
            if (Read<std::uint8_t>(edit, 0x165))
                input_limit(input, kMaximumNoteCodeUnits, 0);
            return true;
        } __finally {
            LeaveCriticalSection(lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Bindings::ReadText(void* control, std::wstring* output) const {
    if (!control || !output) return false;
    const NativeString* value=nullptr;
    if (static_vtable && Read<void*>(control)==static_vtable) {
        value=reinterpret_cast<const NativeString*>(static_cast<const std::uint8_t*>(control)+0x21C);
    } else if (button_vtable && Read<void*>(control)==button_vtable) {
        auto** first=Read<NativeString**>(control,0x190);
        auto** last=Read<NativeString**>(control,0x194);
        const auto start=reinterpret_cast<std::uintptr_t>(first);
        const auto end=reinterpret_cast<std::uintptr_t>(last);
        if (!first || end<=start || end-start>64*sizeof(void*) || (end-start)%sizeof(void*)) return false;
        value=*first;
    } else return false;
    if (!value || value->length>512 || value->capacity<value->length) return false;
    const auto* data=value->capacity<8 ? value->data.small : value->data.pointer;
    if (!data || data[value->length]!=L'\0') return false;
    output->assign(data,value->length);
    return true;
}

void* Bindings::FindNamed(
    void* dialog,
    const wchar_t* name,
    int type) const {
    if (!dialog || !name || !find) return nullptr;
    const auto key = View(name);
    return find(dialog, &key, type);
}

void* Bindings::AddButton(
    void* dialog,
    const wchar_t* name,
    int x,
    int y,
    int width,
    int height,
    void* skinSource,
    const std::wstring& caption) const {
    last_error = "button arguments";
    if (!dialog || !name || !skinSource ||
        !add || !find || !text || !skin || !empty_skin ||
        width <= 0 || height <= 0) {
        return nullptr;
    }
    if (auto* existing = FindNamed(dialog, name, 4)) {
        last_error = "existing button layout or skins";
        return Read<void*>(existing) == button_vtable &&
                       ChildOf(existing, dialog) &&
                       HasThreeButtonSkins(existing)
                   ? existing
                   : nullptr;
    }

    auto* button = AddTextButton(
        dialog, name, x, y, width, height, caption);
    if (!button) return nullptr;

    auto** first = Read<std::uint8_t**>(skinSource, 0x1A4);
    auto** last = Read<std::uint8_t**>(skinSource, 0x1A8);
    const auto count =
        first && last &&
                reinterpret_cast<std::uintptr_t>(last) >=
                    reinterpret_cast<std::uintptr_t>(first)
            ? static_cast<std::size_t>(last - first)
            : 0;
    last_error = "source skin count";
    if (count < 3 || count > 16) return nullptr;
    for (std::size_t index = 0; index < 3; ++index) {
        last_error = "source skin resource";
        if (!first[index]) return nullptr;
        // Stock context menus intentionally use a transparent normal state.
        // The native blank-state appender keeps that slot and its ownership;
        // AddSkin skips an empty filename and would leave only two slots.
        if (IsBlankSkin(first[index])) {
            empty_skin(button);
            continue;
        }
        if (!Read<void*>(first[index])) return nullptr;
        skin(
            button,
            reinterpret_cast<const NativeString*>(first[index] + 4),
            Read<std::uint8_t>(first[index], 0x44),
            Read<std::uint32_t>(first[index], 0x3C),
            Read<std::uint32_t>(first[index], 0x40));
    }
    last_error = "cloned skin count or resources";
    return HasThreeButtonSkins(button)
               ? button
               : nullptr;
}

void* Bindings::AddTextButton(
    void* dialog,
    const wchar_t* name,
    int x,
    int y,
    int width,
    int height,
    const std::wstring& caption) const {
    if (!dialog || !name || !add || !find || !text ||
        width <= 0 || height <= 0) {
        return nullptr;
    }
    if (auto* existing = FindNamed(dialog, name, 4)) return existing;

    const auto key = View(name);
    add(dialog, &key, x, y, width, height, 0);
    auto* button = find(dialog, &key, 4);
    last_error = !button ? "created button not found" :
        Read<void*>(button) != button_vtable ? "created button vtable" :
        "created button parent";
    if (!button || Read<void*>(button) != button_vtable ||
        !ChildOf(button, dialog)) {
        return nullptr;
    }
    SetState(button, false, false);
    Put<std::uint8_t>(button, 0x168, 0);
    SetText(button, caption);
    return button;
}

void Bindings::SetText(
    void* control,
    const std::wstring& value,
    int align) const {
    if (!control || !text) return;
    if (static_vtable && static_caption && Read<void*>(control)==static_vtable) {
        // CUIStatic owns its displayed string at +0x21C. Its virtual caption
        // setter replaces that text; adding a base text item only overlays it.
        const auto caption=View(value);
        static_caption(control,&caption);
        return;
    }
    std::uint32_t color = 0xFF9CB8C8;
    std::uint32_t shadow = 0xFF000000;
    std::uint32_t font = 1;
    auto** first = Read<std::uint8_t**>(control, 0x190);
    auto** last = Read<std::uint8_t**>(control, 0x194);
    if (first && last && first < last && *first) {
        color = Read<std::uint32_t>(*first, 0x58);
        shadow = Read<std::uint32_t>(*first, 0x5C);
        font = Read<std::uint32_t>(*first, 0x6C);
    }
    const auto native = View(value);
    // Stock static controls may contain several text layers (bullet, caption,
    // numeric placeholder). Replacing only layer zero leaves those drawn over
    // the new caption. Empty the other native-owned strings before replacing it.
    const auto begin=reinterpret_cast<std::uintptr_t>(first);
    const auto end=reinterpret_cast<std::uintptr_t>(last);
    const auto layers=first && last && end>=begin && end-begin<=64*sizeof(void*) &&
        (end-begin)%sizeof(void*)==0 ? (end-begin)/sizeof(void*) : 0;
    if (Read<std::uint32_t>(control,0x134)==12 && layers) {
        // Radio captions have a text item for each native visual state.
        // Localizing only the first item blanks the selected tab.
        for (std::size_t index=0;index<layers;++index) {
            const auto* item=first[index];
            if (!item) continue;
            text(control,static_cast<int>(index),&native,
                static_cast<int>(Read<float>(item,0x48)),static_cast<int>(Read<float>(item,0x4C)),
                Read<int>(item,0x50),Read<int>(item,0x54),
                Read<std::uint32_t>(item,0x58),Read<std::uint32_t>(item,0x5C),
                Read<std::uint8_t>(item,0x64),Read<std::uint32_t>(item,0x6C),Read<int>(item,0x68));
        }
        return;
    }
    const auto empty=View(L"");
    for (std::size_t index=1;index<layers;++index)
        text(control,static_cast<int>(index),&empty,0,0,1,12,color,shadow,0,font,align);
    text(
        control, 0, &native, 2, 3,
        std::max(1, Read<int>(control, 0x154) - 4),
        std::max(12, Read<int>(control, 0x158) - 4),
        color, shadow, 0, font, align);
}

void Bindings::SetState(
    void* control,
    bool visible,
    bool enabled) const noexcept {
    if (!control) return;
    __try {
        Put<std::uint8_t>(control, 0x162, visible ? 1 : 0);
        // Native +0x163 gates drawing; +0x167 gates interaction.
        // Read-only captions and disabled buttons must remain visible.
        Put<std::uint8_t>(control, 0x163, visible ? 1 : 0);
        Put<std::uint8_t>(control, 0x167, enabled ? 1 : 0);
        if (!enabled) {
            // +0x21C is a button state, but a CUIStatic string or combo-box
            // selection in other widget classes. Never reset those as buttons.
            if (Read<std::uint32_t>(control,0x134)==4)
                Put<std::uint16_t>(control, 0x21C, 0);
            Put<std::uint8_t>(control, 0x164, 0);
            Put<std::uint32_t>(control, 0x18C, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool FriendEntry::Attach(
    void* service,
    const Bindings& bindings) {
    bindings.last_error = "missing friend service";
    if (!service) return false;
    auto* dialog = Read<void*>(service, 0x4C);
    bindings.last_error = "missing friend dialog";
    if (!dialog) return false;
    auto* source = bindings.FindNamed(dialog, L"Btn5", 4);
    bindings.last_error = "friend source button parent";
    if (!ChildOf(source, dialog)) return false;

    const int width = Read<int>(source, 0x154);
    const int height = Read<int>(source, 0x158);
    bindings.last_error = "friend source button dimensions";
    if (width < 80 || width > 240 || height < 16 || height > 40) {
        return false;
    }
    const int y =
        static_cast<int>(Read<float>(source, 0x14C)) + height + 1;
    const int dialogHeight = Read<int>(dialog, 0x158);
    bindings.last_error = "friend dialog height";
    if (dialogHeight <= 0 || dialogHeight > 4096) return false;
    // 0572 draws separate strip icons; changing only dialog height leaves the
    // added row outside its frame. Clone its last strip before the new button
    // so the stock renderer keeps the original child order and ownership.
    auto* bottom = bindings.FindNamed(dialog,L"Icon1",1);
    auto* strip = bindings.FindNamed(dialog,L"Icon8",1);
    if (bottom && strip && ChildOf(bottom,dialog) && ChildOf(strip,dialog) &&
        bindings.refresh_geometry && bindings.add_icon) {
        const int x = static_cast<int>(Read<float>(source,0x148));
        const auto key = View(L"ModFriendUnitGiftBackground");
        bindings.add_icon(dialog,&key,x,y,width,height);
        auto* background = bindings.FindNamed(dialog,L"ModFriendUnitGiftBackground",1);
        if (background) {
            auto** skins = Read<std::uint8_t**>(strip,0x1A4);
            auto** end = Read<std::uint8_t**>(strip,0x1A8);
            if (!skins || end != skins+1 || !*skins || !Read<void*>(*skins)) return false;
            bindings.skin(background,reinterpret_cast<const NativeString*>(*skins+4),
                Read<std::uint8_t>(*skins,0x44),Read<std::uint32_t>(*skins,0x3C),
                Read<std::uint32_t>(*skins,0x40));
            background_ = background;
            // Native controls default to drawing a random debug rectangle.
            // Loaded stock controls turn this off; appended icons must too.
            Put<std::uint8_t>(background_,0x168,0);
            bottom_ = bottom;
            original_bottom_y_ = Read<float>(bottom,0x14C);
            expanded_bottom_y_ = original_bottom_y_+
                static_cast<float>(y)-Read<float>(source,0x14C);
        }
    }
    auto* button = bindings.AddButton(
        dialog, L"ModFriendUnitGift", 0, y,
        width, height, source, L"赠送机体");
    if (!button) return false;

    bindings_ = &bindings;
    service_ = service;
    dialog_ = dialog;
    button_ = button;
    original_dialog_height_ = dialogHeight;
    expanded_dialog_height_ =
        std::max(dialogHeight, y + height);
    if (bottom_) expanded_dialog_height_ = std::max(expanded_dialog_height_,
        static_cast<int>(expanded_bottom_y_)+Read<int>(bottom_,0x158));
    geometry_captured_ = true;
    geometry_expanded_ = false;
    SetAvailable(available_);
    return true;
}

std::wstring Bindings::UnitCaption(std::uint32_t id,std::uint32_t level) const {
    const auto name=unit_name ? unit_name(id) : L"机体名称暂不可用";
    return L"Lv."+std::to_wstring(level)+L" · "+name;
}

bool FriendEntry::Click(
    void* service,
    void* control) const noexcept {
    return available_ && service && service == service_ &&
           control && control == button_;
}

void FriendEntry::SetAvailable(bool available) noexcept {
    available_ = available;
    ApplyGeometry(available);
    if (bindings_) {
        bindings_->SetState(button_, available, available);
        bindings_->SetState(background_,available,available);
        if (background_) {
            Put<std::uint8_t>(background_,0x167,0);
            Put<std::uint32_t>(background_,0x18C,0);
        }
    }
}

void FriendEntry::Detach(void* service) noexcept {
    if (service != service_) return;
    ApplyGeometry(false);
    if (bindings_) {
        bindings_->SetState(button_, false, false);
        bindings_->SetState(background_, false, false);
    }
    bindings_ = nullptr;
    service_ = nullptr;
    dialog_ = nullptr;
    button_ = nullptr;
    background_ = nullptr;
    bottom_ = nullptr;
    original_bottom_y_ = 0.0f;
    expanded_bottom_y_ = 0.0f;
    original_dialog_height_ = 0;
    expanded_dialog_height_ = 0;
    geometry_captured_ = false;
    geometry_expanded_ = false;
    available_ = false;
}

void FriendEntry::ApplyGeometry(bool expanded) noexcept {
    if (!dialog_ || !geometry_captured_ || expanded == geometry_expanded_) return;
    __try {
        if (!expanded) {
            Put<int>(
                dialog_, 0x158, original_dialog_height_);
            if (bottom_) {
                Put<float>(bottom_,0x14C,original_bottom_y_);
                bindings_->refresh_geometry(bottom_);
            }
            if (bindings_->refresh_geometry) bindings_->refresh_geometry(dialog_);
            geometry_expanded_ = false;
            return;
        }
        Put<int>(
            dialog_, 0x158, expanded_dialog_height_);
        if (bottom_) {
            Put<float>(bottom_,0x14C,expanded_bottom_y_);
            bindings_->refresh_geometry(bottom_);
        }
        if (bindings_->refresh_geometry) bindings_->refresh_geometry(dialog_);
        // Native positioning also propagates the cursor location to children.
        // Replaying cached dialog coordinates on each availability tick moves
        // the menu away from the right-click and offsets those children again.
        if (bindings_->place_popup) bindings_->place_popup(service_,0);
        geometry_expanded_ = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Bindings::SetUnitIcon(void* control,std::uint32_t unit) const {
    if (!control || !unit_icon || !skin || !clear_skins) return;
    const auto path=unit_icon(unit);
    if (path.empty()) return;
    auto** begin=Read<std::uint8_t**>(control,0x1A4);
    if (begin && *begin) {
        const auto* name=reinterpret_cast<const NativeString*>(*begin+4);
        const auto* resourceName=name->capacity<8 ? name->data.small : name->data.pointer;
        if (resourceName && name->length==path.size() && !std::wmemcmp(resourceName,path.data(),path.size())) return;
    }
    clear_skins(control);
    const auto resource=View(path);skin(control,&resource,0,0,0);
    begin=Read<std::uint8_t**>(control,0x1A4);
    auto* sprite=begin && *begin ? Read<std::uint8_t*>(*begin) : nullptr;
    auto* data=sprite ? Read<std::uint8_t*>(sprite,0x168) : nullptr;
    auto** frames=data ? Read<std::uint8_t**>(data,0xC) : nullptr;
    if (frames && *frames) {
        const int width=Read<int>(*frames,4),height=Read<int>(*frames,8);
        if (width>0 && height>0) {
            const float scale=std::min(50.0f/width,50.0f/height);
            Put<float>(sprite,0x94,scale);Put<float>(sprite,0x98,scale);Put<std::uint8_t>(sprite,0xA0,1);
        }
    }
    Put<std::uint8_t>(control,0x168,0);
}

bool StorageView::Attach(void* service,void* root,const Bindings& bindings) {
    if (!service || !root || Read<void*>(service,0x70)!=root) return false;
    auto* source=Read<void*>(service,0xB8);
    if (!ChildOf(source,root) || !ChildOf(Read<void*>(service,0xB4),root)) return false;
    auto* button=bindings.AddButton(root,L"ModFriendUnitGifts",282,32,124,23,source,L"机体赠礼");
    if (!button) return false;
    bindings_=&bindings;service_=service;dialog_=root;button_=button;
    returned_button_=Read<void*>(service,0xB8);
    status_=Read<void*>(service,0x98);
    previous_=Read<void*>(service,0xA4);next_=Read<void*>(service,0xA8);
    receipt_=Read<void*>(service,0xA0);
    for (std::size_t i=0;i<rows_.size();++i) {
        const auto base=0xBC+i*0x20;
        rows_[i]=Read<void*>(service,base);
        native_return_[i]=Read<void*>(service,base+0x14);
        native_accept_[i]=Read<void*>(service,base+0x18);
        if (!ChildOf(rows_[i],root) || !ChildOf(native_accept_[i],root) || !ChildOf(native_return_[i],root)) return false;
    }
    claim_all_=bindings.FindNamed(root,L"ModStorageClaimAll",4);
    SetAvailable(available_);
    return true;
}

void StorageView::Detach(void* service) noexcept {
    if (service != service_) return;
    RestoreStockControls(false);
    ApplyClaimAllVisibility(true);
    if (bindings_) {
        bindings_->SetState(button_, false, false);
        ApplyCustomVisibility(false);
    }
    bindings_ = nullptr;
    service_ = nullptr;
    dialog_ = nullptr;
    button_ = nullptr;
    returned_button_ = nullptr;
    status_ = nullptr;
    claim_all_ = nullptr;
    claim_all_state_ = {};
    claim_all_hidden_ = false;
    rows_.fill(nullptr);
    row_gifts_.fill(0);
    previous_ = nullptr;
    next_ = nullptr;
    accept_ = nullptr;
    return_ = nullptr;
    takeback_ = nullptr;
    provenance_ = nullptr;
    receipt_ = nullptr;
    custom_view_ = false;
    stock_controls_ = {};
    stock_control_count_ = 0;
    stock_hidden_ = false;
}

bool StorageView::Click(
    void* service,
    void* control) const noexcept {
    return available_ && service == service_ && control == button_;
}

StorageView::Action StorageView::ActionFor(void* service,void* control) const noexcept {
    if (!available_ || service!=service_ || !control || !Interactive(control)) return Action::none;
    if (control==button_) return Action::toggle;
    if (!custom_view_) return Action::none;
    if (control==Read<void*>(service_,0xB4)) return Action::open_inbox;
    if (control==returned_button_) return Action::open_returned;
    for (std::size_t i=0;i<rows_.size();++i) {
        if (control==rows_[i]) return static_cast<Action>(static_cast<std::uint8_t>(Action::row0)+i);
        if (control==native_accept_[i]) return row_directions_[i]==ListDirection::returned ? Action::takeback : Action::accept;
        if (control==native_return_[i]) return Action::return_to_sender;
        if (control==Read<void*>(service_,0xC0+i*0x20)) return Action::provenance;
    }
    if (control==previous_) return Action::previous_page;
    if (control==next_) return Action::next_page;
    if (control==receipt_ && receipt_available_) return Action::receipt;
    return Action::none;
}

void StorageView::SetAvailable(bool available) {
    available_=available;
    if (!available && custom_view_) {
        custom_view_=false;RestoreStockControls();ApplyClaimAllVisibility(true);ApplyCustomVisibility(false);
    }
    if (bindings_) bindings_->SetState(button_,available,available);
}

void StorageView::ShowStatus(const std::wstring& status,bool customView) {
    const bool changed=custom_view_!=customView;custom_view_=customView;
    if (!bindings_) return;
    if (changed) {if (customView) HideStockControls();else RestoreStockControls();}
    if (customView) {bindings_->SetText(status_,status,1);bindings_->SetState(status_,true,false);}
    ApplyClaimAllVisibility(!customView);ApplyCustomVisibility(customView);
}

void StorageView::RenderRows(const GiftSummary* rows,std::size_t count,std::uint32_t page,std::uint32_t total,
    std::uint64_t selectedGift,bool allowMutation,bool allowReceipt) {
    if (!bindings_ || !custom_view_) return;
    if (!stock_hidden_) HideStockControls();
    row_gifts_.fill(0);accept_=return_=takeback_=provenance_=nullptr;
    receipt_available_=allowReceipt;
    const auto shown=std::min(count,rows_.size());
    for (std::size_t i=0;i<rows_.size();++i) {
        const auto base=0xBC+i*0x20;
        const bool present=rows && i<shown && rows[i].gift_id;
        for (std::size_t field=0;field<8;++field)
            bindings_->SetState(Read<void*>(service_,base+field*4),false,false);
        if (!present) continue;
        const auto& row=rows[i];row_gifts_[i]=row.gift_id;row_directions_[i]=row.direction;
        auto* icon=Read<void*>(service_,base+4);
        auto* name=Read<void*>(service_,base+8);
        auto* product=Read<void*>(service_,base+0xC);
        auto* date=Read<void*>(service_,base+0x10);
        bindings_->SetState(rows_[i],true,true);
        bindings_->SetState(icon,true,true);
        bindings_->SetUnitIcon(icon,row.unit_number);
        bindings_->SetState(name,true,false);bindings_->SetState(product,true,false);bindings_->SetState(date,true,false);
        bindings_->SetText(name,bindings_->UnitCaption(row.unit_number,row.level),1);
        bindings_->SetText(product,GiftStateText(row.state),1);
        bindings_->SetText(date,Utf16(row.peer_name,row.peer_name_code_units),1);

        const bool pending=row.direction==ListDirection::inbox && row.state==GiftState::pending;
        const bool returned=row.direction==ListDirection::returned;
        bindings_->SetState(native_accept_[i],true,allowMutation && (pending || returned));
        bindings_->SetState(native_return_[i],false,false);
        if (row.gift_id==selectedGift) {
            accept_=native_accept_[i];takeback_=native_accept_[i];return_=native_return_[i];provenance_=icon;
        }
    }
    bindings_->SetState(previous_,true,page>1);
    bindings_->SetState(next_,true,page<(total ? (total+4u)/5u:1u));
    bindings_->SetState(status_,true,false);
    // The native header owns its count caption and does not reset it for an
    // empty ordinary inbox. Keep that caption intact when borrowing the rows.
    bindings_->SetState(Read<void*>(service_,0x94),false,false);
    if (bindings_->set_number) {
        auto* current=Read<void*>(service_,0xAC);auto* maximum=Read<void*>(service_,0xB0);
        bindings_->set_number(current,page);bindings_->set_number(maximum,total ? (total+4u)/5u:1u);
        bindings_->SetState(current,true,false);bindings_->SetState(maximum,true,false);
    }
    bindings_->SetText(button_,L"普通保管库");
    bindings_->SetText(Read<void*>(service_,0xB4),L"收到的机体");
    bindings_->SetText(Read<void*>(service_,0xB8),L"退回的机体");
    bindings_->SetState(Read<void*>(service_,0xB4),true,true);bindings_->SetState(Read<void*>(service_,0xB8),true,true);
    Put<std::uint8_t>(Read<void*>(service_,0xB4),0x168,0);Put<std::uint8_t>(Read<void*>(service_,0xB8),0x168,0);
    bindings_->SetText(receipt_,allowReceipt ? L"查询待确认结果":L"关闭");
    ApplyClaimAllVisibility(false);
}

std::uint64_t StorageView::GiftForControl(void* control) const noexcept {
    for (std::size_t i=0;i<rows_.size();++i) {
        const auto base=0xBC+i*0x20;
        for (std::size_t f=0;f<8;++f)
            if (Read<void*>(service_,base+f*4)==control) return row_gifts_[i];
    }
    return 0;
}

void StorageView::EnforceCustomView() noexcept {
    if (!custom_view_) return;
    ApplyClaimAllVisibility(false);
}

std::uint64_t StorageView::GiftFor(Action action) const noexcept {
    const auto first = static_cast<std::uint8_t>(Action::row0);
    const auto value = static_cast<std::uint8_t>(action);
    if (value < first ||
        value >= first + static_cast<std::uint8_t>(rows_.size())) {
        return 0;
    }
    return row_gifts_[value - first];
}

StorageView::Action StorageView::RowActionForGift(
    std::uint64_t gift) const noexcept {
    if (!gift) return Action::none;
    for (std::size_t index = 0; index < row_gifts_.size(); ++index) {
        if (row_gifts_[index] == gift) {
            return static_cast<Action>(
                static_cast<std::uint8_t>(Action::row0) +
                static_cast<std::uint8_t>(index));
        }
    }
    return Action::none;
}

void* StorageView::ControlFor(Action action) const noexcept {
    switch (action) {
    case Action::toggle:
        return button_;
    case Action::open_returned:
        return returned_button_;
    case Action::open_inbox:
        return service_ ? Read<void*>(service_,0xB4) : nullptr;
    case Action::previous_page:
        return previous_;
    case Action::next_page:
        return next_;
    case Action::accept:
        return accept_;
    case Action::return_to_sender:
        return return_;
    case Action::takeback:
        return takeback_;
    case Action::provenance:
        return provenance_;
    case Action::receipt:
        return receipt_;
    default:
        break;
    }
    const auto first = static_cast<std::uint8_t>(Action::row0);
    const auto value = static_cast<std::uint8_t>(action);
    if (value >= first &&
        value < first + static_cast<std::uint8_t>(rows_.size())) {
        return rows_[value - first];
    }
    return nullptr;
}

void StorageView::ApplyClaimAllVisibility(bool visible) noexcept {
    if (!claim_all_) return;
    if (!visible) {
        if (!claim_all_hidden_) {
            __try {
                claim_all_state_.control = claim_all_;
                claim_all_state_.visible =
                    Read<std::uint8_t>(claim_all_, 0x162);
                claim_all_state_.enabled =
                    Read<std::uint8_t>(claim_all_, 0x163);
                claim_all_state_.hit_test =
                    Read<std::uint8_t>(claim_all_, 0x167);
                claim_all_hidden_ = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                claim_all_state_ = {};
                return;
            }
        }
        __try {
            Put<std::uint8_t>(claim_all_, 0x162, 0);
            Put<std::uint8_t>(claim_all_, 0x163, 0);
            Put<std::uint8_t>(claim_all_, 0x167, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    if (claim_all_hidden_) {
        __try {
            Put<std::uint8_t>(
                claim_all_, 0x162, claim_all_state_.visible);
            Put<std::uint8_t>(
                claim_all_, 0x163, claim_all_state_.enabled);
            Put<std::uint8_t>(
                claim_all_, 0x167, claim_all_state_.hit_test);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        claim_all_state_ = {};
        claim_all_hidden_ = false;
    }
}

void StorageView::ApplyCustomVisibility(bool visible) noexcept {
    if (bindings_) bindings_->SetText(button_,visible ? L"普通保管库" : L"机体赠礼");
}

void StorageView::HideStockControls() noexcept {
    if (!service_) return;
    if (stock_hidden_) {
        for (std::size_t index = 0;
             index < stock_control_count_;
             ++index) {
            const auto& state = stock_controls_[index];
            if (!state.control) continue;
            __try {
                Put<std::uint8_t>(
                    state.control, 0x162, 0);
                Put<std::uint8_t>(
                    state.control, 0x163, 0);
                Put<std::uint8_t>(
                    state.control, 0x167, 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return;
    }
    stock_control_count_ = 0;
    const auto capture = [&](void* control) {
        if (!control ||
            stock_control_count_ >= stock_controls_.size()) {
            return;
        }
        auto& state = stock_controls_[stock_control_count_++];
        state.control = control;
        __try {
            state.visible = Read<std::uint8_t>(control, 0x162);
            state.enabled = Read<std::uint8_t>(control, 0x163);
            state.hit_test = Read<std::uint8_t>(control, 0x167);
            state.x=Read<float>(control,0x148);
            state.width=Read<int>(control,0x154);
            Put<std::uint8_t>(control, 0x162, 0);
            Put<std::uint8_t>(control, 0x163, 0);
            Put<std::uint8_t>(control, 0x167, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            state = {};
            --stock_control_count_;
        }
    };
    for (const auto offset : {
             0x94u, 0x98u, 0xA4u, 0xA8u, 0xACu, 0xB0u, 0xB4u, 0xB8u}) {
        capture(Read<void*>(service_, offset));
    }
    for (std::size_t row = 0; row < 5; ++row) {
        const std::size_t base = 0xBC + row * 0x20;
        for (std::size_t field = 0; field < 8; ++field) {
            capture(Read<void*>(
                service_, base + field * 4u));
        }
    }
    stock_hidden_ = true;
}

void StorageView::RestoreStockControls(bool render) noexcept {
    if (!stock_hidden_) return;
    for (std::size_t index = 0;
         index < stock_control_count_;
         ++index) {
        const auto& state = stock_controls_[index];
        if (!state.control) continue;
        __try {
            Put<std::uint8_t>(
                state.control, 0x162, state.visible);
            Put<std::uint8_t>(
                state.control, 0x163, state.enabled);
            Put<std::uint8_t>(
                state.control, 0x167, state.hit_test);
            Put<float>(state.control,0x148,state.x);
            Put<int>(state.control,0x154,state.width);
            if (bindings_ && bindings_->refresh_geometry) bindings_->refresh_geometry(state.control);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    stock_controls_ = {};
    stock_control_count_ = 0;
    stock_hidden_ = false;
    if (render && bindings_ && bindings_->restore_storage && service_) bindings_->restore_storage(service_);
}

bool UnitDetailView::Attach(
    void* service,
    void* root,
    void* skinSource,
    const Bindings& bindings) {
    if (!service || !root || !skinSource ||
        Read<void*>(skinSource) != bindings.button_vtable ||
        !Read<void*>(skinSource, 0x15C)) {
        return false;
    }
    if (service_ && service_ != service) {
        Detach(service_);
    }
    auto* render = Read<void*>(service, 0x4C);
    if (!ChildOf(render, root)) return false;
    auto* button = bindings.AddButton(
        root,
        L"ModFriendUnitGiftProvenance",
        118,
        268,
        284,
        15,
        skinSource,
        L"查看赠送来源");
    auto* detail = bindings.AddTextButton(
        root,
        L"ModFriendUnitGiftProvenanceDetail",
        118,
        41,
        284,
        227,
        L"");
    if (!button || !detail) {
        bindings.SetState(button, false, false);
        bindings.SetState(detail, false, false);
        return false;
    }
    bindings_ = &bindings;
    service_ = service;
    root_ = root;
    button_ = button;
    detail_ = detail;
    render_ = render;
    render_visible_ = 0;
    render_enabled_ = 0;
    render_hit_test_ = 0;
    render_state_captured_ = false;
    detail_visible_ = false;
    shown_ = false;
    SetUnit(0, false, false, L"查看赠送来源");
    bindings.SetState(detail_, false, false);
    return true;
}

void UnitDetailView::Detach(void* service) noexcept {
    if (service != service_) return;
    ApplyDetailVisibility(false);
    if (bindings_) {
        bindings_->SetState(button_, false, false);
        bindings_->SetState(detail_, false, false);
    }
    bindings_ = nullptr;
    service_ = nullptr;
    root_ = nullptr;
    button_ = nullptr;
    detail_ = nullptr;
    render_ = nullptr;
    unit_uid_ = 0;
    render_visible_ = 0;
    render_enabled_ = 0;
    render_hit_test_ = 0;
    render_state_captured_ = false;
    detail_visible_ = false;
    shown_ = false;
    available_ = false;
}

void UnitDetailView::SetUnit(
    std::uint64_t unitUid,
    bool visible,
    bool enabled,
    const std::wstring& caption) {
    if (unit_uid_ != unitUid) {
        ApplyDetailVisibility(false);
        if (bindings_) bindings_->SetText(detail_, L"", 1);
    }
    unit_uid_ = unitUid;
    const bool shown = visible && unitUid != 0;
    shown_ = shown;
    available_ = shown && enabled;
    if (!bindings_) return;
    if (!shown) ApplyDetailVisibility(false);
    bindings_->SetText(button_, caption, 1);
    bindings_->SetState(
        button_, shown, available_);
}

void UnitDetailView::SetDetail(
    const std::wstring& detail,
    bool visible) {
    if (!bindings_ || !detail_) return;
    bindings_->SetText(detail_, detail, 1);
    ApplyDetailVisibility(
        visible && shown_ && unit_uid_ != 0);
}

void UnitDetailView::ToggleDetail() {
    ApplyDetailVisibility(!detail_visible_);
}

bool UnitDetailView::Click(
    void* service,
    void* control) const noexcept {
    return ready() && service == service_ &&
           control && control == button_;
}

void UnitDetailView::ApplyDetailVisibility(
    bool visible) noexcept {
    if (!bindings_ || !detail_ || !render_) return;
    if (visible) {
        if (!render_state_captured_) {
            __try {
                render_visible_ =
                    Read<std::uint8_t>(render_, 0x162);
                render_enabled_ =
                    Read<std::uint8_t>(render_, 0x163);
                render_hit_test_ =
                    Read<std::uint8_t>(render_, 0x167);
                render_state_captured_ = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return;
            }
        }
        __try {
            Put<std::uint8_t>(render_, 0x162, 0);
            Put<std::uint8_t>(render_, 0x163, 0);
            Put<std::uint8_t>(render_, 0x167, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        bindings_->SetState(detail_, true, false);
        detail_visible_ = true;
        return;
    }

    bindings_->SetState(detail_, false, false);
    if (render_state_captured_) {
        __try {
            Put<std::uint8_t>(
                render_, 0x162, render_visible_);
            Put<std::uint8_t>(
                render_, 0x163, render_enabled_);
            Put<std::uint8_t>(
                render_, 0x167, render_hit_test_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    render_visible_ = 0;
    render_enabled_ = 0;
    render_hit_test_ = 0;
    render_state_captured_ = false;
    detail_visible_ = false;
}

} // namespace sdgo::friend_unit_gift::client::ui
