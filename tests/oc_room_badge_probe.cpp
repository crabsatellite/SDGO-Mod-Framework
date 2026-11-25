// Native-only fixture: real login and room creation, read-only UI checks and D3D capture.
// Never staged into player packages. No fabricated room or account state.
#include <windows.h>
#include <array>
#include <cstring>
#include <string>
#include "sdgo/game_test.h"
#include "sdgo/oc_custom_client_observer.h"
#include "../mods/game_test_client/quiet_frame_capture.h"

namespace {
const SdgoApi* api_ = nullptr;
SdgoMod* mod_ = nullptr;
QuietFrameCapture visualFrame;
bool visual = false;
using Create9 = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Present = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Create9 create9 = nullptr;
CreateDevice createDevice = nullptr;
Present originalPresent = nullptr;
IDirect3DDevice9* retainedDevice = nullptr;
bool Slot(void** slot, void* hook, void** original) {
    if (*slot == hook) return true;
    DWORD protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protect)) return false;
    *original = InterlockedExchangePointer(slot, hook);
    DWORD ignored = 0;
    return VirtualProtect(slot, sizeof(void*), protect, &ignored) != 0;
}
HRESULT STDMETHODCALLTYPE VisualPresent(IDirect3DDevice9* device, const RECT* source,
                                         const RECT* target, HWND window, const RGNDATA* dirty) {
    static bool first = true;
    if (first) { api_->Log(mod_, SDGO_LOG_INFO, "native visual Present observed"); first = false; }
    visualFrame.BeforePresent(device);
    HRESULT captured = E_PENDING;
    static LONG previous = 0;
    const auto state = visualFrame.State(&captured);
    if (state != previous) {
        api_->Log(mod_, SDGO_LOG_INFO, "native frame capture state=%ld hr=%08lx", state, captured);
        previous = state;
    }
    return originalPresent(device, source, target, window, dirty);
}
HRESULT STDMETHODCALLTYPE VisualDevice(IDirect3D9* factory, UINT adapter, D3DDEVTYPE type, HWND window,
                                        DWORD flags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) {
    const auto result = createDevice(factory, adapter, type, window, flags, parameters, device);
    api_->Log(mod_, SDGO_LOG_INFO, "native visual CreateDevice hr=%08lx", result);
    if (SUCCEEDED(result) && device && *device) {
        (*device)->AddRef();
        if (retainedDevice) retainedDevice->Release();
        retainedDevice = *device;
        Slot(&(*reinterpret_cast<void***>(*device))[17], reinterpret_cast<void*>(&VisualPresent),
             reinterpret_cast<void**>(&originalPresent));
    }
    return result;
}
void RefreshCapture() {
    if (visual && retainedDevice)
        Slot(&(*reinterpret_cast<void***>(retainedDevice))[17], reinterpret_cast<void*>(&VisualPresent),
             reinterpret_cast<void**>(&originalPresent));
}
IDirect3D9* WINAPI VisualFactory(UINT version) {
    auto* result = create9(version);
    api_->Log(mod_, SDGO_LOG_INFO, "native visual factory created=%u", result != nullptr);
    if (result) Slot(&(*reinterpret_cast<void***>(result))[16], reinterpret_cast<void*>(&VisualDevice),
                     reinterpret_cast<void**>(&createDevice));
    return result;
}
template<class T> T At(const void* p, unsigned offset = 0) {
    T value{}; std::memcpy(&value, static_cast<const std::uint8_t*>(p) + offset, sizeof(value)); return value;
}
struct String { union { wchar_t inline_data[8]; const wchar_t* pointer; } data{}; unsigned length = 0, capacity = 7; };
using Find = void*(__thiscall*)(void*, const String*, int);
void* Control(void* root, const std::wstring& name, int type) {
    String key; key.length = static_cast<unsigned>(name.size());
    if (key.length < 8) std::memcpy(key.data.inline_data, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
    else { key.data.pointer = name.c_str(); key.capacity = key.length; }
    auto find = reinterpret_cast<Find>(api_->Symbol(mod_, "hangar.ui_find_control"));
    return find(root, &key, type);
}
using TitleUpdate = void(__thiscall*)(void*);
TitleUpdate titleUpdate = nullptr;
void* titleRoot = nullptr;
int(__cdecl* nativeRoom)(const SdgoGameTestContextV1*) = nullptr;
int __cdecl EnterRoom(const SdgoGameTestContextV1* context) {
    RefreshCapture();
    // Let the normal lobby's capability and room-mode queries finish before
    // the automated create request leaves that page.
    if(context->elapsed_ms<8000)return SDGO_GAME_TEST_WAIT;
    return nativeRoom?nativeRoom(context):SDGO_GAME_TEST_FAIL;
}
void __fastcall ObserveTitle(void* owner, void*) {
    titleUpdate(owner);
    titleRoot = At<void*>(owner, 0x48);
}
int __cdecl Capture(const SdgoGameTestContextV1* context) {
    RefreshCapture();
    static bool requested = false, checked = false;
    if (context->elapsed_ms < 20000) return SDGO_GAME_TEST_WAIT;
    static bool diagnosed=false;
    if(!diagnosed) {
        auto get=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(GetModuleHandleW(L"oc_custom_client.dll"),"SdgoOcCustomClientGetSnapshotV2"));
        SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
        if(get)get(&s);
        api_->Log(mod_,SDGO_LOG_INFO,"badge root=%p mode=%u room_rule=%u transport=%u sent=%u replies=%u status=%d cmd=%u title_marks=%u",titleRoot,s.server_oc_mode,s.server_room_rule,s.transport_ready,s.request_count,s.reply_count,s.last_status,s.last_command,s.title_marks);
        diagnosed=true;
    }
    if(!titleRoot)return SDGO_GAME_TEST_WAIT;
    auto* badge = Control(titleRoot, L"OcRoomSystemStatic", 4);
    if (!badge) return SDGO_GAME_TEST_WAIT;
    if (!checked) {
        context->Assert(At<void*>(badge,0x15C)==titleRoot && At<unsigned char>(badge,0x162) &&
                        !At<unsigned char>(badge,0x167), "room badge is visible, owned by the room and read-only");
        void* title = Control(titleRoot,L"TitleChangeBtn",4);
        const float badgeX = At<float>(badge,0x148);
        context->Assert(title && badgeX==At<float>(title,0x148)+(At<int>(title,0x154)-133)/2 &&
                        At<float>(badge,0x14C)==59 &&
                        At<int>(badge,0x154)==133 && At<int>(badge,0x158)==27,
                        "badge is centered below the room title at native skin dimensions");
        // 0912.uir anchors the separate random-team banner at (10,60).
        // Its background begins six pixels to the left and is 113 pixels wide.
        context->Assert(badgeX >= 10-6+113, "badge clears the guest random-team banner");
        auto** skins=At<unsigned char**>(badge,0x1A4);
        auto** end=At<unsigned char**>(badge,0x1A8);
        const auto key=skins && end==skins+1?At<String>(*skins,4):String{};
        const std::wstring path(key.length?(key.capacity<8?key.data.inline_data:key.data.pointer):L"",key.length);
        context->Assert(path==L"sprs\\11448.spr","badge uses the original cyan frame resource");
        for(const auto* name:{L"RandomTeamCheck",L"RandomTeamStatic"}) {
            void* c=Control(titleRoot,name,name==std::wstring(L"RandomTeamCheck")?10:3);
            if(!c) {for(int type=1;type<=16&&!c;++type)c=Control(titleRoot,name,type);}
            context->Assert(c!=nullptr,"original random-team control remains present");
            if(c)context->Assert(At<float>(c,0x148)>=At<float>(badge,0x148)+At<int>(badge,0x154) ||
                At<float>(c,0x14C)+At<int>(c,0x158)<=At<float>(badge,0x14C),
                "random-team control does not overlap the badge");
        }
        auto* items=At<void**>(badge,0x190);const auto text=items?At<String>(items[0],0):String{};
        const std::wstring caption(text.length?(text.capacity<8?text.data.inline_data:text.data.pointer):L"",text.length);
        wchar_t expected[64]{};GetEnvironmentVariableW(L"SDGO_BADGE_EXPECTED",expected,64);
        context->Assert(*expected && caption==expected,"badge caption matches the actual requested room system");
        api_->Log(mod_,SDGO_LOG_INFO,"badge caption=%ls",caption.c_str());
        checked=true;
    }
    if(!requested){requested=visualFrame.Request((std::wstring(context->directory)+L"\\room-badge.bmp").c_str());return SDGO_GAME_TEST_WAIT;}
    HRESULT result=E_PENDING;const auto state=visualFrame.State(&result);
    if(state!=3&&state!=4)return SDGO_GAME_TEST_WAIT;
    context->Assert(state==3&&SUCCEEDED(result),"captured the real rendered waiting room");
    return state==3&&SUCCEEDED(result)?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
}
SDGO_MOD_ENTRY {
    wchar_t run[64]{},role[16]{};
    if(!GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",run,64)||
       !GetEnvironmentVariableW(L"SDGO_GAME_TEST_ROLE",role,16)||std::wcscmp(role,L"client"))return 0;
    api_=api;mod_=mod;visual=true;
    auto** slot=static_cast<void**>(api->Symbol(mod,"oc_custom.client_titlebar_update"));
    if(!slot||!Slot(slot,reinterpret_cast<void*>(&ObserveTitle),reinterpret_cast<void**>(&titleUpdate)))return 0;
    return api->HookImport(mod,"d3d9.dll","Direct3DCreate9",reinterpret_cast<void*>(&VisualFactory),reinterpret_cast<void**>(&create9));
}
SDGO_GAME_TEST_PROVIDER_ENTRY {
    static std::array<SdgoGameTestCaseV1,5> cases{};
    auto runtime=GetModuleHandleW(L"game_test_client.dll");
    auto get=reinterpret_cast<SdgoGameTestGetProviderV1Fn>(GetProcAddress(runtime,SDGO_GAME_TEST_PROVIDER_EXPORT));
    const auto* original=get?get():nullptr;
    if(!original)return nullptr;
    unsigned count=0;
    for(const auto* name:{"framework.bootstrap","flow.login","flow.onboarding","pvp.room"}) {
        for(unsigned i=0;i<original->count;++i)if(!std::strcmp(original->cases[i].id,name))cases[count++]=original->cases[i];
    }
    if(count!=4)return nullptr;
    nativeRoom=cases[3].poll;cases[3].poll=&EnterRoom;
    cases[4]={"oc.room_badge",45000,&Capture};
    static const SdgoGameTestProviderV1 provider{sizeof(provider),SDGO_GAME_TEST_VERSION,"oc-room-badge","client",5,cases.data()};
    return &provider;
}
