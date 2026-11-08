#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <cstdio>
#include "native_actions.h"
#include "hangar_navigation.h"

// Drives the original encyclopedia controls. All selected IDs, model slots
// and form states are read back; no list/profile/model state is written here.
class UnitPresentationsActions {
public:
    enum Result : uint32_t { Waiting=1,Ready=2,Submitted=3,Invalid=0x100,
        ServiceMismatch,ControlMismatch,MissingIndex,WrongDetail,NativeFault,CollectionHintMismatch };
    struct Observation {
        uint32_t unit=0,index_id=0,slot=0,form=0,selection_calls=0,form_calls=0,pose_handle=0;
        uintptr_t list=0,detail=0,model=0;
        float animation_state=0;
        uint32_t phase=0,rank=0,list_flags=0,first_visible_id=0,visible_count=0;
        uint32_t detail_unit=0,detail_active=0,detail_visible=0,projection_mode=0,pending_actions=0;
    };
    bool Bind(const SdgoApi* api,SdgoMod* mod,HangarNavigation* navigation) {
        api_=api;mod_=mod;navigation_=navigation;
        manager_=Symbol<void**>("flow.service_managed_pointer");
        service_=Symbol<Service>("flow.service_find_by_name");
        ctor_=Symbol<Ctor>("flow.native_wstring_ctor");dtor_=Symbol<Dtor>("flow.native_wstring_dtor");
        control_=Symbol<Control>("flow.native_ui_find_control");
        edit_=Symbol<Edit>("flow.native_edit_set_text");
        grid_=Symbol<Grid>("grid.data");find_=Symbol<Find>("grid.find");
        listVtable_=Symbol<void*>("test.unit_index_list_vtable");
        detailVtable_=Symbol<void*>("test.unit_index_detail_vtable");
        listCallback_=Symbol<Callback>("test.unit_index_list_callback");
        detailCallback_=Symbol<Callback>("test.unit_index_detail_callback");
        modelSlot_=Symbol<ModelSlot>("test.unit_index_model_slot");
        entity_=Symbol<Entity>("test.unit_index_model_entity");
        play_=Symbol<Play>("test.unit_index_play_animation");
        return manager_ && service_ && ctor_ && dtor_ && control_ && edit_ && grid_ && find_ &&
            listVtable_ && detailVtable_ && listCallback_ && detailCallback_ && modelSlot_ && entity_ && play_;
    }
    Result EnsureList(uint32_t unit) {
        __try {
            state_.unit=unit;
            state_.phase=1;
            const uint8_t* definition=nullptr;
            if(!find_(grid_(),&definition,unit,2,0) || !definition)return Invalid;
            if(!state_.index_id) {
                for(uint32_t id=0x375BE1;id<0x375BE1+16384;++id) {
                    const uint8_t* row=nullptr;
                    if(!find_(grid_(),&row,id,0x55,0) || !row)break;
                    if(row[5]==1 && Read<uint32_t>(row+6)==unit) {
                        if(state_.index_id)return MissingIndex;
                        state_.index_id=id;
                    }
                }
                if(!state_.index_id)return MissingIndex;
            }
            // Services and roots exist before their native page is entered;
            // the widget's default visible bit alone does not mean it is open.
            if(!opened_) {
                const auto ready=navigation_->EnsureHangar();
                if(ready>=HangarNavigation::kDownmenuServiceMismatch)return Invalid;
                if(ready!=HangarNavigation::kReady)return Waiting;
                const auto clicked=navigation_->ClickOperatorMenuButton(L"UnitListBtn",0x84);
                if(clicked>=HangarNavigation::kDownmenuServiceMismatch)return Invalid;
                opened_=clicked==HangarNavigation::kReady;
                return Submitted;
            }
            auto* list=Resolve(L"UnitCatalogList");
            if(list && Read<void*>(list)!=listVtable_)return ServiceMismatch;
            auto* root=list ? Read<uint8_t*>(list+0x44):nullptr;
            if(!list || !root || !root[0x162]) {
                return Waiting;
            }
            state_.list=reinterpret_cast<uintptr_t>(list);
            state_.rank=Read<uint32_t>(list+0x3C0);
            state_.list_flags=list[0x472]|(static_cast<uint32_t>(list[0x473])<<8);
            state_.first_visible_id=Read<uint32_t>(list+0x3F8);state_.visible_count=0;
            for(unsigned i=0;i<21;++i)if(Read<uint32_t>(list+0x3F8+4*i))++state_.visible_count;
            state_.phase=2;
            if(list[0x472] || list[0x473])return Waiting;
            // 005D2280 changes the selected rank immediately, then queues
            // command 9 through 005CAEF0. The vectors behind the old page are
            // not replaced until that queue completes. Submitting SearchBtn
            // meanwhile queues a search against the unfinished vectors and
            // the native 005D1EE0 renderer can dereference a null entry.
            if(Read<uint32_t>(list+0x300))return Waiting;
            // The native open queue slides the root from offscreen and then
            // builds its first page. Searching earlier is reset by that queue.
            if(!searched_ && (Read<uint32_t>(root+0x148)!=Read<uint32_t>(list+0x2F0) || !state_.visible_count))
                return Waiting;
            state_.phase=3;
            if(!ranked_) {
                const uint32_t rank=definition[308];
                if(rank<1 || rank>4)return Invalid;
                if(Read<uint32_t>(list+0x3C0)!=rank) {
                    const wchar_t* names[]={L"SRankRadio",L"ARankRadio",L"BRankRadio",L"CRankRadio"};
                    auto* radio=Read<uint8_t*>(list+0x260+(4-rank)*4);
                    if(!Match(root,radio,names[4-rank],12))return ControlMismatch;
                    listCallback_(list,0x301,radio+0x118,radio);
                    ranked_=true;return Submitted;
                }
                ranked_=true;
            }
            if(!searched_) {
                state_.phase=4;
                auto* edit=Read<uint8_t*>(list+0x2E0);
                auto* search=Read<uint8_t*>(list+0x27C);
                if(!Match(root,edit,L"SearchEdit",8) || !Match(root,search,L"SearchBtn",4))return ControlMismatch;
                wchar_t title[51]{};
                memcpy(title,definition+5,100);
                alignas(4) uint8_t text[24]{};
                bool made=false;
                __try {
                    made=ctor_(text,title)!=nullptr;
                    if(made)edit_(edit,text,0);
                } __finally {if(made)dtor_(text);}
                if(!made)return Invalid;
                listCallback_(list,0x101,search+0x118,search);
                searched_=true;return Submitted;
            }
            state_.phase=5;
            for(uint32_t slot=0;slot<21;++slot) {
                if(Read<uint32_t>(list+0x3F8+4*slot)==unit) {
                    state_.slot=slot;
                    return Ready;
                }
            }
            return Waiting;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }
    Result OpenDetail() {
        __try {
            auto* detail=ResolveDetail();
            state_.detail=reinterpret_cast<uintptr_t>(detail);
            state_.detail_unit=detail ? DetailUnit(detail):0;
            if(detail && Read<void*>(detail)!=detailVtable_)return ServiceMismatch;
            if(detail && DetailUnit(detail)==state_.unit) {
                const auto observed=ReadDetail(detail);
                if(observed==Ready)return Ready;
            }
            if(state_.selection_calls)return Waiting;
            auto* list=Resolve(L"UnitCatalogList");
            if(!list || Read<void*>(list)!=listVtable_)return ServiceMismatch;
            if(list[0x472] || list[0x473])return Waiting;
            auto* root=Read<uint8_t*>(list+0x44);
            if(!root || Read<uint32_t>(list+0x3F8+4*state_.slot)!=state_.unit)return WrongDetail;
            const size_t offsets[]={0x48,0xF4,0x148};
            const wchar_t* names[]={L"UnitIcon",L"UnitSelectIcon",L"UnitSelectedIcon"};
            for(unsigned i=0;i<3;++i) {
                wchar_t name[48]{};swprintf_s(name,L"%s%u",names[i],state_.slot);
                auto* icon=Read<uint8_t*>(list+offsets[i]+4*state_.slot);
                if(!Match(root,icon,name,1) || !icon[0x162] || !icon[0x167])continue;
                listCallback_(list,0x802,icon+0x118,icon);
                ++state_.selection_calls;return Submitted;
            }
            return ControlMismatch;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }
    Result SwitchForm(uint32_t target) {
        __try {
            if(target>1)return Invalid;
            auto* detail=ResolveDetail();
            const auto ready=ReadDetail(detail);
            if(ready!=Ready)return ready;
            // 005C9D40 restores the scalar to 100 when dequeuing a completed
            // transition. The native queue, not a low scalar, proves completion.
            if(state_.form==target && !state_.pending_actions)return Ready;
            if(formPending_==target+1)return Waiting;
            auto* root=Read<uint8_t*>(detail+0x148);
            const wchar_t* name=target ? L"TabLRadio1":L"TabLRadio0";
            auto* radio=Read<uint8_t*>(detail+(target?0x150:0x14C));
            if(!Match(root,radio,name,12) || !radio[0x162] || !radio[0x167])return ControlMismatch;
            detailCallback_(detail,0x301,radio+0x118,radio);
            formPending_=target+1;++state_.form_calls;return Submitted;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }
    Result Close() {
        __try {
            auto* detail=ResolveDetail();
            if(detail && Read<void*>(detail)!=detailVtable_)return ServiceMismatch;
            auto* root=detail ? Read<uint8_t*>(detail+0x148):nullptr;
            if(!detail || !root || !root[0x162] || !detail[0x70])return Ready;
            if(closed_)return Waiting;
            auto* out=FindControl(root,L"OutBtn",4);
            if(!Match(root,out,L"OutBtn",4))return ControlMismatch;
            detailCallback_(detail,0x101,out+0x118,out);
            closed_=true;return Submitted;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }
    Result ReadyPose(bool choice) {
        __try {
            auto* detail=ResolveDetail();
            const auto ready=ReadDetail(detail);
            if(ready!=Ready)return ready;
            alignas(4) uint8_t name[24]{};bool made=false;uint32_t handle=0;
            __try {
                made=ctor_(name,choice?L"choice_01":L"wait_01")!=nullptr;
                if(made)handle=play_(reinterpret_cast<void*>(state_.model),name,
                    choice?0:1,1.0f,0.1f,2,1,0);
            } __finally {if(made)dtor_(name);}
            if(!handle || Read<uint32_t>(reinterpret_cast<const uint8_t*>(state_.model)+0x400)!=handle)
                return Invalid;
            state_.pose_handle=handle;return Ready;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }
    bool WritePortraitSource(const wchar_t* path,const char* animation) {
        __try {
            auto* detail=ResolveDetail();
            if(ReadDetail(detail)!=Ready)return false;
            auto* render=Read<uint8_t*>(detail+0x1C8);
            if(!render[0x162] || !render[0x163])return false;
            const float fx=Read<float>(render+0x148),fy=Read<float>(render+0x14C);
            int width=Read<int>(render+0x154),height=Read<int>(render+0x158);
            if(!std::isfinite(fx) || !std::isfinite(fy) || width<=0 || height<=0)return false;
            int x=static_cast<int>(fx),y=static_cast<int>(fy);
            auto* parent=Read<uint8_t*>(render+0x15C);
            if(parent) {
                const float px=Read<float>(parent+0x148),py=Read<float>(parent+0x14C);
                if(!std::isfinite(px) || !std::isfinite(py))return false;
                x+=static_cast<int>(px);y+=static_cast<int>(py);
            }
            const uint8_t* row=nullptr;
            if(!find_(grid_(),&row,state_.unit,2,0) || !row)return false;
            wchar_t modelWide[31]{};memcpy(modelWide,row+612,60);
            char model[256]{};
            if(!WideCharToMultiByte(CP_UTF8,0,modelWide,-1,model,sizeof(model),nullptr,nullptr))return false;
            for(char* c=model;*c;++c)if(*c=='\\')*c='/';
            FILE* file=_wfopen(path,L"wb");if(!file)return false;
            const int written=fprintf(file,"{\"schema\":1,\"unit_id\":%u,\"model\":\"%s\",\"animation\":\"%s\",\"pose_handle\":%u,\"viewport\":[%d,%d,%d,%d],\"renderer\":\"original-unit-detail-backbuffer\"}\n",
                state_.unit,model,animation,state_.pose_handle,x,y,width,height);
            return fclose(file)==0 && written>0;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    const Observation& State() const {return state_;}
private:
    using Service=void*(__thiscall*)(void*,const void*);
    using Ctor=void*(__thiscall*)(void*,const wchar_t*);
    using Dtor=void(__thiscall*)(void*);
    using Control=void*(__thiscall*)(void*,const void*,uint32_t);
    using Edit=void(__thiscall*)(void*,const void*,uint32_t);
    using Callback=void(__stdcall*)(void*,uint32_t,const void*,void*);
    using Grid=void*(__cdecl*)();
    using Find=uint8_t(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
    using ModelSlot=void*(__thiscall*)(void*,uint32_t);
    using Entity=void*(__thiscall*)(void*);
    using Play=uint32_t(__thiscall*)(void*,const void*,uint8_t,float,float,uint32_t,uint32_t,uint8_t);
    template<class T>T Symbol(const char* name)const{return reinterpret_cast<T>(api_->Symbol(mod_,name));}
    template<class T>static T Read(const void* p){T value{};memcpy(&value,p,sizeof(value));return value;}
    uint8_t* Resolve(const wchar_t* name)const {
        if(!*manager_)return nullptr;
        alignas(4) uint8_t descriptor[28]{};*reinterpret_cast<uint32_t*>(descriptor)=1;
        bool made=false;void* service=nullptr;
        __try {made=ctor_(descriptor+4,name)!=nullptr;if(made)service=service_(*manager_,descriptor);}
        __finally {if(made)dtor_(descriptor+4);}
        return static_cast<uint8_t*>(service);
    }
    uint8_t* FindControl(void* root,const wchar_t* name,uint32_t kind)const {
        alignas(4) uint8_t text[24]{};bool made=false;void* control=nullptr;
        __try {made=ctor_(text,name)!=nullptr;if(made)control=control_(root,text,kind);}
        __finally {if(made)dtor_(text);}
        return static_cast<uint8_t*>(control);
    }
    uint8_t* ResolveDetail()const {
        uint8_t* waiting=nullptr;
        for(unsigned index=0;index<10;++index) {
            wchar_t name[64]{};swprintf_s(name,L"DetailUnitInfo%u",index);
            auto* detail=Resolve(name);
            if(!detail || Read<void*>(detail)!=detailVtable_ || DetailUnit(detail)!=state_.unit ||
                    Read<uint32_t>(detail+0x68)!=index)continue;
            auto* root=Read<uint8_t*>(detail+0x148);
            if(detail[0x70] && root && root[0x162])return detail;
            if(!waiting)waiting=detail;
        }
        return waiting;
    }
    static uint32_t DetailUnit(const uint8_t* detail) {
        return Read<uint32_t>(detail+0x60);
    }
    bool Match(void* root,uint8_t* control,const wchar_t* name,uint32_t kind)const {
        return root && control && Read<void*>(control+0x15C)==root && FindControl(root,name,kind)==control;
    }
    Result ReadDetail(uint8_t* detail) {
        if(!detail)return Waiting;
        state_.detail=reinterpret_cast<uintptr_t>(detail);state_.detail_unit=DetailUnit(detail);
        if(Read<void*>(detail)!=detailVtable_ || state_.detail_unit!=state_.unit)return WrongDetail;
        auto* root=Read<uint8_t*>(detail+0x148);
        auto* render=Read<uint8_t*>(detail+0x1C8);
        state_.detail_active=detail[0x70];state_.detail_visible=root ? root[0x162]:0;
        state_.projection_mode=render ? render[0x260]:0;
        state_.pending_actions=Read<uint32_t>(detail+0x1DC);
        if(!root || !root[0x162] || !detail[0x70] || !render || !render[0x162] || !render[0x163] ||
                Read<int>(render+0x154)<=0 || Read<int>(render+0x158)<=0)return Waiting;
        const uint32_t form=Read<uint32_t>(detail+0x7C);
        if(form>1)return WrongDetail;
        void* slot=modelSlot_(render,form);
        void* model=slot ? entity_(slot):nullptr;
        if(!model)return Waiting;
        state_.detail=reinterpret_cast<uintptr_t>(detail);state_.model=reinterpret_cast<uintptr_t>(model);
        state_.form=form;state_.animation_state=Read<float>(detail+0x1E4);
        using Expected=const wchar_t*(__cdecl*)(uint32_t);
        auto module=GetModuleHandleW(L"capsule_catalog.dll");
        auto expected=module?reinterpret_cast<Expected>(GetProcAddress(module,"SdgoCollectionAcquisitionExpectedV1")):nullptr;
        const bool hinted=expected && expected(state_.unit);
        if(!hinted) {
            // A unit outside the loaded collection list (for example an owner
            // exclusion) keeps its original record text and panel strings.
            const uint8_t* index=nullptr;
            if(!state_.index_id || !find_(grid_(),&index,state_.index_id,0x55,0) || !index)return MissingIndex;
            wchar_t original[101]{};std::memcpy(original,index+214,200);
            if(wcsstr(original,L"收藏扭蛋"))return CollectionHintMismatch;
            for(unsigned slot=0;slot<10;++slot) {
                wchar_t name[64]{};swprintf_s(name,L"FinishedProduct%u",slot);
                auto* source=Resolve(name);
                if(!source || Read<uint32_t>(source+0x44)!=state_.unit)continue;
                const auto first=Read<uintptr_t>(source+0x178),last=Read<uintptr_t>(source+0x17C);
                if(!first || last<first || last-first>28*256 || (last-first)%28)continue;
                for(auto at=first;at<last;at+=28) {
                    const auto* value=reinterpret_cast<const uint8_t*>(at);
                    const auto length=Read<uint32_t>(value+16),capacity=Read<uint32_t>(value+20);
                    if(!length || length>512 || capacity<length)continue;
                    const auto* text=capacity<8?reinterpret_cast<const wchar_t*>(value):Read<const wchar_t*>(value);
                    if(text && wcsstr(text,L"收藏扭蛋"))return CollectionHintMismatch;
                }
            }
        }
        if(hinted) {
            // Observe the original FinishedProduct service's own copied event
            // strings after its native type-85 consumer populated the panel.
            bool copied=false;
            for(unsigned index=0;index<10;++index) {
                wchar_t name[64]{};swprintf_s(name,L"FinishedProduct%u",index);
                auto* source=Resolve(name);
                if(!source || Read<uint32_t>(source+0x44)!=state_.unit)continue;
                const auto first=Read<uintptr_t>(source+0x178),last=Read<uintptr_t>(source+0x17C);
                if(!first || last<first || last-first>28*256 || (last-first)%28)continue;
                for(auto at=first;at<last;at+=28) {
                    const auto* value=reinterpret_cast<const uint8_t*>(at);
                    const auto length=Read<uint32_t>(value+16),capacity=Read<uint32_t>(value+20);
                    if(!length || length>512 || capacity<length)continue;
                    const auto* text=capacity<8?reinterpret_cast<const wchar_t*>(value):Read<const wchar_t*>(value);
                    if(text && wcsstr(text,L"收藏扭蛋"))copied=true;
                }
            }
            if(!copied)return Waiting;
        }
        return Ready;
    }
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;HangarNavigation* navigation_=nullptr;
    void** manager_=nullptr;Service service_=nullptr;Ctor ctor_=nullptr;Dtor dtor_=nullptr;
    Control control_=nullptr;Edit edit_=nullptr;Grid grid_=nullptr;Find find_=nullptr;
    void* listVtable_=nullptr;void* detailVtable_=nullptr;
    Callback listCallback_=nullptr,detailCallback_=nullptr;ModelSlot modelSlot_=nullptr;Entity entity_=nullptr;Play play_=nullptr;
    Observation state_{};bool opened_=false,ranked_=false,searched_=false,closed_=false;uint32_t formPending_=0;
};
