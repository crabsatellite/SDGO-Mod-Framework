#include "blueprint_actions.h"

void* __cdecl BlueprintActions::ObserveEnqueue(void* source,int flags) {
    auto* owner=active_;if(!owner || !owner->enqueue_)return nullptr;
    bool target=false;
    __try {target=owner->synthArmed_ && source && Read<void*>(source)==owner->dialogVtable_;}
    __except(EXCEPTION_EXECUTE_HANDLER){target=false;}
    auto* live=owner->enqueue_(source,flags);
    __try {
        if(target && live && Read<void*>(live)==owner->dialogVtable_)
            InterlockedExchangePointer(&owner->liveDialog_,live);
    } __except(EXCEPTION_EXECUTE_HANDLER){}
    return live;
}

void __stdcall BlueprintActions::ObserveReply(uintptr_t a,uintptr_t b,uintptr_t c,uint32_t opcode,const uint8_t* packet) {
    auto* owner=active_;if(!owner || !owner->reply_)return;
    uint32_t unit=0;uint64_t uid=0;uint16_t slot=0;bool accepted=false;
    __try {
        if(owner->confirmed_ && opcode==0x7c9 && packet) {
            unit=Read<uint32_t>(packet+0x60);uid=Read<uint64_t>(packet+0x64);
            slot=Read<uint16_t>(packet+0x6c);accepted=true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&owner->replyError_,1);}
    // Completion is published only after the original consumer updates inventory.
    owner->reply_(a,b,c,opcode,packet);
    if(owner->confirmed_ && opcode==0x8bb)InterlockedExchange(&owner->replyError_,1);
    if(accepted) {
        owner->replyUnit_=unit;owner->replyUid_=uid;owner->replySlot_=slot;
        InterlockedIncrement(&owner->replySequence_);
    }
}

BlueprintActions::Result BlueprintActions::Synthesize() {
    __try {
        if(!config_.synthesize)return Invalid;
        if(InterlockedCompareExchange(&replyError_,0,0))return Invalid;
        if(!dialogRequested_) {
            const auto ready=SelectMaterials();if(ready!=Ready)return ready;
            auto* picker=Resolve(L"unitmgr");
            if(!picker || Read<void*>(picker)!=mixingVtable_)return ServiceMismatch;
            auto* root=Read<uint8_t*>(picker+0x64);
            auto* button=root ? Find(root,L"MixingBtn",4):nullptr;
            if(!button || !Match(root,button,L"MixingBtn",4))return ControlMismatch;
            if(!button[0x162] || !button[0x167])return Waiting;
            synthArmed_=true;state_.phase=8;
            mixingCallback_(picker,0x101,button+0x118,button);
            dialogRequested_=true;return Submitted;
        }
        if(!confirmed_) {
            auto* dialog=static_cast<uint8_t*>(InterlockedCompareExchangePointer(&liveDialog_,nullptr,nullptr));
            if(!dialog)return Waiting;
            if(Read<void*>(dialog)!=dialogVtable_)return ServiceMismatch;
            auto* button=Read<uint8_t*>(dialog+0x4c);
            auto* root=button ? Read<uint8_t*>(button+0x15c):nullptr;
            if(!root || !button)return Waiting;
            if(!Match(root,button,L"OkBtn",4)) {
                if(api_->Log)api_->Log(mod_,SDGO_LOG_ERROR,"blueprint confirm identity dialog=%p root=%p button=%p found=%p visible=%u enabled=%u",
                    dialog,root,button,Find(root,L"OkBtn",4),button[0x162],button[0x167]);
                return ControlMismatch;
            }
            if(!button[0x162] || !button[0x167])return Waiting;
            sendBaseline_=native_->TransportState().send_sequence;
            confirmed_=true;state_.phase=9;
            dialogCallback_(dialog,0x101,button+0x118,button);
            ++state_.confirms;
            InterlockedCompareExchangePointer(&liveDialog_,nullptr,dialog);
            return Submitted;
        }
        const auto send=native_->FindTransportSince(sendBaseline_,0x9ad);
        if(!send.send_sequence)return Waiting;
        state_.request_sequence=send.send_sequence;state_.request_size=send.packet_size;
        if(send.packet_size!=0x46 || !send.result)return Invalid;
        const auto replies=InterlockedCompareExchange(&replySequence_,0,0);
        if(!replies)return Waiting;
        state_.reply_sequence=replies;state_.output_unit=replyUnit_;
        state_.output_uid=replyUid_;state_.output_slot=replySlot_;
        if(replyUnit_!=config_.output_unit || !replyUid_ || !replySlot_ || !*info_)return Invalid;
        alignas(4) uint8_t brief[25]{};brief_(*info_,brief,replySlot_);
        if(Read<uint32_t>(brief)!=replyUnit_ || Read<uint64_t>(brief+4)!=replyUid_ || Read<uint16_t>(brief+12)!=replySlot_)return Waiting;
        state_.phase=10;return Ready;
    } __except(EXCEPTION_EXECUTE_HANDLER){return NativeFault;}
}

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

