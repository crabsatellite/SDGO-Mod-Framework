#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Tick-driven access to the original beginner flow. The driver calls only
// shipped client business routines or sends the exact request emitted by those
// routines. Replies, inventory/profile changes, scene transitions and popup
// lifetime remain owned by the original client and server.
class OnboardingActions
{
public:
    enum StarterResult : uint32_t
    {
        kStarterNone = 0,
        kStarterRejected = 1,
        kStarterAccepted = 2,
    };

    enum InitialLoadingResult : uint32_t
    {
        kInitialLoadingNone = 0,
        kInitialLoadingRejected = 1,
        kInitialLoadingAccepted = 2,
    };

    enum StarterBuyingUiState : uint32_t
    {
        kStarterBuyingAbsent = 0,
        kStarterBuyingWaiting = 1,
        kStarterBuyingReady = 2,
        kStarterBuyingIdentityMismatch = 3,
        kStarterBuyingControlMismatch = 4,
        kStarterBuyingNativeFault = 5,
    };

    struct StarterObservation
    {
        uint32_t reply_sequence;
        uint32_t result_confirm_sequence;
        uint32_t buying_clone_sequence;
        uint32_t buying_captured_app_state;
        uint32_t buying_ui_state;
        uint32_t result_popup_clone_sequence;
        uint32_t result_popup_captured_app_state;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t onboarding_stage;
        uint32_t tutorial_level;
        uint32_t profile_pilot_id;
        uint32_t picked_unit_id;
        uint32_t gp_award;
        uint16_t reply_opcode;
        uint16_t reply_error;
        uint8_t reply_tutorial_level;
        uint8_t picked_unit_slot;
        uint8_t starter_draw_busy;
        uint8_t request_attempted;
        uint8_t buying_captured;
        uint8_t buying_named_identity_matched;
        uint8_t buying_control_ready;
        uint8_t buying_validation_failed;
        uint8_t result_popup_captured;
        uint8_t result_popup_ready;
        uint8_t result_confirm_attempted;
        uint8_t native_fault;
    };

    struct InventoryObservation
    {
        uint32_t slot_capacity;
        uint32_t slots_examined;
        uint32_t owned_unit_count;
        uint32_t first_unit_id;
        uint32_t first_unit_uid_low;
        uint32_t first_unit_uid_high;
        uint16_t first_unit_slot;
        uint8_t manager_present;
        uint8_t initialized;
        uint8_t scan_complete;
        uint8_t native_fault;
    };

    struct InitialLoadingObservation
    {
        uint32_t reply_sequence;
        uint32_t confirm_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t onboarding_stage;
        uint32_t popup_state;
        uint32_t pc_room_value;
        uint16_t reply_opcode;
        uint16_t reply_error;
        uint8_t pc_room_code;
        uint8_t reserved[3];
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        onboardingUpdate_ =
            Symbol<OnboardingUpdateFn>("flow.onboarding_update");
        onboardingCapsuleClone_ =
            Symbol<OnboardingCapsuleCloneFn>(
                "flow.onboarding_capsule_clone");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>(
                "flow.service_find_by_name");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        starterBuyingVtable_ =
            Symbol<void*>("flow.onboarding_buying_vtable");
        starterBuyingClone_ =
            Symbol<StarterBuyingCloneFn>(
                "flow.onboarding_buying_clone");
        capsuleBuyAction_ =
            Symbol<CapsuleBuyActionFn>("flow.capsule_buy_action");
        starterDispatch_ =
            Symbol<PacketDispatchFn>("flow.starter_dispatch");
        starterResultPopupClone_ =
            Symbol<StarterResultPopupCloneFn>(
                "flow.starter_result_popup_clone");
        starterResultPopupAction_ =
            Symbol<StarterResultPopupActionFn>(
                "flow.starter_result_popup_action");
        unitInfoPointer_ =
            Symbol<uint8_t**>("flow.unit_info_pointer");
        unitBriefBySlot_ =
            Symbol<UnitBriefBySlotFn>("flow.unit_brief_by_slot");
        pcRoomPopupClone_ =
            Symbol<PcRoomPopupCloneFn>("flow.pc_room_popup_clone");
        pcRoomPopupConfirm_ =
            Symbol<PcRoomPopupConfirmFn>("flow.pc_room_popup_confirm");
        pcRoomPopupDispatch_ =
            Symbol<PacketDispatchFn>("flow.pc_room_popup_dispatch");

