#pragma once
#include "../../hangar_manager/native_ui.h"
#include "sdgo/surface_decal.h"
#include <array>

namespace sdgo::surface_decal::ui {
enum class Action {none,open,slot0,slot1,slot2,place,orbit,left,right,smaller,larger,reset,save,cancel};
class Page {
public:
    bool Bind(const SdgoApi*,SdgoMod*);
    bool Ready(void* service) const noexcept;
    void* FactoryControl(void* service,const wchar_t* name,int kind) const noexcept;
    void* Preview(void* factory) const noexcept {return FactoryControl(factory,L"Multi3DRender",16);}
    void* Footer(void* factory,unsigned index) const noexcept {return index<2?FactoryControl(factory,index?L"ReturnBtn":L"SaveBtn",4):nullptr;}
    bool Attach(void* service,void* buttonSource);
    void Detach(); // before the native page releases its children
    void Render(bool editing,bool placing,unsigned slot,bool busy,bool dirty,bool armed,const Appearance*,const std::wstring& status);
    Action Click(void* control) const;
    void* Control(Action) const;
    bool Visible() const;
    bool Contains(float x,float y) const;
    void* Owner() const {return service_;}
private:
    struct ControlState {void* pointer=nullptr;Action action=Action::none;std::wstring caption;int width=0;bool edit=false;};
    struct Original {void* pointer;std::uint8_t visible;float x,y;};
    sdgo::hangar::ui::Bindings bind_;
    void *service_=nullptr,*root_=nullptr,*table_=nullptr;
    std::vector<ControlState> controls_;
    std::vector<Original> originals_;
    std::array<void*,3> icons_{};
    std::uint32_t font_=1,color_=0xff8ed9ff,shadow_=0xff000000;
    bool editing_=false;
    bool Add(void* source,const wchar_t* name,const wchar_t* caption,Action,int x,int y,int width,bool edit);
    void Text(ControlState&,const std::wstring&);
};
}
