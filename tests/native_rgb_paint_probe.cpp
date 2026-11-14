// Unshipped rendering probe, enabled only in an isolated GameTest client.
// Save mode uses the real paint page and server acknowledgement. The external
// isolated fixture owns backup/restoration of its test account's RGB metadata.
#include "sdgo/rgb_paint_native.h"
#include <windows.h>
#include <cstring>
#include <array>
#include <cstdio>
#include "sdgo/game_test.h"
#include "sdgo/login_showcase_api.h"
#include "sdgo/rgb_paint_observer.h"

namespace {
using Style = void(__cdecl*)(void*, const std::uint16_t*, std::uint16_t, const std::uint32_t*);
Style original = nullptr;
sdgo::rgb_paint::native::Renderer renderer;
const SdgoApi* ownerApi = nullptr;
SdgoMod* ownerMod = nullptr;
unsigned observations = 0;
unsigned regions = 0;
bool panelTest=false;
bool panelInputs=false;
bool panelValid=false;
std::uint64_t beforeSaveRevision=0;
unsigned saveStep=0;
SdgoGameTestCallbackV1 friendPoll=nullptr;
using Observe=SdgoRgbPaintObserveV1Fn;
bool ObservePanel(SdgoRgbPaintObservationV1& out){
    const auto observe=reinterpret_cast<Observe>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),SDGO_RGB_PAINT_OBSERVE_EXPORT));
    return observe&&observe(&out)&&out.visible&&out.frames>0&&out.unit;
}
using Color = void(__cdecl*)(void*,int,std::uint16_t);
Color originalColor = nullptr;
void* targetModel=nullptr;
using MaterialUpdate=void(__thiscall*)(void*,void*);
MaterialUpdate originalUpdate=nullptr,originalObservedUpdate=nullptr;
unsigned materialUpdates=0;
const sdgo::rgb_paint::Rgb palette[] = {{255,0,72},{255,224,245},{255,155,218},
    {255,192,192},{255,192,192},{255,0,128}};
