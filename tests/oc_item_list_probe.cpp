// Native-only OC item placement fixture: real inventory, native UI events and D3D capture.
// Never staged into player packages. Uses an existing isolated test account
// with eligible OC items; cancels confirmation without consuming inventory.
#include <windows.h>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/game_test.h"
#include "sdgo/oc_custom_client_observer.h"
#include "../mods/game_test_client/quiet_frame_capture.h"

namespace {
const SdgoApi* api_ = nullptr;
SdgoMod* mod_ = nullptr;
using CaptureRequest = int(__cdecl*)(const wchar_t*);
using CaptureStatus = LONG(__cdecl*)(HRESULT*);
CaptureRequest quietRequest = nullptr;
CaptureStatus quietStatus = nullptr;
struct ProbeFrameCapture : QuietFrameCapture {
    bool Request(const wchar_t* path) { return quietRequest ? quietRequest(path) != 0 : QuietFrameCapture::Request(path); }
    LONG State(HRESULT* result = nullptr) const { return quietStatus ? quietStatus(result) : QuietFrameCapture::State(result); }
} visualFrame;
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

int(__cdecl* nativeOpen)(const SdgoGameTestContextV1*)=nullptr;
int __cdecl Open(const SdgoGameTestContextV1* context) {
    RefreshCapture();
    return nativeOpen(context);
}
#include "oc_ex_recording_case.h"
#include "oc_ex_selection_case.h"
#include "oc_ui_polish_case.h"
#include "oc_layout_case.h"
int __cdecl Check(const SdgoGameTestContextV1* c) {
    RefreshCapture();
    const auto plan=std::wstring(c->directory)+L"\\run.ini";
    if(GetPrivateProfileIntW(L"run",L"oc_ex_reopen",0,plan.c_str()))return ExReopen(c);
    if(GetPrivateProfileIntW(L"run",L"oc_layout_only",0,plan.c_str()))return LayoutOnly(c);
    static bool polished=false;
    if(GetPrivateProfileIntW(L"run",L"oc_ui_polish",0,plan.c_str())&&!polished) {
        const auto result=PolishUi(c);
        if(result!=SDGO_GAME_TEST_PASS || !GetPrivateProfileIntW(L"run",L"oc_ui_then_items",0,plan.c_str()))return result;
        polished=true;
    }
    if(GetPrivateProfileIntW(L"run",L"oc_full_challenge",0,plan.c_str())) {
        static bool upgraded=false;
        if(!upgraded) {
            const auto result=FullChallenge(c);
            if(result!=SDGO_GAME_TEST_PASS)return result;
            upgraded=true;
        }
        if(GetPrivateProfileIntW(L"run",L"oc_ex_selection",0,plan.c_str()))return ExSelection(c);
        return SDGO_GAME_TEST_PASS;
    }
    static unsigned phase=0, selected=0, selectedRow=0, radar=0;
    static ULONGLONG since=GetTickCount64();
    static bool requested=false;
    static std::array<int16_t,10> counts{};
    auto module=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(module,"SdgoOcCustomClientGetSnapshotV2"));
    auto items=reinterpret_cast<SdgoOcCustomClientItemsV1Fn>(GetProcAddress(module,"SdgoOcCustomClientItemsV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(module,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
    SdgoOcCustomClientItemsV1 v{};v.size=sizeof(v);v.version=1;
    if(!observe||!items||!click||!observe(&s)||!items(&v))return SDGO_GAME_TEST_FAIL;
    if(s.busy || GetTickCount64()-since<1800)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n) {phase=n;since=GetTickCount64();requested=false;return SDGO_GAME_TEST_WAIT;};
    const auto capture=[&](const wchar_t* name) {
        if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\"+name+L".bmp").c_str());return false;}
        HRESULT hr=E_PENDING;auto state=visualFrame.State(&hr);
        if(state!=3&&state!=4)return false;
        c->Assert(state==3&&SUCCEEDED(hr),"captured the rendered OC page before Present");return true;
    };
    const auto visible=[](void* p){return p&&At<unsigned char>(p,0x162)!=0;};
    if(phase==0) {
        c->Assert(!v.list_visible&&!s.challenge_selected&&!s.item_models_shown,"OC starts with radar and no preselected consumables");
        c->Assert(visible(v.toggle),"native skinned item-list switch is visible");
        if(!capture(L"oc-items-radar"))return SDGO_GAME_TEST_WAIT;
        click("over",L"OcItemListBtn");return next(1);
    }
    if(phase==1) {
        c->Assert(v.list_visible&&v.ready,"the switch opens an inventory-backed list");
        c->Assert(s.owned_items>0&&s.challenge_items>0,"reused native account owns an eligible OC item");
        if(!s.challenge_items)return SDGO_GAME_TEST_FAIL;
        for(unsigned i=0;i<10;++i) {
            counts[i]=v.counts[i];
            c->Assert(visible(v.rows[i])==(i<s.owned_items),"grid cells match actual owned item types");
            if(i<s.owned_items) {
                bool eligible=false;for(unsigned j=0;j<s.challenge_items;++j)eligible|=v.owned[i]==s.challenge_item_ids[j];
                c->Assert((At<unsigned char>(v.rows[i],0x167)!=0)==eligible,"only server-eligible inventory cells can be placed");
                if(v.owned[i]==s.challenge_item_ids[0])selectedRow=i;
            }
        }
        for (unsigned cell=0;cell<8;++cell) {
            c->Assert(Control(v.panel,L"Item3DRender"+std::to_wstring(cell),5)==v.rows[cell],"inventory uses the original native 3D item control");
            c->Assert(Control(v.panel,L"NumberStatic"+std::to_wstring(cell),3)!=nullptr,"inventory keeps its native quantity statics");
        }
        c->Assert(Control(v.panel,L"LeftBtn",4)&&Control(v.panel,L"RightBtn",4),"inventory keeps both original paging buttons");
        radar=s.radar_frames;
        click("items",(L"Item3DRender"+std::to_wstring(selectedRow)).c_str());return next(2);
    }
    if(phase==2) {
        c->Assert(s.challenge_selected==1&&v.placed[0]==s.challenge_item_ids[0]&&s.item_models_shown==1,"placing a native grid cell fills exactly one ITEM slot with its native model");
        c->Assert(s.radar_frames==radar,"the radar stops drawing while the item list is shown");
        if(!capture(L"oc-items-placed"))return SDGO_GAME_TEST_WAIT;
        click("items",(L"Item3DRender"+std::to_wstring(selectedRow)).c_str());return next(3);
    }
    if(phase==3) {
        c->Assert(!s.challenge_selected&&!s.item_models_shown,"clicking the selected grid cell again removes the item");
        for(unsigned i=0;i<s.owned_items;++i)if(At<unsigned char>(v.rows[i],0x167)&&selected<3) {
            click("items",(L"Item3DRender"+std::to_wstring(i)).c_str());++selected;
        }
        return next(4);
    }
    if(phase==4) {
        c->Assert(s.item_models_shown==selected,"each selected type has one native item model");
        if(!capture(L"oc-items-selected"))return SDGO_GAME_TEST_WAIT;
        click("info",L"OcRemoveItem0");return next(5);
    }
    if(phase==5) {
        c->Assert(s.item_models_shown==selected-1,"left-slot removal updates the shared selection");
        selected=s.challenge_selected;
        click("items",L"OcItemsRadarBtn");return next(6);
    }
    if(phase==6) {
        c->Assert(!v.list_visible&&s.challenge_selected==selected&&s.radar_frames>radar,"returning to the radar preserves selected items and resumes chart drawing");
        c->Assert(s.windows&SDGO_OC_WINDOW_ENTRY_LIT,"the reused unit is eligible for a challenge preview");
        if(!(s.windows&SDGO_OC_WINDOW_ENTRY_LIT))return SDGO_GAME_TEST_FAIL;
        click("entry",L"OverCustomBtn");return next(7);
    }
    if(phase==7) {
        c->Assert((s.windows&SDGO_OC_WINDOW_CHALLENGE)&&s.challenge_selected==selected,"challenge refresh reconciles selected item IDs without dropping valid choices");
        if(!capture(L"oc-items-challenge"))return SDGO_GAME_TEST_WAIT;
        click("challenge",L"StartBtn");return next(8);
    }
    if(phase==8) {
        c->Assert(s.windows&SDGO_OC_WINDOW_CONFIRM,"starting the challenge still requires the native confirmation");
        if(!capture(L"oc-items-confirm"))return SDGO_GAME_TEST_WAIT;
        click("confirm",L"CancelBtn");return next(9);
    }
    if(phase==9) {
        c->Assert(!(s.windows&SDGO_OC_WINDOW_CONFIRM)&&s.challenge_selected==selected,"cancelling confirmation keeps the chosen items");
        click("challenge",L"CloseBtn");return next(10);
    }
    if(phase==10) {click("over",L"OcItemListBtn");return next(11);}
    c->Assert(v.list_visible&&s.challenge_selected==selected,"closing challenge and reopening the list preserves selection");
    for(unsigned i=0;i<10;++i)c->Assert(v.counts[i]==counts[i],"placement, removal and cancelled challenge do not consume inventory");
    if(!capture(L"oc-items-final"))return SDGO_GAME_TEST_WAIT;
    return SDGO_GAME_TEST_PASS;
}
}
SDGO_MOD_ENTRY {
    wchar_t run[64]{},role[16]{};
    if(!GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",run,64)||
       !GetEnvironmentVariableW(L"SDGO_GAME_TEST_ROLE",role,16)||std::wcscmp(role,L"client"))return 0;
    api_=api;mod_=mod;visual=true;
    wchar_t plan[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"SDGO_GAME_TEST_PLAN",plan,MAX_PATH) && GetPrivateProfileIntW(L"run",L"quiet",0,plan)) {
        // The background runtime owns Direct3DCreate9 and its hidden graphics
        // proxy. Reuse its frame-capture API instead of claiming that hook twice.
        auto runtime=GetModuleHandleW(L"game_test_client.dll");
        quietRequest=reinterpret_cast<CaptureRequest>(GetProcAddress(runtime,"SdgoGameTestCaptureFrameV1"));
        quietStatus=reinterpret_cast<CaptureStatus>(GetProcAddress(runtime,"SdgoGameTestCaptureStatusV1"));
        visual=false;
        return quietRequest&&quietStatus;
    }
    return api->HookImport(mod,"d3d9.dll","Direct3DCreate9",reinterpret_cast<void*>(&VisualFactory),reinterpret_cast<void**>(&create9));
}
SDGO_GAME_TEST_PROVIDER_ENTRY {
    static std::array<SdgoGameTestCaseV1,5> cases{};
    auto get=reinterpret_cast<SdgoGameTestGetProviderV1Fn>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),SDGO_GAME_TEST_PROVIDER_EXPORT));
    const auto* original=get?get():nullptr;
    wchar_t plan[MAX_PATH]{},suite[32]{};
    if(GetEnvironmentVariableW(L"SDGO_GAME_TEST_PLAN",plan,MAX_PATH))
        GetPrivateProfileStringW(L"run",L"suite",L"",suite,32,plan);
    if(!std::wcscmp(suite,L"oc-ex-battle")) {
        // Reuse the real login, allocation, room, battle and return sequence.
        // The independent UI reset exercise is outside this EX case.
        static std::vector<SdgoGameTestCaseV1> battleCases;
        if(!original || !GetPrivateProfileIntW(L"run",L"oc_expected_ex",0,plan))return nullptr;
        bool hasStats=false;battleCases.clear();
        for(unsigned i=0;i<original->count;++i) {
            if(!std::strcmp(original->cases[i].id,"oc.panel"))continue;
            hasStats|=!std::strcmp(original->cases[i].id,"oc.stats");
            battleCases.push_back(original->cases[i]);
        }
        if(!hasStats)return nullptr;
        static SdgoGameTestProviderV1 battle{sizeof(battle),SDGO_GAME_TEST_VERSION,"oc-ex-battle","client",0,nullptr};
        battle.count=static_cast<uint32_t>(battleCases.size());battle.cases=battleCases.data();return &battle;
    }
    if(!original||original->count!=4||std::strcmp(original->cases[3].id,"oc.items_open"))return nullptr;
    for(unsigned i=0;i<4;++i)cases[i]=original->cases[i];
    nativeOpen=cases[3].poll;cases[3].poll=&Open;
    cases[4]={"oc.items",240000,&Check};
    static const SdgoGameTestProviderV1 provider{sizeof(provider),SDGO_GAME_TEST_VERSION,"oc-item-list","client",5,cases.data()};
    return &provider;
}
