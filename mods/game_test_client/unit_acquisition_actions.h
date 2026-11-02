#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "hangar_navigation.h"

// Opens one sealed fixed/custom capsule target through the shipped UI.
//
// The caller supplies machine kind, machine ID and root unit ID from its
// sealed test configuration. This adapter scans the client's live native
// vectors, invokes only the original roommgr/custommgr callbacks and waits
// for the original 0x01F8 consumer to populate the real unitview renderer.
// It never invokes CapsuleDropBtn, selection mutation event 0x0802, a buy
// callback, or a network request.
class UnitAcquisitionActions
{
public:
    enum MachineKind : uint32_t
    {
        kFixedMachine = 1,
        kCustomMachine = 3,
    };

    struct Config
    {
        uint32_t machine_kind;
        uint32_t machine_id;
        uint32_t unit_id;
    };

    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForHangar = 1,
        kOpeningCapsuleRoom = 2,
        kWaitingForMachine = 3,
        kSelectingMachineKind = 4,
        kPagingMachines = 5,
        kSelectingMachine = 6,
        kWaitingForFixedCandidate = 7,
        kOpeningCustomManager = 8,
        kWaitingForCustomManager = 9,
        kPagingCustomCandidates = 10,
        kSelectingCandidate = 11,
        kWaitingForUnitViewEvent = 12,
        kWaitingForUnitViewModel = 13,
        kReady = 14,

