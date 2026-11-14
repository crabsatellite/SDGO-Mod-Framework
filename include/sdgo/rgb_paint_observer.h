#pragma once
#include <cstdint>
// Read-only client observation. No account, connection or native model pointers.
struct SdgoRgbPaintObservationV1 {
    std::uint32_t size=sizeof(SdgoRgbPaintObservationV1),version=1;
    std::uint64_t unit=0;
    std::uint32_t visible=0,editing=0,selected=0,changed=0,frames=0;
    std::uint32_t present_mask=0;
    std::uint8_t rgb[6][3]{};
    std::uint32_t invalid_input=0;
    std::uint32_t authority_ready=0,state=0,reads=0,draws=0,regions=0,matched_material_mask=0;
    std::uint64_t revision=0;
    std::uint32_t saved_mask=0;
    std::uint8_t saved_rgb[6][3]{};
    std::uint32_t battle_draws=0,battle_material_mask=0;
};
#define SDGO_RGB_PAINT_OBSERVE_EXPORT "SdgoRgbPaintObserveV1"
using SdgoRgbPaintObserveV1Fn=int(__cdecl*)(SdgoRgbPaintObservationV1*);

// Only models rendered in the last 250 ms contribute. Older page/battle
// counters cannot be mistaken for the current scene's rendered appearance.
struct SdgoRgbPaintSceneObservationV1 {
    std::uint32_t size=sizeof(SdgoRgbPaintSceneObservationV1),version=1;
    std::uint64_t unit=0,revision=0;
    std::uint32_t room_draws=0,room_mask=0,battle_draws=0,battle_mask=0;
};
#define SDGO_RGB_PAINT_SCENE_EXPORT "SdgoRgbPaintSceneObserveV1"
using SdgoRgbPaintSceneObserveV1Fn=int(__cdecl*)(SdgoRgbPaintSceneObservationV1*);
