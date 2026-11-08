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
inline const SdgoApi* api = nullptr;
inline SdgoMod* mod = nullptr;
inline uint8_t** inventoryPointer = nullptr;
inline uint8_t** appPointer = nullptr;
inline uint8_t** serviceManagerPointer = nullptr;
inline void* menuVtable = nullptr;
inline CatalogOpen originalCatalogOpen = nullptr;
inline MenuBusiness originalMenuBusiness = nullptr;
inline MenuCallback nativeMenuCallback = nullptr;
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
};
struct Pending {
    void* app = nullptr;
    void* inventory = nullptr;
    uint32_t state = 0;
    size_t controlOffset = 0;
};
inline Pending pendingMenu{}, pendingCatalog{};
inline bool Read(void* app, State& state) {
    __try {
        if (app) {
            const auto* bytes = static_cast<const uint8_t*>(app);
            std::memcpy(&state.current, bytes + 0x4a0, 4);
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
inline bool LoginInventoryLoading(const State& state) {
    // Capacity zero is the real slot-count-request phase of the same stream.
    // Initial login itself still starts the original inventory owner normally.
    return (state.current == 5 || state.current == 6) && state.inventory &&
        state.ready == 0 && state.capacity <= UINT16_MAX;
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
inline bool IdentifyControl(void* menu, void* control, size_t& offset, void*& root) {
    __try {
        if (!menu || !control || *static_cast<void**>(menu) != menuVtable) return false;
        const auto* bytes = static_cast<const uint8_t*>(menu);
        root = *reinterpret_cast<void* const*>(bytes + 0x44);
        if (!root || *reinterpret_cast<void**>(static_cast<uint8_t*>(control) + 0x15c) != root) return false;
        constexpr size_t offsets[] = {0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c};
        for (size_t item : offsets) {
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
            if (service && *reinterpret_cast<void**>(service) == menuVtable) {
                control = *reinterpret_cast<void**>(service + request.controlOffset);
                size_t offset = 0; void* root = nullptr;
                if (IdentifyControl(service, control, offset, root) && offset == request.controlOffset) {
                    const auto* widget = static_cast<const uint8_t*>(control);
                    if (widget[0x162] && widget[0x167]) { menu = service; return true; }
                }
            }
            node = *reinterpret_cast<uint8_t**>(node);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void CancelCatalog() {
    if (pendingCatalog.app) InterlockedIncrement(&cancelledCatalog);
    pendingCatalog = {};
}
inline void __fastcall GuardMenuBusiness(void* menu, void*, const void* payload, void* control) {
    const DWORD error = GetLastError();
    State state; void* app = CurrentApp(); size_t offset = 0; void* root = nullptr;
    // MyRoomRadio (+54) can be the native bootstrap into the inventory owner.
    // Keep that path available; the existing unit-management guards protect it.
    if (Read(app, state) && LoginInventoryLoading(state) && IdentifyControl(menu, control, offset, root) && offset != 0x54) {
        CancelCatalog();
        const bool changed = pendingMenu.app != app || pendingMenu.inventory != state.inventory || pendingMenu.controlOffset != offset;
        // Retain an action identity, not the dispatcher's transient UI payload.
        // The native business routine never reads its first explicit argument.
        pendingMenu = {app, state.inventory, state.current, offset};
        InterlockedIncrement(&deferredCalls);
        if (changed) LogWaiting(state, "menu request");
        SetLastError(error); return;
    }
    if (replayingMenu) { replayingMenu = false; InterlockedIncrement(&resumedTransitions); }
    pendingMenu = {}; CancelCatalog();
    SetLastError(error); originalMenuBusiness(menu, payload, control);
}
inline void __cdecl GuardCatalogOpen() {
    const DWORD error = GetLastError();
    State state; void* app = CurrentApp();
    pendingMenu = {};
    if (Read(app, state) && LoginInventoryLoading(state)) {
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
inline void __stdcall ResumeRequests() {
    if (!pendingMenu.app && !pendingCatalog.app) return;
    const DWORD error = GetLastError();
    void* app = CurrentApp(); State state;
    if (!Read(app, state)) { pendingMenu = {}; CancelCatalog(); SetLastError(error); return; }
    if (pendingMenu.app) {
        if (!SameOwner(pendingMenu, app, state)) pendingMenu = {};
        else if (state.ready == 1) {
            void* control = nullptr; void* menu = nullptr;
            if (ResolveControl(pendingMenu, menu, control)) {
                // Reuse the original callback's parent and starter-busy checks.
                // If it declines the action, the business hook is not reached
                // and the intent remains pending for a later native frame.
                replayingMenu = true;
                nativeMenuCallback(menu, 0x301, nullptr, control);
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
    menuVtable = api->Symbol(mod, "native_data_guard.menu_vtable");
    nativeMenuCallback = reinterpret_cast<MenuCallback>(api->Symbol(mod, "native_data_guard.menu_callback"));
    return inventoryPointer && appPointer && serviceManagerPointer && menuVtable && nativeMenuCallback &&
        api->HookSite(mod, "native_data_guard.frame_tail", reinterpret_cast<void*>(&FrameTailSite), &frameTailTrampoline) &&
        api->HookCode(mod, "native_data_guard.menu_business", reinterpret_cast<void*>(&GuardMenuBusiness),
            reinterpret_cast<void**>(&originalMenuBusiness)) &&
        api->HookCode(mod, "native_data_guard.catalog_open", reinterpret_cast<void*>(&GuardCatalogOpen),
            reinterpret_cast<void**>(&originalCatalogOpen));
}
}