        kInvalidConfig = 0x100,
        kNavigationMismatch = 0x101,
        kMachineServiceMismatch = 0x102,
        kMachineDataMismatch = 0x103,
        kTargetMachineMissing = 0x104,
        kTargetUnitMissing = 0x105,
        kMachineControlMismatch = 0x106,
        kMachineActionRejected = 0x107,
        kCustomServiceMismatch = 0x108,
        kCustomDataMismatch = 0x109,
        kCustomControlMismatch = 0x10A,
        kCustomActionRejected = 0x10B,
        kUnitViewServiceMismatch = 0x10C,
        kUnitViewEventMismatch = 0x10D,
        kUnitViewControlMismatch = 0x10E,
        kNativeFault = 0x10F,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t machine_kind;
        uint32_t machine_id;
        uint32_t unit_id;
        uint32_t machine_index;
        uint32_t machine_page;
        uint32_t machine_slot;
        uint32_t candidate_index;
        uint32_t candidate_page;
        uint32_t candidate_slot;
        uint32_t machine_count;
        uint32_t candidate_count;
        uint32_t machine_pointer;
        uint32_t machine_row_pointer;
        uint32_t candidate_service_pointer;
        uint32_t candidate_control_pointer;
        uint32_t unitview_pointer;
        uint32_t unitview_dialog_pointer;
        uint32_t unitview_render_pointer;
        uint32_t unitview_model_pointer;
        uint32_t unitview_event_sequence;
        uint32_t unitview_event_unit;
        uint32_t unitview_event_service;
        int32_t render_width;
        int32_t render_height;
        uint32_t machine_kind_invocations;
        uint32_t machine_page_invocations;
        uint32_t machine_select_invocations;
        uint32_t custom_open_invocations;
        uint32_t custom_page_invocations;
        uint32_t candidate_invocations;
        uint8_t machine_root_visible;
        uint8_t candidate_control_visible;
        uint8_t unitview_event_consumed;
        uint8_t unitview_dialog_visible;
        uint8_t unitview_render_visible;
        uint8_t unitview_model_ready;
        uint8_t reserved[2];
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        HangarNavigation* navigation,
        const Config& config)
    {
        if (bound_ || !api || !mod || !navigation ||
            (config.machine_kind != kFixedMachine &&
             config.machine_kind != kCustomMachine) ||
            !config.machine_id || !config.unit_id)
            return false;

        api_ = api;
        mod_ = mod;
        navigation_ = navigation;
        config_ = config;

        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>("flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>("flow.native_ui_find_control");

        machineVtable_ =
            Symbol<void*>("flow.capsule_machine_vtable");
        machineCallback_ =
            Symbol<UiCallbackFn>("flow.capsule_machine_callback");
        customVtable_ =
            Symbol<void*>("test.unit_acquisition.custom_vtable");
        customCallback_ =
            Symbol<UiCallbackFn>(
                "test.unit_acquisition.custom_callback");
        unitViewVtable_ =
            Symbol<void*>("test.unit_acquisition.unitview_vtable");
        unitViewHandler_ =
            Symbol<UnitViewHandlerFn>(
                "test.unit_acquisition.unitview_event_handler");
        modelSlot_ =
            Symbol<ModelSlotFn>("test.unit_index_model_slot");
        modelEntity_ =
            Symbol<ModelEntityFn>("test.unit_index_model_entity");

        if (!api_->HookCode || !serviceManagedPointer_ ||
            !serviceFindByName_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !nativeUiFindControl_ ||
            !machineVtable_ || !machineCallback_ ||
            !customVtable_ || !customCallback_ ||
            !unitViewVtable_ || !unitViewHandler_ ||
            !modelSlot_ || !modelEntity_)
            return false;

        __try {
            if (!SlotMatches(
                    machineVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(machineCallback_)) ||
                !SlotMatches(
                    customVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(customCallback_)) ||
                !SlotMatches(
                    unitViewVtable_,
                    kUnitViewEventVtableSlot,
                    reinterpret_cast<void*>(unitViewHandler_)))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (active_ && active_ != this)
            return false;
        active_ = this;
        if (!api_->HookCode(
                mod_,
                "test.unit_acquisition.unitview_event_handler",
                reinterpret_cast<void*>(&ObserveUnitViewEvent),
                reinterpret_cast<void**>(&unitViewHandler_))) {
            active_ = nullptr;
            return false;
        }

        state_ = {};
        state_.machine_kind = config_.machine_kind;
        state_.machine_id = config_.machine_id;
        state_.unit_id = config_.unit_id;
        InterlockedExchange(&lastResult_, kWaitingForHangar);
        InterlockedExchange(&unitViewEventSequence_, 0);
        InterlockedExchange(&unitViewEventUnit_, 0);
        InterlockedExchange(&unitViewEventService_, 0);
        bound_ = true;
        return true;
    }

    // Performs at most one original native UI action per call.
    Result Poll()
    {
        if (!bound_)
            return Publish(kUnbound);

        __try {
            void* machine = nullptr;
            const ServiceResult machineService =
                ResolveService(
                    L"roommgr",
                    machineVtable_,
                    &machine);
            if (machineService == kServiceMismatched)
                return Publish(kMachineServiceMismatch);
            if (machineService == kServiceAbsent)
                return OpenCapsuleRoom();

            auto* machineBytes =
                static_cast<uint8_t*>(machine);
            state_.machine_pointer = PointerValue(machine);
            auto* machineRoot =
                Read<uint8_t*>(
                    machineBytes + kMachineRoot);
            state_.machine_root_visible =
                machineRoot &&
                machineRoot[kControlVisible] != 0;
            if (!machineRoot ||
                !state_.machine_root_visible)
                return Publish(kWaitingForMachine);

            MachineTarget target = {};
            const ScanResult scan =
                ScanMachine(machineBytes, &target);
            if (scan == kScanWaiting)
                return Publish(kWaitingForMachine);
            if (scan == kScanInvalid)
                return Publish(kMachineDataMismatch);
            if (scan == kScanMissingMachine)
                return Publish(kTargetMachineMissing);
            if (scan == kScanMissingUnit)
                return Publish(kTargetUnitMissing);
            PublishMachineTarget(target);

            const uint32_t currentKind =
                Read<uint32_t>(
                    machineBytes + kMachineType);
            if (currentKind != config_.machine_kind)
                return SelectMachineKind(
                    machine,
                    machineRoot);

            const uint32_t effectivePages =
                Read<uint32_t>(
                    machineBytes + kMachinePageCount);
            if (!target.page_count ||
                effectivePages != target.page_count)
                return Publish(kMachineDataMismatch);

            const uint32_t currentPage =
                Read<uint32_t>(
                    machineBytes + kMachineCurrentPage);
            if (currentPage >= target.page_count)
                return Publish(kMachineDataMismatch);
            if (currentPage != target.machine_page)
                return PageMachines(
                    machine,
                    machineRoot,
                    currentPage,
                    target.machine_page);

            if (Read<uint32_t>(
                    machineBytes + kMachineSelectedIndex) !=
                    target.machine_index ||
                Read<uint32_t>(
                    machineBytes + kMachineSelectedSlot) !=
                    target.machine_slot)
                return SelectMachine(
                    machine,
                    machineRoot,
                    target);

            if (candidateInvoked_)
                return EvaluateUnitView();

            if (config_.machine_kind == kFixedMachine)
                return SelectFixedCandidate(
                    machine,
                    target);
            return PollCustomCandidate(
                machine,
                machineRoot);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    Observation State() const
    {
        Observation observed = state_;
        observed.result =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &lastResult_),
                    0,
                    0));
        observed.unitview_event_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitViewEventSequence_),
                    0,
                    0));
        observed.unitview_event_unit =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitViewEventUnit_),
                    0,
                    0));
        observed.unitview_event_service =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    const_cast<volatile LONG*>(
                        &unitViewEventService_),
                    0,
                    0));
        return observed;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kWaitingForHangar: return "waiting-for-hangar";
        case kOpeningCapsuleRoom: return "opening-capsule-room";
        case kWaitingForMachine: return "waiting-for-machine";
        case kSelectingMachineKind: return "selecting-machine-kind";
        case kPagingMachines: return "paging-machines";
        case kSelectingMachine: return "selecting-machine";
        case kWaitingForFixedCandidate:
            return "waiting-for-fixed-candidate";
        case kOpeningCustomManager:
            return "opening-custom-manager";
        case kWaitingForCustomManager:
            return "waiting-for-custom-manager";
        case kPagingCustomCandidates:
            return "paging-custom-candidates";
        case kSelectingCandidate: return "selecting-candidate";
        case kWaitingForUnitViewEvent:
            return "waiting-for-unitview-event";
        case kWaitingForUnitViewModel:
            return "waiting-for-unitview-model";
        case kReady: return "ready";
        case kInvalidConfig: return "invalid-config";
        case kNavigationMismatch: return "navigation-mismatch";
        case kMachineServiceMismatch:
            return "machine-service-mismatch";
        case kMachineDataMismatch:
            return "machine-data-mismatch";
        case kTargetMachineMissing:
            return "target-machine-missing";
        case kTargetUnitMissing: return "target-unit-missing";
        case kMachineControlMismatch:
            return "machine-control-mismatch";
        case kMachineActionRejected:
            return "machine-action-rejected";
        case kCustomServiceMismatch:
            return "custom-service-mismatch";
        case kCustomDataMismatch:
            return "custom-data-mismatch";
        case kCustomControlMismatch:
            return "custom-control-mismatch";
        case kCustomActionRejected:
            return "custom-action-rejected";
        case kUnitViewServiceMismatch:
            return "unitview-service-mismatch";
        case kUnitViewEventMismatch:
            return "unitview-event-mismatch";
        case kUnitViewControlMismatch:
            return "unitview-control-mismatch";
        case kNativeFault: return "native-fault";
        }
        return "unknown";
    }