void PreviewMaterials(void* model) {
    if(model==targetModel){++materialUpdates;for(unsigned i=0;i<6;++i)renderer.Apply(model,i,palette[i]);}
}
void __fastcall UpdateMaterials(void* model,void*,void* context) {
    PreviewMaterials(model);originalUpdate(model,context);
}
void __fastcall UpdateObservedMaterials(void* model,void*,void* context) {
    PreviewMaterials(model);originalObservedUpdate(model,context);
}
void __cdecl Paint(void* model,int region,std::uint16_t value) {
    originalColor(model,region,value);
    if (region >= 0 && region < 6 && renderer.Apply(model,region,palette[region])) regions |= 1u << region;
}
void __cdecl Probe(void* model, const std::uint16_t* colors, std::uint16_t shine, const std::uint32_t* decals) {
    original(model, colors, shine, decals);
    const auto error = GetLastError();
    unsigned applied = 0;
    for (std::size_t region = 0; region < sdgo::rgb_paint::kRegions; ++region)
        if (renderer.Apply(model,region,palette[region])) ++applied;
    if (observations++ < 32) ownerApi->Log(ownerMod,SDGO_LOG_INFO,"rgb native material applied=%u regions=6",applied);
    if(applied==6)targetModel=model;
    SetLastError(error);
}
template<class T>T At(const void* p,unsigned offset=0) { T v{};std::memcpy(&v,static_cast<const std::uint8_t*>(p)+offset,sizeof v);return v; }
struct String {union {wchar_t buffer[8];const wchar_t* pointer;} data{};unsigned length=0,capacity=7;};
using Find=void*(__thiscall*)(void*,const String*,int);
using Event=int(__stdcall*)(void*,unsigned,std::uintptr_t,void*);
DWORD WINAPI DiagnosticCapture(void*) {
    wchar_t preview[4]{};
    if(GetEnvironmentVariableW(L"SDGO_RGB_TEST_LOGIN_PREVIEW",preview,4))return 0;
    Sleep(25000);
    wchar_t path[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"SDGO_RGB_TEST_DIAGNOSTIC",path,MAX_PATH))return 0;
    const auto runtime=GetModuleHandleW(L"game_test_client.dll");
    const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(runtime,"SdgoGameTestCaptureFrameV1"));
    if(request)request(path);
    return 0;
}
void* Service(const char* symbol) {
    auto** managed=static_cast<void**>(ownerApi->Symbol(ownerMod,"flow.service_managed_pointer"));
    const auto expected=ownerApi->Symbol(ownerMod,symbol);
    if(!managed||!*managed||!expected)return nullptr;
    auto* head=At<void*>(*managed,0x44);if(!head)return nullptr;
    unsigned count=0;
    for(auto* link=At<void*>(head);link&&link!=head&&count++<512;link=At<void*>(link)) {
        auto* service=At<void*>(link,8);if(service&&At<void*>(service)==expected)return service;
    }
    return nullptr;
}
bool Click(void* service,void* root,const wchar_t* name,int type,unsigned event) {
    if(!root||!At<std::uint8_t>(root,0x162))return false;
    const auto find=reinterpret_cast<Find>(ownerApi->Symbol(ownerMod,"hangar.ui_find_control"));
    String key;key.data.pointer=name;key.length=static_cast<unsigned>(std::wcslen(name));key.capacity=key.length;
    auto* control=find&&root?find(root,&key,type):nullptr;
    if(!control||At<void*>(control,0x15c)!=root||!At<std::uint8_t>(control,0x162)||!At<std::uint8_t>(control,0x167))return false;
    At<Event>(At<void*>(service),14*4)(service,event,reinterpret_cast<std::uintptr_t>(control)+0x118,control);
    return true;
}
bool ExercisePanel(const SdgoGameTestContextV1* context,void* page){
    using App=void*(__cdecl*)();
    const auto app=reinterpret_cast<App>(ownerApi->Symbol(ownerMod,"game.app"));
    const auto hwnd=app&&app()?At<HWND>(app(),0x3c):nullptr;
    RECT client{};if(!hwnd||!GetClientRect(hwnd,&client)||!client.right||!client.bottom)return false;
    auto click=[&](int x,int y){const auto position=MAKELPARAM(x*client.right/1024,y*client.bottom/768);SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,position);SendMessageW(hwnd,WM_LBUTTONUP,0,position);};
    auto type=[&](const wchar_t* text){for(;*text;++text)SendMessageW(hwnd,WM_CHAR,*text,0);SendMessageW(hwnd,WM_KEYDOWN,VK_RETURN,0);};
    auto select=[&](unsigned i){auto* c=At<void*>(page,0x14c+i*4);float x=At<float>(c,0x148),y=At<float>(c,0x14c);for(auto* p=At<void*>(c,0x15c);p;p=At<void*>(p,0x15c)){x+=At<float>(p,0x148);y+=At<float>(p,0x14c);}click(static_cast<int>(x)+25,static_cast<int>(y)+25);};
    SdgoRgbPaintObservationV1 originalState;ObservePanel(originalState);
    click(970,410);type(L"256");SdgoRgbPaintObservationV1 invalid;ObservePanel(invalid);
    bool ok=invalid.invalid_input&&std::memcmp(invalid.rgb,originalState.rgb,sizeof invalid.rgb)==0;
    context->Assert(ok,"native RGB numeric input rejects 256 without changing preview");
    SendMessageW(hwnd,WM_KEYDOWN,VK_ESCAPE,0);
    click(970,410);type(L"17");click(970,448);type(L"34");click(970,486);type(L"51");
    SdgoRgbPaintObservationV1 numeric;ObservePanel(numeric);
    const bool channels=numeric.rgb[0][0]==17&&numeric.rgb[0][1]==34&&numeric.rgb[0][2]==51;
    context->Assert(channels,"native RGB numeric fields edit all three channels");ok=channels&&ok;
    click(850,410);SdgoRgbPaintObservationV1 slider;ObservePanel(slider);
    const bool moved=slider.rgb[0][0]>80&&slider.rgb[0][0]<110&&slider.rgb[0][1]==34;
    context->Assert(moved,"native RGB slider responds to mouse input");ok=moved&&ok;
    click(990,260);click(900,220);SdgoRgbPaintObservationV1 square;ObservePanel(square);
    const bool picked=std::memcmp(square.rgb[0],slider.rgb[0],3)!=0;context->Assert(picked,"native hue and square change the selected color");ok=picked&&ok;
    click(850,570);SdgoRgbPaintObservationV1 cleared;ObservePanel(cleared);
    context->Assert(!(cleared.present_mask&1),"native clear restores selected material");ok=!(cleared.present_mask&1)&&ok;
    click(940,570);SdgoRgbPaintObservationV1 restored;ObservePanel(restored);
    const bool reset=restored.present_mask==originalState.present_mask&&std::memcmp(restored.rgb,originalState.rgb,sizeof restored.rgb)==0;
    context->Assert(reset,"native restore returns all six regions to entry colors");ok=reset&&ok;
    click(940,640);SdgoRgbPaintObservationV1 nativeMode;ObservePanel(nativeMode);
    context->Assert(!nativeMode.editing,"return to stock palette disables RGB editing");ok=!nativeMode.editing&&ok;
    click(940,640);SdgoRgbPaintObservationV1 reopened;ObservePanel(reopened);
    const bool fresh=reopened.editing&&reopened.present_mask==originalState.present_mask&&std::memcmp(reopened.rgb,originalState.rgb,sizeof reopened.rgb)==0;
    context->Assert(fresh,"reopening RGB starts with current native paint");ok=fresh&&ok;
    const wchar_t* colors[]={L"FF0048",L"FFE0F5",L"FF9BDA",L"FFC0C0",L"FFC0C0",L"FF0080"};
    for(unsigned i=0;i<6;++i){select(i);click(850,530);type(colors[i]);SdgoRgbPaintObservationV1 row;ObservePanel(row);auto* c=At<void*>(page,0x14c+i*4);ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB region input=%u selected=%u rect=%.0f,%.0f,%d,%d rgb=%u,%u,%u",i,row.selected,At<float>(c,0x148),At<float>(c,0x14c),At<int>(c,0x154),At<int>(c,0x158),row.rgb[i][0],row.rgb[i][1],row.rgb[i][2]);}
    select(0);SdgoRgbPaintObservationV1 result;ObservePanel(result);
    bool six=result.present_mask==63&&result.selected==0;
    for(unsigned i=0;i<6;++i)six=six&&result.rgb[i][0]==palette[i].r&&result.rgb[i][1]==palette[i].g&&result.rgb[i][2]==palette[i].b;
    context->Assert(six,"six native paint regions retain independently entered RGB hex colors");ok=six&&ok;
    ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB panel input validation=%s frames=%u",ok?"pass":"fail",result.frames);
    if(ok)regions=63;
    return ok;
}
bool ExpectedSaved(const SdgoRgbPaintObservationV1& value){if(value.saved_mask!=63)return false;const bool demo=GetEnvironmentVariableW(L"SDGO_RGB_DEMO_DIR",nullptr,0)>0;for(unsigned i=0;i<6;++i)if(value.saved_rgb[i][0]!=palette[i].r||value.saved_rgb[i][1]!=(demo&&i==0?64:palette[i].g)||value.saved_rgb[i][2]!=palette[i].b)return false;return true;}
int(__cdecl* selectPoll)(const SdgoGameTestContextV1*)=nullptr;
int __cdecl SelectCase(const SdgoGameTestContextV1* context){
    static bool recorded=false;
    if(!recorded){auto** profile=static_cast<void**>(ownerApi->Symbol(ownerMod,"flow.profile_pointer"));
        if(profile&&*profile){ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB test initial owned selection=%llu",At<std::uint64_t>(*profile,0x1b1));recorded=true;}}
    return selectPoll?selectPoll(context):SDGO_GAME_TEST_FAIL;
}
bool DemoFrames(){
    wchar_t directory[MAX_PATH]{};if(!GetEnvironmentVariableW(L"SDGO_RGB_DEMO_DIR",directory,MAX_PATH))return true;
    static unsigned frame=0;static bool waiting=false;static std::uint64_t next=0;if(frame>=36)return true;
    const auto runtime=GetModuleHandleW(L"game_test_client.dll");const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(runtime,"SdgoGameTestCaptureFrameV1"));const auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(runtime,"SdgoGameTestCaptureStatusV1"));if(!request||!status)return false;
    if(waiting){HRESULT hr=E_PENDING;if(status(&hr)!=3||FAILED(hr))return false;waiting=false;++frame;next=GetTickCount64()+100;if(frame>=36)return true;}
    if(GetTickCount64()<next)return false;
    using App=void*(__cdecl*)();const auto app=reinterpret_cast<App>(ownerApi->Symbol(ownerMod,"game.app"));const auto hwnd=app&&app()?At<HWND>(app(),0x3c):nullptr;RECT r{};if(!hwnd||!GetClientRect(hwnd,&r)||!r.right||!r.bottom)return false;
    const auto xy=[&](int x,int y){return MAKELPARAM(x*r.right/1024,y*r.bottom/768);};
    if(frame==6)SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,xy(795,448));
    if(frame>=6&&frame<=25)SendMessageW(hwnd,WM_MOUSEMOVE,MK_LBUTTON,xy(795+static_cast<int>((frame-6)*38/19),448));
    if(frame==26){SendMessageW(hwnd,WM_LBUTTONUP,0,xy(833,448));SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,xy(970,448));SendMessageW(hwnd,WM_LBUTTONUP,0,xy(970,448));SendMessageW(hwnd,WM_CHAR,L'6',0);SendMessageW(hwnd,WM_CHAR,L'4',0);SendMessageW(hwnd,WM_KEYDOWN,VK_RETURN,0);}
    wchar_t file[MAX_PATH]{};swprintf_s(file,L"%ls\\frame-%03u.bmp",directory,frame);waiting=request(file)!=0;return false;
}
bool NativeSaveClick(){using App=void*(__cdecl*)();const auto app=reinterpret_cast<App>(ownerApi->Symbol(ownerMod,"game.app"));const auto hwnd=app&&app()?At<HWND>(app(),0x3c):nullptr;RECT r{};if(!hwnd||!GetClientRect(hwnd,&r)||!r.right||!r.bottom)return false;const auto xy=MAKELPARAM(825*r.right/1024,642*r.bottom/768);SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,xy);SendMessageW(hwnd,WM_LBUTTONUP,0,xy);return true;}
bool ReviewPeer(unsigned pilot){
    using Pilot=unsigned(__thiscall*)(void*,unsigned);using FindUnit=void*(__thiscall*)(void*,unsigned);
    auto** manager=static_cast<void**>(ownerApi->Symbol(ownerMod,"game.unit_manager"));
    const auto object=reinterpret_cast<Pilot>(ownerApi->Symbol(ownerMod,"um.object_id_from_pilot"));const auto find=reinterpret_cast<FindUnit>(ownerApi->Symbol(ownerMod,"um.find_object"));
    __try{auto* um=manager?*manager:nullptr;if(!um||!object||!find)return false;auto* unit=find(um,object(um,pilot));auto* camera=At<void*>(um,0x82c8);
        if(!unit||!camera||At<unsigned>(unit,0x2b68)!=pilot||At<int>(unit,0x38)<=0)return false;
        if(At<void*>(camera,0x344)!=unit){*reinterpret_cast<void**>(static_cast<unsigned char*>(camera)+0x344)=unit;using Reset=void(__thiscall*)(void*,float);auto reset=At<Reset>(At<void*>(camera),21*sizeof(void*));if(reinterpret_cast<std::uintptr_t>(reset)-reinterpret_cast<std::uintptr_t>(ownerApi->host_base)!=0x976f0)return false;reset(camera,1.f);const float look=At<float>(camera,0x358);*reinterpret_cast<float*>(static_cast<unsigned char*>(camera)+0x34c)=look+60.f;*reinterpret_cast<float*>(static_cast<unsigned char*>(camera)+0x350)=300.f;}return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool SceneProof(std::uint64_t uid,const wchar_t* directory,const wchar_t* name,bool battle){
    const auto observe=reinterpret_cast<SdgoRgbPaintSceneObserveV1Fn>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),SDGO_RGB_PAINT_SCENE_EXPORT));
    SdgoRgbPaintSceneObservationV1 value;value.unit=uid;
    if(!observe||!observe(&value)||(battle?value.battle_draws:value.room_draws)<30||(battle?value.battle_mask:value.room_mask)!=63)return false;
    wchar_t file[MAX_PATH]{};swprintf_s(file,L"%ls\\%ls.bin",directory,name);FILE* output=nullptr;
    if(_wfopen_s(&output,file,L"wb")||!output)return false;const bool written=std::fwrite(&value,1,sizeof value,output)==sizeof value;std::fclose(output);
    const auto capture=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));
    swprintf_s(file,L"%ls\\%ls.bmp",directory,name);return written&&capture&&capture(file);
}
void* TargetUnit(unsigned pilot){
    using FindUnit=void*(__thiscall*)(void*,unsigned);using Object=unsigned(__thiscall*)(void*,unsigned);
    auto** manager=static_cast<void**>(ownerApi->Symbol(ownerMod,"game.unit_manager"));
    const auto find=reinterpret_cast<FindUnit>(ownerApi->Symbol(ownerMod,"um.find_object"));const auto object=reinterpret_cast<Object>(ownerApi->Symbol(ownerMod,"um.object_id_from_pilot"));
    __try{if(!manager||!*manager||!find||!object)return nullptr;auto* unit=pilot?find(*manager,object(*manager,pilot)):At<void*>(*manager,0x88);
        if(!unit||find(*manager,At<unsigned>(unit,4))!=unit||(!pilot&&!At<std::uint8_t>(unit,0x31e9)))return nullptr;return unit;
    }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
void TransformProof(const SdgoGameTestContextV1* context,std::uint64_t uid,const wchar_t* directory,unsigned pilot){
    wchar_t first[16]{},second[16]{};if(!GetEnvironmentVariableW(L"SDGO_RGB_FORM_A",first,16)||!GetEnvironmentVariableW(L"SDGO_RGB_FORM_B",second,16))return;
    const auto formA=static_cast<unsigned>(_wcstoui64(first,nullptr,10)),formB=static_cast<unsigned>(_wcstoui64(second,nullptr,10));if(!formA||!formB||formA==formB)return;
    auto* unit=TargetUnit(pilot);if(!unit)return;auto* spec=At<void*>(unit,0x9c4);auto* move=At<void*>(unit,0x9c8);if(!spec||!move||At<void*>(move,0x21c)!=unit)return;
    // Remote actors use a different movement state layout. Their committed
    // definition and fresh rendered materials are the observer's evidence.
    const auto type=At<unsigned>(spec),mode=pilot?0:At<unsigned>(move,0x2c0);static unsigned previous=0,stage=0,issued=0;static std::uint64_t settled=0,nextRequest=0;
    if(type!=previous){ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB native form=%u movement_mode=%u main=%p alternate=%p",type,mode,At<void*>(unit,0x34),At<void*>(unit,0xab4));previous=type;settled=GetTickCount64();}
    const unsigned expected[]={formA,formB,formA};const wchar_t* names[]={L"base",L"form-1",L"form-2"};
    unsigned mainMask=0;for(unsigned i=0;i<6;++i){sdgo::rgb_paint::Rgb actual;if(renderer.ReadRendered(At<void*>(unit,0x34),i,actual)&&actual==palette[i])mainMask|=1u<<i;}
    if(stage<3&&type==expected[stage]&&!mode&&mainMask==63&&GetTickCount64()-settled>3000&&SceneProof(uid,directory,names[stage],true)){
        ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB native form proof=%u actual=%u",stage,type);++stage;settled=GetTickCount64();
    }
    if(pilot||!stage||stage>=3||issued>=stage||mode||GetTickCount64()<nextRequest)return;
    wchar_t command[MAX_PATH]{};swprintf_s(command,L"%ls\\transform-%u",directory,stage);if(GetFileAttributesW(command)==INVALID_FILE_ATTRIBUTES)return;
    using Request=unsigned char(__thiscall*)(void*);const auto request=reinterpret_cast<Request>(ownerApi->Symbol(ownerMod,"test.apsalus_transform_request"));
    if(!request){context->Assert(0,"original native transform admission binding is available");return;}
    nextRequest=GetTickCount64()+500;
    if(request(move)){issued=stage;settled=GetTickCount64();ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB native transform admitted step=%u",issued);}
}
int __cdecl PeerCase(const SdgoGameTestContextV1* context){
    wchar_t sceneId[32]{},directory[MAX_PATH]{};if(GetEnvironmentVariableW(L"SDGO_RGB_PEER_UID",sceneId,32)&&GetEnvironmentVariableW(L"SDGO_RGB_PEER_RECEIPT",directory,MAX_PATH)){
        if(auto* slash=std::wcsrchr(directory,L'\\'))*slash=0;else return SDGO_GAME_TEST_FAIL;
        static bool roomWritten=false;if(!roomWritten)roomWritten=SceneProof(_wcstoui64(sceneId,nullptr,10),directory,L"room",false);
    }
    const auto app=reinterpret_cast<void*(__cdecl*)()>(ownerApi->Symbol(ownerMod,"game.app"));const bool battle=app&&app()&&At<unsigned>(app(),0x4a0)==22;static bool entered=false;static std::uint64_t enteredAt=0;
    // Cosmetic proof does not claim the coordinator's separate AI assertions.
    if(!battle||!entered){const int result=friendPoll(context);if(result==SDGO_GAME_TEST_FAIL)return result;}if(!battle)return SDGO_GAME_TEST_WAIT;if(!entered)enteredAt=GetTickCount64();entered=true;
    wchar_t id[32]{},path[MAX_PATH]{},pilot[32]{};if(!GetEnvironmentVariableW(L"SDGO_RGB_PEER_UID",id,32)||!GetEnvironmentVariableW(L"SDGO_RGB_PEER_RECEIPT",path,MAX_PATH))return SDGO_GAME_TEST_FAIL;
    if(GetEnvironmentVariableW(L"SDGO_RGB_PEER_PILOT",pilot,32)&&!ReviewPeer(static_cast<unsigned>(_wcstoui64(pilot,nullptr,10))))return SDGO_GAME_TEST_WAIT;
    TransformProof(context,_wcstoui64(id,nullptr,10),directory,static_cast<unsigned>(_wcstoui64(pilot,nullptr,10)));
    SdgoRgbPaintObservationV1 value;value.unit=_wcstoui64(id,nullptr,10);const auto observe=reinterpret_cast<Observe>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),SDGO_RGB_PAINT_OBSERVE_EXPORT));if(!observe||!observe(&value))return SDGO_GAME_TEST_FAIL;
    if(context->tick%120==0){char message[200];std::snprintf(message,sizeof message,"RGB peer reads=%u revision=%llu battle_draws=%u material_mask=%u",value.reads,value.revision,value.battle_draws,value.battle_material_mask);context->Observe(message);}
    if(!ExpectedSaved(value)||!value.reads||value.battle_draws<30||value.battle_material_mask!=63||GetTickCount64()-enteredAt<10000)return SDGO_GAME_TEST_WAIT;
    static bool written=false;if(!written){FILE* file=nullptr;if(_wfopen_s(&file,path,L"wb")||!file)return SDGO_GAME_TEST_FAIL;written=std::fwrite(&value,1,sizeof value,file)==sizeof value;std::fclose(file);context->Assert(written,"other player unit uses exact server RGB in genuine native battle materials");
        wchar_t capture[MAX_PATH]{};const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));if(request&&GetEnvironmentVariableW(L"SDGO_RGB_TEST_CAPTURE",capture,MAX_PATH))request(capture);}
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl CaptureCase(const SdgoGameTestContextV1* context) {
    static unsigned step=0;
    static bool openedHangar=false;
    static ULONGLONG at=0;
    wchar_t preview[4]{};
    const bool loginPreview=GetEnvironmentVariableW(L"SDGO_RGB_TEST_LOGIN_PREVIEW",preview,4)>0;
    wchar_t mode[16]{};GetEnvironmentVariableW(L"SDGO_RGB_TEST_MODE",mode,16);const bool saving=!std::wcscmp(mode,L"save"),relogin=!std::wcscmp(mode,L"relogin"),preflight=!std::wcscmp(mode,L"preflight");
    if(loginPreview&&!step){step=2;at=GetTickCount64();}
    if(!step) {
        auto* manager=Service("flow.battleship_lobby_manager_vtable");
        if(manager&&Click(manager,At<void*>(manager,0x64),L"FactoryBtn",4,0x101)) {step=1;at=GetTickCount64();}
        else if(!openedHangar) {
            auto* menu=Service("flow.hangar_downmenu_vtable");
            if(menu)openedHangar=Click(menu,At<void*>(menu,0x44),L"MyRoomRadio",12,0x301);
        }
        return SDGO_GAME_TEST_WAIT;
    }
    if(step==1) {
        auto* menu=Service("test.factory_tab_menu_vtable");
        if(menu&&At<std::uint8_t>(menu,0xac)&&GetTickCount64()-at>1500&&
           Click(menu,At<void*>(menu,0xd0),L"StickerRadio",12,0x301)) {step=2;at=GetTickCount64();}
        return SDGO_GAME_TEST_WAIT;
    }
    const auto test=GetModuleHandleW(L"game_test_client.dll");
    const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(test,"SdgoGameTestCaptureFrameV1"));
    const auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(test,"SdgoGameTestCaptureStatusV1"));
    auto* page=Service("rgb_paint.page_vtable");
    if((!panelTest&&!regions)||!request||!status)return SDGO_GAME_TEST_WAIT;
    if(!loginPreview) {
        if(!page||!At<std::uint8_t>(page,0xac))return SDGO_GAME_TEST_WAIT;
        auto* root=At<void*>(page,0xd0);
        if(!root||!At<std::uint8_t>(root,0x162))return SDGO_GAME_TEST_WAIT;
    }
    if(GetTickCount64()-at<(loginPreview?20000u:3000u))return SDGO_GAME_TEST_WAIT;
    if(panelTest&&!panelInputs){
        SdgoRgbPaintObservationV1 observed;if(!ObservePanel(observed)||(!preflight&&!observed.authority_ready))return SDGO_GAME_TEST_WAIT;
        beforeSaveRevision=observed.revision;
        if(preflight){panelValid=!observed.authority_ready&&observed.state==1&&!observed.reads;context->Assert(panelValid,"missing server authority leaves native panel waiting without claiming saved state");if(panelValid)ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB preflight validation=pass frames=%u",observed.frames);}
        else if(relogin){panelValid=ExpectedSaved(observed)&&observed.reads>0&&observed.regions==6&&observed.matched_material_mask==63&&observed.draws>0;context->Assert(panelValid,"new client login restores exact server RGB and renders all six regions");if(panelValid){regions=63;ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB relogin validation=pass revision=%llu draws=%u",observed.revision,observed.draws);}}
        else panelValid=ExercisePanel(context,page);
        panelInputs=true;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;
    }
    if(saving&&saveStep<2){
        if(!saveStep&&!DemoFrames())return SDGO_GAME_TEST_WAIT;
        SdgoRgbPaintObservationV1 value;if(!ObservePanel(value))return SDGO_GAME_TEST_WAIT;
        if(!saveStep){NativeSaveClick();SdgoRgbPaintObservationV1 confirm;ObservePanel(confirm);context->Assert(confirm.state==2,"native save requires preview confirmation");if(confirm.state!=2)return SDGO_GAME_TEST_FAIL;NativeSaveClick();saveStep=1;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;}
        if(value.state==3||value.state==4||value.revision<=beforeSaveRevision)return SDGO_GAME_TEST_WAIT;
        const bool saved=value.state==1&&ExpectedSaved(value)&&value.regions==6&&value.matched_material_mask==63&&value.draws>0;context->Assert(saved,"native save receives authoritative exact RGB and revision advancement");if(!saved)return SDGO_GAME_TEST_FAIL;
        ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB save validation=pass revision=%llu draws=%u unit=%llu",value.revision,value.draws,value.unit);saveStep=2;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;
    }
    if(step==2) {
        if(!loginPreview) {
            auto* root=At<void*>(page,0xd0);
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB page rect=%.0f,%.0f,%d,%d unit=%u",At<float>(root,0x148),At<float>(root,0x14c),At<int>(root,0x154),At<int>(root,0x158),At<unsigned>(page,0x17c));
            auto* head=At<void*>(root,0x23c);auto* link=head?At<void*>(head):nullptr;
            for(unsigned i=0;link&&link!=head&&i<256;++i){auto* c=At<void*>(link,8);const auto name=At<String>(c,0x118);
                if(name.length<128)ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB control=%.*ls rect=%.0f,%.0f,%d,%d",static_cast<int>(name.length),name.capacity<8?name.data.buffer:name.data.pointer,At<float>(c,0x148),At<float>(c,0x14c),At<int>(c,0x154),At<int>(c,0x158));link=At<void*>(link);}
        }
        const auto observe=reinterpret_cast<SdgoLoginShowcaseObserveV1Fn>(GetProcAddress(GetModuleHandleW(L"startup_crash_fix.dll"),SDGO_LOGIN_SHOWCASE_OBSERVE_EXPORT));
        SdgoLoginShowcaseObservationV1 snapshot{};snapshot.size=sizeof snapshot;
        if(observe&&observe(&snapshot))ownerApi->Log(ownerMod,SDGO_LOG_INFO,"RGB target=%p displayed=%p updates=%u",targetModel,reinterpret_cast<void*>(snapshot.model),materialUpdates);
        wchar_t path[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"SDGO_RGB_TEST_CAPTURE",path,MAX_PATH)&&request(path))step=3;
        return SDGO_GAME_TEST_WAIT;
    }
    HRESULT hr=E_PENDING;const auto state=status(&hr);
    if(state!=3&&state!=4)return SDGO_GAME_TEST_WAIT;
    context->Assert(state==3&&SUCCEEDED(hr),loginPreview?"genuine RGB login-preview backbuffer captured":"genuine RGB paint-page backbuffer captured");
    if(!preflight)context->Assert(regions==63,"all six native paint material regions accepted RGB");
    return state==3&&SUCCEEDED(hr)&&(preflight||regions==63)&&(!panelTest||panelValid)?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
}
SDGO_MOD_ENTRY {
    wchar_t run[64]{},role[16]{};
    const auto n = GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",run,64);
    const auto r = GetEnvironmentVariableW(L"SDGO_GAME_TEST_ROLE",role,16);
    if (!n || n >= 64 || !r || r >= 16 || std::wcscmp(role,L"client") ||
        !api || !api->HookCode || !api->Log || !renderer.Bind(api,mod)) return 0;
    ownerApi=api;ownerMod=mod;
    wchar_t panelMode[4]{};panelTest=GetEnvironmentVariableW(L"SDGO_RGB_TEST_PANEL",panelMode,4)>0;
    if(panelTest)return 1;
    if(auto thread=CreateThread(nullptr,0,&DiagnosticCapture,nullptr,0,nullptr))CloseHandle(thread);
    return api->HookCode(mod,"rgb_paint.unit_style",reinterpret_cast<void*>(&Probe),reinterpret_cast<void**>(&original))&&
        api->HookCode(mod,"rgb_paint.native_color",reinterpret_cast<void*>(&Paint),reinterpret_cast<void**>(&originalColor))&&
        api->HookCode(mod,"rgb_paint.material_update",reinterpret_cast<void*>(&UpdateMaterials),reinterpret_cast<void**>(&originalUpdate))&&
        api->HookCode(mod,"rgb_paint.observed_material_update",reinterpret_cast<void*>(&UpdateObservedMaterials),reinterpret_cast<void**>(&originalObservedUpdate));
}
SDGO_GAME_TEST_PROVIDER_ENTRY {
    static std::array<SdgoGameTestCaseV1,5> cases{};
    const auto runtime=GetModuleHandleW(L"game_test_client.dll");
    const auto get=reinterpret_cast<SdgoGameTestGetProviderV1Fn>(GetProcAddress(runtime,SDGO_GAME_TEST_PROVIDER_EXPORT));
    const auto originalProvider=get?get():nullptr;
    if(!originalProvider||originalProvider->count<3)return nullptr;
    if(GetEnvironmentVariableW(L"SDGO_RGB_PEER_UID",nullptr,0)){
        for(unsigned i=0;i<3;++i)cases[i]=originalProvider->cases[i];for(unsigned i=0;i<originalProvider->count;++i)if(!std::strcmp(originalProvider->cases[i].id,"friends.session"))friendPoll=originalProvider->cases[i].poll;if(!friendPoll)return nullptr;
        cases[3]={"friends.session",600000,&PeerCase};static const SdgoGameTestProviderV1 peer{sizeof peer,SDGO_GAME_TEST_VERSION,"rgb-paint","client",4,cases.data()};return &peer;
    }
    wchar_t preview[4]{};
    if(GetEnvironmentVariableW(L"SDGO_RGB_TEST_LOGIN_PREVIEW",preview,4)>0) {
        cases[0]=originalProvider->cases[0];
        cases[1]={"rgb_paint.native_capture",30000,&CaptureCase};
        static const SdgoGameTestProviderV1 previewProvider{sizeof previewProvider,SDGO_GAME_TEST_VERSION,"rgb-paint","client",2,cases.data()};
        return &previewProvider;
    }
    for(unsigned i=0;i<3;++i)cases[i]=originalProvider->cases[i];
    unsigned count=3;
    for(unsigned i=3;i<originalProvider->count;++i)if(!std::strcmp(originalProvider->cases[i].id,"setup.active_unit")){selectPoll=originalProvider->cases[i].poll;cases[count++]={"setup.active_unit",120000,&SelectCase};}
    cases[count++]={"rgb_paint.native_capture",120000,&CaptureCase};
    static SdgoGameTestProviderV1 provider{sizeof provider,SDGO_GAME_TEST_VERSION,"rgb-paint","client",0,cases.data()};provider.count=count;
    return &provider;
}