        if (!api_ || !mod_ || !native_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !onboardingUpdate_ ||
            !onboardingCapsuleClone_ || !serviceManagedPointer_ ||
            !serviceFindByName_ || !nativeUiFindControl_ ||
            !starterBuyingVtable_ || !starterBuyingClone_ ||
            !capsuleBuyAction_ ||
            !starterDispatch_ || !starterResultPopupClone_ ||
            !starterResultPopupAction_ || !unitInfoPointer_ ||
            !unitBriefBySlot_ ||
            !pcRoomPopupClone_ || !pcRoomPopupConfirm_ ||
            !pcRoomPopupDispatch_)
            return false;
        if (active_ && active_ != this)
            return false;
        __try {
            if (!SlotMatches(
                    starterBuyingVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        capsuleBuyAction_)) ||
                !SlotMatches(
                    starterBuyingVtable_,
                    kCloneVtableSlot,
                    reinterpret_cast<void*>(
                        starterBuyingClone_)))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.onboarding_update",
                reinterpret_cast<void*>(&ObserveOnboardingUpdate),
                reinterpret_cast<void**>(&onboardingUpdate_)) ||
            !api_->HookCode(
                mod_,
                "flow.onboarding_capsule_clone",
                reinterpret_cast<void*>(&ObserveOnboardingCapsuleClone),
                reinterpret_cast<void**>(&onboardingCapsuleClone_)) ||
            !api_->HookCode(
                mod_,
                "flow.onboarding_buying_clone",
                reinterpret_cast<void*>(&ObserveStarterBuyingClone),
                reinterpret_cast<void**>(&starterBuyingClone_)) ||
            !api_->HookCode(
                mod_,
                "flow.starter_dispatch",
                reinterpret_cast<void*>(&ObserveStarterDispatch),
                reinterpret_cast<void**>(&starterDispatch_)) ||
            !api_->HookCode(
                mod_,
                "flow.starter_result_popup_clone",
                reinterpret_cast<void*>(&ObserveStarterResultPopupClone),
                reinterpret_cast<void**>(&starterResultPopupClone_)) ||
            !api_->HookCode(
                mod_,
                "flow.pc_room_popup_clone",
                reinterpret_cast<void*>(&ObservePcRoomPopupClone),
                reinterpret_cast<void**>(&pcRoomPopupClone_)) ||
            !api_->HookCode(
                mod_,
                "flow.pc_room_popup_dispatch",
                reinterpret_cast<void*>(&ObservePcRoomPopupDispatch),
                reinterpret_cast<void**>(&pcRoomPopupDispatch_))) {
            // Hooks installed earlier in this sequence must retain access to
            // their original functions even though the mod will reject Bind.
            return false;
        }
        return true;
    }

    // The onboarding capsule's native FSM creates a separate
    // CServiceCoinBuying named "buying". Invoke the shipped BuyBtn business
    // callback only on that manager-owned live clone after its f_buy_dlg root
    // and real visible/enabled BuyBtn control are ready. The original callback
    // owns its busy guard, pending-request registration and exact 0x072E send.
    // The real 0x054A handler remains solely responsible for granting the unit,
    // 600 GP and tutorial-level progression.
    bool DrawStarterUnit()
    {
        if (!native_ ||
            InterlockedCompareExchange(&onboardingStage_, 0, 0) !=
                kStarterMachineStage)
            return false;
        const NativeActions::OnboardingObservation state =
            native_->OnboardingState();
        if (!state.profile_pilot_id || state.tutorial_level >= 2 ||
            state.starter_draw_busy ||
            InterlockedCompareExchange(&starterReplySequence_, 0, 0) != 0 ||
            InterlockedCompareExchange(&starterRequestAttempted_, 0, 0) != 0)
            return false;

        void* buying = nullptr;
        void* buyControl = nullptr;
        bool namedIdentityMatched = false;
        const StarterBuyingUiState uiState =
            ResolveStarterBuying(
                &buying,
                &buyControl,
                &namedIdentityMatched);
        InterlockedExchange(
            &starterBuyingUiState_,
            static_cast<LONG>(uiState));
        InterlockedExchange(
            &starterBuyingNamedIdentityMatched_,
            namedIdentityMatched ? 1 : 0);
        if (uiState != kStarterBuyingReady)
            return false;
        if (InterlockedCompareExchange(
                &starterRequestAttempted_, 1, 0) != 0)
            return false;

        bool invoked = false;
        __try {
            capsuleBuyAction_(
                buying,
                kUiButtonPressed,
                static_cast<uint8_t*>(
                    buyControl) +
                    kControlName,
                buyControl);
            invoked = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&starterNativeFault_, 1);
        }
        return invoked;
    }

    // The shipped starter-result popup normally invokes this same callback
    // from its OkBtn. The callback emits the real 0x0FA2 service event and
    // closes the live popup through the original object manager. That event,
    // rather than a direct stage write, advances onboarding stage 100.
    bool ConfirmStarterResultPopup()
    {
        if (!nativeWStringCtor_ || !nativeWStringDtor_ ||
            !starterResultPopupAction_ ||
            InterlockedCompareExchange(&starterResult_, 0, 0) !=
                static_cast<LONG>(kStarterAccepted) ||
            InterlockedCompareExchange(
                &starterResultConfirmAttempted_, 0, 0) != 0)
            return false;

        void* popup = InterlockedCompareExchangePointer(
            &starterResultPopup_, nullptr, nullptr);
        if (!popup)
            return false;
        __try {
            if (*reinterpret_cast<const uint8_t*>(
                    static_cast<const uint8_t*>(popup) +
                    kStarterResultPopupReady) == 0)
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedCompareExchangePointer(
                &starterResultPopup_, nullptr, popup);
            InterlockedExchange(&starterNativeFault_, 1);
            return false;
        }

        if (InterlockedCompareExchange(
                &starterResultConfirmAttempted_, 1, 0) != 0)
            return false;
        popup = InterlockedExchangePointer(&starterResultPopup_, nullptr);
        if (!popup)
            return false;
        InterlockedExchangePointer(&starterBuying_, nullptr);

        alignas(4) uint8_t command[kNativeWStringSize] = {};
        bool constructed = false;
        bool invoked = false;
        __try {
            nativeWStringCtor_(command, L"OkBtn");
            constructed = true;
            starterResultPopupAction_(
                popup,
                kUiButtonPressed,
                command,
                0);
            invoked = true;
            InterlockedIncrement(&starterResultConfirmSequence_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&starterNativeFault_, 1);
        }
        if (constructed) {
            __try {
                nativeWStringDtor_(command);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&starterNativeFault_, 1);
                invoked = false;
            }
        }
        return invoked;
    }

    // The normal onboarding state machine creates PCRoomCheckPopup through the
    // UI manager. Its class-specific clone hook gives us the manager-owned live
    // object. Calling the shipped confirm method on that object sends the real
    // 0x095F finish-initial-loading request and closes through the normal path.
    bool ConfirmInitialLoadingPopup()
    {
        void* popup =
            InterlockedCompareExchangePointer(&pcRoomPopup_, nullptr, nullptr);
        if (!popup || !pcRoomPopupConfirm_)
            return false;

        __try {
            if (*reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(popup) +
                    kPcRoomPopupState) != kPcRoomPopupReady)
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchangePointer(&pcRoomPopup_, nullptr);
            return false;
        }

        popup = InterlockedExchangePointer(&pcRoomPopup_, nullptr);
        if (!popup)
            return false;
        __try {
            pcRoomPopupConfirm_(popup);
            InterlockedIncrement(&initialLoadingConfirmSequence_);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    StarterObservation StarterState() const
    {
        StarterObservation observed = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(&starterWriteVersion_, 0, 0);
            if (before & 1)
                continue;
            observed.reply_sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(&starterReplySequence_, 0, 0));
            observed.result_confirm_sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &starterResultConfirmSequence_, 0, 0));
            observed.buying_clone_sequence =
                static_cast<uint32_t>(InterlockedCompareExchange(
                    &starterBuyingCloneSequence_, 0, 0));
            observed.buying_captured_app_state =
                static_cast<uint32_t>(InterlockedCompareExchange(
                    &starterBuyingCapturedAppState_, 0, 0));
            observed.result_popup_clone_sequence =
                static_cast<uint32_t>(InterlockedCompareExchange(
                    &starterResultPopupCloneSequence_, 0, 0));
            observed.result_popup_captured_app_state =
                static_cast<uint32_t>(InterlockedCompareExchange(
                    &starterResultPopupCapturedAppState_, 0, 0));
            observed.result = static_cast<uint32_t>(
                InterlockedCompareExchange(&starterResult_, 0, 0));
            observed.onboarding_stage = static_cast<uint32_t>(
                InterlockedCompareExchange(&onboardingStage_, 0, 0));
            observed.picked_unit_id = static_cast<uint32_t>(
                InterlockedCompareExchange(&starterUnitId_, 0, 0));
            observed.gp_award = static_cast<uint32_t>(
                InterlockedCompareExchange(&starterGpAward_, 0, 0));
            const uint32_t reply = static_cast<uint32_t>(
                InterlockedCompareExchange(&starterReply_, 0, 0));
            observed.reply_opcode =
                static_cast<uint16_t>(reply & 0xFFFFu);
            observed.reply_error =
                static_cast<uint16_t>(reply >> 16);
            observed.reply_tutorial_level = static_cast<uint8_t>(
                InterlockedCompareExchange(
                    &starterReplyTutorialLevel_, 0, 0));
            observed.picked_unit_slot = static_cast<uint8_t>(
                InterlockedCompareExchange(&starterUnitSlot_, 0, 0));
            observed.request_attempted = static_cast<uint8_t>(
                InterlockedCompareExchange(
                    &starterRequestAttempted_, 0, 0) != 0);
            void* buying = InterlockedCompareExchangePointer(
                &starterBuying_, nullptr, nullptr);
            observed.buying_captured =
                static_cast<uint8_t>(buying != nullptr);
            if (!observed.request_attempted) {
                void* resolved = nullptr;
                void* control = nullptr;
                bool namedIdentityMatched = false;
                const StarterBuyingUiState uiState =
                    ResolveStarterBuying(
                        &resolved,
                        &control,
                        &namedIdentityMatched);
                InterlockedExchange(
                    &starterBuyingUiState_,
                    static_cast<LONG>(uiState));
                InterlockedExchange(
                    &starterBuyingNamedIdentityMatched_,
                    namedIdentityMatched ? 1 : 0);
            }
            observed.buying_ui_state =
                static_cast<uint32_t>(InterlockedCompareExchange(
                    &starterBuyingUiState_, 0, 0));
            observed.buying_named_identity_matched =
                static_cast<uint8_t>(InterlockedCompareExchange(
                    &starterBuyingNamedIdentityMatched_, 0, 0) != 0);
            observed.buying_control_ready =
                static_cast<uint8_t>(
                    observed.buying_ui_state ==
                    static_cast<uint32_t>(
                        kStarterBuyingReady));
            observed.buying_validation_failed =
                static_cast<uint8_t>(
                    observed.buying_ui_state >=
                    static_cast<uint32_t>(
                        kStarterBuyingIdentityMismatch));
            void* resultPopup = InterlockedCompareExchangePointer(
                &starterResultPopup_, nullptr, nullptr);
            observed.result_popup_captured =
                static_cast<uint8_t>(resultPopup != nullptr);
            if (resultPopup) {
                __try {
                    observed.result_popup_ready =
                        *reinterpret_cast<const uint8_t*>(
                            static_cast<const uint8_t*>(resultPopup) +
                            kStarterResultPopupReady) != 0;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    observed.result_popup_ready = 0;
                }
            }
            observed.result_confirm_attempted = static_cast<uint8_t>(
                InterlockedCompareExchange(
                    &starterResultConfirmAttempted_, 0, 0) != 0);
            observed.native_fault = static_cast<uint8_t>(
                InterlockedCompareExchange(
                    &starterNativeFault_, 0, 0) != 0);
            if (native_) {
                const NativeActions::OnboardingObservation state =
                    native_->OnboardingState();
                observed.native_app_state = state.native_app_state;
                observed.tutorial_level = state.tutorial_level;
                observed.profile_pilot_id = state.profile_pilot_id;
                observed.starter_draw_busy = state.starter_draw_busy;
            } else {
                observed.native_app_state = UINT32_MAX;
            }
            const LONG after =
                InterlockedCompareExchange(&starterWriteVersion_, 0, 0);
            if (before == after && !(after & 1))
                return observed;
        }
        return observed;
    }

    // Reads the already-created CUnitInfo singleton and asks its shipped
    // read-only slot accessor for each brief record. It never invokes the
    // lazy constructor and never requests, inserts or edits inventory data.
    InventoryObservation InventoryState() const
    {
        InventoryObservation observed = {};
        if (!unitInfoPointer_ || !unitBriefBySlot_)
            return observed;

        __try {
            uint8_t* manager = *unitInfoPointer_;
            if (!manager)
                return observed;
            observed.manager_present = 1;
            observed.initialized =
                manager[kUnitInfoInitialized] == 1 ? 1 : 0;
            const int32_t capacity =
                *reinterpret_cast<const int32_t*>(
                    manager + kUnitInfoSlotCapacity);
            if (!observed.initialized || capacity < 0 ||
                capacity > static_cast<int32_t>(kMaxInventorySlots))
                return observed;

            observed.slot_capacity = static_cast<uint32_t>(capacity);
            for (int32_t slot = 1; slot <= capacity; ++slot) {
                alignas(4) uint8_t brief[kUnitBriefSize] = {};
                unitBriefBySlot_(
                    manager,
                    brief,
                    static_cast<uint16_t>(slot));
                ++observed.slots_examined;
                const uint32_t unitId =
                    *reinterpret_cast<const uint32_t*>(
                        brief + kUnitBriefUnitId);
                if (!unitId)
                    continue;
                ++observed.owned_unit_count;
                if (!observed.first_unit_id) {
                    observed.first_unit_id = unitId;
                    observed.first_unit_uid_low =
                        *reinterpret_cast<const uint32_t*>(
                            brief + kUnitBriefUidLow);
                    observed.first_unit_uid_high =
                        *reinterpret_cast<const uint32_t*>(
                            brief + kUnitBriefUidHigh);
                    observed.first_unit_slot =
                        static_cast<uint16_t>(slot);
                }
            }
            if (*unitInfoPointer_ == manager &&
                manager[kUnitInfoInitialized] == 1 &&
                *reinterpret_cast<const int32_t*>(
                    manager + kUnitInfoSlotCapacity) == capacity)
                observed.scan_complete = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            observed.native_fault = 1;
            observed.scan_complete = 0;
        }
        return observed;
    }

    InitialLoadingObservation InitialLoadingState() const
    {
        InitialLoadingObservation observed = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = InterlockedCompareExchange(
                &initialLoadingWriteVersion_, 0, 0);
            if (before & 1)
                continue;
            observed.reply_sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &initialLoadingReplySequence_, 0, 0));
            observed.confirm_sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &initialLoadingConfirmSequence_, 0, 0));
            observed.result = static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &initialLoadingResult_, 0, 0));
            observed.native_app_state =
                native_ ? native_->AppState() : UINT32_MAX;
            observed.onboarding_stage = static_cast<uint32_t>(
                InterlockedCompareExchange(&onboardingStage_, 0, 0));
            observed.pc_room_value = static_cast<uint32_t>(
                InterlockedCompareExchange(&pcRoomValue_, 0, 0));
            const uint32_t reply = static_cast<uint32_t>(
                InterlockedCompareExchange(&initialLoadingReply_, 0, 0));
            observed.reply_opcode =
                static_cast<uint16_t>(reply & 0xFFFFu);
            observed.reply_error =
                static_cast<uint16_t>(reply >> 16);
            observed.pc_room_code = static_cast<uint8_t>(
                InterlockedCompareExchange(&pcRoomCode_, 0, 0));
            void* popup = InterlockedCompareExchangePointer(
                &pcRoomPopup_, nullptr, nullptr);
            if (popup) {
                __try {
                    observed.popup_state =
                        *reinterpret_cast<const uint32_t*>(
                            static_cast<const uint8_t*>(popup) +
                            kPcRoomPopupState);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    observed.popup_state = UINT32_MAX;
                }
            }
            const LONG after = InterlockedCompareExchange(
                &initialLoadingWriteVersion_, 0, 0);
            if (before == after && !(after & 1))
                return observed;
        }
        return observed;
    }

