#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hangar_navigation.h"
#include "native_actions.h"

// A development-only early-navigation observer. It invokes only the shipped
// enabled UI callbacks exposed by HangarNavigation, then reads the target
// service until its own native loading state settles. It does not construct a
// page, write an application state, or manufacture incomplete inventory data.
class LoadingNavigationActions
{
public:
    enum Target : uint32_t
    {
        kOff = 0,
        kCatalog = 1,
        kCapsule = 2,
        kRoom = 3,
    };

    enum Result : uint32_t
    {
        kWaitingForSource = 1,
        kClicked = 2,
        kWaitingForTarget = 3,
        kTargetReady = 4,
        kRecovering = 5,
        kRecovered = 6,
        kRoomNavigationAccepted = 7,

        kUnbound = 0x100,
        kInvalidTarget = 0x101,
        kSourceMismatch = 0x102,
        kTargetServiceMismatch = 0x103,
        kTargetStateInvalid = 0x104,
        kNativeFault = 0x105,
    };

    struct Observation
    {
        uint32_t target = 0;
        uint32_t result = 0;
        uint32_t native_app_state = UINT32_MAX;
        uint32_t source_result = 0;
        uint32_t click_elapsed_ms = 0;
        uint32_t ready_elapsed_ms = 0;
        uint32_t recovered_elapsed_ms = 0;
        uint32_t target_loading_flags = 0;
        uint32_t target_queue_state = 0;
        uint32_t target_visible_items = 0;
        uint8_t clicked = 0;
        uint8_t target_ready_at_click = 0;
        uint8_t inventory_complete_at_click = 0;
        uint8_t target_service_present = 0;
        uint8_t target_root_visible = 0;
        uint8_t room_owner_valid = 0;
        uint8_t room_manager_valid = 0;
        uint8_t room_main_ready = 0;
        uint8_t room_model_present = 0;
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native,
        HangarNavigation* navigation)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        navigation_ = navigation;
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>("flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        catalogVtable_ =
            Symbol<void*>("test.unit_index_list_vtable");
        capsuleVtable_ =
            Symbol<void*>("flow.capsule_machine_vtable");
        roomListVtable_ =
            Symbol<void*>("flow.native_room_list_vtable");
        modalLoadingVtable_ =
            Symbol<void*>("flow.native_modal_loading_vtable");
        gameApp_ =
            Symbol<GameAppFn>("game.app");
        waitRoomVtable_ =
            Symbol<void*>("flow.three_d_wait_room_vtable");
        normalRoomManagerVtable_ =
            Symbol<void*>("flow.normal_3d_manager_vtable");
        aiRoomManagerVtable_ =
            Symbol<void*>("flow.ai_3d_manager_vtable");
        normalRoomMainVtable_ =
            Symbol<void*>("flow.normal_3d_main_state_vtable");
        bound_ = api_ && mod_ && native_ && navigation_ &&
            serviceManagedPointer_ && serviceFindByName_ &&
            nativeWStringCtor_ && nativeWStringDtor_ &&
            catalogVtable_ && capsuleVtable_ &&
            roomListVtable_ && modalLoadingVtable_ &&
            gameApp_ &&
            waitRoomVtable_ && normalRoomManagerVtable_ &&
            aiRoomManagerVtable_ &&
            normalRoomMainVtable_;
        Reset();
        return bound_;
    }

    void Reset()
    {
        state_ = {};
        state_.native_app_state = UINT32_MAX;
        target_ = kOff;
        recoveryClicked_ = false;
    }

