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
// 0794 CustomIcon: 95,108 296x264. The chart is centred on it with half its
// width as the radius, like the OC client's hexagon render.
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
// The OC client's layer colours, bottom to top (see radar.h).
constexpr DWORD kLayerColor[kLayers] = {0x33FFFFFFu, 0xB2FFFF99u, 0x7F339965u, 0xB2339965u};
constexpr float kGrowthStep = 0.1f; // per drawn frame, as the OC client

using RenderFn = void(__thiscall*)(void*, void*);
constexpr std::size_t kRendererDevice = 8; // CRenderer: IDirect3DDevice9* at +8

void* g_table[kMaxSlots + 1] = {};
void** g_original_vtable = nullptr;
RenderFn g_render = nullptr;
void* g_dialog = nullptr;
Shape g_shape;
void* g_information = nullptr;
void* g_information_panel = nullptr;
struct Panel { void* control=nullptr; Rect clip{}; bool chart=false; };
using ControlDraw=void(__thiscall*)(void*);
ControlDraw g_skin_draw=nullptr;
std::array<Panel,32> g_panels{};
std::size_t g_panel_count=0;
std::array<Panel, 11> g_challenge_panels{};
std::size_t g_challenge_panel_count=0;
void* g_challenge_root=nullptr;
void* g_panel_table[37]{};
Shape g_information_shape;
float g_information_x = 750, g_information_y = 220, g_information_radius = 95;
volatile LONG g_information_frames = 0;
float g_growth = 0.0f;
bool g_was_visible = false;
volatile LONG g_frames = 0;
void* const* g_renderer = nullptr;

void* g_model = nullptr; // the factory's CUIMulti3DRender
void* g_page = nullptr;
bool g_after_model = false;
bool g_hide_page = false;
volatile LONG g_after_frames = 0;
volatile LONG g_native_turns = 0; // charts drawn in 0794's turn while after_model was asked
volatile LONG g_page_draws = 0;   // shown native page draws that were not held back

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
    if (!(value == value)) return 0.0f; // NaN
    return std::clamp(value, 0.0f, 1.0f);
}

void DrawLayer(IDirect3DDevice9* device, float cx, float cy, float rx, float ry,
               const std::array<float, kAxes>& values, DWORD color) {
    Vertex fan[kAxes + 2];
    fan[0] = {cx, cy, 0.5f, 1.0f, color};
    for (std::size_t step = 0; step < kAxes; ++step) {
        const std::size_t axis = kAround[step];
        const float angle = kAngle[axis] * 3.14159265f / 180.0f;
        const float value = Clamp(values[axis]);
        fan[step + 1] = {cx + std::cos(angle) * rx * value, cy - std::sin(angle) * ry * value, 0.5f, 1.0f, color};
    }
    fan[kAxes + 1] = fan[1];
    device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, static_cast<UINT>(kAxes), fan, sizeof(Vertex));
}

