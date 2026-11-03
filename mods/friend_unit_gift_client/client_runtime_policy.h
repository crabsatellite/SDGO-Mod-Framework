#pragma once

#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/inventory_app_state.h"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sdgo::friend_unit_gift::client::runtime {

// CUnitInfo::WriteSyncRecord maps full +0x52 (skill part present) to brief
// byte 15. It does not mark a rental. Bytes 16/17/18 are fleet use, lock and
// time-limit/run-out state. A fitted skill part travels with the owned unit.
inline bool NativeBriefAllowsGift(const std::uint8_t* brief) noexcept {
    return brief && !brief[16] && !brief[17] && !brief[18];
}

enum class InventoryEffect : std::uint8_t {
    none,
    remove_local,
    add_local,
};

inline InventoryEffect LocalInventoryEffect(
    Command command,
    WireStatus status) noexcept {
    if (status != WireStatus::success) {
        return InventoryEffect::none;
    }
    switch (command) {
    case Command::offer:
        return InventoryEffect::remove_local;
    case Command::accept:
    case Command::takeback:
        return InventoryEffect::add_local;
    default:
        return InventoryEffect::none;
    }
}

using UnitDetailInit =
    void(__thiscall*)(void*, std::uint32_t);
using UnitDetailInitHook =
    void(__fastcall*)(void*, void*, std::uint32_t);

inline void ForwardUnitDetailInit(
    UnitDetailInit original,
    void* service,
    std::uint32_t argument) {
    original(service, argument);
}

struct AuthenticatedContext {
    std::uint32_t app_state = UINT32_MAX;
    std::uint32_t pilot_id = 0;
};

inline bool ReadAuthenticatedContext(
    const std::uint8_t* app,
    std::uint8_t* const* profile,
    AuthenticatedContext* output = nullptr) noexcept {
    AuthenticatedContext context;
    if (app) {
        std::memcpy(
            &context.app_state,
            app + 0x4A0,
            sizeof(context.app_state));
    }
    if (profile && *profile) {
        std::memcpy(
            &context.pilot_id,
            *profile + 0x138,
            sizeof(context.pilot_id));
    }
    if (output) *output = context;
    return app &&
           sdgo::inventory::IsMenuState(context.app_state) &&
           profile && *profile &&
           context.pilot_id != 0;
}

} // namespace sdgo::friend_unit_gift::client::runtime
