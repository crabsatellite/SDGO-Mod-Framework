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
                !Read<void*>(first[index])) {
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
    button_vtable =
        api->Symbol(mod, "friend_unit_gift.ui_button_vtable");
    edit_vtable = api->Symbol(mod, "friend_unit_gift.ui_edit_vtable");
    input_limit = reinterpret_cast<InputLimit>(
        api->Symbol(mod, "friend_unit_gift.ui_input_limit"));
    input_context = reinterpret_cast<void**>(
        api->Symbol(mod, "friend_unit_gift.ui_input_context"));
    input_lock = api->Symbol(mod, "friend_unit_gift.ui_input_lock");
    return add && find && text && skin && button_vtable &&
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
    if (!control || !output || Read<void*>(control)!=button_vtable) return false;
    auto** first=Read<NativeString**>(control,0x190);
    auto** last=Read<NativeString**>(control,0x194);
    const auto start=reinterpret_cast<std::uintptr_t>(first);
    const auto end=reinterpret_cast<std::uintptr_t>(last);
    if (!first || end<=start || end-start>64*sizeof(void*) || (end-start)%sizeof(void*)) return false;
    const auto* value=*first;
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
    if (!dialog || !name || !skinSource ||
        !add || !find || !text || !skin ||
        width <= 0 || height <= 0) {
        return nullptr;
    }
    if (auto* existing = FindNamed(dialog, name, 4)) {
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
    if (count < 3 || count > 16) return nullptr;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!first[index] || !Read<void*>(first[index])) return nullptr;
        skin(
            button,
            reinterpret_cast<const NativeString*>(first[index] + 4),
            Read<std::uint8_t>(first[index], 0x44),
            Read<std::uint32_t>(first[index], 0x3C),
            Read<std::uint32_t>(first[index], 0x40));
    }
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
        Put<std::uint8_t>(control, 0x163, enabled ? 1 : 0);
        Put<std::uint8_t>(control, 0x167, enabled ? 1 : 0);
        if (!enabled) {
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
    if (!service) return false;
    auto* dialog = Read<void*>(service, 0x4C);
    if (!dialog) return false;
    auto* source = bindings.FindNamed(dialog, L"Btn5", 4);
    if (!ChildOf(source, dialog)) return false;

    const int width = Read<int>(source, 0x154);
    const int height = Read<int>(source, 0x158);
    if (width < 80 || width > 240 || height < 16 || height > 40) {
        return false;
    }
    const int y =
        static_cast<int>(Read<float>(source, 0x14C)) + height + 1;
    const int dialogHeight = Read<int>(dialog, 0x158);
    if (dialogHeight <= 0 || dialogHeight > 4096) return false;
    auto* button = bindings.AddButton(
        dialog, L"ModFriendUnitGift", 0, y,
        width, height, source, L"赠送机体");
    if (!button) return false;

    bindings_ = &bindings;
    service_ = service;
    dialog_ = dialog;
    button_ = button;
    original_dialog_x_ = Read<float>(dialog, 0x148);
    original_dialog_y_ = Read<float>(dialog, 0x14C);
    original_dialog_height_ = dialogHeight;
    expanded_dialog_height_ =
        std::max(dialogHeight, y + height);
    geometry_captured_ = true;
    SetAvailable(available_);
    return true;
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
    }
}

void FriendEntry::Detach(void* service) noexcept {
    if (service != service_) return;
    ApplyGeometry(false);
    if (bindings_) {
        bindings_->SetState(button_, false, false);
    }
    bindings_ = nullptr;
    service_ = nullptr;
    dialog_ = nullptr;
    button_ = nullptr;
    original_dialog_x_ = 0.0f;
    original_dialog_y_ = 0.0f;
    original_dialog_height_ = 0;
    expanded_dialog_height_ = 0;
    geometry_captured_ = false;
    available_ = false;
}

