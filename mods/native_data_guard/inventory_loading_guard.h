#pragma once
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"

namespace sdgo::native_inventory_guard {
using CatalogOpen = void(__cdecl*)();
using MenuBusiness = void(__thiscall*)(void*, const void*, void*);
using MenuCallback = uint32_t(__stdcall*)(void*, uint32_t, const void*, void*);
using RoomJoinBusiness = void(__thiscall*)(void*, uint32_t);
inline const SdgoApi* api = nullptr;
inline SdgoMod* mod = nullptr;
inline uint8_t** inventoryPointer = nullptr;
inline uint8_t** appPointer = nullptr;
inline uint8_t** serviceManagerPointer = nullptr;
struct Menu {
    void* vtable = nullptr;
    MenuBusiness business = nullptr;
    MenuCallback callback = nullptr;
    uint32_t event = 0;
};
inline Menu bottomMenu{nullptr, nullptr, nullptr, 0x301};
inline Menu channelMenu{nullptr, nullptr, nullptr, 0x101};
inline CatalogOpen originalCatalogOpen = nullptr;
inline RoomJoinBusiness originalRoomJoin = nullptr;
inline void* roomListVtable = nullptr;
inline bool replayingMenu = false;
inline void* frameTailTrampoline = nullptr;
inline volatile LONG deferredCalls = 0, resumedTransitions = 0;
inline volatile LONG deferredCatalog = 0, resumedCatalog = 0, cancelledCatalog = 0;
inline const DWORD defaultMxcsr = 0x1f80;

struct State {
    uint32_t current = UINT32_MAX, target = 0, capacity = 0;
    uint64_t cursor = 0;
    uint8_t ready = 0xff;
    uint8_t* inventory = nullptr;
    void* owner = nullptr;
};
struct Pending {
    void* app = nullptr;
    void* inventory = nullptr;
    uint32_t state = 0;
    size_t controlOffset = 0;
    const Menu* menu = nullptr;
};
struct PendingRoom {
    void* app = nullptr;
    void* inventory = nullptr;
    void* owner = nullptr;
    uint32_t state = 0;
    uint32_t roomId = 0;
};
inline Pending pendingMenu{}, pendingCatalog{};
inline PendingRoom pendingRoom{};
inline bool Read(void* app, State& state) {
    __try {
        if (app) {
            const auto* bytes = static_cast<const uint8_t*>(app);
            std::memcpy(&state.current, bytes + 0x4a0, 4);
            std::memcpy(&state.owner, bytes + 0x4a4, sizeof(state.owner));
            std::memcpy(&state.target, bytes + 0x4b0, 4);
        }
        state.inventory = inventoryPointer ? *inventoryPointer : nullptr;
        if (state.inventory) {
            std::memcpy(&state.capacity, state.inventory + 0x0c, 4);
            std::memcpy(&state.cursor, state.inventory + 0x18, 8);
            state.ready = state.inventory[0x20];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void* CurrentApp() {
    __try { return appPointer ? *appPointer : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
enum class InventoryPhase { OutsideLoginLoad, SlotCount, Paging };
inline InventoryPhase LoginInventoryPhase(const State& state) {
    if ((state.current != 5 && state.current != 6) || !state.inventory ||
        state.ready != 0 || state.capacity > UINT16_MAX)
        return InventoryPhase::OutsideLoginLoad;
    // Both native slot-count handlers allocate capacity immediately before
    // sending the first brief-page request. The cursor is still zero until
    // that page is consumed, even though pagination already owns the stream.
    return state.capacity ? InventoryPhase::Paging : InventoryPhase::SlotCount;
}
inline void LogWaiting(const State& state, const char* action) {
    alignas(16) unsigned char saved[512];
    _fxsave(saved);
    api->Log(mod, SDGO_LOG_INFO,
        "native %s waits for login inventory completion: app=%lu capacity=%lu cursor=%llu",
        action, static_cast<unsigned long>(state.current), static_cast<unsigned long>(state.capacity),
        static_cast<unsigned long long>(state.cursor));
    _fxrstor(saved);
}
inline bool IdentifyControl(const Menu& kind, void* menu, void* control, size_t& offset) {
    __try {
        if (!menu || !control || *static_cast<void**>(menu) != kind.vtable) return false;
        const auto* bytes = static_cast<const uint8_t*>(menu);
        void* root = *reinterpret_cast<void* const*>(bytes + 0x44);
        if (!root || *reinterpret_cast<void**>(static_cast<uint8_t*>(control) + 0x15c) != root) return false;
        // The top menu's channel buttons enter the original channel-list flow.
        // The other buttons perform unrelated actions and keep native timing.
        const bool channel = &kind == &channelMenu;
        if (channel && *reinterpret_cast<const uint32_t*>(bytes + 0x68)) return false;
        const size_t last = channel ? 0x58 : 0x5c;
        for (size_t item = channel ? 0x54 : 0x48; item <= last; item += 4) {
            if (*reinterpret_cast<void* const*>(bytes + item) == control) { offset = item; return true; }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool ResolveControl(const Pending& request, void*& menu, void*& control) {
    __try {
        auto* manager = serviceManagerPointer ? *serviceManagerPointer : nullptr;
        auto* head = manager ? *reinterpret_cast<uint8_t**>(manager + 0x44) : nullptr;
        auto* node = head ? *reinterpret_cast<uint8_t**>(head) : nullptr;
        for (unsigned count = 0; node && node != head && count < 512; ++count) {
            auto* service = *reinterpret_cast<uint8_t**>(node + 8);
            if (service && *reinterpret_cast<void**>(service) == request.menu->vtable) {
                control = *reinterpret_cast<void**>(service + request.controlOffset);
                size_t offset = 0;
                if (IdentifyControl(*request.menu, service, control, offset) && offset == request.controlOffset) {
                    const auto* widget = static_cast<const uint8_t*>(control);
                    if (widget[0x162] && widget[0x167]) { menu = service; return true; }
                }
            }
            node = *reinterpret_cast<uint8_t**>(node);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline constexpr size_t roomListVectorBegin = 0x124;
inline constexpr size_t roomListVectorEnd = 0x128;
inline constexpr size_t roomEntryStride = 0x90;
inline constexpr uint32_t maximumRoomRows = 10;
inline constexpr uint32_t maximumServiceNodes = 512;

inline bool RoomIdAtIndex(void* service, uint32_t index, uint32_t& roomId) {
    roomId = 0;
    __try {
        if (!service || *static_cast<void**>(service) != roomListVtable) return false;
        const auto* bytes = static_cast<const uint8_t*>(service);
        auto* begin = *reinterpret_cast<uint8_t* const*>(bytes + roomListVectorBegin);
        auto* end = *reinterpret_cast<uint8_t* const*>(bytes + roomListVectorEnd);
        if (!begin || !end || reinterpret_cast<uintptr_t>(end) < reinterpret_cast<uintptr_t>(begin)) return false;
        const size_t size = static_cast<size_t>(reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(begin));
        if (!size || size % roomEntryStride) return false;
        const size_t count = size / roomEntryStride;
        if (count > maximumRoomRows || index >= count) return false;
        std::memcpy(&roomId, begin + static_cast<size_t>(index) * roomEntryStride, sizeof(roomId));
        return roomId != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

enum class RoomResolution { Unavailable, Stale, Ready };
inline RoomResolution ResolveRoomJoin(uint32_t roomId, void*& service, uint32_t& index) {
    service = nullptr; index = 0;
    __try {
        auto* manager = serviceManagerPointer ? *serviceManagerPointer : nullptr;
        auto* head = manager ? *reinterpret_cast<uint8_t**>(manager + 0x44) : nullptr;
        auto* node = head ? *reinterpret_cast<uint8_t**>(head) : nullptr;
        if (!head || !node) return RoomResolution::Unavailable;
        unsigned visited = 0;
        while (node != head && visited < maximumServiceNodes) {
            auto* candidate = *reinterpret_cast<uint8_t**>(node + 8);
            if (candidate && *reinterpret_cast<void**>(candidate) == roomListVtable) {
                if (service) return RoomResolution::Stale;
                service = candidate;
            }
            node = *reinterpret_cast<uint8_t**>(node);
            ++visited;
        }
        if (node != head) return RoomResolution::Stale;
        if (!service) return RoomResolution::Unavailable;
        const auto* bytes = static_cast<const uint8_t*>(service);
        auto* begin = *reinterpret_cast<uint8_t* const*>(bytes + roomListVectorBegin);
        auto* end = *reinterpret_cast<uint8_t* const*>(bytes + roomListVectorEnd);
        if (!begin || !end || reinterpret_cast<uintptr_t>(end) < reinterpret_cast<uintptr_t>(begin)) return RoomResolution::Stale;
        const size_t size = static_cast<size_t>(reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(begin));
        if (!size || size % roomEntryStride) return RoomResolution::Stale;
        const size_t count = size / roomEntryStride;
        if (count > maximumRoomRows) return RoomResolution::Stale;
        bool found = false;
        for (size_t item = 0; item < count; ++item) {
            uint32_t candidate = 0;
            std::memcpy(&candidate, begin + item * roomEntryStride, sizeof(candidate));
            if (candidate != roomId) continue;
            if (found) return RoomResolution::Stale;
            found = true; index = static_cast<uint32_t>(item);
        }
        return found ? RoomResolution::Ready : RoomResolution::Stale;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return RoomResolution::Stale; }
}

inline void CancelCatalog() {
    if (pendingCatalog.app) InterlockedIncrement(&cancelledCatalog);
    pendingCatalog = {};
}
inline void GuardNavigation(const Menu& kind, void* menu, const void* payload, void* control) {
    const DWORD error = GetLastError();
    State state; void* app = CurrentApp(); size_t offset = 0;
    // MyRoomRadio can still start the original slot-count bootstrap.
    constexpr size_t myRoomRadio = 0x54;
    const auto phase = Read(app, state) ? LoginInventoryPhase(state) : InventoryPhase::OutsideLoginLoad;
    const bool navigation = IdentifyControl(kind, menu, control, offset);
    const bool bootstrap = &kind == &bottomMenu && offset == myRoomRadio && phase == InventoryPhase::SlotCount;
    if (navigation && phase != InventoryPhase::OutsideLoginLoad && !bootstrap) {
        pendingRoom = {};
        CancelCatalog();
        const bool changed = pendingMenu.app != app || pendingMenu.inventory != state.inventory ||
            pendingMenu.menu != &kind || pendingMenu.controlOffset != offset;
        // Retain the action identity. Rebuild any callback payload from the
        // live control at replay time.
        pendingMenu = {app, state.inventory, state.current, offset, &kind};
        InterlockedIncrement(&deferredCalls);
        if (changed) LogWaiting(state, &kind == &channelMenu ? "channel navigation" : "menu request");
        SetLastError(error); return;
    }
    if (navigation) {
        pendingRoom = {};
        if (replayingMenu) { replayingMenu = false; InterlockedIncrement(&resumedTransitions); }
        pendingMenu = {}; CancelCatalog();
    }
    SetLastError(error); kind.business(menu, payload, control);
}
inline void __fastcall GuardMenuBusiness(void* menu, void*, const void* payload, void* control) {
    GuardNavigation(bottomMenu, menu, payload, control);
}
inline void __fastcall GuardChannelBusiness(void* menu, void*, const void* payload, void* control) {
    GuardNavigation(channelMenu, menu, payload, control);
}
inline void __fastcall GuardRoomJoin(void* service, void*, uint32_t index) {
    const DWORD error = GetLastError();
    State state; void* app = CurrentApp(); uint32_t roomId = 0;
    const auto phase = Read(app, state) ? LoginInventoryPhase(state) : InventoryPhase::OutsideLoginLoad;
    const bool identified = RoomIdAtIndex(service, index, roomId);
    if (identified && state.owner && phase != InventoryPhase::OutsideLoginLoad) {
        pendingMenu = {};
        CancelCatalog();
        const bool changed = pendingRoom.app != app || pendingRoom.inventory != state.inventory ||
            pendingRoom.owner != state.owner || pendingRoom.state != state.current || pendingRoom.roomId != roomId;
        // Keep the stable room identity, never the mutable list index or service pointer.
        pendingRoom = {app, state.inventory, state.owner, state.current, roomId};
        InterlockedIncrement(&deferredCalls);
        if (changed) LogWaiting(state, "room join");
        SetLastError(error); return;
    }
    pendingRoom = {};
    if (identified) { pendingMenu = {}; CancelCatalog(); }
    SetLastError(error); originalRoomJoin(service, index);
}
inline void __cdecl GuardCatalogOpen() {
    const DWORD error = GetLastError();
    State state; void* app = CurrentApp();
    pendingMenu = {}; pendingRoom = {};
    if (Read(app, state) && LoginInventoryPhase(state) != InventoryPhase::OutsideLoginLoad) {
        if (pendingCatalog.app != app || pendingCatalog.inventory != state.inventory || pendingCatalog.state != state.current) {
            CancelCatalog();
            pendingCatalog = {app, state.inventory, state.current};
            InterlockedIncrement(&deferredCatalog);
            LogWaiting(state, "unit catalog request");
        }
        SetLastError(error); return;
    }
    CancelCatalog();
    SetLastError(error); originalCatalogOpen();
}
inline bool SameOwner(const Pending& pending, void* app, const State& state) {
    return app == pending.app && state.inventory == pending.inventory &&
        state.current == pending.state && state.target == 0;
}
inline bool SameOwner(const PendingRoom& pending, void* app, const State& state) {
    return app == pending.app && state.inventory == pending.inventory && state.owner == pending.owner &&
        state.current == pending.state && state.target == 0;
}
inline void __stdcall ResumeRequests() {
    if (!pendingMenu.app && !pendingCatalog.app && !pendingRoom.app) return;
    const DWORD error = GetLastError();
    void* app = CurrentApp(); State state;
    if (!Read(app, state)) { pendingMenu = {}; pendingRoom = {}; CancelCatalog(); SetLastError(error); return; }
    if (pendingMenu.app) {
        if (!SameOwner(pendingMenu, app, state)) pendingMenu = {};
        else if (state.ready == 1) {
            void* control = nullptr; void* menu = nullptr;
            if (ResolveControl(pendingMenu, menu, control)) {
                // Reuse the original callback's parent and starter-busy checks.
                // If it declines the action, the business hook is not reached
                // and the intent remains pending for a later native frame.
                const Menu& kind = *pendingMenu.menu;
                const void* payload = &kind == &channelMenu ? static_cast<uint8_t*>(control) + 0x118 : nullptr;
                replayingMenu = true;
                kind.callback(menu, kind.event, payload, control);
                replayingMenu = false;
            }
        }
    } else if (pendingCatalog.app) {
        if (!SameOwner(pendingCatalog, app, state)) CancelCatalog();
        else if (state.ready == 1) {
            pendingCatalog = {};
            InterlockedIncrement(&resumedCatalog);
            originalCatalogOpen();
        }
    } else if (pendingRoom.app) {
        if (!SameOwner(pendingRoom, app, state)) pendingRoom = {};
        else if (state.ready == 1) {
            void* service = nullptr; uint32_t index = 0;
            const auto resolution = ResolveRoomJoin(pendingRoom.roomId, service, index);
            if (resolution == RoomResolution::Stale) pendingRoom = {};
            else if (resolution == RoomResolution::Ready) {
                pendingRoom = {};
                InterlockedIncrement(&resumedTransitions);
                SetLastError(error);
                originalRoomJoin(service, index);
            }
        }
    }
    SetLastError(error);
}
// Every normal frame joins this epilogue after native update and validation.
// Jump through its trampoline: POP/MOV/POP/RET still owns the native frame.
__declspec(naked) inline void FrameTailSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0FFFFFFF0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [defaultMxcsr] }
    __asm { cld }
    __asm { call ResumeRequests }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [frameTailTrampoline] }
}
inline bool Attach(const SdgoApi* ownerApi, SdgoMod* ownerMod) {
    api = ownerApi; mod = ownerMod;
    inventoryPointer = static_cast<uint8_t**>(api->Symbol(mod, "native_data_guard.inventory_pointer"));
    appPointer = static_cast<uint8_t**>(api->Symbol(mod, "native_data_guard.app_pointer"));
    serviceManagerPointer = static_cast<uint8_t**>(api->Symbol(mod, "native_data_guard.service_manager_pointer"));
    bottomMenu.vtable = api->Symbol(mod, "native_data_guard.menu_vtable");
    bottomMenu.callback = reinterpret_cast<MenuCallback>(api->Symbol(mod, "native_data_guard.menu_callback"));
    channelMenu.vtable = api->Symbol(mod, "flow.lobby_ui_vtable");
    channelMenu.callback = reinterpret_cast<MenuCallback>(api->Symbol(mod, "flow.lobby_ui_callback"));
    roomListVtable = api->Symbol(mod, "flow.native_room_list_vtable");
    return inventoryPointer && appPointer && serviceManagerPointer && bottomMenu.vtable && bottomMenu.callback &&
        channelMenu.vtable && channelMenu.callback && roomListVtable &&
        api->HookSite(mod, "native_data_guard.frame_tail", reinterpret_cast<void*>(&FrameTailSite), &frameTailTrampoline) &&
        api->HookCode(mod, "native_data_guard.menu_business", reinterpret_cast<void*>(&GuardMenuBusiness),
            reinterpret_cast<void**>(&bottomMenu.business)) &&
        api->HookCode(mod, "native_data_guard.channel_business", reinterpret_cast<void*>(&GuardChannelBusiness),
            reinterpret_cast<void**>(&channelMenu.business)) &&
        api->HookCode(mod, "flow.room_join_business", reinterpret_cast<void*>(&GuardRoomJoin),
            reinterpret_cast<void**>(&originalRoomJoin)) &&
        api->HookCode(mod, "native_data_guard.catalog_open", reinterpret_cast<void*>(&GuardCatalogOpen),
            reinterpret_cast<void**>(&originalCatalogOpen));
}
}
