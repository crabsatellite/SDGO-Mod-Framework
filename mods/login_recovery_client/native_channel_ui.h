#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "sdgo/sdgo_api.h"

namespace sdgo::login_recovery {

// Recognizes the shipped SERVICE_CHANNEL owner and its native channel menu.
// WaitingForUser is read-only: it resolves the current owner and named service
// on every call, and reports true only after loading completed and an
// interactive menu or populated selector is visible without a request pending.
class NativeChannelUi
{
public:
    bool Bind(const SdgoApi* api, SdgoMod* mod)
    {
        if (bound_)
            return false;

        api_ = api;
        mod_ = mod;
        manager_ =
            Symbol<ServiceManagerFn>(
                "recovery.manager");
        findByName_ =
            Symbol<ServiceFindByNameFn>(
                "flow.service_find_by_name");
        wstringCtor_ =
            Symbol<NativeWStringCtorFn>(
                "flow.native_wstring_ctor");
        wstringDtor_ =
            Symbol<NativeWStringDtorFn>(
                "flow.native_wstring_dtor");
        channelOwnerVtable_ =
            Symbol<void*>(
                "recovery.channel_owner_vtable");
        channelMenuVtable_ =
            Symbol<void*>(
                "recovery.channel_menu_vtable");
        vsChannelVtable_ =
            Symbol<void*>(
                "recovery.vs_channel_vtable");
        missionChannelVtable_ =
            Symbol<void*>(
                "recovery.mission_channel_vtable");
        clanChannelVtable_ =
            Symbol<void*>(
                "recovery.clan_channel_vtable");

        const bool noChannelSymbols =
            !channelOwnerVtable_ &&
            !channelMenuVtable_ &&
            !vsChannelVtable_ &&
            !missionChannelVtable_ &&
            !clanChannelVtable_;
        channelSymbolsAvailable_ =
            channelOwnerVtable_ &&
            channelMenuVtable_ &&
            vsChannelVtable_ &&
            missionChannelVtable_ &&
            clanChannelVtable_;

        bound_ =
            api_ && mod_ && manager_ &&
            findByName_ && wstringCtor_ &&
            wstringDtor_ &&
            (noChannelSymbols ||
             channelSymbolsAvailable_);
        return bound_;
    }

