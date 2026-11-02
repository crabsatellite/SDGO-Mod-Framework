#pragma once

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>

#include "native_actions.h"

// Selects an owned ordinary unit through the shipped unitmgr UI selection
// method and REQ_ACTIVE_WAITUNIT business. Completion requires the original
// 0x09B4 reply consumer to run and the server-loaded profile plus unit
// inventory to expose the requested UID as the active slot.
//
// This adapter never directly writes the profile, inventory, unitmgr page,
// list item, account, room, or application state. The original
// CUIListBox::OnMouseMessage method owns the complete down/up selection and
// 0x702 event, and the original unitmgr business/reply methods own the request
// and resulting state change.
class UnitSelectActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForHangar = 1,
        kWaitingForUnitManager = 2,
        kWaitingForInventory = 3,
        kSelecting = 4,
        kWaitingForReply = 5,
        kComplete = 6,
        kAlreadySelected = 7,
        kWaitingForListItems = 8,
        kWaitingForFullUnit = 9,
        kNavigatingUnitPage = 10,
        kWaitingForUnitPageState = 11,

        kInvalidTarget = 0x100,
        kUnitManagerMismatch = 0x101,
        kUnitListMissing = 0x102,
        kUnitListMismatch = 0x103,
        kTargetMissing = 0x104,
        kTargetDuplicate = 0x105,
        kTargetPageMismatch = 0x106,
        kNativeSelectionRejected = 0x107,
        kRequestRejected = 0x108,
        kReplyRejected = 0x109,
        kReplyMismatch = 0x10A,
        kInventoryInvalid = 0x10B,
        kNativeFault = 0x10C,
        kPageControlMissing = 0x10D,
        kPageControlMismatch = 0x10E,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        uint32_t request_transport_sequence;
        uint32_t reply_sequence;
        uint32_t select_invocations;
        uint32_t request_size;
        uint32_t request_result;
        uint32_t native_root_pointer;
        uint32_t native_list_pointer;
        uint32_t native_row_pointer;
        uint32_t native_row_control_pointer;
        uint32_t native_row_x_bits;
        uint32_t native_row_y_bits;
        int32_t inventory_capacity;
        int32_t native_click_x;
        int32_t native_click_y;
        int32_t native_list_pressed_index;
        int32_t native_list_selected_index;
        uint32_t inventory_bad_unit_id;
        uint32_t target_unit_id;
        uint64_t target_uid;
        uint64_t inventory_bad_uid;
        uint64_t initial_uid;
        uint64_t current_uid;
        uint64_t reply_current_uid;
        uint16_t request_opcode;
        uint16_t reply_opcode;
        uint16_t reply_status;
        uint16_t initial_slot;
        uint16_t target_slot;
        uint16_t current_slot;
        uint16_t reply_packet_slot;
        uint16_t reply_current_slot;
        uint16_t unit_manager_page;
        uint16_t inventory_bad_slot;
        uint16_t inventory_bad_record_slot;
        uint8_t inventory_ready;
        uint8_t unit_manager_present;
        uint8_t unit_list_present;
        uint8_t native_row_getter_return;
        uint8_t native_row_x_finite;
        uint8_t native_row_y_finite;
        uint8_t native_target_occupied;
        uint8_t native_row_selected_before;
        uint8_t native_row_selected;
        uint8_t native_mouse_down_accepted;
        uint8_t native_mouse_up_accepted;
        uint8_t request_invoked;
        uint8_t reply_consumed;
        uint8_t target_matches;
        uint8_t reserved;
        int32_t unit_manager_page_raw;
        int32_t unit_manager_max_page_raw;
        int32_t unit_manager_page_group_raw;
        int32_t unit_manager_max_page_group_raw;
        uint32_t target_page;
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;

        profilePointer_ =
            Symbol<uint8_t**>("flow.profile_pointer");
        unitInfoPointer_ =
            Symbol<void**>("flow.unit_info_pointer");
        unitBriefBySlot_ =
            Symbol<UnitBriefBySlotFn>(
                "flow.unit_brief_by_slot");
        unitFullBySlot_ = Symbol<UnitBriefBySlotFn>("flow.unit_full_by_slot");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>(
                "flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>(
                "flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>(
                "flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        unitManagerVtable_ =
            Symbol<void*>(
                "flow.battleship_lobby_manager_vtable");
        unitManagerCallback_ =
            Symbol<UiCallbackFn>(
                "flow.battleship_lobby_select_business");
        unitListMouseBusiness_ =
            Symbol<UnitListMouseBusinessFn>(
                "flow.unit_list_mouse_business");
        unitListItemByIndex_ =
            Symbol<UnitListItemByIndexFn>(
                "flow.unit_list_item_by_index");
        selectBusiness_ =
            Symbol<SelectBusinessFn>(
                "flow.unit_active_waitunit_business");
        activeWaitReply_ =
            Symbol<ActiveWaitReplyFn>(
                "flow.unit_active_waitunit_reply");

        if (!api_ || !mod_ || !native_ ||
            !profilePointer_ || !unitInfoPointer_ ||
            !unitBriefBySlot_ || !unitFullBySlot_ || !serviceManagedPointer_ ||
            !serviceFindByName_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !nativeUiFindControl_ ||
            !unitManagerVtable_ || !unitManagerCallback_ ||
            !unitListMouseBusiness_ ||
            !unitListItemByIndex_ || !selectBusiness_ ||
            !activeWaitReply_)
            return false;
        __try {
            if (!CallbackSlotMatches(
                    unitManagerVtable_,
                    unitManagerCallback_))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (active_ && active_ != this)
            return false;

        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.unit_active_waitunit_reply",
                reinterpret_cast<void*>(
                    &ObserveActiveWaitReply),
                reinterpret_cast<void**>(
                    &activeWaitReply_))) {
            active_ = nullptr;
            return false;
        }

        bound_ = true;
        ResetState();
        return true;
    }

    Result Poll(uint64_t desiredUid)
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (!desiredUid)
            return Publish(kInvalidTarget);
        if (targetInitialized_ &&
            desiredUid != targetUid_) {
            if (requestInvoked_)
                return Publish(kInvalidTarget);
            ResetAttempt();
            targetUid_ = desiredUid;
        }
        if (!targetInitialized_) {
            targetUid_ = desiredUid;
            targetInitialized_ = true;
        }

        __try {
            const uint32_t appState =
                native_->AppState();
            InterlockedExchange(
                &nativeAppState_,
                static_cast<LONG>(appState));
            if (appState != kHangarAppState)
                return Publish(kWaitingForHangar);

            void* unitManager = nullptr;
            const ServiceResult service =
                ResolveService(
                    L"unitmgr",
                    unitManagerVtable_,
                    &unitManager);
            InterlockedExchange(
                &unitManagerPresent_,
                unitManager ? 1 : 0);
            if (service == kServiceMismatched)
                return Publish(kUnitManagerMismatch);
            if (service == kServiceAbsent)
                return Publish(kWaitingForUnitManager);

            const InventoryResult inventory =
                ResolveInventory(desiredUid);
            if (inventory == kInventoryWaiting)
                return Publish(kWaitingForInventory);
            if (inventory == kInventoryMalformed)
                return Publish(kInventoryInvalid);
            if (inventory == kInventoryTargetMissing)
                return Publish(kTargetMissing);
            if (inventory == kInventoryTargetDuplicate)
                return Publish(kTargetDuplicate);

            uint16_t activeSlot = 0;
            uint64_t activeUid = 0;
            if (!ReadCurrentUnit(
                    &activeSlot,
                    &activeUid))
                return Publish(kWaitingForInventory);
            PublishCurrent(activeSlot, activeUid);

            if (!initialCaptured_) {
                initialSlot_ = activeSlot;
                initialUid_ = activeUid;
                initialCaptured_ = true;
            }

            if (requestInvoked_)
                return EvaluateReply(
                    activeSlot,
                    activeUid);
            if (activeUid == desiredUid)
                return Publish(kAlreadySelected);

            auto* managerBytes =
                static_cast<uint8_t*>(unitManager);
            void* root =
                *reinterpret_cast<void**>(
                    managerBytes +
                    kUnitManagerRoot);
            InterlockedExchange(
                &nativeRootPointer_,
                PointerValue(root));
            if (!root)
                return Publish(kWaitingForUnitManager);

            void* list =
                FindControl(
                    root,
                    L"UnitList",
                    kListControlKind);
            InterlockedExchange(
                &unitListPresent_,
                list ? 1 : 0);
            InterlockedExchange(
                &nativeListPointer_,
                PointerValue(list));
            if (!list)
                return Publish(kUnitListMissing);
            if (*reinterpret_cast<void**>(
                    static_cast<uint8_t*>(list) +
                    kControlParent) != root ||
                !ControlHasName(
                    list,
                    L"UnitList"))
                return Publish(kUnitListMismatch);
            auto* listBytes =
                static_cast<uint8_t*>(list);
            InterlockedExchange(
                &nativeListPressedIndex_,
                *reinterpret_cast<const int32_t*>(
                    listBytes +
                    kListPressedIndex));
            InterlockedExchange(
                &nativeListSelectedIndex_,
                *reinterpret_cast<const int32_t*>(
                    listBytes +
                    kListSelectedIndex));

            const uint32_t targetPage =
                (targetSlot_ - 1) /
                kUnitsPerPage;
            const int32_t page =
                *reinterpret_cast<const int32_t*>(
                    managerBytes +
                    kUnitManagerPage);
            const int32_t maximumPage =
                *reinterpret_cast<const int32_t*>(
                    managerBytes +
                    kUnitManagerMaximumPage);
            const int32_t currentGroup =
                *reinterpret_cast<const int32_t*>(
                    managerBytes +
                    kUnitManagerPageGroup);
            const int32_t maximumGroup =
                *reinterpret_cast<const int32_t*>(
                    managerBytes +
                    kUnitManagerMaximumPageGroup);
            InterlockedExchange(
                &unitManagerPage_,
                page);
            InterlockedExchange(
                &unitManagerPageRaw_,
                page);
            InterlockedExchange(
                &unitManagerMaximumPageRaw_,
                maximumPage);
            InterlockedExchange(
                &unitManagerPageGroupRaw_,
                currentGroup);
            InterlockedExchange(
                &unitManagerMaximumPageGroupRaw_,
                maximumGroup);
            InterlockedExchange(
                &targetPageObservation_,
                static_cast<LONG>(
                    targetPage));

            const int32_t inventoryCapacity =
                static_cast<int32_t>(
                    InterlockedCompareExchange(
                        &inventoryCapacity_,
                        0,
                        0));
            const int32_t expectedMaximumPage =
                (inventoryCapacity - 1) /
                static_cast<int32_t>(
                    kUnitsPerPage);
            const int32_t expectedMaximumGroup =
                expectedMaximumPage /
                static_cast<int32_t>(
                    kPagesPerGroup);
            if (maximumPage !=
                    expectedMaximumPage ||
                maximumGroup !=
                    expectedMaximumGroup)
                return Publish(
                    kWaitingForUnitPageState);
            if (page < 0 ||
                currentGroup < 0 ||
                page > maximumPage ||
                currentGroup > maximumGroup ||
                targetPage >
                    static_cast<uint32_t>(
                        maximumPage))
                return Publish(kTargetPageMismatch);

            // Native initialization creates the complete UnitList before it
            // commits the final page-group state. The maximum values alone
            // are not a sufficient ready receipt for inventories whose final
            // maximum group is also the constructor default zero.
            if (!unitListItemByIndex_(
                    list,
                    inventoryCapacity - 1))
                return Publish(
                    kWaitingForListItems);
            if (static_cast<uint32_t>(page) !=
                targetPage)
                return NavigateToPage(
                    unitManager,
                    root,
                    static_cast<uint32_t>(page),
                    targetPage,
                    static_cast<uint32_t>(
                        currentGroup),
                    static_cast<uint32_t>(
                        maximumGroup),
                    static_cast<uint32_t>(
                        maximumPage));

            // The native page callback owns REQ_UNIT_SLOT_PAGE and its reply
            // cache. Reaching the requested page is only navigation evidence;
            // wait for the matching full record before selecting anything.
            alignas(4) uint8_t full[0x99]{};
            unitFullBySlot_(
                *unitInfoPointer_,
                full,
                targetSlot_);
            uint32_t fullId = 0;
            uint64_t fullUid = 0;
            std::memcpy(
                &fullId,
                full,
                sizeof(fullId));
            std::memcpy(
                &fullUid,
                full + 8,
                sizeof(fullUid));
            if (fullId != targetUnitId_ ||
                fullUid != desiredUid)
                return Publish(kWaitingForFullUnit);

            const uint32_t localIndex =
                (targetSlot_ - 1) %
                kUnitsPerPage;
            const uint8_t targetOccupied =
                managerBytes[
                    kUnitManagerOccupied +
                    localIndex];
            InterlockedExchange(
                &nativeTargetOccupied_,
                targetOccupied);
            if (targetOccupied == 0)
                return Publish(kInventoryInvalid);

            const int absoluteIndex =
                static_cast<int>(
                    targetSlot_ - 1);
            void* row =
                unitListItemByIndex_(
                    list,
                    absoluteIndex);
            InterlockedExchange(
                &nativeRowPointer_,
                PointerValue(row));
            InterlockedExchange(
                &nativeRowGetterReturn_,
                row ? 1 : 0);
            if (!row)
                return Publish(
                    kWaitingForListItems);
            InterlockedExchange(
                &nativeRowSelectedBefore_,
                *(
                    static_cast<uint8_t*>(
                        row) +
                    kListItemSelected));
            void* rowControl =
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(row) +
                    kListItemControl);
            InterlockedExchange(
                &nativeRowControlPointer_,
                PointerValue(rowControl));
            if (!rowControl)
                return Publish(
                    kNativeSelectionRejected);
            const float rowX =
                *reinterpret_cast<const float*>(
                    static_cast<uint8_t*>(
                        rowControl) +
                    kControlX);
            const float rowY =
                *reinterpret_cast<const float*>(
                    static_cast<uint8_t*>(
                        rowControl) +
                    kControlY);
            uint32_t rowXBits = 0;
            uint32_t rowYBits = 0;
            std::memcpy(
                &rowXBits,
                &rowX,
                sizeof(rowXBits));
            std::memcpy(
                &rowYBits,
                &rowY,
                sizeof(rowYBits));
            InterlockedExchange(
                &nativeRowXBits_,
                static_cast<LONG>(
                    rowXBits));
            InterlockedExchange(
                &nativeRowYBits_,
                static_cast<LONG>(
                    rowYBits));
            InterlockedExchange(
                &nativeRowXFinite_,
                std::isfinite(rowX) ? 1 : 0);
            InterlockedExchange(
                &nativeRowYFinite_,
                std::isfinite(rowY) ? 1 : 0);
            int32_t x = 0;
            int32_t y = 0;
            if (!ControlCoordinateToClick(
                    rowX,
                    &x) ||
                !ControlCoordinateToClick(
                    rowY,
                    &y))
                return Publish(
                    kNativeSelectionRejected);
            InterlockedExchange(
                &nativeClickX_,
                x);
            InterlockedExchange(
                &nativeClickY_,
                y);
            const uint8_t downAccepted =
                unitListMouseBusiness_(
                    list,
                    kMouseLeftDown,
                    x,
                    y,
                    0,
                    0);
            InterlockedExchange(
                &nativeMouseDownAccepted_,
                downAccepted ? 1 : 0);
            const uint8_t upAccepted =
                downAccepted
                    ? unitListMouseBusiness_(
                          list,
                          kMouseLeftUp,
                          x,
                          y,
                          0,
                          0)
                    : 0;
            InterlockedExchange(
                &nativeMouseUpAccepted_,
                upAccepted ? 1 : 0);
            bool selected =
                downAccepted != 0 &&
                upAccepted != 0;
            for (uint32_t check = 0;
                 selected &&
                 check < kUnitsPerPage;
                 ++check) {
                if (managerBytes[
                        kUnitManagerOccupied +
                        check] == 0)
                    continue;
                void* candidate =
                    unitListItemByIndex_(
                        list,
                        static_cast<int>(
                            page *
                                kUnitsPerPage +
                            check));
                const bool candidateSelected =
                    candidate &&
                    *(
                        static_cast<uint8_t*>(
                            candidate) +
                        kListItemSelected) == 1;
                if (candidateSelected !=
                    (check == localIndex))
                    selected = false;
            }
            InterlockedExchange(
                &nativeRowSelected_,
                selected ? 1 : 0);
            if (!selected)
                return Publish(
                    kNativeSelectionRejected);

            const NativeActions::
                TransportObservation before =
                    native_->TransportState();
            replyBaseline_ =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &replySequence_,
                        0,
                        0));
            InterlockedIncrement(
                &selectInvocations_);
            selectBusiness_(unitManager);
            const NativeActions::
                TransportObservation after =
                    native_->TransportState();

            InterlockedExchange(
                &requestTransportSequence_,
                static_cast<LONG>(
                    after.send_sequence));
            InterlockedExchange(
                &requestOpcode_,
                static_cast<LONG>(
                    after.opcode));
            InterlockedExchange(
                &requestSize_,
                static_cast<LONG>(
                    after.packet_size));
            InterlockedExchange(
                &requestResult_,
                static_cast<LONG>(
                    after.result));
            if (after.send_sequence ==
                    before.send_sequence ||
                after.opcode !=
                    kActiveWaitRequestOpcode ||
                after.packet_size !=
                    kActiveWaitRequestSize ||
                after.result == 0)
                return Publish(kRequestRejected);

            requestInvoked_ = true;
            InterlockedExchange(
                &requestInvokedObservation_,
                1);

            uint16_t postSlot = 0;
            uint64_t postUid = 0;
            if (ReadCurrentUnit(
                    &postSlot,
                    &postUid)) {
                PublishCurrent(
                    postSlot,
                    postUid);
                const Result reply =
                    EvaluateReply(
                        postSlot,
                        postUid);
                if (reply != kWaitingForReply)
                    return reply;
            }
            return Publish(kSelecting);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    void Reset()
    {
        if (bound_)
            ResetState();
    }

    Observation State() const
    {
        Observation state = {};
        state.result =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &lastResult_),
                    0,
                    0));
        state.native_app_state =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeAppState_),
                    0,
                    0));
        state.request_transport_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &requestTransportSequence_),
                    0,
                    0));
        state.reply_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &replySequence_),
                    0,
                    0));
        state.select_invocations =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &selectInvocations_),
                    0,
                    0));
        state.request_size =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &requestSize_),
                    0,
                    0));
        state.request_result =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &requestResult_),
                    0,
                    0));
        state.native_root_pointer =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeRootPointer_),
                    0,
                    0));
        state.native_list_pointer =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeListPointer_),
                    0,
                    0));
        state.native_row_pointer =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeRowPointer_),
                    0,
                    0));
        state.native_row_control_pointer =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeRowControlPointer_),
                    0,
                    0));
        state.native_row_x_bits =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeRowXBits_),
                    0,
                    0));
        state.native_row_y_bits =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeRowYBits_),
                    0,
                    0));
        state.inventory_capacity =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &inventoryCapacity_),
                    0,
                    0));
        state.native_click_x =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeClickX_),
                    0,
                    0));
        state.native_click_y =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeClickY_),
                    0,
                    0));
        state.native_list_pressed_index =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeListPressedIndex_),
                    0,
                    0));
        state.native_list_selected_index =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeListSelectedIndex_),
                    0,
                    0));
        state.inventory_bad_unit_id =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &inventoryBadUnitId_),
                    0,
                    0));
        state.target_unit_id = targetUnitId_;
        state.target_uid = targetUid_;
        state.inventory_bad_uid =
            static_cast<uint64_t>(
                InterlockedCompareExchange64(
                    const_cast<volatile LONG64*>(
                        &inventoryBadUid_),
                    0,
                    0));
        state.initial_uid = initialUid_;
        state.current_uid =
            static_cast<uint64_t>(
                InterlockedCompareExchange64(
                    const_cast<volatile LONG64*>(
                        &currentUid_),
                    0,
                    0));
        state.reply_current_uid =
            static_cast<uint64_t>(
                InterlockedCompareExchange64(
                    const_cast<volatile LONG64*>(
                        &replyCurrentUid_),
                    0,
                    0));
        state.request_opcode =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &requestOpcode_),
                    0,
                    0));
        state.reply_opcode =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &replyOpcode_),
                    0,
                    0));
        state.reply_status =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &replyStatus_),
                    0,
                    0));
        state.initial_slot = initialSlot_;
        state.target_slot = targetSlot_;
        state.current_slot =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &currentSlot_),
                    0,
                    0));
        state.reply_packet_slot =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &replyPacketSlot_),
                    0,
                    0));
        state.reply_current_slot =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &replyCurrentSlot_),
                    0,
                    0));
        state.unit_manager_page =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitManagerPage_),
                    0,
                    0));
        state.inventory_bad_slot =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &inventoryBadSlot_),
                    0,
                    0));
        state.inventory_bad_record_slot =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &inventoryBadRecordSlot_),
                    0,
                    0));
        state.inventory_ready =
            static_cast<uint8_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &inventoryReady_),
                    0,
                    0));
        state.unit_manager_present =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &unitManagerPresent_),
                0,
                0) != 0;
        state.unit_list_present =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &unitListPresent_),
                0,
                0) != 0;
        state.native_row_getter_return =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &nativeRowGetterReturn_),
                0,
                0) != 0;
        state.native_row_x_finite =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &nativeRowXFinite_),
                0,
                0) != 0;
        state.native_row_y_finite =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &nativeRowYFinite_),
                0,
                0) != 0;
        state.native_target_occupied =
            static_cast<uint8_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeTargetOccupied_),
                    0,
                    0));
        state.native_row_selected_before =
            static_cast<uint8_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &nativeRowSelectedBefore_),
                    0,
                    0));
        state.native_row_selected =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &nativeRowSelected_),
                0,
                0) != 0;
        state.native_mouse_down_accepted =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &nativeMouseDownAccepted_),
                0,
                0) != 0;
        state.native_mouse_up_accepted =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &nativeMouseUpAccepted_),
                0,
                0) != 0;
        state.request_invoked =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &requestInvokedObservation_),
                0,
                0) != 0;
        state.reply_consumed =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &replyConsumed_),
                0,
                0) != 0;
        state.target_matches =
            state.inventory_ready && state.target_slot != 0 && state.target_uid != 0 &&
                state.current_slot ==
                    state.target_slot &&
                state.current_uid ==
                    state.target_uid;
        state.unit_manager_page_raw =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitManagerPageRaw_),
                    0,
                    0));
        state.unit_manager_max_page_raw =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitManagerMaximumPageRaw_),
                    0,
                    0));
        state.unit_manager_page_group_raw =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitManagerPageGroupRaw_),
                    0,
                    0));
        state.unit_manager_max_page_group_raw =
            static_cast<int32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitManagerMaximumPageGroupRaw_),
                    0,
                    0));
        state.target_page =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &targetPageObservation_),
                    0,
                    0));
        return state;
    }

    static const char* Name(Result result)
    {
        switch (result) {
        case kUnbound:
            return "unbound";
        case kWaitingForHangar:
            return "waiting-for-hangar";
        case kWaitingForUnitManager:
            return "waiting-for-unit-manager";
        case kWaitingForInventory:
            return "waiting-for-inventory";
        case kSelecting:
            return "selecting";
        case kWaitingForReply:
            return "waiting-for-reply";
        case kComplete:
            return "complete";
        case kAlreadySelected:
            return "already-selected";
        case kWaitingForListItems:
            return "waiting-for-list-items";
        case kWaitingForFullUnit:
            return "waiting-for-full-unit";
        case kNavigatingUnitPage:
            return "navigating-unit-page";
        case kWaitingForUnitPageState:
            return "waiting-for-unit-page-state";
        case kInvalidTarget:
            return "invalid-target";
        case kUnitManagerMismatch:
            return "unit-manager-mismatch";
        case kUnitListMissing:
            return "unit-list-missing";
        case kUnitListMismatch:
            return "unit-list-mismatch";
        case kTargetMissing:
            return "target-missing";
        case kTargetDuplicate:
            return "target-duplicate";
        case kTargetPageMismatch:
            return "target-page-mismatch";
        case kNativeSelectionRejected:
            return "native-selection-rejected";
        case kRequestRejected:
            return "request-rejected";
        case kReplyRejected:
            return "reply-rejected";
        case kReplyMismatch:
            return "reply-mismatch";
        case kInventoryInvalid:
            return "inventory-invalid";
        case kNativeFault:
            return "native-fault";
        case kPageControlMissing:
            return "page-control-missing";
        case kPageControlMismatch:
            return "page-control-mismatch";
        default:
            return "unknown";
        }
    }

