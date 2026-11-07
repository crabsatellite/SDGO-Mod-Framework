#include "radar.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sdgo::oc_custom::client::radar {
namespace {

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(pointer) + offset, sizeof(value));
    return value;
}

// Dialog layout (the native UI's CDialog): position +0x148/+0x14C, visible +0x162.
constexpr std::size_t kPositionX = 0x148;
constexpr std::size_t kPositionY = 0x14C;
constexpr std::size_t kVisible = 0x162;
// The service renders each dialog of its map through vtable +0x34 (one argument).
constexpr std::size_t kRenderSlot = 0x34 / sizeof(void*);
constexpr std::size_t kMaxSlots = 160;
// 0794 CustomIcon: 95,108 296x264, the hexagon of sprs\8615.spr around its centre.
constexpr float kCenterX = 95.0f + 148.0f;
constexpr float kCenterY = 108.0f + 132.0f;
constexpr float kRadius = 148.0f;
// The UI is laid out for 1024x768 and scaled to the viewport.
constexpr float kLayoutWidth = 1024.0f;
constexpr float kLayoutHeight = 768.0f;
// Hexagon vertex of each protocol stat, degrees counter-clockwise from the right:
// speed left, HP right, attack top left, defense bottom right, agility top
// right, special bottom left (the 0794 label positions).
constexpr float kAngle[kAxes] = {180.0f, 0.0f, 120.0f, 300.0f, 60.0f, 240.0f};
// The axes in angle order (0, 60, ... 300 degrees) for the fan.
constexpr std::size_t kAround[kAxes] = {1, 4, 2, 0, 5, 3};
constexpr float kFloor = 0.06f; // keeps a zero stat visible as a dent, not a hole

using RenderFn = void(__thiscall*)(void*, void*);
constexpr std::size_t kRendererDevice = 8; // CRenderer: IDirect3DDevice9* at +8

void* g_table[kMaxSlots + 1] = {};
void** g_original_vtable = nullptr;
RenderFn g_render = nullptr;
void* g_dialog = nullptr;
Shape g_shape;
volatile LONG g_frames = 0;
void* const* g_renderer = nullptr;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
};

bool ExecutableRange(std::uintptr_t* low, std::uintptr_t* high) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        *low = reinterpret_cast<std::uintptr_t>(base) + section->VirtualAddress;
        *high = *low + section->Misc.VirtualSize;
        return true;
    }
    return false;
}

IDirect3DDevice9* Device() {
    if (!g_renderer || !*g_renderer) return nullptr;
    return Read<IDirect3DDevice9*>(*g_renderer, kRendererDevice);
}

float Clamp(float value) {
    if (!(value == value)) return kFloor; // NaN
    return std::clamp(value, kFloor, 1.0f);
}

void DrawShape(IDirect3DDevice9* device, float cx, float cy, float rx, float ry,
               const std::array<float, kAxes>& values, DWORD fill, DWORD line) {
    Vertex fan[kAxes + 2];
    fan[0] = {cx, cy, 0.0f, 1.0f, fill};
    for (std::size_t step = 0; step < kAxes; ++step) {
        const std::size_t axis = kAround[step];
        const float angle = kAngle[axis] * 3.14159265f / 180.0f;
        const float value = Clamp(values[axis]);
        fan[step + 1] = {cx + std::cos(angle) * rx * value, cy - std::sin(angle) * ry * value, 0.0f, 1.0f, fill};
    }
    fan[kAxes + 1] = fan[1];
    device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, static_cast<UINT>(kAxes), fan, sizeof(Vertex));
    Vertex outline[kAxes + 1];
    for (std::size_t step = 0; step <= kAxes; ++step) {
        outline[step] = fan[(step % kAxes) + 1];
        outline[step].color = line;
    }
    device->DrawPrimitiveUP(D3DPT_LINESTRIP, static_cast<UINT>(kAxes), outline, sizeof(Vertex));
}

void Draw(void* dialog) {
    IDirect3DDevice9* device = Device();
    if (!device) return;
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport)) || !viewport.Width || !viewport.Height) return;
    const float sx = static_cast<float>(viewport.Width) / kLayoutWidth;
    const float sy = static_cast<float>(viewport.Height) / kLayoutHeight;
    const float cx = static_cast<float>(viewport.X) + (Read<float>(dialog, kPositionX) + kCenterX) * sx;
    const float cy = static_cast<float>(viewport.Y) + (Read<float>(dialog, kPositionY) + kCenterY) * sy;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
    device->SetPixelShader(nullptr);
    device->SetVertexShader(nullptr);
    device->SetTexture(0, nullptr);
    device->SetTexture(1, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    // The unit's own stats, then the shape with its OC points over it.
    DrawShape(device, cx, cy, kRadius * sx, kRadius * sy, g_shape.base, 0x5A78C8FFu, 0xA08CBEE6u);
    DrawShape(device, cx, cy, kRadius * sx, kRadius * sy, g_shape.total, 0x7050E6FFu, 0xF0C8F5FFu);
    saved->Apply();
    saved->Release();
    InterlockedIncrement(&g_frames);
}

void __fastcall RenderHook(void* self, void*, void* argument) {
    g_render(self, argument);
    __try {
        if (self == g_dialog && g_shape.visible && Read<std::uint8_t>(self, kVisible)) Draw(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

bool Install(void* dialog, const char** failure) noexcept {
    const char* reason = nullptr;
    __try {
        if (!dialog) {
            reason = "no allocation window";
        } else if (!g_renderer) {
            reason = "the renderer symbol is missing";
        } else {
            void** vtable = Read<void**>(dialog);
            if (vtable == &g_table[1]) {
                g_dialog = dialog;
                return true;
            }
            std::uintptr_t low = 0, high = 0;
            std::size_t slots = 0;
            if (ExecutableRange(&low, &high))
                while (slots < kMaxSlots) {
                    const auto entry = reinterpret_cast<std::uintptr_t>(vtable[slots]);
                    if (entry < low || entry >= high) break;
                    ++slots;
                }
            if (slots <= kRenderSlot) {
                reason = "the allocation window has no dialog vtable";
            } else if (g_original_vtable && g_original_vtable != vtable) {
                reason = "the allocation window is not the expected dialog class";
            } else {
                // Keep the RTTI locator before slot 0 so dynamic casts still work.
                g_table[0] = vtable[-1];
                std::memcpy(&g_table[1], vtable, slots * sizeof(void*));
                g_render = reinterpret_cast<RenderFn>(vtable[kRenderSlot]);
                g_table[1 + kRenderSlot] = reinterpret_cast<void*>(&RenderHook);
                g_original_vtable = vtable;
                void** ours = &g_table[1];
                std::memcpy(dialog, &ours, sizeof(ours));
                g_dialog = dialog;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = "fault while installing the chart";
    }
    if (failure) *failure = reason;
    return false;
}

void SetRenderer(void* const* renderer_global) noexcept { g_renderer = renderer_global; }

void Forget() noexcept {
    g_dialog = nullptr;
    g_shape.visible = false;
}

void Set(const Shape& shape) noexcept { g_shape = shape; }

std::uint32_t Frames() noexcept { return static_cast<std::uint32_t>(g_frames); }

const Shape& Current() noexcept { return g_shape; }

} // namespace sdgo::oc_custom::client::radar
