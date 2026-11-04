#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include "native_actions.h"
#include "hangar_navigation.h"

// Selects a genuine owned blueprint through the shipped list and SelectBookBtn.
// All service, list, book and recipe state is read-only here. Native mouse
// messages and original UI callbacks own every transition.
class BlueprintActions {
public:
    enum Result : uint32_t { Waiting=1,Ready=2,Submitted=3,Invalid=0x100,
        ServiceMismatch,ControlMismatch,WrongBook,WrongPage,NativeFault };
    struct Config {uint32_t recipe=0;uint64_t book=0;};
    struct Observation {
        uint32_t phase=0,recipe=0,rows=0,row_index=0,page=0,material_count=0;
        uint32_t mouse_calls=0,book_calls=0,visible=0;
        uint64_t book=0,selected_book=0;
        uintptr_t form=0,list=0,mixing=0,slots=0;
        wchar_t entry_control[48]{};
    };
    bool Bind(const SdgoApi* api,SdgoMod* mod,NativeActions* native,HangarNavigation* nav,Config config) {
        api_=api;mod_=mod;native_=native;nav_=nav;config_=config;
        manager_=Symbol<void**>("flow.service_managed_pointer");
        profile_=Symbol<uint8_t**>("flow.profile_pointer");
        service_=Symbol<Service>("flow.service_find_by_name");
        ctor_=Symbol<Ctor>("flow.native_wstring_ctor");dtor_=Symbol<Dtor>("flow.native_wstring_dtor");
        findControl_=Symbol<Control>("flow.native_ui_find_control");
        item_=Symbol<Item>("flow.unit_list_item_by_index");mouse_=Symbol<Mouse>("flow.unit_list_mouse_business");
        gridData_=Symbol<GridData>("grid.data");gridFind_=Symbol<GridFind>("grid.find");
        unitVtable_=Symbol<void*>("flow.battleship_lobby_manager_vtable");
        unitCallback_=Symbol<Callback>("flow.battleship_lobby_select_business");
        formVtable_=Symbol<void*>("test.blueprint_form_vtable");
        formCallback_=Symbol<Callback>("test.blueprint_form_callback");
        state_={};state_.recipe=config.recipe;state_.book=config.book;
        return config.recipe && config.book && manager_ && profile_ && service_ && ctor_ && dtor_ &&
            findControl_ && item_ && mouse_ && gridData_ && gridFind_ &&
            unitVtable_ && unitCallback_ && formVtable_ && formCallback_ &&
            CallbackMatches(unitVtable_,unitCallback_) && CallbackMatches(formVtable_,formCallback_);
    }
    Result EnsureBook();
    Result SelectBook();
    Observation State()const{return state_;}
private:
    using Service=void*(__thiscall*)(void*,const void*);
    using Ctor=void*(__thiscall*)(void*,const wchar_t*);using Dtor=void(__thiscall*)(void*);
    using Control=void*(__thiscall*)(void*,const void*,uint32_t);
    using Callback=void(__stdcall*)(void*,uint32_t,const void*,void*);
    using Item=void*(__thiscall*)(void*,int);
    using Mouse=uint8_t(__thiscall*)(void*,uint32_t,int32_t,int32_t,uint32_t,uint32_t);
    using GridData=void*(__cdecl*)();
    using GridFind=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    template<class T>T Symbol(const char* name)const{return reinterpret_cast<T>(api_->Symbol(mod_,name));}
    template<class T>static T Read(const void* p){T v{};memcpy(&v,p,sizeof(v));return v;}
    static bool CallbackMatches(void* table,Callback cb) {
        __try {return table && cb && static_cast<void**>(table)[0x38/4]==reinterpret_cast<void*>(cb);}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    uint8_t* Resolve(const wchar_t* name)const {
        if(!*manager_)return nullptr;
        alignas(4) uint8_t descriptor[28]{};*reinterpret_cast<uint32_t*>(descriptor)=1;
        bool made=false;void* result=nullptr;
        __try {made=ctor_(descriptor+4,name)!=nullptr;if(made)result=service_(*manager_,descriptor);}
        __finally {if(made)dtor_(descriptor+4);}
        return static_cast<uint8_t*>(result);
    }
    uint8_t* Find(void* root,const wchar_t* name,uint32_t kind)const {
        alignas(4) uint8_t text[24]{};bool made=false;void* result=nullptr;
        __try {made=ctor_(text,name)!=nullptr;if(made)result=findControl_(root,text,kind);}
        __finally {if(made)dtor_(text);}
        return static_cast<uint8_t*>(result);
    }
    bool Match(void* root,uint8_t* control,const wchar_t* name,uint32_t kind)const {
        return root && control && Read<void*>(control+0x15c)==root && Find(root,name,kind)==control;
    }
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;NativeActions* native_=nullptr;HangarNavigation* nav_=nullptr;
    void** manager_=nullptr;uint8_t** profile_=nullptr;Service service_=nullptr;Ctor ctor_=nullptr;Dtor dtor_=nullptr;
    Control findControl_=nullptr;Item item_=nullptr;Mouse mouse_=nullptr;
    GridData gridData_=nullptr;GridFind gridFind_=nullptr;
    void* unitVtable_=nullptr;void* formVtable_=nullptr;Callback unitCallback_=nullptr,formCallback_=nullptr;
    Config config_{};Observation state_{};bool opened_=false;
};
