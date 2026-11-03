#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sdgo/game_test.h"
#include "sdgo/shop_labels.h"

// Ordinary, read-only Shop GameTest adapter.
//
// This adapter reaches the shop through the shipped lobby callback, selects
// the shipped AllSerchRadio and its FilterBtn, pages with the shipped room-manager callback and
// opens the shipped buy/gift option services. It invokes only each popup's
// BuyCancelBtn. It never invokes a popup BuyBtn, writes a wallet/inventory,
// constructs a reply, or treats callback return as completion.
class ShopCatalogActions
{
public:
    bool VisualOnly() const { return visualOnly_; }
    bool RefreshHover() const { return visualOnly_ && target_ && RoomStillMatches() && InvokeHover(bindings_.hover_by_row,room_,targetRow_); }
    enum Result : uint32_t
    {
        kUnbound = 0,
        kUnconfigured = 1,
        kWaitingForLobby = 2,
        kOpeningShop = 3,
        kWaitingForShop = 4,
        kSelectingAll = 5,
        kWaitingForCatalog = 6,
        kChangingPage = 7,
        kOpeningBuyPreview = 8,
        kWaitingForBuyPreview = 9,
        kCancellingBuyPreview = 10,
        kOpeningGiftPreview = 11,
        kWaitingForGiftPreview = 12,
        kCancellingGiftPreview = 13,
        kComplete = 14,
        kSubmittingSearch = 15,
        kWaitingForFullInventory = 16,
        kShowingHover = 17,

        kInvalidContext = 0x100,
        kRunChanged = 0x101,
        kThreadChanged = 0x102,
        kNativeFault = 0x103,
        kAppStateMismatch = 0x104,
        kServiceMismatch = 0x105,
        kControlMismatch = 0x106,
        kMalformedCatalog = 0x107,
        kCatalogTargetMissing = 0x108,
        kCatalogOptionMismatch = 0x109,
        kNativeNameMismatch = 0x10A,
        kUiNameMismatch = 0x10B,
        kUiDurationMismatch = 0x10C,
        kPopupMismatch = 0x10D,
        kPopupOptionMismatch = 0x10E,
        kWalletChanged = 0x10F,
        kInventoryChanged = 0x110,
        kInventoryUnavailable = 0x111,
        kUnsafeModalPresent = 0x112,
        kPageDidNotAdvance = 0x113,
        kConfigurationRejected = 0x114,
    };

    enum Stage : uint32_t
    {
        kStageIdle = 0,
        kStageBaseline,
        kStageEnterShop,
        kStageWaitShop,
        kStageSelectAll,
        kStageWaitAll,
        kStageScanPage,
        kStageWaitPage,
        kStageOpenBuy,
        kStageWaitBuy,
        kStageCancelBuy,
        kStageWaitBuyGone,
        kStageOpenGift,
        kStageWaitGift,
        kStageCancelGift,
        kStageWaitGiftGone,
        kStageDone,
        kStageSubmitSearch,
        kStageShowHover,
        kStageFinishProduct,
    };

    using GameAppFn = uint8_t*(__cdecl*)();
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using UiCallbackFn =
        uint32_t(__stdcall*)(void*, uint32_t, const void*, void*);
    using PopupCallbackFn =
        void(__stdcall*)(void*, uint32_t, const void*, void*);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn =
        uint8_t(__thiscall*)(
            void*,
            const uint8_t**,
            uint32_t,
            int,
            char);
    using UnitBySlotFn =
        void(__thiscall*)(void*, void*, uint16_t);
    using ButtonTextItemFn =
        void*(__thiscall*)(void*, uint32_t);
    struct alignas(4) NativeRequestName {std::array<uint8_t,0x1C> bytes{};};
    static_assert(sizeof(NativeRequestName)==0x1C,"native query string stack size changed");
    using RequestPendingFn=uint32_t(__thiscall*)(void*,NativeRequestName);
    using UnitPageRequestFn=void(__stdcall*)(uint32_t);
    using EditSetTextFn=void(__thiscall*)(void*,const void*,int);
    using EditGetTextFn=const void*(__thiscall*)(void*);
    using HoverByRowFn=void(__thiscall*)(void*,int);

    struct Bindings
    {
        GameAppFn game_app = nullptr;
        uint8_t** profile_pointer = nullptr;
        void** unit_info_pointer = nullptr;
        UnitBySlotFn unit_brief_by_slot = nullptr;
        UnitBySlotFn unit_full_by_slot = nullptr;
        void** service_managed_pointer = nullptr;
        ServiceFindByNameFn service_find_by_name = nullptr;
        NativeWStringCtorFn native_wstring_ctor = nullptr;
        NativeWStringDtorFn native_wstring_dtor = nullptr;
        RequestPendingFn request_pending=nullptr;
        UnitPageRequestFn request_unit_page=nullptr;
        EditSetTextFn edit_set_text=nullptr;
        EditGetTextFn edit_get_text=nullptr;
        HoverByRowFn hover_by_row=nullptr;
        NativeUiFindControlFn native_ui_find_control = nullptr;
        GridDataFn grid_data = nullptr;
        GridFindFn grid_find = nullptr;
        ButtonTextItemFn button_text_item = nullptr;
        void* downmenu_vtable = nullptr;
        UiCallbackFn downmenu_callback = nullptr;
        void* room_vtable = nullptr;
        UiCallbackFn room_callback = nullptr;
        void* search_vtable = nullptr;
        UiCallbackFn search_callback = nullptr;
        void* buy_vtable = nullptr;
        PopupCallbackFn buy_callback = nullptr;
        void* gift_vtable = nullptr;
        PopupCallbackFn gift_callback = nullptr;
        void* static_text_vtable = nullptr;
    };

    struct UnitImage
    {
        std::array<uint8_t, 0x19> brief = {};
        std::array<uint8_t, 0x99> full = {};

        bool operator==(const UnitImage& other) const
        {
            return brief == other.brief && full == other.full;
        }
    };

    struct IntegrityImage
    {
        uint32_t gp = 0;
        uint32_t cash = 0;
        uint16_t active_slot = 0;
        int32_t capacity = 0;
        std::vector<UnitImage> units;

        bool operator==(const IntegrityImage& other) const
        {
            return gp == other.gp &&
                   cash == other.cash &&
                   active_slot == other.active_slot &&
                   capacity == other.capacity &&
                   units == other.units;
        }
    };

    struct Observation
    {
        uint32_t result = kUnbound;
        uint32_t stage = kStageIdle;
        uint32_t configured = 0;
        uint32_t verified = 0;
        uint32_t current_page = 0;
        uint32_t max_page = 0;
        uint32_t product_id = 0;
        uint32_t option_id = 0;
        uint32_t category = 0;
        int32_t validity = 0;
        int32_t price = 0;
        uint32_t payment = 0;
        int32_t time_limit = -1;
        uint32_t buy_previews = 0;
        uint32_t gift_previews = 0;
        uint32_t hover_previews = 0;
        uint32_t popup_name_found=0;
        std::wstring popup_name;
        uint32_t cancellations = 0;
        uint32_t original_actions = 0;
        uint32_t popup_count = 0;
        uint32_t stock_assertions = 0;
        uint32_t duplicate_native_title_rows = 0;
        uint32_t native_page_slots = 0;
        uint32_t native_page_nonempty = 0;
        uint32_t native_page_bytes = 0;
        uint32_t native_page_options = 0;
        uint32_t native_request_pending = 0;
        uint32_t wallet_unchanged = 0;
        uint32_t inventory_unchanged = 0;
        uint32_t inventory_failure = 0;
        uint32_t inventory_probe_slot = 0;
        uint32_t inventory_ready = 0;
        int32_t inventory_capacity = 0;
        uint32_t inventory_brief_unit = 0;
        uint32_t inventory_full_unit = 0;
        uint32_t inventory_brief_slot = 0;
        uint32_t inventory_full_slot = 0;
        uint64_t inventory_brief_uid = 0;
        uint64_t inventory_full_uid = 0;
        uint32_t process_id = 0;
        uint32_t ui_thread_id = 0;
        std::vector<uint32_t> duplicate_native_product_ids;
        std::wstring native_name;
        std::wstring visible_name;
        std::wstring option_label;
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod)
    {
        if (!api || !api->Symbol)
            return false;
        Bindings bindings = {};
        bindings.game_app =
            Symbol<GameAppFn>(
                api, mod, "game.app");
        bindings.profile_pointer =
            Symbol<uint8_t**>(
                api, mod, "flow.profile_pointer");
        bindings.unit_info_pointer =
            Symbol<void**>(
                api, mod, "flow.unit_info_pointer");
        bindings.unit_brief_by_slot =
            Symbol<UnitBySlotFn>(
                api, mod, "flow.unit_brief_by_slot");
        bindings.unit_full_by_slot =
            Symbol<UnitBySlotFn>(
                api, mod, "flow.unit_full_by_slot");
        bindings.service_managed_pointer =
            Symbol<void**>(
                api, mod, "flow.service_managed_pointer");
        bindings.service_find_by_name =
            Symbol<ServiceFindByNameFn>(
                api, mod, "flow.service_find_by_name");
        bindings.native_wstring_ctor =
            Symbol<NativeWStringCtorFn>(
                api, mod, "flow.native_wstring_ctor");
        bindings.native_wstring_dtor =
            Symbol<NativeWStringDtorFn>(
                api, mod, "flow.native_wstring_dtor");
        bindings.request_pending=Symbol<RequestPendingFn>(api,mod,"flow.native_request_pending");
        bindings.request_unit_page=Symbol<UnitPageRequestFn>(api,mod,"flow.unit_page_request");
        bindings.edit_set_text=Symbol<EditSetTextFn>(api,mod,"room_titles.ui_edit_set_text");
        bindings.edit_get_text=Symbol<EditGetTextFn>(api,mod,"room_titles.ui_edit_get_text");
        bindings.hover_by_row=Symbol<HoverByRowFn>(api,mod,"flow.shop_hover_by_row");
        bindings.native_ui_find_control =
            Symbol<NativeUiFindControlFn>(
                api, mod, "flow.native_ui_find_control");
        bindings.grid_data =
            Symbol<GridDataFn>(
                api, mod, "grid.data");
        bindings.grid_find =
            Symbol<GridFindFn>(
                api, mod, "grid.find");
        bindings.button_text_item =
            Symbol<ButtonTextItemFn>(
                api, mod, "room_titles.ui_button_text_item");
        bindings.downmenu_vtable =
            api->Symbol(mod, "flow.hangar_downmenu_vtable");
        bindings.downmenu_callback =
            Symbol<UiCallbackFn>(
                api, mod, "flow.hangar_downmenu_callback");
        bindings.room_vtable =
            api->Symbol(mod, "flow.shop_room_manager_vtable");
        bindings.room_callback =
            Symbol<UiCallbackFn>(
                api, mod, "flow.shop_room_manager_callback");
        bindings.search_vtable=api->Symbol(mod,"flow.shop_search_vtable");
        bindings.search_callback=Symbol<UiCallbackFn>(api,mod,"flow.shop_search_callback");
        bindings.buy_vtable =
            api->Symbol(mod, "flow.shop_buy_options_vtable");
        bindings.buy_callback =
            Symbol<PopupCallbackFn>(
                api, mod, "flow.shop_buy_options_callback");
        bindings.gift_vtable =
            api->Symbol(mod, "flow.shop_gift_options_vtable");
        bindings.gift_callback =
            Symbol<PopupCallbackFn>(
                api, mod, "flow.shop_gift_options_callback");
        bindings.static_text_vtable =
            api->Symbol(
                mod, "room_titles.ui_static_text_vtable");
        return Bind(bindings);
    }