void Draw(void* dialog, bool information = false) {
    IDirect3DDevice9* device = Device();
    if (!device) return;
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport)) || !viewport.Width || !viewport.Height) return;
    if (g_growth < 1.0f) g_growth = (std::min)(1.0f, g_growth + kGrowthStep);
    const float sx = static_cast<float>(viewport.Width) / kLayoutWidth;
    const float sy = static_cast<float>(viewport.Height) / kLayoutHeight;
    const float cx = static_cast<float>(viewport.X) + (Read<float>(dialog, kPositionX) + (information ? g_information_x : kCenterX)) * sx;
    const float cy = static_cast<float>(viewport.Y) + (Read<float>(dialog, kPositionY) + (information ? g_information_y : kCenterY)) * sy;
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
    const float radius = information ? g_information_radius : kRadius * g_growth;
    const float rx = radius * sx, ry = radius * sy;
    if(information) {
        // The native information chart uses a neutral hexagon with spokes.
        // Keep allocation-page panel artwork out of this read-only card.
        std::array<float,kAxes> extent;extent.fill(1.0f);
        DrawLayer(device,cx,cy,rx,ry,extent,0xFF2B2C31);
        Vertex outline[kAxes+1],spokes[kAxes*2];
        for(std::size_t i=0;i<kAxes;++i) {
            const float angle=kAngle[kAround[i]]*3.14159265f/180.0f;
            const float x=cx+std::cos(angle)*rx,y=cy-std::sin(angle)*ry;
            outline[i]={x,y,0.5f,1,0xFF63646A};
            spokes[2*i]={cx,cy,0.5f,1,0xFF48494F};spokes[2*i+1]={x,y,0.5f,1,0xFF48494F};
            const float dx=1.5f*sx,dy=1.5f*sy;
            Vertex point[4]={{x-dx,y-dy,0.5f,1,0xFFE0E0E0},{x+dx,y-dy,0.5f,1,0xFFE0E0E0},
                             {x-dx,y+dy,0.5f,1,0xFFE0E0E0},{x+dx,y+dy,0.5f,1,0xFFE0E0E0}};
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,point,sizeof(Vertex));
        }
        outline[kAxes]=outline[0];
        device->DrawPrimitiveUP(D3DPT_LINELIST,static_cast<UINT>(kAxes),spokes,sizeof(Vertex));
        device->DrawPrimitiveUP(D3DPT_LINESTRIP,static_cast<UINT>(kAxes),outline,sizeof(Vertex));
    }
    const auto& shape = information ? g_information_shape : g_shape;
    for (std::size_t layer = 0; layer < kLayers; ++layer)
        DrawLayer(device, cx, cy, rx, ry, shape.layer[layer], kLayerColor[layer]);
    saved->Apply();
    saved->Release();
    if (information) InterlockedIncrement(&g_information_frames);
    else InterlockedIncrement(&g_frames);
}

void DrawChart(void* dialog) {
    const bool visible = g_shape.visible && Read<std::uint8_t>(dialog, kVisible);
    if (visible && !g_was_visible) g_growth = 0.0f;
    g_was_visible = visible;
    if (visible) Draw(dialog);
}