BlueprintActions::Result BlueprintActions::SelectMaterials() {
    __try {
        const auto selected=SelectBook();if(selected!=Ready)return selected;
        if(!config_.material_count || state_.material_count!=config_.material_count)return Invalid;
        const uint8_t* recipe=nullptr;auto* grid=gridData_();
        if(!grid || !gridFind_(grid,&recipe,config_.recipe,0x21,0) || !recipe)return Invalid;
        auto* profile=*profile_;if(!profile || !*info_)return Waiting;
        auto* info=static_cast<uint8_t*>(*info_);
        if(info[0x20]!=1)return Waiting;
        const auto capacity=Read<int32_t>(info+0x0c);
        if(capacity<=0 || capacity>8192)return Invalid;
        for(uint32_t i=0;i<config_.material_count;++i) {
            if(Read<uint32_t>(recipe+233+i*4)!=config_.material_units[i])return Invalid;
        }
        while(material_index_<config_.material_count) {
            state_.phase=6;state_.material_index=material_index_;
            const auto wanted=config_.material_uids[material_index_];
            const auto actual=Read<uint64_t>(profile+0x670+material_index_*8);
            state_.material_expected_uid=wanted;state_.material_actual_uid=actual;
            if(actual==wanted){++material_index_;material_pending_=false;continue;}
            if(material_pending_)return Waiting;
            uint16_t slot=0;uint32_t matches=0;
            for(int32_t index=1;index<=capacity;++index) {
                alignas(4) uint8_t brief[25]{};brief_(info,brief,static_cast<uint16_t>(index));
                if(Read<uint64_t>(brief+4)!=wanted)continue;
                if(Read<uint32_t>(brief)!=config_.material_units[material_index_] ||
                   Read<uint16_t>(brief+12)!=index)return Invalid;
                slot=static_cast<uint16_t>(index);++matches;
            }
            if(matches!=1 || !slot)return Invalid;
            // SelectBook replaces unitmgr with the original synthesis-specific
            // subclass (008C3F60 / 008C422E). mixingmgr is a different service.
            auto* picker=Resolve(L"unitmgr");if(!picker)return Waiting;
            if(Read<void*>(picker)!=mixingVtable_)return ServiceMismatch;
            auto* root=Read<uint8_t*>(picker+0x64);
            auto* list=Read<uint8_t*>(picker+0x1cc);
            if(!root || !root[0x162] || !list)return Waiting;
            if(!Match(root,list,L"UnitList",7))return ControlMismatch;
            state_.material_slot=slot;
            const auto page=Read<int32_t>(picker+0x74),maximum=Read<int32_t>(picker+0x78);
            const auto group=Read<int32_t>(picker+0x6c),maximumGroup=Read<int32_t>(picker+0x70);
            const auto target=static_cast<uint32_t>((slot-1)/6);
            state_.material_page=page>=0 ? static_cast<uint32_t>(page):~0u;
            if(page<0 || maximum<0 || page>maximum || target>static_cast<uint32_t>(maximum) ||
                group<0 || group>maximumGroup)return WrongPage;
            if(material_page_pending_) {
                if(static_cast<uint32_t>(page)==material_page_from_)return Waiting;
                material_page_pending_=false;
            }
            if(static_cast<uint32_t>(page)!=target) {
                const auto targetGroup=target/5;
                const wchar_t* anchor=targetGroup<static_cast<uint32_t>(group) ? L"LeftBtn":
                    targetGroup>static_cast<uint32_t>(group) ? L"RightBtn":
                    target<static_cast<uint32_t>(page) ? L"LeftBtn":L"RightBtn";
                const wchar_t* command=targetGroup!=static_cast<uint32_t>(group) ? anchor:
                    target<static_cast<uint32_t>(page) ? L"LeftOneBtn":L"RightOneBtn";
                auto* arrow=Find(root,anchor,4);
                if(!arrow || !Match(root,arrow,anchor,4))return ControlMismatch;
                if(!arrow[0x162] || !arrow[0x167])return Waiting;
                // Reuse the original unit-manager page business, as used by
                // UnitSelectActions: a real root-owned directional anchor and
                // the retained single-page command. No page/cache is written.
                alignas(4) uint8_t descriptor[24]{};bool constructed=false;
                __try {
                    constructed=ctor_(descriptor,command)!=nullptr;
                    if(!constructed)return NativeFault;
                    mixingCallback_(picker,0x101,descriptor,arrow);
                } __finally {if(constructed)dtor_(descriptor);}
                material_page_pending_=true;material_page_from_=page;
                ++state_.material_page_calls;return Submitted;
            }
            alignas(4) uint8_t full[153]{};full_(info,full,slot);
            if(Read<uint32_t>(full)!=config_.material_units[material_index_] ||
               Read<uint64_t>(full+8)!=wanted)return Waiting;
            auto* row=static_cast<uint8_t*>(item_(list,slot-1));if(!row)return Waiting;
            auto* visual=Read<uint8_t*>(row+0x1c);if(!visual)return ControlMismatch;
            const auto x=Read<float>(visual+0x148),y=Read<float>(visual+0x14c);
            if(!std::isfinite(x) || !std::isfinite(y) || std::fabs(x)>32760 || std::fabs(y)>32760)return Invalid;
            const auto px=static_cast<int32_t>(x)+2,py=static_cast<int32_t>(y)+2;
            if(!mouse_(list,WM_LBUTTONDOWN,px,py,0,0) || !mouse_(list,WM_LBUTTONUP,px,py,0,0) || !row[0x25])return ControlMismatch;
            // The native 0x701 handler reads this selected row and retrieves
            // its authentic 153-byte record itself. Never supply a unit buffer
            // or write the shared material identity pairs from the test.
            mixingCallback_(picker,0x701,list+0x118,list);
            ++state_.material_calls;material_pending_=true;return Submitted;
        }
        state_.phase=7;state_.material_index=material_index_;return Ready;
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