    bool Bind(const Bindings& bindings)
    {
        bindings_ = bindings;
        bound_ =
            bindings_.game_app &&
            bindings_.profile_pointer &&
            bindings_.unit_info_pointer &&
            bindings_.unit_brief_by_slot &&
            bindings_.unit_full_by_slot &&
            bindings_.service_managed_pointer &&
            bindings_.service_find_by_name &&
            bindings_.native_wstring_ctor &&
            bindings_.native_wstring_dtor &&
            bindings_.request_pending &&
            bindings_.request_unit_page &&
            bindings_.native_ui_find_control &&
            bindings_.grid_data &&
            bindings_.grid_find &&
            bindings_.button_text_item &&
            bindings_.downmenu_vtable &&
            bindings_.downmenu_callback &&
            bindings_.room_vtable &&
            bindings_.room_callback &&
            bindings_.search_vtable && bindings_.search_callback &&
            bindings_.buy_vtable &&
            bindings_.buy_callback &&
            bindings_.gift_vtable &&
            bindings_.gift_callback &&
            bindings_.static_text_vtable &&
            CallbackMatches(
                bindings_.downmenu_vtable,
                bindings_.downmenu_callback) &&
            CallbackMatches(
                bindings_.room_vtable,
                bindings_.room_callback) &&
            CallbackMatches(bindings_.search_vtable,bindings_.search_callback) &&
            CallbackMatches(
                bindings_.buy_vtable,
                bindings_.buy_callback) &&
            CallbackMatches(
                bindings_.gift_vtable,
                bindings_.gift_callback);
        ResetRun();
        return bound_;
    }

    bool Configure(
        const std::wstring& catalogAssertions,
        const std::wstring& runtimeDisplay)
    {
        ResetRun();
        products_.clear();
        runtime_.clear();
        stockAssertions_ = false;
        configured_ = false;
        try {
            const Ini assertions =
                LoadIni(catalogAssertions);
            const Ini display = LoadIni(runtimeDisplay);
            ParseCatalogAssertions(assertions);
            ParseRuntime(display);
            CrossCheckConfiguration();
            configured_ = true;
            observation_.configured =
                static_cast<uint32_t>(products_.size());
            observation_.stock_assertions =
                stockAssertions_ ? 1u : 0u;
            Publish(kUnbound);
            return true;
        } catch (...) {
            products_.clear();
            runtime_.clear();
            stockAssertions_ = false;
            configured_ = false;
            Publish(kConfigurationRejected);
            return false;
        }
    }

    Result Poll(const SdgoGameTestContextV1* context)
    {
        const Result identity = CheckContext(context);
        if (identity != kComplete)
            return Publish(identity);
        if (!bound_)
            return Publish(kUnbound);
        if (!configured_)
            return Publish(kUnconfigured);

        if (hasBaseline_) {
            const Result unchanged = CheckIntegrity();
            if (unchanged != kComplete)
                return Publish(unchanged);
        }

        switch (stage_) {
        case kStageIdle:
        case kStageBaseline:
            return PollBaseline();
        case kStageEnterShop:
            return EnterShop();
        case kStageWaitShop:
            return WaitForShop();
        case kStageSelectAll:
            return SelectAll();
        case kStageWaitAll:
            return WaitForAll(context);
        case kStageSubmitSearch:
            return SubmitSearch();
        case kStageScanPage:
            return ScanPage(context);
        case kStageWaitPage:
            return WaitForPage();
        case kStageOpenBuy:
            return OpenPreview(false);
        case kStageWaitBuy:
            return WaitForPreview(false);
        case kStageCancelBuy:
            return CancelPreview(false);
        case kStageWaitBuyGone:
            return WaitForPreviewGone(false);
        case kStageOpenGift:
            return OpenPreview(true);
        case kStageWaitGift:
            return WaitForPreview(true);
        case kStageCancelGift:
            return CancelPreview(true);
        case kStageWaitGiftGone:
            return WaitForPreviewGone(true);
        case kStageDone:
            return Publish(kComplete);
        case kStageShowHover:
            if(!RefreshHover())return Publish(kNativeFault);
            ++observation_.hover_previews;stage_=kStageFinishProduct;
            return Publish(kShowingHover);
        case kStageFinishProduct:
            return FinishProduct();
        default:
            return Publish(kNativeFault);
        }
    }

    void ResetRun()
    {
        stage_ = kStageIdle;
        runHash_ = 0;
        uiThread_ = 0;
        processId_ = 0;
        lastElapsed_ = 0;
        hasBaseline_ = false;
        baseline_ = {};
        room_ = nullptr;
        roomRoot_ = nullptr;
        popup_ = nullptr;
        target_ = nullptr;
        targetRow_ = 0;
        targetOptionIndex_ = 0;
        pageBefore_ = 0;
        allSelectedAt_ = 0;
        observedCatalogChange_ = false;
        initialCatalogSignature_ = 0;
        verifiedKeys_.clear();
        requestedInventoryPages_.clear();
        nativeNames_.clear();
        observation_ = {};
        observation_.result =
            bound_ ? kUnconfigured : kUnbound;
        observation_.stage = kStageIdle;
        observation_.configured =
            static_cast<uint32_t>(products_.size());
    }

    Observation State() const
    {
        return observation_;
    }

    const IntegrityImage& Baseline() const
    {
        return baseline_;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kUnconfigured: return "unconfigured";
        case kWaitingForLobby: return "waiting-for-lobby";
        case kOpeningShop: return "opening-shop";
        case kWaitingForShop: return "waiting-for-shop";
        case kSelectingAll: return "selecting-all";
        case kWaitingForCatalog: return "waiting-for-catalog";
        case kChangingPage: return "changing-page";
        case kOpeningBuyPreview: return "opening-buy-preview";
        case kWaitingForBuyPreview: return "waiting-for-buy-preview";
        case kCancellingBuyPreview: return "cancelling-buy-preview";
        case kOpeningGiftPreview: return "opening-gift-preview";
        case kWaitingForGiftPreview: return "waiting-for-gift-preview";
        case kCancellingGiftPreview: return "cancelling-gift-preview";
        case kComplete: return "complete";
        case kSubmittingSearch: return "submitting-search";
        case kWaitingForFullInventory: return "waiting-for-full-inventory";
        case kInvalidContext: return "invalid-context";
        case kRunChanged: return "run-changed";
        case kThreadChanged: return "thread-changed";
        case kNativeFault: return "native-fault";
        case kAppStateMismatch: return "app-state-mismatch";
        case kServiceMismatch: return "service-mismatch";
        case kControlMismatch: return "control-mismatch";
        case kMalformedCatalog: return "malformed-catalog";
        case kCatalogTargetMissing: return "catalog-target-missing";
        case kCatalogOptionMismatch: return "catalog-option-mismatch";
        case kNativeNameMismatch: return "native-name-mismatch";
        case kUiNameMismatch: return "ui-name-mismatch";
        case kUiDurationMismatch: return "ui-duration-mismatch";
        case kPopupMismatch: return "popup-mismatch";
        case kPopupOptionMismatch: return "popup-option-mismatch";
        case kWalletChanged: return "wallet-changed";
        case kInventoryChanged: return "inventory-changed";
        case kInventoryUnavailable: return "inventory-unavailable";
        case kUnsafeModalPresent: return "unsafe-modal-present";
        case kPageDidNotAdvance: return "page-did-not-advance";
        case kConfigurationRejected: return "configuration-rejected";
        default: return "unknown";
        }
    }

