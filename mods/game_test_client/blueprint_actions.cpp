#include "blueprint_actions.h"

BlueprintActions::Result BlueprintActions::EnsureBook() {
        __try {
            state_.phase=1;
            if(!opened_) {
                const auto ready=nav_->EnsureHangar();
                if(ready>=HangarNavigation::kDownmenuServiceMismatch)return Invalid;
                if(ready!=HangarNavigation::kReady)return Waiting;
                // UnitMixingBtn belongs to the original unitmgr page. Its
                // verified callback owns the entry transition to formmixing.
                auto* unit=Resolve(L"unitmgr");
                if(!unit)return Waiting;
                if(Read<void*>(unit)!=unitVtable_)return ServiceMismatch;
                auto* root=Read<uint8_t*>(unit+0x64);
                if(!root || !root[0x162])return Waiting;
                auto* button=Find(root,L"UnitMixingBtn",4);
                if(!button || !Match(root,button,L"UnitMixingBtn",4) || !button[0x162] || !button[0x167])return ControlMismatch;
                wcscpy_s(state_.entry_control,L"UnitMixingBtn");
                unitCallback_(unit,0x101,button+0x118,button);
                opened_=true;return Submitted;
            }
            // The original UnitMixingBtn transition enters the blueprint
            // application scene (12), rather than staying in My Room (5).
            if(native_->AppState()!=12)return Waiting;
            state_.phase=2;
            auto* form=Resolve(L"formmixing");state_.form=reinterpret_cast<uintptr_t>(form);
            if(!form)return Waiting;
            if(Read<void*>(form)!=formVtable_)return ServiceMismatch;
            auto* root=Read<uint8_t*>(form+0x64);
            state_.visible=root ? root[0x162]:0;
            if(!root || !state_.visible)return Waiting;
            auto* list=Read<uint8_t*>(form+0x68);state_.list=reinterpret_cast<uintptr_t>(list);
            if(!list)return Waiting;
            const auto begin=Read<uintptr_t>(list+0x244),end=Read<uintptr_t>(list+0x248);
            if(!begin || !end || begin==end)return Waiting;
            if(end<begin || (end-begin)%4 || (end-begin)/4>4096)return Invalid;
            state_.rows=static_cast<uint32_t>((end-begin)/4);state_.page=Read<uint32_t>(form+0x12c);
            uint8_t* selected=nullptr;uint32_t selectedIndex=0,matches=0;
            for(uint32_t i=0;i<state_.rows;++i) {
                auto* row=static_cast<uint8_t*>(item_(list,static_cast<int>(i)));
                auto* book=row ? Read<uint8_t*>(row+0x2c):nullptr;
                if(book && Read<uint32_t>(book+8)==config_.recipe && Read<uint64_t>(book)==config_.book) {
                    selected=row;selectedIndex=i;++matches;
                }
            }
            if(!matches)return Waiting;
            if(matches!=1)return WrongBook;
            state_.row_index=selectedIndex;
            if(selectedIndex/6!=state_.page)return WrongPage;
            if(!selected[0x25]) {
                if(state_.mouse_calls)return Waiting;
                auto* visual=Read<uint8_t*>(selected+0x1c);
                if(!visual)return Waiting;
                const float x=Read<float>(visual+0x148),y=Read<float>(visual+0x14c);
                if(!std::isfinite(x) || !std::isfinite(y) || std::fabs(x)>32760 || std::fabs(y)>32760)return Invalid;
                const auto px=static_cast<int32_t>(x)+2,py=static_cast<int32_t>(y)+2;
                const auto down=mouse_(list,WM_LBUTTONDOWN,px,py,0,0);
                const auto up=down ? mouse_(list,WM_LBUTTONUP,px,py,0,0):0;
                ++state_.mouse_calls;
                if(!down || !up || !selected[0x25])return ControlMismatch;
            }
            for(uint32_t i=state_.page*6;i<state_.rows && i<(state_.page+1)*6;++i) {
                auto* row=static_cast<uint8_t*>(item_(list,static_cast<int>(i)));
                if(row && (row[0x25]!=0)!=(i==selectedIndex))return WrongBook;
            }
            state_.phase=3;return Ready;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }

BlueprintActions::Result BlueprintActions::SelectBook() {
        __try {
            state_.phase=4;
            if(state_.book_calls) {
                if(!*profile_)return Waiting;
                auto* profile=*profile_;
                state_.selected_book=Read<uint64_t>(profile+0x6d4);
                const auto recipeId=Read<uint32_t>(profile+0x6dc);
                if(state_.selected_book!=config_.book || recipeId!=config_.recipe)return WrongBook;
                if(Read<uint32_t>(profile+0x6cc)!=config_.recipe)return Waiting;
                auto* grid=gridData_();if(!grid)return Waiting;
                const uint8_t* recipe=nullptr;
                if(!gridFind_(grid,&recipe,recipeId,0x21,0) || !recipe)return Waiting;
                if(Read<uint32_t>(recipe)!=config_.recipe)return WrongBook;
                state_.material_count=Read<uint32_t>(recipe+105);
                state_.mixing=reinterpret_cast<uintptr_t>(Resolve(L"mixingmgr"));
                state_.slots=reinterpret_cast<uintptr_t>(Resolve(L"mixingslot"));
                if(state_.material_count<3 || state_.material_count>5)return Invalid;
                if(!state_.mixing || !state_.slots)return Waiting;
                state_.phase=5;return Ready;
            }
            const auto ready=EnsureBook();if(ready!=Ready)return ready;
            state_.phase=4;
            auto* form=reinterpret_cast<uint8_t*>(state_.form);
            auto* root=Read<uint8_t*>(form+0x64);auto* button=Read<uint8_t*>(form+0x114);
            if(!button)return Waiting;
            // The callback selects its +0x114 control by pointer identity.
            // "SelectBookBtn" is a diagnostic label, not its shipped UI name.
            auto* descriptor=button+0x118;
            const auto length=Read<uint32_t>(descriptor+0x10),capacity=Read<uint32_t>(descriptor+0x14);
            if(!length || length>=48 || length>capacity)return ControlMismatch;
            const auto* text=capacity<=7 ? reinterpret_cast<const wchar_t*>(descriptor):Read<const wchar_t*>(descriptor);
            if(!text)return ControlMismatch;
            wchar_t name[48]{};memcpy(name,text,length*sizeof(wchar_t));
            if(!Match(root,button,name,4))return ControlMismatch;
            if(!button[0x162] || !button[0x167])return Waiting;
            if(api_->Log)api_->Log(mod_,SDGO_LOG_INFO,"original blueprint select control=%ls pointer=%p",name,button);
            formCallback_(form,0x101,button+0x118,button);++state_.book_calls;return Submitted;
        } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
    }