private:
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using ChildLookupFn =
        void*(__thiscall*)(void*, const void*);
    using UiCallbackFn =
        void(__stdcall*)(void*, uint32_t, const void*, void*);
    using UnitViewHandlerFn =
        void(__thiscall*)(void*, const void*);
    using ModelSlotFn =
        void*(__thiscall*)(void*, uint32_t);
    using ModelEntityFn =
        void*(__thiscall*)(void*);

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
        kScanMissingMachine,
        kScanMissingUnit,
    };

    struct MachineTarget
    {
        const uint8_t* row;
        uint32_t machine_index;
        uint32_t machine_page;
        uint32_t machine_slot;
        uint32_t candidate_slot;
        uint32_t machine_count;
        uint32_t page_count;
    };

    struct CandidateTarget
    {
        uint32_t index;
        uint32_t page;
        uint32_t slot;
        uint32_t count;
    };

    template<class T>
    T Symbol(const char* name) const
    {
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    template<class T>
    static T Read(const void* source)
    {
        T value = {};
        if (source)
            memcpy(&value, source, sizeof(value));
        return value;
    }

    static uint32_t PointerValue(const void* pointer)
    {
        return static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(pointer));
    }

    static bool SlotMatches(
        const void* vtable,
        size_t offset,
        const void* expected)
    {
        return vtable && expected &&
            Read<void*>(
                static_cast<const uint8_t*>(
                    vtable) +
                offset) == expected;
    }

    Result Publish(Result result)
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        state_.result = result;
        return result;
    }

    Result OpenCapsuleRoom()
    {
        if (capsuleRoomInvoked_)
            return Publish(kWaitingForMachine);
        const auto hangar =
            navigation_->EnsureHangar();
        if (hangar >=
            HangarNavigation::kDownmenuServiceMismatch)
            return Publish(kNavigationMismatch);
        if (hangar != HangarNavigation::kReady)
            return Publish(kWaitingForHangar);
        const auto clicked =
            navigation_->ClickBottomRadio(
                L"CapsuleRoomRadio",
                kCapsuleRoomRadioOffset);
        if (clicked >=
            HangarNavigation::kDownmenuServiceMismatch)
            return Publish(kNavigationMismatch);
        if (clicked != HangarNavigation::kReady)
            return Publish(kWaitingForHangar);
        capsuleRoomInvoked_ = true;
        return Publish(kOpeningCapsuleRoom);
    }

    Result SelectMachineKind(
        void* machine,
        void* root)
    {
        const bool custom =
            config_.machine_kind == kCustomMachine;
        const size_t cachedOffset =
            custom
                ? kCustomMachineRadio
                : kFixedMachineRadio;
        const wchar_t* name =
            custom
                ? L"MachineRadio1"
                : L"MachineRadio0";
        auto* control =
            Read<uint8_t*>(
                static_cast<uint8_t*>(machine) +
                cachedOffset);
        if (!ValidateControl(
                root,
                control,
                name,
                kRadioControlKind,
                true,
                true))
            return Publish(kMachineControlMismatch);
        ++state_.machine_kind_invocations;
        machineCallback_(
            machine,
            kRadioEvent,
            control + kControlName,
            control);
        if (Read<uint32_t>(
                static_cast<uint8_t*>(machine) +
                kMachineType) !=
                config_.machine_kind)
            return Publish(kMachineActionRejected);
        return Publish(kSelectingMachineKind);
    }

    Result PageMachines(
        void* machine,
        void* root,
        uint32_t current,
        uint32_t target)
    {
        const bool right = current < target;
        const wchar_t* name =
            right
                ? L"CapsuleRightBtn"
                : L"CapsuleLeftBtn";
        auto* button =
            static_cast<uint8_t*>(
                FindControl(
                    root,
                    name,
                    kButtonControlKind));
        if (!ValidateControl(
                root,
                button,
                name,
                kButtonControlKind,
                true,
                true))
            return Publish(kMachineControlMismatch);
        ++state_.machine_page_invocations;
        machineCallback_(
            machine,
            kButtonEvent,
            button + kControlName,
            button);
        const uint32_t after =
            Read<uint32_t>(
                static_cast<uint8_t*>(machine) +
                kMachineCurrentPage);
        if (after !=
            (right ? current + 1 : current - 1))
            return Publish(kMachineActionRejected);
        return Publish(kPagingMachines);
    }

    Result SelectMachine(
        void* machine,
        void* root,
        const MachineTarget& target)
    {
        auto* bytes =
            static_cast<uint8_t*>(machine);
        auto* point =
            Read<uint8_t*>(
                bytes +
                kPointMachineTiles +
                target.machine_slot *
                    sizeof(void*));
        auto* event =
            Read<uint8_t*>(
                bytes +
                kEventMachineTiles +
                target.machine_slot *
                    sizeof(void*));
        wchar_t pointName[32] = {};
        wchar_t eventName[32] = {};
        swprintf_s(
            pointName,
            L"PointMachineRadio%u",
            target.machine_slot);
        swprintf_s(
            eventName,
            L"EventMachineRadio%u",
            target.machine_slot);
        const bool pointReady =
            ValidateControl(
                root,
                point,
                pointName,
                kRadioControlKind,
                true,
                true);
        const bool eventReady =
            ValidateControl(
                root,
                event,
                eventName,
                kRadioControlKind,
                true,
                true);
        if (pointReady == eventReady)
            return Publish(kMachineControlMismatch);
        auto* tile = pointReady ? point : event;
        ++state_.machine_select_invocations;
        machineCallback_(
            machine,
            kRadioEvent,
            tile + kControlName,
            tile);
        if (Read<uint32_t>(
                bytes + kMachineSelectedIndex) !=
                target.machine_index ||
            Read<uint32_t>(
                bytes + kMachineSelectedSlot) !=
                target.machine_slot)
            return Publish(kMachineActionRejected);
        return Publish(kSelectingMachine);
    }

    Result SelectFixedCandidate(
        void* machine,
        const MachineTarget& target)
    {
        if (Read<uint32_t>(
                static_cast<uint8_t*>(machine) +
                kMachineType) !=
                kFixedMachine)
            return Publish(kMachineDataMismatch);
        auto* frame =
            static_cast<uint8_t*>(
                FindChild(machine, L"frame"));
        if (!frame ||
            !frame[kControlVisible])
            return Publish(kWaitingForFixedCandidate);
        wchar_t name[24] = {};
        swprintf_s(
            name,
            L"UnitRadio%u",
            target.candidate_slot);
        auto* control =
            static_cast<uint8_t*>(
                FindControl(
                    frame,
                    name,
                    kRadioControlKind));
        if (!ValidateControl(
                frame,
                control,
                name,
                kRadioControlKind,
                true,
                true))
            return Publish(kMachineControlMismatch);
        state_.candidate_control_pointer =
            PointerValue(control);
        state_.candidate_control_visible = 1;
        ArmCandidateInvocation();
        machineCallback_(
            machine,
            kRadioEvent,
            control + kControlName,
            control);
        return Publish(kSelectingCandidate);
    }

    Result PollCustomCandidate(
        void* machine,
        void* machineRoot)
    {
        void* custom = nullptr;
        const ServiceResult customService =
            ResolveService(
                L"custommgr",
                customVtable_,
                &custom);
        if (customService == kServiceMismatched)
            return Publish(kCustomServiceMismatch);
        if (customService == kServiceAbsent) {
            if (customOpenInvoked_)
                return Publish(kWaitingForCustomManager);
            auto* button =
                static_cast<uint8_t*>(
                    FindControl(
                        machineRoot,
                        L"MachineSelectBtn",
                        kButtonControlKind));
            if (!ValidateControl(
                    machineRoot,
                    button,
                    L"MachineSelectBtn",
                    kButtonControlKind,
                    true,
                    true))
                return Publish(kMachineControlMismatch);
            customOpenInvoked_ = true;
            ++state_.custom_open_invocations;
            machineCallback_(
                machine,
                kButtonEvent,
                button + kControlName,
                button);
            return Publish(kOpeningCustomManager);
        }

        auto* bytes =
            static_cast<uint8_t*>(custom);
        state_.candidate_service_pointer =
            PointerValue(custom);
        auto* root =
            Read<uint8_t*>(
                bytes + kCustomRoot);
        if (!root ||
            !root[kControlVisible])
            return Publish(kWaitingForCustomManager);
        if (Read<uint32_t>(
                bytes + kCustomMachineId) !=
                config_.machine_id)
            return Publish(kCustomDataMismatch);

        CandidateTarget target = {};
        const ScanResult scan =
            ScanCustomCandidates(
                bytes,
                &target);
        if (scan == kScanWaiting)
            return Publish(kWaitingForCustomManager);
        if (scan == kScanInvalid)
            return Publish(kCustomDataMismatch);
        if (scan == kScanMissingUnit)
            return Publish(kTargetUnitMissing);
        state_.candidate_index = target.index;
        state_.candidate_page = target.page;
        state_.candidate_slot = target.slot;
        state_.candidate_count = target.count;

        const uint32_t currentPage =
            Read<uint32_t>(
                bytes + kCustomCurrentPage);
        if (currentPage != target.page)
            return PageCustomCandidates(
                custom,
                root,
                currentPage,
                target.page);

        wchar_t name[32] = {};
        swprintf_s(
            name,
            L"CapsuleUnitIcon%u",
            target.slot);
        auto* control =
            Read<uint8_t*>(
                bytes +
                kCustomCandidateControls +
                target.slot *
                    sizeof(void*));
        if (!ValidateControl(
                root,
                control,
                name,
                kIconControlKind,
                true,
                false))
            return Publish(kCustomControlMismatch);
        state_.candidate_control_pointer =
            PointerValue(control);
        state_.candidate_control_visible = 1;
        ArmCandidateInvocation();
        customCallback_(
            custom,
            kCustomUnitViewEvent,
            control + kControlName,
            control);
        return Publish(kSelectingCandidate);
    }

    Result PageCustomCandidates(
        void* custom,
        void* root,
        uint32_t current,
        uint32_t target)
    {
        auto* bytes =
            static_cast<uint8_t*>(custom);
        uint8_t* control = nullptr;
        const wchar_t* name = nullptr;
        wchar_t pageName[24] = {};
        const bool groupNavigation =
            current / kCustomPageButtons !=
            target / kCustomPageButtons;
        if (groupNavigation) {
            const bool right =
                current / kCustomPageButtons <
                target / kCustomPageButtons;
            name =
                right
                    ? L"UnitPageRightBtn"
                    : L"UntiPageLeftBtn";
            control =
                Read<uint8_t*>(
                    bytes +
                    (right
                        ? kCustomPageRight
                        : kCustomPageLeft));
        } else {
            const uint32_t local =
                target % kCustomPageButtons;
            swprintf_s(
                pageName,
                L"PageBtn%u",
                local);
            name = pageName;
            control =
                Read<uint8_t*>(
                    bytes +
                    kCustomPageButtonControls +
                    local *
                        sizeof(void*));
        }
        if (!ValidateControl(
                root,
                control,
                name,
                kButtonControlKind,
                true,
                true))
            return Publish(kCustomControlMismatch);
        ++state_.custom_page_invocations;
        customCallback_(
            custom,
            kButtonEvent,
            control + kControlName,
            control);
        const uint32_t after =
            Read<uint32_t>(
                bytes + kCustomCurrentPage);
        if ((!groupNavigation && after != target) ||
            (groupNavigation &&
             (after == current ||
              (current < target && after < current) ||
              (current > target && after > current))))
            return Publish(kCustomActionRejected);
        return Publish(kPagingCustomCandidates);
    }

    void ArmCandidateInvocation()
    {
        eventBaseline_ =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &unitViewEventSequence_,
                    0,
                    0));
        candidateInvoked_ = true;
        ++state_.candidate_invocations;
    }

    Result EvaluateUnitView()
    {
        const uint32_t sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &unitViewEventSequence_,
                    0,
                    0));
        if (sequence <= eventBaseline_)
            return Publish(kWaitingForUnitViewEvent);
        const uint32_t eventUnit =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &unitViewEventUnit_,
                    0,
                    0));
        if (eventUnit != config_.unit_id)
            return Publish(kUnitViewEventMismatch);

        void* unitView = nullptr;
        const ServiceResult service =
            ResolveService(
                L"unitview",
                unitViewVtable_,
                &unitView);
        if (service == kServiceMismatched)
            return Publish(kUnitViewServiceMismatch);
        if (service == kServiceAbsent)
            return Publish(kWaitingForUnitViewModel);
        state_.unitview_pointer =
            PointerValue(unitView);
        const uint32_t eventService =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &unitViewEventService_,
                    0,
                    0));
        if (eventService != PointerValue(unitView) ||
            Read<uint32_t>(
                static_cast<uint8_t*>(unitView) +
                kUnitViewTargetUnit) !=
                config_.unit_id)
            return Publish(kUnitViewEventMismatch);
        state_.unitview_event_consumed = 1;

        auto* dialog =
            static_cast<uint8_t*>(
                FindChild(unitView, L"dlg"));
        if (!dialog)
            return Publish(kWaitingForUnitViewModel);
        state_.unitview_dialog_pointer =
            PointerValue(dialog);
        state_.unitview_dialog_visible =
            dialog[kControlVisible] != 0;
        if (!state_.unitview_dialog_visible)
            return Publish(kWaitingForUnitViewModel);

        auto* render =
            static_cast<uint8_t*>(
                FindControl(
                    dialog,
                    L"MultiRender",
                    kRenderControlKind));
        if (!ValidateControl(
                dialog,
                render,
                L"MultiRender",
                kRenderControlKind,
                true,
                false))
            return Publish(kUnitViewControlMismatch);
        state_.unitview_render_pointer =
            PointerValue(render);
        state_.render_width =
            Read<int32_t>(
                render + kControlWidth);
        state_.render_height =
            Read<int32_t>(
                render + kControlHeight);
        state_.unitview_render_visible =
            render[kControlVisible] != 0 &&
            render[kRenderVisible] != 0;
        if (!state_.unitview_render_visible ||
            state_.render_width <= 0 ||
            state_.render_height <= 0)
            return Publish(kWaitingForUnitViewModel);

        void* slot = modelSlot_(render, 0);
        void* model =
            slot ? modelEntity_(slot) : nullptr;
        state_.unitview_model_pointer =
            PointerValue(model);
        state_.unitview_model_ready =
            model != nullptr;
        if (!model)
            return Publish(kWaitingForUnitViewModel);
        return Publish(kReady);
    }

    ScanResult ScanMachine(
        const uint8_t* service,
        MachineTarget* output) const
    {
        if (!service || !output)
            return kScanInvalid;
        *output = {};

        const bool fixed =
            config_.machine_kind == kFixedMachine;
        const size_t beginOffset =
            fixed
                ? kFixedMachineBegin
                : kCustomMachineBegin;
        const size_t endOffset =
            fixed
                ? kFixedMachineEnd
                : kCustomMachineEnd;
        const size_t capacityOffset =
            fixed
                ? kFixedMachineCapacity
                : kCustomMachineCapacity;
        const size_t stride =
            fixed
                ? kFixedMachineStride
                : kCustomMachineStride;
        const size_t pageCountOffset =
            fixed
                ? kFixedMachinePageCount
                : kCustomMachinePageCount;
        const uintptr_t begin =
            reinterpret_cast<uintptr_t>(
                Read<void*>(
                    service + beginOffset));
        const uintptr_t end =
            reinterpret_cast<uintptr_t>(
                Read<void*>(
                    service + endOffset));
        const uintptr_t capacity =
            reinterpret_cast<uintptr_t>(
                Read<void*>(
                    service + capacityOffset));
        if (!begin || !end || !capacity)
            return kScanWaiting;
        if (end < begin || capacity < end)
            return kScanInvalid;
        const uintptr_t bytes = end - begin;
        if (!bytes)
            return kScanWaiting;
        if (bytes % stride)
            return kScanInvalid;
        const uint32_t count =
            static_cast<uint32_t>(
                bytes / stride);
        if (!count ||
            count > kMaximumMachineRows ||
            count % kMachinesPerPage)
            return kScanInvalid;

        uint32_t machineMatches = 0;
        uint32_t unitMatches = 0;
        for (uint32_t index = 0;
             index < count;
             ++index) {
            const auto* row =
                reinterpret_cast<const uint8_t*>(
                    begin +
                    static_cast<uintptr_t>(
                        index) *
                    stride);
            if (Read<uint32_t>(
                    row + kMachineIdOffset) !=
                    config_.machine_id)
                continue;
            ++machineMatches;
            output->row = row;
            output->machine_index = index;
            output->machine_page =
                index / kMachinesPerPage;
            output->machine_slot =
                index % kMachinesPerPage;
            if (!fixed)
                continue;
            const uint32_t slots =
                Read<uint32_t>(
                    row + kFixedUnitCount);
            if (!slots ||
                slots > kFixedMaximumUnits)
                return kScanInvalid;
            for (uint32_t slot = 0;
                 slot < slots;
                 ++slot) {
                if (Read<uint32_t>(
                        row +
                        kFixedUnitIds +
                        slot *
                            sizeof(uint32_t)) ==
                    config_.unit_id) {
                    ++unitMatches;
                    output->candidate_slot = slot;
                }
            }
        }
        if (machineMatches != 1)
            return kScanMissingMachine;
        if (fixed && unitMatches != 1)
            return kScanMissingUnit;

        output->machine_count = count;
        output->page_count =
            Read<uint32_t>(
                service + pageCountOffset);
        if (!output->page_count ||
            output->page_count !=
                count / kMachinesPerPage ||
            output->machine_page >=
                output->page_count)
            return kScanInvalid;
        return kScanReady;
    }

    ScanResult ScanCustomCandidates(
        const uint8_t* custom,
        CandidateTarget* output) const
    {
        if (!custom || !output)
            return kScanInvalid;
        *output = {};
        const uintptr_t begin =
            reinterpret_cast<uintptr_t>(
                Read<void*>(
                    custom + kCustomCandidateBegin));
        const uintptr_t end =
            reinterpret_cast<uintptr_t>(
                Read<void*>(
                    custom + kCustomCandidateEnd));
        if (!begin || !end)
            return kScanWaiting;
        if (end < begin)
            return kScanInvalid;
        const uintptr_t bytes = end - begin;
        if (!bytes)
            return kScanWaiting;
        if (bytes % kCustomCandidateStride)
            return kScanInvalid;
        const uint32_t count =
            static_cast<uint32_t>(
                bytes / kCustomCandidateStride);
        if (!count ||
            count > kMaximumCandidateRows)
            return kScanInvalid;

        uint32_t matches = 0;
        for (uint32_t index = 0;
             index < count;
             ++index) {
            const auto* row =
                reinterpret_cast<const uint8_t*>(
                    begin +
                    static_cast<uintptr_t>(
                        index) *
                    kCustomCandidateStride);
            if (Read<uint32_t>(
                    row + kCustomCandidateUnitId) !=
                    config_.unit_id)
                continue;
            ++matches;
            output->index = index;
            output->page =
                index / kCustomCandidatesPerPage;
            output->slot =
                index % kCustomCandidatesPerPage;
        }
        if (matches != 1)
            return kScanMissingUnit;
        output->count = count;
        const uint32_t expectedLastPage =
            (count - 1) /
            kCustomCandidatesPerPage;
        if (Read<uint32_t>(
                custom + kCustomLastPage) !=
                expectedLastPage ||
            output->page > expectedLastPage)
            return kScanInvalid;
        return kScanReady;
    }

    void PublishMachineTarget(
        const MachineTarget& target)
    {
        state_.machine_index =
            target.machine_index;
        state_.machine_page =
            target.machine_page;
        state_.machine_slot =
            target.machine_slot;
        state_.candidate_slot =
            target.candidate_slot;
        state_.machine_count =
            target.machine_count;
        state_.machine_row_pointer =
            PointerValue(target.row);
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
            !*serviceManagedPointer_)
            return kServiceAbsent;
        alignas(4) uint8_t descriptor[28] = {};
        *reinterpret_cast<uint32_t*>(descriptor) = 1;
        bool made = false;
        void* service = nullptr;
        __try {
            made =
                nativeWStringCtor_(
                    descriptor + 4,
                    name) != nullptr;
            if (made)
                service =
                    serviceFindByName_(
                        *serviceManagedPointer_,
                        descriptor);
        } __finally {
            if (made)
                nativeWStringDtor_(
                    descriptor + 4);
        }
        if (!service)
            return kServiceAbsent;
        if (Read<void*>(service) !=
            expectedVtable)
            return kServiceMismatched;
        *output = service;
        return kServiceMatched;
    }

    void* FindChild(
        void* service,
        const wchar_t* name) const
    {
        if (!service || !name)
            return nullptr;
        auto* vtable =
            Read<uint8_t*>(service);
        const auto lookup =
            vtable
                ? Read<ChildLookupFn>(
                      vtable +
                      kChildLookupVtableSlot)
                : nullptr;
        if (!lookup)
            return nullptr;
        alignas(4) uint8_t nativeName[24] = {};
        bool made = false;
        void* child = nullptr;
        __try {
            made =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (made)
                child =
                    lookup(
                        service,
                        nativeName);
        } __finally {
            if (made)
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
        if (!root || !name)
            return nullptr;
        alignas(4) uint8_t nativeName[24] = {};
        bool made = false;
        void* control = nullptr;
        __try {
            made =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (made)
                control =
                    nativeUiFindControl_(
                        root,
                        nativeName,
                        kind);
        } __finally {
            if (made)
                nativeWStringDtor_(
                    nativeName);
        }
        return control;
    }

    bool ValidateControl(
        void* root,
        const uint8_t* control,
        const wchar_t* name,
        uint32_t kind,
        bool requireVisible,
        bool requireEnabled) const
    {
        if (!root || !control || !name ||
            FindControl(root, name, kind) !=
                control ||
            Read<void*>(
                control + kControlParent) !=
                root ||
            !ControlHasName(control, name))
            return false;
        if (requireVisible &&
            !control[kControlVisible])
            return false;
        return !requireEnabled ||
            control[kControlEnabled] != 0;
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
            Read<uint32_t>(
                nativeName +
                kNativeStringSize);
        const uint32_t capacity =
            Read<uint32_t>(
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
                : Read<const wchar_t*>(
                      nativeName);
        return value &&
            wmemcmp(
                value,
                expected,
                size) == 0;
    }

    static void __fastcall ObserveUnitViewEvent(
        void* self,
        void*,
        const void* event)
    {
        auto* current = active_;
        if (!current ||
            !current->unitViewHandler_)
            return;
        current->unitViewHandler_(
            self,
            event);
        __try {
            if (!event ||
                Read<uint16_t>(
                    static_cast<const uint8_t*>(
                        event) +
                    kEventCode) !=
                    kUnitViewEventCode)
                return;
            const uint32_t unit =
                Read<uint32_t>(
                    static_cast<const uint8_t*>(
                        event) +
                    kEventUnit);
            InterlockedExchange(
                &current->unitViewEventUnit_,
                static_cast<LONG>(unit));
            InterlockedExchange(
                &current->unitViewEventService_,
                static_cast<LONG>(
                    PointerValue(self)));
            InterlockedIncrement(
                &current->unitViewEventSequence_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    static constexpr size_t kUiCallbackVtableSlot = 0x38;
    static constexpr size_t kUnitViewEventVtableSlot = 0x68;
    static constexpr size_t kChildLookupVtableSlot = 0x5C;
    static constexpr size_t kCapsuleRoomRadioOffset = 0x4C;

    static constexpr uint32_t kRadioEvent = 0x301;
    static constexpr uint32_t kButtonEvent = 0x101;
    static constexpr uint32_t kCustomUnitViewEvent = 0x801;
    static constexpr uint16_t kUnitViewEventCode = 0x01F8;
    static constexpr uint32_t kRadioControlKind = 12;
    static constexpr uint32_t kButtonControlKind = 4;
    static constexpr uint32_t kIconControlKind = 1;
    static constexpr uint32_t kRenderControlKind = 16;

    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlWidth = 0x154;
    static constexpr size_t kControlHeight = 0x158;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kControlVisible = 0x162;
    static constexpr size_t kRenderVisible = 0x163;
    static constexpr size_t kControlEnabled = 0x167;
    static constexpr size_t kNativeStringSize = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;

    static constexpr size_t kMachineRoot = 0x60;
    static constexpr size_t kFixedMachineBegin = 0x64;
    static constexpr size_t kFixedMachineEnd = 0x68;
    static constexpr size_t kFixedMachineCapacity = 0x6C;
    static constexpr size_t kMachineCurrentPage = 0x78;
    static constexpr size_t kMachinePageCount = 0x7C;
    static constexpr size_t kMachineSelectedIndex = 0x80;
    static constexpr size_t kMachineSelectedSlot = 0x84;
    static constexpr size_t kPointMachineTiles = 0x94;
    static constexpr size_t kEventMachineTiles = 0xB4;
    static constexpr size_t kFixedMachineRadio = 0xD4;
    static constexpr size_t kCustomMachineRadio = 0xD8;
    static constexpr size_t kMachineType = 0x1A0;
    static constexpr size_t kCustomMachineBegin = 0x1C4;
    static constexpr size_t kCustomMachineEnd = 0x1C8;
    static constexpr size_t kCustomMachineCapacity = 0x1CC;
    static constexpr size_t kFixedMachinePageCount = 0x1D4;
    static constexpr size_t kCustomMachinePageCount = 0x1D8;
    static constexpr size_t kFixedMachineStride = 0x3FB;
    static constexpr size_t kCustomMachineStride = 0xE7;
    static constexpr size_t kMachineIdOffset = 5;
    static constexpr size_t kFixedUnitCount = 0x0B;
    static constexpr size_t kFixedUnitIds = 0x1B;
    static constexpr uint32_t kMachinesPerPage = 8;
    static constexpr uint32_t kFixedMaximumUnits = 6;
    static constexpr uint32_t kMaximumMachineRows = 4096;

    static constexpr size_t kCustomRoot = 0x44;
    static constexpr size_t kCustomCandidateControls = 0x78;
    static constexpr size_t kCustomPageLeft = 0x2B8;
    static constexpr size_t kCustomPageRight = 0x2BC;
    static constexpr size_t kCustomPageButtonControls = 0x2C0;
    static constexpr size_t kCustomCandidateBegin = 0x3B0;
    static constexpr size_t kCustomCandidateEnd = 0x3B4;
    static constexpr size_t kCustomMachineId = 0x3DC;
    static constexpr size_t kCustomCurrentPage = 0x428;
    static constexpr size_t kCustomLastPage = 0x42C;
    static constexpr size_t kCustomCandidateStride = 0x12;
    static constexpr size_t kCustomCandidateUnitId = 9;
    static constexpr uint32_t kCustomCandidatesPerPage = 24;
    static constexpr uint32_t kCustomPageButtons = 5;
    static constexpr uint32_t kMaximumCandidateRows = 16384;

    static constexpr size_t kEventCode = 4;
    static constexpr size_t kEventUnit = 0x2C;
    static constexpr size_t kUnitViewTargetUnit = 0x64;

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    HangarNavigation* navigation_ = nullptr;
    Config config_ = {};
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    void* machineVtable_ = nullptr;
    UiCallbackFn machineCallback_ = nullptr;
    void* customVtable_ = nullptr;
    UiCallbackFn customCallback_ = nullptr;
    void* unitViewVtable_ = nullptr;
    UnitViewHandlerFn unitViewHandler_ = nullptr;
    ModelSlotFn modelSlot_ = nullptr;
    ModelEntityFn modelEntity_ = nullptr;
    Observation state_ = {};
    volatile LONG lastResult_ = kUnbound;
    volatile LONG unitViewEventSequence_ = 0;
    volatile LONG unitViewEventUnit_ = 0;
    volatile LONG unitViewEventService_ = 0;
    uint32_t eventBaseline_ = 0;
    bool bound_ = false;
    bool capsuleRoomInvoked_ = false;
    bool customOpenInvoked_ = false;
    bool candidateInvoked_ = false;

    inline static UnitAcquisitionActions* active_ = nullptr;
};
