// The six-dimension chart of the OC allocation window.
//
// uirs\0794.uir arranges the six stats around a hexagon (sprs\8615.spr):
// attack top left, agility top right, speed left, HP right, special bottom
// left, defense bottom right. Its CustomIcon (95,108 296x264) carries no
// sprite: the OC client drew the unit's stat shape there. The current client
// has no polygon control, so the chart is drawn with the engine's own
// Direct3D 9 device right after the dialog renders (the dialog's render slot
// +0x34, called by the owning service in dialog order), which keeps the
// confirmation and challenge windows, filed after it, above the chart.
#pragma once

#include <array>
#include <cstdint>

namespace sdgo::oc_custom::client::radar {

inline constexpr std::size_t kAxes = 6; // protocol stat order: speed, HP, attack, defense, agility, special

struct Shape {
    bool visible = false;
    std::array<float, kAxes> base{};  // native stat / cap, 0..1
    std::array<float, kAxes> total{}; // with the normal and Over points
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
std::uint32_t Frames() noexcept; // charts drawn
const Shape& Current() noexcept;

} // namespace sdgo::oc_custom::client::radar
