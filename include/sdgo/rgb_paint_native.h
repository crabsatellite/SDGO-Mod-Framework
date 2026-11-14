#pragma once
#include "sdgo/rgb_paint.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::rgb_paint::native {

struct Bindings {
    using Material = void*(__thiscall*)(void*, int, int, int);
    using Reset = void(__thiscall*)(void*, int);
    Material material = nullptr;
    Reset reset = nullptr;
    const void* model_vtable = nullptr;
    const void* observed_model_vtable = nullptr;
};

// Uses the same material slots as the stock paint function. The caller holds
// the original 3D control's render lock, or runs in its model setup callback.
// No model, shared resource, or native pointer is retained across calls.
class Renderer {
public:
    bool Bind(const SdgoApi* api, SdgoMod* mod) noexcept;
    bool Bind(Bindings bindings) noexcept;
    bool Apply(void* model, std::size_t region, std::optional<Rgb> color) const noexcept;
    bool ReadRendered(void* model, std::size_t region, Rgb& color) const noexcept;
private:
    Bindings bindings_{};
};

} // namespace sdgo::rgb_paint::native
