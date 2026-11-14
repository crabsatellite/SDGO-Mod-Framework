#include "panel.h"
#include "sync.h"
#include "../../hangar_manager/native_ui.h"
#include "sdgo/rgb_paint_native.h"
#include "sdgo/rgb_paint_observer.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <vector>

namespace {
using namespace sdgo::rgb_paint;
using String=sdgo::hangar::ui::NativeString;
using Bindings=sdgo::hangar::ui::Bindings;
template<class T>T Read(const void* p,unsigned off=0){T value{};std::memcpy(&value,static_cast<const std::uint8_t*>(p)+off,sizeof value);return value;}
template<class T>void Put(void* p,unsigned off,T value){std::memcpy(static_cast<std::uint8_t*>(p)+off,&value,sizeof value);}
String View(const std::wstring& value){String result;result.length=static_cast<unsigned>(value.size());if(value.size()<8)std::memcpy(&result.data,value.c_str(),(value.size()+1)*2);else{result.data.pointer=value.c_str();result.capacity=result.length;}return result;}
const SdgoApi* ownerApi=nullptr;SdgoMod* ownerMod=nullptr;
Bindings uiBindings;
native::Renderer materials;
ui::Panel panel;
using Simple=void(__thiscall*)(void*);
using Destroy=void*(__thiscall*)(void*,unsigned);
using Draw=void(__thiscall*)(void*,void*);
using App=void*(__cdecl*)();
using EndScene=HRESULT(__thiscall*)(void*);
EndScene originalEndScene=nullptr;
Simple originalUpdate=nullptr,lockPreview=nullptr,unlockPreview=nullptr;
Simple originalInventoryUpdate=nullptr,originalInventoryPick=nullptr;
using InventoryEvent=int(__stdcall*)(void*,unsigned,std::uintptr_t,void*);
InventoryEvent originalInventoryEvent=nullptr;
unsigned pointerButtons=0;
void* inventoryTable=nullptr;
void* hiddenInventory=nullptr;
std::uint8_t inventoryVisible=0,inventoryInput=0;
Destroy originalDestroy=nullptr;
Draw originalDraw=nullptr;
App app=nullptr;
void **services=nullptr,**renderer=nullptr,*factoryTable=nullptr,*pageTable=nullptr;
void *page=nullptr,*root=nullptr,*saveButton=nullptr;
HWND window=nullptr;
constexpr UINT_PTR kSubclass=0x52474231;
void* drawTable[162]{};
void** originalTable=nullptr;
unsigned frames=0;
bool active=false,lastEditing=false;
std::uint8_t saveInput=0;
std::uint8_t saveVisible=0;
Record pageRecord;
bool authorityReady=false;
std::optional<Save> saveRequest;
unsigned reconcileAfterReads=0;
std::wstring notice=L"正在读取服务器涂装…";
struct Hidden {void* pointer;std::uint8_t visible,input;};
struct Label {void* pointer;int width;std::wstring text;};
std::vector<Hidden> hidden;
std::vector<Label> labels;
std::uint32_t font=1;

void* Service(void* table){
    if(!services||!*services)return nullptr;
    auto* head=Read<void*>(*services,0x44);auto* link=head?Read<void*>(head):nullptr;
    for(unsigned i=0;link&&link!=head&&i<512;++i){auto* s=Read<void*>(link,8);if(s&&Read<void*>(s)==table)return s;link=Read<void*>(link);}return nullptr;
}
bool Visible(){return page&&root&&Read<void*>(page)==pageTable&&Read<void*>(page,0xd0)==root&&Read<std::uint8_t>(page,0xac)&&Read<std::uint8_t>(root,0x162);}
// The inventory polls native input independently of the window messages and
// its dialog visibility. Keep it idle throughout an RGB-owned gesture, even
// when the mode button has already switched back to native painting.
bool InventoryBlocked(void* self){return active&&Visible()&&(panel.editing||pointerButtons)&&self&&Read<void*>(self)==inventoryTable;}
void __fastcall InventoryUpdateHook(void* self,void*){if(!InventoryBlocked(self))originalInventoryUpdate(self);}
void __fastcall InventoryPickHook(void* self,void*){if(!InventoryBlocked(self))originalInventoryPick(self);}
int __stdcall InventoryEventHook(void* self,unsigned message,std::uintptr_t id,void* control){return InventoryBlocked(self)?0:originalInventoryEvent(self,message,id,control);}
void ResetPointer(){
    const bool owned=pointerButtons!=0;pointerButtons=0;panel.Up();
    if(owned&&window&&GetCapture()==window)ReleaseCapture();
}
ui::Rect ControlRect(void* c){
    float x=Read<float>(c,0x148),y=Read<float>(c,0x14c);
    auto* parent=Read<void*>(c,0x15c);
    for(unsigned i=0;parent&&i<8;++i){x+=Read<float>(parent,0x148);y+=Read<float>(parent,0x14c);parent=Read<void*>(parent,0x15c);}
    return {x,y,static_cast<float>(Read<int>(c,0x154)),static_cast<float>(Read<int>(c,0x158))};
}
Snapshot NativeSnapshot(void* self){
    Record value;sync::ReadNative(static_cast<std::uint8_t*>(self)+0x17c,value);return value.state;
}
void ApplyPreview(const Palette& colors){
    auto* factory=Service(factoryTable);auto* multi=factory?Read<void*>(factory,0xd4):nullptr;
    if(!multi)return;
    auto** first=Read<void**>(multi,0x21c);auto** end=Read<void**>(multi,0x220);
    if(!first||end<first||end-first>8)return;
    for(auto** it=first;it!=end;++it)if(*it){
        // The CRenderModel lock also protects native model replacement.
        lockPreview(*it);auto* model=Read<void*>(*it,0x34);
        sync::BindModel(model,pageRecord);
        for(unsigned i=0;i<6;++i)materials.Apply(model,i,colors[i]);
        unlockPreview(*it);
    }
}
void NativeVisibility(bool show){for(const auto& c:hidden){Put<std::uint8_t>(c.pointer,0x162,show?c.visible:0);Put<std::uint8_t>(c.pointer,0x167,show?c.input:0);}if(saveButton){Put<std::uint8_t>(saveButton,0x167,show?saveInput:0);Put<std::uint8_t>(saveButton,0x162,show?saveVisible:0);}}
void InventoryVisibility(bool show){
    auto* service=Service(inventoryTable);auto* dialog=service?Read<void*>(service,0x108):nullptr;
    if(hiddenInventory&&hiddenInventory!=dialog)hiddenInventory=nullptr;
    if(!show&&dialog&&!hiddenInventory){hiddenInventory=dialog;inventoryVisible=Read<std::uint8_t>(dialog,0x162);inventoryInput=Read<std::uint8_t>(dialog,0x167);}
    if(hiddenInventory){Put<std::uint8_t>(dialog,0x162,show?inventoryVisible:0);Put<std::uint8_t>(dialog,0x167,show?inventoryInput:0);if(show)hiddenInventory=nullptr;}
}
void LabelText(unsigned index,const std::wstring& text){
    if(index>=labels.size()||labels[index].text==text)return;
    auto& label=labels[index];const auto view=View(text);
    uiBindings.text(label.pointer,0,&view,1,3,label.width-2,18,0xffd2f2ff,0xff00101c,0,font,1);
    label.text=text;
}
bool AddLabel(const wchar_t* name,int x,int y,int width){
    const std::wstring key=name;const auto native=View(key);
    const int localX=x-static_cast<int>(Read<float>(root,0x148));
    const int localY=y-static_cast<int>(Read<float>(root,0x14c));
    uiBindings.add(root,&native,localX,localY,width,22,0);
    auto* c=uiBindings.find(root,&native,4);
    if(!c||Read<void*>(c)!=uiBindings.buttonVtable||Read<void*>(c,0x15c)!=root)return false;
    Put<std::uint8_t>(c,0x162,0);Put<std::uint8_t>(c,0x167,0);Put<std::uint8_t>(c,0x168,0);
    labels.push_back({c,width,{}});return true;
}
void Captions(){
    if(labels.size()!=13)return;
    for(unsigned i=0;i<labels.size();++i)Put<std::uint8_t>(labels[i].pointer,0x162,active&&(panel.editing||i==12)?1:0);
    LabelText(0,L"区域 "+std::to_wstring(panel.editor.selected()+1)+L" · RGB 颜色");
    const auto color=panel.picker.color();const unsigned rgb[]={color.r,color.g,color.b};
    for(unsigned i=0;i<3;++i){LabelText(1+i,std::wstring(1,L"RGB"[i]));LabelText(4+i,panel.focus==static_cast<int>(i)?panel.buffer:std::to_wstring(rgb[i]));}
    const auto hex=Hex(color);LabelText(7,panel.focus==3?panel.buffer:std::wstring(hex.begin(),hex.end()));
    LabelText(8,L"清洗当前区域");LabelText(9,L"还原全部");
    LabelText(10,panel.invalid?(panel.focus==3?L"请输入 6 位十六进制颜色":L"R、G、B 请输入 0–255"):notice);
    LabelText(11,panel.editor.state()==State::confirming?L"确认保存":panel.editor.state()==State::saving?L"保存中…":L"保存涂装");LabelText(12,panel.editing?L"原生涂装":L"RGB 取色");
}

struct Vertex {float x,y,z,rhw;DWORD color;};
IDirect3DDevice9* device=nullptr;float scaleX=1,scaleY=1;
DWORD Color(Rgb value){return 0xff000000u|(static_cast<DWORD>(value.r)<<16)|(static_cast<DWORD>(value.g)<<8)|value.b;}
void Quad(ui::Rect r,DWORD tl,DWORD tr,DWORD bl,DWORD br){
    const float x=r.x*scaleX-.5f,y=r.y*scaleY-.5f,w=r.w*scaleX,h=r.h*scaleY;
    const Vertex v[]={{x,y,0,1,tl},{x+w,y,0,1,tr},{x,y+h,0,1,bl},{x+w,y+h,0,1,br}};
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(Vertex));
}
void Fill(ui::Rect r,DWORD c){Quad(r,c,c,c,c);}
void Border(ui::Rect r,DWORD c,float width=1){Fill({r.x,r.y,r.w,width},c);Fill({r.x,r.y+r.h-width,r.w,width},c);Fill({r.x,r.y,width,r.h},c);Fill({r.x+r.w-width,r.y,width,r.h},c);}
void DrawPanel(bool finalColors=false){
    if(!active||!Visible()||!renderer||!*renderer)return;
    device=Read<IDirect3DDevice9*>(*renderer,8);if(!device)return;
    IDirect3DSurface9* target=nullptr;D3DSURFACE_DESC desc{};
    if(FAILED(device->GetRenderTarget(0,&target))||!target)return;
    if(finalColors){IDirect3DSurface9* back=nullptr;const auto result=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);const bool finalTarget=SUCCEEDED(result)&&back==target;if(back)back->Release();if(!finalTarget){target->Release();return;}}
    const auto hr=target->GetDesc(&desc);target->Release();if(FAILED(hr)||!desc.Width||!desc.Height)return;
    IDirect3DStateBlock9* saved=nullptr;if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&saved))||!saved)return;
    IDirect3DVertexBuffer9* stream=nullptr;UINT streamOffset=0,streamStride=0;
    const bool haveStream=SUCCEEDED(device->GetStreamSource(0,&stream,&streamOffset,&streamStride));
    D3DVIEWPORT9 full{0,0,desc.Width,desc.Height,0,1};device->SetViewport(&full);
    scaleX=desc.Width/1024.f;scaleY=desc.Height/768.f;
    device->SetPixelShader(nullptr);device->SetVertexShader(nullptr);device->SetTexture(0,nullptr);device->SetTexture(1,nullptr);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_FOGENABLE,FALSE);device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);device->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE);
    if(panel.editing){
        if(!finalColors){Fill({750,104,274,560},0xff102b3d);Border({758,112,258,544},0xff2589b7);}
        // Narrow strips approximate the bilinear HSV square without a texture
        // or a diagonal seam from one large Gouraud-shaded quad.
        for(unsigned row=0;row<64;++row){
            const double top=1-row/64.,bottom=1-(row+1)/64.;Rgb a{},b{},c{},d{};
            FromHsv({panel.picker.hsv().h,0,top},a);FromHsv({panel.picker.hsv().h,1,top},b);
            FromHsv({panel.picker.hsv().h,0,bottom},c);FromHsv({panel.picker.hsv().h,1,bottom},d);
            auto r=panel.layout.square;r.y+=r.h*row/64;r.h/=64;Quad(r,Color(a),Color(b),Color(c),Color(d));
        }
        for(unsigned i=0;i<6;++i){Rgb a{},b{};FromHsv({i*60.,1,1},a);FromHsv({(i+1)*60.,1,1},b);auto r=panel.layout.hue;r.y+=r.h*i/6;r.h/=6;Quad(r,Color(a),Color(a),Color(b),Color(b));}
        const auto hsv=panel.picker.hsv();
        const float sx=panel.layout.square.x+static_cast<float>(hsv.s)*panel.layout.square.w,sy=panel.layout.square.y+static_cast<float>(1-hsv.v)*panel.layout.square.h;
        Border({sx-4,sy-4,9,9},0xff000000,2);Border({sx-3,sy-3,7,7},0xffffffff);
        const float hy=panel.layout.hue.y+static_cast<float>(hsv.h/360)*panel.layout.hue.h;Border({panel.layout.hue.x-2,hy-2,panel.layout.hue.w+4,5},0xffffffff);
        const auto rgb=panel.picker.color();const unsigned values[]={rgb.r,rgb.g,rgb.b};
        for(unsigned i=0;i<3;++i){
            auto low=rgb,high=rgb;if(i==0){low.r=0;high.r=255;}else if(i==1){low.g=0;high.g=255;}else{low.b=0;high.b=255;}
            Quad(panel.layout.bars[i],Color(low),Color(high),Color(low),Color(high));
            const auto r=panel.layout.bars[i];const float x=r.x+r.w*values[i]/255.f;Fill({x-2,r.y-3,4,r.h+6},0xffffffff);
            if(!finalColors)Fill(panel.layout.fields[i],0xff061521);Border(panel.layout.fields[i],panel.focus==static_cast<int>(i)?(panel.invalid?0xffff6170:0xff6ee7ff):0xff306b86);
        }
        Fill({770,520,32,28},Color(rgb));if(!finalColors)Fill(panel.layout.hex,0xff061521);Border(panel.layout.hex,panel.focus==3?(panel.invalid?0xffff6170:0xff6ee7ff):0xff306b86);
        for(auto r:{panel.layout.clear,panel.layout.restore}){if(!finalColors)Fill(r,0xff163c52);Border(r,0xff3eaad0);}
        if(!finalColors)Fill(panel.layout.save,0xff163c52);Border(panel.layout.save,authorityReady&&panel.editor.dirty()?0xff6ee7ff:0xff306b86);
        for(unsigned i=0;i<6;++i){auto r=panel.layout.regions[i];if(r.w<1)continue;Fill(r,panel.editor.preview()[i]?Color(*panel.editor.preview()[i]):0xff173443);Border(r,i==panel.editor.selected()?0xffffffff:0xff42c6dd,i==panel.editor.selected()?2.f:1.f);}
    }
    if(!finalColors)Fill(panel.layout.toggle,0xff163c52);Border(panel.layout.toggle,0xff3eaad0);
    saved->Apply();saved->Release();if(haveStream)device->SetStreamSource(0,stream,streamOffset,streamStride);if(stream)stream->Release();++frames;
}
HRESULT __fastcall EndSceneHook(void* self,void*){DrawPanel(true);return originalEndScene(self);}
void __fastcall DrawHook(void* self,void*,void* argument){
    if(self!=root||!active){originalDraw(self,argument);return;}
    InventoryVisibility(!panel.editing);
    for(auto& label:labels)Put<std::uint8_t>(label.pointer,0x162,0);
    originalDraw(self,argument);DrawPanel();Captions();
    for(auto& label:labels)if(Read<std::uint8_t>(label.pointer,0x162))Read<Draw>(Read<void*>(label.pointer),13*4)(label.pointer,argument);
}
bool Adopt(){
    auto** table=Read<void**>(root);if(table==&drawTable[1])return true;
    if(originalTable&&table!=originalTable)return false;
    auto* base=static_cast<std::uint8_t*>(ownerApi->host_base);const auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);const auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    const auto* section=IMAGE_FIRST_SECTION(nt);std::uintptr_t lo=0,hi=0;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i,++section)if(section->Characteristics&IMAGE_SCN_MEM_EXECUTE){lo=reinterpret_cast<std::uintptr_t>(base)+section->VirtualAddress;hi=lo+section->Misc.VirtualSize;break;}
    unsigned count=0;while(count<160){const auto address=reinterpret_cast<std::uintptr_t>(table[count]);if(address<lo||address>=hi)break;++count;}if(count<=13)return false;
    drawTable[0]=table[-1];std::memcpy(&drawTable[1],table,count*sizeof(void*));originalDraw=reinterpret_cast<Draw>(table[13]);drawTable[14]=reinterpret_cast<void*>(&DrawHook);originalTable=table;Put<void**>(root,0,&drawTable[1]);return true;
}
void Detach(bool restore){
    ResetPointer();
    InventoryVisibility(true);
    if(restore&&root){NativeVisibility(true);for(auto& label:labels)Put<std::uint8_t>(label.pointer,0x162,0);if(Read<void**>(root)==&drawTable[1])Put<void**>(root,0,originalTable);}
    sync::Preview(0,nullptr);authorityReady=false;saveRequest.reset();panel.Close();hidden.clear();labels.clear();active=lastEditing=false;page=root=saveButton=nullptr;
}
bool Attach(void* self){
    auto* current=Read<void*>(self,0xd0);if(page==self&&root==current)return true;
    // Wait for the native slide-in animation before deriving hit rectangles.
    if(!current||std::fabs(Read<float>(current,0x148))>.5f)return false;
    Detach(false);auto* factory=Service(factoryTable);auto* source=factory?Read<void*>(factory,0xe4):nullptr;
    if(!current||!source||Read<void*>(source)!=uiBindings.buttonVtable)return false;
    page=self;root=current;saveButton=source;saveInput=Read<std::uint8_t>(source,0x167);saveVisible=Read<std::uint8_t>(source,0x162);
    auto** text=Read<std::uint8_t**>(source,0x190);auto** end=Read<std::uint8_t**>(source,0x194);if(text&&end>text&&*text)font=Read<unsigned>(*text,0x6c);
    auto* head=Read<void*>(root,0x23c);auto* link=head?Read<void*>(head):nullptr;
    for(unsigned i=0;link&&link!=head&&i<256;++i){auto* c=Read<void*>(link,8);if(c){const auto name=Read<String>(c,0x118);if(name.length<128){const std::wstring key(name.capacity<8?reinterpret_cast<const wchar_t*>(&name.data):name.data.pointer,name.length);bool slot=false;for(unsigned j=0;j<6;++j){const auto n=std::to_wstring(j);slot=slot||key==L"PaletteBtn"+n||key==L"PaletteIcon"+n||key==L"NoneIcon"+n||key==L"OutLineIcon"+n||key==L"DragIcon"+n||key==L"RGBStatic"+n;}if(slot)hidden.push_back({c,Read<std::uint8_t>(c,0x162),Read<std::uint8_t>(c,0x167)});}}link=Read<void*>(link);}
    for(unsigned i=0;i<6;++i){
        void* icon=nullptr;const auto wanted=L"PaletteIcon"+std::to_wstring(i);auto* node=head?Read<void*>(head):nullptr;
        for(unsigned n=0;node&&node!=head&&n<256;++n){auto* c=Read<void*>(node,8);const auto name=Read<String>(c,0x118);if(name.length<128&&std::wstring(name.capacity<8?reinterpret_cast<const wchar_t*>(&name.data):name.data.pointer,name.length)==wanted){icon=c;break;}node=Read<void*>(node);}
        if(!icon){Detach(false);return false;}auto r=ControlRect(icon);panel.layout.regions[i]={r.x+2,r.y+2,r.w-4,r.h-4};
    }
    bool ok=AddLabel(L"RgbPaintHeading",770,122,234);
    for(unsigned i=0;i<3;++i)ok=AddLabel((L"RgbChannelName"+std::to_wstring(i)).c_str(),770,398+38*i,22)&&ok;
    for(unsigned i=0;i<3;++i)ok=AddLabel((L"RgbChannelValue"+std::to_wstring(i)).c_str(),960,398+38*i,42)&&ok;
    ok=AddLabel(L"RgbHexValue",820,522,180)&&ok;
    ok=AddLabel(L"RgbClearLabel",775,568,106)&&ok;ok=AddLabel(L"RgbRestoreLabel",908,568,94)&&ok;
    ok=AddLabel(L"RgbInputNotice",770,599,236)&&ok;ok=AddLabel(L"RgbSaveNotice",789,633,90)&&ok;
    ok=AddLabel(L"RgbModeLabel",909,633,94)&&ok;
    if(!ok||!Adopt()){Detach(true);return false;}
    ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB picker attached to native paint page");return true;
}
void SyncEditing(){
    if(panel.editing!=lastEditing){
        if(!panel.editing){sync::Preview(0,nullptr);ApplyPreview(panel.editor.saved().colors);}
        else if(page){Record current;if(sync::Current(pageRecord.state.identity.unit,current))panel.Open(current.state);else {panel.Open(NativeSnapshot(page));authorityReady=false;}}
        lastEditing=panel.editing;
    }
    NativeVisibility(!panel.editing);InventoryVisibility(!panel.editing);Captions();
}
void SavedReply(std::uint64_t session,std::uint64_t unit,bool accepted,const Record* record){
    if(!saveRequest||saveRequest->identity!=Identity{session,unit}||panel.editor.saved().identity!=saveRequest->identity)return;
    if(!record||!panel.editor.Reply(*saveRequest,record->state,accepted)){panel.editor.Timeout(*saveRequest);sync::Observation current;reconcileAfterReads=sync::Observe(unit,current)?current.reads:0;sync::Want(unit,true);notice=L"保存待确认，正在重新读取";return;}
    if(!accepted)panel.editor.Restore();
    pageRecord=*record;saveRequest.reset();panel.Select(static_cast<unsigned>(panel.editor.selected()));
    notice=accepted?L"涂装已保存，其他玩家可见":L"版本已变更，请重新调色";
}
void SaveClick(){
    if(!authorityReady){notice=L"正在读取服务器涂装…";return;}
    if(panel.editor.state()==State::saving||panel.editor.state()==State::uncertain)return;
    if(panel.editor.state()!=State::confirming){if(!panel.Commit())return;if(panel.editor.Confirm())notice=L"核对预览，再次点击确认";return;}
    saveRequest=panel.editor.Submit();if(!saveRequest)return;
    auto proposal=pageRecord;proposal.state=panel.editor.saved();proposal.state.colors=saveRequest->colors;
    if(!sync::SaveRecord(proposal,&SavedReply)){panel.editor.Timeout(*saveRequest);sync::Observation current;reconcileAfterReads=sync::Observe(proposal.state.identity.unit,current)?current.reads:0;sync::Want(proposal.state.identity.unit,true);notice=L"通信忙，正在重新确认";}
    else notice=L"正在等待服务器确认…";
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(message==WM_NCDESTROY){ResetPointer();RemoveWindowSubclass(hwnd,&WindowProc,kSubclass);window=nullptr;return DefSubclassProc(hwnd,message,wp,lp);}
    if(message==WM_KILLFOCUS||message==WM_CANCELMODE||(message==WM_CAPTURECHANGED&&pointerButtons)){ResetPointer();panel.focus=-1;}
    if(active&&Visible()){
        bool handled=false;
        unsigned button=0;bool down=false,up=false;
        switch(message){
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:button=1;down=true;break;
        case WM_LBUTTONUP:button=1;up=true;break;
        case WM_RBUTTONDOWN:case WM_RBUTTONDBLCLK:button=2;down=true;break;
        case WM_RBUTTONUP:button=2;up=true;break;
        case WM_MBUTTONDOWN:case WM_MBUTTONDBLCLK:button=4;down=true;break;
        case WM_MBUTTONUP:button=4;up=true;break;
        case WM_XBUTTONDOWN:case WM_XBUTTONDBLCLK:button=GET_XBUTTON_WPARAM(wp)==XBUTTON1?8:16;down=true;break;
        case WM_XBUTTONUP:button=GET_XBUTTON_WPARAM(wp)==XBUTTON1?8:16;up=true;break;
        }
        const bool wheel=message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL;
        if(button||message==WM_MOUSEMOVE||wheel){
            RECT client{};GetClientRect(hwnd,&client);const float w=static_cast<float>(client.right),h=static_cast<float>(client.bottom);
            if(w>0&&h>0){POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(wheel)ScreenToClient(hwnd,&point);
                const float x=point.x*1024.f/w,y=point.y*768.f/h;const bool inside=panel.Contains(x,y);
                if(down){
                    handled=pointerButtons!=0||inside;
                    if(button==1){
                        if(panel.editing&&panel.layout.save.Contains(x,y)){SaveClick();handled=true;}
                        else if(panel.editing&&!authorityReady){notice=L"正在读取服务器涂装…";}
                        else handled=panel.Down(x,y)||handled;
                    }
                    if(handled){pointerButtons|=button;if(GetCapture()!=hwnd)SetCapture(hwnd);}
                }else if(up){
                    handled=(pointerButtons&button)!=0;
                    pointerButtons&=~button;if(button==1&&handled)panel.Up();
                    if(handled&&!pointerButtons&&GetCapture()==hwnd)ReleaseCapture();
                }else if(message==WM_MOUSEMOVE){handled=panel.Move(x,y)||pointerButtons!=0||inside;}
                else handled=pointerButtons!=0||inside;
            }
        }else if(message==WM_CHAR)handled=panel.Character(static_cast<wchar_t>(wp));
        else if(message==WM_KEYDOWN)handled=panel.Key(static_cast<unsigned>(wp));
        if(handled){SyncEditing();return button>=8?TRUE:0;}
    }
    return DefSubclassProc(hwnd,message,wp,lp);
}
void Tick(void* self){
    sync::Pump();
    auto* current=Read<void*>(self,0xd0);
    if(!current||!Read<std::uint8_t>(self,0xac)||!Read<std::uint8_t>(current,0x162)){
        if(active&&page==self){sync::Preview(0,nullptr);ApplyPreview(panel.editor.saved().colors);active=false;NativeVisibility(true);InventoryVisibility(true);Captions();ResetPointer();panel.focus=-1;}
        return;
    }
    if(!Attach(self))return;
    Record nativeRecord;if(!sync::ReadNative(static_cast<std::uint8_t*>(self)+0x17c,nativeRecord))return;
    const auto snapshot=nativeRecord.state;
    if(!snapshot.identity.unit)return;
    if(!active||snapshot.identity!=panel.editor.saved().identity){pageRecord=nativeRecord;panel.Open(snapshot);authorityReady=false;saveRequest.reset();notice=L"正在读取服务器涂装…";active=true;lastEditing=panel.editing;}
    sync::Want(snapshot.identity.unit,true);Record authority;
    if(sync::Current(snapshot.identity.unit,authority)&&authority.type==nativeRecord.type&&authority.native==nativeRecord.native){
        if(!authorityReady){pageRecord=authority;panel.Open(authority.state);authorityReady=true;notice=L"调色后保存 · 全服同步";}
        else if(panel.editor.state()==State::uncertain){sync::Observation observation;if(sync::Observe(snapshot.identity.unit,observation)&&observation.reads>reconcileAfterReads&&panel.editor.Reconcile(authority.state)){pageRecord=authority;saveRequest.reset();panel.Select(static_cast<unsigned>(panel.editor.selected()));notice=L"已读取服务器确认的涂装";}}
        else if(panel.editor.state()==State::editing&&!panel.editor.dirty()&&authority.state.revision>panel.editor.saved().revision){pageRecord=authority;panel.Open(authority.state);notice=L"涂装已同步";}
    }
    if(!window&&app&&app()){const auto hwnd=Read<HWND>(app(),0x3c);DWORD pid=0;const auto thread=GetWindowThreadProcessId(hwnd,&pid);if(pid==GetCurrentProcessId()&&thread==GetCurrentThreadId()&&SetWindowSubclass(hwnd,&WindowProc,kSubclass,0))window=hwnd;}
    NativeVisibility(!panel.editing);InventoryVisibility(!panel.editing);Captions();if(panel.editing){sync::Preview(snapshot.identity.unit,&panel.editor.preview());ApplyPreview(panel.editor.preview());}
}
void __fastcall UpdateHook(void* self,void*){originalUpdate(self);const auto error=GetLastError();Tick(self);SetLastError(error);}
void* __fastcall DestroyHook(void* self,void*,unsigned flags){if(page==self){if(active)ApplyPreview(panel.editor.saved().colors);Detach(true);}return originalDestroy(self,flags);}
}
extern "C" __declspec(dllexport) int __cdecl SdgoRgbPaintObserveV1(SdgoRgbPaintObservationV1* out){
    if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    SdgoRgbPaintObservationV1 value;value.visible=active&&Visible();value.editing=panel.editing;value.unit=panel.editor.saved().identity.unit;
    value.selected=static_cast<unsigned>(panel.editor.selected());value.changed=panel.changes;value.frames=frames;value.invalid_input=panel.invalid;
    value.authority_ready=authorityReady;value.state=static_cast<unsigned>(panel.editor.state());
    sync::Observation current;if(sync::Observe(out->unit?out->unit:value.unit,current)){value.unit=out->unit?out->unit:value.unit;value.revision=current.revision;value.reads=current.reads;value.draws=current.draws;value.regions=current.regions;value.matched_material_mask=current.matched;value.battle_draws=current.battleDraws;value.battle_material_mask=current.battleMatched;for(unsigned i=0;i<6;++i)if(const auto& c=current.colors[i]){value.saved_mask|=1u<<i;value.saved_rgb[i][0]=c->r;value.saved_rgb[i][1]=c->g;value.saved_rgb[i][2]=c->b;}}
    for(unsigned i=0;i<6;++i)if(const auto& c=panel.editor.preview()[i]){value.present_mask|=1u<<i;value.rgb[i][0]=c->r;value.rgb[i][1]=c->g;value.rgb[i][2]=c->b;}
    *out=value;return 1;
}
extern "C" __declspec(dllexport) int __cdecl SdgoRgbPaintSceneObserveV1(SdgoRgbPaintSceneObservationV1* out){
    if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    return sync::ObserveScene(*out)?1:0;
}
int LoadRgb(const SdgoApi* api, SdgoMod* mod) {
    if(!api||!mod||!api->Symbol||!api->HookCode||!api->HookSlot||!api->SettingInt||!api->SettingInt(mod,"RgbEnabled",1))return 0;
    ownerApi=api;ownerMod=mod;if(!materials.Bind(api,mod))return 0;
    if(!sync::Bind(api,mod))return 0;
    uiBindings.add=reinterpret_cast<Bindings::Add>(api->Symbol(mod,"hangar.ui_add_button"));uiBindings.find=reinterpret_cast<Bindings::Find>(api->Symbol(mod,"hangar.ui_find_control"));
    uiBindings.text=reinterpret_cast<Bindings::Text>(api->Symbol(mod,"hangar.ui_text_item"));uiBindings.buttonVtable=api->Symbol(mod,"hangar.ui_button_vtable");
    app=reinterpret_cast<App>(api->Symbol(mod,"game.app"));services=static_cast<void**>(api->Symbol(mod,"flow.service_managed_pointer"));renderer=static_cast<void**>(api->Symbol(mod,"oc_custom.client_renderer"));
    factoryTable=api->Symbol(mod,"rgb_paint.factory_vtable");pageTable=api->Symbol(mod,"rgb_paint.page_vtable");
    inventoryTable=api->Symbol(mod,"rgb_paint.inventory_vtable");
    lockPreview=reinterpret_cast<Simple>(api->Symbol(mod,"rgb_paint.preview_lock"));unlockPreview=reinterpret_cast<Simple>(api->Symbol(mod,"rgb_paint.preview_unlock"));
    if(!uiBindings.add||!uiBindings.find||!uiBindings.text||!uiBindings.buttonVtable||!app||!services||!renderer||!factoryTable||!pageTable||!lockPreview||!unlockPreview)return 0;
    // OC also observes the native update body. Intercept the inventory's virtual
    // dispatch so forwarding still reaches that observer without claiming its code hook.
    return inventoryTable&&api->HookSlot(mod,"rgb_paint.inventory_update",reinterpret_cast<void*>(&InventoryUpdateHook),reinterpret_cast<void**>(&originalInventoryUpdate))&&api->HookCode(mod,"rgb_paint.inventory_event",reinterpret_cast<void*>(&InventoryEventHook),reinterpret_cast<void**>(&originalInventoryEvent))&&api->HookCode(mod,"rgb_paint.inventory_pick",reinterpret_cast<void*>(&InventoryPickHook),reinterpret_cast<void**>(&originalInventoryPick))&&api->HookCode(mod,"rgb_paint.page_update",reinterpret_cast<void*>(&UpdateHook),reinterpret_cast<void**>(&originalUpdate))&&api->HookCode(mod,"rgb_paint.page_destroy",reinterpret_cast<void*>(&DestroyHook),reinterpret_cast<void**>(&originalDestroy))&&api->HookCode(mod,"rgb_paint.end_scene",reinterpret_cast<void*>(&EndSceneHook),reinterpret_cast<void**>(&originalEndScene));
}

void AbortRgbLoad(){sync::AbortLoad();}