    Result Poll(
        Target target,
        uint64_t elapsedMs,
        bool inventoryComplete)
    {
        if (!bound_ || !native_ || !navigation_)
            return Publish(kUnbound);
        if (target != kCatalog &&
            target != kCapsule &&
            target != kRoom)
            return Publish(kInvalidTarget);
        if (target_ == kOff)
            target_ = target;
        if (target_ != target)
            return Publish(kInvalidTarget);

        __try {
            state_.target = static_cast<uint32_t>(target_);
            state_.native_app_state = native_->AppState();
            if (!state_.clicked) {
                const TargetState before =
                    target_ == kRoom
                        ? TargetState{}
                        : ReadTarget();
                CopyTarget(before);
                state_.target_ready_at_click =
                    static_cast<uint8_t>(
                        target_ != kRoom &&
                        !before.mismatch &&
                        before.ready);
                state_.inventory_complete_at_click =
                    static_cast<uint8_t>(inventoryComplete);
                const HangarNavigation::Result source =
                    target_ == kCatalog
                        ? navigation_->ClickOperatorMenuButton(
                              L"UnitListBtn",
                              0x84)
                        : navigation_->ClickBottomRadio(
                              target_ == kCapsule
                                  ? L"CapsuleRoomRadio"
                                  : L"GameRoomRadio",
                              target_ == kCapsule ? 0x4C : 0x48);
                state_.source_result =
                    static_cast<uint32_t>(source);
                if (source == HangarNavigation::kUnbound ||
                    source >=
                        HangarNavigation::kDownmenuServiceMismatch)
                    return Publish(kSourceMismatch);
                if (source != HangarNavigation::kReady)
                    return Publish(kWaitingForSource);
                state_.clicked = 1;
                state_.click_elapsed_ms =
                    static_cast<uint32_t>(elapsedMs);
                return Publish(kClicked);
            }

            if (target_ == kRoom)
                return state_.native_app_state == kGameRoomAppState
                    ? Publish(kRoomNavigationAccepted)
                    : Publish(kWaitingForTarget);
            const TargetState current = ReadTarget();
            CopyTarget(current);
            // roommgr is also a genuine source-lobby service. While entering
            // the capsule page, its old owner may remain registered until the
            // native transition replaces it. Only the expected vtable can
            // become ready; a foreign source owner is therefore still a wait.
            if (current.mismatch)
                return elapsedMs - state_.click_elapsed_ms < 30000
                    ? Publish(kWaitingForTarget)
                    : Publish(kTargetServiceMismatch);
            if (!current.ready)
                return Publish(kWaitingForTarget);
            if (!state_.ready_elapsed_ms)
                state_.ready_elapsed_ms =
                    static_cast<uint32_t>(elapsedMs);
            return Publish(kTargetReady);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    Result Recover(uint64_t elapsedMs)
    {
        if (!bound_ || !native_ || !navigation_)
            return Publish(kUnbound);
        if (target_ != kCatalog && target_ != kCapsule)
            return Publish(kInvalidTarget);
        __try {
            state_.native_app_state = native_->AppState();
            const TargetState current = ReadTarget();
            CopyTarget(current);
            if (recoveryClicked_ &&
                state_.native_app_state == kGameRoomAppState &&
                state_.ready_elapsed_ms &&
                !current.ready) {
                // UnitCatalogList keeps its registered root and the root's
                // default visible bit after the native scene switch. Its
                // settled item set is revoked, however. Accept recovery only
                // after that transition and after the shipped room-list
                // service exposes a genuine enabled creation control.
                const auto destination =
                    navigation_->EnsureGameRoom(
                        roomListVtable_,
                        modalLoadingVtable_);
                if (destination == HangarNavigation::kUnbound)
                    return Publish(kUnbound);
                if (destination >=
                    HangarNavigation::kDownmenuServiceMismatch)
                    return Publish(kSourceMismatch);
                if (destination == HangarNavigation::kReady) {
                    state_.recovered_elapsed_ms =
                        static_cast<uint32_t>(elapsedMs);
                    return Publish(kRecovered);
                }
            }
            if (!recoveryClicked_) {
                const auto result =
                    navigation_->ClickBottomRadio(
                        L"GameRoomRadio",
                        0x48);
                state_.source_result =
                    static_cast<uint32_t>(result);
                if (result == HangarNavigation::kUnbound ||
                    result >=
                        HangarNavigation::kDownmenuServiceMismatch)
                    return Publish(kSourceMismatch);
                if (result != HangarNavigation::kReady)
                    return Publish(kRecovering);
                recoveryClicked_ = true;
            }
            return Publish(kRecovering);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    Result ObserveThreeDRoom(uint64_t elapsedMs)
    {
        if (!bound_ || target_ != kRoom || !gameApp_)
            return Publish(kUnbound);
        __try {
            state_.native_app_state = native_->AppState();
            uint8_t* app = gameApp_();
            if (!app ||
                *reinterpret_cast<const uint32_t*>(
                    app + kGameAppState) !=
                    kThreeDWaitRoomState)
                return Publish(kWaitingForTarget);
            uint8_t* owner =
                *reinterpret_cast<uint8_t**>(
                    app + kGameAppStateObject);
            state_.room_owner_valid =
                static_cast<uint8_t>(
                    owner &&
                    *reinterpret_cast<void**>(owner) ==
                        waitRoomVtable_);
            if (!state_.room_owner_valid)
                return Publish(kTargetStateInvalid);
            uint8_t* manager =
                *reinterpret_cast<uint8_t**>(
                    owner + kWaitRoomObject);
            state_.room_manager_valid =
                static_cast<uint8_t>(
                    manager &&
                    (*reinterpret_cast<void**>(manager) ==
                         normalRoomManagerVtable_ ||
                     *reinterpret_cast<void**>(manager) ==
                         aiRoomManagerVtable_));
            if (!state_.room_manager_valid)
                return Publish(kTargetStateInvalid);
            state_.room_model_present =
                static_cast<uint8_t>(
                    *reinterpret_cast<void**>(
                        manager + kRoomModel) != nullptr);
            uint8_t** states =
                *reinterpret_cast<uint8_t***>(
                    manager + kThreeDRoomStateArray);
            uint8_t* main = states ? states[0] : nullptr;
            state_.room_main_ready =
                static_cast<uint8_t>(
                    main &&
                    *reinterpret_cast<void**>(main) ==
                        normalRoomMainVtable_ &&
                    *reinterpret_cast<const uint32_t*>(
                        main + kRoomStateStage) == 2 &&
                    *reinterpret_cast<uint8_t**>(
                        main + kRoomStateContext) ==
                        manager + kRoomContext &&
                    *reinterpret_cast<uint8_t**>(
                        main + kRoomStateOwner) ==
                        manager);
            if (!state_.room_model_present ||
                !state_.room_main_ready)
                return Publish(kWaitingForTarget);
            if (!state_.ready_elapsed_ms)
                state_.ready_elapsed_ms =
                    static_cast<uint32_t>(elapsedMs);
            return Publish(kTargetReady);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    void MarkRoomRecovered(uint64_t elapsedMs)
    {
        state_.native_app_state =
            native_ ? native_->AppState() : UINT32_MAX;
        state_.recovered_elapsed_ms =
            static_cast<uint32_t>(elapsedMs);
        Publish(kRecovered);
    }

    const Observation& State() const
    {
        return state_;
    }

private:
    struct TargetState
    {
        bool present = false;
        bool ready = false;
        bool mismatch = false;
        bool rootVisible = false;
        uint32_t loadingFlags = 0;
        uint32_t queueState = 0;
        uint32_t visibleItems = 0;
    };

    template <class T>
    T Symbol(const char* name) const
    {
        return api_ && api_->Symbol
            ? reinterpret_cast<T>(
                  api_->Symbol(mod_, name))
            : nullptr;
    }

    void* Resolve(const wchar_t* name) const
    {
        if (!name || !serviceManagedPointer_ ||
            !serviceFindByName_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !*serviceManagedPointer_)
            return nullptr;
        alignas(4) uint8_t descriptor[
            kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    descriptor + sizeof(uint32_t),
                    name) != nullptr;
            if (constructed)
                service =
                    serviceFindByName_(
                        *serviceManagedPointer_,
                        descriptor);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    descriptor + sizeof(uint32_t));
        }
        return service;
    }

    TargetState ReadTarget() const
    {
        TargetState result = {};
        const wchar_t* name =
            target_ == kCatalog
                ? L"UnitCatalogList"
                : L"roommgr";
        void* expected =
            target_ == kCatalog
                ? catalogVtable_
                : capsuleVtable_;
        uint8_t* service =
            static_cast<uint8_t*>(Resolve(name));
        if (!service)
            return result;
        if (*reinterpret_cast<void**>(service) != expected) {
            result.mismatch = true;
            return result;
        }
        result.present = true;
        if (target_ == kCatalog) {
            uint8_t* root =
                *reinterpret_cast<uint8_t**>(
                    service + kCatalogRoot);
            result.rootVisible =
                root && root[kControlVisible];
            result.loadingFlags =
                service[kCatalogLoading] |
                (static_cast<uint32_t>(
                     service[kCatalogLoading + 1])
                 << 8);
            result.queueState =
                *reinterpret_cast<const uint32_t*>(
                    service + kCatalogQueue);
            for (unsigned index = 0;
                 index < kCatalogSlots;
                 ++index) {
                if (*reinterpret_cast<const uint32_t*>(
                        service + kCatalogFirstItem +
                        index * sizeof(uint32_t)))
                    ++result.visibleItems;
            }
            result.ready =
                result.rootVisible &&
                !result.loadingFlags &&
                !result.queueState &&
                result.visibleItems != 0;
        } else {
            uint8_t* root =
                *reinterpret_cast<uint8_t**>(
                    service + kCapsuleRoot);
            result.rootVisible =
                root && root[kControlVisible];
            result.visibleItems =
                *reinterpret_cast<const uint32_t*>(
                    service + kCapsulePageCount);
            result.ready =
                result.rootVisible &&
                result.visibleItems != 0;
        }
        return result;
    }

    void CopyTarget(const TargetState& value)
    {
        state_.target_service_present =
            static_cast<uint8_t>(value.present);
        state_.target_root_visible =
            static_cast<uint8_t>(value.rootVisible);
        state_.target_loading_flags =
            value.loadingFlags;
        state_.target_queue_state =
            value.queueState;
        state_.target_visible_items =
            value.visibleItems;
    }

    Result Publish(Result result)
    {
        state_.result = static_cast<uint32_t>(result);
        return result;
    }

    static constexpr uint32_t kGameRoomAppState = 6;
    static constexpr uint32_t kThreeDWaitRoomState = 18;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kControlVisible = 0x162;

    static constexpr size_t kCatalogRoot = 0x44;
    static constexpr size_t kCatalogQueue = 0x300;
    static constexpr size_t kCatalogFirstItem = 0x3F8;
    static constexpr size_t kCatalogLoading = 0x472;
    static constexpr unsigned kCatalogSlots = 21;

    static constexpr size_t kCapsuleRoot = 0x60;
    static constexpr size_t kCapsulePageCount = 0x7C;

    static constexpr size_t kGameAppState = 0x4A0;
    static constexpr size_t kGameAppStateObject = 0x4A4;
    static constexpr size_t kWaitRoomObject = 0x04;
    static constexpr size_t kThreeDRoomStateArray = 0x0C;
    static constexpr size_t kRoomContext = 0x24;
    static constexpr size_t kRoomModel = 0x38;
    static constexpr size_t kRoomStateStage = 0x08;
    static constexpr size_t kRoomStateContext = 0x0C;
    static constexpr size_t kRoomStateOwner = 0x10;

    using GameAppFn = uint8_t*(__cdecl*)();
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    HangarNavigation* navigation_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    GameAppFn gameApp_ = nullptr;
    void* catalogVtable_ = nullptr;
    void* capsuleVtable_ = nullptr;
    void* roomListVtable_ = nullptr;
    void* modalLoadingVtable_ = nullptr;
    void* waitRoomVtable_ = nullptr;
    void* normalRoomManagerVtable_ = nullptr;
    void* aiRoomManagerVtable_ = nullptr;
    void* normalRoomMainVtable_ = nullptr;
    Target target_ = kOff;
    Observation state_ = {};
    bool bound_ = false;
    bool recoveryClicked_ = false;
};
