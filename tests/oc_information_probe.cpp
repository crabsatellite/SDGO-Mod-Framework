// Unshipped native GameTest provider. The isolated fixture has two OC-profiled
// units selected through SDGO_OC_INFORMATION_UNIT / SECOND_UNIT and an ordinary
// unprofiled unit in its native inventory. Opens real server-loaded records,
// switches units, closes/reopens and captures actual D3D frames. No SQL writes,
// fabricated records, allocation requests or inventory mutations.
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <string>
#include "sdgo/game_test.h"
#include "sdgo/friend_unit_gift_client_observer.h"
#include "sdgo/oc_custom_protocol.h"
#include "../mods/game_test_client/quiet_frame_capture.h"

namespace {
const SdgoApi* api_ = nullptr;
SdgoMod* mod_ = nullptr;
QuietFrameCapture visualFrame;
bool visual = false;
sdgo::oc_custom::Profile expectedProfile;
bool expectedReady=false, expectedSent=false;
void __cdecl ExpectedReply(void*,std::uint16_t command,std::int32_t status,const std::uint8_t* data,std::uint32_t bytes) {
    int result=-1;
    expectedReady=command==static_cast<std::uint16_t>(sdgo::oc_custom::Command::get_profile) && status==0 &&
        sdgo::oc_custom::ReadProfileReply(data,bytes,&result,&expectedProfile) && result==0;
}
using Create9 = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Present = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Create9 create9 = nullptr;
CreateDevice createDevice = nullptr;
Present originalPresent = nullptr;
int(__cdecl* nativeOnboarding)(const SdgoGameTestContextV1*) = nullptr;
IDirect3DDevice9* retainedDevice = nullptr;
void* currentCard = nullptr;
using CardInit = void(__thiscall*)(void*, void*);
CardInit cardInit = nullptr;
void __fastcall ObserveCard(void* service, void*, void* argument) {
    cardInit(service, argument);
    currentCard = service;
}
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
void* Service(const char* symbol = "oc_custom.information_vtable") {
    auto** manager = static_cast<void**>(api_->Symbol(mod_, "flow.service_managed_pointer"));
    const auto vtable = api_->Symbol(mod_, symbol);
    if (!vtable) return nullptr;
    if (!manager || !*manager) return nullptr;
    auto* head = At<void*>(*manager, 0x44);
    if (!head) return nullptr;
    unsigned guard = 0;
    for (auto* link = At<void*>(head); link && link != head && guard++ < 512; link = At<void*>(link)) {
        auto* service = At<void*>(link, 8);
        if (service && At<void*>(service) == vtable) return service;
    }
    return nullptr;
}
int __cdecl OpenPage(const SdgoGameTestContextV1* context) {
    RefreshCapture();
    static bool lobbyRequested = false, pageRequested = false;
    auto* grid = Service("flow.unit_management_vtable");
    auto* root = grid ? At<void*>(grid, 0x44) : nullptr;
    if (root && At<std::uint8_t>(root, 0x162)) {
        context->Assert(1, "the native unit-management service owns a visible page");
        return SDGO_GAME_TEST_PASS;
    }
    using App = void*(__cdecl*)();
    const auto app = reinterpret_cast<App>(api_->Symbol(mod_, "game.app"));
    const auto state = app && app() ? At<unsigned>(app(), 0x4A0) : 0;
    using Callback = unsigned(__stdcall*)(void*, unsigned, void*, void*);
    if (state == 6 && !lobbyRequested) {
        auto* menu = Service("flow.hangar_downmenu_vtable");
        auto* parent = menu ? At<void*>(menu, 0x44) : nullptr;
        auto* radio = parent ? Control(parent, L"MyRoomRadio", 0x0C) : nullptr;
        auto callback = reinterpret_cast<Callback>(api_->Symbol(mod_, "flow.hangar_downmenu_callback"));
        if (radio && callback && At<void*>(radio, 0x15C) == parent && At<void*>(menu, 0x54) == radio) {
            callback(menu, 0x301, static_cast<std::uint8_t*>(radio) + 0x118, radio);
            lobbyRequested = true;
        }
    } else if (state == 5 && !pageRequested) {
        auto* menu = Service("flow.operator_menu_vtable");
        auto* button = menu ? At<void*>(menu, 0x80) : nullptr;
        auto* parent = button ? At<void*>(button, 0x15C) : nullptr;
        auto callback = reinterpret_cast<Callback>(api_->Symbol(mod_, "flow.operator_menu_callback"));
        if (parent && callback && Control(parent, L"UnitBtn", 4) == button &&
            At<std::uint8_t>(parent, 0x162) && At<std::uint8_t>(button, 0x162) && At<std::uint8_t>(button, 0x167)) {
            callback(menu, 0x101, static_cast<std::uint8_t*>(button) + 0x118, button);
            pageRequested = true;
        }
    }
    return SDGO_GAME_TEST_WAIT;
}
std::wstring Text(void* control) {
    if (!control) return {};
    const auto begin = At<String**>(control, 0x190), end = At<String**>(control, 0x194);
    if (!begin || begin == end || !*begin) return {};
    const auto& text = **begin;
    if (text.length > 256 || text.capacity < text.length) return {};
    return {text.capacity < 8 ? text.data.inline_data : text.data.pointer, text.length};
}
int __cdecl Ready(const SdgoGameTestContextV1* context) {
    RefreshCapture();
    static ULONGLONG begin = GetTickCount64();
    static bool diagnostic = false;
    if (visual && !diagnostic && GetTickCount64() - begin > 10000)
        diagnostic = visualFrame.Request((std::wstring(context->directory) + L"\\ready.bmp").c_str());
    if (!nativeOnboarding) return SDGO_GAME_TEST_FAIL;
    const auto ready = nativeOnboarding(context);
    if (ready != SDGO_GAME_TEST_PASS) return ready;
    using App = void*(__cdecl*)();
    const auto app = reinterpret_cast<App>(api_->Symbol(mod_, "game.app"));
    auto** profile = static_cast<std::uint8_t**>(api_->Symbol(mod_, "hangar.profile_global"));
    if (!app || !app() || !profile || !*profile) return SDGO_GAME_TEST_WAIT;
    const auto state = At<unsigned>(app(), 0x4A0);
    if ((state != 5 && state != 6) || !At<std::uint64_t>(*profile, 0x1B1)) return SDGO_GAME_TEST_WAIT;
    context->Assert(1, "the authenticated native lobby or hangar and server-loaded active unit are available");
    return SDGO_GAME_TEST_PASS;
}
int __cdecl Capture(const SdgoGameTestContextV1* context) {
    RefreshCapture();
    static bool selected = false, requested = false, logged = false;
    static unsigned stage = 0;
    static ULONGLONG visibleAt = 0;
    if (stage == 4) {
        auto* remaining = Service();
        auto* page = remaining ? At<void*>(remaining, 0x6C) : nullptr;
        if (page && At<std::uint8_t>(page, 0x162)) return SDGO_GAME_TEST_WAIT;
        context->Assert(1, "the original Close button removed the final information popup");
        return SDGO_GAME_TEST_PASS;
    }
    auto* module = GetModuleHandleW(L"friend_unit_gift_client.dll");
    const auto observe = reinterpret_cast<SdgoFriendUnitGiftClientGetSnapshotV1Fn>(
        GetProcAddress(module, SDGO_FRIEND_UNIT_GIFT_CLIENT_SNAPSHOT_EXPORT));
    const auto action = reinterpret_cast<SdgoFriendUnitGiftClientActionV1Fn>(
        GetProcAddress(module, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_EXPORT));
    SdgoFriendUnitGiftClientSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot); snapshot.version = SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION;
    if (!observe || !action || !observe(&snapshot) || !snapshot.ready) return SDGO_GAME_TEST_WAIT;
    auto** profile = static_cast<std::uint8_t**>(api_->Symbol(mod_, "hangar.profile_global"));
    wchar_t target[32]{};
    GetEnvironmentVariableW(L"SDGO_OC_INFORMATION_UNIT", target, 32);
    const auto primary = *target ? _wcstoui64(target, nullptr, 10) : (profile && *profile ? At<std::uint64_t>(*profile, 0x1B1) : 0);
    wchar_t second[32]{};
    GetEnvironmentVariableW(L"SDGO_OC_INFORMATION_SECOND_UNIT", second, 32);
    const auto secondary = _wcstoui64(second, nullptr, 10);
    static std::uint64_t unprofiled = 0;
    if (stage == 3 && !unprofiled) {
        using Brief = void*(__thiscall*)(void*, void*, std::uint16_t);
        auto brief = reinterpret_cast<Brief>(api_->Symbol(mod_, "hangar.brief"));
        auto** inventory = static_cast<void**>(api_->Symbol(mod_, "flow.unit_info_pointer"));
        if (!brief || !inventory || !*inventory) return SDGO_GAME_TEST_FAIL;
        const auto capacity = At<int>(*inventory, 0x0C);
        if (capacity < 1 || capacity > 1200) return SDGO_GAME_TEST_FAIL;
        for (int slot = 1; slot <= capacity; ++slot) {
            std::array<std::uint8_t, 25> record{};
            brief(*inventory, record.data(), static_cast<std::uint16_t>(slot));
            const auto candidate = At<std::uint64_t>(record.data(), 4);
            if (At<unsigned>(record.data()) && candidate && candidate != primary && candidate != secondary) {
                unprofiled = candidate;
                break;
            }
        }
    }
    const auto uid = stage == 1 ? secondary : (stage == 3 ? unprofiled : primary);
    if (!uid) return SDGO_GAME_TEST_WAIT;
    if (!selected) {
        SdgoFriendUnitGiftClientActionRequestV1 request{};
        request.size = sizeof(request); request.version = SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION;
        request.run_hash = snapshot.run_hash; request.action = SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_UNIT_TEMPLATE;
        request.unit_uid = uid; request.gift_id = 1;
        selected = action(&request) != 0;
        return SDGO_GAME_TEST_WAIT;
    }
    auto* service = currentCard;
    auto* root = service ? At<void*>(service, 0x6C) : nullptr;
    if (!root || !At<std::uint8_t>(root, 0x162) || At<std::uint64_t>(service, 0x94) != uid)
        return SDGO_GAME_TEST_WAIT;
    if (!visibleAt) visibleAt = GetTickCount64();
    if (GetTickCount64() - visibleAt < 2500) return SDGO_GAME_TEST_WAIT;
    if(stage!=3 && !expectedReady) {
        const auto send=reinterpret_cast<sdgo::friend_unit_gift::ClientExtensionSendFn>(GetProcAddress(module,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));
        const auto state=reinterpret_cast<sdgo::friend_unit_gift::ClientExtensionStateFn>(GetProcAddress(module,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT));
        if(!expectedSent && send && state && state()==1) {
            std::uint8_t data[sdgo::oc_custom::kUnitRequestBytes]{};
            sdgo::oc_custom::UnitRequest request;request.unit_uid=uid;
            sdgo::oc_custom::WriteUnitRequest(data,request);
            expectedSent=send(static_cast<std::uint16_t>(sdgo::oc_custom::Command::get_profile),data,sizeof(data),&ExpectedReply,nullptr)!=0;
        }
        return SDGO_GAME_TEST_WAIT;
    }
    // The independent reply may arrive one UI update before the card's own
    // request is applied. Allow a bounded settle period, then run the same
    // assertions even if it never converges (the mismatch must still fail).
    if(stage!=3 && expectedReady && GetTickCount64()-visibleAt<10000) {
        unsigned used=0;for(auto n:expectedProfile.normal)used+=n;
        const unsigned budget=(std::min)({4u,unsigned(expectedProfile.normal_budget),unsigned(expectedProfile.normal_max_lv)});
        used=(std::min)(used,budget);
        for(unsigned i=0;i<4;++i) {
            auto* icon=Control(root,L"OcInformationNormalSlot"+std::to_wstring(i),1);
            if(!icon || At<unsigned>(icon,0x18C)!=(i<used?2u:i<budget?1u:0u))return SDGO_GAME_TEST_WAIT;
        }
    }
    if (!logged) {
        if(stage!=3)context->Assert(expectedProfile.unit_uid==uid,"the independent profile reply identifies the inspected unit");
        for(const auto* name:{L"OcInformationRadarPanel",L"OcInformationFrameLeft"}) {
            auto* c=Control(root,name,4);auto** skin=c?At<unsigned char**>(c,0x1A4):nullptr;
            auto* sprite=skin&&*skin?At<unsigned char*>(*skin):nullptr;
            auto* data=sprite?At<unsigned char*>(sprite,0x168):nullptr;
            auto** frames=data?At<unsigned char**>(data,0xC):nullptr;
            if(frames&&*frames)api_->Log(mod_,SDGO_LOG_INFO,"information native art %ls %dx%d",name,At<int>(*frames,4),At<int>(*frames,8));
        }
        api_->Log(mod_, SDGO_LOG_INFO, "information geometry x=%.0f y=%.0f width=%d height=%d",
                  At<float>(root, 0x148), At<float>(root, 0x14C), At<int>(root, 0x154), At<int>(root, 0x158));
        sdgo::oc_custom::Carrier carrier;
        const bool present = sdgo::oc_custom::ReadCarrier(static_cast<std::uint8_t*>(service) + 0x8C, &carrier);
        api_->Log(mod_, SDGO_LOG_INFO, "information carrier=%u flags=%u level=%u", present, carrier.flags, carrier.oc_level);
        auto* head = At<void*>(root, 0x23C);
        unsigned guard = 0;
        for (auto* link = head ? At<void*>(head) : nullptr; link && link != head && guard++ < 256; link = At<void*>(link)) {
            auto* control = At<void*>(link, 8);
            const auto name = At<String>(control, 0x118);
            if (name.length < 128) api_->Log(mod_, SDGO_LOG_INFO, "information control=%.*ls rect=%.0f,%.0f,%d,%d z=%.1f",
                static_cast<int>(name.length), name.capacity < 8 ? name.data.inline_data : name.data.pointer,
                At<float>(control, 0x148), At<float>(control, 0x14C), At<int>(control, 0x154), At<int>(control, 0x158), At<float>(control,0x150));
        }
        const bool hasProfile = (carrier.flags & sdgo::oc_custom::carrier_profile) != 0;
        context->Assert(present && hasProfile == (stage != 3),
                        "the server record supplies the expected profile or missing-profile state");
        for (unsigned i = 0; i < 18; ++i) {
            auto* control=Control(root,L"OcInformationText"+std::to_wstring(i),4);
            if(i==1 || i==2 || (i>=9 && i<=15))context->Assert(!control,"native information omits extra radar descriptions and a second experience line");
            else context->Assert(control && At<void*>(control,0x15C)==root && At<std::uint8_t>(control,0x162),"native page retains its detail, axis and parts captions");
        }
        unsigned used=0;for(auto n:expectedProfile.normal)used+=n;
        const unsigned budget=hasProfile?(std::min)({4u,unsigned(expectedProfile.normal_budget),unsigned(expectedProfile.normal_max_lv)}):0;
        used=(std::min)(used,budget);
        for(unsigned i=0;i<4;++i) {
            auto* icon=Control(root,L"OcInformationNormalSlot"+std::to_wstring(i),1);
            context->Assert(icon && At<void*>(icon,0x15C)==root && At<std::uint8_t>(icon,0x162) && !At<std::uint8_t>(icon,0x167),"four read-only native ordinary-point indicators belong to the inspected card");
            auto** skins=icon?At<unsigned char**>(icon,0x1A4):nullptr;
            const auto selected=icon?At<unsigned>(icon,0x18C):3;
            const auto end=icon?At<unsigned char**>(icon,0x1A8):nullptr;
            const bool valid=skins && end==skins+3 && selected<3;
            context->Assert(valid,"ordinary-point control owns exactly three native state skins");
            const auto value=valid?At<String>(skins[selected],4):String{};
            const std::wstring path(value.capacity<8?value.data.inline_data:value.data.pointer,value.length);
            const auto expected=L"sprs\\"+std::to_wstring(5546+i*3+(i<used?2:i<budget?1:0))+L".spr";
            context->Assert(path==expected,"I..IV native skins match ordinary-point budget and allocation in independent server profile");
        }
        context->Assert(At<int>(root,0x154)>=900,"the unit-information popup keeps all three columns");
        context->Assert(At<int>(root,0x158)==492 && !Control(root,L"OcInformationBackground",4),
                        "the native popup has no extra footer explanation panel");
        auto* level=Control(root,L"LevelStatic",3);
        const auto nativeText=At<String>(level,0x21C);
        std::wstring expected(nativeText.capacity<8?nativeText.data.inline_data:nativeText.data.pointer,nativeText.length);
        auto displayed=Text(Control(root,L"OcInformationText0",4));
        expected.erase(std::remove(expected.begin(),expected.end(),L' '),expected.end());
        displayed.erase(std::remove(displayed.begin(),displayed.end(),L' '),displayed.end());
        context->Assert(expected==displayed,"the left level cell retains full native level and experience on one line");
        for(const auto* name:{L"OcInformationTablePanel",L"OcInformationWeaponsPanel",L"OcInformationSkillsPanel"}) {
            auto* panel=Control(root,name,4);
            context->Assert(panel && At<void*>(panel,0x15C)==root && At<std::uint8_t>(panel,0x162),
                            "the native popup owns the separate information table, weapons and skill grids");
        }
        for(unsigned i=0;i<4;++i){
            auto* icon=Control(root,L"AttributeSkillIcon"+std::to_wstring(i),1);
            context->Assert(icon && At<void*>(icon,0x15C)==root && At<float>(icon,0x148)>=600,
                            "native G2 icons and their tooltip targets remain in the right column");
        }
        logged = true;
    }
    auto test = GetModuleHandleW(L"game_test_client.dll");
    const auto request = reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(test, "SdgoGameTestCaptureFrameV1"));
    const auto status = reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(test, "SdgoGameTestCaptureStatusV1"));
    if (!visual && (!request || !status)) return SDGO_GAME_TEST_FAIL;
    if (!requested) {
        const auto path = std::wstring(context->directory) + L"\\oc-information-" + std::to_wstring(stage) + L".bmp";
        requested = visual ? visualFrame.Request(path.c_str()) : request(path.c_str()) != 0;
        return SDGO_GAME_TEST_WAIT;
    }
    HRESULT result = E_PENDING;
    const auto state = visual ? visualFrame.State(&result) : status(&result);
    if (state != 3 && state != 4) return SDGO_GAME_TEST_WAIT;
    context->Assert(state == 3 && SUCCEEDED(result), "capture the real native information page before Present");
    if (state != 3 || FAILED(result)) return SDGO_GAME_TEST_FAIL;
    using Callback = unsigned(__stdcall*)(void*, unsigned, void*, void*);
    auto close = reinterpret_cast<Callback>(api_->Symbol(mod_, "friend_unit_gift.native_card_callback"));
    auto* button = Control(root, L"CloseBtn", 4);
    if (!button || !close || At<void*>(button, 0x15C) != root) return SDGO_GAME_TEST_FAIL;
    currentCard = nullptr;
    close(service, 0x101, static_cast<std::uint8_t*>(button) + 0x118, button);
    context->Assert(1, "closed the inspected card through its original Close button callback");
    ++stage;
    selected = requested = logged = false;
    expectedReady=expectedSent=false;expectedProfile={};
    visibleAt = 0;
    return SDGO_GAME_TEST_WAIT;
}
// The opt-in peer scenario retains the native coordinated room-session case.
#include "oc_information_peer_probe.h"
}
SDGO_MOD_ENTRY {
    wchar_t run[64]{}, role[16]{};
    if (!GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN", run, 64) ||
        !GetEnvironmentVariableW(L"SDGO_GAME_TEST_ROLE", role, 16) || std::wcscmp(role, L"client")) return 0;
    api_ = api; mod_ = mod;
    if (!api->HookSlot(mod, "test.oc_information_init", reinterpret_cast<void*>(&ObserveCard),
                       reinterpret_cast<void**>(&cardInit))) return 0;
    wchar_t mode[4]{}, plan[MAX_PATH]{};
    visual = GetEnvironmentVariableW(L"SDGO_OC_INFORMATION_VISUAL", mode, 4) && !std::wcscmp(mode, L"1");
    if (visual) {
        GetEnvironmentVariableW(L"SDGO_GAME_TEST_PLAN", plan, MAX_PATH);
        if (!*plan || GetPrivateProfileIntW(L"run", L"quiet", 1, plan)) return 0;
        return api->HookImport(mod, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&VisualFactory),
                               reinterpret_cast<void**>(&create9));
    }
    return 1;
}
SDGO_GAME_TEST_PROVIDER_ENTRY {
    static std::array<SdgoGameTestCaseV1, 5> cases{};
    auto runtime = GetModuleHandleW(L"game_test_client.dll");
    auto get = reinterpret_cast<SdgoGameTestGetProviderV1Fn>(GetProcAddress(runtime, SDGO_GAME_TEST_PROVIDER_EXPORT));
    const auto* original = get ? get() : nullptr;
    if (!original || original->count < 3) return nullptr;
    if(GetEnvironmentVariableW(L"SDGO_OC_INFORMATION_PEER",nullptr,0)) {
        static std::array<SdgoGameTestCaseV1,4> peerCases{};
        if(original->count!=4 || std::strcmp(original->cases[3].id,"friends.session"))return nullptr;
        for(unsigned i=0;i<4;++i)peerCases[i]=original->cases[i];
        nativeOnboarding=peerCases[2].poll;peerCases[2].poll=&PeerReady;
        peerSession=peerCases[3].poll;peerCases[3].poll=&PeerCapture;
        static const SdgoGameTestProviderV1 peerProvider{sizeof(peerProvider),SDGO_GAME_TEST_VERSION,"oc-information","client",4,peerCases.data()};
        return &peerProvider;
    }
    if (std::strcmp(original->cases[2].id, "flow.onboarding")) return nullptr;
    nativeOnboarding = original->cases[2].poll;
    for (unsigned i = 0; i < 2; ++i) cases[i] = original->cases[i];
    cases[2] = {"flow.information_ready", 90000, &Ready};
    cases[3] = {"oc.information_open", 30000, &OpenPage};
    cases[4] = {"oc.information", 90000, &Capture};
    static const SdgoGameTestProviderV1 provider{sizeof(provider), SDGO_GAME_TEST_VERSION, "oc-information", "client", 5, cases.data()};
    return &provider;
}