private:
    enum ServiceResult
    {
        kServiceAbsent,
        kServiceMatched,
        kServiceMismatched,
    };

    enum InventoryResult
    {
        kInventoryReady,
        kInventoryWaiting,
        kInventoryMalformed,
        kInventoryTargetMissing,
        kInventoryTargetDuplicate,
    };

    static constexpr uint32_t kHangarAppState = 5;
    static constexpr uint16_t
        kActiveWaitRequestOpcode = 0x04CF;
    static constexpr uint32_t
        kActiveWaitRequestSize = 14;
    static constexpr uint16_t
        kActiveWaitReplyOpcode = 0x09B4;
    static constexpr uint32_t kUnitsPerPage = 6;
    static constexpr uint32_t kPagesPerGroup = 5;
    static constexpr uint32_t
        kMaxInventorySlots = UINT16_MAX;
    static constexpr uint32_t
        kListControlKind = 7;
    static constexpr uint32_t
        kButtonControlKind = 4;
    static constexpr uint32_t
        kClickEvent = 0x101;
    static constexpr size_t
        kBriefUnitSize = 0x19;
    static constexpr size_t
        kBriefUnitId = 0;
    static constexpr size_t
        kBriefUnitUid = 4;
    static constexpr size_t
        kBriefUnitSlot = 12;
    static constexpr size_t
        kProfileActiveSlot = 0x1AD;
    static constexpr size_t
        kUnitInfoCapacity = 0x0C;
    static constexpr size_t
        kUnitInfoReady = 0x20;
    static constexpr size_t
        kUnitManagerRoot = 0x64;
    static constexpr size_t
        kUnitManagerPageGroup = 0x6C;
    static constexpr size_t
        kUnitManagerMaximumPageGroup = 0x70;
    static constexpr size_t
        kUnitManagerPage = 0x74;
    static constexpr size_t
        kUnitManagerMaximumPage = 0x78;
    static constexpr size_t
        kUnitManagerOccupied = 0xD4;
    static constexpr size_t
        kControlName = 0x118;
    static constexpr size_t
        kControlParent = 0x15C;
    static constexpr size_t
        kControlVisible = 0x162;
    static constexpr size_t
        kControlEnabled = 0x167;
    static constexpr size_t
        kControlX = 0x148;
    static constexpr size_t
        kControlY = 0x14C;
    static constexpr size_t
        kListPressedIndex = 0x220;
    static constexpr size_t
        kListSelectedIndex = 0x224;
    static constexpr size_t
        kListItemControl = 0x1C;
    static constexpr size_t
        kListItemSelected = 0x25;
    static constexpr uint32_t
        kMouseLeftDown = 0x201;
    static constexpr uint32_t
        kMouseLeftUp = 0x202;
    static constexpr size_t
        kNativeWStringSize = 0x1C;
    static constexpr size_t
        kServiceDescriptorSize =
            sizeof(uint32_t) +
            kNativeWStringSize;
    static constexpr size_t
        kNativeStringSize = 0x10;
    static constexpr size_t
        kNativeStringCapacity = 0x14;
    static constexpr size_t
        kReplyStatus = 6;
    static constexpr size_t
        kReplyProfile = 8;
    static constexpr size_t
        kReplyActiveSlot =
            kReplyProfile +
            (kProfileActiveSlot - 0x1A9);

    using UnitBriefBySlotFn =
        void(__thiscall*)(
            void*,
            void*,
            uint16_t);
    using ServiceFindByNameFn =
        void*(__thiscall*)(
            void*,
            const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(
            void*,
            const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(
            void*,
            const void*,
            uint32_t);
    using UiCallbackFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
    using UnitListMouseBusinessFn =
        uint8_t(__thiscall*)(
            void*,
            uint32_t,
            int32_t,
            int32_t,
            uint32_t,
            uint32_t);
    using UnitListItemByIndexFn =
        void*(__thiscall*)(void*, int);
    using SelectBusinessFn =
        uint32_t(__thiscall*)(void*);
    using ActiveWaitReplyFn =
        void(__thiscall*)(
            void*,
            const void*);
    using ActiveWaitReplyDetourFn =
        void(__fastcall*)(
            void*,
            void*,
            const void*);

    static bool CallbackSlotMatches(
        void* vtable,
        UiCallbackFn callback)
    {
        return vtable && callback &&
               static_cast<void**>(vtable)
                       [0x38 / sizeof(void*)] ==
                   reinterpret_cast<void*>(callback);
    }

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    Result Publish(Result result)
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        return result;
    }

    Result NavigateToPage(
        void* unitManager,
        void* root,
        uint32_t currentPage,
        uint32_t targetPage,
        uint32_t currentGroup,
        uint32_t maximumGroup,
        uint32_t maximumPage)
    {
        if (!unitManager || !root ||
            !unitManagerCallback_)
            return Publish(kNativeFault);
        auto* managerBytes =
            static_cast<uint8_t*>(unitManager);
        const uint32_t targetGroup =
            targetPage /
            kPagesPerGroup;
        if (currentPage > maximumPage ||
            targetPage > maximumPage ||
            currentGroup > maximumGroup ||
            targetGroup > maximumGroup)
            return Publish(kTargetPageMismatch);

        const wchar_t* dispatchName = nullptr;
        const wchar_t* anchorName = nullptr;
        if (targetGroup < currentGroup) {
            dispatchName = L"LeftBtn";
            anchorName = dispatchName;
        } else if (targetGroup > currentGroup) {
            dispatchName = L"RightBtn";
            anchorName = dispatchName;
        } else if (targetPage < currentPage) {
            dispatchName = L"LeftOneBtn";
            anchorName = L"LeftBtn";
        } else if (targetPage > currentPage) {
            dispatchName = L"RightOneBtn";
            anchorName = L"RightBtn";
        } else {
            return Publish(kWaitingForFullUnit);
        }

        void* control =
            FindControl(
                root,
                anchorName,
                kButtonControlKind);
        if (!control)
            return Publish(kPageControlMissing);
        auto* controlBytes =
            static_cast<uint8_t*>(control);
        if (*reinterpret_cast<void**>(
                controlBytes +
                kControlParent) != root ||
            !ControlHasName(
                control,
                anchorName))
            return Publish(kPageControlMismatch);

        // This is the shipped stdcall callback at unitmgr vtable +0x38.
        // The current shipped UIR exposes the root-owned group arrows but not
        // the legacy one-page arrow controls. The callback ABI validates the
        // fourth-argument control's root, then dispatches the independent
        // third-argument native name through the original business method.
        // Use a real directional button as that validated anchor and construct
        // only the retained one-page dispatch name. No control or page state is
        // synthesized. The callback return is always zero and is not evidence
        // of completion.
        alignas(4) uint8_t nativeDispatchName[
            kNativeWStringSize] = {};
        bool dispatchNameConstructed = false;
        const void* nativeName =
            controlBytes +
            kControlName;
        __try {
            if (std::wcscmp(
                    dispatchName,
                    anchorName) != 0) {
                dispatchNameConstructed =
                    nativeWStringCtor_(
                        nativeDispatchName,
                        dispatchName) != nullptr;
                if (!dispatchNameConstructed)
                    return Publish(kNativeFault);
                nativeName =
                    nativeDispatchName;
            }
            unitManagerCallback_(
                unitManager,
                kClickEvent,
                nativeName,
                control);
        } __finally {
            if (dispatchNameConstructed)
                nativeWStringDtor_(
                    nativeDispatchName);
        }
        const int32_t observedPage =
            *reinterpret_cast<const int32_t*>(
                managerBytes +
                kUnitManagerPage);
        if (observedPage >= 0 &&
            observedPage <=
                static_cast<int32_t>(
                    UINT16_MAX)) {
            InterlockedExchange(
                &unitManagerPage_,
                observedPage);
        }
        return Publish(kNavigatingUnitPage);
    }

    static bool ControlCoordinateToClick(
        float coordinate,
        int32_t* click)
    {
        if (!click ||
            !std::isfinite(coordinate))
            return false;
        const double value =
            static_cast<double>(coordinate);
        const double minimum =
            static_cast<double>(
                (std::numeric_limits<
                    int32_t>::min)());
        const double maximumBeforeInset =
            static_cast<double>(
                (std::numeric_limits<
                    int32_t>::max)()) -
            1.0;
        if (value < minimum ||
            value > maximumBeforeInset)
            return false;
        *click =
            static_cast<int32_t>(
                coordinate) +
            1;
        return true;
    }

    static LONG PointerValue(
        const void* pointer)
    {
        return static_cast<LONG>(
            static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(
                    pointer)));
    }

    Result EvaluateReply(
        uint16_t activeSlot,
        uint64_t activeUid)
    {
        const uint32_t sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &replySequence_,
                    0,
                    0));
        if (sequence <= replyBaseline_)
            return Publish(kWaitingForReply);
        if (static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &replyOpcode_,
                    0,
                    0)) !=
                kActiveWaitReplyOpcode ||
            InterlockedCompareExchange(
                &replyConsumed_,
                0,
                0) == 0)
            return Publish(kReplyMismatch);
        if (static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &replyStatus_,
                    0,
                    0)) != 0)
            return Publish(kReplyRejected);
        if (static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &replyPacketSlot_,
                    0,
                    0)) != targetSlot_ ||
            activeSlot != targetSlot_ ||
            activeUid != targetUid_)
            return Publish(kReplyMismatch);
        return Publish(kComplete);
    }

    InventoryResult ResolveInventory(
        uint64_t desiredUid)
    {
        auto* info =
            unitInfoPointer_
                ? static_cast<uint8_t*>(
                      *unitInfoPointer_)
                : nullptr;
        const uint8_t ready =
            info ? info[kUnitInfoReady] : 0;
        InterlockedExchange(
            &inventoryReady_,
            ready);
        if (!info || ready != 1)
            return kInventoryWaiting;

        const int32_t capacity =
            *reinterpret_cast<const int32_t*>(
                info +
                kUnitInfoCapacity);
        InterlockedExchange(
            &inventoryCapacity_,
            capacity);
        if (capacity <= 0 ||
            capacity >
                static_cast<int32_t>(
                    kMaxInventorySlots))
            return kInventoryMalformed;

        uint32_t matches = 0;
        uint16_t matchedSlot = 0;
        uint32_t matchedId = 0;
        for (int32_t index = 0;
             index < capacity;
             ++index) {
            alignas(4) uint8_t brief[
                kBriefUnitSize] = {};
            unitBriefBySlot_(
                info,
                brief,
                static_cast<uint16_t>(
                    index + 1));
            uint32_t unitId = 0;
            uint64_t uid = 0;
            uint16_t slot = 0;
            std::memcpy(
                &unitId,
                brief +
                    kBriefUnitId,
                sizeof(unitId));
            std::memcpy(
                &uid,
                brief +
                    kBriefUnitUid,
                sizeof(uid));
            std::memcpy(
                &slot,
                brief +
                    kBriefUnitSlot,
                sizeof(slot));
            const uint16_t querySlot =
                static_cast<uint16_t>(
                    index + 1);
            // GetBrief clears all 0x19 bytes when the slot has no backing
            // record. A preallocated empty record retains its own slot while
            // both identity fields remain zero. These are the two native
            // empty encodings observed by the read-only inventory paths.
            if (!unitId && !uid &&
                (!slot || slot == querySlot))
                continue;
            if (!unitId || !uid ||
                slot != querySlot) {
                InterlockedExchange(
                    &inventoryBadSlot_,
                    querySlot);
                InterlockedExchange(
                    &inventoryBadUnitId_,
                    static_cast<LONG>(
                        unitId));
                InterlockedExchange64(
                    &inventoryBadUid_,
                    static_cast<LONG64>(
                        uid));
                InterlockedExchange(
                    &inventoryBadRecordSlot_,
                    slot);
                return kInventoryMalformed;
            }
            if (uid != desiredUid)
                continue;
            ++matches;
            matchedSlot = slot;
            matchedId = unitId;
        }
        if (!matches)
            return kInventoryTargetMissing;
        if (matches != 1)
            return kInventoryTargetDuplicate;
        targetSlot_ = matchedSlot;
        targetUnitId_ = matchedId;
        return kInventoryReady;
    }

    bool ReadCurrentUnit(
        uint16_t* slot,
        uint64_t* uid) const
    {
        if (slot)
            *slot = 0;
        if (uid)
            *uid = 0;
        if (!slot || !uid ||
            !profilePointer_ ||
            !unitInfoPointer_ ||
            !unitBriefBySlot_)
            return false;
        auto* profile = *profilePointer_;
        auto* info = static_cast<uint8_t*>(
            *unitInfoPointer_);
        if (!profile || !info ||
            info[kUnitInfoReady] != 1)
            return false;
        const uint16_t activeSlot =
            *reinterpret_cast<const uint16_t*>(
                profile +
                kProfileActiveSlot);
        const int32_t capacity =
            *reinterpret_cast<const int32_t*>(
                info +
                kUnitInfoCapacity);
        if (!activeSlot ||
            activeSlot > capacity)
            return false;

        alignas(4) uint8_t brief[
            kBriefUnitSize] = {};
        unitBriefBySlot_(
            info,
            brief,
            activeSlot);
        uint32_t unitId = 0;
        uint64_t currentUid = 0;
        uint16_t briefSlot = 0;
        std::memcpy(
            &unitId,
            brief +
                kBriefUnitId,
            sizeof(unitId));
        std::memcpy(
            &currentUid,
            brief +
                kBriefUnitUid,
            sizeof(currentUid));
        std::memcpy(
            &briefSlot,
            brief +
                kBriefUnitSlot,
            sizeof(briefSlot));
        if (!unitId || !currentUid ||
            briefSlot != activeSlot)
            return false;
        *slot = activeSlot;
        *uid = currentUid;
        return true;
    }

    void PublishCurrent(
        uint16_t slot,
        uint64_t uid)
    {
        InterlockedExchange(
            &currentSlot_,
            slot);
        InterlockedExchange64(
            &currentUid_,
            static_cast<LONG64>(uid));
    }

    ServiceResult ResolveService(
        const wchar_t* name,
        void* expectedVtable,
        void** output) const
    {
        if (output)
            *output = nullptr;
        if (!name || !expectedVtable ||
            !output ||
            !serviceManagedPointer_ ||
            !serviceFindByName_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return kServiceAbsent;

        void* manager =
            *serviceManagedPointer_;
        if (!manager)
            return kServiceAbsent;

        alignas(4) uint8_t descriptor[
            kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(
            descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    descriptor +
                        sizeof(uint32_t),
                    name) != nullptr;
            if (constructed) {
                service =
                    serviceFindByName_(
                        manager,
                        descriptor);
            }
        } __finally {
            if (constructed) {
                nativeWStringDtor_(
                    descriptor +
                        sizeof(uint32_t));
            }
        }
        if (!service)
            return kServiceAbsent;
        *output = service;
        return *reinterpret_cast<void**>(
                   service) ==
                       expectedVtable
                   ? kServiceMatched
                   : kServiceMismatched;
    }

    void* FindControl(
        void* root,
        const wchar_t* name,
        uint32_t kind) const
    {
        if (!root || !name ||
            !nativeUiFindControl_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;

        alignas(4) uint8_t nativeName[
            kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (constructed) {
                control =
                    nativeUiFindControl_(
                        root,
                        nativeName,
                        kind);
            }
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    nativeName);
        }
        if (!control)
            return nullptr;
        const auto* bytes =
            static_cast<const uint8_t*>(
                control);
        if (bytes[kControlVisible] == 0 ||
            bytes[kControlEnabled] == 0)
            return nullptr;
        return control;
    }

    static bool ControlHasName(
        const void* control,
        const wchar_t* expected)
    {
        if (!control || !expected)
            return false;
        const auto* nativeName =
            static_cast<const uint8_t*>(
                control) +
            kControlName;
        const uint32_t size =
            *reinterpret_cast<
                const uint32_t*>(
                    nativeName +
                    kNativeStringSize);
        const uint32_t capacity =
            *reinterpret_cast<
                const uint32_t*>(
                    nativeName +
                    kNativeStringCapacity);
        const size_t expectedSize =
            std::wcslen(expected);
        if (size != expectedSize ||
            size > 63)
            return false;
        const wchar_t* value =
            capacity < 8
                ? reinterpret_cast<
                      const wchar_t*>(
                      nativeName)
                : *reinterpret_cast<
                      wchar_t* const*>(
                      nativeName);
        return value &&
               std::wmemcmp(
                   value,
                   expected,
                   size) == 0;
    }

    static void __fastcall
    ObserveActiveWaitReply(
        void* self,
        void*,
        const void* packet)
    {
        UnitSelectActions* owner = active_;
        uint16_t opcode = 0;
        uint16_t status = UINT16_MAX;
        uint16_t packetSlot = 0;
        bool observed = false;
        if (owner && packet) {
            __try {
                const auto* bytes =
                    static_cast<
                        const uint8_t*>(
                        packet);
                std::memcpy(
                    &opcode,
                    bytes,
                    sizeof(opcode));
                std::memcpy(
                    &status,
                    bytes +
                        kReplyStatus,
                    sizeof(status));
                std::memcpy(
                    &packetSlot,
                    bytes +
                        kReplyActiveSlot,
                    sizeof(packetSlot));
                observed =
                    opcode ==
                    kActiveWaitReplyOpcode;
            } __except (
                EXCEPTION_EXECUTE_HANDLER) {
                observed = false;
            }
        }

        if (owner && owner->activeWaitReply_) {
            owner->activeWaitReply_(
                self,
                packet);
        }
        if (!owner || !observed)
            return;

        uint16_t currentSlot = 0;
        uint64_t currentUid = 0;
        bool currentValid = false;
        __try {
            currentValid =
                owner->ReadCurrentUnit(
                    &currentSlot,
                    &currentUid);
        } __except (
            EXCEPTION_EXECUTE_HANDLER) {
            currentValid = false;
        }

        InterlockedExchange(
            &owner->replyOpcode_,
            opcode);
        InterlockedExchange(
            &owner->replyStatus_,
            status);
        InterlockedExchange(
            &owner->replyPacketSlot_,
            packetSlot);
        InterlockedExchange(
            &owner->replyCurrentSlot_,
            currentValid ?
                currentSlot : 0);
        InterlockedExchange64(
            &owner->replyCurrentUid_,
            currentValid ?
                static_cast<LONG64>(
                    currentUid) :
                0);
        InterlockedExchange(
            &owner->replyConsumed_,
            1);
        InterlockedIncrement(
            &owner->replySequence_);
    }

    void ResetState()
    {
        targetInitialized_ = false;
        targetUid_ = 0;
        ResetAttempt();
        InterlockedExchange(
            &replySequence_,
            0);
        InterlockedExchange(
            &replyOpcode_,
            0);
        InterlockedExchange(
            &replyStatus_,
            0);
        InterlockedExchange(
            &replyPacketSlot_,
            0);
        InterlockedExchange(
            &replyCurrentSlot_,
            0);
        InterlockedExchange64(
            &replyCurrentUid_,
            0);
        InterlockedExchange(
            &replyConsumed_,
            0);
        InterlockedExchange(
            &lastResult_,
            kWaitingForHangar);
    }

    void ResetAttempt()
    {
        requestInvoked_ = false;
        replyBaseline_ =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &replySequence_,
                    0,
                    0));
        targetUnitId_ = 0;
        targetSlot_ = 0;
        initialCaptured_ = false;
        initialSlot_ = 0;
        initialUid_ = 0;
        InterlockedExchange(
            &nativeAppState_,
            UINT32_MAX);
        InterlockedExchange(
            &requestTransportSequence_,
            0);
        InterlockedExchange(
            &selectInvocations_,
            0);
        InterlockedExchange(
            &requestOpcode_,
            0);
        InterlockedExchange(
            &requestSize_,
            0);
        InterlockedExchange(
            &requestResult_,
            0);
        InterlockedExchange(
            &nativeRootPointer_,
            0);
        InterlockedExchange(
            &nativeListPointer_,
            0);
        InterlockedExchange(
            &nativeRowPointer_,
            0);
        InterlockedExchange(
            &nativeRowControlPointer_,
            0);
        InterlockedExchange(
            &nativeRowXBits_,
            0);
        InterlockedExchange(
            &nativeRowYBits_,
            0);
        InterlockedExchange(
            &inventoryCapacity_,
            0);
        InterlockedExchange(
            &inventoryBadSlot_,
            0);
        InterlockedExchange(
            &inventoryBadUnitId_,
            0);
        InterlockedExchange64(
            &inventoryBadUid_,
            0);
        InterlockedExchange(
            &inventoryBadRecordSlot_,
            0);
        InterlockedExchange(
            &inventoryReady_,
            0);
        InterlockedExchange(
            &currentSlot_,
            0);
        InterlockedExchange64(
            &currentUid_,
            0);
        InterlockedExchange(
            &unitManagerPage_,
            0);
        InterlockedExchange(
            &unitManagerPageRaw_,
            0);
        InterlockedExchange(
            &unitManagerMaximumPageRaw_,
            0);
        InterlockedExchange(
            &unitManagerPageGroupRaw_,
            0);
        InterlockedExchange(
            &unitManagerMaximumPageGroupRaw_,
            0);
        InterlockedExchange(
            &targetPageObservation_,
            0);
        InterlockedExchange(
            &unitManagerPresent_,
            0);
        InterlockedExchange(
            &unitListPresent_,
            0);
        InterlockedExchange(
            &nativeRowSelected_,
            0);
        InterlockedExchange(
            &nativeClickX_,
            0);
        InterlockedExchange(
            &nativeClickY_,
            0);
        InterlockedExchange(
            &nativeListPressedIndex_,
            -1);
        InterlockedExchange(
            &nativeListSelectedIndex_,
            -1);
        InterlockedExchange(
            &nativeRowGetterReturn_,
            0);
        InterlockedExchange(
            &nativeRowXFinite_,
            0);
        InterlockedExchange(
            &nativeRowYFinite_,
            0);
        InterlockedExchange(
            &nativeTargetOccupied_,
            0);
        InterlockedExchange(
            &nativeRowSelectedBefore_,
            0);
        InterlockedExchange(
            &nativeMouseDownAccepted_,
            0);
        InterlockedExchange(
            &nativeMouseUpAccepted_,
            0);
        InterlockedExchange(
            &requestInvokedObservation_,
            0);
        InterlockedExchange(
            &replyConsumed_,
            0);
    }

    inline static UnitSelectActions* active_ = nullptr;

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    void** unitInfoPointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    UnitBriefBySlotFn unitBriefBySlot_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    UnitListMouseBusinessFn unitListMouseBusiness_ = nullptr;
    UnitListItemByIndexFn unitListItemByIndex_ = nullptr;
    UnitBriefBySlotFn unitFullBySlot_ = nullptr;
    SelectBusinessFn selectBusiness_ = nullptr;
    ActiveWaitReplyFn activeWaitReply_ = nullptr;
    UiCallbackFn unitManagerCallback_ = nullptr;
    void* unitManagerVtable_ = nullptr;

    bool bound_ = false;
    bool targetInitialized_ = false;
    bool requestInvoked_ = false;
    bool initialCaptured_ = false;
    uint32_t replyBaseline_ = 0;
    uint32_t targetUnitId_ = 0;
    uint64_t targetUid_ = 0;
    uint64_t initialUid_ = 0;
    uint16_t targetSlot_ = 0;
    uint16_t initialSlot_ = 0;

    volatile LONG lastResult_ = kUnbound;
    volatile LONG nativeAppState_ = UINT32_MAX;
    volatile LONG requestTransportSequence_ = 0;
    volatile LONG replySequence_ = 0;
    volatile LONG selectInvocations_ = 0;
    volatile LONG requestOpcode_ = 0;
    volatile LONG requestSize_ = 0;
    volatile LONG requestResult_ = 0;
    volatile LONG nativeRootPointer_ = 0;
    volatile LONG nativeListPointer_ = 0;
    volatile LONG nativeRowPointer_ = 0;
    volatile LONG nativeRowControlPointer_ = 0;
    volatile LONG nativeRowXBits_ = 0;
    volatile LONG nativeRowYBits_ = 0;
    volatile LONG inventoryCapacity_ = 0;
    volatile LONG inventoryBadSlot_ = 0;
    volatile LONG inventoryBadUnitId_ = 0;
    alignas(8) volatile LONG64
        inventoryBadUid_ = 0;
    volatile LONG inventoryBadRecordSlot_ = 0;
    volatile LONG inventoryReady_ = 0;
    volatile LONG replyOpcode_ = 0;
    volatile LONG replyStatus_ = 0;
    volatile LONG currentSlot_ = 0;
    volatile LONG replyPacketSlot_ = 0;
    volatile LONG replyCurrentSlot_ = 0;
    volatile LONG unitManagerPage_ = 0;
    volatile LONG unitManagerPageRaw_ = 0;
    volatile LONG unitManagerMaximumPageRaw_ = 0;
    volatile LONG unitManagerPageGroupRaw_ = 0;
    volatile LONG unitManagerMaximumPageGroupRaw_ = 0;
    volatile LONG targetPageObservation_ = 0;
    volatile LONG unitManagerPresent_ = 0;
    volatile LONG unitListPresent_ = 0;
    volatile LONG nativeRowSelected_ = 0;
    volatile LONG nativeClickX_ = 0;
    volatile LONG nativeClickY_ = 0;
    volatile LONG nativeListPressedIndex_ = -1;
    volatile LONG nativeListSelectedIndex_ = -1;
    volatile LONG nativeRowGetterReturn_ = 0;
    volatile LONG nativeRowXFinite_ = 0;
    volatile LONG nativeRowYFinite_ = 0;
    volatile LONG nativeTargetOccupied_ = 0;
    volatile LONG nativeRowSelectedBefore_ = 0;
    volatile LONG nativeMouseDownAccepted_ = 0;
    volatile LONG nativeMouseUpAccepted_ = 0;
    volatile LONG requestInvokedObservation_ = 0;
    volatile LONG replyConsumed_ = 0;
    alignas(8) volatile LONG64 currentUid_ = 0;
    alignas(8) volatile LONG64 replyCurrentUid_ = 0;
};