private:
    static constexpr uint32_t kLobbyState = 6;
    static constexpr uint32_t kHangarState = 5;
    static constexpr uint32_t kShopState = 9;
    static constexpr uint32_t kClickEvent = 0x101;
    static constexpr uint32_t kRadioEvent = 0x301;
    static constexpr uint32_t kButtonKind = 4;
    static constexpr uint32_t kStaticKind = 3;
    static constexpr uint32_t kRadioKind = 0x0C;
    static constexpr size_t kCallbackSlot = 0x38 / sizeof(void*);
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kControlText = 0x21C;
    static constexpr size_t kStringLength = 0x10;
    static constexpr size_t kStringCapacity = 0x14;
    static constexpr size_t kDownmenuRoot = 0x44;
    static constexpr size_t kDownmenuShopRadio = 0x50;
    static constexpr size_t kDownmenuMode = 0x60;
    static constexpr size_t kRoomRoot = 0x64;
    static constexpr size_t kRoomPage = 0x68;
    static constexpr size_t kRoomMaxPage = 0x6C;
    static constexpr size_t kRoomProductsBegin = 0x78;
    static constexpr size_t kRoomProductsEnd = 0x7C;
    static constexpr size_t kRoomOptionsBegin = 0x88;
    static constexpr size_t kRoomOptionsEnd = 0x8C;
    static constexpr size_t kRoomAllSearch = 0x2F4;
    static constexpr size_t kProductStride = 0x11;
    static constexpr size_t kPopupRoot = 0x4C;
    static constexpr size_t kBuyCombo = 0x50;
    static constexpr size_t kBuyCancel = 0x60;
    static constexpr size_t kBuyOptionsBegin = 0xB0;
    static constexpr size_t kBuyOptionsEnd = 0xB4;
    static constexpr size_t kGiftCancel = 0xA8;
    static constexpr size_t kGiftCombo = 0xAC;
    static constexpr size_t kGiftOptionsBegin = 0xB4;
    static constexpr size_t kGiftOptionsEnd = 0xB8;
    static constexpr size_t kComboItemsBegin = 0x234;
    static constexpr size_t kComboItemsEnd = 0x238;
    static constexpr size_t kManagerPopupSentinel = 0x54;
    static constexpr size_t kManagerPopupCount = 0x58;
    static constexpr size_t kProfileGp = 0x18F;
    static constexpr size_t kProfileActiveSlot = 0x1AD;
    static constexpr size_t kProfileCash = 0x58C;
    static constexpr size_t kProfileStarterBusy = 0x62E;
    static constexpr size_t kUnitInfoCapacity = 0x0C;
    static constexpr size_t kUnitInfoReady = 0x20;
    static constexpr uint32_t kMaximumInventorySlots = 4096;
    static constexpr uint32_t kMaximumPages = 4096;
    static constexpr uint32_t kMaximumPopupCount = 16;
    static constexpr uint64_t kCatalogWaitMilliseconds = 15000;

    struct Product
    {
        std::wstring key;
        uint32_t productId = 0;
        uint32_t itemId = 0;
        uint32_t templateId = 0;
        uint32_t category = 0;
        int32_t price = 0;
        int32_t validity = 0;
        uint8_t payment = 0;
        std::wstring name;
        std::wstring displayName;
        std::wstring query;
        std::wstring label;
    };

    struct RuntimeOption
    {
        uint32_t id = 0;
        uint32_t category = 0;
        int32_t validity = 0;
        int32_t payment = 0;
        int32_t price = 0;
        std::wstring text;
    };

    struct Ini
    {
        using Keys = std::map<std::wstring, std::wstring>;
        std::map<std::wstring, Keys> sections;
    };

    struct ProductRow
    {
        uint32_t productId = 0;
        uint32_t row = 0;
    };

    struct PopupList
    {
        uint32_t count = 0;
        std::array<void*, kMaximumPopupCount> services = {};
    };

    template <typename T>
    static T Symbol(
        const SdgoApi* api,
        SdgoMod* mod,
        const char* name)
    {
        return api && api->Symbol
            ? reinterpret_cast<T>(
                  api->Symbol(mod, name))
            : nullptr;
    }

    template <typename Callback>
    static bool CallbackMatches(
        void* vtable,
        Callback callback)
    {
        if (!vtable || !callback)
            return false;
        __try {
            return static_cast<void**>(vtable)
                       [kCallbackSlot] ==
                   reinterpret_cast<void*>(callback);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static std::wstring Trim(const std::wstring& value)
    {
        size_t first = 0;
        while (first < value.size() &&
               (value[first] == L' ' ||
                value[first] == L'\t'))
            ++first;
        size_t last = value.size();
        while (last > first &&
               (value[last - 1] == L' ' ||
                value[last - 1] == L'\t'))
            --last;
        return value.substr(first, last - first);
    }

    static std::wstring ReadTextFile(
        const std::wstring& path)
    {
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw 1;
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size) ||
            size.QuadPart < 0 ||
            size.QuadPart > 4 * 1024 * 1024) {
            CloseHandle(file);
            throw 1;
        }
        std::vector<uint8_t> bytes(
            static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        const bool ok =
            bytes.empty() ||
            (ReadFile(
                 file,
                 bytes.data(),
                 static_cast<DWORD>(bytes.size()),
                 &read,
                 nullptr) &&
             read == bytes.size());
        CloseHandle(file);
        if (!ok)
            throw 1;

        if (bytes.size() >= 2 &&
            bytes[0] == 0xFF &&
            bytes[1] == 0xFE) {
            if ((bytes.size() - 2) % 2 != 0)
                throw 1;
            std::wstring result(
                (bytes.size() - 2) / 2,
                L'\0');
            if (!result.empty())
                memcpy(
                    &result[0],
                    bytes.data() + 2,
                    result.size() * sizeof(wchar_t));
            if (result.find(L'\0') != std::wstring::npos)
                throw 1;
            return result;
        }

        size_t begin = 0;
        if (bytes.size() >= 3 &&
            bytes[0] == 0xEF &&
            bytes[1] == 0xBB &&
            bytes[2] == 0xBF)
            begin = 3;
        const int sourceSize =
            static_cast<int>(bytes.size() - begin);
        const int length = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(
                bytes.data() + begin),
            sourceSize,
            nullptr,
            0);
        if (sourceSize && length <= 0)
            throw 1;
        std::wstring result(
            static_cast<size_t>(length),
            L'\0');
        if (length &&
            MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(
                    bytes.data() + begin),
                sourceSize,
                &result[0],
                length) != length)
            throw 1;
        if (result.find(L'\0') != std::wstring::npos)
            throw 1;
        return result;
    }

    static Ini LoadIni(const std::wstring& path)
    {
        const std::wstring text = ReadTextFile(path);
        Ini result;
        std::wstring current;
        size_t begin = 0;
        while (begin <= text.size()) {
            size_t end = text.find(L'\n', begin);
            if (end == std::wstring::npos)
                end = text.size();
            std::wstring line =
                text.substr(begin, end - begin);
            if (!line.empty() &&
                line.back() == L'\r')
                line.pop_back();
            line = Trim(line);
            begin =
                end == text.size()
                    ? text.size() + 1
                    : end + 1;
            if (line.empty() ||
                line[0] == L';' ||
                line[0] == L'#')
                continue;
            if (line.front() == L'[') {
                if (line.size() < 3 ||
                    line.back() != L']')
                    throw 1;
                current = Trim(
                    line.substr(1, line.size() - 2));
                if (current.empty() ||
                    result.sections.count(current))
                    throw 1;
                result.sections.emplace(
                    current, Ini::Keys{});
                continue;
            }
            if (current.empty())
                throw 1;
            const size_t equals = line.find(L'=');
            if (equals == std::wstring::npos)
                throw 1;
            const std::wstring key =
                Trim(line.substr(0, equals));
            const std::wstring value =
                Trim(line.substr(equals + 1));
            if (key.empty() ||
                result.sections[current].count(key))
                throw 1;
            result.sections[current].emplace(
                key, value);
        }
        return result;
    }

    static const Ini::Keys& Section(
        const Ini& ini,
        const std::wstring& name)
    {
        const auto found = ini.sections.find(name);
        if (found == ini.sections.end())
            throw 1;
        return found->second;
    }

    static std::wstring Value(
        const Ini::Keys& keys,
        const std::wstring& name,
        const wchar_t* fallback = nullptr)
    {
        const auto found = keys.find(name);
        if (found != keys.end())
            return found->second;
        if (fallback)
            return fallback;
        throw 1;
    }

    static int64_t Integer(
        const Ini::Keys& keys,
        const std::wstring& name,
        int64_t minimum,
        int64_t maximum,
        const wchar_t* fallback = nullptr)
    {
        const std::wstring text =
            Value(keys, name, fallback);
        if (text.empty())
            throw 1;
        wchar_t* end = nullptr;
        errno = 0;
        const long long number =
            _wcstoi64(text.c_str(), &end, 10);
        if (errno == ERANGE ||
            !end ||
            end == text.c_str() ||
            *end ||
            number < minimum ||
            number > maximum)
            throw 1;
        return number;
    }

    static bool Key(const std::wstring& value)
    {
        if (value.empty() || value.size() > 64)
            return false;
        for (wchar_t character : value) {
            if (!((character >= L'a' &&
                   character <= L'z') ||
                  (character >= L'A' &&
                   character <= L'Z') ||
                  (character >= L'0' &&
                   character <= L'9') ||
                  character == L'-' ||
                  character == L'_' ||
                  character == L'.'))
                return false;
        }
        return true;
    }

    static bool Text(const std::wstring& value)
    {
        return !value.empty() &&
               value.size() <= 50 &&
               value.find_first_of(L"\r\n") ==
                   std::wstring::npos;
    }

    void ParseCatalogAssertions(const Ini& ini)
    {
        const Ini::Keys& shop =
            Section(ini, L"shop");
        if (Integer(shop, L"Schema", 1, 1) != 1)
            throw 1;
        const std::wstring scope =
            Value(
                shop,
                L"Scope",
                L"OwnerCatalog");
        visualOnly_ = scope == L"StockVisualAssertions";
        if(visualOnly_ && !bindings_.hover_by_row)throw 1;
        query_ = visualOnly_ ? Value(shop,L"Query",L"") : L"";
        if(query_.size()>36 || (!query_.empty() && (!bindings_.edit_set_text || !bindings_.edit_get_text)))throw 1;
        if (scope == L"StockAssertions" || visualOnly_)
            stockAssertions_ = true;
        else if (scope == L"OwnerCatalog")
            stockAssertions_ = false;
        else
            throw 1;
        catalogId_ = Value(shop, L"CatalogId");
        revision_ =
            Value(shop, L"Revision", L"manual");
        if (!Key(catalogId_) || !Key(revision_))
            throw 1;

        std::set<std::wstring> productKeys;
        std::set<uint32_t> productIds;
        std::set<uint32_t> itemIds;
        for (const auto& section : ini.sections) {
            if (section.first.compare(
                    0, 8, L"product:") != 0)
                continue;
            if (products_.size() >= 256)
                throw 1;
            Product product = {};
            product.key = section.first.substr(8);
            product.productId =
                static_cast<uint32_t>(
                    Integer(
                        section.second,
                        L"ProductId",
                        1,
                        INT32_MAX));
            const std::wstring item =
                Value(section.second, L"ItemId");
            if (item.size() != 6 ||
                !std::all_of(
                    item.begin(),
                    item.end(),
                    [](wchar_t character) {
                        return character >= L'0' &&
                               character <= L'9';
                    }))
                throw 1;
            product.itemId =
                static_cast<uint32_t>(
                    wcstoul(
                        item.c_str(),
                        nullptr,
                        10));
            product.templateId =
                static_cast<uint32_t>(
                    Integer(
                        section.second,
                        L"TemplateId",
                        1,
                        INT32_MAX));
            product.category =
                static_cast<uint32_t>(
                    Integer(
                        section.second,
                        L"Category",
                        1,
                        INT32_MAX));
            product.price =
                static_cast<int32_t>(
                    Integer(
                        section.second,
                        L"Price",
                        1,
                        INT32_MAX));
            product.validity =
                static_cast<int32_t>(
                    Integer(
                        section.second,
                        L"Validity",
                        1,
                        1000000));
            const std::wstring currency =
                Value(section.second, L"Currency");
            if (currency == L"GB")
                product.payment = 1;
            else if (currency == L"MB")
                product.payment = 2;
            else
                throw 1;
            product.name =
                Value(section.second, L"Name");
            product.displayName = visualOnly_ ? Value(section.second, L"DisplayName") : product.name;
            product.query=visualOnly_ ? Value(section.second,L"Query",query_.c_str()) : L"";
            if(product.query.size()>36 || (!product.query.empty() && (!bindings_.edit_set_text || !bindings_.edit_get_text)))throw 1;
            product.label =
                Value(section.second, L"Label");
            const bool enabled =
                Integer(
                    section.second,
                    L"Enabled",
                    0,
                    1,
                    L"1") != 0;
            if (!Key(product.key) ||
                !Text(product.name) ||
                !Text(product.displayName) ||
                !Text(product.label) ||
                (stockAssertions_
                     ? product.productId !=
                           product.templateId
                     : product.productId ==
                           product.templateId) ||
                !productKeys.insert(
                     product.key).second ||
                !productIds.insert(
                     product.productId).second ||
                !itemIds.insert(
                     product.itemId).second)
                throw 1;
            if (enabled)
                products_.push_back(
                    std::move(product));
        }
        if (products_.empty())
            throw 1;
    }

    void ParseRuntime(const Ini& ini)
    {
        const Ini::Keys& shop =
            Section(ini, L"shop");
        if (Value(shop, L"Revision") != revision_)
            throw 1;
        for (const auto& section : ini.sections) {
            if (section.first.compare(
                    0, 7, L"option:") != 0)
                continue;
            const std::wstring idText =
                section.first.substr(7);
            wchar_t* end = nullptr;
            const unsigned long id =
                wcstoul(
                    idText.c_str(),
                    &end,
                    10);
            if (!id ||
                id > INT32_MAX ||
                !end ||
                *end ||
                runtime_.count(
                    static_cast<uint32_t>(id)))
                throw 1;
            RuntimeOption option = {};
            option.id = static_cast<uint32_t>(id);
            option.category =
                static_cast<uint32_t>(
                    Integer(
                        section.second,
                        L"Category",
                        1,
                        INT32_MAX));
            option.validity =
                static_cast<int32_t>(
                    Integer(
                        section.second,
                        L"Validity",
                        1,
                        1000000));
            option.payment =
                static_cast<int32_t>(
                    Integer(
                        section.second,
                        L"Payment",
                        1,
                        2));
            option.price =
                static_cast<int32_t>(
                    Integer(
                        section.second,
                        L"Price",
                        1,
                        INT32_MAX));
            option.text =
                Value(section.second, L"Text");
            if (!Text(option.text))
                throw 1;
            runtime_.emplace(
                option.id, std::move(option));
        }
        if (runtime_.empty())
            throw 1;
    }

    void CrossCheckConfiguration() const
    {
        for (const Product& product : products_) {
            size_t matches = 0;
            for (const auto& value : runtime_) {
                const RuntimeOption& option =
                    value.second;
                if (option.category ==
                        product.category &&
                    option.validity ==
                        product.validity &&
                    option.payment ==
                        product.payment &&
                    option.price ==
                        product.price &&
                    option.text ==
                        product.label)
                    ++matches;
            }
            if (matches != 1)
                throw 1;
        }
    }

    static uint64_t HashContext(
        const SdgoGameTestContextV1* context)
    {
        if (!context ||
            context->size < sizeof(*context) ||
            context->version !=
                SDGO_GAME_TEST_VERSION ||
            !context->run_id ||
            !context->role ||
            strcmp(context->role, "client") != 0)
            return 0;
        uint64_t value = 1469598103934665603ULL;
        const char* fields[] = {
            context->run_id,
            context->role,
        };
        for (const char* field : fields) {
            size_t count = 0;
            while (field[count] &&
                   count < 256) {
                value ^=
                    static_cast<uint8_t>(
                        field[count++]);
                value *= 1099511628211ULL;
            }
            if (!count || count == 256)
                return 0;
            value ^= 0xFF;
            value *= 1099511628211ULL;
        }
        return value;
    }

    Result CheckContext(
        const SdgoGameTestContextV1* context)
    {
        const uint64_t hash = HashContext(context);
        if (!hash)
            return kInvalidContext;
        const DWORD thread = GetCurrentThreadId();
        if (!runHash_) {
            runHash_ = hash;
            uiThread_ = thread;
            processId_ = GetCurrentProcessId();
            lastElapsed_ = context->elapsed_ms;
            observation_.ui_thread_id = uiThread_;
            observation_.process_id = processId_;
            return kComplete;
        }
        if (hash != runHash_ ||
            context->elapsed_ms < lastElapsed_)
            return kRunChanged;
        if (thread != uiThread_)
            return kThreadChanged;
        lastElapsed_ = context->elapsed_ms;
        return kComplete;
    }

    Result PollBaseline()
    {
        stage_ = kStageBaseline;
        uint32_t state = 0;
        if (!ReadAppState(&state))
            return kNativeFault;
        if (state != kLobbyState && state != kHangarState)
            return Publish(kWaitingForLobby);
        IntegrityImage image;
        const Result captured =
            CaptureIntegrity(&image);
        if(visualOnly_ && captured==kInventoryUnavailable &&
           (observation_.inventory_failure<=3 || observation_.inventory_failure==7))
            return Publish(kWaitingForFullInventory);
        if(captured==kInventoryUnavailable && observation_.inventory_failure==6 &&
           observation_.inventory_brief_uid && !observation_.inventory_full_unit &&
           !observation_.inventory_full_uid &&
           observation_.inventory_brief_slot==observation_.inventory_probe_slot &&
           observation_.inventory_full_slot==observation_.inventory_probe_slot) {
            // The original unitmgr service in the real 3D hangar consumes the
            // page reply. Sending from the lobby alone cannot load that cache.
            if(state!=kHangarState)return Publish(kWaitingForFullInventory);
            const uint32_t page=(observation_.inventory_probe_slot-1)/6;
            if(!requestedInventoryPages_.count(page)) {
                const auto ready=CatalogRequestReady(L"REQ_UNIT_SLOT_PAGE");
                if(ready!=kComplete)return Publish(ready);
                // Shipped0096EBF0 is a static one-stack-argument RET4 function.
                // It requests genuinely unloaded six-slot pages, then the
                // original reply consumer fills the cache. No record writes.
                if(!RequestUnitPage(bindings_.request_unit_page,page))return Publish(kNativeFault);
                requestedInventoryPages_.insert(page);
            }
            return Publish(kWaitingForFullInventory);
        }
        if (captured != kComplete)
            return Publish(captured);
        baseline_ = std::move(image);
        hasBaseline_ = true;
        observation_.wallet_unchanged = 1;
        observation_.inventory_unchanged = 1;
        stage_ = kStageEnterShop;
        return Publish(kWaitingForLobby);
    }

    Result EnterShop()
    {
        uint32_t state = 0;
        if (!ReadAppState(&state))
            return kNativeFault;
        if (state == kShopState) {
            stage_ = kStageWaitShop;
            return Publish(kWaitingForShop);
        }
        if (state != kLobbyState && state != kHangarState)
            return Publish(kAppStateMismatch);

        PopupList popups = {};
        const Result popupResult =
            ReadPopupList(&popups);
        if (popupResult != kComplete)
            return Publish(popupResult);
        if (popups.count)
            return Publish(kUnsafeModalPresent);

        void* service = nullptr;
        const ServiceResult resolved =
            ResolveService(
                L"downmenu",
                bindings_.downmenu_vtable,
                &service);
        if (resolved == kServiceAbsent)
            return Publish(kWaitingForLobby);
        if (resolved != kServiceMatched)
            return Publish(kServiceMismatch);

        void* root = nullptr;
        void* cached = nullptr;
        uint32_t mode = 0;
        uint8_t busy = 0;
        if (!ReadPointer(
                service,
                kDownmenuRoot,
                &root) ||
            !ReadPointer(
                service,
                kDownmenuShopRadio,
                &cached) ||
            !ReadValue(
                service,
                kDownmenuMode,
                &mode) ||
            !ReadProfileByte(
                kProfileStarterBusy,
                &busy))
            return Publish(kNativeFault);
        if (!root || !cached)
            return Publish(kWaitingForLobby);
        if (mode == 8 && busy)
            return Publish(kWaitingForLobby);
        void* found =
            FindControl(
                root,
                L"ShopRoomRadio",
                kRadioKind);
        if (found != cached ||
            !ControlIdentity(
                found,
                root,
                L"ShopRoomRadio"))
            return Publish(kControlMismatch);
        if (!Invoke(
                bindings_.downmenu_callback,
                service,
                kRadioEvent,
                found))
            return Publish(kNativeFault);
        ++observation_.original_actions;
        stage_ = kStageWaitShop;
        return Publish(kOpeningShop);
    }

    Result WaitForShop()
    {
        uint32_t state = 0;
        if (!ReadAppState(&state))
            return Publish(kNativeFault);
        if (state != kShopState)
            return Publish(kWaitingForShop);
        void* service = nullptr;
        const ServiceResult resolved =
            ResolveService(
                L"roommgr",
                bindings_.room_vtable,
                &service);
        if (resolved == kServiceAbsent)
            return Publish(kWaitingForShop);
        if (resolved != kServiceMatched)
            return Publish(kServiceMismatch);
        void* root = nullptr;
        if (!ReadPointer(
                service,
                kRoomRoot,
                &root))
            return Publish(kNativeFault);
        if (!root)
            return Publish(kWaitingForShop);
        room_ = service;
        roomRoot_ = root;
        stage_ = kStageSelectAll;
        return Publish(kWaitingForShop);
    }

    Result CatalogRequestReady(const wchar_t* requestName=L"REQ_GET_STORE_ITEM_LIST")
    {
        void* manager=nullptr;
        if(!ReadIndirect(reinterpret_cast<uint8_t**>(bindings_.service_managed_pointer),
                         reinterpret_cast<uint8_t**>(&manager)) || !manager)return kNativeFault;
        NativeRequestName name;
        __try {
            if(!bindings_.native_wstring_ctor(name.bytes.data(),requestName))return kNativeFault;
            // Native6FB930 takes and destroys this 28-byte string by value.
            // The local POD is not a second owning std::wstring.
            const auto pending=bindings_.request_pending(manager,name);
            observation_.native_request_pending=pending;
            return pending ? kWaitingForCatalog : kComplete;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return kNativeFault;}
    }

    Result SelectAll()
    {
        if (!RoomStillMatches())
            return Publish(kServiceMismatch);
        const auto request=CatalogRequestReady();
        if(request!=kComplete)return Publish(request);
        PopupList popups = {};
        const Result popupResult =
            ReadPopupList(&popups);
        if (popupResult != kComplete)
            return Publish(popupResult);
        if (popups.count)
            return Publish(kUnsafeModalPresent);
        if(visualOnly_) {
            const auto next=std::find_if(products_.begin(),products_.end(),[this](const Product& p){return !verifiedKeys_.count(p.key);});
            if(next!=products_.end())query_=next->query;
        }

        void* cached = nullptr;
        if (!ReadPointer(
                room_,
                kRoomAllSearch,
                &cached))
            return Publish(kNativeFault);
        void* found =
            FindControl(
                roomRoot_,
                L"AllSerchRadio",
                kRadioKind);
        if (!cached ||
            found != cached ||
            !ControlIdentity(
                found,
                roomRoot_,
                L"AllSerchRadio"))
            return Publish(kControlMismatch);
        initialCatalogSignature_ =
            CatalogSignature();
        if (!Invoke(
                bindings_.room_callback,
                room_,
                kRadioEvent,
                found))
            return Publish(kNativeFault);
        ++observation_.original_actions;
        allSelectedAt_ = lastElapsed_;
        // Native 009E6350 opens an empty search view. It does not send a
        // stock query; 009E02D0/009CD9E0 handles the real FilterBtn click.
        stage_ = kStageSubmitSearch;
        return Publish(kSelectingAll);
    }

    Result SubmitSearch()
    {
        if(!RoomStillMatches())return Publish(kServiceMismatch);
        const auto request=CatalogRequestReady();
        if(request!=kComplete)return Publish(request);
        PopupList popups={};
        const auto popupResult=ReadPopupList(&popups);
        if(popupResult!=kComplete)return Publish(popupResult);
        if(popups.count)return Publish(kUnsafeModalPresent);
        void* service=nullptr;
        const auto resolved=ResolveService(L"subTabDlg",bindings_.search_vtable,&service);
        if(resolved==kServiceAbsent)return Publish(kWaitingForCatalog);
        if(resolved!=kServiceMatched)return Publish(kServiceMismatch);
        uint32_t active=0;void* root=nullptr;void* cached=nullptr;
        if(!ReadValue(service,0xD0,&active) || !ReadPointer(service,0x58,&root) ||
           !ReadPointer(service,0xB8,&cached))return Publish(kNativeFault);
        if(active!=1 || !root || !cached)return Publish(kWaitingForCatalog);
        void* found=FindControl(root,L"FilterBtn",kButtonKind);
        if(found!=cached || !ControlIdentity(found,root,L"FilterBtn"))return Publish(kControlMismatch);
        if(!query_.empty()) {
            void* edit=nullptr;void* parent=nullptr;
            if(!ReadPointer(service,0xB4,&edit) || !edit || !ReadPointer(edit,kControlParent,&parent) || parent!=root)return Publish(kControlMismatch);
            const void* populated=SetNativeEditQuery(bindings_,edit,query_.c_str());
            std::wstring actual;
            if(!populated || !ReadHostString(populated,64,&actual) || actual!=query_)return Publish(kUiNameMismatch);
        }
        if(!Invoke(bindings_.search_callback,service,kClickEvent,found))return Publish(kNativeFault);
        ++observation_.original_actions;
        allSelectedAt_=lastElapsed_;
        stage_=kStageWaitAll;
        return Publish(kSubmittingSearch);
    }

    Result WaitForAll(
        const SdgoGameTestContextV1* context)
    {
        if (!RoomStillMatches())
            return Publish(kServiceMismatch);
        const auto request=CatalogRequestReady();
        if(request!=kComplete)return Publish(request);
        const uint64_t signature =
            CatalogSignature();
        if (!signature)
            return Publish(kWaitingForCatalog);
        if (signature != initialCatalogSignature_)
            observedCatalogChange_ = true;
        std::vector<ProductRow> rows;
        uint32_t page = 0;
        uint32_t maximum = 0;
        if (!ReadProductRows(
                &rows,
                &page,
                &maximum))
            return Publish(context->elapsed_ms<allSelectedAt_+kCatalogWaitMilliseconds ? kWaitingForCatalog : kMalformedCatalog);
        if (rows.empty())
            return Publish(context->elapsed_ms<allSelectedAt_+kCatalogWaitMilliseconds ? kWaitingForCatalog : kCatalogTargetMissing);
        if (page != 0) {
            if (context->elapsed_ms <
                allSelectedAt_ +
                    kCatalogWaitMilliseconds)
                return Publish(kWaitingForCatalog);
            return Publish(kPageDidNotAdvance);
        }
        if (!LoadNativeNames())
            return Publish(kNativeNameMismatch);
        stage_ = kStageScanPage;
        return Publish(kWaitingForCatalog);
    }

    Result ScanPage(
        const SdgoGameTestContextV1* context)
    {
        if (!RoomStillMatches())
            return Publish(kServiceMismatch);
        std::vector<ProductRow> rows;
        uint32_t page = 0;
        uint32_t maximum = 0;
        if (!ReadProductRows(
                &rows,
                &page,
                &maximum))
            return Publish(kMalformedCatalog);
        observation_.current_page = page;
        observation_.max_page = maximum;

        for (const ProductRow& row : rows) {
            Product* product =
                PendingProduct(row.productId);
            if (!product)
                continue;
            const Result validated =
                ValidateProduct(
                    *product,
                    row.row);
            if (validated != kComplete)
                return Publish(validated);
            target_ = product;
            targetRow_ = row.row;
            stage_ = kStageOpenBuy;
            return Publish(kWaitingForCatalog);
        }

        if (verifiedKeys_.size() ==
            products_.size()) {
            stage_ = kStageDone;
            return Publish(kComplete);
        }
        if (page < maximum) {
            void* control =
                FindControl(
                    roomRoot_,
                    L"RightBtn",
                    kButtonKind);
            if (!control ||
                !ControlIdentity(
                    control,
                    roomRoot_,
                    L"RightBtn"))
                return Publish(kControlMismatch);
            pageBefore_ = page;
            if (!Invoke(
                    bindings_.room_callback,
                    room_,
                    kClickEvent,
                    control))
                return Publish(kNativeFault);
            ++observation_.original_actions;
            stage_ = kStageWaitPage;
            return Publish(kChangingPage);
        }

        if (context->elapsed_ms <
            allSelectedAt_ +
                kCatalogWaitMilliseconds)
            return Publish(kWaitingForCatalog);
        return Publish(kCatalogTargetMissing);
    }

    Result WaitForPage()
    {
        if (!RoomStillMatches())
            return Publish(kServiceMismatch);
        const auto request=CatalogRequestReady();
        if(request!=kComplete)return Publish(request);
        uint32_t page = 0;
        if (!ReadValue(
                room_,
                kRoomPage,
                &page))
            return Publish(kNativeFault);
        if (page == pageBefore_)
            return Publish(kChangingPage);
        if (page != pageBefore_ + 1)
            return Publish(kPageDidNotAdvance);
        stage_ = kStageScanPage;
        return Publish(kChangingPage);
    }

    Result OpenPreview(bool gift)
    {
        if (!target_ ||
            !RoomStillMatches())
            return Publish(kServiceMismatch);
        std::vector<ProductRow> rows;
        uint32_t page = 0;
        uint32_t maximum = 0;
        if (!ReadProductRows(
                &rows,
                &page,
                &maximum))
            return Publish(kMalformedCatalog);
        const auto found = std::find_if(
            rows.begin(),
            rows.end(),
            [this](const ProductRow& row) {
                return row.row == targetRow_ &&
                       row.productId ==
                           target_->productId;
            });
        if (found == rows.end())
            return Publish(kCatalogTargetMissing);

        PopupList popups = {};
        const Result popupResult =
            ReadPopupList(&popups);
        if (popupResult != kComplete)
            return Publish(popupResult);
        if (popups.count)
            return Publish(kUnsafeModalPresent);

        const std::wstring name =
            std::wstring(
                gift ? L"GiftBtn" : L"BuyBtn") +
            std::to_wstring(targetRow_);
        void* control =
            FindControl(
                roomRoot_,
                name.c_str(),
                kButtonKind);
        if (!control ||
            !ControlIdentity(
                control,
                roomRoot_,
                name.c_str()))
            return Publish(kControlMismatch);
        if (!Invoke(
                bindings_.room_callback,
                room_,
                kClickEvent,
                control))
            return Publish(kNativeFault);
        ++observation_.original_actions;
        popup_ = nullptr;
        stage_ =
            gift
                ? kStageWaitGift
                : kStageWaitBuy;
        return Publish(
            gift
                ? kOpeningGiftPreview
                : kOpeningBuyPreview);
    }

    Result WaitForPreview(bool gift)
    {
        PopupList popups = {};
        const Result popupResult =
            ReadPopupList(&popups);
        if (popupResult != kComplete)
            return Publish(popupResult);
        if (!popups.count)
            return Publish(
                gift
                    ? kWaitingForGiftPreview
                    : kWaitingForBuyPreview);
        if (popups.count != 1)
            return Publish(kUnsafeModalPresent);
        void* popup = popups.services[0];
        void* expected =
            gift
                ? bindings_.gift_vtable
                : bindings_.buy_vtable;
        if (!HasVtable(popup, expected))
            return Publish(kPopupMismatch);
        const Result verified =
            ValidatePopup(popup, gift);
        if (verified != kComplete)
            return Publish(verified);
        popup_ = popup;
        if (gift)
            ++observation_.gift_previews;
        else
            ++observation_.buy_previews;
        stage_ =
            gift
                ? kStageCancelGift
                : kStageCancelBuy;
        return Publish(
            gift
                ? kWaitingForGiftPreview
                : kWaitingForBuyPreview);
    }

    Result CancelPreview(bool gift)
    {
        if (!popup_ ||
            !HasVtable(
                popup_,
                gift
                    ? bindings_.gift_vtable
                    : bindings_.buy_vtable))
            return Publish(kPopupMismatch);
        void* root = nullptr;
        void* cancel = nullptr;
        if (!ReadPointer(
                popup_,
                kPopupRoot,
                &root) ||
            !ReadPointer(
                popup_,
                gift
                    ? kGiftCancel
                    : kBuyCancel,
                &cancel))
            return Publish(kNativeFault);
        if (!root ||
            !cancel ||
            !ControlIdentity(
                cancel,
                root,
                L"BuyCancelBtn"))
            return Publish(kControlMismatch);
        PopupCallbackFn callback =
            gift
                ? bindings_.gift_callback
                : bindings_.buy_callback;
        if (!Invoke(
                callback,
                popup_,
                kClickEvent,
                cancel))
            return Publish(kNativeFault);
        ++observation_.original_actions;
        ++observation_.cancellations;
        stage_ =
            gift
                ? kStageWaitGiftGone
                : kStageWaitBuyGone;
        return Publish(
            gift
                ? kCancellingGiftPreview
                : kCancellingBuyPreview);
    }

    Result WaitForPreviewGone(bool gift)
    {
        PopupList popups = {};
        const Result popupResult =
            ReadPopupList(&popups);
        if (popupResult != kComplete)
            return Publish(popupResult);
        for (uint32_t index = 0;
             index < popups.count;
             ++index) {
            if (popups.services[index] == popup_)
                return Publish(
                    gift
                        ? kCancellingGiftPreview
                        : kCancellingBuyPreview);
        }
        if (popups.count)
            return Publish(kUnsafeModalPresent);
        popup_ = nullptr;
        if (!gift) {
            stage_ = kStageOpenGift;
            return Publish(kCancellingBuyPreview);
        }
        if (!target_)
            return Publish(kNativeFault);
        if(visualOnly_) {stage_=kStageShowHover;return Publish(kCancellingGiftPreview);}
        return FinishProduct();
    }

    Result FinishProduct()
    {
        if(!target_)return Publish(kNativeFault);
        verifiedKeys_.insert(target_->key);
        observation_.verified =
            static_cast<uint32_t>(
                verifiedKeys_.size());
        target_ = nullptr;
        const auto next=std::find_if(products_.begin(),products_.end(),[this](const Product& p){return !verifiedKeys_.count(p.key);});
        const bool anotherQuery=visualOnly_ && next!=products_.end() && next->query!=query_;
        stage_ = anotherQuery ? kStageSelectAll : kStageScanPage;
        return Publish(kCancellingGiftPreview);
    }

    Result ValidateProduct(
        const Product& product,
        uint32_t row)
    {
        observation_.product_id=product.productId;
        std::vector<sdgo::shop::Option> observedOptions;
        if(!ReadOptions(room_,kRoomOptionsBegin,kRoomOptionsEnd,&observedOptions))return kMalformedCatalog;
        observation_.native_page_options=static_cast<uint32_t>(observedOptions.size());
        const auto name =
            nativeNames_.find(product.productId);
        if (name == nativeNames_.end() ||
            name->second != product.name)
            return kNativeNameMismatch;

        const std::wstring controlName =
            L"GoodsStatic" +
            std::to_wstring(row);
        void* control =
            FindControl(
                roomRoot_,
                controlName.c_str(),
                kStaticKind);
        if (!control ||
            !HasVtable(
                control,
                bindings_.static_text_vtable) ||
            !ControlIdentity(
                control,
                roomRoot_,
                controlName.c_str()))
            return kControlMismatch;
        std::wstring visible;
        if (!ReadHostString(
                static_cast<uint8_t*>(
                    control) +
                    kControlText,
                64,
                &visible))
            return kUiNameMismatch;
        std::wstring shortened;
        if (product.displayName.size() > 10)
            shortened =
                product.displayName.substr(0, 10) +
                L"..";
        observation_.native_name = name->second;
        observation_.visible_name = visible;
        if (visible != product.displayName &&
            visible != shortened)
            return kUiNameMismatch;

        std::vector<sdgo::shop::Option> options;
        if (!ReadOptions(
                room_,
                kRoomOptionsBegin,
                kRoomOptionsEnd,
                &options))
            return kMalformedCatalog;
        size_t match = 0;
        uint32_t optionIndex = 0;
        uint32_t matchedId = 0;
        for (uint32_t index = 0;
             index < options.size();
             ++index) {
            const sdgo::shop::Option& option =
                options[index];
            if (option.productId !=
                product.productId ||
                option.category !=
                    product.category ||
                option.validity !=
                    product.validity ||
                option.price !=
                    product.price ||
                option.payment !=
                    product.payment)
                continue;
            const auto generated =
                runtime_.find(option.id);
            if (generated ==
                    runtime_.end() ||
                !RuntimeMatches(
                    generated->second,
                    product))
                continue;
            ++match;
            optionIndex = index;
            matchedId = option.id;
        }
        if (match != 1)
            return kCatalogOptionMismatch;

        int32_t timeLimit = -1;
        if (!ReadTimeLimit(
                product.category,
                &timeLimit))
            return kCatalogOptionMismatch;
        if (timeLimit < 0 ||
            timeLimit > 2)
            return kCatalogOptionMismatch;
        const uint32_t kind =
            product.category / 100000;
        if (kind == 11 &&
            timeLimit != 0 &&
            timeLimit != 1)
            return kCatalogOptionMismatch;
        if (kind == 11 &&
            timeLimit == 0 &&
            product.validity != 1)
            return kCatalogOptionMismatch;
        if (timeLimit == 1 &&
            product.validity > 36500)
            return kCatalogOptionMismatch;

        targetOptionIndex_ = optionIndex;
        observation_.product_id =
            product.productId;
        observation_.option_id = matchedId;
        observation_.category =
            product.category;
        observation_.validity =
            product.validity;
        observation_.price = product.price;
        observation_.payment =
            product.payment;
        observation_.time_limit =
            timeLimit;
        observation_.native_name =
            name->second;
        observation_.visible_name =
            visible;
        return kComplete;
    }

    Result ValidatePopup(
        void* popup,
        bool gift)
    {
        void* root = nullptr;
        void* combo = nullptr;
        void* cancel = nullptr;
        if (!ReadPointer(
                popup,
                kPopupRoot,
                &root) ||
            !ReadPointer(
                popup,
                gift
                    ? kGiftCombo
                    : kBuyCombo,
                &combo) ||
            !ReadPointer(
                popup,
                gift
                    ? kGiftCancel
                    : kBuyCancel,
                &cancel))
            return kNativeFault;
        if (!root ||
            !combo ||
            !cancel ||
            !ControlIdentity(
                combo,
                root,
                L"Combo") ||
            !ControlIdentity(
                cancel,
                root,
                L"BuyCancelBtn"))
            return kControlMismatch;

        std::vector<sdgo::shop::Option> options;
        if (!ReadOptions(
                popup,
                gift
                    ? kGiftOptionsBegin
                    : kBuyOptionsBegin,
                gift
                    ? kGiftOptionsEnd
                    : kBuyOptionsEnd,
                &options) ||
            options.empty())
            return kPopupOptionMismatch;
        uint32_t matching = UINT32_MAX;
        for (uint32_t index = 0;
             index < options.size();
             ++index) {
            if (options[index].productId !=
                target_->productId)
                return kPopupOptionMismatch;
            if (options[index].id ==
                observation_.option_id) {
                if (matching != UINT32_MAX)
                    return kPopupOptionMismatch;
                matching = index;
            }
        }
        if (matching == UINT32_MAX)
            return kPopupOptionMismatch;
        const sdgo::shop::Option& option =
            options[matching];
        if (option.category !=
                target_->category ||
            option.validity !=
                target_->validity ||
            option.price !=
                target_->price ||
            option.payment !=
                target_->payment)
            return kPopupOptionMismatch;

        if(visualOnly_) {
            void* title=FindControl(root,L"TitleStatic",kStaticKind);
            std::wstring actual;
            observation_.popup_name_found=title?1u:0u;
            const bool read=title && ReadHostString(static_cast<uint8_t*>(title)+kControlText,255,&actual);
            observation_.popup_name=actual;
            if(!read || actual!=target_->displayName)
                return kUiNameMismatch;
        }

        uintptr_t itemBegin = 0;
        uintptr_t itemEnd = 0;
        if (!ReadValue(
                combo,
                kComboItemsBegin,
                &itemBegin) ||
            !ReadValue(
                combo,
                kComboItemsEnd,
                &itemEnd) ||
            !itemBegin ||
            itemEnd < itemBegin ||
            (itemEnd - itemBegin) %
                    sizeof(void*) !=
                0 ||
            (itemEnd - itemBegin) /
                    sizeof(void*) !=
                options.size())
            return kPopupOptionMismatch;
        void* nativeLabel = nullptr;
        void* entry=nullptr;void* optionButton=nullptr;
        if(!ReadAddress(itemBegin+matching*sizeof(void*),&entry) || !entry ||
           !ReadPointer(entry,0x1C,&optionButton) || !optionButton ||
           !ControlIdentity(optionButton,combo,std::to_wstring(matching).c_str()) ||
           !ReadButtonTextItem(
                optionButton,
                0,
                &nativeLabel) ||
            !nativeLabel)
            return kUiDurationMismatch;
        std::wstring label;
        if (!ReadHostString(
                nativeLabel,
                255,
                &label))
            return kUiDurationMismatch;
        std::wstring expected =
            sdgo::shop::DurationLabel(
                option,
                observation_.time_limit);
        if (expected.empty()) {
            const auto runtime =
                runtime_.find(option.id);
            if (runtime == runtime_.end())
                return kPopupOptionMismatch;
            expected = runtime->second.text;
        }
        if(visualOnly_) {
            if(label.rfind(expected+L" :",0)!=0 || label.find(std::to_wstring(option.price))==std::wstring::npos)
                return kUiDurationMismatch;
        } else if (label != expected)
            return kUiDurationMismatch;
        targetOptionIndex_ = matching;
        observation_.option_label = label;
        return kComplete;
    }

    bool RuntimeMatches(
        const RuntimeOption& option,
        const Product& product) const
    {
        return option.category ==
                   product.category &&
               option.validity ==
                   product.validity &&
               option.payment ==
                   product.payment &&
               option.price ==
                   product.price &&
               option.text ==
                   product.label;
    }

    Product* PendingProduct(uint32_t productId)
    {
        for (Product& product : products_) {
            if (product.productId ==
                    productId &&
                !verifiedKeys_.count(
                    product.key))
                return &product;
        }
        return nullptr;
    }

    bool LoadNativeNames()
    {
        nativeNames_.clear();
        observation_.duplicate_native_product_ids.clear();
        observation_.duplicate_native_title_rows = 0;
        std::set<uint32_t> duplicateProductIds;
        void* data = nullptr;
        if (!ReadGridData(&data) || !data)
            return false;
        for (uint32_t id = 210001;
             id < 310001;
             ++id) {
            const uint8_t* row = nullptr;
            if (!FindGrid(
                    data,
                    id,
                    0x23,
                    &row))
                break;
            uint32_t productId = 0;
            memcpy(
                &productId,
                row + 5,
                sizeof(productId));
            std::wstring name;
            const bool requested=std::any_of(products_.begin(),products_.end(),
                [productId](const Product& product){return product.productId==productId;});
            if (!productId)continue;
            if (!ReadBoundedWide(
                    reinterpret_cast<
                        const wchar_t*>(
                        row + 9),
                    50,
                    &name) ||
                name.empty()) {
                // Stock titles outside this read-only plan can fill all 50
                // native characters without a terminator. They do not make
                // an independently identified target title ambiguous.
                if(requested)return false;
                continue;
            }
            const auto inserted =
                nativeNames_.emplace(
                    productId,
                    std::move(name));
            if (!inserted.second) {
                duplicateProductIds.insert(productId);
                ++observation_.
                    duplicate_native_title_rows;
            }
        }
        observation_.duplicate_native_product_ids.assign(
            duplicateProductIds.begin(),
            duplicateProductIds.end());
        for (const Product& product : products_) {
            if (!nativeNames_.count(
                    product.productId) ||
                duplicateProductIds.count(
                    product.productId))
                return false;
        }
        return true;
    }

    Result CheckIntegrity()
    {
        IntegrityImage current;
        const Result captured =
            CaptureIntegrity(&current);
        if (captured != kComplete)
            return captured;
        if (current.gp != baseline_.gp ||
            current.cash != baseline_.cash) {
            observation_.wallet_unchanged = 0;
            return kWalletChanged;
        }
        observation_.wallet_unchanged = 1;
        if (current.active_slot !=
                baseline_.active_slot ||
            current.capacity !=
                baseline_.capacity ||
            current.units !=
                baseline_.units) {
            observation_.inventory_unchanged = 0;
            return kInventoryChanged;
        }
        observation_.inventory_unchanged = 1;
        return kComplete;
    }

    Result CaptureIntegrity(IntegrityImage* output)
    {
        observation_.inventory_failure=1;
        if (!output)
            return kInventoryUnavailable;
        uint8_t* profile = nullptr;
        uint8_t* info = nullptr;
        int32_t capacity = 0;
        uint8_t ready = 0;
        if (!ReadIndirect(
                bindings_.profile_pointer,
                &profile) ||
            !ReadIndirect(
                reinterpret_cast<uint8_t**>(
                    bindings_.unit_info_pointer),
                &info) ||
            !profile ||
            !info ||
            !ReadValue(
                info,
                kUnitInfoCapacity,
                &capacity) ||
            !ReadValue(
                info,
                kUnitInfoReady,
                &ready))
            return kInventoryUnavailable;
        observation_.inventory_ready=ready;
        observation_.inventory_capacity=capacity;
        observation_.inventory_failure=2;
        if (!ready ||
            capacity <= 0 ||
            capacity >
                static_cast<int32_t>(
                    kMaximumInventorySlots))
            return kInventoryUnavailable;
        IntegrityImage image;
        observation_.inventory_failure=3;
        if (!ReadValue(
                profile,
                kProfileGp,
                &image.gp) ||
            !ReadValue(
                profile,
                kProfileCash,
                &image.cash) ||
            !ReadValue(
                profile,
                kProfileActiveSlot,
                &image.active_slot))
            return kInventoryUnavailable;
        image.capacity = capacity;
        observation_.inventory_failure=4;
        if (image.active_slot >
            static_cast<uint16_t>(capacity))
            return kInventoryUnavailable;
        image.units.resize(
            static_cast<size_t>(capacity));
        for (int32_t index = 0;
             index < capacity;
             ++index) {
            observation_.inventory_failure=5;
            observation_.inventory_probe_slot=static_cast<uint32_t>(index+1);
            UnitImage unit;
            if (!ReadUnit(
                    info,
                    static_cast<uint16_t>(
                        index + 1),
                    &unit))
                return kInventoryUnavailable;
            uint32_t type = 0;
            uint64_t uid = 0;
            uint16_t slot = 0;
            memcpy(
                &type,
                unit.brief.data(),
                sizeof(type));
            memcpy(
                &uid,
                unit.brief.data() + 4,
                sizeof(uid));
            memcpy(
                &slot,
                unit.brief.data() + 12,
                sizeof(slot));
            if (type && (!uid || slot != index + 1))
                return kInventoryUnavailable;
            if (type && !visualOnly_) {
                uint32_t fullType = 0;
                uint16_t fullSlot = 0;
                uint64_t fullUid = 0;
                memcpy(
                    &fullType,
                    unit.full.data(),
                    sizeof(fullType));
                memcpy(
                    &fullSlot,
                    unit.full.data() + 4,
                    sizeof(fullSlot));
                memcpy(
                    &fullUid,
                    unit.full.data() + 8,
                    sizeof(fullUid));
                observation_.inventory_failure=6;
                observation_.inventory_brief_unit=type;
                observation_.inventory_full_unit=fullType;
                observation_.inventory_brief_slot=slot;
                observation_.inventory_full_slot=fullSlot;
                observation_.inventory_brief_uid=uid;
                observation_.inventory_full_uid=fullUid;
                if (!uid ||
                    slot != index + 1 ||
                    fullType != type ||
                    fullSlot !=
                        static_cast<uint16_t>(
                            index + 1) ||
                    fullUid != uid)
                    return kInventoryUnavailable;
            }
            image.units[
                static_cast<size_t>(index)] =
                std::move(unit);
        }
        *output = std::move(image);
        observation_.inventory_failure=0;
        return kComplete;
    }

    bool ReadUnit(
        void* info,
        uint16_t slot,
        UnitImage* output) const
    {
        if (!info || !output)
            return false;
        UnitImage unit;
        __try {
            bindings_.unit_brief_by_slot(
                info,
                unit.brief.data(),
                slot);
            if (!visualOnly_) bindings_.unit_full_by_slot(
                info,
                unit.full.data(),
                slot);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        *output = unit;
        return true;
    }

    bool ReadAppState(uint32_t* state) const
    {
        if (!state)
            return false;
        __try {
            uint8_t* app = bindings_.game_app();
            if (!app)
                return false;
            memcpy(
                state,
                app + 0x4A0,
                sizeof(*state));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool RoomStillMatches() const
    {
        void* service = nullptr;
        return ResolveService(
                   L"roommgr",
                   bindings_.room_vtable,
                   &service) ==
                   kServiceMatched &&
               service == room_ &&
               HasVtable(
                   service,
                   bindings_.room_vtable);
    }

    enum ServiceResult
    {
        kServiceAbsent,
        kServiceMatched,
        kServiceWrong,
    };

    ServiceResult ResolveService(
        const wchar_t* name,
        void* expectedVtable,
        void** output) const
    {
        if (output)
            *output = nullptr;
        if (!name ||
            !expectedVtable ||
            !output)
            return kServiceAbsent;
        void* manager = nullptr;
        if (!ReadIndirect(
                reinterpret_cast<uint8_t**>(
                    bindings_.service_managed_pointer),
                reinterpret_cast<uint8_t**>(
                    &manager)) ||
            !manager)
            return kServiceAbsent;
        alignas(4) uint8_t descriptor[
            kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(
            descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            constructed =
                bindings_.native_wstring_ctor(
                    descriptor +
                        sizeof(uint32_t),
                    name) != nullptr;
            if (constructed)
                service =
                    bindings_.service_find_by_name(
                        manager,
                        descriptor);
        } __finally {
            if (constructed)
                bindings_.native_wstring_dtor(
                    descriptor +
                    sizeof(uint32_t));
        }
        if (!service)
            return kServiceAbsent;
        *output = service;
        return HasVtable(
                   service,
                   expectedVtable)
            ? kServiceMatched
            : kServiceWrong;
    }

    void* FindControl(
        void* root,
        const wchar_t* name,
        uint32_t kind) const
    {
        if (!root || !name)
            return nullptr;
        alignas(4) uint8_t nativeName[
            kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            constructed =
                bindings_.native_wstring_ctor(
                    nativeName,
                    name) != nullptr;
            if (constructed)
                control =
                    bindings_.native_ui_find_control(
                        root,
                        nativeName,
                        kind);
        } __finally {
            if (constructed)
                bindings_.native_wstring_dtor(
                    nativeName);
        }
        return control;
    }

    static bool HasVtable(
        const void* object,
        const void* expected)
    {
        if (!object || !expected)
            return false;
        __try {
            return *reinterpret_cast<
                       void* const*>(object) ==
                   expected;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadHostString(
        const void* native,
        size_t maximum,
        std::wstring* output)
    {
        if (!native || !output)
            return false;
        __try {
            const uint8_t* bytes =
                static_cast<const uint8_t*>(
                    native);
            uint32_t length = 0;
            uint32_t capacity = 0;
            memcpy(
                &length,
                bytes + kStringLength,
                sizeof(length));
            memcpy(
                &capacity,
                bytes + kStringCapacity,
                sizeof(capacity));
            if (length > maximum ||
                capacity < length)
                return false;
            const wchar_t* value =
                capacity < 8
                    ? reinterpret_cast<
                          const wchar_t*>(bytes)
                    : *reinterpret_cast<
                          wchar_t* const*>(bytes);
            if (!value ||
                value[length] != L'\0')
                return false;
            output->assign(value, length);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadBoundedWide(
        const wchar_t* value,
        size_t maximum,
        std::wstring* output)
    {
        if (!value || !output)
            return false;
        __try {
            size_t length = 0;
            while (length <= maximum &&
                   value[length])
                ++length;
            if (!length || length > maximum)
                return false;
            output->assign(value, length);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ControlIdentity(
        const void* control,
        const void* parent,
        const wchar_t* name)
    {
        if (!control || !parent || !name)
            return false;
        void* actualParent = nullptr;
        if (!ReadPointer(
                control,
                kControlParent,
                &actualParent) ||
            actualParent != parent)
            return false;
        std::wstring actual;
        return ReadHostString(
                   static_cast<const uint8_t*>(
                       control) +
                       kControlName,
                   63,
                   &actual) &&
               actual == name;
    }

    static bool Invoke(
        UiCallbackFn callback,
        void* service,
        uint32_t event,
        void* control)
    {
        if (!callback ||
            !service ||
            !control)
            return false;
        __try {
            callback(
                service,
                event,
                static_cast<uint8_t*>(
                    control) +
                    kControlName,
                control);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool Invoke(
        PopupCallbackFn callback,
        void* service,
        uint32_t event,
        void* control)
    {
        if (!callback ||
            !service ||
            !control)
            return false;
        __try {
            callback(
                service,
                event,
                static_cast<uint8_t*>(
                    control) +
                    kControlName,
                control);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Result ReadPopupList(
        PopupList* output)
    {
        if (!output)
            return kNativeFault;
        PopupList list = {};
        __try {
            uint8_t* manager =
                bindings_.service_managed_pointer
                    ? static_cast<uint8_t*>(
                          *bindings_.
                              service_managed_pointer)
                    : nullptr;
            if (!manager)
                return kNativeFault;
            void* sentinel =
                *reinterpret_cast<void**>(
                    manager +
                    kManagerPopupSentinel);
            const uint32_t count =
                *reinterpret_cast<uint32_t*>(
                    manager +
                    kManagerPopupCount);
            if (!sentinel ||
                count >
                    kMaximumPopupCount)
                return kNativeFault;
            void* previous = sentinel;
            void* node =
                *reinterpret_cast<void**>(
                    sentinel);
            uint32_t seen = 0;
            while (node != sentinel) {
                if (!node ||
                    seen >= count ||
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(
                            node) + 4) !=
                        previous)
                    return kNativeFault;
                void* service =
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(
                            node) + 8);
                if (!service)
                    return kNativeFault;
                list.services[seen++] =
                    service;
                previous = node;
                node =
                    *reinterpret_cast<void**>(
                        node);
            }
            if (seen != count ||
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(
                        sentinel) + 4) !=
                    previous)
                return kNativeFault;
            list.count = count;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return kNativeFault;
        }
        *output = list;
        observation_.popup_count =
            list.count;
        return kComplete;
    }

    bool ReadProductRows(
        std::vector<ProductRow>* output,
        uint32_t* page,
        uint32_t* maximum)
    {
        if (!output || !page || !maximum)
            return false;
        uintptr_t begin = 0;
        uintptr_t end = 0;
        uint32_t current = 0;
        uint32_t last = 0;
        if (!ReadValue(
                room_,
                kRoomProductsBegin,
                &begin) ||
            !ReadValue(
                room_,
                kRoomProductsEnd,
                &end) ||
            !ReadValue(
                room_,
                kRoomPage,
                &current) ||
            !ReadValue(
                room_,
                kRoomMaxPage,
                &last))
            return false;
        observation_.current_page=current;observation_.max_page=last;
        observation_.native_page_bytes=end>=begin ? static_cast<uint32_t>(end-begin) : UINT32_MAX;
        if (end < begin ||
            (end - begin) %
                    kProductStride !=
                0 ||
            (end - begin) /
                    kProductStride >
                8 ||
            current > last ||
            last > kMaximumPages)
            return false;
        std::vector<ProductRow> rows;
        const uint32_t count =
            static_cast<uint32_t>(
                (end - begin) /
                kProductStride);
        observation_.native_page_slots=count;observation_.native_page_nonempty=0;
        rows.reserve(count);
        for (uint32_t index = 0;
             index < count;
             ++index) {
            uint32_t productId = 0;
            if (!ReadAddress(
                    begin +
                        index *
                            kProductStride,
                    &productId))
                return false;
            if (!productId)continue; // native pages retain empty display slots
            ++observation_.native_page_nonempty;
            for (const ProductRow& row : rows) {
                if (row.productId ==
                    productId)
                    return false;
            }
            rows.push_back(
                {productId, index});
        }
        output->swap(rows);
        *page = current;
        *maximum = last;
        return true;
    }

    bool ReadOptions(
        const void* owner,
        size_t beginOffset,
        size_t endOffset,
        std::vector<sdgo::shop::Option>* output) const
    {
        if (!owner || !output)
            return false;
        uintptr_t begin = 0;
        uintptr_t end = 0;
        if (!ReadValue(
                owner,
                beginOffset,
                &begin) ||
            !ReadValue(
                owner,
                endOffset,
                &end) ||
            end < begin ||
            (end - begin) %
                    sizeof(
                        sdgo::shop::Option) !=
                0 ||
            (end - begin) /
                    sizeof(
                        sdgo::shop::Option) >
                128)
            return false;
        const size_t count =
            (end - begin) /
            sizeof(sdgo::shop::Option);
        std::vector<sdgo::shop::Option>
            options(count);
        if (count &&
            !CopyAddress(
                begin,
                options.data(),
                count *
                    sizeof(
                        sdgo::shop::Option)))
            return false;
        output->swap(options);
        return true;
    }

    bool ReadTimeLimit(
        uint32_t category,
        int32_t* output) const
    {
        if (!output)
            return false;
        void* data = nullptr;
        const uint8_t* row = nullptr;
        if (!ReadGridData(&data) ||
            !data ||
            !FindGrid(
                data,
                category,
                0x22,
                &row))
            return false;
        uint32_t actual = 0;
        memcpy(
            &actual,
            row,
            sizeof(actual));
        if (actual != category)
            return false;
        *output = row[0x6D];
        return true;
    }

    bool ReadGridData(void** output) const
    {
        if (!output)
            return false;
        __try {
            *output = bindings_.grid_data();
            return *output != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *output = nullptr;
            return false;
        }
    }

    bool FindGrid(
        void* data,
        uint32_t id,
        int type,
        const uint8_t** output) const
    {
        if (!data || !output)
            return false;
        __try {
            *output = nullptr;
            return bindings_.grid_find(
                       data,
                       output,
                       id,
                       type,
                       0) != 0 &&
                   *output;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *output = nullptr;
            return false;
        }
    }

    bool ReadButtonTextItem(
        void* combo,
        uint32_t index,
        void** output) const
    {
        if (!combo || !output)
            return false;
        __try {
            *output =
                bindings_.button_text_item(
                    combo,
                    index);
            return *output != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *output = nullptr;
            return false;
        }
    }

    uint64_t CatalogSignature() const
    {
        if (!room_)
            return 0;
        uintptr_t productsBegin = 0;
        uintptr_t productsEnd = 0;
        uintptr_t optionsBegin = 0;
        uintptr_t optionsEnd = 0;
        uint32_t page = 0;
        uint32_t maximum = 0;
        if (!ReadValue(
                room_,
                kRoomProductsBegin,
                &productsBegin) ||
            !ReadValue(
                room_,
                kRoomProductsEnd,
                &productsEnd) ||
            !ReadValue(
                room_,
                kRoomOptionsBegin,
                &optionsBegin) ||
            !ReadValue(
                room_,
                kRoomOptionsEnd,
                &optionsEnd) ||
            !ReadValue(
                room_,
                kRoomPage,
                &page) ||
            !ReadValue(
                room_,
                kRoomMaxPage,
                &maximum))
            return 0;
        uint64_t hash = 1469598103934665603ULL;
        const uint64_t values[] = {
            productsBegin,
            productsEnd,
            optionsBegin,
            optionsEnd,
            page,
            maximum,
        };
        for (uint64_t value : values) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    template <typename T>
    static bool ReadValue(
        const void* object,
        size_t offset,
        T* output)
    {
        if (!object || !output)
            return false;
        __try {
            memcpy(
                output,
                static_cast<const uint8_t*>(
                    object) +
                    offset,
                sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <typename T>
    static bool ReadAddress(
        uintptr_t address,
        T* output)
    {
        if (!address || !output)
            return false;
        __try {
            memcpy(
                output,
                reinterpret_cast<const void*>(
                    address),
                sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool CopyAddress(
        uintptr_t address,
        void* output,
        size_t size)
    {
        if ((!address && size) ||
            (!output && size))
            return false;
        __try {
            if (size)
                memcpy(
                    output,
                    reinterpret_cast<
                        const void*>(address),
                    size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadPointer(
        const void* object,
        size_t offset,
        void** output)
    {
        return ReadValue(
            object,
            offset,
            output);
    }

    static bool ReadIndirect(
        uint8_t** slot,
        uint8_t** output)
    {
        if (!slot || !output)
            return false;
        __try {
            *output = *slot;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *output = nullptr;
            return false;
        }
    }

    bool ReadProfileByte(
        size_t offset,
        uint8_t* output) const
    {
        uint8_t* profile = nullptr;
        return ReadIndirect(
                   bindings_.profile_pointer,
                   &profile) &&
               profile &&
               ReadValue(
                   profile,
                   offset,
                   output);
    }

    Result Publish(Result result)
    {
        observation_.result =
            static_cast<uint32_t>(result);
        observation_.stage =
            static_cast<uint32_t>(stage_);
        return result;
    }

    static bool RequestUnitPage(UnitPageRequestFn request,uint32_t page) {
        __try {request(page);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    }
    static bool InvokeHover(HoverByRowFn fn,void* room,uint32_t row) {
        if(!fn || !room || row>=8)return false;
        __try {fn(room,static_cast<int>(row));return true;}
        __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    }

    static const void* SetNativeEditQuery(const Bindings& b,void* edit,const wchar_t* query) {
        NativeRequestName text;
        bool constructed=false;
        const void* result=nullptr;
        __try {
            constructed=b.native_wstring_ctor(text.bytes.data(),query)!=nullptr;
            if(constructed) {b.edit_set_text(edit,text.bytes.data(),0);result=b.edit_get_text(edit);}
        } __finally {if(constructed)b.native_wstring_dtor(text.bytes.data());}
        return result;
    }

    Bindings bindings_ = {};
    bool bound_ = false;
    bool configured_ = false;
    Stage stage_ = kStageIdle;
    uint64_t runHash_ = 0;
    DWORD uiThread_ = 0;
    DWORD processId_ = 0;
    uint64_t lastElapsed_ = 0;
    std::wstring catalogId_;
    std::wstring revision_;
    std::vector<Product> products_;
    std::map<uint32_t, RuntimeOption> runtime_;
    std::map<uint32_t, std::wstring> nativeNames_;
    std::set<std::wstring> verifiedKeys_;
    std::set<uint32_t> requestedInventoryPages_;
    bool stockAssertions_ = false;
    bool visualOnly_ = false;
    std::wstring query_;
    IntegrityImage baseline_;
    bool hasBaseline_ = false;
    void* room_ = nullptr;
    void* roomRoot_ = nullptr;
    void* popup_ = nullptr;
    Product* target_ = nullptr;
    uint32_t targetRow_ = 0;
    uint32_t targetOptionIndex_ = 0;
    uint32_t pageBefore_ = 0;
    uint64_t allSelectedAt_ = 0;
    uint64_t initialCatalogSignature_ = 0;
    bool observedCatalogChange_ = false;
    Observation observation_ = {};
};
