#include "native_ui.h"
#include <windows.h>
#include <cstring>
#include <cmath>
namespace sdgo::storage::ui {
namespace {
template<class T>T Read(const void* p,size_t offset=0){T value;memcpy(&value,static_cast<const uint8_t*>(p)+offset,sizeof(value));return value;}
template<class T>void Put(void* p,size_t offset,T value){memcpy(static_cast<uint8_t*>(p)+offset,&value,sizeof(value));}
String View(const std::wstring& value){String s;s.length=static_cast<uint32_t>(value.size());if(value.size()<8)memcpy(s.data.small,value.c_str(),(value.size()+1)*2);else{s.data.pointer=value.c_str();s.capacity=s.length;}return s;}
}
bool Page::Bind(const SdgoApi* api,SdgoMod* mod) {
    bindings_.add=reinterpret_cast<Bindings::Add>(api->Symbol(mod,"storage.ui_add_button"));
    bindings_.find=reinterpret_cast<Bindings::Find>(api->Symbol(mod,"storage.ui_find_control"));
    bindings_.text=reinterpret_cast<Bindings::Text>(api->Symbol(mod,"storage.ui_text_item"));
    bindings_.skin=reinterpret_cast<Bindings::Skin>(api->Symbol(mod,"storage.ui_add_skin"));
    bindings_.buttonVtable=api->Symbol(mod,"storage.ui_button_vtable");
    return bindings_.add && bindings_.find && bindings_.text && bindings_.skin && bindings_.buttonVtable;
}
void Page::Detach(){service_=dialog_=identity_=button_=status_=notice_=nullptr;noticeHidden_=false;published_=false;caption_.clear();message_.clear();}
void Page::Unpublish(){if(button_){Put<uint8_t>(button_,0x162,0);Put<uint8_t>(button_,0x167,0);}if(status_)Put<uint8_t>(status_,0x162,0);
    if(notice_ && noticeHidden_){Put<uint8_t>(notice_,0x162,1);noticeHidden_=false;}published_=false;}
bool Page::Attach(void* service) {
    Detach();if(!service || !bindings_.add || !bindings_.find || !bindings_.text || !bindings_.skin)return false;
    auto* dialog=Read<void*>(service,0x70);auto* source=Read<void*>(service,0xB4); // TabRadio0
    if(!dialog || !source || Read<void*>(source,0x15C)!=dialog)return false;
    const int w=Read<int>(dialog,0x154),h=Read<int>(dialog,0x158);
    const int bw=Read<int>(source,0x154),bh=Read<int>(source,0x158);
    const float y=Read<float>(source,0x14C);
    auto* other=Read<void*>(service,0xB8);
    if(!other || Read<void*>(other,0x15C)!=dialog)return false;
    const float x=Read<float>(other,0x148)+static_cast<float>(Read<int>(other,0x154))+9.0f;
    if(w<420 || w>2048 || h<520 || h>2048 || bw<100 || bw>200 || bh<18 || bh>36 || !std::isfinite(x) || !std::isfinite(y) || x<0 || y<0 || x+bw>w-20 || y+bh>h-50)return false;
    auto** first=Read<uint8_t**>(source,0x1A4);auto** last=Read<uint8_t**>(source,0x1A8);
    if(!first || reinterpret_cast<uintptr_t>(last)<reinterpret_cast<uintptr_t>(first) || reinterpret_cast<uintptr_t>(last)-reinterpret_cast<uintptr_t>(first)!=4*sizeof(void*))return false;
    for(size_t i:{0u,3u})if(!first[i] || !Read<void*>(first[i]) || !Read<uint32_t>(first[i],0x14))return false;
    auto** texts=Read<uint8_t**>(source,0x190);auto** textEnd=Read<uint8_t**>(source,0x194);
    if(!texts || reinterpret_cast<uintptr_t>(textEnd)<=reinterpret_cast<uintptr_t>(texts) || !*texts)return false;
    font_=Read<uint32_t>(*texts,0x6C);color_=Read<uint32_t>(*texts,0x58);shadow_=Read<uint32_t>(*texts,0x5C);
    auto add=[&](const wchar_t* name,int ax,int ay,int aw,int ah)->void*{
        const std::wstring ownedName=name;
        const auto key=View(ownedName);bindings_.add(dialog,&key,ax,ay,aw,ah,0);
        auto* child=bindings_.find(dialog,&key,4);
        if(!child || Read<void*>(child)!=bindings_.buttonVtable || Read<void*>(child,0x15C)!=dialog)return nullptr;
        Put<uint8_t>(child,0x162,0);Put<uint8_t>(child,0x167,0);Put<uint8_t>(child,0x168,0);return child;
    };
    auto* button=add(L"ModStorageClaimAll",static_cast<int>(x),static_cast<int>(y),bw,bh);
    if(!button)return false;
    for(size_t i:{0u,3u,3u})bindings_.skin(button,reinterpret_cast<const String*>(first[i]+4),Read<uint8_t>(first[i],0x44),Read<uint32_t>(first[i],0x3C),Read<uint32_t>(first[i],0x40));
    auto** skins=Read<uint8_t**>(button,0x1A4);auto** end=Read<uint8_t**>(button,0x1A8);
    if(!skins || reinterpret_cast<uintptr_t>(end)-reinterpret_cast<uintptr_t>(skins)!=3*sizeof(void*))return false;
    for(size_t i=0;i<3;++i)if(!skins[i] || !Read<void*>(skins[i]))return false;
    // While there is a batch to describe, the status takes the place of the
    // native expiry notice line (inside the list frame); the notice returns after.
    auto* notice=Read<void*>(service,0x98);
    int sx=12,sy=h-73,sw=w-24;
    if(notice && Read<void*>(notice,0x15C)==dialog) {
        const float nx=Read<float>(notice,0x148),ny=Read<float>(notice,0x14C);const int nw=Read<int>(notice,0x154);
        if(std::isfinite(nx) && std::isfinite(ny) && nx>=0 && ny>y+bh && ny<h-40 && nw>=150 && nx+nw<=w){sx=static_cast<int>(nx);sy=static_cast<int>(ny);sw=nw;}
        else notice=nullptr;
    }else notice=nullptr;
    auto* status=add(L"ModStorageClaimStatus",sx,sy,sw,14);if(!status)return false;
    service_=service;dialog_=dialog;identity_=Read<void*>(dialog);button_=button;status_=status;notice_=notice;noticeHidden_=false;width_=bw;
    Display(L"一键领取",L"");
    published_=true;Put<uint8_t>(button_,0x162,1);return true;
}
bool Page::Visible()const {
    if(!published_)return false;
    __try{return Read<void*>(service_,0x70)==dialog_ && Read<void*>(dialog_)==identity_ && Read<uint8_t>(dialog_,0x162)!=0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Page::Click(void* control)const{return Visible() && control==button_ && Read<uint8_t>(button_,0x167)!=0;}
void Page::Display(const std::wstring& caption,const std::wstring& status,bool enabled) {
    if(!button_ || !status_)return;
    if(caption_!=caption){const auto text=View(caption);bindings_.text(button_,0,&text,2,3,width_-4,14,color_,shadow_,0,font_,2);caption_=caption;}
    if(message_!=status){const auto text=View(status);bindings_.text(status_,0,&text,0,0,Read<int>(status_,0x154),14,color_,shadow_,0,font_,2);message_=status;}
    // The status line only appears while there is a batch to describe; it then
    // covers the native expiry notice, which comes back once the status clears.
    if(published_) {
        Put<uint8_t>(status_,0x162,status.empty()?0:1);
        if(notice_) {
            if(!status.empty() && !noticeHidden_ && Read<uint8_t>(notice_,0x162)){Put<uint8_t>(notice_,0x162,0);noticeHidden_=true;}
            else if(status.empty() && noticeHidden_){Put<uint8_t>(notice_,0x162,1);noticeHidden_=false;}
        }
    }
    Put<uint8_t>(button_,0x167,enabled?1:0);
    if(!enabled){Put<uint16_t>(button_,0x21C,0);Put<uint8_t>(button_,0x164,0);Put<uint32_t>(button_,0x18C,0);}
}
}
