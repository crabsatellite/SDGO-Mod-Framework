#pragma once

#include "native_actions.h"
#include "sdgo/game_test.h"
#include "sdgo/room_titles_current.h"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>
#include <intrin.h>

// Drives live registered services and their original UI callbacks. Text enters
// the game's actual shared Win32 EDIT via WM_CHAR/EM_REPLACESEL; no SetText,
// input-limit write, native room write, packet synthesis or fake reply is used.
class RoomTitleActions {
public:
    enum Surface { PvpCreate, PvpRename, MissionCreate, MissionSettings, MissionRename, PvpGridCreate, PvpTeamCreate, TeamSettings, TeamRename };
    bool Bind(const SdgoApi* api,SdgoMod* mod,NativeActions* actions) {
        api_=api;mod_=mod;actions_=actions;
        manager_=Symbol<void**>("flow.service_managed_pointer");
        find_=Symbol<FindControl>("room_titles.ui_find_control");
        get_=Symbol<GetText>("room_titles.ui_edit_get_text");
        focus_=Symbol<Focus>("title_test.dialog_focus");
        notify_=Symbol<Notify>("title_test.dialog_notify");
        input_=Symbol<uint8_t**>("room_titles.ui_input_context");
        if(!(manager_&&find_&&get_&&focus_&&notify_&&input_))return false;
        active_=this;
        return api_->HookCode(mod_,"room_titles.ui_input_limit",reinterpret_cast<void*>(&ObserveInputLimit),reinterpret_cast<void**>(&inputLimit_)) &&
            api_->HookCode(mod_,"title_test.edit_input_focus",reinterpret_cast<void*>(&ObserveEditInputFocus),reinterpret_cast<void**>(&editInputFocus_)) &&
            api_->HookCode(mod_,"title_test.key_pressed",reinterpret_cast<void*>(&TitleKeyPulse),reinterpret_cast<void**>(&keyPressed_));
    }
    void Reset() { phase_=0;inputIndex_=0;opened_=false;reopened_=false;dialog_=nullptr;edit_=nullptr;uiRestarts_=0;pendingTitleKey_=0; }
    int Poll(Surface surface,const SdgoGameTestContextV1* context) {
        try { return Step(surface,context); }
        catch(const std::exception& error){context->Assert(0,error.what());return SDGO_GAME_TEST_FAIL;}
    }
    // Generic callbacks consume the control's real native name and identity.
    bool Click(void* service,void* root,const wchar_t* name,unsigned kind=4) {
        void* control=Control(root,name,kind);
        if(!control)return false;
        auto* bytes=static_cast<uint8_t*>(control);
        if(!bytes[0x162]||!bytes[0x167]||*reinterpret_cast<void**>(bytes+0x15C)!=root)return false;
        if(!static_cast<uint8_t*>(root)[0x162])return false;
        (void)service;
        notify_(root,kind==12?0x301:0x101,1,control);
        return true;
    }
    void* Service(const char* symbol) const {return FindService(api_->Symbol(mod_,symbol));}
    void* Root(void* service,size_t offset)const {return service?*reinterpret_cast<void**>(static_cast<uint8_t*>(service)+offset):nullptr;}
    bool MissionEntry() {
        auto* lobby=Service("title_test.mission_lobby_vtable");
        if(lobby)return true;
        auto* top=Service("flow.lobby_ui_vtable");
        if(top&&!opened_&&Click(top,Root(top,0x44),L"SingleBtn"))opened_=true;
        return false;
    }
    bool Leave(const SdgoGameTestContextV1* context) {
        if(actions_->AppState()==6)return true;
        // These are verified room-header roots, not arbitrary readable fields
        // in the bottom unit/start menu (whose +0x4C is a static control).
        for(const auto& entry:std::array<std::pair<const char*,size_t>,3>{{
            {"title_test.team_titlebar_vtable",0x48},{"title_test.coop_titlebar_vtable",0x44},
            {"room_titles.ui_titlebar_vtable",0x48}}}) {
            void* owner=Service(entry.first);void* root=Root(owner,entry.second);
            if(Click(owner,root,L"ExitBtn")){context->Observe("clicked native room-header exit control");return false;}
        }
        return false;
    }
private:
    struct WString {union {wchar_t inlineText[8];const wchar_t* pointer;};uint32_t size,capacity;};
    static_assert(sizeof(WString)==24 && sizeof(wchar_t)==2,"native title string layout changed");
    using FindControl=void*(__thiscall*)(void*,const WString*,int);
    using GetText=const WString*(__thiscall*)(void*);
    using Focus=void(__thiscall*)(void*,void*);
    using Notify=void(__thiscall*)(void*,uint32_t,int,void*);
    using InputLimit=void(__thiscall*)(void*,uint32_t,int);
    using EditInputFocus=void(__thiscall*)(void*);
    static void __fastcall ObserveEditInputFocus(void* edit,void*) {
        auto* owner=active_;owner->editInputFocus_(edit);
        if(Readable(edit,0x168)&&static_cast<uint8_t*>(edit)[0x165])owner->actualInputEdit_=edit;
    }
    using KeyPressed=bool(__cdecl*)(uint32_t);
    static bool __cdecl TitleKeyPulse(uint32_t scan) {
        auto* owner=active_;const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
        // The shipped inline title edit has no buttons. Deliver one requested
        // Enter/Escape pulse only to its pinned native Update input query.
        // All other services and keys retain their real keyboard result.
        if(owner->pendingTitleKey_==scan && ((scan==0x1C&&caller==0x00A46A5F)||(scan==1&&caller==0x00A46AF8)) &&
           owner->dialog_==owner->Service("title_test.rename_vtable")){
            owner->pendingTitleKey_=0;return true;
        }
        return owner->keyPressed_(scan);
    }
    static void __fastcall ObserveInputLimit(void* self,void*,uint32_t units,int clear) {
        auto* owner=active_;
        owner->lastLimitCaller_=reinterpret_cast<uintptr_t>(_ReturnAddress());
        owner->lastLimit_=units;owner->lastClear_=clear;
        // Passive receipt only. This observer never changes a requested limit.
        owner->inputLimit_(self,units,clear);
    }
    bool MessengerReset()const {
        const bool otherEdit=edit_&&Readable(edit_,0x168)&&static_cast<uint8_t*>(edit_)[0x165]&&
            actualInputEdit_&&actualInputEdit_!=edit_;
        return (lastLimitCaller_==0x0096200Bu&&lastLimit_==39&&lastClear_!=0)||otherEdit;
    }
    template<class T>T Symbol(const char* name){return reinterpret_cast<T>(api_->Symbol(mod_,name));}
    static bool Readable(const void* address,size_t size) {
        if(!address)return false;MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(address,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
        const auto start=reinterpret_cast<uintptr_t>(address);const auto end=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
        return start<=end&&size<=end-start;
    }
    void* FindService(void* table)const {
        if(!table||!manager_||!*manager_)return nullptr;
        auto* owner=static_cast<uint8_t*>(*manager_);
        auto* head=*reinterpret_cast<uint8_t**>(owner+0x44);
        if(!Readable(head,12))return nullptr;
        auto* node=*reinterpret_cast<uint8_t**>(head);
        void* found=nullptr;
        for(unsigned n=0;node&&node!=head&&n<512;++n){
            if(!Readable(node,12))return nullptr;
            void* service=*reinterpret_cast<void**>(node+8);
            if(Readable(service,4)&&*reinterpret_cast<void**>(service)==table){if(found)return nullptr;found=service;}
            node=*reinterpret_cast<uint8_t**>(node);
        }
        return found;
    }
    void DumpServices(const SdgoGameTestContextV1* c)const {
        if(!manager_||!*manager_)return;
        auto* head=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(*manager_)+0x44);
        if(!Readable(head,12))return;
        auto* node=*reinterpret_cast<uint8_t**>(head);
        for(unsigned i=0;node&&node!=head&&i<128;++i){
            if(!Readable(node,12))break;
            auto* service=*reinterpret_cast<uint8_t**>(node+8);
            if(Readable(service,0x50)){
                char line[128];sprintf_s(line,"live service vtable=%08X",*reinterpret_cast<uint32_t*>(service));c->Observe(line);
            }
            node=*reinterpret_cast<uint8_t**>(node);
        }
    }
    void* Control(void* root,const wchar_t* name,unsigned kind)const {
        if(!Readable(root,0x238))return nullptr;
        WString view{};view.pointer=name;view.size=static_cast<uint32_t>(wcslen(name));view.capacity=view.size;
        if(view.size<8){wcscpy_s(view.inlineText,name);view.capacity=7;}
        return find_(root,&view,static_cast<int>(kind));
    }
    static std::wstring Text(const WString* value) {
        if(!Readable(value,sizeof(*value))||value->size>256||value->capacity<value->size)throw std::runtime_error("invalid live edit string");
        const wchar_t* chars=value->capacity<8?value->inlineText:value->pointer;
        if(!Readable(chars,(value->size+1)*2))throw std::runtime_error("unreadable live edit text");
        return std::wstring(chars,value->size);
    }
    void DumpControls(const SdgoGameTestContextV1* c,void* root)const {
        if(!Readable(root,0x240))return;
        auto* head=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(root)+0x23C);
        if(!Readable(head,12))return;
        auto* node=*reinterpret_cast<uint8_t**>(head);
        for(unsigned i=0;node&&node!=head&&i<96;++i){
            if(!Readable(node,12))break;
            auto* control=*reinterpret_cast<uint8_t**>(node+8);
            if(Readable(control,0x168)){
                try{
                    const auto name=Text(reinterpret_cast<const WString*>(control+0x118));
                    if(!name.empty()){
                        std::string ascii;for(auto ch:name)ascii+=ch>=32&&ch<127?static_cast<char>(ch):'?';
                        c->Observe(("native room-title control="+ascii).c_str());
                    }
                }catch(const std::exception&){}
            }
            node=*reinterpret_cast<uint8_t**>(node);
        }
    }
    std::string Utf8(const std::wstring& text)const {
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
        std::string result(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);return result;
    }
    static bool IsPvpCreate(Surface s){return s==PvpCreate||s==PvpGridCreate;}
    static bool IsVsCreate(Surface s){return IsPvpCreate(s)||s==PvpTeamCreate;}
    static bool IsTeamSettings(Surface s){return s==TeamSettings||s==TeamRename;}
    static const char* DialogSymbol(Surface s){return IsTeamSettings(s)?"title_test.team_settings_vtable": s==PvpTeamCreate?"title_test.team_create_vtable":s==PvpGridCreate?"title_test.grid_create_vtable":s==PvpCreate?"title_test.pvp_create_vtable":s==MissionCreate?"title_test.mission_create_vtable":s==MissionSettings?"title_test.mission_settings_vtable":"title_test.rename_vtable";}
    void Observe(const SdgoGameTestContextV1* c,const char* stage,Surface s)const {
        char line[192];sprintf_s(line,"room-title surface=%u stage=%s app=%u phase=%u input=%u",s,stage,actions_->AppState(),phase_,inputIndex_);c->Observe(line);
    }
    bool Open(Surface s) {
        if(IsVsCreate(s)){
            void* list=Service("flow.native_room_list_vtable");if(!list)return false;
            const uint32_t mode=s==PvpTeamCreate?0:s==PvpGridCreate?2:1;
            auto* current=Symbol<const uint32_t*>("title_test.room_list_mode");
            if(!current)return false;
            if(*current!=mode){
                void* radio=Root(list,mode==0?0x64:mode==1?0x68:0x6C);
                Click(list,Root(radio,0x15C),mode==0?L"Radio1":mode==1?L"Radio2":L"Radio3",12);
                return false; // let the original list request/UI transition finish
            }
            void* button=Root(list,s==PvpTeamCreate?0x88:0x84);
            return Click(list,Root(button,0x15C),s==PvpTeamCreate?L"MakeTeamBtn":L"MakeRoomBtn");
        }
        if(s==MissionCreate){
            void* lobby=Service("title_test.mission_lobby_vtable");void* root=Root(lobby,0x60);
            if(lobby&&static_cast<uint8_t*>(lobby)[0x64]!=3)return Click(lobby,root,L"MultiRadio",12);
            return Click(lobby,root,L"MakeRoomBtn");
        }
        if(s==MissionSettings||IsTeamSettings(s)){
            void* bar=Service(IsTeamSettings(s)?"title_test.team_titlebar_vtable":"title_test.coop_titlebar_vtable");
            if(Click(bar,Root(bar,IsTeamSettings(s)?0x48:0x44),L"SettingBtn"))return true;
            bar=Service("room_titles.ui_titlebar_vtable");
            return Click(bar,Root(bar,0x48),L"OptionBtn");
        }
        void* bar=Service("room_titles.ui_titlebar_vtable");return Click(bar,Root(bar,0x48),L"TitleChangeBtn");
    }
    bool Close(Surface s) {
        if(s==PvpRename||s==MissionRename){pendingTitleKey_=1;return true;}
        void* root=Root(dialog_,IsTeamSettings(s)?0x7C:s==MissionSettings?0x5C:IsVsCreate(s)?0x50:0x4C);
        for(const auto* name:{L"MakeCancelBtnW",L"SettingCancelBtn",L"CancelBtn",L"TitleCancelBtn",L"CloseBtnW",L"CloseBtn"})if(Click(dialog_,root,name))return true;
        return false;
    }
    bool Submit(Surface s) {
        if(s==PvpRename||s==MissionRename){pendingTitleKey_=0x1C;return true;}
        void* root=Root(dialog_,IsTeamSettings(s)?0x7C:s==MissionSettings?0x5C:IsVsCreate(s)?0x50:0x4C);
        for(const auto* name:{L"SettingOkBtn",L"OkBtn",L"TitleOkBtn",L"ConfirmBtn"})if(Click(dialog_,root,name))return true;
        return false;
    }
    int Step(Surface s,const SdgoGameTestContextV1* c) {
        if(phase_==5){
            if(Service(DialogSymbol(s)))return SDGO_GAME_TEST_WAIT;
            phase_=0;inputIndex_=0;opened_=false;reopened_=false;dialog_=nullptr;edit_=nullptr;
            return SDGO_GAME_TEST_WAIT;
        }
        if((phase_==1||phase_==2)&&MessengerReset()){
            if(++uiRestarts_>2||!Close(s))throw std::runtime_error("unrelated native Messenger initialization repeatedly interrupted room input");
            c->Observe("another original edit claimed the shared native input; cancel/reopen the room dialog and repeat every input assertion");
            phase_=5;return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==0){
            if(s==MissionCreate&&!Service("title_test.mission_lobby_vtable")){MissionEntry();return SDGO_GAME_TEST_WAIT;}
            void* live=Service(DialogSymbol(s));
            if(!live){
                if(!opened_ || s==MissionCreate || s==MissionSettings || IsTeamSettings(s) || s==PvpRename || s==MissionRename){
                    if(Open(s)){opened_=true;Observe(c,"open-requested",s);}
                }
                if(++polls_%300==0){Observe(c,"waiting-for-native-service",s);DumpServices(c);}
                return SDGO_GAME_TEST_WAIT;
            }
            dialog_=live;void* root=Root(live,IsTeamSettings(s)?0x7C:s==MissionSettings?0x5C:IsVsCreate(s)?0x50:0x4C);
            // Registration/Init precedes the native state-3 update that creates
            // controls, fills defaults and shows this dialog. Observe readiness.
            if(IsPvpCreate(s)&&(!Readable(live,0x48)||*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(live)+0x44)<4))return SDGO_GAME_TEST_WAIT;
            if(!Readable(root,0x238)||!static_cast<uint8_t*>(root)[0x162])return SDGO_GAME_TEST_WAIT;
            edit_=IsPvpCreate(s)?Root(live,0x54):s==PvpRename||s==MissionRename?Root(live,0x50):Control(root,L"GameRoomNameEdit",8);
            if(!Readable(edit_,0x2A4)||*reinterpret_cast<void**>(edit_)!=api_->Symbol(mod_,"room_titles.ui_edit_vtable"))throw std::runtime_error("expected live room-name edit is absent");
            if(!static_cast<uint8_t*>(edit_)[0x162]||!static_cast<uint8_t*>(edit_)[0x167])return SDGO_GAME_TEST_WAIT;
            c->Assert(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(edit_)+0x2A0)==512,"room-name edit exposes the extended UTF-16 byte limit");
            if(static_cast<uint8_t*>(edit_)[0x165]&&actualInputEdit_==edit_&&!MessengerReset()) {
                HWND inputWindow=*reinterpret_cast<HWND*>(*input_+0x134);
                c->Assert(SendMessageW(inputWindow,EM_GETLIMITTEXT,0,0)>=128,"initially focused room edit already has the expanded real input limit");
            }
            // The shipped UI uses CUIDialog::SetFocus followed by the edit's
            // input-focus method (for example at 007B2A29/007B2A37). The focus
            // flag alone does not bind the shared IME context to this root.
            focus_(root,edit_);ObserveEditInputFocus(edit_,nullptr);
            tests_={std::wstring(21,L'A'),L"\u4efb\u52a1\u623f\u95f4\u540d\u79f0"+std::wstring(40,L'\u4e2d'),std::wstring(128,L'\u623f')};
            tests_.back()[0]=static_cast<wchar_t>(0x623F+s);
            if(!reopened_){std::array<char,513> before{};const auto n=sdgo::CurrentRoomTitle(api_->ModDirectory(mod_),before.data(),static_cast<uint32_t>(before.size()));beforeCancel_.assign(before.data(),n);}
            phase_=1;Observe(c,"live-dialog-ready",s);DumpControls(c,root);return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==1){
            if(!static_cast<uint8_t*>(edit_)[0x165])throw std::runtime_error("room edit did not receive native focus");
            ObserveEditInputFocus(edit_,nullptr);
            HWND hwnd=*reinterpret_cast<HWND*>(*input_+0x134);
            if(!IsWindow(hwnd))throw std::runtime_error("native input HWND absent");
            SendMessageW(hwnd,EM_SETSEL,0,-1);
            SendMessageW(hwnd,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L""));
            if(inputIndex_==0){for(wchar_t ch:tests_[0])SendMessageW(hwnd,WM_CHAR,ch,0);}
            else SendMessageW(hwnd,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(tests_[inputIndex_].c_str()));
            phase_=2;polls_=0;return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==2){
            if(Text(get_(edit_))!=tests_[inputIndex_]){
                if(++polls_<120)return SDGO_GAME_TEST_WAIT;
                HWND hwnd=*reinterpret_cast<HWND*>(*input_+0x134);
                char line[240];sprintf_s(line,"input lengths: expected=%zu native_edit=%zu hwnd=%d hwnd_limit=%lld edit_limit=%u setter=%08X requested=%u clear=%d",tests_[inputIndex_].size(),Text(get_(edit_)).size(),GetWindowTextLengthW(hwnd),static_cast<long long>(SendMessageW(hwnd,EM_GETLIMITTEXT,0,0)),*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(edit_)+0x2A0),static_cast<unsigned>(lastLimitCaller_),lastLimit_,lastClear_);c->Observe(line);
                throw std::runtime_error("typed/pasted title was truncated or absent in the real native edit");
            }
            c->Assert(1,inputIndex_==0?"21 keyboard characters survive in the real game edit":inputIndex_==1?"Chinese title beyond twenty characters survives native input":"128 Chinese characters survive native input without truncation");
            if(++inputIndex_<tests_.size()){phase_=1;return SDGO_GAME_TEST_WAIT;}
            if(!reopened_){if(!Close(s))throw std::runtime_error("native cancel control unavailable");cancelAt_=GetTickCount64();phase_=3;Observe(c,"cancel-requested",s);return SDGO_GAME_TEST_WAIT;}
            expected_=Utf8(tests_.back());
            if(!Submit(s))throw std::runtime_error("native title confirmation control unavailable");
            phase_=4;Observe(c,"submit-requested",s);return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==3){
            if(Service(DialogSymbol(s))||GetTickCount64()-cancelAt_<500)return SDGO_GAME_TEST_WAIT;
            std::array<char,513> current{};const auto n=sdgo::CurrentRoomTitle(api_->ModDirectory(mod_),current.data(),static_cast<uint32_t>(current.size()));
            c->Assert(std::string(current.data(),n)==beforeCancel_,"native cancel preserves the previously confirmed full room title");
            reopened_=true;opened_=false;inputIndex_=0;phase_=0;Observe(c,"cancel-consumed-reopen",s);return SDGO_GAME_TEST_WAIT;
        }
        std::array<char,513> confirmed{};
        const uint32_t length=sdgo::CurrentRoomTitle(api_->ModDirectory(mod_),confirmed.data(),static_cast<uint32_t>(confirmed.size()));
        if(length!=expected_.size()||std::string(confirmed.data())!=expected_)return SDGO_GAME_TEST_WAIT;
        c->Assert(1,"server-confirmed current title exactly matches the complete 128-character native input");
        Observe(c,"server-confirmed",s);Reset();return SDGO_GAME_TEST_PASS;
    }
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;NativeActions* actions_=nullptr;
    void** manager_=nullptr;FindControl find_=nullptr;GetText get_=nullptr;Focus focus_=nullptr;Notify notify_=nullptr;uint8_t** input_=nullptr;
    inline static RoomTitleActions* active_=nullptr;InputLimit inputLimit_=nullptr;
    EditInputFocus editInputFocus_=nullptr;void* actualInputEdit_=nullptr;
    uintptr_t lastLimitCaller_=0;uint32_t lastLimit_=0;int lastClear_=0;unsigned uiRestarts_=0;
    KeyPressed keyPressed_=nullptr;uint32_t pendingTitleKey_=0;
    unsigned phase_=0,inputIndex_=0,polls_=0;bool opened_=false,reopened_=false;void* dialog_=nullptr;void* edit_=nullptr;
    std::vector<std::wstring> tests_;std::string expected_,beforeCancel_;uint64_t cancelAt_=0;
};
