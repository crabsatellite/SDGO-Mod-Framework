#pragma once

#include "native_actions.h"
#include "sdgo/game_test.h"
#include "sdgo/room_titles_current.h"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

// Drives live registered services and their original UI callbacks. Text enters
// the game's actual shared Win32 EDIT via WM_CHAR/EM_REPLACESEL; no SetText,
// input-limit write, native room write, packet synthesis or fake reply is used.
class RoomTitleActions {
public:
    enum Surface { PvpCreate, PvpRename, MissionCreate, MissionSettings, MissionRename };
    bool Bind(const SdgoApi* api,SdgoMod* mod,NativeActions* actions) {
        api_=api;mod_=mod;actions_=actions;
        manager_=Symbol<void**>("flow.service_managed_pointer");
        find_=Symbol<FindControl>("room_titles.ui_find_control");
        get_=Symbol<GetText>("room_titles.ui_edit_get_text");
        focus_=Symbol<Focus>("title_test.dialog_focus");
        input_=Symbol<uint8_t**>("room_titles.ui_input_context");
        return manager_&&find_&&get_&&focus_&&input_;
    }
    void Reset() { phase_=0;inputIndex_=0;opened_=false;reopened_=false;dialog_=nullptr;edit_=nullptr; }
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
        auto table=*reinterpret_cast<void***>(service);
        reinterpret_cast<Callback>(table[0x38/4])(service,kind==12?0x301:0x101,bytes+0x118,control);
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
        void* menu=Service("flow.wait_menu_vtable");
        for(size_t off:{0x44u,0x48u,0x4Cu}) {
            void* root=Root(menu,off);
            if(!Readable(root,0x238))continue;
            for(const auto* name:{L"ExitBtn",L"ExitRoomBtn",L"LeaveRoomBtn",L"RoomOutBtn"})
                if(Click(menu,root,name)){context->Observe("clicked native room-exit control");return false;}
        }
        return false;
    }
