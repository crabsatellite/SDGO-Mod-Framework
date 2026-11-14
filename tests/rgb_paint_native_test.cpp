#include "sdgo/rgb_paint_native.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
using namespace sdgo::rgb_paint;
namespace {
int checks = 0, resets = 0, requests = 0, lastSlot = 0;
std::array<std::uint8_t, 0x50> material, rendered;
bool available = true;
void Check(bool ok, const char* why) { ++checks; if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
void* __fastcall Material(void*, void*, int slot, int a, int b) {
    Check(slot >= 1 && slot <= 6 && a == 0 && (b == 0 || b == 1), "native material call ABI");
    ++requests; lastSlot = slot; return available ? (b ? rendered.data() : material.data()) : nullptr;
}
void __fastcall Reset(void*, void*, int slot) { Check(slot == lastSlot, "native reset slot"); ++resets; }
float Float(std::size_t offset) { float f; std::memcpy(&f,material.data()+offset,4); return f; }
}
int main() {
    native::Renderer renderer;
    std::uint32_t table = 0, other = 0;
    const void* model = &table;
    native::Bindings b{reinterpret_cast<native::Bindings::Material>(&Material),
        reinterpret_cast<native::Bindings::Reset>(&Reset), &table, nullptr};
    Check(!renderer.Apply(&model,0,Rgb{255,0,72}), "unbound renderer fails closed");
    Check(renderer.Bind(b), "bind explicit fixture");
    for (std::size_t region = 0; region < kRegions; ++region) {
        material.fill(0x5a); const auto before = material;
        rendered.fill(0x3c);const auto renderedBefore=rendered;
        Check(renderer.Apply(&model,region,Rgb{255,0,72}), "apply reference exact RGB");
        Rgb readback;Check(renderer.ReadRendered(&model,region,readback)&&readback==Rgb{255,0,72},"read actual rendered material");
        const std::size_t rgb = region == 5 ? 0x34 : 4;
        Check(Float(rgb) == 1.f && Float(rgb+4) == 0.f && Float(rgb+8) == 72/255.f, "no RGB555 quantization");
        if (region < 5) Check(Float(0x14) == .5f && Float(0x18) == 0 && Float(0x1c) == 36/255.f, "native ambient scaling");
        for (std::size_t i = 0; i < material.size(); ++i) {
            const bool changed = (i >= rgb && i < rgb+12) || (region < 5 && i >= 0x14 && i < 0x20);
            if (!changed) Check(material[i] == before[i], "alpha gloss textures and unrelated fields untouched");
            Check(rendered[i]==(changed?material[i]:renderedBefore[i]),"rendered copy receives RGB but retains its own alpha and other fields");
        }
    }
    Check(renderer.Apply(&model,0,Rgb{0,0,0}) && resets == 0 && Float(4) == 0, "black does not reset material");
    const float invalid=-1.f;std::memcpy(rendered.data()+4,&invalid,4);Rgb invalidColor;Check(!renderer.ReadRendered(&model,0,invalidColor),"invalid material channels rejected");
    Check(renderer.Apply(&model,0,std::nullopt) && resets == 1, "clear uses original reset");
    model = &other;
    const int count = requests;
    Check(!renderer.Apply(&model,0,Rgb{}) && !renderer.Apply(nullptr,0,Rgb{}) && requests == count, "wrong model rejected before native calls");
    model = &table;
    Check(!renderer.Apply(&model,6,Rgb{}) && requests == count, "region bounded");
    available = false;
    Check(!renderer.Apply(&model,0,Rgb{}), "missing material rejected");
    Check(!renderer.ReadRendered(&model,0,invalidColor),"missing readback rejected");
    Check(!renderer.Bind({}) && !renderer.Apply(&model,0,Rgb{}), "failed rebind revokes old bindings");
    std::cout << checks << " native RGB material checks passed\n";
}
