#include "unit_name_display.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace sdgo::unit_name_display {
namespace {

constexpr std::size_t kPrimaryNameOffset = 5;
constexpr std::uint32_t kPrimaryNameCapacity = 50;

using Display =
    void(__thiscall*)(void*, void*, const std::uint8_t*);

struct Context {
    ContextView view{};
    std::uintptr_t assign_return = 0;
};

Display g_plain_original = nullptr;
Display g_preview_original = nullptr;
Display g_display_original = nullptr;
std::array<std::uintptr_t, 2> g_plain_returns{};
std::array<std::uintptr_t, 4> g_preview_returns{};
std::array<std::uintptr_t, 2> g_display_returns{};
std::uintptr_t g_plain_assign_return = 0;
std::uintptr_t g_preview_assign_return = 0;
std::uintptr_t g_display_assign_return = 0;
thread_local Context g_context;

template <std::size_t Count>
bool IsCaller(const std::array<std::uintptr_t, Count>& admitted,
              const void* caller) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    for (const auto candidate : admitted) {
        if (address == candidate) return true;
    }
    return false;
}

bool ReadContext(const std::uint8_t* record, ContextView* output) noexcept {
    if (!record || !output) return false;
    ContextView value{};
    __try {
        std::memcpy(&value.unit_id, record, sizeof(value.unit_id));
        if (!value.unit_id) return false;
        for (std::uint32_t index = 0; index < kPrimaryNameCapacity;
             ++index) {
            std::uint16_t code_unit = 0;
            std::memcpy(&code_unit,
                        record + kPrimaryNameOffset +
                            index * sizeof(code_unit),
                        sizeof(code_unit));
            value.primary_name[index] =
                static_cast<wchar_t>(code_unit);
            if (!code_unit) {
                value.primary_chars = index;
                *output = value;
                return true;
            }
        }
        // A non-terminated primary field is not safe for fallback use. The
        // stable identity remains useful for exact ID-aware translation.
        value.primary_name[0] = L'\0';
        value.primary_chars = 0;
        *output = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <std::size_t Count>
void Invoke(Display original, void* manager, void* control,
            const std::uint8_t* record, const void* caller,
            const std::array<std::uintptr_t, Count>& admitted,
            std::uintptr_t assign_return) {
    const Context previous = g_context;
    Context active{};
    if (IsCaller(admitted, caller) &&
        ReadContext(record, &active.view)) {
        active.assign_return = assign_return;
    }
    // A nested invocation receives only its own verified context. Clearing the
    // TLS value for an unverified nested caller prevents it from borrowing an
    // outer helper's identical internal assignment return.
    g_context = active;
    __try {
        original(manager, control, record);
    } __finally {
        g_context = previous;
    }
}

void __fastcall PlainName(void* manager, void*, void* control,
                          const std::uint8_t* record) {
    Invoke(g_plain_original, manager, control, record, _ReturnAddress(),
           g_plain_returns, g_plain_assign_return);
}

void __fastcall PreviewName(void* manager, void*, void* control,
                            const std::uint8_t* record) {
    Invoke(g_preview_original, manager, control, record, _ReturnAddress(),
           g_preview_returns, g_preview_assign_return);
}

void __fastcall DisplayName(void* manager, void*, void* control,
                            const std::uint8_t* record) {
    Invoke(g_display_original, manager, control, record, _ReturnAddress(),
           g_display_returns, g_display_assign_return);
}

bool ReturnAddress(const SdgoApi* api, SdgoMod* mod, const char* symbol,
                   std::uintptr_t* output) {
    if (!output) return false;
    const auto* call =
        static_cast<const std::uint8_t*>(api->Symbol(mod, symbol));
    if (!call) return false;
    *output = reinterpret_cast<std::uintptr_t>(call + 5);
    return true;
}

} // namespace

bool CurrentUnitNameContext(const void* assign_return,
                            ContextView* output) noexcept {
    if (!assign_return || !output || !g_context.view.unit_id ||
        reinterpret_cast<std::uintptr_t>(assign_return) !=
            g_context.assign_return) {
        return false;
    }
    __try {
        *output = g_context.view;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Attach(const SdgoApi* api, SdgoMod* mod) {
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol ||
        !api->HookCode) {
        return false;
    }

    if (!ReturnAddress(api, mod, "shop_localization.unit_name_display.call.0083A5A4",
                       &g_plain_returns[0]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.00970764",
                       &g_plain_returns[1]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.00812654",
                       &g_preview_returns[0]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.0089751A",
                       &g_preview_returns[1]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.008C051C",
                       &g_preview_returns[2]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.009CA9D4",
                       &g_preview_returns[3]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.005F6BFD",
                       &g_display_returns[0]) ||
        !ReturnAddress(api, mod, "shop_localization.unit_name_display.call.00620E58",
                       &g_display_returns[1]) ||
        !ReturnAddress(api, mod,
                       "shop_localization.unit_name_display.assign_call.006EF1F4",
                       &g_plain_assign_return) ||
        !ReturnAddress(api, mod,
                       "shop_localization.unit_name_display.assign_call.006EF31D",
                       &g_display_assign_return) ||
        !ReturnAddress(api, mod,
                       "shop_localization.unit_name_display.assign_call.006EF404",
                       &g_preview_assign_return)) {
        return false;
    }

    if (!api->HookCode(mod, "shop_localization.unit_name_display.unit_name_5",
                       reinterpret_cast<void*>(&PlainName),
                       reinterpret_cast<void**>(&g_plain_original)) ||
        !api->HookCode(mod, "shop_localization.unit_name_display.unit_name_105",
                       reinterpret_cast<void*>(&PreviewName),
                       reinterpret_cast<void**>(&g_preview_original)) ||
        !api->HookCode(mod, "shop_localization.unit_name_display.unit_name_205",
                       reinterpret_cast<void*>(&DisplayName),
                       reinterpret_cast<void**>(&g_display_original))) {
        return false;
    }
    return true;
}

} // namespace sdgo::unit_name_display