private:
    static constexpr uint32_t kStarterMachineStage = 100;
    static constexpr uint16_t kStarterAcceptedOpcode = 0x054A;
    static constexpr uint16_t kStarterRejectedOpcode = 0x063C;
    static constexpr uint16_t kInitialLoadingAcceptedOpcode = 0x086D;
    static constexpr uint16_t kInitialLoadingRejectedOpcode = 0x077B;
    static constexpr int kUiButtonPressed = 0x0101;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kReplyError = 0x06;
    static constexpr size_t kStarterReplyTutorialLevel = 0x06;
    static constexpr size_t kStarterReplyUnitId = 0x07;
    static constexpr size_t kStarterReplyUnitSlot = 0x0B;
    static constexpr size_t kStarterReplyGp = 0x0C;
    static constexpr size_t kStarterResultPopupReady = 0x2C;
    static constexpr size_t kUnitInfoSlotCapacity = 0x0C;
    static constexpr size_t kUnitInfoInitialized = 0x20;
    static constexpr size_t kUnitBriefSize = 0x19;
    static constexpr size_t kUnitBriefUnitId = 0x00;
    static constexpr size_t kUnitBriefUidLow = 0x04;
    static constexpr size_t kUnitBriefUidHigh = 0x08;
    static constexpr uint32_t kMaxInventorySlots = UINT16_MAX;
    static constexpr size_t kPcRoomReplyCode = 0x06;
    static constexpr size_t kPcRoomReplyValue = 0x07;
    static constexpr size_t kOnboardingStage = 0x40;
    static constexpr size_t kPcRoomPopupState = 0x44;
    static constexpr uint32_t kPcRoomPopupReady = 3;
    static constexpr uint32_t kButtonControlKind = 4;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kNativeStringSize = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kControlVisible = 0x162;
    static constexpr size_t kControlEnabled = 0x167;
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
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(
            void*,
            const void*,
            uint32_t);
    using FindChildFn =
        void*(__thiscall*)(void*, const void*);
    using OnboardingUpdateFn = void(__thiscall*)(void*);
    using OnboardingCapsuleCloneFn = void*(__thiscall*)(void*);
    using StarterBuyingCloneFn = void*(__thiscall*)(void*);
    using CapsuleBuyActionFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
    using StarterResultPopupCloneFn = void*(__thiscall*)(void*);
    using StarterResultPopupActionFn =
        void(__stdcall*)(void*, int, void*, int);
    using UnitBriefBySlotFn =
        void*(__thiscall*)(void*, void*, uint16_t);
    using PacketDispatchFn =
        int(__thiscall*)(void*, int, int, int, int, void*);
    using PcRoomPopupCloneFn = void*(__thiscall*)(void*);
    using PcRoomPopupConfirmFn = void(__thiscall*)(void*);

    static bool SlotMatches(
        void* vtable,
        size_t slot,
        void* function)
    {
        return vtable && function &&
            static_cast<void**>(vtable)[slot] ==
                function;
    }

    void* FindNamedService(
        const wchar_t* name) const
    {
        if (!name || !serviceManagedPointer_ ||
            !serviceFindByName_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;
        void* manager = *serviceManagedPointer_;
        if (!manager)
            return nullptr;

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
        return service;
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

    bool ValidateControl(
        void* root,
        void* control,
        const wchar_t* name,
        uint32_t kind) const
    {
        if (!root || !control || !name)
            return false;
        if (FindControl(root, name, kind) !=
            control)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(
                control);
        return *reinterpret_cast<void* const*>(
                   bytes +
                   kControlParent) ==
                root &&
            ControlHasName(
                control,
                name);
    }

    StarterBuyingUiState ResolveStarterBuying(
        void** buyingOutput,
        void** controlOutput,
        bool* namedIdentityMatched) const
    {
        if (buyingOutput)
            *buyingOutput = nullptr;
        if (controlOutput)
            *controlOutput = nullptr;
        if (namedIdentityMatched)
            *namedIdentityMatched = false;
        if (!buyingOutput || !controlOutput ||
            !namedIdentityMatched)
            return kStarterBuyingControlMismatch;

        __try {
            void* captured =
                InterlockedCompareExchangePointer(
                    &starterBuying_,
                    nullptr,
                    nullptr);
            if (!captured)
                return kStarterBuyingAbsent;
            if (*reinterpret_cast<void**>(
                    captured) !=
                starterBuyingVtable_)
                return kStarterBuyingIdentityMismatch;

            void* named =
                FindNamedService(L"buying");
            if (!named)
                return kStarterBuyingWaiting;
            if (named != captured ||
                *reinterpret_cast<void**>(
                    named) !=
                    starterBuyingVtable_)
                return kStarterBuyingIdentityMismatch;
            *namedIdentityMatched = true;

            void* root =
                FindNamedChild(
                    named,
                    L"f_buy_dlg");
            if (!root)
                return kStarterBuyingWaiting;
            void* control =
                FindControl(
                    root,
                    L"BuyBtn",
                    kButtonControlKind);
            if (!control)
                return kStarterBuyingWaiting;
            if (!ValidateControl(
                    root,
                    control,
                    L"BuyBtn",
                    kButtonControlKind))
                return kStarterBuyingControlMismatch;
            const auto* bytes =
                static_cast<const uint8_t*>(
                    control);
            if (!bytes[kControlVisible] ||
                !bytes[kControlEnabled])
                return kStarterBuyingWaiting;

            *buyingOutput = named;
            *controlOutput = control;
            return kStarterBuyingReady;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(
                &starterNativeFault_,
                1);
            return kStarterBuyingNativeFault;
        }
    }

    static void __fastcall ObserveOnboardingUpdate(void* self, void*)
    {
        OnboardingActions* actions = active_;
        if (!actions || !actions->onboardingUpdate_)
            return;
        actions->onboardingUpdate_(self);
        LONG stage = 0;
        __try {
            if (self) {
                stage = *reinterpret_cast<const LONG*>(
                    static_cast<const uint8_t*>(self) + kOnboardingStage);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            stage = -1;
        }
        InterlockedExchange(&actions->onboardingStage_, stage);
    }

    static void* __fastcall ObserveOnboardingCapsuleClone(
        void* self,
        void*)
    {
        OnboardingActions* actions = active_;
        if (!actions || !actions->onboardingCapsuleClone_)
            return nullptr;
        void* live = actions->onboardingCapsuleClone_(self);
        if (live)
            InterlockedExchangePointer(
                &actions->onboardingCapsule_, live);
        return live;
    }

    static void* __fastcall ObserveStarterBuyingClone(
        void* self,
        void*)
    {
        OnboardingActions* actions = active_;
        if (!actions ||
            !actions->starterBuyingClone_)
            return nullptr;
        void* live =
            actions->starterBuyingClone_(self);
        if (!live ||
            InterlockedCompareExchange(
                &actions->onboardingStage_,
                0,
                0) !=
                kStarterMachineStage)
            return live;

        bool target = false;
        __try {
            const NativeActions::OnboardingObservation
                state =
                    actions->native_
                        ? actions->native_->
                              OnboardingState()
                        : NativeActions::
                              OnboardingObservation{};
            target =
                state.profile_pilot_id != 0 &&
                state.tutorial_level < 2 &&
                *reinterpret_cast<void**>(
                    self) ==
                    actions->starterBuyingVtable_ &&
                *reinterpret_cast<void**>(
                    live) ==
                    actions->starterBuyingVtable_;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(
                &actions->starterNativeFault_,
                1);
        }
        if (!target) {
            InterlockedExchange(
                &actions->starterBuyingUiState_,
                static_cast<LONG>(
                    kStarterBuyingIdentityMismatch));
            return live;
        }

        InterlockedExchange(
            &actions->starterBuyingCapturedAppState_,
            actions->native_
                ? static_cast<LONG>(
                      actions->native_->AppState())
                : -1);
        InterlockedExchangePointer(
            &actions->starterBuying_,
            live);
        InterlockedExchange(
            &actions->starterBuyingUiState_,
            static_cast<LONG>(
                kStarterBuyingWaiting));
        InterlockedExchange(
            &actions->
                starterBuyingNamedIdentityMatched_,
            0);
        InterlockedIncrement(
            &actions->starterBuyingCloneSequence_);
        return live;
    }

    static int __fastcall ObserveStarterDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        OnboardingActions* actions = active_;
        if (!actions || !actions->starterDispatch_)
            return 0;

        const int normalizedOpcode = opcode & 0xFFFF;
        uint16_t error = 0;
        uint8_t tutorialLevel = 0;
        uint8_t unitSlot = 0;
        uint32_t unitId = 0;
        uint32_t gpAward = 0;
        if (packet &&
            (normalizedOpcode == kStarterAcceptedOpcode ||
             normalizedOpcode == kStarterRejectedOpcode)) {
            __try {
                const uint8_t* bytes = static_cast<const uint8_t*>(packet);
                if (normalizedOpcode == kStarterAcceptedOpcode) {
                    tutorialLevel = bytes[kStarterReplyTutorialLevel];
                    unitId = *reinterpret_cast<const uint32_t*>(
                        bytes + kStarterReplyUnitId);
                    unitSlot = bytes[kStarterReplyUnitSlot];
                    gpAward = *reinterpret_cast<const uint32_t*>(
                        bytes + kStarterReplyGp);
                } else {
                    error = *reinterpret_cast<const uint16_t*>(
                        bytes + kReplyError);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                error = UINT16_MAX;
                tutorialLevel = 0;
                unitId = 0;
                unitSlot = 0;
                gpAward = 0;
            }
        }

        // Publish only after the shipped handler has performed its inventory,
        // GP, pending-request, UI and profile work.
        const int result =
            actions->starterDispatch_(self, a, b, c, opcode, packet);
        if (normalizedOpcode != kStarterAcceptedOpcode &&
            normalizedOpcode != kStarterRejectedOpcode)
            return result;

        if (normalizedOpcode == kStarterRejectedOpcode) {
            InterlockedCompareExchangePointer(
                &actions->onboardingCapsule_, nullptr, self);
        }
        InterlockedIncrement(&actions->starterWriteVersion_);
        InterlockedExchange(
            &actions->starterReply_,
            static_cast<LONG>(
                static_cast<uint32_t>(normalizedOpcode) |
                (static_cast<uint32_t>(error) << 16)));
        InterlockedExchange(
            &actions->starterResult_,
            normalizedOpcode == kStarterAcceptedOpcode
                ? static_cast<LONG>(kStarterAccepted)
                : static_cast<LONG>(kStarterRejected));
        InterlockedExchange(
            &actions->starterReplyTutorialLevel_,
            static_cast<LONG>(tutorialLevel));
        InterlockedExchange(
            &actions->starterUnitId_,
            static_cast<LONG>(unitId));
        InterlockedExchange(
            &actions->starterUnitSlot_,
            static_cast<LONG>(unitSlot));
        InterlockedExchange(
            &actions->starterGpAward_,
            static_cast<LONG>(gpAward));
        InterlockedIncrement(&actions->starterReplySequence_);
        InterlockedIncrement(&actions->starterWriteVersion_);
        return result;
    }

    static void* __fastcall ObserveStarterResultPopupClone(
        void* self,
        void*)
    {
        OnboardingActions* actions = active_;
        if (!actions || !actions->starterResultPopupClone_)
            return nullptr;
        void* live = actions->starterResultPopupClone_(self);
        if (live) {
            InterlockedExchange(
                &actions->starterResultPopupCapturedAppState_,
                actions->native_
                    ? static_cast<LONG>(actions->native_->AppState())
                    : -1);
            InterlockedExchangePointer(
                &actions->starterResultPopup_, live);
            InterlockedIncrement(
                &actions->starterResultPopupCloneSequence_);
        }
        return live;
    }

    static void* __fastcall ObservePcRoomPopupClone(void* self, void*)
    {
        OnboardingActions* actions = active_;
        if (!actions || !actions->pcRoomPopupClone_)
            return nullptr;
        void* live = actions->pcRoomPopupClone_(self);
        if (live)
            InterlockedExchangePointer(&actions->pcRoomPopup_, live);
        return live;
    }

    static int __fastcall ObservePcRoomPopupDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        OnboardingActions* actions = active_;
        if (!actions || !actions->pcRoomPopupDispatch_)
            return 0;

        const int normalizedOpcode = opcode & 0xFFFF;
        uint16_t error = 0;
        uint8_t pcRoomCode = 0;
        uint32_t pcRoomValue = 0;
        if (packet &&
            (normalizedOpcode == kInitialLoadingAcceptedOpcode ||
             normalizedOpcode == kInitialLoadingRejectedOpcode)) {
            __try {
                const uint8_t* bytes = static_cast<const uint8_t*>(packet);
                if (normalizedOpcode == kInitialLoadingAcceptedOpcode) {
                    pcRoomCode = bytes[kPcRoomReplyCode];
                    pcRoomValue = *reinterpret_cast<const uint32_t*>(
                        bytes + kPcRoomReplyValue);
                } else {
                    error = *reinterpret_cast<const uint16_t*>(
                        bytes + kReplyError);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                error = UINT16_MAX;
                pcRoomCode = 0;
                pcRoomValue = 0;
            }
            // A success handler closes this live popup. Never retain the
            // manager-owned pointer across the original dispatch.
            InterlockedCompareExchangePointer(
                &actions->pcRoomPopup_, nullptr, self);
        }

        const int result =
            actions->pcRoomPopupDispatch_(self, a, b, c, opcode, packet);
        if (normalizedOpcode != kInitialLoadingAcceptedOpcode &&
            normalizedOpcode != kInitialLoadingRejectedOpcode)
            return result;

        InterlockedIncrement(&actions->initialLoadingWriteVersion_);
        InterlockedExchange(
            &actions->initialLoadingReply_,
            static_cast<LONG>(
                static_cast<uint32_t>(normalizedOpcode) |
                (static_cast<uint32_t>(error) << 16)));
        InterlockedExchange(
            &actions->initialLoadingResult_,
            normalizedOpcode == kInitialLoadingAcceptedOpcode
                ? static_cast<LONG>(kInitialLoadingAccepted)
                : static_cast<LONG>(kInitialLoadingRejected));
        InterlockedExchange(
            &actions->pcRoomCode_,
            static_cast<LONG>(pcRoomCode));
        InterlockedExchange(
            &actions->pcRoomValue_,
            static_cast<LONG>(pcRoomValue));
        InterlockedIncrement(&actions->initialLoadingReplySequence_);
        InterlockedIncrement(&actions->initialLoadingWriteVersion_);
        return result;
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_ ? reinterpret_cast<Fn>(api_->Symbol(mod_, name)) : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    OnboardingUpdateFn onboardingUpdate_ = nullptr;
    OnboardingCapsuleCloneFn onboardingCapsuleClone_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    void* starterBuyingVtable_ = nullptr;
    StarterBuyingCloneFn starterBuyingClone_ = nullptr;
    CapsuleBuyActionFn capsuleBuyAction_ = nullptr;
    PacketDispatchFn starterDispatch_ = nullptr;
    StarterResultPopupCloneFn starterResultPopupClone_ = nullptr;
    StarterResultPopupActionFn starterResultPopupAction_ = nullptr;
    uint8_t** unitInfoPointer_ = nullptr;
    UnitBriefBySlotFn unitBriefBySlot_ = nullptr;
    PcRoomPopupCloneFn pcRoomPopupClone_ = nullptr;
    PcRoomPopupConfirmFn pcRoomPopupConfirm_ = nullptr;
    PacketDispatchFn pcRoomPopupDispatch_ = nullptr;
    mutable PVOID volatile onboardingCapsule_ = nullptr;
    mutable PVOID volatile starterBuying_ = nullptr;
    mutable PVOID volatile starterResultPopup_ = nullptr;
    mutable PVOID volatile pcRoomPopup_ = nullptr;
    mutable volatile LONG onboardingStage_ = 0;
    mutable volatile LONG starterRequestAttempted_ = 0;
    mutable volatile LONG starterNativeFault_ = 0;
    mutable volatile LONG starterWriteVersion_ = 0;
    mutable volatile LONG starterReplySequence_ = 0;
    mutable volatile LONG starterResultConfirmSequence_ = 0;
    mutable volatile LONG starterBuyingCloneSequence_ = 0;
    mutable volatile LONG starterBuyingCapturedAppState_ = -1;
    mutable volatile LONG starterBuyingUiState_ =
        kStarterBuyingAbsent;
    mutable volatile LONG starterBuyingNamedIdentityMatched_ = 0;
    mutable volatile LONG starterResultPopupCloneSequence_ = 0;
    mutable volatile LONG starterResultPopupCapturedAppState_ = -1;
    mutable volatile LONG starterResultConfirmAttempted_ = 0;
    mutable volatile LONG starterReply_ = 0;
    mutable volatile LONG starterResult_ = kStarterNone;
    mutable volatile LONG starterReplyTutorialLevel_ = 0;
    mutable volatile LONG starterUnitId_ = 0;
    mutable volatile LONG starterUnitSlot_ = 0;
    mutable volatile LONG starterGpAward_ = 0;
    mutable volatile LONG initialLoadingWriteVersion_ = 0;
    mutable volatile LONG initialLoadingReplySequence_ = 0;
    mutable volatile LONG initialLoadingConfirmSequence_ = 0;
    mutable volatile LONG initialLoadingReply_ = 0;
    mutable volatile LONG initialLoadingResult_ = kInitialLoadingNone;
    mutable volatile LONG pcRoomCode_ = 0;
    mutable volatile LONG pcRoomValue_ = 0;
    inline static OnboardingActions* active_ = nullptr;
};
