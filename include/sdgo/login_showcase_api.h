#pragma once

#include <cstdint>

#define SDGO_LOGIN_SHOWCASE_REGISTER_EXPORT "SdgoRegisterLoginShowcaseChooserV1"

using SdgoLoginShowcaseChooseUnitV1Fn = uint32_t(__cdecl*)();
using SdgoRegisterLoginShowcaseChooserV1Fn =
    int(__cdecl*)(SdgoLoginShowcaseChooseUnitV1Fn chooser);

// Read-only native observation. Pointers are borrowed addresses in the current
// client process; a caller must request a fresh observation in the login scene.
struct SdgoLoginShowcaseObservationV1 {
    uint32_t size,unit_id,original_unit_id,loading_set_id,completed_calls;
    uintptr_t screen,model;
    uint32_t animation_handle;
};
#define SDGO_LOGIN_SHOWCASE_OBSERVE_EXPORT "SdgoLoginShowcaseObserveV1"
using SdgoLoginShowcaseObserveV1Fn = int(__cdecl*)(SdgoLoginShowcaseObservationV1*);
