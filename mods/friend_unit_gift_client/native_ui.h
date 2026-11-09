#pragma once

#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/sdgo_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sdgo::friend_unit_gift::client::ui {

struct NativeString {
    union {
        wchar_t small[8];
        const wchar_t* pointer;
    } data{};
    std::uint32_t length = 0;
    std::uint32_t capacity = 7;
};
static_assert(sizeof(NativeString) == 24, "native string ABI");

struct Bindings {
    using Add = std::uint32_t(__thiscall*)(
        void*, const NativeString*, int, int, int, int, std::uint32_t);
    using Find = void*(__thiscall*)(void*, const NativeString*, int);
    using Text = void(__thiscall*)(
        void*, int, const NativeString*, int, int, int, int,
        std::uint32_t, std::uint32_t, std::uint8_t, std::uint32_t, int);
    using Skin = void(__thiscall*)(
        void*, const NativeString*, std::uint8_t,
        std::uint32_t, std::uint32_t);
    using EmptySkin = void(__thiscall*)(void*);
    using RefreshGeometry = void(__thiscall*)(void*);
    using AddIcon = std::uint32_t(__thiscall*)(void*,const NativeString*,int,int,int,int);
    // Shipped popup initializer ignores its argument but still ends in RET 4.
    using PlacePopup = void(__thiscall*)(void*,std::uint32_t);
    using InputLimit = void(__thiscall*)(void*, std::uint32_t, int);
    using Caption = void(__thiscall*)(void*,const NativeString*);
    using ClearSkins = void(__thiscall*)(void*);

    bool Bind(const SdgoApi* api, SdgoMod* mod) noexcept;
    void* FindNamed(void* dialog, const wchar_t* name, int type) const;
    void* AddButton(
        void* dialog,
        const wchar_t* name,
        int x,
        int y,
        int width,
        int height,
        void* skinSource,
        const std::wstring& caption) const;
    void* AddTextButton(
        void* dialog,
        const wchar_t* name,
        int x,
        int y,
        int width,
        int height,
        const std::wstring& caption) const;
    void SetText(
        void* control,
        const std::wstring& value,
        int align = 2) const;
    void SetState(
        void* control,
        bool visible,
        bool enabled) const noexcept;
    bool SetNoteLimit(void* edit) const noexcept;
    bool ReadText(void* control, std::wstring* text) const;
    std::wstring UnitCaption(std::uint32_t id,std::uint32_t level) const;
    void SetUnitIcon(void* control,std::uint32_t unit) const;

    Add add = nullptr;
    Find find = nullptr;
    Text text = nullptr;
    Skin skin = nullptr;
    EmptySkin empty_skin = nullptr;
    RefreshGeometry refresh_geometry = nullptr;
    AddIcon add_icon = nullptr;
    PlacePopup place_popup = nullptr;
    void* button_vtable = nullptr;
    void* static_vtable = nullptr;
    Caption static_caption = nullptr;
    ClearSkins clear_skins = nullptr;
    using Number = void(__thiscall*)(void*,std::uint32_t);
    Number set_number = nullptr;
    void (*restore_storage)(void*) = nullptr;
    std::wstring (*unit_icon)(std::uint32_t) = nullptr;
    void* edit_vtable = nullptr;
    InputLimit input_limit = nullptr;
    void** input_context = nullptr;
    void* input_lock = nullptr;
    mutable const char* last_error = "";
    std::wstring (*unit_name)(std::uint32_t) = nullptr;
};

class FriendEntry {
public:
    bool Attach(void* service, const Bindings& bindings);
    bool AttachCommunity(void* service, const Bindings& bindings);
    bool Click(void* service, void* control) const noexcept;
    void SetAvailable(bool available) noexcept;
    void Detach(void* service) noexcept;
    void* service() const noexcept { return service_; }
    void* button() const noexcept { return button_; }
    bool ready() const noexcept {
        return service_ && dialog_ && button_ && available_;
    }

private:
    bool AttachDialog(void* service, const Bindings& bindings,
                      void* dialog, void* source, bool community);
    void ApplyGeometry(bool expanded) noexcept;

    const Bindings* bindings_ = nullptr;
    void* service_ = nullptr;
    void* dialog_ = nullptr;
    void* button_ = nullptr;
    void* background_ = nullptr;
    void* bottom_ = nullptr;
    void* frame_ = nullptr;
    void* frame_sprite_ = nullptr;
    float original_frame_scale_y_ = 1.0f;
    std::uint8_t original_frame_scaled_ = 0;
    int original_frame_height_ = 0;
    float original_bottom_y_ = 0.0f;
    float expanded_bottom_y_ = 0.0f;
    int original_dialog_height_ = 0;
    int expanded_dialog_height_ = 0;
    bool geometry_captured_ = false;
    bool geometry_expanded_ = false;
    bool available_ = false;
};

class StorageView {
public:
    enum class Action : std::uint8_t {
        none,
        toggle,
        open_returned,
        open_inbox,
        row0,
        row1,
        row2,
        row3,
        row4,
        previous_page,
        next_page,
        accept,
        return_to_sender,
        takeback,
        provenance,
        receipt,
    };

