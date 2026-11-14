#include "sdgo/rgb_paint_native.h"
#include <windows.h>
#include <cstring>
#include <cmath>

namespace sdgo::rgb_paint::native {
static_assert(sizeof(void*) == 4, "GOnline material adapter requires Win32");

bool Renderer::Bind(const SdgoApi* api, SdgoMod* mod) noexcept {
    bindings_ = {};
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol) return false;
    Bindings b;
    b.material = reinterpret_cast<Bindings::Material>(api->Symbol(mod, "rgb_paint.model_material"));
    b.reset = reinterpret_cast<Bindings::Reset>(api->Symbol(mod, "rgb_paint.model_reset_material"));
    b.model_vtable = api->Symbol(mod, "rgb_paint.model_vtable");
    b.observed_model_vtable = api->Symbol(mod, "rgb_paint.observed_model_vtable");
    return Bind(b);
}
bool Renderer::Bind(Bindings bindings) noexcept {
    bindings_ = {};
    if (!bindings.material || !bindings.reset || !bindings.model_vtable) return false;
    bindings_ = bindings; return true;
}
bool Renderer::Apply(void* model, std::size_t region, std::optional<Rgb> color) const noexcept {
    if (!model || region >= kRegions || !bindings_.material || !bindings_.reset) return false;
    __try {
        const void* vtable = nullptr;
        std::memcpy(&vtable, model, sizeof vtable);
        if (!vtable || (vtable != bindings_.model_vtable && vtable != bindings_.observed_model_vtable)) return false;
        const int slot = static_cast<int>(region) + 1;
        auto* material = static_cast<std::uint8_t*>(bindings_.material(model, slot, 0, 0));
        if (!material) return false;
        if (!color) { bindings_.reset(model, slot); return true; }
        auto* rendered = static_cast<std::uint8_t*>(bindings_.material(model, slot, 0, 1));
        if (!rendered) return false;
        const float rgb[] = {color->r / 255.f, color->g / 255.f, color->b / 255.f};
        // GOnline 006E7710: regions 0..4 update diffuse and half-strength
        // ambient; region 5 updates emissive. Alpha, gloss and textures stay
        // owned by the native material. Exact black is distinct from reset.
        // Static previews can draw the existing per-instance material without
        // another native material-update pass. Update that copy as well as
        // the authored instance values, preserving each copy's alpha.
        std::uint8_t* copies[] = {material, rendered};
        for (auto* copy : copies) {
            if (region == 5) std::memcpy(copy + 0x34, rgb, sizeof rgb);
            else {
                const float ambient[] = {rgb[0] * .5f, rgb[1] * .5f, rgb[2] * .5f};
                std::memcpy(copy + 4, rgb, sizeof rgb);
                std::memcpy(copy + 0x14, ambient, sizeof ambient);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Renderer::ReadRendered(void* model,std::size_t region,Rgb& color) const noexcept {
    if(!model||region>=kRegions||!bindings_.material)return false;
    __try {
        const void* table=nullptr;std::memcpy(&table,model,sizeof table);
        if(table!=bindings_.model_vtable&&table!=bindings_.observed_model_vtable)return false;
        const auto* material=static_cast<const std::uint8_t*>(bindings_.material(model,static_cast<int>(region)+1,0,1));
        if(!material)return false;float values[3]{};std::memcpy(values,material+(region==5?0x34:4),sizeof values);
        std::uint8_t channels[3]{};for(unsigned i=0;i<3;++i){if(!std::isfinite(values[i])||values[i]<0||values[i]>1)return false;channels[i]=static_cast<std::uint8_t>(std::lround(values[i]*255));}
        color={channels[0],channels[1],channels[2]};return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
} // namespace sdgo::rgb_paint::native
