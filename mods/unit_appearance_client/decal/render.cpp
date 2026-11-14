#include "render.h"
#include <cstring>

namespace sdgo::surface_decal::render {
bool Capture(IDirect3DDevice9* device,const std::array<float,16>& world,View& out) {
    if(!device)return false;
    D3DMATRIX camera{},projection{};D3DVIEWPORT9 viewport{};
    if(FAILED(device->GetTransform(D3DTS_VIEW,&camera))||FAILED(device->GetTransform(D3DTS_PROJECTION,&projection))||FAILED(device->GetViewport(&viewport)))return false;
    View v;for(unsigned i=0;i<16;++i){v.world[i]=world[i];v.camera[i]=(&camera.m[0][0])[i];v.projection[i]=(&projection.m[0][0])[i];}
    v.x=viewport.X;v.y=viewport.Y;v.width=viewport.Width;v.height=viewport.Height;out=v;return true;
}
namespace {
struct State {
    IDirect3DStateBlock9* block=nullptr;
    ~State(){if(block){block->Apply();block->Release();}}
};
struct Vertex {float x,y,z,u,v;};
D3DMATRIX Native(const Matrix& m) {D3DMATRIX n{};for(unsigned i=0;i<16;++i)(&n.m[0][0])[i]=static_cast<float>(m[i]);return n;}
}
bool Draw(IDirect3DDevice9* d,IDirect3DBaseTexture9* texture,const Patch& patch,const View& view) {
    if(!d||!texture||patch.vertices.empty()||patch.vertices.size()%3||patch.vertices.size()>8192*3)return false;
    std::vector<Vertex> vertices;vertices.reserve(patch.vertices.size());
    for(const auto& p:patch.vertices)vertices.push_back({static_cast<float>(p.position.x),static_cast<float>(p.position.y),static_cast<float>(p.position.z),static_cast<float>(p.uv.x),static_cast<float>(p.uv.y)});
    State state;if(FAILED(d->CreateStateBlock(D3DSBT_ALL,&state.block))||!state.block)return false;
    bool ok=true;const auto rs=[&](D3DRENDERSTATETYPE t,DWORD value){ok=SUCCEEDED(d->SetRenderState(t,value))&&ok;};
    const auto stage=[&](DWORD s,D3DTEXTURESTAGESTATETYPE t,DWORD v){ok=SUCCEEDED(d->SetTextureStageState(s,t,v))&&ok;};
    const auto world=Native(view.world),camera=Native(view.camera),projection=Native(view.projection);
    ok=SUCCEEDED(d->SetVertexShader(nullptr))&&ok;ok=SUCCEEDED(d->SetPixelShader(nullptr))&&ok;
    ok=SUCCEEDED(d->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1))&&ok;
    ok=SUCCEEDED(d->SetTransform(D3DTS_WORLD,&world))&&ok;ok=SUCCEEDED(d->SetTransform(D3DTS_VIEW,&camera))&&ok;ok=SUCCEEDED(d->SetTransform(D3DTS_PROJECTION,&projection))&&ok;
    rs(D3DRS_LIGHTING,FALSE);rs(D3DRS_CULLMODE,D3DCULL_NONE);
    rs(D3DRS_ZENABLE,D3DZB_TRUE);rs(D3DRS_ZWRITEENABLE,FALSE);rs(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
    rs(D3DRS_ALPHABLENDENABLE,TRUE);rs(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);rs(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);rs(D3DRS_BLENDOP,D3DBLENDOP_ADD);
    rs(D3DRS_ALPHATESTENABLE,TRUE);rs(D3DRS_ALPHAREF,0);rs(D3DRS_ALPHAFUNC,D3DCMP_GREATER);
    rs(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);rs(D3DRS_STENCILENABLE,FALSE);rs(D3DRS_CLIPPLANEENABLE,0);
    rs(D3DRS_COLORWRITEENABLE,15);const float bias=-0.00002f;DWORD bits;std::memcpy(&bits,&bias,4);rs(D3DRS_DEPTHBIAS,bits);rs(D3DRS_SLOPESCALEDEPTHBIAS,0);
    ok=SUCCEEDED(d->SetTexture(0,texture))&&ok;
    stage(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);stage(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    stage(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);stage(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    stage(0,D3DTSS_TEXCOORDINDEX,0);stage(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);stage(1,D3DTSS_COLOROP,D3DTOP_DISABLE);stage(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
    for(auto axis:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})ok=SUCCEEDED(d->SetSamplerState(0,axis,D3DTADDRESS_CLAMP))&&ok;
    ok=SUCCEEDED(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR))&&ok;ok=SUCCEEDED(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR))&&ok;
    return ok&&SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,static_cast<UINT>(vertices.size()/3),vertices.data(),sizeof(Vertex)));
}
}
