#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

#include "hangar_navigation.h"
#include "native_actions.h"
#include "sdgo/game_test.h"
#include "sdgo/shop_labels.h"

// Drives the mutating half of one ordinary-shop operator purchase.
//
// ShopCatalogActions owns entry, catalog selection, and the read-only option
// preview. Its validated live option popup is handed to AdoptOptionsPopup().
// This class then invokes only the two shipped UI callbacks:
//
//   option BuyBtn -> confirmation OKBtn -> 0931
//
// The shipped shop service owns every reply and automatically sends 0758 after
// a successful 074D preflight. The class observes that original transport and
// post-original shop replies. It never constructs a packet, writes a wallet,
// inserts an operator, or retries the confirmation mutation.
//
// Completion is deliberately stronger than transport success: the original
// 0574 ACK_BUY_STORE_ITEM handler must finish, the native wallet must have the
// exact debit, and the native operator inventory must contain exactly one new
// UID for the configured OperatorID.
class OperatorFlowActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kUnconfigured = 1,
        kWaitingForBaseline = 2,
        kWaitingForOptions = 3,
        kOpeningConfirmation = 4,
        kWaitingForConfirmation = 5,
        kSubmittingPurchase = 6,
        kWaitingForPreflightRequest = 7,
        kWaitingForPreflightReply = 8,
        kWaitingForAutomaticCommit = 9,
        kWaitingForPurchaseAck = 10,
        kWaitingForNativeReadback = 11,
        kComplete = 12,

        kInvalidContext = 0x100,
        kRunChanged = 0x101,
        kThreadChanged = 0x102,
        kProcessChanged = 0x103,
        kTimeMovedBackward = 0x104,
        kConfigurationRejected = 0x105,
        kBaselineRejected = 0x106,
        kOptionsMismatch = 0x107,
        kOptionsControlMismatch = 0x108,
        kConfirmationMismatch = 0x109,
        kConfirmationControlMismatch = 0x10A,
        kNativeFault = 0x10B,
        kMutationAttemptSealed = 0x10C,
        kPreflightRequestRejected = 0x10D,
        kPreflightReplyRejected = 0x10E,
        kAutomaticCommitRejected = 0x10F,
        kPurchaseAckRejected = 0x110,
        kServerRejected = 0x111,
        kWalletMismatch = 0x112,
        kOperatorReadbackMismatch = 0x113,
        kTimedOut = 0x114,
        kObservationOrderMismatch = 0x115,
    };

    enum Stage : uint32_t
    {
        kStageIdle = 0,
        kStageBaseline,
        kStageValidateOptions,
        kStageInvokeOptions,
        kStageWaitConfirmation,
        kStageInvokeConfirmation,
        kStageWait0931,
        kStageWait074D,
        kStageWait0758,
        kStageWait0574,
        kStageReadback,
        kStageDone,
        kStageFailed,
    };

    struct Config
    {
        uint32_t operator_id = 0;
        uint32_t product_id = 0;
        uint32_t option_id = 0;
        uint32_t category_id = 0;
        int32_t price = 0;
        int32_t validity = 0;
        uint8_t payment = 0; // native ordinary-shop value: GP/GB=1, cash=2
        uint8_t reserved[3] = {};
        uint64_t ui_timeout_ms = 15000;
        uint64_t network_timeout_ms = 30000;
        uint64_t readback_timeout_ms = 15000;
    };

    struct WalletImage
    {
        uint32_t gp = 0;
        uint32_t cash = 0;
    };

    struct OperatorRecord
    {
        uint64_t uid = 0;
        uint32_t operator_id = 0;
        uint16_t slot = 0;
        uint8_t enabled = 0;
        uint8_t reserved = 0;
    };

    struct InventoryImage
    {
        std::vector<OperatorRecord> records;
    };

    struct Observation
    {
        uint32_t result = kUnbound;
        uint32_t stage = kStageIdle;
        uint32_t operator_id = 0;
        uint32_t product_id = 0;
        uint32_t option_id = 0;
        uint32_t category_id = 0;
        int32_t price = 0;
        int32_t validity = 0;
        uint32_t payment = 0;
        uint32_t baseline_gp = 0;
        uint32_t baseline_cash = 0;
        uint32_t current_gp = 0;
        uint32_t current_cash = 0;
        uint32_t baseline_operator_count = 0;
        uint32_t current_operator_count = 0;
        uint64_t new_operator_uid = 0;
        uint32_t options_actions = 0;
        uint32_t confirmation_actions = 0;
        uint32_t request_0931_sequence = 0;
        uint32_t request_0758_sequence = 0;
        uint32_t reply_074d_sequence = 0;
        uint32_t reply_0574_sequence = 0;
        uint32_t rejected_reply_sequence = 0;
        uint32_t managed_active_count = 0;
        uint32_t managed_pending_count = 0;
        uint32_t managed_active_matches = 0;
        uint32_t managed_pending_matches = 0;
        uint32_t managed_expected_vtable_rva = 0;
        uint32_t managed_pending_first_vtable_rva = 0;
        uint32_t managed_pending_first_native_state =
            std::numeric_limits<uint32_t>::max();
        uint32_t confirmation_native_state =
            std::numeric_limits<uint32_t>::max();
        uint32_t confirmation_product_observed = 0;
        uint32_t confirmation_option_observed = 0;
        uint32_t confirmation_readiness = 0;
        uint32_t popup_scan_diagnostic = 0;
        uint16_t rejected_reply_opcode = 0;
        uint16_t rejected_reply_status = 0;
        uint8_t baseline_ready = 0;
        uint8_t options_validated = 0;
        uint8_t confirmation_validated = 0;
        uint8_t mutation_attempted = 0;
        uint8_t original_0574_completed = 0;
        uint8_t wallet_verified = 0;
        uint8_t operator_verified = 0;
        uint8_t managed_pending_first_matches_expected = 0;
    };

    using UiCallbackFn =
        void(__stdcall*)(void*, uint32_t, const void*, void*);
    using TransportStateFn =
        NativeActions::TransportObservation(__cdecl*)(void*);
    using FindTransportSinceFn =
        NativeActions::TransportObservation(__cdecl*)(
            void*,
            uint32_t,
            uint16_t);
    using ReadInventoryFn =
        bool(__cdecl*)(void*, InventoryImage*);

    struct Bindings
    {
        uint8_t** profile_pointer = nullptr;
        void** service_managed_pointer = nullptr;
        uintptr_t image_base = 0;
        void* options_vtable = nullptr;
        UiCallbackFn options_callback = nullptr;
        void* confirmation_vtable = nullptr;
        UiCallbackFn confirmation_callback = nullptr;
        void* transport_context = nullptr;
        TransportStateFn transport_state = nullptr;
        FindTransportSinceFn find_transport_since = nullptr;
        void* inventory_context = nullptr;
        ReadInventoryFn read_inventory = nullptr;
    };

    bool Configure(const Config& config)
    {
        if (started_ || MutationAttempted() ||
            !config.operator_id || !config.product_id ||
            !config.option_id || !config.category_id ||
            config.price <= 0 ||
            (config.payment != 1 && config.payment != 2) ||
            !config.ui_timeout_ms ||
            !config.network_timeout_ms ||
            !config.readback_timeout_ms) {
            Publish(kConfigurationRejected);
            return false;
        }
        config_ = config;
        configured_ = true;
        CopyConfigToObservation();
        Publish(bound_ ? kWaitingForBaseline : kUnbound);
        return true;
    }

    bool Bind(const Bindings& bindings)
    {
        if (bound_ || started_ || MutationAttempted())
            return false;
        bindings_ = bindings;
        if (!bindings_.profile_pointer ||
            !bindings_.service_managed_pointer ||
            !bindings_.options_vtable ||
            !bindings_.options_callback ||
            !bindings_.confirmation_vtable ||
            !bindings_.confirmation_callback ||
            !bindings_.transport_context ||
            !bindings_.transport_state ||
            !bindings_.find_transport_since ||
            !bindings_.inventory_context ||
            !bindings_.read_inventory)
            return false;

        __try {
            if (!CallbackMatches(
                    bindings_.options_vtable,
                    bindings_.options_callback) ||
                !CallbackMatches(
                    bindings_.confirmation_vtable,
                    bindings_.confirmation_callback))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        bound_ = true;
        Publish(configured_ ? kWaitingForBaseline : kUnconfigured);
        return true;
    }

    bool Bind(
        const Bindings& bindings,
        NativeActions* native)
    {
        Bindings completed = bindings;
        if (native) {
            completed.transport_context = native;
            completed.transport_state =
                &NativeTransportState;
            completed.find_transport_since =
                &NativeFindTransportSince;
        }
        return Bind(completed);
    }

    // Capture this before entering the shop. A repeated baseline cannot replace
    // the identity set after any UI action has started.
    bool PrimeBaseline(
        const WalletImage& wallet,
        const InventoryImage& inventory)
    {
        if (started_ || MutationAttempted() ||
            !configured_ ||
            !InventoryValid(inventory))
            return false;
        if ((config_.payment == 1 &&
             wallet.gp <
                 static_cast<uint32_t>(
                     config_.price)) ||
            (config_.payment == 2 &&
             wallet.cash <
                 static_cast<uint32_t>(
                     config_.price)))
            return false;
        if (CountOperator(
                inventory,
                config_.operator_id) != 0)
            return false;

        baselineWallet_ = wallet;
        baselineInventory_ = inventory;
        baselineReady_ = true;
        observation_.baseline_ready = 1;
        observation_.baseline_gp = wallet.gp;
        observation_.baseline_cash = wallet.cash;
        observation_.baseline_operator_count =
            static_cast<uint32_t>(
                inventory.records.size());
        Publish(bound_ ? kWaitingForOptions : kUnbound);
        return true;
    }

    // This pointer must be the still-live popup already validated by
    // ShopCatalogActions. Ownership remains with the shipped service manager.
    bool AdoptOptionsPopup(void* popup)
    {
        if (!bound_ || !configured_ ||
            !baselineReady_ || started_ ||
            MutationAttempted() || !popup)
            return false;
        optionsPopup_ = popup;
        started_ = true;
        stage_ = kStageValidateOptions;
        return true;
    }

    Result Poll(const SdgoGameTestContextV1* context)
    {
        if (stage_ == kStageFailed ||
            stage_ == kStageDone)
            return static_cast<Result>(
                observation_.result);
        const Result identity = CheckContext(context);
        if (identity != kComplete)
            return Fail(identity);
        if (!bound_)
            return Publish(kUnbound);
        if (!configured_)
            return Publish(kUnconfigured);
        if (!baselineReady_)
            return Publish(kWaitingForBaseline);
        if (!started_)
            return Publish(kWaitingForOptions);
        if ((stage_ == kStageValidateOptions ||
             stage_ == kStageInvokeOptions) &&
            !optionsPopup_)
            return Fail(kOptionsMismatch);

        switch (stage_) {
        case kStageValidateOptions:
            return ValidateOptions();
        case kStageInvokeOptions:
            return InvokeOptions();
        case kStageWaitConfirmation:
            return WaitForConfirmation();
        case kStageInvokeConfirmation:
            return InvokeConfirmation();
        case kStageWait0931:
            return WaitFor0931();
        case kStageWait074D:
            return WaitFor074D();
        case kStageWait0758:
            return WaitFor0758();
        case kStageWait0574:
            return WaitFor0574();
        case kStageReadback:
            return VerifyReadback();
        case kStageDone:
            return Publish(kComplete);
        case kStageFailed:
            return static_cast<Result>(
                observation_.result);
        default:
            return Fail(kNativeFault);
        }
    }

    // Call only after the original shop dispatch has returned. A shared
    // exact-service hook may feed this method; no packet-dispatch hook is
    // installed here.
    bool ObserveShopReply(
        uint16_t opcode,
        const void* packet,
        uint32_t packetSize,
        bool originalCompleted)
    {
        if (!started_ || !MutationAttempted() ||
            !packet ||
            !originalCompleted)
            return false;

        uint16_t status = 0;
        if (packetSize >= 8)
            memcpy(
                &status,
                static_cast<const uint8_t*>(
                    packet) +
                    6,
                sizeof(status));

        if (opcode == kReply074D) {
            const auto request =
                bindings_.find_transport_since(
                    bindings_.transport_context,
                    requestBaseline_,
                    kRequest0931);
            if (packetSize < k074DMinimumSize ||
                !request.send_sequence ||
                request.packet_size !=
                    kRequest0931Size ||
                request.result == 0)
                return false;
            const uint32_t sequence =
                static_cast<uint32_t>(
                    InterlockedIncrement(
                        &shopReplySequence_));
            uint32_t product = 0;
            uint32_t option = 0;
            memcpy(
                &product,
                static_cast<const uint8_t*>(
                    packet) +
                    6,
                sizeof(product));
            memcpy(
                &option,
                static_cast<const uint8_t*>(
                    packet) +
                    10,
                sizeof(option));
            if (product != config_.product_id ||
                option != config_.option_id) {
                InterlockedExchange(
                    &reply074DIdentityMismatch_,
                    1);
                return true;
            }
            InterlockedExchange(
                &reply074DSequence_,
                static_cast<LONG>(sequence));
            return true;
        }
        if (opcode == kReply0574) {
            const auto commit =
                bindings_.find_transport_since(
                    bindings_.transport_context,
                    commitBaseline_,
                    kRequest0758);
            if (packetSize < k0574MinimumSize ||
                !commit.send_sequence ||
                commit.packet_size !=
                    kRequest0758Size ||
                commit.result == 0)
                return false;
            const uint32_t sequence =
                static_cast<uint32_t>(
                    InterlockedIncrement(
                        &shopReplySequence_));
            InterlockedExchange(
                &reply0574Sequence_,
                static_cast<LONG>(sequence));
            InterlockedExchange(
                &reply0574OriginalCompleted_,
                1);
            return true;
        }
        if (opcode == 0x05D8 ||
            opcode == 0x05DA ||
            opcode == 0x09BD) {
            const uint32_t sequence =
                static_cast<uint32_t>(
                    InterlockedIncrement(
                        &shopReplySequence_));
            InterlockedExchange(
                &rejectedReplySequence_,
                static_cast<LONG>(sequence));
            InterlockedExchange(
                &rejectedReply_,
                static_cast<LONG>(
                    (static_cast<uint32_t>(
                         opcode)
                     << 16) |
                    status));
            return true;
        }
        return false;
    }

    Observation State() const
    {
        Observation value = observation_;
        value.stage = stage_;
        value.mutation_attempted =
            MutationAttempted() ? 1 : 0;
        value.reply_074d_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &reply074DSequence_),
                    0,
                    0));
        value.reply_0574_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &reply0574Sequence_),
                    0,
                    0));
        value.original_0574_completed =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &reply0574OriginalCompleted_),
                0,
                0) != 0;
        const uint32_t rejected =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &rejectedReply_),
                    0,
                    0));
        value.rejected_reply_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &rejectedReplySequence_),
                    0,
                    0));
        value.rejected_reply_opcode =
            static_cast<uint16_t>(
                rejected >> 16);
        value.rejected_reply_status =
            static_cast<uint16_t>(
                rejected);
        return value;
    }

    bool ConfirmationReadyForCapture() const
    {
        if (stage_ != kStageInvokeConfirmation ||
            MutationAttempted() ||
            !observation_.confirmation_validated ||
            !confirmationPopup_ ||
            !confirmationOk_)
            return false;
        __try {
            if (!HasVtable(
                    confirmationPopup_,
                    bindings_.confirmation_vtable))
                return false;
            uint32_t nativeState = 0;
            void* root = nullptr;
            void* ok = nullptr;
            uint32_t product = 0;
            uint32_t option = 0;
            if (!Read(
                    confirmationPopup_,
                    kPopupNativeState,
                    &nativeState) ||
                !Read(
                    confirmationPopup_,
                    kPopupRoot,
                    &root) ||
                !Read(
                    confirmationPopup_,
                    kConfirmationOk,
                    &ok) ||
                !Read(
                    confirmationPopup_,
                    kConfirmationProduct,
                    &product) ||
                !Read(
                    confirmationPopup_,
                    kConfirmationOption,
                    &option))
                return false;
            const uint32_t readiness =
                InspectConfirmationReadiness(
                    root,
                    ok,
                    product,
                    option);
            return nativeState == 3 &&
                   root &&
                   ok == confirmationOk_ &&
                   product == config_.product_id &&
                   option == config_.option_id &&
                   (readiness &
                    kConfirmationReadyMask) ==
                       kConfirmationReadyMask &&
                   ControlIdentity(
                       ok,
                       root,
                       L"OKBtn");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // A timed-out or uncertain mutation remains sealed for this object.
    bool ResetRun()
    {
        if (MutationAttempted()) {
            Fail(kMutationAttemptSealed);
            return false;
        }
        started_ = false;
        stage_ = kStageIdle;
        optionsPopup_ = nullptr;
        optionsBuy_ = nullptr;
        confirmationPopup_ = nullptr;
        confirmationOk_ = nullptr;
        contextCaptured_ = false;
        runHash_ = 0;
        uiThread_ = 0;
        processId_ = 0;
        lastElapsed_ = 0;
        stageStartedAt_ = 0;
        requestBaseline_ = 0;
        commitBaseline_ = 0;
        InterlockedExchange(
            &shopReplySequence_,
            0);
        InterlockedExchange(
            &reply074DSequence_,
            0);
        InterlockedExchange(
            &reply074DIdentityMismatch_,
            0);
        InterlockedExchange(
            &reply0574Sequence_,
            0);
        InterlockedExchange(
            &reply0574OriginalCompleted_,
            0);
        InterlockedExchange(
            &rejectedReplySequence_,
            0);
        InterlockedExchange(
            &rejectedReply_,
            0);
        observation_ = {};
        CopyConfigToObservation();
        observation_.baseline_ready =
            baselineReady_ ? 1 : 0;
        observation_.baseline_gp =
            baselineWallet_.gp;
        observation_.baseline_cash =
            baselineWallet_.cash;
        observation_.baseline_operator_count =
            static_cast<uint32_t>(
                baselineInventory_.records.size());
        Publish(
            !bound_
                ? kUnbound
                : !configured_
                      ? kUnconfigured
                      : !baselineReady_
                            ? kWaitingForBaseline
                            : kWaitingForOptions);
        return true;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kUnconfigured: return "unconfigured";
        case kWaitingForBaseline: return "waiting-for-baseline";
        case kWaitingForOptions: return "waiting-for-options";
        case kOpeningConfirmation: return "opening-confirmation";
        case kWaitingForConfirmation: return "waiting-for-confirmation";
        case kSubmittingPurchase: return "submitting-purchase";
        case kWaitingForPreflightRequest: return "waiting-for-0931";
        case kWaitingForPreflightReply: return "waiting-for-074d";
        case kWaitingForAutomaticCommit: return "waiting-for-0758";
        case kWaitingForPurchaseAck: return "waiting-for-0574";
        case kWaitingForNativeReadback: return "waiting-for-native-readback";
        case kComplete: return "complete";
        case kInvalidContext: return "invalid-context";
        case kRunChanged: return "run-changed";
        case kThreadChanged: return "thread-changed";
        case kProcessChanged: return "process-changed";
        case kTimeMovedBackward: return "time-moved-backward";
        case kConfigurationRejected: return "configuration-rejected";
        case kBaselineRejected: return "baseline-rejected";
        case kOptionsMismatch: return "options-mismatch";
        case kOptionsControlMismatch: return "options-control-mismatch";
        case kConfirmationMismatch: return "confirmation-mismatch";
        case kConfirmationControlMismatch: return "confirmation-control-mismatch";
        case kNativeFault: return "native-fault";
        case kMutationAttemptSealed: return "mutation-attempt-sealed";
        case kPreflightRequestRejected: return "0931-rejected";
        case kPreflightReplyRejected: return "074d-rejected";
        case kAutomaticCommitRejected: return "0758-rejected";
        case kPurchaseAckRejected: return "0574-rejected";
        case kServerRejected: return "server-rejected";
        case kWalletMismatch: return "wallet-mismatch";
        case kOperatorReadbackMismatch: return "operator-readback-mismatch";
        case kTimedOut: return "timed-out";
        case kObservationOrderMismatch: return "observation-order-mismatch";
        default: return "unknown";
        }
    }

    // Exact readback helper for the shipped CServiceOperatorInventory layout.
    // The caller must supply the live "OpInv" service after its original page
    // handler has populated +C0/+C4.
    static bool ReadOperatorInventoryService(
        void* service,
        InventoryImage* output)
    {
        if (!service || !output)
            return false;
        uintptr_t begin = 0;
        uintptr_t end = 0;
        if (!ReadOperatorVectorBounds(
                service,
                &begin,
                &end) ||
            !begin || end < begin ||
            (end - begin) %
                    kOperatorRecordStride !=
                0)
            return false;
        const size_t count =
            (end - begin) /
            kOperatorRecordStride;
        if (count > kMaximumOperators)
            return false;
        InventoryImage image;
        image.records.reserve(count);
        for (size_t index = 0;
             index < count;
             ++index) {
            const void* record =
                reinterpret_cast<const void*>(
                    begin +
                    index *
                        kOperatorRecordStride);
            OperatorRecord value;
            if (!ReadOperatorRecord(
                    record,
                    &value))
                return false;
            if (!value.uid &&
                !value.operator_id) {
                if (value.enabled != 0 ||
                    value.slot != index + 1)
                    return false;
                continue;
            }
            if (!value.uid ||
                !value.operator_id)
                return false;
            image.records.push_back(value);
        }
        if (!InventoryValid(image))
            return false;
        *output = image;
        return true;
    }

private:
    static constexpr uint32_t kClickEvent = 0x101;
    static constexpr size_t kCallbackSlot =
        0x38 / sizeof(void*);
    static constexpr size_t kPopupNativeState = 0x44;
    static constexpr size_t kPopupRoot = 0x4C;
    static constexpr size_t kOptionsCombo = 0x50;
    static constexpr size_t kOptionsBuy = 0x5C;
    static constexpr size_t kOptionsBegin = 0xB0;
    static constexpr size_t kOptionsEnd = 0xB4;
    static constexpr size_t kComboSelected = 0x21C;
    static constexpr size_t kConfirmationOk = 0x78;
    static constexpr size_t kConfirmationProduct = 0x84;
    static constexpr size_t kConfirmationOption = 0x8C;
    // The native confirmation root at popup+0x4C has no stable ControlName.
    // Its identity is the exact root pointer plus the OKBtn parent relation.
    // Bit 4 remains diagnostic if a build happens to name the root.
    static constexpr uint32_t kConfirmationReadyMask = 0x1EF;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kControlVisible = 0x162;
    static constexpr size_t kControlEnabled = 0x167;
    static constexpr size_t kNativeStringLength = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;
    static constexpr size_t kManagerActiveSentinel = 0x44;
    static constexpr size_t kManagerActiveCount = 0x48;
    static constexpr size_t kManagerPopupSentinel = 0x54;
    static constexpr size_t kManagerPopupCount = 0x58;
    static constexpr size_t kProfileGp = 0x18F;
    static constexpr size_t kProfileCash = 0x58C;
    static constexpr size_t kOperatorRecordsBegin = 0xC0;
    static constexpr size_t kOperatorRecordsEnd = 0xC4;
    static constexpr size_t kOperatorRecordStride = 0x50;
    static constexpr size_t kMaximumOperators = 4096;
    // The active list contains the complete native service set, while the
    // pending list contains only queued dialogs. Keep the manager-wide bound
    // aligned with the other native service-list readers and retain the
    // tighter bound for pending popups.
    static constexpr size_t kMaximumManagedServiceCount = 512;
    static constexpr size_t kMaximumPopupCount = 16;
    static constexpr uint16_t kRequest0931 = 0x0931;
    static constexpr uint16_t kRequest0758 = 0x0758;
    static constexpr uint16_t kReply074D = 0x074D;
    static constexpr uint16_t kReply0574 = 0x0574;
    static constexpr uint32_t kRequest0931Size = 0x0E;
    static constexpr uint32_t kRequest0758Size = 0x39;
    static constexpr uint32_t k074DMinimumSize = 0x33;
    static constexpr uint32_t k0574MinimumSize = 0xD4;

    static NativeActions::TransportObservation __cdecl
    NativeTransportState(void* context)
    {
        return static_cast<NativeActions*>(
                   context)
            ->TransportState();
    }

    static NativeActions::TransportObservation __cdecl
    NativeFindTransportSince(
        void* context,
        uint32_t baseline,
        uint16_t opcode)
    {
        return static_cast<NativeActions*>(
                   context)
            ->FindTransportSince(
                baseline,
                opcode);
    }

    static bool CallbackMatches(
        void* vtable,
        UiCallbackFn callback)
    {
        return vtable && callback &&
               reinterpret_cast<void**>(
                   vtable)[kCallbackSlot] ==
                   reinterpret_cast<void*>(
                       callback);
    }

    static bool InventoryValid(
        const InventoryImage& inventory)
    {
        if (inventory.records.size() >
            kMaximumOperators)
            return false;
        for (size_t index = 0;
             index < inventory.records.size();
             ++index) {
            const OperatorRecord& record =
                inventory.records[index];
            if (!record.uid ||
                !record.operator_id ||
                !record.slot ||
                record.enabled > 1)
                return false;
            for (size_t other = 0;
                 other < index;
                 ++other) {
                if (inventory.records[other].uid ==
                        record.uid ||
                    inventory.records[other].slot ==
                        record.slot)
                    return false;
            }
        }
        return true;
    }

    static bool ReadOperatorVectorBounds(
        const void* service,
        uintptr_t* begin,
        uintptr_t* end)
    {
        if (!service || !begin || !end)
            return false;
        __try {
            *begin =
                *reinterpret_cast<
                    const uintptr_t*>(
                    static_cast<
                        const uint8_t*>(
                        service) +
                    kOperatorRecordsBegin);
            *end =
                *reinterpret_cast<
                    const uintptr_t*>(
                    static_cast<
                        const uint8_t*>(
                        service) +
                    kOperatorRecordsEnd);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadOperatorRecord(
        const void* source,
        OperatorRecord* output)
    {
        if (!source || !output)
            return false;
        __try {
            const uint8_t* record =
                static_cast<const uint8_t*>(
                    source);
            OperatorRecord value;
            memcpy(
                &value.uid,
                record,
                sizeof(value.uid));
            memcpy(
                &value.operator_id,
                record + 8,
                sizeof(value.operator_id));
            memcpy(
                &value.slot,
                record + 0x10,
                sizeof(value.slot));
            value.enabled = record[0x12];
            *output = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static size_t CountOperator(
        const InventoryImage& inventory,
        uint32_t operatorId)
    {
        return static_cast<size_t>(
            std::count_if(
                inventory.records.begin(),
                inventory.records.end(),
                [operatorId](
                    const OperatorRecord& record) {
                    return record.operator_id ==
                           operatorId;
                }));
    }

    static bool ContainsUid(
        const InventoryImage& inventory,
        uint64_t uid)
    {
        return std::any_of(
            inventory.records.begin(),
            inventory.records.end(),
            [uid](
                const OperatorRecord& record) {
                return record.uid == uid;
            });
    }

    bool MutationAttempted() const
    {
        return InterlockedCompareExchange(
                   const_cast<volatile LONG*>(
                       &mutationAttempted_),
                   0,
                   0) != 0;
    }

    Result ValidateOptions()
    {
        __try {
            if (!HasVtable(
                    optionsPopup_,
                    bindings_.options_vtable))
                return Fail(kOptionsMismatch);
            uint32_t nativeState = 0;
            void* root = nullptr;
            void* combo = nullptr;
            void* buy = nullptr;
            uintptr_t begin = 0;
            uintptr_t end = 0;
            int32_t selected = -1;
            if (!Read(
                    optionsPopup_,
                    kPopupNativeState,
                    &nativeState) ||
                !Read(
                    optionsPopup_,
                    kPopupRoot,
                    &root) ||
                !Read(
                    optionsPopup_,
                    kOptionsCombo,
                    &combo) ||
                !Read(
                    optionsPopup_,
                    kOptionsBuy,
                    &buy) ||
                !Read(
                    optionsPopup_,
                    kOptionsBegin,
                    &begin) ||
                !Read(
                    optionsPopup_,
                    kOptionsEnd,
                    &end) ||
                !combo ||
                !Read(
                    combo,
                    kComboSelected,
                    &selected))
                return Fail(kNativeFault);
            if (nativeState != 4 || !root ||
                !buy || !begin || end < begin ||
                (end - begin) %
                        sizeof(sdgo::shop::Option) !=
                    0)
                return Fail(kOptionsMismatch);
            const size_t count =
                (end - begin) /
                sizeof(sdgo::shop::Option);
            if (!count || count > 4096 ||
                selected < 0 ||
                static_cast<size_t>(selected) >=
                    count)
                return Fail(kOptionsMismatch);
            if (!ControlIdentity(
                    buy,
                    root,
                    L"BuyBtn"))
                return Fail(
                    kOptionsControlMismatch);
            sdgo::shop::Option option = {};
            memcpy(
                &option,
                reinterpret_cast<const void*>(
                    begin +
                    static_cast<size_t>(
                        selected) *
                        sizeof(option)),
                sizeof(option));
            if (option.productId !=
                    config_.product_id ||
                option.id != config_.option_id ||
                option.category !=
                    config_.category_id ||
                option.price != config_.price ||
                option.validity !=
                    config_.validity ||
                option.payment !=
                    config_.payment)
                return Fail(kOptionsMismatch);
            optionsBuy_ = buy;
            observation_.options_validated = 1;
            stage_ = kStageInvokeOptions;
            BeginStage(config_.ui_timeout_ms);
            return Publish(
                kOpeningConfirmation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Fail(kNativeFault);
        }
    }

    Result InvokeOptions()
    {
        if (Expired(config_.ui_timeout_ms))
            return Fail(kTimedOut);
        __try {
            if (!optionsPopup_ ||
                !optionsBuy_ ||
                !HasVtable(
                    optionsPopup_,
                    bindings_.options_vtable))
                return Fail(kOptionsMismatch);
            uint32_t nativeState = 0;
            void* root = nullptr;
            void* combo = nullptr;
            void* buy = nullptr;
            uintptr_t begin = 0;
            uintptr_t end = 0;
            int32_t selected = -1;
            if (!Read(
                    optionsPopup_,
                    kPopupNativeState,
                    &nativeState) ||
                !Read(
                    optionsPopup_,
                    kPopupRoot,
                    &root) ||
                !Read(
                    optionsPopup_,
                    kOptionsCombo,
                    &combo) ||
                !Read(
                    optionsPopup_,
                    kOptionsBuy,
                    &buy) ||
                !Read(
                    optionsPopup_,
                    kOptionsBegin,
                    &begin) ||
                !Read(
                    optionsPopup_,
                    kOptionsEnd,
                    &end) ||
                nativeState != 4 ||
                !root || !combo ||
                buy != optionsBuy_ ||
                !ControlIdentity(
                    buy,
                    root,
                    L"BuyBtn") ||
                !Read(
                    combo,
                    kComboSelected,
                    &selected) ||
                !begin || end < begin ||
                (end - begin) %
                        sizeof(sdgo::shop::Option) !=
                    0)
                return Fail(
                    kOptionsControlMismatch);
            const size_t count =
                (end - begin) /
                sizeof(sdgo::shop::Option);
            if (selected < 0 ||
                static_cast<size_t>(selected) >=
                    count)
                return Fail(kOptionsMismatch);
            sdgo::shop::Option option = {};
            memcpy(
                &option,
                reinterpret_cast<const void*>(
                    begin +
                    static_cast<size_t>(selected) *
                        sizeof(option)),
                sizeof(option));
            if (option.productId !=
                    config_.product_id ||
                option.id != config_.option_id ||
                option.category !=
                    config_.category_id ||
                option.price != config_.price ||
                option.validity !=
                    config_.validity ||
                option.payment !=
                    config_.payment)
                return Fail(kOptionsMismatch);
            bindings_.options_callback(
                optionsPopup_,
                kClickEvent,
                static_cast<uint8_t*>(
                    optionsBuy_) +
                    kControlName,
                optionsBuy_);
            ++observation_.options_actions;
            optionsPopup_ = nullptr;
            optionsBuy_ = nullptr;
            stage_ = kStageWaitConfirmation;
            BeginStage(config_.ui_timeout_ms);
            return Publish(
                kWaitingForConfirmation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Fail(kNativeFault);
        }
    }

    Result WaitForConfirmation()
    {
        if (Expired(config_.ui_timeout_ms))
            return Fail(kTimedOut);
        observation_.confirmation_native_state =
            std::numeric_limits<uint32_t>::max();
        observation_.confirmation_product_observed = 0;
        observation_.confirmation_option_observed = 0;
        observation_.confirmation_readiness = 0;
        void* popup = nullptr;
        const PopupScan scan =
            FindManagedPopup(
                bindings_.confirmation_vtable,
                &popup);
        if (scan == kPopupFault)
            return Fail(kNativeFault);
        if (scan == kPopupMismatched)
            return Fail(kConfirmationMismatch);
        if (scan == kPopupAbsent)
            return Publish(
                kWaitingForConfirmation);

        __try {
            uint32_t nativeState = 0;
            void* root = nullptr;
            void* ok = nullptr;
            uint32_t product = 0;
            uint32_t option = 0;
            if (!Read(
                    popup,
                    kPopupNativeState,
                    &nativeState) ||
                !Read(
                    popup,
                    kPopupRoot,
                    &root) ||
                !Read(
                    popup,
                    kConfirmationOk,
                    &ok) ||
                !Read(
                    popup,
                    kConfirmationProduct,
                    &product) ||
                !Read(
                    popup,
                    kConfirmationOption,
                    &option))
                return Fail(kNativeFault);
            observation_.confirmation_native_state =
                nativeState;
            observation_.
                confirmation_product_observed =
                    product;
            observation_.
                confirmation_option_observed =
                    option;
            observation_.confirmation_readiness =
                InspectConfirmationReadiness(
                    root,
                    ok,
                    product,
                    option);
            if (nativeState < 3)
                return Publish(
                    kWaitingForConfirmation);
            if (nativeState != 3 ||
                product != config_.product_id ||
                option != config_.option_id ||
                !root || !ok)
                return Fail(
                    kConfirmationMismatch);
            if ((observation_.
                    confirmation_readiness &
                    kConfirmationReadyMask) !=
                    kConfirmationReadyMask ||
                !ControlIdentity(
                    ok,
                    root,
                    L"OKBtn"))
                return Fail(
                    kConfirmationControlMismatch);
            confirmationPopup_ = popup;
            confirmationOk_ = ok;
            observation_.
                confirmation_validated = 1;
            stage_ =
                kStageInvokeConfirmation;
            return Publish(
                kSubmittingPurchase);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Fail(kNativeFault);
        }
    }

    Result InvokeConfirmation()
    {
        if (MutationAttempted())
            return Fail(
                kMutationAttemptSealed);
        if (Expired(config_.ui_timeout_ms))
            return Fail(kTimedOut);
        __try {
            if (!confirmationPopup_ ||
                !confirmationOk_ ||
                !HasVtable(
                    confirmationPopup_,
                    bindings_.confirmation_vtable))
                return Fail(
                    kConfirmationMismatch);
            uint32_t nativeState = 0;
            void* root = nullptr;
            void* ok = nullptr;
            uint32_t product = 0;
            uint32_t option = 0;
            if (!Read(
                    confirmationPopup_,
                    kPopupNativeState,
                    &nativeState) ||
                !Read(
                    confirmationPopup_,
                    kPopupRoot,
                    &root) ||
                !Read(
                    confirmationPopup_,
                    kConfirmationOk,
                    &ok) ||
                !Read(
                    confirmationPopup_,
                    kConfirmationProduct,
                    &product) ||
                !Read(
                    confirmationPopup_,
                    kConfirmationOption,
                    &option))
                return Fail(
                    kConfirmationControlMismatch);
            observation_.confirmation_native_state =
                nativeState;
            observation_.
                confirmation_product_observed =
                    product;
            observation_.
                confirmation_option_observed =
                    option;
            observation_.confirmation_readiness =
                InspectConfirmationReadiness(
                    root,
                    ok,
                    product,
                    option);
            if (nativeState != 3 ||
                !root ||
                ok != confirmationOk_ ||
                product != config_.product_id ||
                option != config_.option_id ||
                (observation_.
                    confirmation_readiness &
                    kConfirmationReadyMask) !=
                    kConfirmationReadyMask ||
                !ControlIdentity(
                    ok,
                    root,
                    L"OKBtn"))
                return Fail(
                    kConfirmationControlMismatch);
            requestBaseline_ =
                bindings_.transport_state(
                    bindings_.
                        transport_context)
                    .send_sequence;
            InterlockedExchange(
                &mutationAttempted_,
                1);
            observation_.mutation_attempted = 1;
            bindings_.confirmation_callback(
                confirmationPopup_,
                kClickEvent,
                static_cast<uint8_t*>(
                    confirmationOk_) +
                    kControlName,
                confirmationOk_);
            ++observation_.
                confirmation_actions;
            confirmationPopup_ = nullptr;
            confirmationOk_ = nullptr;
            stage_ = kStageWait0931;
            BeginStage(
                config_.network_timeout_ms);
            return Publish(
                kWaitingForPreflightRequest);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Fail(kNativeFault);
        }
    }

    Result WaitFor0931()
    {
        if (RejectedReply())
            return Fail(kServerRejected);
        const auto sent =
            bindings_.find_transport_since(
                bindings_.transport_context,
                requestBaseline_,
                kRequest0931);
        if (!sent.send_sequence) {
            if (Expired(
                    config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForPreflightRequest);
        }
        observation_.request_0931_sequence =
            sent.send_sequence;
        if (sent.packet_size !=
                kRequest0931Size ||
            sent.result == 0)
            return Fail(
                kPreflightRequestRejected);
        stage_ = kStageWait074D;
        BeginStage(
            config_.network_timeout_ms);
        return Publish(
            kWaitingForPreflightReply);
    }

    Result WaitFor074D()
    {
        if (RejectedReply())
            return Fail(kServerRejected);
        if (InterlockedCompareExchange(
                &reply074DIdentityMismatch_,
                0,
                0) != 0)
            return Fail(
                kPreflightReplyRejected);
        const uint32_t sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &reply074DSequence_,
                    0,
                    0));
        if (!sequence) {
            if (Expired(
                    config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForPreflightReply);
        }
        observation_.reply_074d_sequence =
            sequence;
        commitBaseline_ =
            observation_.request_0931_sequence;
        stage_ = kStageWait0758;
        BeginStage(
            config_.network_timeout_ms);
        return Publish(
            kWaitingForAutomaticCommit);
    }

    Result WaitFor0758()
    {
        if (RejectedReply())
            return Fail(kServerRejected);
        const auto sent =
            bindings_.find_transport_since(
                bindings_.transport_context,
                commitBaseline_,
                kRequest0758);
        if (!sent.send_sequence) {
            if (Expired(
                    config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForAutomaticCommit);
        }
        if (sent.send_sequence <=
                observation_.
                    request_0931_sequence)
            return Fail(
                kObservationOrderMismatch);
        observation_.request_0758_sequence =
            sent.send_sequence;
        if (sent.packet_size !=
                kRequest0758Size ||
            sent.result == 0)
            return Fail(
                kAutomaticCommitRejected);
        stage_ = kStageWait0574;
        BeginStage(
            config_.network_timeout_ms);
        return Publish(
            kWaitingForPurchaseAck);
    }

    Result WaitFor0574()
    {
        if (RejectedReply())
            return Fail(kServerRejected);
        const uint32_t sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &reply0574Sequence_,
                    0,
                    0));
        if (!sequence) {
            if (Expired(
                    config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForPurchaseAck);
        }
        if (sequence <=
                observation_.reply_074d_sequence ||
            InterlockedCompareExchange(
                &reply0574OriginalCompleted_,
                0,
                0) == 0)
            return Fail(
                kObservationOrderMismatch);
        observation_.reply_0574_sequence =
            sequence;
        observation_.
            original_0574_completed = 1;
        stage_ = kStageReadback;
        BeginStage(
            config_.readback_timeout_ms);
        return Publish(
            kWaitingForNativeReadback);
    }

    Result VerifyReadback()
    {
        WalletImage wallet;
        InventoryImage inventory;
        if (!ReadWallet(&wallet) ||
            !bindings_.read_inventory(
                bindings_.inventory_context,
                &inventory) ||
            !InventoryValid(inventory)) {
            if (Expired(
                    config_.readback_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForNativeReadback);
        }
        observation_.current_gp = wallet.gp;
        observation_.current_cash =
            wallet.cash;
        observation_.current_operator_count =
            static_cast<uint32_t>(
                inventory.records.size());

        const uint32_t price =
            static_cast<uint32_t>(
                config_.price);
        const uint32_t expectedGp =
            config_.payment == 1
                ? baselineWallet_.gp - price
                : baselineWallet_.gp;
        const uint32_t expectedCash =
            config_.payment == 2
                ? baselineWallet_.cash - price
                : baselineWallet_.cash;
        if (wallet.gp != expectedGp ||
            wallet.cash != expectedCash) {
            if (Expired(
                    config_.readback_timeout_ms))
                return Fail(kWalletMismatch);
            return Publish(
                kWaitingForNativeReadback);
        }
        observation_.wallet_verified = 1;

        std::vector<uint64_t> additions;
        for (const OperatorRecord& record :
             inventory.records) {
            if (!ContainsUid(
                    baselineInventory_,
                    record.uid))
                additions.push_back(record.uid);
        }
        if (additions.size() != 1 ||
            inventory.records.size() !=
                baselineInventory_.
                        records.size() +
                    1)
            return ReadbackMismatch();
        const auto found = std::find_if(
            inventory.records.begin(),
            inventory.records.end(),
            [this, &additions](
                const OperatorRecord& record) {
                return record.uid ==
                           additions.front() &&
                       record.operator_id ==
                           config_.operator_id;
            });
        if (found == inventory.records.end() ||
            CountOperator(
                inventory,
                config_.operator_id) != 1)
            return ReadbackMismatch();
        for (const OperatorRecord& before :
             baselineInventory_.records) {
            const auto after = std::find_if(
                inventory.records.begin(),
                inventory.records.end(),
                [&before](
                    const OperatorRecord& value) {
                    return value.uid ==
                           before.uid;
                });
            if (after == inventory.records.end() ||
                after->operator_id !=
                    before.operator_id)
                return ReadbackMismatch();
        }
        observation_.new_operator_uid =
            additions.front();
        observation_.operator_verified = 1;
        stage_ = kStageDone;
        return Publish(kComplete);
    }

    Result ReadbackMismatch()
    {
        if (Expired(
                config_.readback_timeout_ms))
            return Fail(
                kOperatorReadbackMismatch);
        return Publish(
            kWaitingForNativeReadback);
    }

    enum PopupScan
    {
        kPopupAbsent,
        kPopupMatched,
        kPopupMismatched,
        kPopupFault,
    };

    struct ManagedListProbe
    {
        void* first_vtable = nullptr;
        uint32_t first_native_state =
            std::numeric_limits<uint32_t>::max();
        uint32_t matches = 0;
        uint8_t first_matches_expected = 0;
    };

    PopupScan FindManagedPopup(
        void* expectedVtable,
        void** output)
    {
        observation_.popup_scan_diagnostic = 0;
        observation_.managed_active_matches = 0;
        observation_.managed_pending_matches = 0;
        observation_.managed_expected_vtable_rva =
            PointerRva(expectedVtable);
        observation_.managed_pending_first_vtable_rva = 0;
        observation_.managed_pending_first_native_state =
            std::numeric_limits<uint32_t>::max();
        observation_.
            managed_pending_first_matches_expected = 0;
        if (!output) {
            observation_.popup_scan_diagnostic = 1;
            return kPopupFault;
        }
        *output = nullptr;
        __try {
            uint8_t* manager =
                bindings_.
                        service_managed_pointer
                    ? static_cast<uint8_t*>(
                          *bindings_.
                               service_managed_pointer)
                    : nullptr;
            if (!manager) {
                observation_.popup_scan_diagnostic = 2;
                return kPopupFault;
            }
            observation_.managed_active_count =
                *reinterpret_cast<uint32_t*>(
                    manager + kManagerActiveCount);
            observation_.managed_pending_count =
                *reinterpret_cast<uint32_t*>(
                    manager + kManagerPopupCount);
            void* active = nullptr;
            ManagedListProbe activeProbe;
            const PopupScan activeScan =
                ScanManagedList(
                    manager,
                    kManagerActiveSentinel,
                    kManagerActiveCount,
                    kMaximumManagedServiceCount,
                    expectedVtable,
                    &active,
                    &activeProbe);
            observation_.managed_active_matches =
                activeProbe.matches;
            if (activeScan == kPopupFault ||
                activeScan == kPopupMismatched) {
                observation_.popup_scan_diagnostic =
                    activeScan == kPopupFault ? 3 : 4;
                return activeScan;
            }
            void* pending = nullptr;
            ManagedListProbe pendingProbe;
            const PopupScan pendingScan =
                ScanManagedList(
                    manager,
                    kManagerPopupSentinel,
                    kManagerPopupCount,
                    kMaximumPopupCount,
                    expectedVtable,
                    &pending,
                    &pendingProbe);
            observation_.managed_pending_matches =
                pendingProbe.matches;
            observation_.
                managed_pending_first_vtable_rva =
                    PointerRva(
                        pendingProbe.first_vtable);
            observation_.
                managed_pending_first_native_state =
                    pendingProbe.first_native_state;
            observation_.
                managed_pending_first_matches_expected =
                    pendingProbe.
                        first_matches_expected;
            if (pendingScan == kPopupFault ||
                pendingScan == kPopupMismatched) {
                observation_.popup_scan_diagnostic =
                    pendingScan == kPopupFault ? 5 : 6;
                return pendingScan;
            }
            if (active && pending &&
                active != pending) {
                observation_.popup_scan_diagnostic = 7;
                return kPopupMismatched;
            }
            *output = active ? active : pending;
            if (!*output)
                return kPopupAbsent;
            return kPopupMatched;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            observation_.popup_scan_diagnostic = 8;
            return kPopupFault;
        }
    }

    static PopupScan ScanManagedList(
        uint8_t* manager,
        size_t sentinelOffset,
        size_t countOffset,
        size_t maximumCount,
        void* expectedVtable,
        void** output,
        ManagedListProbe* probe)
    {
        if (!manager || !expectedVtable ||
            !output || !probe)
            return kPopupFault;
        *output = nullptr;
        *probe = {};
        void* sentinel =
            *reinterpret_cast<void**>(
                manager + sentinelOffset);
        const uint32_t count =
            *reinterpret_cast<uint32_t*>(
                manager + countOffset);
        if (!sentinel ||
            count > maximumCount)
            return kPopupFault;
        void* previous = sentinel;
        void* node =
            *reinterpret_cast<void**>(
                sentinel);
        uint32_t seen = 0;
        while (node != sentinel) {
            if (!node || seen >= count ||
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(
                        node) +
                    4) != previous)
                return kPopupFault;
            void* service =
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(
                        node) +
                    8);
            if (!service)
                return kPopupFault;
            void* serviceVtable =
                *reinterpret_cast<void**>(
                    service);
            const bool matches =
                serviceVtable == expectedVtable;
            if (!seen) {
                probe->first_vtable =
                    serviceVtable;
                probe->first_matches_expected =
                    matches ? 1 : 0;
                TryReadDiagnostic(
                    service,
                    kPopupNativeState,
                    &probe->first_native_state);
            }
            if (matches) {
                ++probe->matches;
                if (*output)
                    return kPopupMismatched;
                *output = service;
            }
            previous = node;
            node =
                *reinterpret_cast<void**>(
                    node);
            ++seen;
        }
        if (seen != count ||
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(
                    sentinel) +
                4) != previous)
            return kPopupFault;
        return *output
            ? kPopupMatched
            : kPopupAbsent;
    }

    uint32_t PointerRva(
        const void* pointer) const
    {
        if (!pointer || !bindings_.image_base)
            return 0;
        const uintptr_t value =
            reinterpret_cast<uintptr_t>(
                pointer);
        if (value < bindings_.image_base)
            return 0;
        const uintptr_t delta =
            value - bindings_.image_base;
        if (delta >
            std::numeric_limits<
                uint32_t>::max())
            return 0;
        return static_cast<uint32_t>(delta);
    }

    static bool TryReadDiagnostic(
        const void* base,
        size_t offset,
        uint32_t* output)
    {
        if (!base || !output)
            return false;
        __try {
            *output =
                *reinterpret_cast<
                    const uint32_t*>(
                    static_cast<
                        const uint8_t*>(
                        base) +
                    offset);
            return true;
        } __except (
            EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    uint32_t InspectConfirmationReadiness(
        const void* root,
        const void* ok,
        uint32_t product,
        uint32_t option) const
    {
        // Bits deliberately expose every condition required immediately
        // before the original confirmation callback:
        // 0 root, 1 OK pointer, 2 product, 3 option, 4 optional root name,
        // 5 OK parent, 6 visible, 7 enabled, 8 OK name.
        uint32_t readiness = 0;
        if (root)
            readiness |= 1u << 0;
        if (ok)
            readiness |= 1u << 1;
        if (product == config_.product_id)
            readiness |= 1u << 2;
        if (option == config_.option_id)
            readiness |= 1u << 3;
        __try {
            if (root &&
                ControlName(root, L"dlg"))
                readiness |= 1u << 4;
            if (ok) {
                void* owner = nullptr;
                uint8_t visible = 0;
                uint8_t enabled = 0;
                if (Read(
                        ok,
                        kControlParent,
                        &owner) &&
                    owner == root)
                    readiness |= 1u << 5;
                if (Read(
                        ok,
                        kControlVisible,
                        &visible) &&
                    visible == 1)
                    readiness |= 1u << 6;
                if (Read(
                        ok,
                        kControlEnabled,
                        &enabled) &&
                    enabled == 1)
                    readiness |= 1u << 7;
                if (ControlName(
                        ok,
                        L"OKBtn"))
                    readiness |= 1u << 8;
            }
        } __except (
            EXCEPTION_EXECUTE_HANDLER) {
            return readiness;
        }
        return readiness;
    }

    template <typename T>
    static bool Read(
        const void* base,
        size_t offset,
        T* output)
    {
        if (!base || !output)
            return false;
        memcpy(
            output,
            static_cast<const uint8_t*>(
                base) +
                offset,
            sizeof(T));
        return true;
    }

    static bool HasVtable(
        const void* object,
        const void* expected)
    {
        return object && expected &&
               *reinterpret_cast<
                   void* const*>(object) ==
                   expected;
    }

    static bool ControlName(
        const void* control,
        const wchar_t* expected)
    {
        if (!control || !expected)
            return false;
        const uint8_t* storage =
            static_cast<const uint8_t*>(
                control) +
            kControlName;
        const uint32_t length =
            *reinterpret_cast<
                const uint32_t*>(
                storage +
                kNativeStringLength);
        const uint32_t capacity =
            *reinterpret_cast<
                const uint32_t*>(
                storage +
                kNativeStringCapacity);
        if (length > 255 ||
            capacity < length)
            return false;
        const wchar_t* text =
            capacity < 8
                ? reinterpret_cast<
                      const wchar_t*>(
                      storage)
                : *reinterpret_cast<
                      wchar_t* const*>(
                      storage);
        return text &&
               std::wcslen(expected) ==
                   length &&
               std::wmemcmp(
                   text,
                   expected,
                   length) == 0 &&
               text[length] == L'\0';
    }

    static bool ControlIdentity(
        const void* control,
        const void* parent,
        const wchar_t* expected)
    {
        void* owner = nullptr;
        uint8_t visible = 0;
        uint8_t enabled = 0;
        return Read(
                   control,
                   kControlParent,
                   &owner) &&
               Read(
                   control,
                   kControlVisible,
                   &visible) &&
               Read(
                   control,
                   kControlEnabled,
                   &enabled) &&
               owner == parent &&
               visible == 1 &&
               enabled == 1 &&
               ControlName(control, expected);
    }

    bool ReadWallet(WalletImage* output) const
    {
        if (!output ||
            !bindings_.profile_pointer)
            return false;
        __try {
            const uint8_t* profile =
                *bindings_.profile_pointer;
            if (!profile)
                return false;
            WalletImage image;
            memcpy(
                &image.gp,
                profile + kProfileGp,
                sizeof(image.gp));
            memcpy(
                &image.cash,
                profile + kProfileCash,
                sizeof(image.cash));
            *output = image;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Result CheckContext(
        const SdgoGameTestContextV1* context)
    {
        if (!context ||
            context->size !=
                sizeof(SdgoGameTestContextV1) ||
            context->version !=
                SDGO_GAME_TEST_VERSION ||
            !context->run_id ||
            !context->role ||
            !context->case_id)
            return kInvalidContext;
        const uint64_t hash =
            HashContext(context);
        const DWORD thread =
            GetCurrentThreadId();
        const DWORD process =
            GetCurrentProcessId();
        if (!contextCaptured_) {
            contextCaptured_ = true;
            runHash_ = hash;
            uiThread_ = thread;
            processId_ = process;
            lastElapsed_ = context->elapsed_ms;
            stageStartedAt_ =
                context->elapsed_ms;
            return kComplete;
        }
        if (process != processId_)
            return kProcessChanged;
        if (thread != uiThread_)
            return kThreadChanged;
        if (hash != runHash_)
            return kRunChanged;
        if (context->elapsed_ms <
            lastElapsed_)
            return kTimeMovedBackward;
        lastElapsed_ = context->elapsed_ms;
        return kComplete;
    }

    static uint64_t HashString(
        uint64_t hash,
        const char* text)
    {
        while (text && *text) {
            hash ^=
                static_cast<uint8_t>(*text++);
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFF;
        hash *= 1099511628211ULL;
        return hash;
    }

    static uint64_t HashContext(
        const SdgoGameTestContextV1* context)
    {
        uint64_t hash =
            1469598103934665603ULL;
        hash = HashString(
            hash,
            context->run_id);
        hash = HashString(hash, context->role);
        return HashString(
            hash,
            context->case_id);
    }

    void BeginStage(uint64_t)
    {
        stageStartedAt_ = lastElapsed_;
    }

    bool Expired(uint64_t timeout) const
    {
        return lastElapsed_ >=
                   stageStartedAt_ &&
               lastElapsed_ -
                       stageStartedAt_ >=
                   timeout;
    }

    bool RejectedReply()
    {
        const LONG sequence =
            InterlockedCompareExchange(
                &rejectedReplySequence_,
                0,
                0);
        if (!sequence)
            return false;
        const uint32_t rejected =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &rejectedReply_,
                    0,
                    0));
        observation_.rejected_reply_sequence =
            static_cast<uint32_t>(sequence);
        observation_.rejected_reply_opcode =
            static_cast<uint16_t>(
                rejected >> 16);
        observation_.rejected_reply_status =
            static_cast<uint16_t>(rejected);
        return true;
    }

    void CopyConfigToObservation()
    {
        observation_.operator_id =
            config_.operator_id;
        observation_.product_id =
            config_.product_id;
        observation_.option_id =
            config_.option_id;
        observation_.category_id =
            config_.category_id;
        observation_.price = config_.price;
        observation_.validity =
            config_.validity;
        observation_.payment =
            config_.payment;
    }

    Result Publish(Result result)
    {
        observation_.result = result;
        observation_.stage = stage_;
        return result;
    }

    Result Fail(Result result)
    {
        stage_ = kStageFailed;
        return Publish(result);
    }

    Config config_ = {};
    Bindings bindings_ = {};
    Observation observation_ = {};
    WalletImage baselineWallet_ = {};
    InventoryImage baselineInventory_;
    Stage stage_ = kStageIdle;
    bool configured_ = false;
    bool bound_ = false;
    bool baselineReady_ = false;
    bool started_ = false;
    bool contextCaptured_ = false;
    uint64_t runHash_ = 0;
    DWORD uiThread_ = 0;
    DWORD processId_ = 0;
    uint64_t lastElapsed_ = 0;
    uint64_t stageStartedAt_ = 0;
    uint32_t requestBaseline_ = 0;
    uint32_t commitBaseline_ = 0;
    void* optionsPopup_ = nullptr;
    void* optionsBuy_ = nullptr;
    void* confirmationPopup_ = nullptr;
    void* confirmationOk_ = nullptr;

    volatile LONG mutationAttempted_ = 0;
    volatile LONG shopReplySequence_ = 0;
    volatile LONG reply074DSequence_ = 0;
    volatile LONG reply074DIdentityMismatch_ = 0;
    volatile LONG reply0574Sequence_ = 0;
    volatile LONG reply0574OriginalCompleted_ = 0;
    volatile LONG rejectedReplySequence_ = 0;
    volatile LONG rejectedReply_ = 0;
};

// Opens and drives the shipped operator inventory UI without constructing a
// service, popup, packet, inventory record, or selection flag. The caller owns
// transport/reply observation. Independent one-shot seals prevent retrying the
// original nickname and activation callbacks after their first invocation.
class OperatorInventoryActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForHangar = 1,
        kOpeningInventory = 2,
        kWaitingForInventory = 3,
        kReady = 4,
        kNavigatingPage = 5,
        kWaitingForNamingPopup = 6,
        kNamingSubmitted = 7,
        kNameAlreadyApplied = 8,
        kActivationSubmitted = 9,
        kAlreadyActive = 10,

        kNavigationRejected = 0x100,
        kMainframeMismatch = 0x101,
        kOperatorRadioMismatch = 0x102,
        kInventoryMismatch = 0x103,
        kInventoryControlMismatch = 0x104,
        kTargetMissing = 0x105,
        kTargetIdentityMismatch = 0x106,
        kPageControlMismatch = 0x107,
        kNamingPopupMismatch = 0x108,
        kNamingControlMismatch = 0x109,
        kNicknameRejected = 0x10A,
        kNativeSelectionRejected = 0x10B,
        kActivationControlMismatch = 0x10C,
        kMutationAttemptSealed = 0x10D,
        kNativeFault = 0x10E,
    };

    using UiCallbackFn =
        void(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
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
    using EditSetTextFn =
        void(__thiscall*)(
            void*,
            const void*,
            int);
    using EditGetTextFn =
        const void*(__thiscall*)(void*);
    using ListMouseBusinessFn =
        uint8_t(__thiscall*)(
            void*,
            uint32_t,
            int32_t,
            int32_t,
            uint32_t,
            uint32_t);
    using ListItemByIndexFn =
        void*(__thiscall*)(void*, int);

    struct Bindings
    {
        uint8_t** profile_pointer = nullptr;
        void** service_managed_pointer = nullptr;
        ServiceFindByNameFn service_find_by_name =
            nullptr;
        NativeWStringCtorFn native_wstring_ctor =
            nullptr;
        NativeWStringDtorFn native_wstring_dtor =
            nullptr;
        NativeUiFindControlFn
            native_ui_find_control = nullptr;
        EditSetTextFn edit_set_text = nullptr;
        EditGetTextFn edit_get_text = nullptr;
        void* mainframe_vtable = nullptr;
        UiCallbackFn mainframe_callback = nullptr;
        void* inventory_vtable = nullptr;
        UiCallbackFn inventory_callback = nullptr;
        void* input_vtable = nullptr;
        UiCallbackFn input_callback = nullptr;
        ListMouseBusinessFn list_mouse_business =
            nullptr;
        ListItemByIndexFn list_item_by_index =
            nullptr;
    };

    struct Observation
    {
        uint32_t result = kUnbound;
        uint32_t inventory_open_actions = 0;
        uint32_t page_actions = 0;
        uint32_t naming_actions = 0;
        uint32_t selection_actions = 0;
        uint32_t activation_actions = 0;
        uint32_t current_page = 0;
        uint32_t target_page = 0;
        uint32_t target_operator_id = 0;
        uint64_t target_uid = 0;
        uint16_t target_slot = 0;
        uint8_t inventory_ready = 0;
        uint8_t naming_sealed = 0;
        uint8_t activation_sealed = 0;
        uint8_t row_selected = 0;
        uint8_t page_control_found = 0;
        uint8_t page_control_parent = 0;
        uint8_t page_control_name = 0;
        uint8_t page_control_visible = 0;
        uint8_t page_control_enabled = 0;
        uint8_t page_reply_count = 0;
        uint8_t page_reply_empty = 0;
        uint32_t page_control_kind = 0;
        uint32_t page_control_address = 0;
        uint32_t page_control_parent_address = 0;
        uint32_t page_control_root_address = 0;
        uint32_t page_reply_sequence = 0;
        uint32_t page_reply_page = UINT32_MAX;
        uint32_t page_reply_size = 0;
        char page_control_actual_name[24] = {};
    };

    struct InputPopupInspection {
        uint32_t code = 0, list = 0, active_count = 0, pending_count = 0;
        uint32_t expected_vtable = 0, found_operator = 0, native_state = 0;
        uint64_t found_uid = 0;
    };
    InputPopupInspection InputPopupState() const { return inputInspection_; }

    bool Bind(
        const Bindings& bindings,
        HangarNavigation* navigation)
    {
        if (bound_)
            return false;
        bindings_ = bindings;
        navigation_ = navigation;
        if (!navigation_ ||
            !bindings_.profile_pointer ||
            !bindings_.service_managed_pointer ||
            !bindings_.service_find_by_name ||
            !bindings_.native_wstring_ctor ||
            !bindings_.native_wstring_dtor ||
            !bindings_.native_ui_find_control ||
            !bindings_.edit_set_text ||
            !bindings_.edit_get_text ||
            !bindings_.mainframe_vtable ||
            !bindings_.mainframe_callback ||
            !bindings_.inventory_vtable ||
            !bindings_.inventory_callback ||
            !bindings_.input_vtable ||
            !bindings_.input_callback ||
            !bindings_.list_mouse_business ||
            !bindings_.list_item_by_index)
            return false;
        __try {
            if (!CallbackMatches(
                    bindings_.mainframe_vtable,
                    bindings_.mainframe_callback) ||
                !CallbackMatches(
                    bindings_.inventory_vtable,
                    bindings_.inventory_callback) ||
                !CallbackMatches(
                    bindings_.input_vtable,
                    bindings_.input_callback))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        bound_ = true;
        return Publish(kWaitingForHangar) !=
               kUnbound;
    }

    Result EnsureInventory(
        void** liveService = nullptr)
    {
        if (liveService)
            *liveService = nullptr;
        if (!bound_ || !navigation_)
            return Publish(kUnbound);
        __try {
            void* inventory = nullptr;
            const ServiceResult inventoryResult =
                ResolveService(
                    L"OpInv",
                    bindings_.inventory_vtable,
                    &inventory);
            if (inventoryResult ==
                kServiceMismatched)
                return Publish(
                    kInventoryMismatch);
            if (inventoryResult ==
                kServiceMatched) {
                auto* bytes =
                    static_cast<uint8_t*>(
                        inventory);
                void* root =
                    *reinterpret_cast<void**>(
                        bytes +
                        kInventoryRoot);
                void* list =
                    root
                        ? FindControl(
                              root,
                              L"List",
                              kListKind)
                        : nullptr;
                if (!root || !list)
                    return Publish(
                        kWaitingForInventory);
                if (!ControlIdentity(
                        list,
                        root,
                        L"List") ||
                    !static_cast<uint8_t*>(
                         root)[kControlVisible] ||
                    !static_cast<uint8_t*>(
                         list)[kControlVisible] ||
                    !static_cast<uint8_t*>(
                         list)[kControlEnabled])
                    return Publish(
                        kInventoryControlMismatch);
                if (liveInventory_ != inventory ||
                    liveRoot_ != root ||
                    liveList_ != list) {
                    pagePending_ = false;
                    pageDataPending_ = true;
                    InterlockedExchange(
                        &pageExpectedReplyPage_,
                        -1);
                }
                liveInventory_ = inventory;
                liveRoot_ = root;
                liveList_ = list;
                openingInvoked_ = false;
                observation_.inventory_ready = 1;
                if (liveService)
                    *liveService = inventory;
                return Publish(kReady);
            }

            liveInventory_ = nullptr;
            liveRoot_ = nullptr;
            liveList_ = nullptr;
            observation_.inventory_ready = 0;
            if (openingInvoked_)
                return Publish(
                    kWaitingForInventory);

            const HangarNavigation::Result hangar =
                navigation_->EnsureHangar();
            if (hangar !=
                HangarNavigation::kReady) {
                if (hangar >=
                    HangarNavigation::
                        kDownmenuServiceMismatch)
                    return Publish(
                        kNavigationRejected);
                return Publish(
                    kWaitingForHangar);
            }

            void* mainframe = nullptr;
            const ServiceResult mainframeResult =
                ResolveService(
                    L"mainframe",
                    bindings_.mainframe_vtable,
                    &mainframe);
            if (mainframeResult ==
                kServiceMismatched)
                return Publish(
                    kMainframeMismatch);
            if (mainframeResult !=
                kServiceMatched)
                return Publish(
                    kWaitingForHangar);
            auto* mainframeBytes =
                static_cast<uint8_t*>(
                    mainframe);
            void* root =
                *reinterpret_cast<void**>(
                    mainframeBytes +
                    kMainframeRoot);
            void* radio =
                root
                    ? FindControl(
                          root,
                          L"OperatorRadio",
                          kRadioKind)
                    : nullptr;
            if (!root || !radio)
                return Publish(
                    kWaitingForHangar);
            if (!ControlIdentity(
                    radio,
                    root,
                    L"OperatorRadio") ||
                !static_cast<uint8_t*>(
                     root)[kControlVisible] ||
                !static_cast<uint8_t*>(
                     radio)[kControlVisible] ||
                !static_cast<uint8_t*>(
                     radio)[kControlEnabled])
                return Publish(
                    kOperatorRadioMismatch);
            openingInvoked_ = true;
            ++observation_.
                inventory_open_actions;
            bindings_.mainframe_callback(
                mainframe,
                kRadioEvent,
                static_cast<uint8_t*>(
                    radio) +
                    kControlName,
                radio);
            return Publish(
                kOpeningInventory);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    Result ReadFullInventory(
        OperatorFlowActions::InventoryImage* output)
    {
        if (!output)
            return Publish(kInventoryMismatch);
        void* service = nullptr;
        const Result ready =
            EnsureInventory(&service);
        if (ready != kReady)
            return ready;
        if (!inventoryScanActive_ ||
            inventoryScanService_ != service) {
            ResetInventoryScan();
            inventoryScanActive_ = true;
            inventoryScanService_ = service;
        }

        uint32_t current = 0;
        uint32_t maximum = 0;
        if (!ReadPageBounds(
                &current,
                &maximum)) {
            ResetInventoryScan();
            return Publish(
                kTargetIdentityMismatch);
        }
        if (inventoryScanMaximumPage_ < 0) {
            inventoryScanMaximumPage_ =
                static_cast<int32_t>(
                    maximum);
            inventoryScanSeenPages_.assign(
                maximum + 1,
                0);
        } else if (
            static_cast<uint32_t>(
                inventoryScanMaximumPage_) !=
            maximum) {
            ResetInventoryScan();
            return Publish(
                kTargetIdentityMismatch);
        }

        const uint32_t target =
            inventoryScanNextPage_;
        if (target > maximum) {
            ResetInventoryScan();
            return Publish(
                kTargetIdentityMismatch);
        }
        const Result page =
            EnsurePage(target);
        if (page != kReady)
            return page;

        OperatorFlowActions::InventoryImage
            pageImage;
        const PageImageResult read =
            ReadPageImage(
                target,
                maximum,
                &pageImage);
        if (read == kPageImageWaiting)
            return Publish(
                kWaitingForInventory);
        if (read != kPageImageReady ||
            target >=
                inventoryScanSeenPages_.
                    size() ||
            inventoryScanSeenPages_[
                target]) {
            ResetInventoryScan();
            return Publish(
                kTargetIdentityMismatch);
        }
        inventoryScanSeenPages_[
            target] = 1;

        if (target < maximum) {
            inventoryScanNextPage_ =
                target + 1;
            return Publish(
                kNavigatingPage);
        }

        if (!std::all_of(
                inventoryScanSeenPages_.
                    begin(),
                inventoryScanSeenPages_.
                    end(),
                [](uint8_t seen) {
                    return seen == 1;
                })) {
            ResetInventoryScan();
            return Publish(
                kTargetIdentityMismatch);
        }
        std::sort(
            pageImage.records.
                begin(),
            pageImage.records.
                end(),
            [](const auto& left,
               const auto& right) {
                return left.slot <
                       right.slot;
            });
        knownInventory_ = pageImage;
        *output = knownInventory_;
        ResetInventoryScan();
        return Publish(kReady);
    }

    void ObservePageReply(
        uint16_t opcode,
        const uint8_t* packet,
        uint32_t size)
    {
        if (opcode != kPageReply ||
            !packet || size < 7)
            return;
        __try {
            const uint8_t count = packet[6];
            if (count > kRecordsPerPage ||
                size !=
                    7u +
                        static_cast<uint32_t>(
                            count) *
                            kRecordStride)
                return;
            uint32_t page = UINT32_MAX;
            uint16_t seenSlots[
                kRecordsPerPage] = {};
            const int32_t expectedPage =
                InterlockedCompareExchange(
                    &pageExpectedReplyPage_,
                    0,
                    0);
            if (!count) {
                uint32_t current = 0;
                uint32_t maximum = 0;
                if (expectedPage < 0 ||
                    !ReadPageBounds(
                        &current,
                        &maximum) ||
                    current !=
                        static_cast<uint32_t>(
                            expectedPage) ||
                    current > maximum)
                    return;
                page = current;
            }
            for (uint32_t index = 0;
                 index < count;
                 ++index) {
                const uint8_t* record =
                    packet + 7 +
                    index * kRecordStride;
                uint64_t uid = 0;
                uint32_t operatorId = 0;
                uint16_t slot = 0;
                memcpy(
                    &uid,
                    record,
                    sizeof(uid));
                memcpy(
                    &operatorId,
                    record +
                        kRecordOperatorId,
                    sizeof(operatorId));
                memcpy(
                    &slot,
                    record +
                        kRecordSlot,
                    sizeof(slot));
                if (!uid || !operatorId ||
                    !slot)
                    return;
                const uint32_t recordPage =
                    (slot - 1) /
                    kRecordsPerPage;
                if (recordPage >=
                        kMaximumPages ||
                    (page != UINT32_MAX &&
                     page != recordPage))
                    return;
                for (uint32_t other = 0;
                     other < index;
                     ++other)
                    if (seenSlots[other] ==
                        slot)
                        return;
                seenSlots[index] = slot;
                page = recordPage;
            }
            if (expectedPage >= 0 &&
                page != static_cast<uint32_t>(
                            expectedPage))
                return;
            InterlockedExchange(
                &pageReplyCount_,
                static_cast<LONG>(count));
            InterlockedExchange(
                &pageReplySize_,
                static_cast<LONG>(size));
            InterlockedExchange(
                &pageReplyEmpty_,
                count ? 0 : 1);
            InterlockedExchange(
                &pageReplyPage_,
                static_cast<LONG>(page));
            MemoryBarrier();
            InterlockedIncrement(
                &pageReplySequence_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    static bool __cdecl ReadInventory(
        void* context,
        OperatorFlowActions::
            InventoryImage* output)
    {
        auto* self =
            static_cast<
                OperatorInventoryActions*>(
                context);
        if (!self || !output)
            return false;
        return self->ReadFullInventory(
                   output) == kReady;
    }

    Result SubmitNickname(
        uint32_t operatorId,
        uint64_t uid,
        const wchar_t* nickname)
    {
        if (!operatorId || !uid ||
            !NicknameValid(nickname))
            return Publish(
                kNicknameRejected);
        if (InterlockedCompareExchange(
                &namingSealed_,
                0,
                0) != 0)
            return Publish(
                kMutationAttemptSealed);
        const Result ready =
            EnsureInventory();
        if (ready != kReady)
            return ready;
        __try {
            OperatorFlowActions::OperatorRecord
                known;
            const Result knownResult =
                FindKnownTarget(
                    operatorId,
                    uid,
                    &known);
            if (knownResult != kReady)
                return Publish(
                    knownResult);
            SetTarget(
                operatorId,
                uid,
                known.slot);
            const Result page =
                EnsureTargetPage(
                    known.slot);
            if (page != kReady)
                return page;

            uint8_t* record = nullptr;
            uint16_t slot = 0;
            const Result target =
                FindTarget(
                    operatorId,
                    uid,
                    &record,
                    &slot);
            if (target != kReady)
                return Publish(target);
            if (slot != known.slot)
                return Publish(
                    kTargetIdentityMismatch);
            if (!RecordNicknameTerminated(
                    record))
                return Publish(
                    kTargetIdentityMismatch);

            void* popup = nullptr;
            const PopupResult popupResult =
                FindInputPopup(
                    operatorId,
                    uid,
                    &popup);
            if (popupResult == kPopupAbsent) {
                if (RecordNicknameEquals(
                        record,
                        nickname))
                    return Publish(
                        kNameAlreadyApplied);
                return Publish(
                    kWaitingForNamingPopup);
            }
            if (popupResult != kPopupMatched)
                return Publish(
                    kNamingPopupMismatch);

            void* root =
                ResolvePopupRoot(
                    popup,
                    L"dlg");
            void* edit =
                root
                    ? FindControl(
                          root,
                          L"Edit",
                          kEditKind)
                    : nullptr;
            void* button =
                root
                    ? FindControl(
                          root,
                          L"Btn2",
                          kButtonKind)
                    : nullptr;
            if (!root || !edit || !button ||
                !ControlIdentity(
                    edit,
                    root,
                    L"Edit") ||
                !ControlIdentity(
                    button,
                    root,
                    L"Btn2"))
                return Publish(
                    kNamingControlMismatch);

            if (!SetEditText(
                    edit,
                    nickname))
                return Publish(
                    kNicknameRejected);

            InterlockedExchange(
                &namingSealed_,
                1);
            observation_.naming_sealed = 1;
            ++observation_.naming_actions;
            bindings_.input_callback(
                popup,
                kClickEvent,
                static_cast<uint8_t*>(
                    button) +
                    kControlName,
                button);
            return Publish(
                kNamingSubmitted);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    bool NamingPopupReady(
        uint32_t operatorId,
        uint64_t uid) const
    {
        if (!bound_ || !operatorId || !uid)
            return false;
        __try {
            void* popup = nullptr;
            if (FindInputPopup(
                    operatorId,
                    uid,
                    &popup) !=
                kPopupMatched)
                return false;
            void* root =
                ResolvePopupRoot(
                    popup,
                    L"dlg");
            void* edit =
                root
                    ? FindControl(
                          root,
                          L"Edit",
                          kEditKind)
                    : nullptr;
            void* button =
                root
                    ? FindControl(
                          root,
                          L"Btn2",
                          kButtonKind)
                    : nullptr;
            return root && edit && button &&
                   ControlIdentity(
                       edit,
                       root,
                       L"Edit") &&
                   ControlIdentity(
                       button,
                       root,
                       L"Btn2");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Result Activate(
        uint32_t operatorId,
        uint64_t uid)
    {
        if (!operatorId || !uid)
            return Publish(
                kTargetIdentityMismatch);
        if (InterlockedCompareExchange(
                &activationSealed_,
                0,
                0) != 0)
            return Publish(
                kMutationAttemptSealed);
        const Result ready =
            EnsureInventory();
        if (ready != kReady)
            return ready;
        __try {
            OperatorFlowActions::OperatorRecord
                known;
            const Result knownResult =
                FindKnownTarget(
                    operatorId,
                    uid,
                    &known);
            if (knownResult != kReady)
                return Publish(
                    knownResult);
            SetTarget(
                operatorId,
                uid,
                known.slot);
            const Result page =
                EnsureTargetPage(
                    known.slot);
            if (page != kReady)
                return page;

            uint8_t* record = nullptr;
            uint16_t slot = 0;
            const Result target =
                FindTarget(
                    operatorId,
                    uid,
                    &record,
                    &slot);
            if (target != kReady)
                return Publish(target);
            if (slot != known.slot)
                return Publish(
                    kTargetIdentityMismatch);

            void* list =
                FindControl(
                    liveRoot_,
                    L"List",
                    kListKind);
            if (!list ||
                list != liveList_ ||
                !ControlIdentity(
                    list,
                    liveRoot_,
                    L"List"))
                return Publish(
                    kInventoryControlMismatch);
            const uint32_t currentPage =
                static_cast<uint32_t>(
                    (slot - 1) /
                    kRecordsPerPage);
            const int localIndex =
                static_cast<int>(
                    (slot - 1) %
                    kRecordsPerPage);
            auto* row =
                static_cast<uint8_t*>(
                    bindings_.
                        list_item_by_index(
                            list,
                            static_cast<int>(slot - 1)));
            if (!row)
                return Publish(
                    kNativeSelectionRejected);
            auto* rowRecord =
                *reinterpret_cast<uint8_t**>(
                    row +
                    kListItemData);
            void* rowControl =
                *reinterpret_cast<void**>(
                    row +
                    kListItemControl);
            if (!rowRecord || !rowControl ||
                rowRecord != record)
                return Publish(
                    kTargetIdentityMismatch);
            const float rowX =
                *reinterpret_cast<float*>(
                    static_cast<uint8_t*>(
                        rowControl) +
                    kControlX);
            const float rowY =
                *reinterpret_cast<float*>(
                    static_cast<uint8_t*>(
                        rowControl) +
                    kControlY);
            int32_t x = 0;
            int32_t y = 0;
            if (!CoordinateToClick(
                    rowX,
                    &x) ||
                !CoordinateToClick(
                    rowY,
                    &y))
                return Publish(
                    kNativeSelectionRejected);
            const uint8_t down =
                bindings_.
                    list_mouse_business(
                        list,
                        kMouseLeftDown,
                        x,
                        y,
                        0,
                        0);
            const uint8_t up =
                down
                    ? bindings_.
                          list_mouse_business(
                              list,
                              kMouseLeftUp,
                              x,
                              y,
                              0,
                              0)
                    : 0;
            ++observation_.
                selection_actions;
            if (!down || !up ||
                !ExclusiveSelection(
                    list,
                    currentPage,
                    localIndex))
                return Publish(
                    kNativeSelectionRejected);
            observation_.row_selected = 1;
            if (record[kRecordEnabled] == 1)
                return Publish(kAlreadyActive);

            auto* button =
                *reinterpret_cast<uint8_t**>(
                    static_cast<uint8_t*>(
                        liveInventory_) +
                    kActivationButton);
            if (!button ||
                FindControl(
                    liveRoot_,
                    L"OpSallyBtn",
                    kButtonKind) != button ||
                !ControlIdentity(
                    button,
                    liveRoot_,
                    L"OpSallyBtn"))
                return Publish(
                    kActivationControlMismatch);
            InterlockedExchange(
                &activationSealed_,
                1);
            observation_.activation_sealed =
                1;
            ++observation_.
                activation_actions;
            bindings_.inventory_callback(
                liveInventory_,
                kClickEvent,
                button + kControlName,
                button);
            return Publish(
                kActivationSubmitted);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    bool ReadNickname(
        uint64_t uid,
        std::string* output)
    {
        if (!uid || !output)
            return false;
        OperatorFlowActions::OperatorRecord
            record;
        return ReadOperatorState(
                   uid,
                   &record,
                   output,
                   nullptr) == kReady;
    }

    bool ReadEnabled(
        uint64_t uid,
        bool* output)
    {
        if (!uid || !output)
            return false;
        OperatorFlowActions::OperatorRecord
            record;
        if (ReadOperatorState(
                uid,
                &record,
                nullptr,
                nullptr) != kReady)
            return false;
        *output = record.enabled == 1;
        return true;
    }

    Result ReadOperatorState(
        uint64_t uid,
        OperatorFlowActions::
            OperatorRecord* output,
        std::string* nickname = nullptr,
        uint8_t* setNameState = nullptr)
    {
        if (!uid || !output)
            return Publish(
                kTargetIdentityMismatch);
        const Result ready =
            EnsureInventory();
        if (ready != kReady)
            return ready;
        __try {
            OperatorFlowActions::OperatorRecord
                known;
            const Result knownResult =
                FindKnownRecordByUid(
                    uid,
                    &known);
            if (knownResult != kReady)
                return Publish(
                    knownResult);
            const Result page =
                EnsureTargetPage(
                    known.slot);
            if (page != kReady)
                return page;

            uint8_t* record = nullptr;
            if (!FindRecordByUid(
                    uid,
                    &record))
                return Publish(
                    kTargetIdentityMismatch);
            OperatorFlowActions::
                OperatorRecord value;
            memcpy(
                &value.uid,
                record,
                sizeof(value.uid));
            memcpy(
                &value.operator_id,
                record +
                    kRecordOperatorId,
                sizeof(value.operator_id));
            memcpy(
                &value.slot,
                record +
                    kRecordSlot,
                sizeof(value.slot));
            value.enabled =
                record[kRecordEnabled];
            if (!value.uid ||
                !value.operator_id ||
                !value.slot ||
                value.enabled > 1 ||
                value.operator_id !=
                    known.operator_id ||
                value.slot != known.slot ||
                (nickname &&
                 !ReadRecordNickname(
                     record,
                     nickname)) ||
                (setNameState &&
                 record[
                     kRecordSetNameState] >
                     1))
                return Publish(
                    kTargetIdentityMismatch);
            if (setNameState)
                *setNameState =
                    record[
                        kRecordSetNameState];
            *output = value;
            return Publish(kReady);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    bool ReadOperator(
        uint64_t uid,
        OperatorFlowActions::
            OperatorRecord* output,
        std::string* nickname = nullptr,
        uint8_t* setNameState = nullptr)
    {
        return ReadOperatorState(
                   uid,
                   output,
                   nickname,
                   setNameState) == kReady;
    }

    bool ReadActiveOperator(
        OperatorFlowActions::
            OperatorRecord* output) const
    {
        if (!output ||
            !bindings_.profile_pointer)
            return false;
        __try {
            const uint8_t* profile =
                *bindings_.profile_pointer;
            if (!profile)
                return false;
            const uint8_t* record =
                profile +
                kProfileActiveOperator;
            OperatorFlowActions::
                OperatorRecord value;
            memcpy(
                &value.uid,
                record,
                sizeof(value.uid));
            memcpy(
                &value.operator_id,
                record +
                    kRecordOperatorId,
                sizeof(value.operator_id));
            memcpy(
                &value.slot,
                record +
                    kRecordSlot,
                sizeof(value.slot));
            // Profile+0x242 is the active descriptor, copied verbatim from
            // 08A0 (including its old inventory flag). Its location denotes
            // selection; only the inventory backing vector owns enabled bits.
            value.enabled = 1;
            if (!value.uid ||
                !value.operator_id ||
                !value.slot ||
                value.enabled != 1)
                return false;
            *output = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Observation State() const
    {
        Observation value = observation_;
        value.naming_sealed =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &namingSealed_),
                0,
                0) != 0;
        value.activation_sealed =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &activationSealed_),
                0,
                0) != 0;
        value.page_reply_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &pageReplySequence_),
                    0,
                    0));
        value.page_reply_page =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &pageReplyPage_),
                    0,
                    0));
        value.page_reply_size =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &pageReplySize_),
                    0,
                    0));
        value.page_reply_count =
            static_cast<uint8_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &pageReplyCount_),
                    0,
                    0));
        value.page_reply_empty =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(
                    &pageReplyEmpty_),
                0,
                0) != 0;
        return value;
    }

private:
    enum ServiceResult
    {
        kServiceAbsent,
        kServiceMatched,
        kServiceMismatched,
    };

    enum PopupResult
    {
        kPopupAbsent,
        kPopupMatched,
        kPopupMismatched,
        kPopupFault,
    };

    enum PageImageResult
    {
        kPageImageWaiting,
        kPageImageReady,
        kPageImageInvalid,
    };

    class NativeString
    {
    public:
        NativeString(
            const Bindings& bindings,
            const wchar_t* value)
            : bindings_(bindings)
        {
            if (value &&
                bindings_.
                    native_wstring_ctor)
                constructed_ =
                    bindings_.
                        native_wstring_ctor(
                            bytes_,
                            value) != nullptr;
        }

        ~NativeString()
        {
            if (constructed_ &&
                bindings_.
                    native_wstring_dtor)
                bindings_.
                    native_wstring_dtor(
                        bytes_);
        }

        explicit operator bool() const
        {
            return constructed_;
        }

        void* get()
        {
            return bytes_;
        }

    private:
        const Bindings& bindings_;
        alignas(4) uint8_t bytes_[
            0x1C] = {};
        bool constructed_ = false;
    };

    static constexpr size_t
        kCallbackSlot = 0x38 /
                        sizeof(void*);
    static constexpr size_t
        kMainframeRoot = 0x64;
    static constexpr size_t
        kInventoryRoot = 0x64;
    static constexpr size_t
        kCurrentPage = 0xB8;
    static constexpr size_t
        kMaximumPage = 0xBC;
    static constexpr size_t
        kRecordsBegin = 0xC0;
    static constexpr size_t
        kRecordsEnd = 0xC4;
    static constexpr size_t
        kActivationButton = 0xD4;
    static constexpr size_t
        kRecordStride = 0x50;
    static constexpr size_t
        kRecordOperatorId = 8;
    static constexpr size_t
        kRecordSlot = 0x10;
    static constexpr size_t
        kRecordEnabled = 0x12;
    static constexpr size_t
        kRecordNickname = 0x14;
    static constexpr size_t
        kRecordSetNameState = 0x4D;
    static constexpr size_t
        kProfileActiveOperator = 0x242;
    static constexpr size_t
        kInputOperatorId = 0x4C;
    static constexpr size_t
        kInputUidLow = 0x50;
    static constexpr size_t
        kInputUidHigh = 0x54;
    static constexpr size_t
        kControlName = 0x118;
    static constexpr size_t
        kControlX = 0x148;
    static constexpr size_t
        kControlY = 0x14C;
    static constexpr size_t
        kControlParent = 0x15C;
    static constexpr size_t
        kControlVisible = 0x162;
    static constexpr size_t
        kControlEnabled = 0x167;
    static constexpr size_t
        kNativeStringLength = 0x10;
    static constexpr size_t
        kNativeStringCapacity = 0x14;
    static constexpr size_t
        kManagerPopupSentinel = 0x54;
    static constexpr size_t
        kManagerPopupCount = 0x58;
    static constexpr size_t
        kListItemControl = 0x1C;
    static constexpr size_t
        kListItemSelected = 0x25;
    static constexpr size_t
        kListItemData = 0x2C;
    static constexpr uint32_t
        kRadioEvent = 0x301;
    static constexpr uint32_t
        kClickEvent = 0x101;
    static constexpr uint32_t
        kMouseLeftDown = 0x201;
    static constexpr uint32_t
        kMouseLeftUp = 0x202;
    static constexpr uint16_t
        kPageReply = 0x059F;
    static constexpr uint32_t
        kButtonKind = 4;
    static constexpr uint32_t
        kListKind = 7;
    static constexpr uint32_t
        kEditKind = 8;
    static constexpr uint32_t
        kRadioKind = 0x0C;
    static constexpr size_t
        kMaximumRecords = 4096;
    static constexpr uint32_t
        kRecordsPerPage = 6;
    static constexpr uint32_t
        kMaximumPages =
            (kMaximumRecords +
             kRecordsPerPage - 1) /
            kRecordsPerPage;
    static constexpr size_t
        kMaximumPopups = 16;

    using PopupRootFn =
        void*(__thiscall*)(
            void*,
            const void*);

    static bool CallbackMatches(
        void* vtable,
        UiCallbackFn callback)
    {
        return vtable && callback &&
               reinterpret_cast<void**>(
                   vtable)[kCallbackSlot] ==
                   reinterpret_cast<void*>(
                       callback);
    }

    ServiceResult ResolveService(
        const wchar_t* name,
        void* expectedVtable,
        void** output) const
    {
        if (output)
            *output = nullptr;
        if (!name || !expectedVtable ||
            !output)
            return kServiceAbsent;
        void* manager =
            *bindings_.
                 service_managed_pointer;
        if (!manager)
            return kServiceAbsent;
        alignas(4) uint8_t descriptor[
            sizeof(uint32_t) + 0x1C] = {};
        *reinterpret_cast<uint32_t*>(
            descriptor) = 1;
        NativeString native(
            bindings_,
            name);
        if (!native)
            return kServiceAbsent;
        memcpy(
            descriptor +
                sizeof(uint32_t),
            native.get(),
            0x1C);
        // Transfer only the temporary bit image. Its owned allocation, if any,
        // remains with NativeString and is destroyed after service lookup.
        void* service =
            bindings_.
                service_find_by_name(
                    manager,
                    descriptor);
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
        if (!root || !name)
            return nullptr;
        NativeString native(
            bindings_,
            name);
        return native
                   ? bindings_.
                         native_ui_find_control(
                             root,
                             native.get(),
                             kind)
                   : nullptr;
    }

    static bool NativeStringEquals(
        const void* storage,
        const wchar_t* expected)
    {
        if (!storage || !expected)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(
                storage);
        const uint32_t length =
            *reinterpret_cast<
                const uint32_t*>(
                bytes +
                kNativeStringLength);
        const uint32_t capacity =
            *reinterpret_cast<
                const uint32_t*>(
                bytes +
                kNativeStringCapacity);
        if (length > 255 ||
            capacity < length ||
            wcslen(expected) != length)
            return false;
        const wchar_t* text =
            capacity < 8
                ? reinterpret_cast<
                      const wchar_t*>(
                      bytes)
                : *reinterpret_cast<
                      wchar_t* const*>(
                      bytes);
        return text &&
               wmemcmp(
                   text,
                   expected,
                   length) == 0 &&
               text[length] == L'\0';
    }

    static void CopyNativeStringAscii(
        const void* storage,
        char* output,
        size_t outputCapacity)
    {
        if (!output || !outputCapacity)
            return;
        output[0] = '\0';
        if (!storage)
            return;
        __try {
            const auto* bytes =
                static_cast<const uint8_t*>(
                    storage);
            const uint32_t length =
                *reinterpret_cast<
                    const uint32_t*>(
                    bytes +
                    kNativeStringLength);
            const uint32_t capacity =
                *reinterpret_cast<
                    const uint32_t*>(
                    bytes +
                    kNativeStringCapacity);
            if (length >= outputCapacity ||
                capacity < length)
                return;
            const wchar_t* text =
                capacity < 8
                    ? reinterpret_cast<
                          const wchar_t*>(
                          bytes)
                    : *reinterpret_cast<
                          wchar_t* const*>(
                          bytes);
            if (!text)
                return;
            for (uint32_t index = 0;
                 index < length;
                 ++index) {
                const wchar_t value =
                    text[index];
                output[index] =
                    value >= 0x20 &&
                            value <= 0x7E
                        ? static_cast<char>(
                              value)
                        : '?';
            }
            output[length] = '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            output[0] = '\0';
        }
    }

    static bool ControlName(
        const void* control,
        const wchar_t* expected)
    {
        return control &&
               NativeStringEquals(
                   static_cast<
                       const uint8_t*>(
                       control) +
                       kControlName,
                   expected);
    }

    static bool ControlIdentity(
        const void* control,
        const void* parent,
        const wchar_t* expected)
    {
        if (!control || !parent)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(
                control);
        return *reinterpret_cast<
                   void* const*>(
                   bytes +
                   kControlParent) ==
                   parent &&
               bytes[kControlVisible] == 1 &&
               bytes[kControlEnabled] == 1 &&
               ControlName(
                   control,
                   expected);
    }

    bool ReadPageBounds(
        uint32_t* current,
        uint32_t* maximum)
    {
        if (current)
            *current = 0;
        if (maximum)
            *maximum = 0;
        if (!liveInventory_ ||
            !current || !maximum)
            return false;
        __try {
            const auto* service =
                static_cast<const uint8_t*>(
                    liveInventory_);
            const int32_t currentRaw =
                *reinterpret_cast<
                    const int32_t*>(
                    service +
                    kCurrentPage);
            const int32_t maximumRaw =
                *reinterpret_cast<
                    const int32_t*>(
                    service +
                    kMaximumPage);
            observation_.current_page =
                currentRaw >= 0
                    ? static_cast<uint32_t>(
                          currentRaw)
                    : UINT32_MAX;
            if (currentRaw < 0 ||
                maximumRaw < 0 ||
                currentRaw > maximumRaw ||
                static_cast<uint32_t>(
                    maximumRaw) >=
                    kMaximumPages)
                return false;
            *current =
                static_cast<uint32_t>(
                    currentRaw);
            *maximum =
                static_cast<uint32_t>(
                    maximumRaw);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    PageImageResult ReadPageImage(
        uint32_t page,
        uint32_t maximum,
        OperatorFlowActions::
            InventoryImage* output)
    {
        if (!output || page > maximum)
            return kPageImageInvalid;
        uint32_t current = 0;
        uint32_t observedMaximum = 0;
        if (!ReadPageBounds(
                &current,
                &observedMaximum) ||
            current != page ||
            observedMaximum != maximum)
            return kPageImageWaiting;

        OperatorFlowActions::InventoryImage
            image;
        if (!OperatorFlowActions::
                ReadOperatorInventoryService(
                    liveInventory_,
                    &image))
            return kPageImageInvalid;

        const uint32_t firstSlot =
            page * kRecordsPerPage + 1;
        const uint32_t finalSlot =
            firstSlot +
            kRecordsPerPage - 1;
        uint32_t pageRecords = 0;
        for (size_t index = 0;
             index < image.records.size();
             ++index) {
            const auto& record =
                image.records[index];
            if (record.slot >= firstSlot &&
                record.slot <= finalSlot)
                ++pageRecords;
            for (size_t other = 0;
                 other < index;
                 ++other)
                if (image.records[other].uid ==
                        record.uid ||
                    image.records[other].slot ==
                        record.slot)
                    return kPageImageInvalid;
        }
        const int32_t replyPage =
            InterlockedCompareExchange(
                &pageReplyPage_,
                0,
                0);
        if (replyPage ==
            static_cast<int32_t>(page)) {
            const uint32_t replyCount =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &pageReplyCount_,
                        0,
                        0));
            const uint32_t replySize =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &pageReplySize_,
                        0,
                        0));
            if (replyCount >
                    kRecordsPerPage ||
                replySize !=
                    7u +
                        replyCount *
                            kRecordStride)
                return kPageImageInvalid;
            if (pageRecords != replyCount)
                return kPageImageWaiting;
        } else if (!pageRecords) {
            return kPageImageWaiting;
        }
        *output = image;
        return kPageImageReady;
    }

    void ResetInventoryScan()
    {
        inventoryScanActive_ = false;
        inventoryScanService_ = nullptr;
        inventoryScanMaximumPage_ = -1;
        inventoryScanNextPage_ = 0;
        inventoryScanSeenPages_.clear();
    }

    Result FindKnownRecordByUid(
        uint64_t uid,
        OperatorFlowActions::
            OperatorRecord* output) const
    {
        if (output)
            *output = {};
        if (!uid || !output)
            return kTargetIdentityMismatch;
        unsigned matches = 0;
        for (const auto& record :
             knownInventory_.records) {
            if (record.uid != uid)
                continue;
            ++matches;
            *output = record;
        }
        if (!matches)
            return kTargetMissing;
        return matches == 1
                   ? kReady
                   : kTargetIdentityMismatch;
    }

    Result FindKnownTarget(
        uint32_t operatorId,
        uint64_t uid,
        OperatorFlowActions::
            OperatorRecord* output) const
    {
        const Result found =
            FindKnownRecordByUid(
                uid,
                output);
        if (found != kReady)
            return found;
        return output->operator_id ==
                       operatorId
                   ? kReady
                   : kTargetIdentityMismatch;
    }

    Result FindTarget(
        uint32_t operatorId,
        uint64_t uid,
        uint8_t** output,
        uint16_t* slot) const
    {
        if (output)
            *output = nullptr;
        if (slot)
            *slot = 0;
        if (!liveInventory_ ||
            !output || !slot)
            return kInventoryMismatch;
        const uintptr_t begin =
            *reinterpret_cast<uintptr_t*>(
                static_cast<uint8_t*>(
                    liveInventory_) +
                kRecordsBegin);
        const uintptr_t end =
            *reinterpret_cast<uintptr_t*>(
                static_cast<uint8_t*>(
                    liveInventory_) +
                kRecordsEnd);
        if (!begin || end < begin ||
            (end - begin) %
                    kRecordStride !=
                0)
            return kInventoryMismatch;
        const size_t count =
            (end - begin) /
            kRecordStride;
        if (count > kMaximumRecords)
            return kInventoryMismatch;
        unsigned matches = 0;
        for (size_t index = 0;
             index < count;
             ++index) {
            auto* record =
                reinterpret_cast<uint8_t*>(
                    begin +
                    index *
                        kRecordStride);
            uint64_t recordUid = 0;
            uint32_t recordId = 0;
            uint16_t recordSlot = 0;
            memcpy(
                &recordUid,
                record,
                sizeof(recordUid));
            memcpy(
                &recordId,
                record +
                    kRecordOperatorId,
                sizeof(recordId));
            memcpy(
                &recordSlot,
                record +
                    kRecordSlot,
                sizeof(recordSlot));
            if (!recordUid &&
                !recordId) {
                if (record[kRecordEnabled] ||
                    recordSlot != index + 1)
                    return
                        kTargetIdentityMismatch;
                continue;
            }
            if (!recordUid || !recordId ||
                recordSlot != index + 1 ||
                record[kRecordEnabled] > 1)
                return
                    kTargetIdentityMismatch;
            if (recordUid != uid)
                continue;
            if (recordId != operatorId)
                return
                    kTargetIdentityMismatch;
            ++matches;
            *output = record;
            *slot = recordSlot;
        }
        if (!matches)
            return kTargetMissing;
        return matches == 1
                   ? kReady
                   : kTargetIdentityMismatch;
    }

    bool FindRecordByUid(
        uint64_t uid,
        uint8_t** output) const
    {
        if (output)
            *output = nullptr;
        if (!liveInventory_ || !output)
            return false;
        const uintptr_t begin =
            *reinterpret_cast<uintptr_t*>(
                static_cast<uint8_t*>(
                    liveInventory_) +
                kRecordsBegin);
        const uintptr_t end =
            *reinterpret_cast<uintptr_t*>(
                static_cast<uint8_t*>(
                    liveInventory_) +
                kRecordsEnd);
        if (!begin || end < begin ||
            (end - begin) %
                    kRecordStride !=
                0)
            return false;
        uint8_t* found = nullptr;
        for (uintptr_t current = begin;
             current < end;
             current += kRecordStride) {
            uint64_t value = 0;
            memcpy(
                &value,
                reinterpret_cast<void*>(
                    current),
                sizeof(value));
            if (value != uid)
                continue;
            if (found)
                return false;
            found =
                reinterpret_cast<uint8_t*>(
                    current);
        }
        *output = found;
        return found != nullptr;
    }

    Result EnsureTargetPage(
        uint16_t slot)
    {
        if (!slot)
            return Publish(
                kTargetIdentityMismatch);
        const uint32_t target =
            (slot - 1) /
            kRecordsPerPage;
        observation_.target_page = target;
        return EnsurePage(target);
    }

    Result EnsurePage(
        uint32_t target)
    {
        if (!liveInventory_ ||
            !liveRoot_)
            return Publish(
                kTargetIdentityMismatch);
        uint32_t current = 0;
        uint32_t maximum = 0;
        if (!ReadPageBounds(
                &current,
                &maximum) ||
            target > maximum)
            return Publish(
                kTargetIdentityMismatch);
        if (pagePending_) {
            if (current ==
                static_cast<uint32_t>(
                    pageInvokedFrom_))
                return Publish(
                    kNavigatingPage);
            const int32_t expected =
                pageInvokedFrom_ +
                pageInvokedDirection_;
            if (expected < 0 ||
                current !=
                    static_cast<uint32_t>(
                        expected))
                return Publish(
                    kTargetIdentityMismatch);
            const uint32_t replySequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &pageReplySequence_,
                        0,
                        0));
            const int32_t replyPage =
                InterlockedCompareExchange(
                    &pageReplyPage_,
                    0,
                    0);
            if (replySequence <=
                    pageReplyBaseline_ ||
                replyPage < 0 ||
                current !=
                    static_cast<uint32_t>(
                        replyPage))
                return Publish(
                    kNavigatingPage);
            pagePending_ = false;
            InterlockedExchange(
                &pageExpectedReplyPage_,
                -1);
            pageDataPending_ = true;
        }
        if (pageDataPending_) {
            OperatorFlowActions::InventoryImage
                pageImage;
            const PageImageResult loaded =
                ReadPageImage(
                    current,
                    maximum,
                    &pageImage);
            if (loaded == kPageImageWaiting)
                return Publish(
                    kNavigatingPage);
            if (loaded != kPageImageReady)
                return Publish(
                    kTargetIdentityMismatch);
            pageDataPending_ = false;
        }
        if (current == target)
            return Publish(kReady);

        const wchar_t* dispatchName =
            current < target
                ? L"RightOneBtn"
                : L"LeftOneBtn";
        const wchar_t* anchorName =
            current < target
                ? L"RightBtn"
                : L"LeftBtn";
        const uint32_t buttonKind =
            kButtonKind;
        observation_.page_control_kind =
            buttonKind;
        observation_.
            page_control_root_address =
            static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(
                    liveRoot_));
        observation_.page_control_address = 0;
        observation_.
            page_control_parent_address = 0;
        memset(
            observation_.
                page_control_actual_name,
            0,
            sizeof(observation_.
                       page_control_actual_name));
        void* button =
            FindControl(
                liveRoot_,
                anchorName,
                buttonKind);
        observation_.page_control_found =
            button ? 1 : 0;
        if (button) {
            auto* bytes =
                static_cast<uint8_t*>(
                    button);
            void* parent =
                *reinterpret_cast<void**>(
                    bytes +
                    kControlParent);
            observation_.page_control_address =
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(
                        button));
            observation_.
                page_control_parent_address =
                static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(
                        parent));
            observation_.page_control_parent =
                parent == liveRoot_
                    ? 1
                    : 0;
            CopyNativeStringAscii(
                bytes +
                    kControlName,
                observation_.
                    page_control_actual_name,
                sizeof(observation_.
                           page_control_actual_name));
            observation_.page_control_name =
                ControlName(
                    button,
                    anchorName)
                    ? 1
                    : 0;
            observation_.page_control_visible =
                bytes[kControlVisible]
                    ? 1
                    : 0;
            observation_.page_control_enabled =
                bytes[kControlEnabled]
                    ? 1
                    : 0;
        }
        if (!button ||
            !observation_.
                 page_control_parent ||
            !observation_.
                 page_control_name ||
            !observation_.
                 page_control_visible ||
            !observation_.
                 page_control_enabled)
            return Publish(
                kPageControlMismatch);
        NativeString nativeDispatchName(
            bindings_,
            dispatchName);
        if (!nativeDispatchName)
            return Publish(kNativeFault);
        pagePending_ = true;
        pageInvokedFrom_ =
            static_cast<int32_t>(
                current);
        pageInvokedDirection_ =
            current < target ? 1 : -1;
        pageReplyBaseline_ =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &pageReplySequence_,
                    0,
                    0));
        InterlockedExchange(
            &pageExpectedReplyPage_,
            pageInvokedFrom_ +
                pageInvokedDirection_);
        ++observation_.page_actions;
        bindings_.inventory_callback(
            liveInventory_,
            kClickEvent,
            nativeDispatchName.get(),
            button);
        return Publish(
            kNavigatingPage);
    }

    PopupResult FindInputPopup(
        uint32_t operatorId,
        uint64_t uid,
        void** output) const
    {
        inputInspection_ = {};
        inputInspection_.expected_vtable = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bindings_.input_vtable));
        const auto done = [this](PopupResult result, uint32_t code) {
            inputInspection_.code = code; return result;
        };
        if (output) *output = nullptr;
        if (!output) return done(kPopupFault, 1);
        auto* manager = static_cast<uint8_t*>(*bindings_.service_managed_pointer);
        if (!manager) return done(kPopupFault, 2);
        inputInspection_.active_count = *reinterpret_cast<uint32_t*>(manager + 0x48);
        inputInspection_.pending_count = *reinterpret_cast<uint32_t*>(manager + 0x58);
        void* matched = nullptr;
        // Native services move from the pending list to the active list before
        // the next observer tick. Neither list alone owns popup discovery.
        const size_t sentinels[] = {0x44, kManagerPopupSentinel};
        const size_t counts[] = {0x48, kManagerPopupCount};
        for (unsigned list = 0; list < 2; ++list) {
            inputInspection_.list = list;
            void* sentinel = *reinterpret_cast<void**>(manager + sentinels[list]);
            const uint32_t count = *reinterpret_cast<uint32_t*>(manager + counts[list]);
            if (!sentinel || count > (list ? kMaximumPopups : 256)) return done(kPopupFault, 3);
            void* node = *reinterpret_cast<void**>(sentinel);
            void* previous = sentinel;
            uint32_t seen = 0;
            while (node != sentinel) {
                if (!node || seen >= count ||
                    *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + 4) != previous)
                    return done(kPopupFault, 4);
                void* service = *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + 8);
                if (!service) return done(kPopupFault, 5);
                if (*reinterpret_cast<void**>(service) == bindings_.input_vtable) {
                    const auto* bytes = static_cast<const uint8_t*>(service);
                    uint32_t popupId = 0, low = 0, high = 0;
                    memcpy(&popupId, bytes + kInputOperatorId, sizeof(popupId));
                    memcpy(&low, bytes + kInputUidLow, sizeof(low));
                    memcpy(&high, bytes + kInputUidHigh, sizeof(high));
                    inputInspection_.found_operator = popupId;
                    inputInspection_.found_uid = static_cast<uint64_t>(high) << 32 | low;
                    const uint32_t nativeState = *reinterpret_cast<const uint32_t*>(bytes + 0x44);
                    inputInspection_.native_state = nativeState;
                    // The manager retains inactive and closing dialogs. Only
                    // the ready dialog for this exact owned instance is actionable.
                    if (popupId == operatorId && (static_cast<uint64_t>(high) << 32 | low) == uid && nativeState == 3) {
                        if (matched && matched != service) return done(kPopupMismatched, 6);
                        matched = service;
                    }
                }
                previous = node;
                node = *reinterpret_cast<void**>(node);
                ++seen;
            }
            if (seen != count || *reinterpret_cast<void**>(static_cast<uint8_t*>(sentinel) + 4) != previous)
                return done(kPopupFault, 8);
        }
        if (!matched) return done(kPopupAbsent, 9);
        *output = matched;
        return done(kPopupMatched, 10);
    }

    void* ResolvePopupRoot(
        void* popup,
        const wchar_t* name) const
    {
        if (!popup || !name)
            return nullptr;
        auto** vtable =
            *reinterpret_cast<void***>(
                popup);
        auto root =
            vtable
                ? reinterpret_cast<
                      PopupRootFn>(
                      vtable[
                          0x5C /
                          sizeof(void*)])
                : nullptr;
        if (!root)
            return nullptr;
        NativeString native(
            bindings_,
            name);
        return native
                   ? root(
                         popup,
                         native.get())
                   : nullptr;
    }

    bool ExclusiveSelection(
        void* list,
        uint32_t page,
        int target) const
    {
        if (!list ||
            page >= kMaximumPages ||
            target < 0 ||
            target >=
                static_cast<int>(
                    kRecordsPerPage))
            return false;
        for (uint32_t local = 0;
             local < kRecordsPerPage;
             ++local) {
            auto* row =
                static_cast<uint8_t*>(
                    bindings_.
                        list_item_by_index(
                            list,
                            static_cast<int>(
                                page * kRecordsPerPage + local)));
            if (!row)
                return false;
            const bool selected =
                row[kListItemSelected] == 1;
            if (selected !=
                (static_cast<int>(
                     local) == target))
                return false;
        }
        return true;
    }

    static bool CoordinateToClick(
        float coordinate,
        int32_t* output)
    {
        if (!output ||
            !std::isfinite(coordinate))
            return false;
        const double value =
            static_cast<double>(
                coordinate);
        const double minimum =
            static_cast<double>(
                (std::numeric_limits<
                    int32_t>::min)());
        const double maximum =
            static_cast<double>(
                (std::numeric_limits<
                    int32_t>::max)()) -
            1.0;
        if (value < minimum ||
            value > maximum)
            return false;
        *output =
            static_cast<int32_t>(
                coordinate) +
            1;
        return true;
    }

    static bool NicknameValid(
        const wchar_t* nickname)
    {
        if (!nickname)
            return false;
        size_t length = 0;
        while (nickname[length]) {
            if (length >= 16 ||
                nickname[length] < 0x20 ||
                nickname[length] > 0x7E)
                return false;
            ++length;
        }
        return length != 0;
    }

    bool SetEditText(
        void* edit,
        const wchar_t* nickname) const
    {
        if (!edit || !nickname)
            return false;
        NativeString text(
            bindings_,
            nickname);
        if (!text)
            return false;
        bindings_.edit_set_text(
            edit,
            text.get(),
            0);
        return NativeStringEquals(
            bindings_.edit_get_text(
                edit),
            nickname);
    }

    static bool RecordNicknameTerminated(
        const uint8_t* record)
    {
        if (!record)
            return false;
        for (size_t index = 0;
             index <= 16;
             ++index) {
            if (record[
                    kRecordNickname +
                    index] == 0)
                return true;
        }
        return false;
    }

    static bool RecordNicknameEquals(
        const uint8_t* record,
        const wchar_t* nickname)
    {
        if (!record || !nickname)
            return false;
        size_t index = 0;
        while (nickname[index] &&
               index < 16) {
            if (record[
                    kRecordNickname +
                    index] !=
                static_cast<uint8_t>(
                    nickname[index]))
                return false;
            ++index;
        }
        return !nickname[index] &&
               record[
                   kRecordNickname +
                   index] == 0;
    }

    static bool ReadRecordNickname(
        const uint8_t* record,
        std::string* output)
    {
        if (!record || !output)
            return false;
        const char* nickname =
            reinterpret_cast<const char*>(
                record +
                kRecordNickname);
        size_t length = 0;
        while (length <= 16 &&
               nickname[length])
            ++length;
        if (length > 16)
            return false;
        output->assign(
            nickname,
            nickname + length);
        return true;
    }

    void SetTarget(
        uint32_t operatorId,
        uint64_t uid,
        uint16_t slot)
    {
        observation_.target_operator_id =
            operatorId;
        observation_.target_uid = uid;
        observation_.target_slot = slot;
    }

    Result Publish(Result result)
    {
        observation_.result = result;
        return result;
    }

    Bindings bindings_ = {};
    HangarNavigation* navigation_ = nullptr;
    mutable InputPopupInspection inputInspection_ = {};
    Observation observation_ = {};
    void* liveInventory_ = nullptr;
    void* liveRoot_ = nullptr;
    void* liveList_ = nullptr;
    bool bound_ = false;
    bool openingInvoked_ = false;
    bool pagePending_ = false;
    bool pageDataPending_ = false;
    int32_t pageInvokedFrom_ = -1;
    int32_t pageInvokedDirection_ = 0;
    uint32_t pageReplyBaseline_ = 0;
    bool inventoryScanActive_ = false;
    void* inventoryScanService_ = nullptr;
    int32_t inventoryScanMaximumPage_ = -1;
    uint32_t inventoryScanNextPage_ = 0;
    std::vector<uint8_t>
        inventoryScanSeenPages_;
    OperatorFlowActions::InventoryImage
        knownInventory_;
    volatile LONG pageReplySequence_ = 0;
    volatile LONG pageReplyPage_ = -1;
    volatile LONG pageReplyCount_ = 0;
    volatile LONG pageReplySize_ = 0;
    volatile LONG pageReplyEmpty_ = 0;
    volatile LONG pageExpectedReplyPage_ = -1;
    volatile LONG namingSealed_ = 0;
    volatile LONG activationSealed_ = 0;
};
