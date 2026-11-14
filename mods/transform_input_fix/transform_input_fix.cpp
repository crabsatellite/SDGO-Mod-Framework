// Apsalus III can be owned in either form. Starting with 24788 makes the
// stock early swap request missing 1957.mod/change_ma (and the reverse edge
// requests its missing change_ms). ChangeAni returns zero while the model
// can continue publishing its previous nonzero animation handle. The stock
// TransformUpdate then waits forever, locking movement and attack.
//
// Reuse TransformUpdate's existing empty-event WAIT path for this exact
// missing-handle case. Normal TransformRelease still commits the latched
// target, resets the transform mode, and owns all model/weapon cleanup.
// Source: GOnline 54BE1568, 0x006D9AD0 / 0x006D8E40 / 0x006798F0.
// No timer, invented animation, model rewrite or inventory/database change.
#include <windows.h>
#include <cstdint>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_transform_recovery.h"

namespace {
using UpdateFn = void(__thiscall*)(uint8_t*, void*);
using SignalFn = uint8_t(__thiscall*)(uint8_t*, void*);
using SelectFn = void(__thiscall*)(uint8_t*, unsigned);
UpdateFn originalUpdate = nullptr;
SignalFn signalState = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;

template<class T> T Read(const uint8_t* p, size_t offset) {
    return *reinterpret_cast<const T*>(p + offset);
}
bool MissingTransformAnimation(uint8_t* move, void* context) {
    if (!move || !context || Read<unsigned>(move, 0x2c0) != 3 ||
        Read<unsigned>(move, 0x1ac) != 0) return false;
    auto* events = Read<const uint8_t*>(static_cast<const uint8_t*>(context), 8);
    if (!events || Read<unsigned>(events, 4) == 0) return false;
    // The native empty-list path has already queued WAIT in that case.
    if (Read<const uint8_t*>(move, 8) == move + 0x20) return false;
    auto* unit = Read<const uint8_t*>(move, 0x21c);
    if (!unit) return false;
    auto* current = Read<const uint8_t*>(unit, 0x9c4);
    auto* parked = Read<const uint8_t*>(unit, 0xab0);
    if (!current || !parked) return false;
    const unsigned id = Read<unsigned>(current, 0);
    const unsigned peer = Read<unsigned>(parked, 0);
    const unsigned target = Read<unsigned>(move, 0x2b0);
    if(((id == 24787 && peer == 24788) || (id == 24788 && peer == 24787)) &&
        (target == 24787 || target == 24788))return true;
    // A cached imported model can retain its previous animation events after
    // ChangeAni returns no handle. The catalog independently recognizes only
    // an owned, already committed chain; ordinary stock actors stay unchanged.
    const auto catalog=GetModuleHandleW(L"unit_catalog.dll");
    const auto managed=catalog?reinterpret_cast<SdgoIsImportedTransformRecoveryV1Fn>(
        GetProcAddress(catalog,SDGO_UNIT_TRANSFORM_RECOVERY_EXPORT)):nullptr;
    return managed&&managed(unit)!=0;
}
void __fastcall Update(uint8_t* self, void*, void* context) {
    originalUpdate(self, context);
    const DWORD error = GetLastError();
    if (!MissingTransformAnimation(self, context)) {SetLastError(error);return;}
    // Exactly the native 6D9B22..6D9B39 empty-event sequence and ABI:
    // BE3920(this, &this->Wait); virtual+D8(this, WAIT=1).
    signalState(self, self + 0x20);
    auto select = reinterpret_cast<SelectFn>(Read<void**>(self, 0)[0xd8 / sizeof(void*)]);
    select(self, 1);
    g_api->Log(g_mod, SDGO_LOG_INFO, "null transform animation: queued native WAIT and TransformRelease for a verified supported form");
    SetLastError(error);
}
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookCode) + sizeof(api->HookCode) ||
        !api->Symbol || !api->HookCode || !api->Log) return 0;
    g_api = api;
    g_mod = mod;
    signalState = reinterpret_cast<SignalFn>(api->Symbol(mod, "transform_fix.signal_state"));
    if (!signalState) return 0;
    if (!api->HookCode(mod, "transform_fix.update", reinterpret_cast<void*>(&Update),
                       reinterpret_cast<void**>(&originalUpdate))) return 0;
    api->Log(mod, SDGO_LOG_INFO, "missing-animation transform input recovery active for Apsalus III and verified imported chains");
    return 1;
}
