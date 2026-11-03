#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Drives one collection-capsule draw through the shipped capsule UI.
//
// EnsurePreview is non-purchasing: it selects the activity machine, pages to
// machine 990001, selects its native tile, opens the original confirmation
// dialog, and waits for the live clone to finish native initialization.
// DrawOnce is the only purchasing entry point. It invokes the activity
// dialog's real BuyBtn callback at most once and accepts the action only when
// NativeActions observes the original 10-byte 0x05D3 item-machine transport.
//
// Server replies remain owned by the original "buying" service. The detour
// snapshots only the 0x04E1 result status and 0x09C6 acknowledgement fields
// before the original handler consumes the packet, calls that handler first,
// and publishes the observation afterwards. CloseResult uses the shipped
// CapsuleClick 0x0801 callback only after the genuine result control is visible.
class CapsuleDrawActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForMachine = 1,
        kWaitingForMachineUi = 2,
        kOpeningActivity = 3,
        kPagingActivity = 4,
        kSelectingTarget = 5,
        kOpeningPreview = 6,
        kWaitingForPreview = 7,
        kPreviewReady = 8,
        kBuySubmitted = 9,
        kWaitingForReply = 10,
        kReplyAccepted = 11,
        kWaitingForResult = 12,
        kResultReady = 13,
        kClosingResult = 14,
        kComplete = 15,

        kMachineServiceMismatch = 0x100,
        kMachineControlMismatch = 0x101,
        kActivityDataMismatch = 0x102,
        kTargetMachineMismatch = 0x103,
        kNativeActionRejected = 0x104,
        kDialogMismatch = 0x105,
        kPreviewNotReady = 0x106,
        kBuyAlreadyAttempted = 0x107,
        kBuyTransportRejected = 0x108,
        kServerRejected = 0x109,
        kResultServiceMismatch = 0x10A,
        kResultControlMismatch = 0x10B,
        kResultCloseRejected = 0x10C,
        kNativeFault = 0x10D,
    };

    struct Target
    {
        uint32_t machine = 990001;
        uint32_t price = 100000;
        uint32_t currency = 0;
    };

    bool ConfigureTarget(const Target& target)
    {
        if(bound_ || !target.machine || target.machine>0x7fffffff ||
           !target.price || target.price>0x7fffffff || target.currency>2)return false;
        expectedMachine_=target.machine;expectedPrice_=target.price;expectedCurrency_=target.currency;
        return true;
    }
    Target ExpectedTarget() const {return {expectedMachine_,expectedPrice_,expectedCurrency_};}

    struct Observation
    {
        uint32_t result;
        uint32_t target_machine;
        uint32_t target_price;
        uint32_t target_index;
        uint32_t target_page;
        uint32_t target_slot;
        uint32_t activity_count;
        uint32_t activity_slots;
        uint32_t current_page;
        uint32_t dialog_clone_sequence;
        uint32_t request_transport_sequence;
        uint32_t request_size;
        uint32_t request_result;
        uint32_t result_reply_sequence;
        uint32_t acknowledgement_sequence;
        uint64_t dropbox_serial;
        uint32_t reward_item_category;
        uint32_t reward_amount;
        uint32_t remaining_gp;
        uint16_t request_opcode;
        uint16_t result_reply_opcode;
        uint16_t result_reply_status;
        uint16_t acknowledgement_opcode;
        uint8_t activity_selected;
        uint8_t target_selected;
        uint8_t preview_ready;
        uint8_t buy_attempted;
        uint8_t buy_submitted;
        uint8_t result_ready;
        uint8_t close_attempted;
        uint8_t original_reply_completed;
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        if (bound_)
            return false;
        api_ = api;
        mod_ = mod;
        native_ = native;

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

        machineVtable_ =
            Symbol<void*>("flow.capsule_machine_vtable");
        machineCallback_ =
            Symbol<UiCallbackFn>(
                "flow.capsule_machine_callback");
        dialogVtable_ =
            Symbol<void*>(
                "flow.capsule_activity_dialog_vtable");
        dialogClone_ =
            Symbol<DialogCloneFn>(
                "flow.capsule_activity_dialog_clone");
        buyAction_ =
            Symbol<UiCallbackFn>(
                "flow.capsule_activity_buy_action");
        replyDispatch_ =
            Symbol<ReplyDispatchFn>(
                "flow.capsule_reply_dispatch");
        buyingVtable_ =
            Symbol<void*>("flow.capsule_buying_vtable");
        resultCallback_ =
            Symbol<UiCallbackFn>(
                "flow.capsule_result_callback");

        if (!api_ || !api_->HookCode || !mod_ || !native_ ||
            !serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_ ||
            !nativeUiFindControl_ || !machineVtable_ ||
            !machineCallback_ || !dialogVtable_ ||
            !dialogClone_ || !buyAction_ || !replyDispatch_ ||
            !buyingVtable_ || !resultCallback_)
            return false;

        __try {
            if (!SlotMatches(
                    machineVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        machineCallback_)) ||
                !SlotMatches(
                    dialogVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        buyAction_)) ||
                !SlotMatches(
                    dialogVtable_,
                    kCloneVtableSlot,
                    reinterpret_cast<void*>(
                        dialogClone_)) ||
                !SlotMatches(
                    buyingVtable_,
                    kReplyVtableSlot,
                    reinterpret_cast<void*>(
                        replyDispatch_)) ||
                !SlotMatches(
                    buyingVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        resultCallback_)))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (active_ && active_ != this)
            return false;
        active_ = this;

        if (!api_->HookCode(
                mod_,
                "flow.capsule_activity_dialog_clone",
                reinterpret_cast<void*>(
                    &ObserveDialogClone),
                reinterpret_cast<void**>(
                    &dialogClone_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.capsule_reply_dispatch",
                reinterpret_cast<void*>(
                    &ObserveReplyDispatch),
                reinterpret_cast<void**>(
                    &replyDispatch_))) {
            active_ = nullptr;
            return false;
        }

        InterlockedExchangePointer(&liveDialog_, nullptr);
        InterlockedExchange(&lastResult_, kWaitingForMachine);
        InterlockedExchange(&previewArmed_, 0);
        InterlockedExchange(&dropInvoked_, 0);
        InterlockedExchange(&dialogCloneSequence_, 0);
        InterlockedExchange(&previewReady_, 0);
        InterlockedExchange(&buyAttempted_, 0);
        InterlockedExchange(&buySubmitted_, 0);
        InterlockedExchange(&closeAttempted_, 0);
        InterlockedExchange(&activitySelected_, 0);
        InterlockedExchange(&targetSelected_, 0);
        InterlockedExchange(&resultReady_, 0);
        InterlockedExchange(&requestTransportSequence_, 0);
        InterlockedExchange(&requestOpcode_, 0);
        InterlockedExchange(&requestSize_, 0);
        InterlockedExchange(&requestResult_, 0);
        InterlockedExchange(&resultReplySequence_, 0);
        InterlockedExchange(&resultReplyOpcode_, 0);
        InterlockedExchange(&resultReplyStatus_, 0);
        InterlockedExchange(&ackSequence_, 0);
        InterlockedExchange(&ackOpcode_, 0);
        InterlockedExchange(&dropBoxSerialLow_, 0);
        InterlockedExchange(&dropBoxSerialHigh_, 0);
        InterlockedExchange(&rewardItemCategory_, 0);
        InterlockedExchange(&rewardAmount_, 0);
        InterlockedExchange(&remainingGp_, 0);
        InterlockedExchange(&originalReplyCompleted_, 0);
        bound_ = true;
        return true;
    }

    // Non-purchasing preparation. At most one original UI action is invoked
    // per call, so the runner can wait for native page/dialog initialization.
    Result EnsurePreview()
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (InterlockedCompareExchange(
                &buyAttempted_, 0, 0) != 0)
            return Publish(
                InterlockedCompareExchange(
                    &buySubmitted_, 0, 0)
                    ? kBuySubmitted
                    : kBuyAlreadyAttempted);

        __try {
            void* machine = nullptr;
            const ServiceResult service =
                ResolveService(
                    L"roommgr",
                    machineVtable_,
                    &machine);
            if (service == kServiceAbsent)
                return Publish(kWaitingForMachine);
            if (service == kServiceMismatched)
                return Publish(
                    kMachineServiceMismatch);

            auto* bytes =
                static_cast<uint8_t*>(machine);
            void* root =
                *reinterpret_cast<void**>(
                    bytes + kMachineRoot);
            if (!root)
                return Publish(
                    kWaitingForMachineUi);

            TargetScan target = {};
            const ScanResult scan =
                ScanTarget(bytes, target);
            if (scan == kScanWaiting)
                return Publish(
                    kWaitingForMachineUi);
            if (scan == kScanInvalid)
                return Publish(
                    kActivityDataMismatch);
            if (scan == kScanTargetMismatch)
                return Publish(
                    kTargetMachineMismatch);
            PublishTarget(target);

            if (*reinterpret_cast<uint32_t*>(
                    bytes + kMachineType) !=
                kActivityMachineType) {
                void* radio =
                    *reinterpret_cast<void**>(
                        bytes + kActivityRadio);
                if (!ValidateControl(
                        root,
                        radio,
                        L"MachineRadio2",
                        kRadioControlKind,
                        true))
                    return Publish(
                        kMachineControlMismatch);
                machineCallback_(
                    machine,
                    kRadioEvent,
                    static_cast<uint8_t*>(
                        radio) +
                        kControlName,
                    radio);
                if (*reinterpret_cast<uint32_t*>(
                        bytes + kMachineType) !=
                    kActivityMachineType)
                    return Publish(
                        kNativeActionRejected);
                InterlockedExchange(
                    &activitySelected_, 1);
                return Publish(
                    kOpeningActivity);
            }
            InterlockedExchange(
                &activitySelected_, 1);

            const uint32_t effectivePages =
                *reinterpret_cast<uint32_t*>(
                    bytes + kPageCount);
            const uint32_t activityPages =
                *reinterpret_cast<uint32_t*>(
                    bytes + kActivityPageCount);
            if (!activityPages ||
                effectivePages != activityPages)
                return Publish(
                    kActivityDataMismatch);

            const uint32_t currentPage =
                *reinterpret_cast<uint32_t*>(
                    bytes + kCurrentPage);
            InterlockedExchange(
                &currentPage_,
                static_cast<LONG>(currentPage));
            if (currentPage != target.page) {
                const bool goRight =
                    currentPage < target.page;
                const wchar_t* name =
                    goRight
                        ? L"CapsuleRightBtn"
                        : L"CapsuleLeftBtn";
                void* button =
                    FindControl(
                        root,
                        name,
                        kButtonControlKind);
                if (!ValidateControl(
                        root,
                        button,
                        name,
                        kButtonControlKind,
                        true))
                    return Publish(
                        kMachineControlMismatch);
                machineCallback_(
                    machine,
                    kButtonEvent,
                    static_cast<uint8_t*>(
                        button) +
                        kControlName,
                    button);
                const uint32_t after =
                    *reinterpret_cast<uint32_t*>(
                        bytes + kCurrentPage);
                const uint32_t expected =
                    goRight
                        ? currentPage + 1
                        : currentPage - 1;
                InterlockedExchange(
                    &currentPage_,
                    static_cast<LONG>(after));
                if (after != expected)
                    return Publish(
                        kNativeActionRejected);
                return Publish(
                    kPagingActivity);
            }

            const uint32_t selected =
                *reinterpret_cast<uint32_t*>(
                    bytes + kSelectedIndex);
            const uint32_t selectedSlot =
                *reinterpret_cast<uint32_t*>(
                    bytes + kSelectedSlot);
            if (selected != target.index ||
                selectedSlot != target.slot) {
                const size_t controls =
                    target.event_tile
                        ? kEventTileControls
                        : kPointTileControls;
                void* tile =
                    *reinterpret_cast<void**>(
                        bytes + controls +
                        target.slot *
                            sizeof(void*));
                wchar_t pointName[] =
                    L"PointMachineRadio0";
                wchar_t eventName[] =
                    L"EventMachineRadio0";
                wchar_t* tileName =
                    target.event_tile
                        ? eventName
                        : pointName;
                tileName[17] =
                    static_cast<wchar_t>(
                        L'0' + target.slot);
                if (!ValidateControl(
                        root,
                        tile,
                        tileName,
                        kRadioControlKind,
                        true))
                    return Publish(
                        kMachineControlMismatch);
                machineCallback_(
                    machine,
                    kRadioEvent,
                    static_cast<uint8_t*>(
                        tile) +
                        kControlName,
                    tile);
                if (*reinterpret_cast<uint32_t*>(
                        bytes + kSelectedIndex) !=
                        target.index ||
                    *reinterpret_cast<uint32_t*>(
                        bytes + kSelectedSlot) !=
                        target.slot)
                    return Publish(
                        kNativeActionRejected);
                InterlockedExchange(
                    &targetSelected_, 1);
                return Publish(
                    kSelectingTarget);
            }
            InterlockedExchange(
                &targetSelected_, 1);

            void* dialog =
                InterlockedCompareExchangePointer(
                    &liveDialog_,
                    nullptr,
                    nullptr);
            if (!dialog) {
                if (InterlockedCompareExchange(
                        &dropInvoked_, 0, 0) == 0) {
                    void* drop =
                        FindControl(
                            root,
                            L"CapsuleDropBtn",
                            kButtonControlKind);
                    if (!ValidateControl(
                            root,
                            drop,
                            L"CapsuleDropBtn",
                            kButtonControlKind,
                            true))
                        return Publish(
                            kMachineControlMismatch);
                    InterlockedExchange(
                        &previewArmed_, 1);
                    InterlockedExchange(
                        &dropInvoked_, 1);
                    machineCallback_(
                        machine,
                        kButtonEvent,
                        static_cast<uint8_t*>(
                            drop) +
                            kControlName,
                        drop);
                    return Publish(
                        kOpeningPreview);
                }
                return Publish(
                    kWaitingForPreview);
            }

            void* buyControl = nullptr;
            const DialogResult ready =
                ResolveBuyControl(
                    dialog,
                    &buyControl);
            if (ready == kDialogWaiting)
                return Publish(
                    kWaitingForPreview);
            if (ready == kDialogInvalid)
                return Publish(
                    kDialogMismatch);
            InterlockedExchange(
                &previewReady_, 1);
            return Publish(kPreviewReady);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    // Explicit destructive boundary. The callback is invoked at most once,
    // even when the native callback faults or its transport cannot be proven.
    Result DrawOnce()
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (InterlockedCompareExchange(
                &buyAttempted_, 0, 0) != 0)
            return Publish(
                kBuyAlreadyAttempted);
        if (InterlockedCompareExchange(
                &previewReady_, 0, 0) == 0)
            return Publish(kPreviewNotReady);

        __try {
            void* dialog =
                InterlockedCompareExchangePointer(
                    &liveDialog_,
                    nullptr,
                    nullptr);
            void* buyControl = nullptr;
            if (ResolveBuyControl(
                    dialog,
                    &buyControl) !=
                kDialogReady)
                return Publish(kPreviewNotReady);

            const NativeActions::TransportObservation
                before = native_->TransportState();
            InterlockedExchange(&buyAttempted_, 1);
            buyAction_(
                dialog,
                kButtonEvent,
                static_cast<uint8_t*>(
                    buyControl) +
                    kControlName,
                buyControl);
            const NativeActions::TransportObservation
                after = native_->TransportState();
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
                    kPurchaseRequestOpcode ||
                after.packet_size !=
                    kPurchaseRequestSize ||
                after.result == 0)
                return Publish(
                    kBuyTransportRejected);

            InterlockedExchange(
                &buySubmitted_, 1);
            return Publish(kBuySubmitted);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(
                &buyAttempted_, 1);
            return Publish(kNativeFault);
        }
    }

    Result Poll()
    {
        if (!bound_)
            return Publish(kUnbound);
        if (InterlockedCompareExchange(
                &buyAttempted_, 0, 0) == 0)
            return Publish(
                InterlockedCompareExchange(
                    &previewReady_, 0, 0)
                    ? kPreviewReady
                    : kPreviewNotReady);
        if (InterlockedCompareExchange(
                &buySubmitted_, 0, 0) == 0)
            return Publish(
                kBuyTransportRejected);

        const uint32_t resultReplies =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &resultReplySequence_,
                    0,
                    0));
        const uint16_t status =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &resultReplyStatus_,
                    0,
                    0));
        if (resultReplies && status != 0)
            return Publish(kServerRejected);

        if (InterlockedCompareExchange(
                &ackSequence_, 0, 0) == 0)
            return Publish(
                resultReplies
                    ? kReplyAccepted
                    : kWaitingForReply);

        __try {
            void* service = nullptr;
            void* control = nullptr;
            const ResultUiState ui =
                ResolveResultControl(
                    &service,
                    &control);
            if (ui == kResultUiAbsent) {
                if (InterlockedCompareExchange(
                        &closeAttempted_,
                        0,
                        0) != 0)
                    return Publish(kComplete);
                return Publish(
                    kWaitingForResult);
            }
            if (ui == kResultUiServiceMismatch)
                return Publish(
                    kResultServiceMismatch);
            if (ui == kResultUiControlMismatch)
                return Publish(
                    kResultControlMismatch);
            if (ui == kResultUiWaiting) {
                if(service && InterlockedCompareExchange(&closeAttempted_,0,0)!=0 &&
                    *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(service)+kBuyingState)==kBuyingCloseState)
                    return Publish(kComplete);
                return Publish(kWaitingForResult);
            }
            InterlockedExchange(
                &resultReady_, 1);
            return Publish(kResultReady);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    Result CloseResult()
    {
        if (!bound_)
            return Publish(kUnbound);
        if (InterlockedCompareExchange(
                &ackSequence_, 0, 0) == 0)
            return Publish(kWaitingForResult);
        if (InterlockedCompareExchange(
                &closeAttempted_, 0, 0) != 0)
            return Poll();

        __try {
            void* service = nullptr;
            void* control = nullptr;
            const ResultUiState ui =
                ResolveResultControl(
                    &service,
                    &control);
            if (ui == kResultUiAbsent ||
                ui == kResultUiWaiting)
                return Publish(
                    kWaitingForResult);
            if (ui == kResultUiServiceMismatch)
                return Publish(
                    kResultServiceMismatch);
            if (ui != kResultUiReady)
                return Publish(
                    kResultControlMismatch);

            InterlockedExchange(
                &closeAttempted_, 1);
            resultCallback_(
                service,
                kResultClickEvent,
                static_cast<uint8_t*>(
                    control) +
                    kControlName,
                control);
            if (*reinterpret_cast<uint16_t*>(
                    static_cast<uint8_t*>(
                        service) +
                    kBuyingState) !=
                kBuyingCloseState)
                return Publish(
                    kResultCloseRejected);
            return Publish(kClosingResult);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(
                &closeAttempted_, 1);
            return Publish(kNativeFault);
        }
    }

    Observation State() const
    {
        Observation observed = {};
        for (int attempt = 0;
             attempt < 3;
             ++attempt) {
            const LONG before =
                InterlockedCompareExchange(
                    &observationWriteVersion_,
                    0,
                    0);
            if (before & 1)
                continue;
            observed.result =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &lastResult_, 0, 0));
            observed.target_machine =
                expectedMachine_;
            observed.target_price =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &targetPrice_, 0, 0));
            observed.target_index =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &targetIndex_, 0, 0));
            observed.target_page =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &targetPage_, 0, 0));
            observed.target_slot =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &targetSlot_, 0, 0));
            observed.activity_count =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &activityCount_, 0, 0));
            observed.activity_slots =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &activitySlots_, 0, 0));
            observed.current_page =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &currentPage_, 0, 0));
            observed.dialog_clone_sequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &dialogCloneSequence_,
                        0,
                        0));
            observed.request_transport_sequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &requestTransportSequence_,
                        0,
                        0));
            observed.request_size =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &requestSize_, 0, 0));
            observed.request_result =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &requestResult_, 0, 0));
            observed.result_reply_sequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &resultReplySequence_,
                        0,
                        0));
            observed.acknowledgement_sequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &ackSequence_, 0, 0));
            const uint32_t serialLow =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &dropBoxSerialLow_, 0, 0));
            const uint32_t serialHigh =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &dropBoxSerialHigh_, 0, 0));
            observed.dropbox_serial =
                (static_cast<uint64_t>(
                     serialHigh)
                 << 32) |
                serialLow;
            observed.reward_item_category =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &rewardItemCategory_,
                        0,
                        0));
            observed.reward_amount =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &rewardAmount_, 0, 0));
            observed.remaining_gp =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &remainingGp_, 0, 0));
            observed.request_opcode =
                static_cast<uint16_t>(
                    InterlockedCompareExchange(
                        &requestOpcode_, 0, 0));
            observed.result_reply_opcode =
                static_cast<uint16_t>(
                    InterlockedCompareExchange(
                        &resultReplyOpcode_, 0, 0));
            observed.result_reply_status =
                static_cast<uint16_t>(
                    InterlockedCompareExchange(
                        &resultReplyStatus_, 0, 0));
            observed.acknowledgement_opcode =
                static_cast<uint16_t>(
                    InterlockedCompareExchange(
                        &ackOpcode_, 0, 0));
            observed.activity_selected =
                InterlockedCompareExchange(
                    &activitySelected_, 0, 0) != 0;
            observed.target_selected =
                InterlockedCompareExchange(
                    &targetSelected_, 0, 0) != 0;
            observed.preview_ready =
                InterlockedCompareExchange(
                    &previewReady_, 0, 0) != 0;
            observed.buy_attempted =
                InterlockedCompareExchange(
                    &buyAttempted_, 0, 0) != 0;
            observed.buy_submitted =
                InterlockedCompareExchange(
                    &buySubmitted_, 0, 0) != 0;
            observed.result_ready =
                InterlockedCompareExchange(
                    &resultReady_, 0, 0) != 0;
            observed.close_attempted =
                InterlockedCompareExchange(
                    &closeAttempted_, 0, 0) != 0;
            observed.original_reply_completed =
                InterlockedCompareExchange(
                    &originalReplyCompleted_,
                    0,
                    0) != 0;
            const LONG after =
                InterlockedCompareExchange(
                    &observationWriteVersion_,
                    0,
                    0);
            if (before == after &&
                !(after & 1))
                return observed;
        }
        memset(&observed, 0, sizeof(observed));
        return observed;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound:
            return "unbound";
        case kWaitingForMachine:
            return "waiting-for-machine";
        case kWaitingForMachineUi:
            return "waiting-for-machine-ui";
        case kOpeningActivity:
            return "opening-activity";
        case kPagingActivity:
            return "paging-activity";
        case kSelectingTarget:
            return "selecting-target";
        case kOpeningPreview:
            return "opening-preview";
        case kWaitingForPreview:
            return "waiting-for-preview";
        case kPreviewReady:
            return "preview-ready";
        case kBuySubmitted:
            return "buy-submitted";
        case kWaitingForReply:
            return "waiting-for-reply";
        case kReplyAccepted:
            return "reply-accepted";
        case kWaitingForResult:
            return "waiting-for-result";
        case kResultReady:
            return "result-ready";
        case kClosingResult:
            return "closing-result";
        case kComplete:
            return "complete";
        case kMachineServiceMismatch:
            return "machine-service-mismatch";
        case kMachineControlMismatch:
            return "machine-control-mismatch";
        case kActivityDataMismatch:
            return "activity-data-mismatch";
        case kTargetMachineMismatch:
            return "target-machine-mismatch";
        case kNativeActionRejected:
            return "native-action-rejected";
        case kDialogMismatch:
            return "dialog-mismatch";
        case kPreviewNotReady:
            return "preview-not-ready";
        case kBuyAlreadyAttempted:
            return "buy-already-attempted";
        case kBuyTransportRejected:
            return "buy-transport-rejected";
        case kServerRejected:
            return "server-rejected";
        case kResultServiceMismatch:
            return "result-service-mismatch";
        case kResultControlMismatch:
            return "result-control-mismatch";
        case kResultCloseRejected:
            return "result-close-rejected";
        case kNativeFault:
            return "native-fault";
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

    enum ScanResult
    {
        kScanWaiting,
        kScanReady,
        kScanInvalid,
        kScanTargetMismatch,
    };

    enum DialogResult
    {
        kDialogWaiting,
        kDialogReady,
        kDialogInvalid,
    };

    enum ResultUiState
    {
        kResultUiAbsent,
        kResultUiWaiting,
        kResultUiReady,
        kResultUiServiceMismatch,
        kResultUiControlMismatch,
    };

    struct TargetScan
    {
        uint32_t index;
        uint32_t page;
        uint32_t slot;
        uint32_t price;
        uint32_t valid_count;
        uint32_t slots;
        bool event_tile;
    };

    uint32_t expectedMachine_ =
        990001;
    uint32_t expectedPrice_ =
        100000;
    uint32_t expectedCurrency_ =
        0;
    static constexpr uint32_t kActivityMachineType =
        2;
    static constexpr uint32_t kRadioEvent =
        0x301;
    static constexpr uint32_t kButtonEvent =
        0x101;
    static constexpr uint32_t kResultClickEvent =
        0x801;
    static constexpr uint16_t kPurchaseRequestOpcode =
        0x05D3;
    static constexpr uint32_t kPurchaseRequestSize =
        10;
    static constexpr uint16_t kResultReplyOpcode =
        0x04E1;
    static constexpr uint16_t kAcknowledgementOpcode =
        0x09C6;
    static constexpr uint16_t kBuyingResultState =
        2;
    static constexpr uint16_t kBuyingCloseState =
        4;

    static constexpr uint32_t kRadioControlKind =
        0x0C;
    static constexpr uint32_t kButtonControlKind =
        4;
    static constexpr uint32_t kIconControlKind =
        1;
    static constexpr size_t kNativeWStringSize =
        0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kNativeStringSize =
        0x10;
    static constexpr size_t kNativeStringCapacity =
        0x14;
    static constexpr size_t kControlName =
        0x118;
    static constexpr size_t kControlParent =
        0x15C;
    static constexpr size_t kControlVisible =
        0x162;
    static constexpr size_t kControlEnabled =
        0x167;

    static constexpr size_t kMachineRoot =
        0x60;
    static constexpr size_t kCurrentPage =
        0x78;
    static constexpr size_t kPageCount =
        0x7C;
    static constexpr size_t kActivityPageCount =
        0x1DC;
    static constexpr size_t kSelectedIndex =
        0x80;
    static constexpr size_t kSelectedSlot =
        0x84;
    static constexpr size_t kPointTileControls =
        0x94;
    static constexpr size_t kEventTileControls =
        0xB4;
    static constexpr size_t kActivityRadio =
        0xDC;
    static constexpr size_t kMachineType =
        0x1A0;
    static constexpr size_t kActivityBegin =
        0x1A4;
    static constexpr size_t kActivityEnd =
        0x1A8;
    static constexpr size_t kActivityCapacity =
        0x1AC;
    static constexpr size_t kActivityStride =
        0xB3;
    static constexpr size_t kMachineIdOffset =
        5;
    static constexpr size_t kCurrencyOffset =
        9;
    static constexpr size_t kEventTileOffset =
        10;
    static constexpr size_t kPriceOffset =
        15;
    static constexpr uint32_t kMaximumActivitySlots =
        4096;

    static constexpr size_t kDialogMachineId =
        0x50;
    static constexpr size_t kDialogType =
        0x60;
    static constexpr size_t kDialogActivityRecord =
        0x90;
    static constexpr uint32_t kActivityDialogType =
        3;
    static constexpr size_t kBuyingState =
        0x48;
    static constexpr size_t kBuyingMachineId =
        0x70;
    static constexpr size_t kBuyingRewardCategory =
        0x88;
    static constexpr size_t kBuyingRewardAmount =
        0x8C;

    static constexpr size_t kReplyStatusOffset =
        6;
    static constexpr size_t kAckDropBoxSerialOffset =
        6;
    static constexpr size_t kAckRewardCategoryOffset =
        0x0E;
    static constexpr size_t kAckRewardAmountOffset =
        0x12;
    static constexpr size_t kAckRemainingGpOffset =
        0x1E;
    static constexpr size_t kAckCurrencyFlagOffset =
        0x22;
    static constexpr uint32_t kRewardCategoryBegin =
        1100000;
    static constexpr uint32_t kRewardCategoryEnd =
        1200000;

    static constexpr size_t kReplyVtableSlot =
        0x28 / sizeof(void*);
    static constexpr size_t kUiCallbackVtableSlot =
        0x38 / sizeof(void*);
    static constexpr size_t kCloneVtableSlot =
        0x54 / sizeof(void*);
    static constexpr size_t kFindChildVtableSlot =
        0x5C / sizeof(void*);

    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(
            void*,
            const void*,
            uint32_t);
    using FindChildFn =
        void*(__thiscall*)(void*, const void*);
    using UiCallbackFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
    using DialogCloneFn =
        void*(__thiscall*)(void*);
    using ReplyDispatchFn =
        int(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    static bool SlotMatches(
        void* vtable,
        size_t slot,
        void* function)
    {
        return vtable && function &&
            static_cast<void**>(vtable)[slot] ==
                function;
    }

    Result Publish(Result result)
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        return result;
    }

    void PublishTarget(const TargetScan& target)
    {
        InterlockedExchange(
            &targetPrice_,
            static_cast<LONG>(target.price));
        InterlockedExchange(
            &targetIndex_,
            static_cast<LONG>(target.index));
        InterlockedExchange(
            &targetPage_,
            static_cast<LONG>(target.page));
        InterlockedExchange(
            &targetSlot_,
            static_cast<LONG>(target.slot));
        InterlockedExchange(
            &activityCount_,
            static_cast<LONG>(target.valid_count));
        InterlockedExchange(
            &activitySlots_,
            static_cast<LONG>(target.slots));
    }

    ScanResult ScanTarget(
        uint8_t* service,
        TargetScan& target) const
    {
        target = {};
        if (!service)
            return kScanWaiting;
        const auto begin =
            reinterpret_cast<uintptr_t>(
                *reinterpret_cast<void**>(
                    service + kActivityBegin));
        const auto end =
            reinterpret_cast<uintptr_t>(
                *reinterpret_cast<void**>(
                    service + kActivityEnd));
        const auto capacity =
            reinterpret_cast<uintptr_t>(
                *reinterpret_cast<void**>(
                    service + kActivityCapacity));
        if (!begin || !end || !capacity)
            return kScanWaiting;
        if (end < begin || capacity < end)
            return kScanInvalid;
        const uintptr_t bytes = end - begin;
        if (bytes == 0)
            return kScanWaiting;
        if (bytes % kActivityStride != 0)
            return kScanInvalid;
        const uint32_t slots =
            static_cast<uint32_t>(
                bytes / kActivityStride);
        if (!slots ||
            slots > kMaximumActivitySlots)
            return kScanInvalid;

        uint32_t matches = 0;
        uint32_t valid = 0;
        for (uint32_t index = 0;
             index < slots;
             ++index) {
            const auto* row =
                reinterpret_cast<const uint8_t*>(
                    begin +
                    static_cast<uintptr_t>(
                        index) *
                        kActivityStride);
            const uint32_t machine =
                ReadU32(
                    row + kMachineIdOffset);
            if (machine)
                ++valid;
            if (machine != expectedMachine_)
                continue;
            ++matches;
            target.index = index;
            target.page = index / 8;
            target.slot = index % 8;
            target.price =
                ReadU32(
                    row + kPriceOffset);
            target.event_tile =
                row[kEventTileOffset] != 0;
            if (row[kCurrencyOffset] !=
                    expectedCurrency_ ||
                target.price != expectedPrice_)
                return kScanTargetMismatch;
        }
        target.valid_count = valid;
        target.slots = slots;
        if (matches != 1)
            return kScanTargetMismatch;

        const uint32_t pages =
            *reinterpret_cast<uint32_t*>(
                service + kActivityPageCount);
        if (!pages ||
            slots % 8 != 0 ||
            target.page >= pages ||
            pages != slots / 8)
            return kScanInvalid;
        return kScanReady;
    }

    DialogResult ResolveBuyControl(
        void* dialog,
        void** output) const
    {
        if (output)
            *output = nullptr;
        if (!dialog || !output)
            return kDialogWaiting;
        auto* bytes =
            static_cast<uint8_t*>(dialog);
        if (*reinterpret_cast<void**>(dialog) !=
            dialogVtable_)
            return kDialogInvalid;
        if (*reinterpret_cast<uint32_t*>(
                bytes + kDialogMachineId) !=
            expectedMachine_)
            return kDialogInvalid;
        const uint32_t type =
            *reinterpret_cast<uint32_t*>(
                bytes + kDialogType);
        if (type == 0)
            return kDialogWaiting;
        if (type != kActivityDialogType)
            return kDialogInvalid;
        const auto* record =
            *reinterpret_cast<const uint8_t**>(
                bytes + kDialogActivityRecord);
        if (!record)
            return kDialogWaiting;
        if (ReadU32(record + kMachineIdOffset) !=
                expectedMachine_ ||
            record[kCurrencyOffset] !=
                expectedCurrency_ ||
            ReadU32(record + kPriceOffset) !=
                expectedPrice_)
            return kDialogInvalid;

        void* root =
            FindNamedChild(
                dialog,
                L"f_buy_dlg");
        if (!root)
            return kDialogWaiting;
        void* buy =
            FindControl(
                root,
                L"BuyBtn",
                kButtonControlKind);
        if (!buy)
            return kDialogWaiting;
        if (!ValidateControl(
                root,
                buy,
                L"BuyBtn",
                kButtonControlKind,
                false))
            return kDialogInvalid;
        auto* control =
            static_cast<uint8_t*>(buy);
        if (!control[kControlVisible] ||
            !control[kControlEnabled])
            return kDialogWaiting;
        *output = buy;
        return kDialogReady;
    }

    ResultUiState ResolveResultControl(
        void** serviceOutput,
        void** controlOutput) const
    {
        if (serviceOutput)
            *serviceOutput = nullptr;
        if (controlOutput)
            *controlOutput = nullptr;
        if (!serviceOutput || !controlOutput)
            return kResultUiControlMismatch;

        void* service = nullptr;
        const ServiceResult found =
            ResolveService(
                L"machine",
                buyingVtable_,
                &service);
        if (found == kServiceAbsent)
            return kResultUiAbsent;
        if (found == kServiceMismatched)
            return kResultUiServiceMismatch;
        *serviceOutput = service;

        auto* bytes =
            static_cast<uint8_t*>(service);
        if (*reinterpret_cast<uint16_t*>(
                bytes + kBuyingState) !=
            kBuyingResultState)
            return kResultUiWaiting;
        void* root =
            FindNamedChild(
                service,
                L"machine");
        if (!root)
            return kResultUiWaiting;
        void* control =
            FindControl(
                root,
                L"CapsuleClick",
                kIconControlKind);
        if (!control)
            return kResultUiWaiting;
        if (!ValidateControl(
                root,
                control,
                L"CapsuleClick",
                kIconControlKind,
                false))
            return kResultUiControlMismatch;
        auto* controlBytes =
            static_cast<uint8_t*>(control);
        if (!controlBytes[kControlVisible] ||
            !controlBytes[kControlEnabled])
            return kResultUiWaiting;
        *controlOutput = control;
        return kResultUiReady;
    }

    ServiceResult ResolveService(
        const wchar_t* name,
        void* expectedVtable,
        void** output) const
    {
        if (output)
            *output = nullptr;
        if (!name || !expectedVtable ||
            !output || !serviceManagedPointer_ ||
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

    void* FindNamedChild(
        void* object,
        const wchar_t* name) const
    {
        if (!object || !name ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;
        void** vtable =
            *static_cast<void***>(object);
        if (!vtable)
            return nullptr;
        const auto find =
            reinterpret_cast<FindChildFn>(
                vtable[kFindChildVtableSlot]);
        if (!find)
            return nullptr;

        alignas(4) uint8_t nativeName[
            kNativeWStringSize] = {};
        bool constructed = false;
        void* child = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (constructed)
                child = find(
                    object,
                    nativeName);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    nativeName);
        }
        return child;
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
        return control;
    }

    bool ValidateControl(
        void* root,
        void* control,
        const wchar_t* name,
        uint32_t kind,
        bool requireReady) const
    {
        if (!root || !control || !name)
            return false;
        if (FindControl(root, name, kind) !=
            control)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(
                control);
        if (*reinterpret_cast<void* const*>(
                bytes + kControlParent) !=
            root)
            return false;
        if (!ControlHasName(
                control,
                name))
            return false;
        return !requireReady ||
            (bytes[kControlVisible] != 0 &&
             bytes[kControlEnabled] != 0);
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
            *reinterpret_cast<const uint32_t*>(
                nativeName +
                kNativeStringSize);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                nativeName +
                kNativeStringCapacity);
        const size_t expectedSize =
            wcslen(expected);
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
            wmemcmp(
                value,
                expected,
                size) == 0;
    }

    static uint32_t ReadU32(
        const void* source)
    {
        uint32_t value = 0;
        if (source)
            memcpy(
                &value,
                source,
                sizeof(value));
        return value;
    }

    static void* __fastcall ObserveDialogClone(
        void* source,
        void*)
    {
        CapsuleDrawActions* actions = active_;
        if (!actions || !actions->dialogClone_)
            return nullptr;
        void* live =
            actions->dialogClone_(source);
        if (!live ||
            InterlockedCompareExchange(
                &actions->previewArmed_,
                0,
                0) == 0)
            return live;

        bool target = false;
        __try {
            const auto* bytes =
                static_cast<const uint8_t*>(
                    source);
            target =
                source &&
                *reinterpret_cast<void* const*>(
                    source) ==
                    actions->dialogVtable_ &&
                *reinterpret_cast<uint32_t*>(
                    const_cast<uint8_t*>(
                        bytes) +
                    kDialogMachineId) ==
                    actions->expectedMachine_ &&
                *reinterpret_cast<void* const*>(
                    live) ==
                    actions->dialogVtable_;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            target = false;
        }
        if (target) {
            InterlockedIncrement(
                &actions->
                    observationWriteVersion_);
            InterlockedExchangePointer(
                &actions->liveDialog_,
                live);
            InterlockedIncrement(
                &actions->
                    dialogCloneSequence_);
            InterlockedIncrement(
                &actions->
                    observationWriteVersion_);
        }
        return live;
    }

    static int __fastcall ObserveReplyDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        CapsuleDrawActions* actions = active_;
        if (!actions ||
            !actions->replyDispatch_)
            return 0;

        const uint16_t normalized =
            static_cast<uint16_t>(
                opcode & 0xFFFF);
        uint16_t status = 0;
        uint32_t serialLow = 0;
        uint32_t serialHigh = 0;
        uint32_t rewardCategory = 0;
        uint32_t rewardAmount = 0;
        uint32_t remaining = 0;
        bool packetReadable = false;
        bool targetService = false;
        __try {
            const auto* bytes =
                static_cast<const uint8_t*>(
                    self);
            targetService =
                self &&
                *reinterpret_cast<void* const*>(
                    self) ==
                    actions->buyingVtable_ &&
                *reinterpret_cast<
                    const uint32_t*>(
                    bytes +
                    kBuyingMachineId) ==
                    actions->expectedMachine_;
        } __except (
            EXCEPTION_EXECUTE_HANDLER) {
            targetService = false;
        }
        if ((normalized ==
                kResultReplyOpcode ||
             normalized ==
                kAcknowledgementOpcode) &&
            targetService &&
            packet) {
            __try {
                if (normalized ==
                    kResultReplyOpcode) {
                    status =
                        *reinterpret_cast<
                            const uint16_t*>(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kReplyStatusOffset);
                } else {
                    serialLow =
                        ReadU32(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kAckDropBoxSerialOffset);
                    serialHigh =
                        ReadU32(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kAckDropBoxSerialOffset +
                            sizeof(uint32_t));
                    rewardCategory =
                        ReadU32(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kAckRewardCategoryOffset);
                    rewardAmount =
                        ReadU32(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kAckRewardAmountOffset);
                    remaining =
                        ReadU32(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kAckRemainingGpOffset);
                    packetReadable =
                        (serialLow != 0 ||
                         serialHigh != 0) &&
                        (serialHigh &
                         0x80000000U) == 0 &&
                        rewardCategory >=
                            kRewardCategoryBegin &&
                        rewardCategory <
                            kRewardCategoryEnd &&
                        rewardAmount == 1 &&
                        *(
                            static_cast<
                                const uint8_t*>(
                                packet) +
                            kAckCurrencyFlagOffset) ==
                            actions->expectedCurrency_;
                }
                if (normalized ==
                    kResultReplyOpcode)
                    packetReadable = true;
            } __except (
                EXCEPTION_EXECUTE_HANDLER) {
                status = UINT16_MAX;
                serialLow = 0;
                serialHigh = 0;
                rewardCategory = 0;
                rewardAmount = 0;
                remaining = 0;
                packetReadable = false;
            }
        }

        const int result =
            actions->replyDispatch_(
                self,
                a,
                b,
                c,
                opcode,
                packet);

        if (InterlockedCompareExchange(
                &actions->buySubmitted_,
                0,
                0) == 0 ||
            !packetReadable)
            return result;

        if (normalized ==
            kAcknowledgementOpcode) {
            bool originalConsumed = false;
            __try {
                const auto* bytes =
                    static_cast<const uint8_t*>(
                        self);
                originalConsumed =
                    *reinterpret_cast<
                        const uint16_t*>(
                        bytes +
                        kBuyingState) ==
                        kBuyingResultState &&
                    *reinterpret_cast<
                        const uint32_t*>(
                        bytes +
                        kBuyingRewardCategory) ==
                        rewardCategory &&
                    *reinterpret_cast<
                        const uint32_t*>(
                        bytes +
                        kBuyingRewardAmount) ==
                        rewardAmount;
            } __except (
                EXCEPTION_EXECUTE_HANDLER) {
                originalConsumed = false;
            }
            if (!originalConsumed)
                return result;
        }

        if (normalized ==
            kResultReplyOpcode) {
            InterlockedIncrement(
                &actions->
                    observationWriteVersion_);
            InterlockedExchange(
                &actions->
                    resultReplyOpcode_,
                normalized);
            InterlockedExchange(
                &actions->
                    resultReplyStatus_,
                status);
            InterlockedIncrement(
                &actions->
                    resultReplySequence_);
            InterlockedExchange(
                &actions->
                    originalReplyCompleted_,
                1);
            InterlockedIncrement(
                &actions->
                    observationWriteVersion_);
        } else if (normalized ==
                   kAcknowledgementOpcode) {
            InterlockedIncrement(
                &actions->
                    observationWriteVersion_);
            InterlockedExchange(
                &actions->ackOpcode_,
                normalized);
            InterlockedExchange(
                &actions->dropBoxSerialLow_,
                static_cast<LONG>(
                    serialLow));
            InterlockedExchange(
                &actions->dropBoxSerialHigh_,
                static_cast<LONG>(
                    serialHigh));
            InterlockedExchange(
                &actions->
                    rewardItemCategory_,
                static_cast<LONG>(
                    rewardCategory));
            InterlockedExchange(
                &actions->rewardAmount_,
                static_cast<LONG>(
                    rewardAmount));
            InterlockedExchange(
                &actions->remainingGp_,
                static_cast<LONG>(
                    remaining));
            InterlockedIncrement(
                &actions->ackSequence_);
            InterlockedExchange(
                &actions->
                    originalReplyCompleted_,
                1);
            InterlockedIncrement(
                &actions->
                    observationWriteVersion_);
        }
        return result;
    }

    inline static CapsuleDrawActions* active_ =
        nullptr;

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ =
        nullptr;
    NativeWStringCtorFn nativeWStringCtor_ =
        nullptr;
    NativeWStringDtorFn nativeWStringDtor_ =
        nullptr;
    NativeUiFindControlFn nativeUiFindControl_ =
        nullptr;
    void* machineVtable_ = nullptr;
    UiCallbackFn machineCallback_ = nullptr;
    void* dialogVtable_ = nullptr;
    DialogCloneFn dialogClone_ = nullptr;
    UiCallbackFn buyAction_ = nullptr;
    ReplyDispatchFn replyDispatch_ = nullptr;
    void* buyingVtable_ = nullptr;
    UiCallbackFn resultCallback_ = nullptr;
    bool bound_ = false;

    mutable void* volatile liveDialog_ = nullptr;
    mutable volatile LONG lastResult_ = kUnbound;
    mutable volatile LONG previewArmed_ = 0;
    mutable volatile LONG dropInvoked_ = 0;
    mutable volatile LONG dialogCloneSequence_ = 0;
    mutable volatile LONG previewReady_ = 0;
    mutable volatile LONG buyAttempted_ = 0;
    mutable volatile LONG buySubmitted_ = 0;
    mutable volatile LONG closeAttempted_ = 0;
    mutable volatile LONG activitySelected_ = 0;
    mutable volatile LONG targetSelected_ = 0;
    mutable volatile LONG resultReady_ = 0;
    mutable volatile LONG targetPrice_ = 0;
    mutable volatile LONG targetIndex_ = 0;
    mutable volatile LONG targetPage_ = 0;
    mutable volatile LONG targetSlot_ = 0;
    mutable volatile LONG activityCount_ = 0;
    mutable volatile LONG activitySlots_ = 0;
    mutable volatile LONG currentPage_ = 0;
    mutable volatile LONG requestTransportSequence_ = 0;
    mutable volatile LONG requestOpcode_ = 0;
    mutable volatile LONG requestSize_ = 0;
    mutable volatile LONG requestResult_ = 0;
    mutable volatile LONG resultReplySequence_ = 0;
    mutable volatile LONG resultReplyOpcode_ = 0;
    mutable volatile LONG resultReplyStatus_ = 0;
    mutable volatile LONG ackSequence_ = 0;
    mutable volatile LONG ackOpcode_ = 0;
    mutable volatile LONG dropBoxSerialLow_ = 0;
    mutable volatile LONG dropBoxSerialHigh_ = 0;
    mutable volatile LONG rewardItemCategory_ = 0;
    mutable volatile LONG rewardAmount_ = 0;
    mutable volatile LONG remainingGp_ = 0;
    mutable volatile LONG originalReplyCompleted_ = 0;
    mutable volatile LONG observationWriteVersion_ = 0;
};
