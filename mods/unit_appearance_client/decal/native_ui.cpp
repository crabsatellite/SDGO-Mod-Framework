#include "native_ui.h"
#include <windows.h>
#include <cstring>
#include <cmath>

namespace sdgo::surface_decal::ui {
namespace {
using String=sdgo::hangar::ui::NativeString;
template<class T>T Read(const void* p,unsigned offset=0){T v{};std::memcpy(&v,static_cast<const std::uint8_t*>(p)+offset,sizeof v);return v;}
template<class T>void Put(void* p,unsigned offset,T value){std::memcpy(static_cast<std::uint8_t*>(p)+offset,&value,sizeof value);}
String View(const std::wstring& value){String s;s.length=static_cast<std::uint32_t>(value.size());if(s.length<8)std::memcpy(s.data.small,value.c_str(),(s.length+1)*2);else{s.capacity=s.length;s.data.pointer=value.c_str();}return s;}
}
bool Page::Bind(const SdgoApi* api,SdgoMod* mod){
    using B=sdgo::hangar::ui::Bindings;
    bind_.add=reinterpret_cast<B::Add>(api->Symbol(mod,"hangar.ui_add_button"));bind_.find=reinterpret_cast<B::Find>(api->Symbol(mod,"hangar.ui_find_control"));
    bind_.text=reinterpret_cast<B::Text>(api->Symbol(mod,"hangar.ui_text_item"));bind_.skin=reinterpret_cast<B::Skin>(api->Symbol(mod,"hangar.ui_add_skin"));
    bind_.buttonVtable=api->Symbol(mod,"hangar.ui_button_vtable");return bind_.add&&bind_.find&&bind_.text&&bind_.skin&&bind_.buttonVtable;
}
bool Page::Ready(void* service) const noexcept {
    __try {
        // Native page updates also run while the page is inactive and its
        // cached control fields have not been initialized. Check activation
        // before even reading the dialog pointer.
        if(!service||!Read<std::uint8_t>(service,0xac))return false;
        auto* root=Read<void*>(service,0xd0);
        return root&&Read<std::uint8_t>(root,0x162)!=0;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void* Page::FactoryControl(void* service,const wchar_t* name,int kind) const noexcept {
    __try {
        if(!bind_.find||!Ready(service))return nullptr;
        auto* root=Read<void*>(service,0xd0);
        String key;key.length=static_cast<unsigned>(std::wcslen(name));
        if(key.length<8)std::memcpy(key.data.small,name,(key.length+1)*sizeof(wchar_t));
        else {key.data.pointer=name;key.capacity=key.length;}
        auto* child=bind_.find(root,&key,kind);
        if(!child||Read<void*>(child,0x15c)!=root||Read<unsigned>(child,0x134)!=static_cast<unsigned>(kind))return nullptr;
        if(kind==4&&Read<void*>(child)!=bind_.buttonVtable)return nullptr;
        return child;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
void Page::Detach(){service_=root_=table_=nullptr;controls_.clear();originals_.clear();icons_={};editing_=false;}
bool Page::Add(void* source,const wchar_t* name,const wchar_t* caption,Action action,int x,int y,int width,bool edit){
    const std::wstring key=name;const auto native=View(key);bind_.add(root_,&native,x,y,width,22,0);
    auto* c=bind_.find(root_,&native,4);if(!c||Read<void*>(c)!=bind_.buttonVtable||Read<void*>(c,0x15c)!=root_)return false;
    Put<std::uint8_t>(c,0x162,0);Put<std::uint8_t>(c,0x167,0);Put<std::uint8_t>(c,0x168,0);
    controls_.push_back({c,action,{},width,edit});
    if(action!=Action::none){
        auto** skins=Read<std::uint8_t**>(source,0x1a4);auto** end=Read<std::uint8_t**>(source,0x1a8);
        if(!skins||end-skins<3||end-skins>16)return false;
        for(unsigned i=0;i<3;++i){if(!skins[i])return false;bind_.skin(c,reinterpret_cast<String*>(skins[i]+4),Read<std::uint8_t>(skins[i],0x44),Read<std::uint32_t>(skins[i],0x3c),Read<std::uint32_t>(skins[i],0x40));}
    }
    Text(controls_.back(),caption);return true;
}
void Page::Text(ControlState& c,const std::wstring& caption){
    if(c.caption==caption)return;const auto s=View(caption);bind_.text(c.pointer,0,&s,2,5,c.width-4,14,color_,shadow_,0,font_,c.action==Action::none?1:2);c.caption=caption;
}
bool Page::Attach(void* service,void* source){
    if(service_==service&&root_==Read<void*>(service,0xd0))return true;
    Detach();if(!service||!source||Read<void*>(source)!=bind_.buttonVtable)return false;
    auto* root=Read<void*>(service,0xd0);if(!root||Read<int>(root,0x154)<260||Read<int>(root,0x158)<530)return false;
    service_=service;root_=root;table_=Read<void*>(service);
    auto* head=Read<void*>(root,0x23c);auto* link=head?Read<void*>(head):nullptr;
    for(unsigned i=0;link&&link!=head&&i<256;++i){auto* c=Read<void*>(link,8);if(c)originals_.push_back({c,Read<std::uint8_t>(c,0x162),Read<float>(c,0x148),Read<float>(c,0x14c)});link=Read<void*>(link);}
    auto** text=Read<std::uint8_t**>(source,0x190);auto** end=Read<std::uint8_t**>(source,0x194);
    if(text&&end>text&&*text){font_=Read<std::uint32_t>(*text,0x6c);color_=Read<std::uint32_t>(*text,0x58);shadow_=Read<std::uint32_t>(*text,0x5c);}
    for(unsigned i=0;i<3;++i)icons_[i]=Read<void*>(service,0xe0+i*4);
    bool ok=Add(source,L"SurfaceOpen",L"自由贴花位置",Action::open,65,456,138,false);
    const wchar_t* labels[]={L"贴纸 1",L"贴纸 2",L"贴纸 3"};
    // The native button sprite has a fixed width of 138 pixels. Its hit box
    // does not resize the sprite, so keep a single column inside this panel.
    for(unsigned i=0;i<3;++i){const auto name=L"SurfaceSlot"+std::to_wstring(i);ok=Add(source,name.c_str(),labels[i],static_cast<Action>(static_cast<unsigned>(Action::slot0)+i),65,104+26*i,138,true)&&ok;}
    ok=Add(source,L"SurfaceHint0",L"左键放置 · 右键转动",Action::none,16,184,246,true)&&ok;
    ok=Add(source,L"SurfacePlace",L"放置贴纸",Action::place,65,214,138,true)&&ok;
    ok=Add(source,L"SurfaceOrbit",L"旋转机体",Action::orbit,65,240,138,true)&&ok;
    ok=Add(source,L"SurfaceLeft",L"贴纸左转",Action::left,65,266,138,true)&&ok;
    ok=Add(source,L"SurfaceRight",L"贴纸右转",Action::right,65,292,138,true)&&ok;
    ok=Add(source,L"SurfaceSmaller",L"缩小贴纸",Action::smaller,65,318,138,true)&&ok;
    ok=Add(source,L"SurfaceLarger",L"放大贴纸",Action::larger,65,344,138,true)&&ok;
    ok=Add(source,L"SurfaceReset",L"恢复原位置",Action::reset,65,370,138,true)&&ok;
    ok=Add(source,L"SurfaceSave",L"保存位置",Action::save,65,396,138,true)&&ok;
    ok=Add(source,L"SurfaceCancel",L"取消并返回",Action::cancel,65,422,138,true)&&ok;
    ok=Add(source,L"SurfaceViewHint",L"上下拖动可调整俯仰",Action::none,16,456,246,true)&&ok;
    ok=Add(source,L"SurfaceStatus0",L"",Action::none,16,494,246,false)&&ok;
    ok=Add(source,L"SurfaceStatus1",L"",Action::none,16,514,246,false)&&ok;
    if(!ok){for(auto& c:controls_)Put<std::uint8_t>(c.pointer,0x162,0);Detach();return false;}return true;
}
bool Page::Visible() const{return service_&&Read<void*>(service_)==table_&&Read<void*>(service_,0xd0)==root_&&Read<std::uint8_t>(service_,0xac)&&Read<std::uint8_t>(root_,0x162);}
bool Page::Contains(float x,float y) const {
    if(!Visible())return false;float left=Read<float>(root_,0x148),top=Read<float>(root_,0x14c);
    return x>=left&&y>=top&&x<left+Read<int>(root_,0x154)&&y<top+Read<int>(root_,0x158);
}
void Page::Render(bool edit,bool placing,unsigned slot,bool busy,bool dirty,bool armed,const Appearance* appearance,const std::wstring& status){
    if(!service_||Read<void*>(service_)!=table_||Read<void*>(service_,0xd0)!=root_){Detach();return;}
    if(edit!=editing_){
        for(const auto& o:originals_){Put<std::uint8_t>(o.pointer,0x162,edit?0:o.visible);Put<float>(o.pointer,0x148,o.x);Put<float>(o.pointer,0x14c,o.y);}
        editing_=edit;
    }
    if(edit){
        for(const auto& o:originals_)Put<std::uint8_t>(o.pointer,0x162,0);
        for(unsigned i=0;i<3;++i)if(icons_[i]){Put<std::uint8_t>(icons_[i],0x162,1);Put<float>(icons_[i],0x148,static_cast<float>(19+80*i));Put<float>(icons_[i],0x14c,28);}
    }
    unsigned statusLine=0;
    for(auto& c:controls_){
        bool show=c.edit==edit,enable=c.action!=Action::none&&!busy;
        const bool isStatus=c.action==Action::none&&!c.edit;
        if(isStatus){show=true;const auto start=statusLine++*17;Text(c,status.size()>start?status.substr(start,17):L"");}
        if(c.action==Action::open)enable=appearance&&!busy;
        if(c.action>=Action::slot0&&c.action<=Action::slot2){const auto i=static_cast<unsigned>(c.action)-static_cast<unsigned>(Action::slot0);enable=enable&&appearance&&appearance->slots[i].sticker;Text(c,(i==slot?L"● ":L"")+std::wstring(L"贴纸 ")+std::to_wstring(i+1));}
        if(c.action==Action::place)Text(c,placing?L"● 放置贴纸":L"放置贴纸");
        if(c.action==Action::orbit)Text(c,placing?L"旋转机体":L"● 旋转机体");
        if(c.action>=Action::left&&c.action<=Action::larger)enable=enable&&appearance&&slot<3&&appearance->slots[slot].positioned;
        if(c.action==Action::save){enable=enable&&dirty;Text(c,armed?L"确认保存位置":L"保存位置");}
        Put<std::uint8_t>(c.pointer,0x162,show?1:0);Put<std::uint8_t>(c.pointer,0x167,enable?1:0);
    }
}
Action Page::Click(void* control) const {
    if(!Visible())return Action::none;
    for(const auto& c:controls_)if(c.pointer==control&&Read<std::uint8_t>(control,0x162))return Read<std::uint8_t>(control,0x167)?c.action:Action::none;
    if(editing_)for(unsigned i=0;i<3;++i)if(icons_[i]==control)return static_cast<Action>(static_cast<unsigned>(Action::slot0)+i);
    return Action::none;
}
void* Page::Control(Action action) const{for(const auto& c:controls_)if(c.action==action)return c.pointer;return nullptr;}
}
