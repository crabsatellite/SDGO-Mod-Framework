#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "sdgo/sdgo_api.h"

namespace sdgo::login_recovery {

// Recreates either stock CServiceGate state-2 login service without advancing
// the gate state. The shipped derived Mode-4 gate registers LoginInterFace2;
// the base gate registers LoginInterFace. The caller supplies the initialization
// context observed as GateInit(self, param1); the original GateInit stores that
// same value at gate+0x2060 before the normal service-manager activation.
//
// Disconnected-return integration:
// - wait until the native Mode 4 teardown/rebuild has produced the new gate;
// - keep nickname and ordinary user-waiting paths under their stock owner;
// - for a terminal login-recovery failure with no welcome reply, call
//   EnsureVisible(captured_param1);
// - treat Present() as return readiness. Stock gate state 3 remains the
//   separate online-ready condition.
//
// This class never reads or writes the gate state, never writes gate data, and
// never changes the username/password controls. CServiceGateLogin::Initialize
// remains responsible for restoring the native saved-username preference.
class NativeLoginUi
{
public:
    enum class Variant
    {
        // Prefer the shipped derived Mode-4 UI when its exact constructor,
        // destructor and vtable are available. A caller that captured the
        // base GateInit can select kBase explicitly.
        kAuto,
        kBase,
        kDerived,
    };

    struct View
    {
        void* service = nullptr;
        void* root = nullptr;
        void* login_button = nullptr;
        void* username_edit = nullptr;
        void* password_edit = nullptr;
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod)
    {
        if (bound_)
            return false;

        api_ = api;
        mod_ = mod;
        manager_ =
            Symbol<ServiceManagerFn>(
                "login_recovery.client.service_manager");
        findByName_ =
            Symbol<ServiceFindByNameFn>(
                "flow.service_find_by_name");
        wstringCtor_ =
            Symbol<NativeWStringCtorFn>(
                "flow.native_wstring_ctor");
        wstringDtor_ =
            Symbol<NativeWStringDtorFn>(
                "flow.native_wstring_dtor");
        serviceCtor_ =
            Symbol<ServiceCtorFn>(
                "login_recovery.client.service_base_ctor");
        serviceDtor_ =
            Symbol<ServiceDtorFn>(
                "login_recovery.client.service_base_dtor");
        cloneService_ =
            Symbol<CloneServiceFn>(
                "login_recovery.client.service_clone");
        registerService_ =
            Symbol<RegisterServiceFn>(
                "login_recovery.client.service_register");
        activateServices_ =
            Symbol<ActivateServicesFn>(
                "login_recovery.client.service_activate");
        loginVtable_ =
            Symbol<void*>(
                "login_recovery.client.login_ui_vtable");
        login2Ctor_ =
            Symbol<ServiceCtorFn>(
                "login_recovery.client.login_ui2_ctor");
        login2Dtor_ =
            Symbol<ServiceDtorFn>(
                "login_recovery.client.login_ui2_dtor");
        login2Vtable_ =
            Symbol<void*>(
                "login_recovery.client.login_ui2_vtable");

        const bool noDerivedSymbols =
            !login2Ctor_ && !login2Dtor_ &&
            !login2Vtable_;
        derivedAvailable_ =
            login2Ctor_ && login2Dtor_ &&
            login2Vtable_;

        bound_ =
            api_ && mod_ && manager_ && findByName_ &&
            wstringCtor_ && wstringDtor_ &&
            serviceCtor_ && serviceDtor_ &&
            cloneService_ && registerService_ &&
            activateServices_ && loginVtable_ &&
            (noDerivedSymbols || derivedAvailable_);
        return bound_;
    }

    bool Present(View* output = nullptr) const
    {
        View view = {};
        void* manager = nullptr;
        const ServiceState state =
            Resolve(&manager, &view.service);
        if (state != ServiceState::kMatched ||
            !ReadReadyView(view.service, &view))
            return false;
        if (output)
            *output = view;
        return true;
    }

    bool EnsureVisible(
        void* initializationContext,
        View* output = nullptr) const
    {
        return EnsureVisible(
            initializationContext,
            Variant::kAuto,
            output);
    }

