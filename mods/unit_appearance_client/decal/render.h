#pragma once
#include "sdgo/surface_decal_view.h"
#include <d3d9.h>

namespace sdgo::surface_decal::render {
bool Capture(IDirect3DDevice9*,const std::array<float,16>& world,View&);
// Preserve all device state, including the stream binding cleared by UP draws.
bool Draw(IDirect3DDevice9*,IDirect3DBaseTexture9*,const Patch&,const View&);
}