    bool WaitingForUser(void* app) const
    {
        if (!bound_ || !channelSymbolsAvailable_ ||
            !app)
            return false;

        bool waiting = false;
        __try {
            auto* appBytes =
                static_cast<std::uint8_t*>(app);
            if (*reinterpret_cast<std::uint32_t*>(
                    appBytes + kAppState) !=
                kServiceChannelState)
                return false;

            void* owner =
                *reinterpret_cast<void**>(
                    appBytes + kAppOwner);
            if (!owner ||
                *static_cast<void**>(owner) !=
                    channelOwnerVtable_)
                return false;

            auto* ownerBytes =
                static_cast<std::uint8_t*>(
                    owner);
            if (ownerBytes[kChannelReady] != 1 ||
                *reinterpret_cast<std::uint32_t*>(
                    ownerBytes +
                    kChannelLoadState) !=
                    kChannelLoaded)
                return false;

            void* manager = manager_();
            if (!manager)
                return false;

            const std::uint32_t mode =
                *reinterpret_cast<std::uint32_t*>(
                    ownerBytes +
                    kChannelMode);
            switch (mode) {
            case 0:
            case 2:
            case 4:
            case 6:
            case 8:
                waiting =
                    MenuWaiting(manager);
                break;
            case 1:
                waiting =
                    SelectorWaiting(
                        manager,
                        L"VSChannelSelect",
                        vsChannelVtable_);
                break;
            case 3:
                waiting =
                    SelectorWaiting(
                        manager,
                        L"MissionChannelSelect",
                        missionChannelVtable_);
                break;
            case 7:
                waiting =
                    SelectorWaiting(
                        manager,
                        L"ClanChannelSelect",
                        clanChannelVtable_);
                break;
            default:
                waiting = false;
                break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            waiting = false;
        }
        return waiting;
    }

private:
    static constexpr std::uint32_t
        kServiceChannelState = 25;
    static constexpr std::uint32_t
        kChannelLoaded = 2;
    static constexpr std::size_t
        kNativeWStringSize = 0x1C;
    static constexpr std::size_t
        kServiceDescriptorSize =
            sizeof(std::uint32_t) +
            kNativeWStringSize;
    static constexpr std::size_t
        kAppState = 0x4A0;
    static constexpr std::size_t
        kAppOwner = 0x4A4;
    static constexpr std::size_t
        kChannelReady = 0x20BC;
    static constexpr std::size_t
        kChannelLoadState = 0x20C0;
    static constexpr std::size_t
        kChannelMode = 0x2238;
    static constexpr std::size_t
        kServiceActive = 0x04;
    static constexpr std::size_t
        kServiceRetired = 0x2D;
    static constexpr std::size_t
        kServiceRoot = 0x60;
    static constexpr std::size_t
        kMenuExitButton = 0x68;
    static constexpr std::size_t
        kMenuRoomButton = 0x6C;
    static constexpr std::size_t
        kMenuVsButton = 0x70;
    static constexpr std::size_t
        kMenuMissionButton = 0x74;
    static constexpr std::size_t
        kMenuClanButton = 0x78;
    static constexpr std::size_t
        kSelectorPage = 0x1A4;
    static constexpr std::size_t
        kSelectorMaxPage = 0x1A8;
    static constexpr std::size_t
        kSelectorRequestPending = 0x1AC;
    static constexpr std::size_t
        kSelectorTransitioning = 0x1AD;
    static constexpr std::size_t
        kSelectorRowsBegin = 0x1B0;
    static constexpr std::size_t
        kSelectorRowsEnd = 0x1B4;
    static constexpr std::size_t
        kRootInteractive = 0x162;
    static constexpr std::size_t
        kControlEnabled = 0x167;
    static constexpr std::uintptr_t
        kChannelRowSize = 0x28;
    static constexpr std::uintptr_t
        kMaximumChannelRows = 256;

    using ServiceManagerFn =
        void*(__cdecl*)();
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

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    bool MenuWaiting(void* manager) const
    {
        void* service =
            FindNamed(
                manager,
                L"GateChannelMenu");
        if (!ServiceVisible(
                service,
                channelMenuVtable_))
            return false;

        auto* bytes =
            static_cast<std::uint8_t*>(
                service);
        void* exitButton =
            *reinterpret_cast<void**>(
                bytes + kMenuExitButton);
        void* roomButton =
            *reinterpret_cast<void**>(
                bytes + kMenuRoomButton);
        void* vsButton =
            *reinterpret_cast<void**>(
                bytes + kMenuVsButton);
        void* missionButton =
            *reinterpret_cast<void**>(
                bytes + kMenuMissionButton);
        void* clanButton =
            *reinterpret_cast<void**>(
                bytes + kMenuClanButton);
        if (!exitButton || !roomButton ||
            !vsButton || !missionButton ||
            !clanButton)
            return false;

        return ControlEnabled(vsButton) ||
            ControlEnabled(missionButton) ||
            ControlEnabled(clanButton);
    }

    bool SelectorWaiting(
        void* manager,
        const wchar_t* name,
        void* expectedVtable) const
    {
        void* service =
            FindNamed(
                manager,
                name);
        if (!ServiceVisible(
                service,
                expectedVtable))
            return false;

        auto* bytes =
            static_cast<std::uint8_t*>(
                service);
        if (bytes[kSelectorRequestPending] != 0 ||
            bytes[kSelectorTransitioning] != 0)
            return false;

        const std::uint32_t page =
            *reinterpret_cast<std::uint32_t*>(
                bytes + kSelectorPage);
        const std::uint32_t maxPage =
            *reinterpret_cast<std::uint32_t*>(
                bytes + kSelectorMaxPage);
        if (page > maxPage)
            return false;

        const std::uintptr_t begin =
            reinterpret_cast<std::uintptr_t>(
                *reinterpret_cast<void**>(
                    bytes +
                    kSelectorRowsBegin));
        const std::uintptr_t end =
            reinterpret_cast<std::uintptr_t>(
                *reinterpret_cast<void**>(
                    bytes +
                    kSelectorRowsEnd));
        if (!begin || end <= begin)
            return false;

        const std::uintptr_t bytesUsed =
            end - begin;
        if (bytesUsed % kChannelRowSize != 0)
            return false;
        const std::uintptr_t rows =
            bytesUsed / kChannelRowSize;
        return rows > 0 &&
            rows <= kMaximumChannelRows;
    }

    bool ServiceVisible(
        void* service,
        void* expectedVtable) const
    {
        if (!service || !expectedVtable)
            return false;
        auto* bytes =
            static_cast<std::uint8_t*>(
                service);
        if (*reinterpret_cast<void**>(bytes) !=
                expectedVtable ||
            bytes[kServiceActive] == 0 ||
            bytes[kServiceRetired] != 0)
            return false;

        void* root =
            *reinterpret_cast<void**>(
                bytes + kServiceRoot);
        return root &&
            static_cast<std::uint8_t*>(
                root)[kRootInteractive] != 0;
    }

    bool ControlEnabled(void* control) const
    {
        return control &&
            static_cast<std::uint8_t*>(
                control)[kControlEnabled] != 0;
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

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    ServiceManagerFn manager_ = nullptr;
    ServiceFindByNameFn findByName_ = nullptr;
    NativeWStringCtorFn wstringCtor_ = nullptr;
    NativeWStringDtorFn wstringDtor_ = nullptr;
    void* channelOwnerVtable_ = nullptr;
    void* channelMenuVtable_ = nullptr;
    void* vsChannelVtable_ = nullptr;
    void* missionChannelVtable_ = nullptr;
    void* clanChannelVtable_ = nullptr;
    bool channelSymbolsAvailable_ = false;
    bool bound_ = false;
};

} // namespace sdgo::login_recovery