    bool EnsureVisible(
        void* initializationContext,
        Variant requested,
        View* output = nullptr) const
    {
        if (!bound_)
            return false;

        void* manager = nullptr;
        void* service = nullptr;
        ServiceState state = Resolve(&manager, &service);
        if (state == ServiceState::kMismatched ||
            !manager)
            return false;

        // A matching service can exist between registration and activation.
        // Re-running the native manager activation is safe: CService::Activate
        // ignores already-active services and initializes only inactive ones.
        if (state == ServiceState::kMatched) {
            if (!ReadReadyView(service, output)) {
                __try {
                    activateServices_(
                        manager,
                        initializationContext,
                        false);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    return false;
                }
            }
            return Present(output);
        }

        bool constructedName = false;
        bool constructedSource = false;
        bool sourceReady = false;
        bool registered = false;
        const Variant variant =
            requested == Variant::kAuto
                ? (derivedAvailable_
                       ? Variant::kDerived
                       : Variant::kBase)
                : requested;
        if (variant == Variant::kDerived &&
            !derivedAvailable_)
            return false;
        const wchar_t* serviceName =
            variant == Variant::kDerived
                ? L"LoginInterFace2"
                : L"LoginInterFace";
        alignas(4) std::uint8_t name[
            kNativeWStringSize] = {};
        alignas(4) std::uint8_t source[
            kLoginService2Size] = {};

        __try {
            __try {
                constructedName =
                    wstringCtor_(
                        name,
                        serviceName) != nullptr;
                if (constructedName) {
                    constructedSource =
                        ConstructSource(
                            variant,
                            source,
                            &sourceReady);

                    void* clone = sourceReady
                        ? cloneService_(source, false)
                        : nullptr;
                    if (clone) {
                        // register(..., false) is the exact stock path. It
                        // unconditionally transfers the clone to the manager;
                        // only the flag=true duplicate-rejection branch
                        // destroys a candidate.
                        void* managed =
                            registerService_(
                                manager,
                                clone,
                                name,
                                false);
                        registered = managed == clone;
                        if (registered) {
                            activateServices_(
                                manager,
                                initializationContext,
                                false);
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                registered = false;
            }
        }
        __finally {
            if (constructedSource) {
                DestroySource(
                    variant,
                    source);
            }
            if (constructedName)
                wstringDtor_(name);
        }

        return registered && Present(output);
    }

private:
    enum class ServiceState
    {
        kAbsent,
        kMatched,
        kMismatched,
    };

    static constexpr std::size_t
        kNativeWStringSize = 0x1C;
    static constexpr std::size_t
        kServiceDescriptorSize =
            sizeof(std::uint32_t) +
            kNativeWStringSize;
    static constexpr std::size_t
        kLoginService2Size = 0xA0;
    static constexpr std::size_t
        kVtable = 0x00;
    static constexpr std::size_t
        kActive = 0x04;
    static constexpr std::size_t
        kRetired = 0x2D;
    static constexpr std::size_t
        kRoot = 0x44;
    static constexpr std::size_t
        kLoginButton = 0x48;
    static constexpr std::size_t
        kUsernameEdit = 0x60;
    static constexpr std::size_t
        kPasswordEdit = 0x64;
    static constexpr std::size_t
        kLoginSelection = 0x6C;

    using ServiceManagerFn =
        void*(__cdecl*)();
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using ServiceCtorFn =
        void*(__thiscall*)(void*);
    using ServiceDtorFn =
        void(__thiscall*)(void*);
    using CloneServiceFn =
        void*(__cdecl*)(void*, bool);
    using RegisterServiceFn =
        void*(__thiscall*)(
            void*,
            void*,
            const void*,
            bool);
    using ActivateServicesFn =
        void(__thiscall*)(void*, void*, bool);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    ServiceState Resolve(
        void** managerOutput,
        void** serviceOutput) const
    {
        if (managerOutput)
            *managerOutput = nullptr;
        if (serviceOutput)
            *serviceOutput = nullptr;
        if (!bound_ || !managerOutput ||
            !serviceOutput)
            return ServiceState::kAbsent;

        void* manager = nullptr;
        void* service = nullptr;

        __try {
            manager = manager_();
            if (manager && derivedAvailable_) {
                service = FindNamed(
                    manager,
                    L"LoginInterFace2");
                if (service)
                    return ResolveResult(
                        manager,
                        service,
                        login2Vtable_,
                        managerOutput,
                        serviceOutput);
            }
            if (manager) {
                service = FindNamed(
                    manager,
                    L"LoginInterFace");
                if (service)
                    return ResolveResult(
                        manager,
                        service,
                        loginVtable_,
                        managerOutput,
                        serviceOutput);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            manager = nullptr;
            service = nullptr;
        }

        *managerOutput = manager;
        *serviceOutput = service;
        return ServiceState::kAbsent;
    }

    ServiceState ResolveResult(
        void* manager,
        void* service,
        void* expectedVtable,
        void** managerOutput,
        void** serviceOutput) const
    {
        void* actualVtable = nullptr;
        __try {
            actualVtable =
                *static_cast<void**>(service);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return ServiceState::kMismatched;
        }
        *managerOutput = manager;
        *serviceOutput = service;
        return actualVtable == expectedVtable
            ? ServiceState::kMatched
            : ServiceState::kMismatched;
    }

    void* FindNamed(
        void* manager,
        const wchar_t* name) const
    {
        if (!manager || !name)
            return nullptr;
        alignas(4) std::uint8_t descriptor[
            kServiceDescriptorSize] = {};
        bool constructed = false;
        void* service = nullptr;
        __try {
            __try {
                *reinterpret_cast<std::uint32_t*>(
                    descriptor) = 1;
                constructed =
                    wstringCtor_(
                        descriptor +
                            sizeof(std::uint32_t),
                        name) != nullptr;
                if (constructed) {
                    service =
                        findByName_(
                            manager,
                            descriptor);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                service = nullptr;
            }
        }
        __finally {
            if (constructed) {
                wstringDtor_(
                    descriptor +
                        sizeof(std::uint32_t));
            }
        }
        return service;
    }

    bool ConstructSource(
        Variant variant,
        std::uint8_t* source,
        bool* ready) const
    {
        if (!source || !ready)
            return false;
        *ready = false;

        if (variant == Variant::kDerived) {
            if (!login2Ctor_)
                return false;
            void* result = login2Ctor_(source);
            if (!result)
                return false;
            *ready =
                result == source &&
                SourceMatches(source, variant);
            return true;
        }

        if (!serviceCtor_)
            return false;
        void* result = serviceCtor_(source);
        if (!result)
            return false;
        *reinterpret_cast<void**>(
            source + kVtable) = loginVtable_;
        *reinterpret_cast<std::int32_t*>(
            source + kLoginSelection) = -1;
        *ready =
            result == source &&
            SourceMatches(source, variant);
        return true;
    }

    void DestroySource(
        Variant variant,
        std::uint8_t* source) const
    {
        if (variant == Variant::kDerived) {
            login2Dtor_(source);
            return;
        }
        // The base stock path constructs CService first, then installs the
        // stateless LoginInterFace vtable before cloning.
        *reinterpret_cast<void**>(
            source + kVtable) = loginVtable_;
        serviceDtor_(source);
    }

    bool SourceMatches(
        const std::uint8_t* source,
        Variant variant) const
    {
        return source &&
            *reinterpret_cast<void* const*>(
                source + kVtable) ==
                (variant == Variant::kDerived
                     ? login2Vtable_
                     : loginVtable_);
    }

    bool IsLoginVtable(
        void* vtable) const
    {
        return vtable == loginVtable_ ||
            (derivedAvailable_ &&
             vtable == login2Vtable_);
    }

    bool ReadReadyView(
        void* service,
        View* output) const
    {
        if (!service)
            return false;

        View view = {};
        bool ready = false;
        __try {
            auto* bytes =
                static_cast<std::uint8_t*>(
                    service);
            ready =
                IsLoginVtable(
                    *reinterpret_cast<void**>(
                        bytes + kVtable)) &&
                bytes[kActive] != 0 &&
                bytes[kRetired] == 0;
            if (ready) {
                view.service = service;
                view.root =
                    *reinterpret_cast<void**>(
                        bytes + kRoot);
                view.login_button =
                    *reinterpret_cast<void**>(
                        bytes + kLoginButton);
                view.username_edit =
                    *reinterpret_cast<void**>(
                        bytes + kUsernameEdit);
                view.password_edit =
                    *reinterpret_cast<void**>(
                        bytes + kPasswordEdit);
                ready =
                    view.root &&
                    view.login_button &&
                    view.username_edit &&
                    view.password_edit;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ready = false;
        }
        if (ready && output)
            *output = view;
        return ready;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    ServiceManagerFn manager_ = nullptr;
    ServiceFindByNameFn findByName_ = nullptr;
    NativeWStringCtorFn wstringCtor_ = nullptr;
    NativeWStringDtorFn wstringDtor_ = nullptr;
    ServiceCtorFn serviceCtor_ = nullptr;
    ServiceDtorFn serviceDtor_ = nullptr;
    CloneServiceFn cloneService_ = nullptr;
    RegisterServiceFn registerService_ = nullptr;
    ActivateServicesFn activateServices_ = nullptr;
    void* loginVtable_ = nullptr;
    ServiceCtorFn login2Ctor_ = nullptr;
    ServiceDtorFn login2Dtor_ = nullptr;
    void* login2Vtable_ = nullptr;
    bool derivedAvailable_ = false;
    bool bound_ = false;
};

} // namespace sdgo::login_recovery