    bool Attach(
        void* service,
        void* root,
        const Bindings& bindings);
    // Invalidates cached references; the native page may already be destroyed.
    void Detach(void* service) noexcept;
    bool Click(void* service, void* control) const noexcept;
    Action ActionFor(void* service, void* control) const noexcept;
    void SetAvailable(bool available);
    void ShowStatus(const std::wstring& status, bool customView);
    void RenderRows(
        const GiftSummary* rows,
        std::size_t count,
        std::uint32_t page,
        std::uint32_t total,
        std::uint64_t selectedGift,
        bool allowMutation,
        bool allowReceipt);
    void EnforceCustomView() noexcept;
    std::uint64_t GiftFor(Action action) const noexcept;
    std::uint64_t GiftForControl(void* control) const noexcept;
    Action RowActionForGift(std::uint64_t gift) const noexcept;
    void* ControlFor(Action action) const noexcept;
    bool custom_view() const noexcept { return custom_view_; }
    void* status_control() const noexcept { return status_; }
    bool attached() const noexcept { return service_ != nullptr; }
    void* service() const noexcept { return service_; }
    void* button() const noexcept { return button_; }
    bool ready() const noexcept {
        return service_ && dialog_ && button_ && available_;
    }

private:
    struct StockControlState {
        void* control = nullptr;
        std::uint8_t visible = 0;
        std::uint8_t enabled = 0;
        std::uint8_t hit_test = 0;
        float x = 0.0f;
        int width = 0;
    };

    void ApplyClaimAllVisibility(bool visible) noexcept;
    void ApplyCustomVisibility(bool visible) noexcept;
    void HideStockControls() noexcept;
    void RestoreStockControls(bool render=true) noexcept;

    const Bindings* bindings_ = nullptr;
    void* service_ = nullptr;
    void* dialog_ = nullptr;
    void* button_ = nullptr;
    void* returned_button_ = nullptr;
    void* status_ = nullptr;
    void* claim_all_ = nullptr;
    StockControlState claim_all_state_{};
    bool claim_all_hidden_ = false;
    std::array<void*, 5> rows_{};
    std::array<std::uint64_t, 5> row_gifts_{};
    std::array<void*,5> native_accept_{};
    std::array<void*,5> native_return_{};
    std::array<ListDirection,5> row_directions_{};
    bool receipt_available_=false;
    void* previous_ = nullptr;
    void* next_ = nullptr;
    void* accept_ = nullptr;
    void* return_ = nullptr;
    void* takeback_ = nullptr;
    void* provenance_ = nullptr;
    void* receipt_ = nullptr;
    std::array<StockControlState, 48> stock_controls_{};
    std::size_t stock_control_count_ = 0;
    bool stock_hidden_ = false;
    bool available_ = false;
    bool custom_view_ = false;
};

// Gift origin on the unit-indexing unit-information page (1334). Nothing is
// shown for ordinary units. For a unit the server confirms as a received gift,
// a compact native panel sits on the lower edge of the 3D viewport with the
// sender and dates, and a native blue button opens the full received card.
struct OriginLayout {
    int panel_x=0,panel_y=0,panel_width=0,panel_height=0;
    int text_x=0,title_y=0,dates_y=0,text_width=0,text_height=0;
    int button_x=0,button_y=0,button_width=0,button_height=0;
    bool valid=false;
};
// The viewport is the native 3D render control of the page.
inline OriginLayout OriginLayoutFor(int x,int y,int width,int height) {
    OriginLayout l;
    l.panel_x=x+4;l.panel_width=width-8;l.panel_height=46;l.panel_y=y+height-l.panel_height-4;
    l.button_width=72;l.button_height=24;
    l.button_x=l.panel_x+l.panel_width-l.button_width-8;l.button_y=l.panel_y+(l.panel_height-l.button_height)/2;
    l.text_x=l.panel_x+10;l.text_width=l.button_x-8-l.text_x;l.text_height=15;
    l.title_y=l.panel_y+6;l.dates_y=l.panel_y+25;
    l.valid=width>=200 && height>=120 && x>=0 && y>=0 && l.text_width>=120;
    return l;
}

class UnitDetailView {
public:
    bool Attach(
        void* service,
        void* root,
        const Bindings& bindings);
    // Invalidates cached references; the native owner may already have
    // destroyed the unit-detail page and all of its child controls.
    void Detach(void* service) noexcept;
    // A different unit is displayed: the origin of the previous one is hidden.
    void SetUnit(std::uint64_t unitUid);
    void ShowOrigin(
        const std::wstring& title,
        const std::wstring& dates,
        bool enabled);
    void HideOrigin() noexcept;
    bool Click(void* service, void* control) const noexcept;
    void* service() const noexcept { return service_; }
    void* button() const noexcept { return button_; }
    void* panel() const noexcept { return panel_; }
    void* title() const noexcept { return title_; }
    void* dates() const noexcept { return dates_; }
    std::uint64_t unit_uid() const noexcept { return unit_uid_; }
    bool origin_visible() const noexcept { return visible_; }
    bool ready() const noexcept {
        return service_ && root_ && button_ && unit_uid_ && visible_ && enabled_;
    }

private:
    const Bindings* bindings_ = nullptr;
    void* service_ = nullptr;
    void* root_ = nullptr;
    void* panel_ = nullptr;
    void* title_ = nullptr;
    void* dates_ = nullptr;
    void* button_ = nullptr;
    std::uint64_t unit_uid_ = 0;
    bool visible_ = false;
    bool enabled_ = false;
};

} // namespace sdgo::friend_unit_gift::client::ui