// The model control draws this frame (the checks of its own draw: shown,
// sized, with models).
bool ModelDrawn() noexcept {
    __try {
        auto* control = static_cast<std::uint8_t*>(g_model);
        return control && Read<std::uint8_t>(control, 0x162) && Read<std::uint8_t>(control, 0x163) &&
               Read<std::int32_t>(control, 0x154) > 0 && Read<std::int32_t>(control, 0x158) > 0 &&
               Read<void*>(control, 0x21C) != Read<void*>(control, 0x220);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall RenderHook(void* self, void*, void* argument) {
    bool skip = false;
    __try {
        skip = self == g_page && g_hide_page;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (skip) return;
    __try {
        if (self == g_page && g_page && Read<std::uint8_t>(self, kVisible)) InterlockedIncrement(&g_page_draws);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_render(self, argument);
    __try {

        if (self == g_dialog) {
            if (g_after_model && ModelDrawn()) return; // drawn after the model instead
            if (g_after_model && g_shape.visible) InterlockedIncrement(&g_native_turns);
            DrawChart(self);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Gives the dialog the private vtable (one dialog class only).
bool Adopt(void* dialog, const char** failure) noexcept {
    const char* reason = nullptr;
    __try {
        if (!dialog) {
            reason = "no dialog";
        } else {
            void** vtable = Read<void**>(dialog);
            if (vtable == &g_table[1]) return true;
            std::uintptr_t low = 0, high = 0;
            std::size_t slots = 0;
            if (ExecutableRange(&low, &high))
                while (slots < kMaxSlots) {
                    const auto entry = reinterpret_cast<std::uintptr_t>(vtable[slots]);
                    if (entry < low || entry >= high) break;
                    ++slots;
                }
            if (slots <= kRenderSlot) {
                reason = "the window has no dialog vtable";
            } else if (g_original_vtable && g_original_vtable != vtable) {
                reason = "the window is not the expected dialog class";
            } else {
                if (!g_original_vtable) {
                    // Keep the RTTI locator before slot 0 so dynamic casts still work.
                    g_table[0] = vtable[-1];
                    std::memcpy(&g_table[1], vtable, slots * sizeof(void*));
                    g_render = reinterpret_cast<RenderFn>(vtable[kRenderSlot]);
                    g_table[1 + kRenderSlot] = reinterpret_cast<void*>(&RenderHook);
                    g_original_vtable = vtable;
                }
                void** ours = &g_table[1];
                std::memcpy(dialog, &ours, sizeof(ours));
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reason = "fault while adopting the window";
    }
    if (failure) *failure = reason;
    return false;
}

} // namespace


Shape FromProfile(const Profile& profile, const Ranges& ranges) noexcept {
    Shape shape; shape.visible = true;
    for (std::size_t stat = 0; stat < kAxes; ++stat) {
        const auto scale = [&](float value) {
            const auto& r = ranges[stat];
            return r.maximum > r.minimum ? (std::clamp(value, r.minimum, r.maximum) - r.minimum) / (r.maximum - r.minimum) : 0.0f;
        };
        const float base = static_cast<float>(profile.base[stat]);
        const float normal = base + static_cast<float>(profile.normal[stat]) * profile.normal_stat[stat];
        const float over = normal + static_cast<float>(profile.over[stat]) * profile.over_stat[stat];
        shape.layer[3][stat] = scale(base);
        shape.layer[2][stat] = scale(normal);
        shape.layer[1][stat] = shape.layer[0][stat] = scale(over);
    }
    return shape;
}
void __fastcall InformationSkinDraw(void* control, void*) {
    __try {
        const Panel* panel=nullptr;
        for(std::size_t i=0;i<g_panel_count;++i)if(g_panels[i].control==control){panel=&g_panels[i];break;}
        void* root=g_information;
        if(!panel)for(std::size_t i=0;i<g_challenge_panel_count;++i)
            if(g_challenge_panels[i].control==control){panel=&g_challenge_panels[i];root=g_challenge_root;break;}
        if(!root || Read<void*>(control,0x15C)!=root || !Read<std::uint8_t>(root,kVisible) ||
           !Read<std::uint8_t>(control,kVisible) || !Read<std::uint8_t>(control,0x163))return;
        auto* device=Device();D3DVIEWPORT9 viewport{};
        if(!panel || !device || FAILED(device->GetViewport(&viewport)))return;
        const float sx=static_cast<float>(viewport.Width)/kLayoutWidth,sy=static_cast<float>(viewport.Height)/kLayoutHeight;
        const float x=Read<float>(root,kPositionX)+panel->clip.x;
        const float y=Read<float>(root,kPositionY)+panel->clip.y;
        RECT clip={static_cast<LONG>(viewport.X+x*sx),static_cast<LONG>(viewport.Y+y*sy),
                   static_cast<LONG>(viewport.X+(x+panel->clip.width)*sx),static_cast<LONG>(viewport.Y+(y+panel->clip.height)*sy)};
        clip.left=(std::max)(clip.left,static_cast<LONG>(viewport.X));clip.top=(std::max)(clip.top,static_cast<LONG>(viewport.Y));
        clip.right=(std::min)(clip.right,static_cast<LONG>(viewport.X+viewport.Width));clip.bottom=(std::min)(clip.bottom,static_cast<LONG>(viewport.Y+viewport.Height));
        if(clip.right<=clip.left || clip.bottom<=clip.top)return;
        IDirect3DStateBlock9* saved=nullptr;
        if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&saved)) || !saved)return;
        device->SetScissorRect(&clip);device->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE);
        g_skin_draw(control);
        saved->Apply();saved->Release();
        if(panel->chart && g_information_shape.visible)Draw(g_information,true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool InstallInformationSkin(void* control, const Rect& clip, bool chart) noexcept {
    if(!control || g_panel_count==g_panels.size())return false;
    auto** table=Read<void**>(control);
    if(!g_panel_table[1]) {
        g_panel_table[0]=table[-1];
        std::memcpy(g_panel_table+1,table,36*sizeof(void*));
        g_skin_draw=reinterpret_cast<ControlDraw>(table[8]);
        g_panel_table[1+8]=reinterpret_cast<void*>(&InformationSkinDraw);
    }
    void** ours=g_panel_table+1;std::memcpy(control,&ours,sizeof(ours));
    g_panels[g_panel_count++]={control,clip,chart};
    if(chart)g_information_panel=control;
    return true;
}
bool InstallChallengeSkin(void* control, void* root, const Rect& clip) noexcept {
    if(!control || !root || g_challenge_panel_count==g_challenge_panels.size())return false;
    auto** table=Read<void**>(control);
    if(!g_panel_table[1]) {
        g_panel_table[0]=table[-1];
        std::memcpy(g_panel_table+1,table,36*sizeof(void*));
        g_skin_draw=reinterpret_cast<ControlDraw>(table[8]);
        g_panel_table[1+8]=reinterpret_cast<void*>(&InformationSkinDraw);
    }
    void** ours=g_panel_table+1;std::memcpy(control,&ours,sizeof(ours));
    g_challenge_root=root;
    g_challenge_panels[g_challenge_panel_count++]={control,clip,false};
    return true;
}
void ClearChallengeSkins() noexcept {g_challenge_root=nullptr;g_challenge_panel_count=0;}
bool InstallInformation(void* dialog) noexcept {
    if (!g_renderer || !dialog) return false;
    g_information = dialog;
    return true;
}
void SetInformation(const Shape& shape, float x, float y, float radius) noexcept {
    g_information_shape = shape; g_information_x = x; g_information_y = y; g_information_radius = radius;
}
void ForgetInformation(void* dialog) noexcept {
    if (dialog == g_information) { g_information = nullptr; g_information_panel = nullptr; g_information_shape = {}; g_panels={}; g_panel_count=0; }
}
const Shape& CurrentInformation() noexcept { return g_information_shape; }
std::uint32_t InformationFrames() noexcept { return static_cast<std::uint32_t>(g_information_frames); }

bool Install(void* dialog, const char** failure) noexcept {
    if (!g_renderer) {
        if (failure) *failure = "the renderer symbol is missing";
        return false;
    }
    if (!Adopt(dialog, failure)) return false;
    if (g_dialog != dialog) g_was_visible = false;
    g_dialog = dialog;
    return true;
}

void SetRenderer(void* const* renderer_global) noexcept { g_renderer = renderer_global; }

void Forget() noexcept {
    g_dialog = nullptr;
    g_shape.visible = false;
    g_page = nullptr;
    g_after_model = false;
    g_hide_page = false;
    g_was_visible = false;
}

void Set(const Shape& shape) noexcept { g_shape = shape; }

void Restart() noexcept { g_growth = 0.0f; }

std::uint32_t Frames() noexcept { return static_cast<std::uint32_t>(g_frames); }

const Shape& Current() noexcept { return g_shape; }

bool SetReplacedPage(void* dialog) noexcept {
    if (dialog && !Adopt(dialog, nullptr)) return false;
    g_page = dialog;
    return true;
}

void SetModelControl(void* control) noexcept { g_model = control; }

void SetDrawOrder(bool after_model, bool hide_page) noexcept {
    g_after_model = after_model;
    g_hide_page = hide_page;
}

void AfterModelDraw(void* control) noexcept {
    __try {
        if (!g_after_model || !control || control != g_model || !g_dialog) return;
        if (!Read<std::uint8_t>(g_dialog, kVisible)) return;
        DrawChart(g_dialog);
        InterlockedIncrement(&g_after_frames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::uint32_t AfterModelFrames() noexcept { return static_cast<std::uint32_t>(g_after_frames); }

std::uint32_t NativeTurnFrames() noexcept { return static_cast<std::uint32_t>(g_native_turns); }

std::uint32_t PageDraws() noexcept { return static_cast<std::uint32_t>(g_page_draws); }

bool ModelControl(void* control) noexcept { return control && control == g_model; }

} // namespace sdgo::oc_custom::client::radar
