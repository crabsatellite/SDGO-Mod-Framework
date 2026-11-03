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
    bool Click(void* service, void* control) const noexcept;
    void SetAvailable(bool available) noexcept;
    void Detach(void* service) noexcept;
    void* service() const noexcept { return service_; }
    void* button() const noexcept { return button_; }
    bool ready() const noexcept {
        return service_ && dialog_ && button_ && available_;
    }

private:
    void ApplyGeometry(bool expanded) noexcept;

    const Bindings* bindings_ = nullptr;
    void* service_ = nullptr;
    void* dialog_ = nullptr;
    void* button_ = nullptr;
    void* background_ = nullptr;
    void* bottom_ = nullptr;
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

class UnitDetailView {
public:
    bool Attach(
        void* service,
        void* root,
        void* skinSource,
        const Bindings& bindings);
    void Detach(void* service) noexcept;
    void SetUnit(
        std::uint64_t unitUid,
        bool visible,
        bool enabled,
        const std::wstring& caption);
    void SetDetail(
        const std::wstring& detail,
        bool visible);
    void ToggleDetail();
    bool Click(void* service, void* control) const noexcept;
    void* service() const noexcept { return service_; }
    void* button() const noexcept { return button_; }
    void* detail() const noexcept { return detail_; }
    std::uint64_t unit_uid() const noexcept { return unit_uid_; }
    bool detail_visible() const noexcept {
        return detail_visible_;
    }
    bool ready() const noexcept {
        return service_ && root_ && button_ && detail_ && render_ &&
               unit_uid_ && available_;
    }

private:
    void ApplyDetailVisibility(bool visible) noexcept;

    const Bindings* bindings_ = nullptr;
    void* service_ = nullptr;
    void* root_ = nullptr;
    void* button_ = nullptr;
    void* detail_ = nullptr;
    void* render_ = nullptr;
    std::uint64_t unit_uid_ = 0;
    std::uint8_t render_visible_ = 0;
    std::uint8_t render_enabled_ = 0;
    std::uint8_t render_hit_test_ = 0;
    bool render_state_captured_ = false;
    bool detail_visible_ = false;
    bool shown_ = false;
    bool available_ = false;
};

} // namespace sdgo::friend_unit_gift::client::ui