void FriendEntry::ApplyGeometry(bool expanded) noexcept {
    if (!dialog_ || !geometry_captured_) return;
    __try {
        if (!expanded) {
            Put<float>(dialog_, 0x148, original_dialog_x_);
            Put<float>(dialog_, 0x14C, original_dialog_y_);
            Put<int>(
                dialog_, 0x158, original_dialog_height_);
            return;
        }

        float x = original_dialog_x_;
        float y = original_dialog_y_;
        const int width = Read<int>(dialog_, 0x154);
        auto* viewport = Read<void*>(dialog_, 0x15C);
        if (viewport && width > 0) {
            const int viewportWidth =
                Read<int>(viewport, 0x154);
            const int viewportHeight =
                Read<int>(viewport, 0x158);
            if (viewportWidth >= width &&
                viewportWidth <= 16384) {
                x = std::clamp(
                    x,
                    0.0f,
                    static_cast<float>(viewportWidth - width));
            }
            if (viewportHeight >= expanded_dialog_height_ &&
                viewportHeight <= 16384) {
                y = std::clamp(
                    y,
                    0.0f,
                    static_cast<float>(
                        viewportHeight -
                        expanded_dialog_height_));
            }
        }
        Put<float>(dialog_, 0x148, x);
        Put<float>(dialog_, 0x14C, y);
        Put<int>(
            dialog_, 0x158, expanded_dialog_height_);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool StorageView::Attach(
    void* service,
    void* root,
    const Bindings& bindings) {
    if (!service || !root || Read<void*>(service, 0x70) != root) {
        return false;
    }
    auto* source = Read<void*>(service, 0xB8);
    if (!ChildOf(source, root)) return false;

    auto* button = bindings.AddButton(
        root, L"ModFriendUnitGifts", 291, 417, 124, 24,
        source, L"机体礼物");
    auto* returnedButton = bindings.AddButton(
        root, L"ModFriendUnitGiftReturned", 194, 417, 94, 24,
        source, L"退回机体");
    auto* status = bindings.AddButton(
        root, L"ModFriendUnitGiftStatus", 35, 464, 380, 14,
        source, L"");
    if (!button || !returnedButton || !status) return false;

    bindings_ = &bindings;
    service_ = service;
    dialog_ = root;
    button_ = button;
    returned_button_ = returnedButton;
    status_ = status;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const auto name =
            L"ModFriendUnitGiftRow" + std::to_wstring(index);
        rows_[index] = bindings.AddButton(
            root,
            name.c_str(),
            35,
            100 + static_cast<int>(index) * 52,
            380,
            44,
            source,
            L"");
        if (!rows_[index]) return false;
    }
    previous_ = bindings.AddButton(
        root, L"ModFriendUnitGiftPrevious", 35, 417, 74, 24,
        source, L"上一页");
    next_ = bindings.AddButton(
        root, L"ModFriendUnitGiftNext", 112, 417, 74, 24,
        source, L"下一页");
    accept_ = bindings.AddButton(
        root, L"ModFriendUnitGiftAccept", 35, 370, 88, 24,
        source, L"接收");
    return_ = bindings.AddButton(
        root, L"ModFriendUnitGiftReturn", 126, 370, 88, 24,
        source, L"退回");
    takeback_ = bindings.AddButton(
        root, L"ModFriendUnitGiftTakeback", 217, 370, 88, 24,
        source, L"取回");
    provenance_ = bindings.AddButton(
        root, L"ModFriendUnitGiftProvenance", 308, 370, 107, 24,
        source, L"来源");
    receipt_ = bindings.AddButton(
        root, L"ModFriendUnitGiftReceipt", 217, 398, 198, 16,
        source, L"查询待确认结果");
    if (!previous_ || !next_ || !accept_ || !return_ ||
        !takeback_ || !provenance_ || !receipt_) {
        return false;
    }
    claim_all_ =
        bindings.FindNamed(root, L"ModStorageClaimAll", 4);
    bindings.SetState(status_, false, false);
    ApplyCustomVisibility(false);
    SetAvailable(available_);
    return true;
}

void StorageView::Detach(void* service) noexcept {
    if (service != service_) return;
    RestoreStockControls();
    ApplyClaimAllVisibility(true);
    if (bindings_) {
        bindings_->SetState(button_, false, false);
        bindings_->SetState(returned_button_, false, false);
        bindings_->SetState(status_, false, false);
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

StorageView::Action StorageView::ActionFor(
    void* service,
    void* control) const noexcept {
    if (!available_ || service != service_ || !control) {
        return Action::none;
    }
    if (!Interactive(control)) return Action::none;
    if (control == button_) return Action::toggle;
    if (control == returned_button_) return Action::open_returned;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (control == rows_[index]) {
            return static_cast<Action>(
                static_cast<std::uint8_t>(Action::row0) +
                static_cast<std::uint8_t>(index));
        }
    }
    if (control == previous_) return Action::previous_page;
    if (control == next_) return Action::next_page;
    if (control == accept_) return Action::accept;
    if (control == return_) return Action::return_to_sender;
    if (control == takeback_) return Action::takeback;
    if (control == provenance_) return Action::provenance;
    if (control == receipt_) return Action::receipt;
    return Action::none;
}

void StorageView::SetAvailable(bool available) {
    available_ = available;
    if (!available && custom_view_) {
        custom_view_ = false;
        RestoreStockControls();
        ApplyClaimAllVisibility(true);
        if (bindings_) {
            bindings_->SetState(status_, false, false);
            ApplyCustomVisibility(false);
        }
    }
    if (bindings_) {
        bindings_->SetState(button_, available, available);
        bindings_->SetState(
            returned_button_, available, available);
    }
}

void StorageView::ShowStatus(
    const std::wstring& status,
    bool customView) {
    const bool changed = custom_view_ != customView;
    custom_view_ = customView;
    if (!bindings_) return;
    if (changed) {
        if (customView) HideStockControls();
        else RestoreStockControls();
    }
    if (customView) EnforceCustomView();
    bindings_->SetText(status_, status, 1);
    bindings_->SetState(status_, customView, false);
    ApplyClaimAllVisibility(!customView);
    ApplyCustomVisibility(customView);
}

void StorageView::RenderRows(
    const GiftSummary* rows,
    std::size_t count,
    std::uint32_t page,
    std::uint32_t total,
    std::uint64_t selectedGift,
    bool allowMutation,
    bool allowReceipt) {
    if (!bindings_) return;
    EnforceCustomView();
    row_gifts_.fill(0);
    const auto shown = std::min(count, rows_.size());
    const GiftSummary* selected = nullptr;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const bool present = rows && index < shown &&
                             rows[index].gift_id != 0;
        if (present) {
            row_gifts_[index] = rows[index].gift_id;
            if (rows[index].gift_id == selectedGift) {
                selected = &rows[index];
            }
            std::wstring caption =
                rows[index].gift_id == selectedGift ? L"▶ " : L"  ";
            caption +=
                L"机体 #" + std::to_wstring(rows[index].unit_number) +
                L" / UID " + std::to_wstring(rows[index].unit_uid) +
                L" / " + Utf16(
                    rows[index].peer_name,
                    rows[index].peer_name_code_units) +
                L" / " + GiftStateText(rows[index].state);
            bindings_->SetText(rows_[index], caption, 1);
        } else {
            bindings_->SetText(rows_[index], L"", 1);
        }
        bindings_->SetState(
            rows_[index], custom_view_ && present, present);
    }
    const std::uint32_t maximumPage =
        total ? (total + 4u) / 5u : 1u;
    bindings_->SetState(
        previous_, custom_view_, page > 1);
    bindings_->SetState(
        next_, custom_view_, page < maximumPage);

    const bool inboxPending =
        selected &&
        selected->direction == ListDirection::inbox &&
        selected->state == GiftState::pending;
    const bool returned =
        selected &&
        selected->direction == ListDirection::returned;
    bindings_->SetState(
        accept_, custom_view_, allowMutation && inboxPending);
    bindings_->SetState(
        return_, custom_view_, allowMutation && inboxPending);
    bindings_->SetState(
        takeback_, custom_view_, allowMutation && returned);
    bindings_->SetState(
        provenance_, custom_view_, selected != nullptr);
    bindings_->SetState(
        receipt_, custom_view_ && allowReceipt, allowReceipt);
}

void StorageView::EnforceCustomView() noexcept {
    if (!custom_view_) return;
    HideStockControls();
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
    if (!bindings_) return;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        bindings_->SetState(
            rows_[index],
            visible && row_gifts_[index] != 0,
            visible && row_gifts_[index] != 0);
    }
    for (auto* control : {
             previous_, next_, accept_, return_, takeback_, provenance_,
             receipt_}) {
        bindings_->SetState(control, visible, false);
    }
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
            Put<std::uint8_t>(control, 0x162, 0);
            Put<std::uint8_t>(control, 0x163, 0);
            Put<std::uint8_t>(control, 0x167, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            state = {};
            --stock_control_count_;
        }
    };
    for (const auto offset : {
             0x94u, 0x98u, 0xA4u, 0xA8u, 0xACu, 0xB0u}) {
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

void StorageView::RestoreStockControls() noexcept {
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
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    stock_controls_ = {};
    stock_control_count_ = 0;
    stock_hidden_ = false;
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
