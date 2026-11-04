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
    struct Config {
        uint32_t recipe=0;uint64_t book=0;
        uint32_t material_count=0,material_units[5]{};
        uint64_t material_uids[5]{};
        bool synthesize=false;uint32_t output_unit=0;
    };
    struct Observation {
        uint32_t phase=0,recipe=0,rows=0,row_index=0,page=0,material_count=0;
        uint32_t mouse_calls=0,book_calls=0,visible=0;
        uint32_t material_index=0,material_calls=0,material_slot=0;
        uint64_t material_expected_uid=0,material_actual_uid=0;
        uint32_t confirms=0,request_sequence=0,request_size=0,reply_sequence=0,output_unit=0,output_slot=0;
        uint64_t output_uid=0;
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
        if(config.material_count) {
            if(config.material_count<3 || config.material_count>5)return false;
            for(uint32_t i=0;i<config.material_count;++i)
                if(!config.material_units[i] || !config.material_uids[i])return false;
            mixingVtable_=Symbol<void*>("test.blueprint_mixing_vtable");
            mixingCallback_=Symbol<Callback>("test.blueprint_mixing_callback");
            info_=Symbol<void**>("flow.unit_info_pointer");
            brief_=Symbol<UnitRecord>("flow.unit_brief_by_slot");
            full_=Symbol<UnitRecord>("flow.unit_full_by_slot");
            if(!info_ || !brief_ || !full_ || !CallbackMatches(mixingVtable_,mixingCallback_))return false;
        }
        if(config.synthesize) {
            if(!config.material_count || !config.output_unit)return false;
            dialogVtable_=Symbol<void*>("test.blueprint_confirm_vtable");
            dialogCallback_=Symbol<Callback>("test.blueprint_confirm_callback");
            enqueue_=Symbol<Enqueue>("flow.enqueue_ui_object");
            reply_=Symbol<Reply>("test.blueprint_reply_callback");
            if(!enqueue_ || !reply_ || !CallbackMatches(dialogVtable_,dialogCallback_) || (active_ && active_!=this))return false;
            active_=this;
            if(!api_->HookCode(mod_,"flow.enqueue_ui_object",reinterpret_cast<void*>(&ObserveEnqueue),reinterpret_cast<void**>(&enqueue_)) ||
               !api_->HookCode(mod_,"test.blueprint_reply_callback",reinterpret_cast<void*>(&ObserveReply),reinterpret_cast<void**>(&reply_)))return false;
        }
        state_={};state_.recipe=config.recipe;state_.book=config.book;
        return config.recipe && config.book && manager_ && profile_ && service_ && ctor_ && dtor_ &&
            findControl_ && item_ && mouse_ && gridData_ && gridFind_ &&
            unitVtable_ && unitCallback_ && formVtable_ && formCallback_ &&
            CallbackMatches(unitVtable_,unitCallback_) && CallbackMatches(formVtable_,formCallback_);
    }
    Result EnsureBook();
    Result SelectBook();
    Result SelectMaterials();
    Result Synthesize();
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
    using UnitRecord=void(__thiscall*)(void*,void*,uint16_t);
    using Enqueue=void*(__cdecl*)(void*,int);
    using Reply=void(__stdcall*)(uintptr_t,uintptr_t,uintptr_t,uint32_t,const uint8_t*);
    static void* __cdecl ObserveEnqueue(void* source,int flags);
    static void __stdcall ObserveReply(uintptr_t a,uintptr_t b,uintptr_t c,uint32_t opcode,const uint8_t* packet);
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
    void* mixingVtable_=nullptr;Callback mixingCallback_=nullptr;
    void** info_=nullptr;UnitRecord brief_=nullptr,full_=nullptr;
    uint32_t material_index_=0;bool material_pending_=false;
    void* dialogVtable_=nullptr;Callback dialogCallback_=nullptr;Enqueue enqueue_=nullptr;Reply reply_=nullptr;
    void* volatile liveDialog_=nullptr;volatile LONG replySequence_=0,replyError_=0;
    uint32_t replyUnit_=0;uint64_t replyUid_=0;uint16_t replySlot_=0;
    bool synthArmed_=false,dialogRequested_=false,confirmed_=false;
    uint32_t sendBaseline_=0;
    inline static BlueprintActions* active_=nullptr;
    Config config_{};Observation state_{};bool opened_=false;
};
