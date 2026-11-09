// The six-dimension chart of the OC allocation window, and the draw order of
// the OC windows in the factory.
//
// uirs\0794.uir arranges the six stats around a hexagon (sprs\8615.spr):
// attack top left, agility top right, speed left, HP right, special bottom
// left, defense bottom right. Its CustomIcon (95,108 296x264) carries no
// sprite: the OC client drew the unit's stat shape there with its hexagon
// render control (CUnitInfoHexagonRender in the dual-system client), which the
// current client no longer has. The chart is drawn the same way with the
// engine's own Direct3D 9 device: four filled hexagons around the CustomIcon
// centre, radius half its width, growing from the centre when the chart opens;
// bottom to top
//   with the pending points   0x33FFFFFF (this client allocates point by point,
//                             so the layer equals the next one)
//   with the normal and Over  0xB2FFFF99 (only the Over gain shows: yellow)
//   with the normal points    0x7F339965
//   the unit's own stats      0xB2339965
// each stat scaled by its range in the client's stat graph table (grid type 7,
// ids 27501..27506: stat index at +5, minimum +6, maximum +10).
//
// The factory's unit model (uirs\0803.uir Multi3DRender, 0,80 512x608) is a
// CUIMulti3DRender control: the UI draws it after the dialogs' own 2D turn and
// before the batched sprites, so a chart drawn in 0794's turn ended up under
// the model. While the OC tab shows and the model is on screen, the chart is
// drawn right after that control's draw instead: above the model, below the
// windows' sprites and the confirmation and challenge windows. The native page
// whose slot the OC tab takes is not drawn at all from the click on.
#pragma once

#include <array>
#include <cstdint>

namespace sdgo::oc_custom::client::radar {

inline constexpr std::size_t kAxes = 6; // protocol stat order: speed, HP, attack, defense, agility, special
inline constexpr std::size_t kLayers = 4;

struct Shape {
    bool visible = false;
    // Bottom to top: pending, normal + Over, normal, base. 0..1 each.
    std::array<std::array<float, kAxes>, kLayers> layer{};
};

// The engine's renderer global (symbol oc_custom.client_renderer: CRenderer*,
// whose IDirect3DDevice9* is at +8). GOnline.exe links the engine statically.
void SetRenderer(void* const* renderer_global) noexcept;
// Game thread. Gives the 0794 dialog a private copy of its vtable whose render
// slot draws the chart after the original render. Safe to call again for a
// new dialog; returns false (with a reason) when the dialog does not match.
bool Install(void* dialog, const char** failure) noexcept;
void Forget() noexcept; // the dialog went away with its page
void Set(const Shape& shape) noexcept;
void Restart() noexcept; // grow the chart from its centre again (a new unit)
std::uint32_t Frames() noexcept; // charts drawn
const Shape& Current() noexcept;

// Draw order (game thread).
bool SetReplacedPage(void* dialog) noexcept;                  // native page in the OC tab's slot (adopted)
void SetModelControl(void* control) noexcept;                 // the factory's unit model control
void SetDrawOrder(bool after_model, bool hide_page) noexcept; // per frame
void AfterModelDraw(void* control) noexcept;                  // from the CUIMulti3DRender draw hook
std::uint32_t AfterModelFrames() noexcept;                    // charts drawn after the unit model
std::uint32_t NativeTurnFrames() noexcept;                    // charts drawn in 0794's turn while after_model was asked

} // namespace sdgo::oc_custom::client::radar