private:
    struct WString {union {wchar_t inlineText[8];const wchar_t* pointer;};uint32_t size,capacity;};
    static_assert(sizeof(WString)==24 && sizeof(wchar_t)==2,"native title string layout changed");
    using FindControl=void*(__thiscall*)(void*,const WString*,int);
    using GetText=const WString*(__thiscall*)(void*);
    using Focus=void(__thiscall*)(void*,void*);
    using Callback=uint32_t(__stdcall*)(void*,uint32_t,const void*,void*);
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
                char line[128];sprintf_s(line,"live service vtable=%08X root44=%08X root4C=%08X",*reinterpret_cast<uint32_t*>(service),*reinterpret_cast<uint32_t*>(service+0x44),*reinterpret_cast<uint32_t*>(service+0x4C));c->Observe(line);
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
    std::string Utf8(const std::wstring& text)const {
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
        std::string result(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);return result;
    }
    static const char* DialogSymbol(Surface s){return s==PvpCreate?"title_test.pvp_create_vtable":s==MissionCreate?"title_test.mission_create_vtable":s==MissionSettings?"title_test.mission_settings_vtable":"title_test.rename_vtable";}
    void Observe(const SdgoGameTestContextV1* c,const char* stage,Surface s)const {
        char line[192];sprintf_s(line,"room-title surface=%u stage=%s app=%u phase=%u input=%u",s,stage,actions_->AppState(),phase_,inputIndex_);c->Observe(line);
    }
    bool Open(Surface s) {
        if(s==PvpCreate){void* list=Service("flow.native_room_list_vtable");return Click(list,Root(list,0x44),L"MakeRoomBtn");}
        if(s==MissionCreate){
            void* lobby=Service("title_test.mission_lobby_vtable");void* root=Root(lobby,0x60);
            if(lobby&&static_cast<uint8_t*>(lobby)[0x64]!=3)return Click(lobby,root,L"MultiRadio",12);
            return Click(lobby,root,L"MakeRoomBtn");
        }
        if(s==MissionSettings){
            void* menu=Service("flow.wait_menu_vtable");
            for(size_t off:{0x44u,0x48u,0x4Cu}){void* root=Root(menu,off);if(Click(menu,root,L"SettingBtn"))return true;}
            return false;
        }
        void* bar=Service("room_titles.ui_titlebar_vtable");return Click(bar,Root(bar,0x48),L"TitleChangeBtn");
    }
    bool Close(Surface s) {
        void* root=Root(dialog_,s==MissionSettings?0x5C:0x4C);
        for(const auto* name:{L"MakeCancelBtnW",L"SettingCancelBtn",L"CancelBtn",L"TitleCancelBtn",L"CloseBtn"})if(Click(dialog_,root,name))return true;
        return false;
    }
    bool Submit(Surface s) {
        void* root=Root(dialog_,s==MissionSettings?0x5C:0x4C);
        for(const auto* name:{L"SettingOkBtn",L"OkBtn",L"TitleOkBtn",L"ConfirmBtn"})if(Click(dialog_,root,name))return true;
        return false;
    }
    int Step(Surface s,const SdgoGameTestContextV1* c) {
        if(phase_==0){
            if(s==MissionCreate&&!Service("title_test.mission_lobby_vtable")){MissionEntry();return SDGO_GAME_TEST_WAIT;}
            void* live=Service(DialogSymbol(s));
            if(!live){
                if(!opened_ || s==MissionCreate || s==MissionSettings || s==PvpRename || s==MissionRename){
                    if(Open(s)){opened_=true;Observe(c,"open-requested",s);}
                }
                if(++polls_%300==0){Observe(c,"waiting-for-native-service",s);DumpServices(c);}
                return SDGO_GAME_TEST_WAIT;
            }
            dialog_=live;void* root=Root(live,s==MissionSettings?0x5C:0x4C);
            edit_=s==PvpRename||s==MissionRename?Root(live,0x50):Control(root,L"GameRoomNameEdit",8);
            if(!Readable(edit_,0x2A4)||*reinterpret_cast<void**>(edit_)!=api_->Symbol(mod_,"room_titles.ui_edit_vtable"))throw std::runtime_error("expected live room-name edit is absent");
            c->Assert(*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(edit_)+0x2A0)==512,"room-name edit exposes the extended UTF-16 byte limit");
            if(static_cast<uint8_t*>(edit_)[0x165]) {
                HWND inputWindow=*reinterpret_cast<HWND*>(*input_+0x134);
                c->Assert(SendMessageW(inputWindow,EM_GETLIMITTEXT,0,0)>=128,"initially focused room edit already has the expanded real input limit");
            }
            else focus_(root,edit_);
            tests_={std::wstring(21,L'A'),L"\u4efb\u52a1\u623f\u95f4\u540d\u79f0"+std::wstring(40,L'\u4e2d'),std::wstring(128,L'\u623f')};
            tests_.back()[0]=static_cast<wchar_t>(L'A'+s);
            phase_=1;Observe(c,"live-dialog-ready",s);return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==1){
            if(!static_cast<uint8_t*>(edit_)[0x165])throw std::runtime_error("room edit did not receive native focus");
            HWND hwnd=*reinterpret_cast<HWND*>(*input_+0x134);
            if(!IsWindow(hwnd))throw std::runtime_error("native input HWND absent");
            SendMessageW(hwnd,EM_SETSEL,0,-1);
            SendMessageW(hwnd,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L""));
            if(inputIndex_==0){for(wchar_t ch:tests_[0])SendMessageW(hwnd,WM_CHAR,ch,0);}
            else SendMessageW(hwnd,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(tests_[inputIndex_].c_str()));
            phase_=2;polls_=0;return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==2){
            if(Text(get_(edit_))!=tests_[inputIndex_]){if(++polls_<120)return SDGO_GAME_TEST_WAIT;throw std::runtime_error("typed/pasted title was truncated or absent in the real native edit");}
            c->Assert(1,inputIndex_==0?"21 keyboard characters survive in the real game edit":inputIndex_==1?"Chinese title beyond twenty characters survives native input":"128 Chinese characters survive native input without truncation");
            if(++inputIndex_<tests_.size()){phase_=1;return SDGO_GAME_TEST_WAIT;}
            if(!reopened_){if(!Close(s))throw std::runtime_error("native cancel control unavailable");phase_=3;Observe(c,"cancel-requested",s);return SDGO_GAME_TEST_WAIT;}
            expected_=Utf8(tests_.back());
            if(!Submit(s))throw std::runtime_error("native title confirmation control unavailable");
            phase_=4;Observe(c,"submit-requested",s);return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==3){if(Service(DialogSymbol(s)))return SDGO_GAME_TEST_WAIT;reopened_=true;opened_=false;inputIndex_=0;phase_=0;Observe(c,"cancel-consumed-reopen",s);return SDGO_GAME_TEST_WAIT;}
        std::array<char,513> confirmed{};
        const uint32_t length=sdgo::CurrentRoomTitle(api_->ModDirectory(mod_),confirmed.data(),static_cast<uint32_t>(confirmed.size()));
        if(length!=expected_.size()||std::string(confirmed.data())!=expected_)return SDGO_GAME_TEST_WAIT;
        c->Assert(1,"server-confirmed current title exactly matches the complete 128-character native input");
        Observe(c,"server-confirmed",s);Reset();return SDGO_GAME_TEST_PASS;
    }
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;NativeActions* actions_=nullptr;
    void** manager_=nullptr;FindControl find_=nullptr;GetText get_=nullptr;Focus focus_=nullptr;uint8_t** input_=nullptr;
    unsigned phase_=0,inputIndex_=0,polls_=0;bool opened_=false,reopened_=false;void* dialog_=nullptr;void* edit_=nullptr;
    std::vector<std::wstring> tests_;std::string expected_;
};
