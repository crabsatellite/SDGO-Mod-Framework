#include "native_ui.h"

#include "sdgo/sdgo_api.h"

#include <cstdint>

namespace {

using PopulateFn = void(__thiscall*)(void*, int, uint8_t, int8_t);
using ClearFn = void(__thiscall*)(void*, int);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
PopulateFn g_populate = nullptr;
ClearFn g_clear = nullptr;
sdgo::shop_art::ui::Overlay g_overlay;

void __fastcall Populate(void* service, void*, int row, uint8_t state, int8_t limited) {
    g_populate(service, row, state, limited);
    g_overlay.Update(service, row);
}

void __fastcall Clear(void* service, void*, int row) {
    g_clear(service, row);
    g_overlay.Clear(service, row);
}

} // namespace

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol ||
        !api->HookCode || !api->Log || !g_overlay.Bind(api, mod)) return 0;
    g_api = api;
    g_mod = mod;
    if (!api->HookCode(mod, "shop_art_localization.populate",
                       reinterpret_cast<void*>(&Populate),
                       reinterpret_cast<void**>(&g_populate)) ||
        !api->HookCode(mod, "shop_art_localization.clear",
                       reinterpret_cast<void*>(&Clear),
                       reinterpret_cast<void**>(&g_clear))) return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "shop art localization ready: verified sprs/17825 rank label only");
    return 1;
}
