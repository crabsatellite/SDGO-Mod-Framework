// Unshipped, read-only observer of genuine native unit-model styling. This
// does not create a unit, consume stickers, alter geometry or emulate replies.
#include "sdgo/surface_decal_native.h"
#include "sdgo/surface_decal_observer.h"
#include <windows.h>
#include <cstdio>
#include <exception>
#include <cstring>
#include <mutex>
#include <map>
#include "../mods/unit_appearance_client/decal/render.h"
#include "sdgo/game_test.h"
#include "sdgo/rgb_paint_observer.h"
#include "sdgo/pvp_ai_client_observer.h"
#undef small
#include "../mods/hangar_manager/native_ui.h"
#include "../mods/unit_appearance_client/decal/native_ui.h"
#include "../mods/game_test_client/shop_pointer_input.h"

namespace {
using Style=void(__cdecl*)(void*,const std::uint16_t*,std::uint16_t,const std::uint32_t*);
Style original=nullptr;
sdgo::surface_decal::native::GeometryReader reader;
sdgo::surface_decal::ui::Page editorPage;
const SdgoApi* ownerApi=nullptr;
SdgoMod* ownerMod=nullptr;
unsigned observed=0;
bool runtimeMode=false,expectBackend=false,saveTest=false,reloadTest=false,battleTest=false;
int(__cdecl* friendPoll)(const SdgoGameTestContextV1*)=nullptr;
using Draw=void(__thiscall*)(void*);
Draw direct=nullptr,shader=nullptr;
using RenderCallback=void(__thiscall*)(void*,void*,unsigned);
RenderCallback callback=nullptr;
void** renderer=nullptr;
std::mutex gate;
void* target=nullptr;
std::vector<sdgo::surface_decal::native::Node> cached;
std::map<void*,std::vector<sdgo::surface_decal::native::Node>> modelCache;
unsigned drawn=0;
std::uint64_t captureAt=0;
bool captureReady=false,captureComplete=false;
template<class T>T At(const void* p,unsigned offset=0){T v{};std::memcpy(&v,static_cast<const std::uint8_t*>(p)+offset,sizeof v);return v;}
void RenderProbe(void* rigid) {
    using namespace sdgo::surface_decal;
    std::lock_guard<std::mutex> hold(gate);
    target=At<void*>(rigid,0x118);
    const auto known=modelCache.find(target);
    if(known==modelCache.end()||!renderer||!*renderer)return;
    const auto& meshNodes=known->second;
    const auto* first=At<void**>(target,0x124);const auto* last=At<void**>(target,0x128);
    if(!first||last<first||last-first>4096)return;
    std::uint32_t index=0;while(first+index<last&&first[index]!=rigid)++index;
    const auto node=std::find_if(meshNodes.begin(),meshNodes.end(),[&](const native::Node& n){return n.index==index;});
    if(node==meshNodes.end())return;
    auto* device=At<IDirect3DDevice9*>(*renderer,8);View view;
    const auto world=At<std::array<float,16>>(rigid,0x130);
    if(!render::Capture(device,world,view))return;
    Ray ray;Hit hit;
    if(!ScreenRay(view,view.x+view.width*0.5,view.y+view.height*0.5,ray)||Pick(node->mesh,ray,hit)!=Result::ok)return;
    Placement placement;Patch patch;
    if(MakePlacement(node->mesh,hit,node->geometry,node->index,{1,0,0},3,3,0.75,placement)!=Result::ok||
       Project(node->mesh,node->geometry,node->index,placement,patch)!=Result::ok)return;
    IDirect3DTexture9* texture=nullptr;
    if(FAILED(device->CreateTexture(32,32,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)))return;
    D3DLOCKED_RECT pixels{};
    if(SUCCEEDED(texture->LockRect(0,&pixels,nullptr,0))){
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
            reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(pixels.pBits)+y*pixels.Pitch)[x]=
                (x<7||y<7||(y>=13&&y<19&&x<24))?0xFFFFFF00:0xFF203060;
        texture->UnlockRect(0);
        if(render::Draw(device,texture,patch,view)&&++drawn==1){
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface actual draw pass triangles=%u viewport=%.0f,%.0f,%.0f,%.0f",static_cast<unsigned>(patch.vertices.size()/3),view.x,view.y,view.width,view.height);
        }
    }
    texture->Release();
    const auto test=GetModuleHandleW(L"game_test_client.dll");
    const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(test,"SdgoGameTestCaptureFrameV1"));
    const auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(test,"SdgoGameTestCaptureStatusV1"));
    if(request&&status&&captureReady&&drawn>=1&&!captureAt){
        wchar_t path[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",path,MAX_PATH)&&request(path))captureAt=GetTickCount64();
    } else if(captureAt&&captureAt!=UINT64_MAX&&status){
        HRESULT hr=E_PENDING;const auto state=status(&hr);
        if(state==3||state==4){ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface capture state=%ld result=%08lX",state,static_cast<unsigned long>(hr));captureAt=UINT64_MAX;captureComplete=state==3&&SUCCEEDED(hr);}
    }
}
void __fastcall Direct(void* self,void*) {direct(self);try{RenderProbe(self);}catch(...) {}}
void __fastcall Shader(void* self,void*) {shader(self);try{RenderProbe(self);}catch(...) {}}
void __fastcall Callback(void* self,void*,void* entry,unsigned flags) {
    callback(self,entry,flags);
    if(At<std::uint8_t>(entry,0x14)==0)try{RenderProbe(self);}catch(...) {}
}
void* Service(const char* symbol) {
    auto** managed=static_cast<void**>(ownerApi->Symbol(ownerMod,"flow.service_managed_pointer"));
    const auto expected=ownerApi->Symbol(ownerMod,symbol);if(!managed||!*managed||!expected)return nullptr;
    auto* head=At<void*>(*managed,0x44);if(!head)return nullptr;
    auto* link=At<void*>(head);
    for(unsigned i=0;link&&link!=head&&i<512;++i){auto* service=At<void*>(link,8);if(service&&At<void*>(service)==expected)return service;link=At<void*>(link);}return nullptr;
}
template<class T>T SafeAt(const void* p,unsigned offset=0)noexcept{
    __try{return p?At<T>(p,offset):T{};}__except(EXCEPTION_EXECUTE_HANDLER){return T{};}
}
bool ExitFactory(){
    using String=sdgo::hangar::ui::NativeString;
    using Find=sdgo::hangar::ui::Bindings::Find;using Event=void(__stdcall*)(void*,unsigned,std::uintptr_t,void*);
    const auto find=reinterpret_cast<Find>(ownerApi->Symbol(ownerMod,"hangar.ui_find_control"));
    auto** managed=static_cast<void**>(ownerApi->Symbol(ownerMod,"flow.service_managed_pointer"));
    auto* head=SafeAt<void*>(SafeAt<void*>(managed),0x44);auto* link=SafeAt<void*>(head);
    String key;std::memcpy(key.data.small,L"ExitBtn",16);key.length=7;
    for(unsigned guard=0;link&&link!=head&&guard<512;++guard){
        auto* service=SafeAt<std::uint8_t*>(link,8);link=SafeAt<void*>(link);
        if(!service)continue;
        for(unsigned slot=0;slot<3;++slot){
            auto* tree=SafeAt<void*>(service,0x30+slot*4);if(SafeAt<std::uint8_t>(tree,0x2d)!=1)continue;
            std::array<void*,32> stack{};unsigned depth=0;auto* root=SafeAt<void*>(tree,4);if(root&&root!=tree)stack[depth++]=root;
            for(unsigned count=0;depth&&count<64;++count){
                auto* node=stack[--depth];if(!node||node==tree||SafeAt<std::uint8_t>(node,0x2d))continue;
                auto* dialog=SafeAt<void*>(node,0x28);
                if(dialog&&SafeAt<std::uint8_t>(dialog,0x162)){
                    auto* control=find(dialog,&key,4);
                    if(control&&SafeAt<void*>(control,0x15c)==dialog&&SafeAt<std::uint8_t>(control,0x162)&&SafeAt<std::uint8_t>(control,0x167)){
                        const auto event=SafeAt<Event>(SafeAt<void*>(service),14*4);
                        if(event){event(service,0x101,reinterpret_cast<std::uintptr_t>(control)+0x118,control);return true;}
                    }
                }
                for(unsigned offset:{0u,8u}){auto* child=SafeAt<void*>(node,offset);if(child&&child!=tree&&depth<stack.size())stack[depth++]=child;}
            }
        }
    }
    return false;
}
int CaptureScene(const wchar_t* name){
    wchar_t directory[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"SDGO_SURFACE_DEMO_DIR",directory,MAX_PATH))return 1;
    static std::wstring active;static std::uint64_t since=0;static bool requested=false,done=false;
    if(active!=name){active=name;since=GetTickCount64();requested=done=false;}
    if(done)return 1;
    if(GetTickCount64()-since<700)return 0;
    const auto runtime=GetModuleHandleW(L"game_test_client.dll");
    const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(runtime,"SdgoGameTestCaptureFrameV1"));
    const auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(runtime,"SdgoGameTestCaptureStatusV1"));
    if(!request||!status)return -1;
    if(!requested){const auto path=std::wstring(directory)+L"\\"+name+L".bmp";requested=request(path.c_str())!=0;return requested?0:-1;}
    HRESULT result=E_PENDING;const auto state=status(&result);
    if(state==4)return -1;if(state!=3)return 0;
    done=SUCCEEDED(result);return done?1:-1;
}
int __cdecl BattleCase(const SdgoGameTestContextV1* context){
    using namespace sdgo::surface_decal;
    static std::uint64_t entered=0;static std::uint32_t initialDraws=0;
    const auto app=reinterpret_cast<void*(__cdecl*)()>(ownerApi->Symbol(ownerMod,"game.app"));
    if(!app||!app()||At<unsigned>(app(),0x4a0)!=22)return SDGO_GAME_TEST_WAIT;
    const auto runtime=GetModuleHandleW(L"unit_appearance_client.dll");
    const auto observe=reinterpret_cast<test::Observe>(GetProcAddress(runtime,test::observeExport));
    test::Snapshot snapshot;if(!observe||!observe(&snapshot))return SDGO_GAME_TEST_FAIL;
    if(!entered){entered=GetTickCount64();initialDraws=snapshot.battleDraws;}
    if(context->tick%120==0){char text[140];std::snprintf(text,sizeof text,"battle placement waiting; models=%u positioned=%u draws=%u",snapshot.battleModels,snapshot.battlePositioned,snapshot.battleDraws);context->Observe(text);}
    if(!snapshot.battleModels||!snapshot.battlePositioned||snapshot.battleDraws-initialDraws<30||GetTickCount64()-entered<5000)return SDGO_GAME_TEST_WAIT;
    const auto test=GetModuleHandleW(L"game_test_client.dll");
    const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(test,"SdgoGameTestCaptureFrameV1"));
    const auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(test,"SdgoGameTestCaptureStatusV1"));
    if(!request||!status)return SDGO_GAME_TEST_FAIL;
    if(!captureAt){wchar_t path[MAX_PATH]{};if(GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",path,MAX_PATH)&&request(path))captureAt=GetTickCount64();}
    HRESULT result=E_PENDING;const auto captured=status(&result);
    if(captured==4)return SDGO_GAME_TEST_FAIL;
    if(captured!=3||FAILED(result))return SDGO_GAME_TEST_WAIT;
    ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface runtime battle pass; models=%u positioned=%u draws=%u",snapshot.battleModels,snapshot.battlePositioned,snapshot.battleDraws);
    context->Assert(1,"the real native battle actor draws the server-saved placement using its exact owned-unit identity");return SDGO_GAME_TEST_PASS;
}
bool CombinedRgb(const SdgoGameTestContextV1* context,std::uint64_t uid,bool room){
    if(!GetEnvironmentVariableW(L"SDGO_APPEARANCE_COMBINED",nullptr,0))return true;
    const auto observe=reinterpret_cast<SdgoRgbPaintSceneObserveV1Fn>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),SDGO_RGB_PAINT_SCENE_EXPORT));
    SdgoRgbPaintSceneObservationV1 value;value.unit=uid;
    if(!observe||!observe(&value))return false;
    const auto mask=room?value.room_mask:value.battle_mask,draws=room?value.room_draws:value.battle_draws;
    if(context->tick%120==0){char text[192];std::snprintf(text,sizeof text,"combined RGB unit=%llu revision=%llu current_draws=%u matched_regions=%u",uid,value.revision,draws,mask);context->Observe(text);}
    if(!value.revision||!draws||mask!=63)return false;
    wchar_t path[MAX_PATH]{};if(!GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_RECEIPT",path,MAX_PATH))return false;
    FILE* file=nullptr;const auto name=std::wstring(path)+L".rgb";
    if(_wfopen_s(&file,name.c_str(),L"wb")||!file)return false;
    const bool written=std::fwrite(&value,1,sizeof value,file)==sizeof value;std::fclose(file);return written;
}
int AppearanceSwitchCase(const SdgoGameTestContextV1* context,std::uint64_t uid,const wchar_t* path){
    wchar_t flag[4]{};GetEnvironmentVariableW(L"SDGO_APPEARANCE_RGB",flag,4);const bool rgb=flag[0]!=L'0';
    GetEnvironmentVariableW(L"SDGO_APPEARANCE_DECALS",flag,4);const bool decals=flag[0]!=L'0';
    const auto module=GetModuleHandleW(L"unit_appearance_client.dll");
    const auto readRgb=reinterpret_cast<SdgoRgbPaintSceneObserveV1Fn>(GetProcAddress(module,SDGO_RGB_PAINT_SCENE_EXPORT));
    const auto readDecals=reinterpret_cast<sdgo::surface_decal::test::RoomTarget>(GetProcAddress(module,sdgo::surface_decal::test::roomExport));
    SdgoRgbPaintSceneObservationV1 colors;colors.unit=uid;
    sdgo::surface_decal::test::RoomSnapshot stickers;
    const bool hasRgb=readRgb&&readRgb(&colors),hasDecals=readDecals&&readDecals(uid,&stickers);
    if(rgb?(!hasRgb||!colors.revision||!colors.room_draws||colors.room_mask!=63):(hasRgb&&colors.room_draws))return SDGO_GAME_TEST_WAIT;
    if(decals?(!hasDecals||!stickers.triangles||stickers.draws<30):(hasDecals&&stickers.draws))return SDGO_GAME_TEST_WAIT;
    static std::uint64_t since=GetTickCount64();if(GetTickCount64()-since<3000)return SDGO_GAME_TEST_WAIT;
    static bool written=false;if(!written){
        FILE* file=nullptr;const auto name=std::wstring(path)+L".switch";
        if(_wfopen_s(&file,name.c_str(),L"wb")||!file)return SDGO_GAME_TEST_FAIL;
        std::fprintf(file,"RGB=%u decals=%u RGBdraws=%u RGBmask=%u decalDraws=%u\n",rgb?1u:0u,decals?1u:0u,colors.room_draws,colors.room_mask,stickers.draws);std::fclose(file);
        wchar_t capture[MAX_PATH]{};GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",capture,MAX_PATH);
        const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));
        if(!request||!request(capture))return SDGO_GAME_TEST_FAIL;written=true;
        context->Assert(1,"independent switches control actual current-room RGB and sticker rendering");
    }
    return SDGO_GAME_TEST_WAIT;
}
int RoomPeerCase(const SdgoGameTestContextV1* context){
    using namespace sdgo::surface_decal;
    const int result=friendPoll(context);if(result==SDGO_GAME_TEST_FAIL)return result;
    const auto app=reinterpret_cast<void*(__cdecl*)()>(ownerApi->Symbol(ownerMod,"game.app"));
    if(!app||!app()||At<unsigned>(app(),0x4a0)!=18)return SDGO_GAME_TEST_WAIT;
    wchar_t id[32]{},path[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_UID",id,32)||!GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_RECEIPT",path,MAX_PATH))return SDGO_GAME_TEST_FAIL;
    if(GetEnvironmentVariableW(L"SDGO_APPEARANCE_SWITCH_TEST",nullptr,0))return AppearanceSwitchCase(context,_wcstoui64(id,nullptr,10),path);
    const auto observe=reinterpret_cast<test::RoomTarget>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),test::roomExport));
    test::RoomSnapshot snapshot;if(!observe||!observe(_wcstoui64(id,nullptr,10),&snapshot))return SDGO_GAME_TEST_FAIL;
    static std::uint64_t entered=GetTickCount64();static bool diagnosticCapture=false;
    if(!diagnosticCapture&&GetTickCount64()-entered>10000){
        wchar_t capture[MAX_PATH]{};GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",capture,MAX_PATH);
        const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));
        const auto name=std::wstring(capture)+L".initial.bmp";
        if(request&&capture[0])diagnosticCapture=request(name.c_str())!=0;
    }
    if(context->tick%120==0){char message[200];std::snprintf(message,sizeof message,"room target models=%u triangles=%u draws=%u nodes=%u pending=%u matching=%u authority=%u",snapshot.models,snapshot.triangles,snapshot.draws,snapshot.nodes,snapshot.pendingGeometry,snapshot.matchingStickers,snapshot.appearance[0]);context->Observe(message);}
    Appearance a;
    // The second native form may remain unloaded until transformation. Only
    // require usable geometry and sustained draws from a loaded target form.
    if(!wire::Decode(snapshot.appearance.data(),snapshot.appearance.size(),a)||!snapshot.models||!snapshot.matchingStickers||!snapshot.triangles||snapshot.pendingGeometry>=snapshot.models)return SDGO_GAME_TEST_WAIT;
    static bool settled=false;static unsigned draws=0;static std::uint64_t since=0;
    if(!settled){settled=true;draws=snapshot.draws;since=GetTickCount64();}
    if(GetTickCount64()-since<3000||snapshot.draws-draws<30)return SDGO_GAME_TEST_WAIT;
    if(!CombinedRgb(context,a.unitSerial,true))return SDGO_GAME_TEST_WAIT;
    if(GetEnvironmentVariableW(L"SDGO_SURFACE_ROOM_CLEAR_PANEL",nullptr,0)){
        // Click the native player-list arrow before the final capture. The
        // room camera and model transforms remain under native game control.
        static ShopPointerInput pointer;static unsigned phase=0;static std::uint64_t at=0;
        if(!phase){
            if(!pointer.Install(ownerApi,ownerMod)||!pointer.MoveToClientPoint(331,409))return SDGO_GAME_TEST_FAIL;
            pointer.SetLeftButton(true);phase=1;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;
        }
        if(GetTickCount64()-at<500)return SDGO_GAME_TEST_WAIT;
        if(phase==1){pointer.SetLeftButton(false);phase=2;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;}
        pointer.Clear();
    }
    static bool written=false;
    if(!written){FILE* file=nullptr;if(_wfopen_s(&file,path,L"wb")||!file)return SDGO_GAME_TEST_FAIL;
        if(GetEnvironmentVariableW(L"SDGO_SURFACE_ROOM_EIGHT_SLOTS",nullptr,0)){
            const auto owner=At<void*>(app(),0x4a4),manager=At<void*>(owner,4);
            const auto room=At<void*>(manager,0x38),visual=At<void*>(manager,0x40);
            const bool capacities=At<unsigned>(room,0x28)==8&&At<unsigned>(visual,8)==12;
            context->Assert(capacities,"eight real player slots bind within the twelve-object native visual pool");
            if(!capacities){std::fclose(file);return SDGO_GAME_TEST_FAIL;}
        }
        written=std::fwrite(&snapshot,1,sizeof snapshot,file)==sizeof snapshot;std::fclose(file);
        context->Assert(written,"waiting room draws the exact target's server-saved placement after asynchronous loading");
        wchar_t capture[MAX_PATH]{};GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",capture,MAX_PATH);
        const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));
        if(request&&capture[0])request(capture);
    }
    return SDGO_GAME_TEST_WAIT;
}
int __cdecl PeerCase(const SdgoGameTestContextV1* context){
    using namespace sdgo::surface_decal;
    if(GetEnvironmentVariableW(L"SDGO_SURFACE_ROOM_TEST",nullptr,0))return RoomPeerCase(context);
    const auto app=reinterpret_cast<void*(__cdecl*)()>(ownerApi->Symbol(ownerMod,"game.app"));
    static bool battleEntered=false;
    const bool inBattle=app&&app()&&At<unsigned>(app(),0x4a0)==22;
    // The shared coordinator's complete Start command also waits for PvP AI.
    // This human-only cosmetic scenario stops driving that command after the
    // real native start packet and GAME_MAIN transition; it never claims its
    // AI assertions passed.
    if(!inBattle||!battleEntered){const int result=friendPoll(context);if(result==SDGO_GAME_TEST_FAIL)return result;}
    if(!inBattle)return SDGO_GAME_TEST_WAIT;
    battleEntered=true;
    const auto readBattle=reinterpret_cast<SdgoPvpAiClientGetSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"),SDGO_PVP_AI_CLIENT_SNAPSHOT_EXPORT));
    SdgoPvpAiClientSnapshotV1 live{};live.size=sizeof live;live.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    const bool observedBattle=readBattle&&readBattle(&live);
    if(context->tick%120==0){auto* service=SafeAt<void*>(app(),0x4a4);char message[192];std::snprintf(message,sizeof message,"native peer start observed=%u humans=%u bots=%u packet=%u ready=%u now=%u deadline=%u",observedBattle?1u:0u,live.human_participant_count,live.bot_count,live.received_battle_start_count,SafeAt<unsigned>(service,0x3c),GetTickCount(),SafeAt<unsigned>(service,0x44));context->Observe(message);}
    if(!observedBattle||live.human_participant_count!=2||live.bot_count||!live.received_battle_start_count)return SDGO_GAME_TEST_WAIT;
    static std::uint64_t movementAt=0;
    const auto move=reinterpret_cast<int(__cdecl*)(int)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestReviewForwardV1"));
    if(!move)return SDGO_GAME_TEST_FAIL;
    const auto moved=move(!movementAt||GetTickCount64()-movementAt<1500?1:0);
    if(moved<0)return SDGO_GAME_TEST_FAIL;
    if(!moved)return SDGO_GAME_TEST_WAIT;
    if(!movementAt){movementAt=GetTickCount64();context->Assert(1,"two real human participants received the native battle start and the local controller accepts mapped movement");}
    if(GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_BASELINE",nullptr,0))return SDGO_GAME_TEST_PASS;
    // Let the native start banner clear, then require fresh draws from the
    // ordinary player camera. Do not substitute a spectator camera.
    if(GetTickCount64()-movementAt<6000)return SDGO_GAME_TEST_WAIT;
    wchar_t id[32]{},path[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_UID",id,32)||!GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_RECEIPT",path,MAX_PATH))return SDGO_GAME_TEST_FAIL;
    const auto uid=_wcstoui64(id,nullptr,10);
    const auto observe=reinterpret_cast<test::Target>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),test::targetExport));
    test::TargetSnapshot snapshot;if(!observe||!observe(uid,&snapshot))return SDGO_GAME_TEST_FAIL;
    if(context->tick%120==0){char message[200];std::snprintf(message,sizeof message,"peer target models=%u triangles=%u draws=%u nodes=%u pending=%u matching=%u authority=%u",snapshot.battleModels,snapshot.triangles,snapshot.draws,snapshot.nodes,snapshot.pendingGeometry,snapshot.matchingStickers,snapshot.appearance[0]);context->Observe(message);
        FILE* f=nullptr;std::wstring debug=std::wstring(path)+L".latest";if(!_wfopen_s(&f,debug.c_str(),L"wb")&&f){std::fwrite(&snapshot,1,sizeof snapshot,f);std::fclose(f);}
    }
    static bool settled=false;static unsigned settledDraws=0;
    if(!settled){settled=true;settledDraws=snapshot.draws;}
    static std::uint64_t entered=GetTickCount64();static bool initialCapture=false;
    if(!initialCapture&&GetTickCount64()-entered>8000){
        wchar_t capture[MAX_PATH]{};GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",capture,MAX_PATH);
        const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));
        if(request&&capture[0])initialCapture=request((std::wstring(capture)+L".initial.bmp").c_str())!=0;
    }
    if(GetEnvironmentVariableW(L"SDGO_APPEARANCE_CAMERA_SWEEP",nullptr,0)){
        // Drive the native mouse consumer while retaining the local player's
        // camera target. A rendered remote unit must stay visible to pass.
        static ShopPointerInput pointer;static bool installed=false;
        static unsigned centeringSteps=0;static std::uint64_t centeredAt=0;
        const auto readRgb=reinterpret_cast<SdgoRgbPaintSceneObserveV1Fn>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),SDGO_RGB_PAINT_SCENE_EXPORT));
        SdgoRgbPaintSceneObservationV1 current;current.unit=uid;
        if(!readRgb||!readRgb(&current)||!current.battle_draws||snapshot.draws-settledDraws<30){
            if(!installed){if(!pointer.Install(ownerApi,ownerMod)||!pointer.MoveToClientPoint(512,384))return SDGO_GAME_TEST_FAIL;installed=true;pointer.Motion(0,-70);}
            if(context->tick%3==0)pointer.Motion(6,0);
        }
        // The first visible triangles may only touch the edge of the screen.
        // Continue the same native mouse turn before requesting a review frame.
        else if(installed&&centeringSteps<90){
            if(context->tick%3==0){pointer.Motion(6,0);++centeringSteps;}
            return SDGO_GAME_TEST_WAIT;
        }
        else if(installed){
            if(!centeredAt)centeredAt=GetTickCount64();
            if(GetTickCount64()-centeredAt<600)return SDGO_GAME_TEST_WAIT;
        }
    }
    if(!snapshot.battleModels||!snapshot.triangles||snapshot.draws-settledDraws<30||!CombinedRgb(context,uid,false))return SDGO_GAME_TEST_WAIT;
    static bool written=false;
    if(!written){FILE* f=nullptr;if(_wfopen_s(&f,path,L"wb")||!f)return SDGO_GAME_TEST_FAIL;
        written=std::fwrite(&snapshot,1,sizeof snapshot,f)==sizeof snapshot;std::fclose(f);
        context->Assert(written,"exact target unit uses server-authoritative placement in this real client");
        wchar_t capture[MAX_PATH]{};GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",capture,MAX_PATH);
        const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(GetModuleHandleW(L"game_test_client.dll"),"SdgoGameTestCaptureFrameV1"));
        if(request&&capture[0])request(capture);
    }
    return SDGO_GAME_TEST_WAIT; // the coordinator waits for both independent clients
}
int PreviewInputCase(const SdgoGameTestContextV1* context,void* service){
    using namespace sdgo::surface_decal;
    static ShopPointerInput pointer;static unsigned phase=0;
    static std::uint64_t at=0;static float yaw=0,pitch=0,framing=0;
    static Appearance baseline;
    if(phase==9)return 1;
    auto* factory=Service("surface_decal.factory_vtable");auto* multi=editorPage.Preview(factory);
    if(!multi)return 0;
    const auto* first=At<void**>(multi,0x21c);const auto* end=At<void**>(multi,0x220);
    const auto selected=At<unsigned>(multi,0x268);
    if(!first||end<first||end-first>8||selected>=static_cast<unsigned>(end-first)||!first[selected])return 0;
    auto* model=At<void*>(first[selected],0x34);if(!model)return 0;
    const auto observe=reinterpret_cast<test::Observe>(GetProcAddress(GetModuleHandleW(L"unit_appearance_client.dll"),test::observeExport));
    test::Snapshot snapshot;Appearance draft;
    if(!observe||!observe(&snapshot)||!wire::Decode(snapshot.preview.data(),snapshot.preview.size(),draft))return -1;
    const auto click=[&](const wchar_t* name){
        using String=sdgo::hangar::ui::NativeString;
        const auto find=reinterpret_cast<sdgo::hangar::ui::Bindings::Find>(ownerApi->Symbol(ownerMod,"hangar.ui_find_control"));
        String key;key.length=static_cast<unsigned>(std::wcslen(name));key.capacity=key.length;key.data.pointer=name;
        auto* control=find(At<void*>(service,0xd0),&key,4);if(!control||!At<std::uint8_t>(control,0x162)||!At<std::uint8_t>(control,0x167))return false;
        using Event=void(__stdcall*)(void*,unsigned,std::uintptr_t,void*);
        At<Event>(At<void*>(service),14*4)(service,0x101,reinterpret_cast<std::uintptr_t>(control)+0x118,control);return true;
    };
    if(!phase){
        if(!pointer.Install(ownerApi,ownerMod)||!click(L"SurfacePlace")||!pointer.MoveToClientPoint(500,350))return -1;
        baseline=draft;framing=At<float>(model,0x94);phase=1;at=GetTickCount64();return 0;
    }
    if(GetTickCount64()-at<450)return 0;
    const auto closeEnough=[](float a,float b){return std::abs(a-b)<0.0001f;};
    const auto check=[&](bool ok,const char* text){context->Assert(ok,text);return ok;};
    if(phase==1){const auto frame=CaptureScene(L"00-before-orbit");if(frame<1)return frame;yaw=At<float>(multi,0x250);pitch=At<float>(multi,0x254);pointer.SetLeftButton(true);phase=2;at=GetTickCount64();return 0;}
    if(phase==2){pointer.MoveToClientPoint(540,380);pointer.Motion(40,30);phase=3;at=GetTickCount64();return 0;}
    if(phase==3){
        pointer.SetLeftButton(false);
        if(!check(closeEnough(yaw,At<float>(multi,0x250))&&closeEnough(pitch,At<float>(multi,0x254)),"placement left drag never rotates the native model"))return -1;
        pointer.SetRightButton(true);phase=4;at=GetTickCount64();return 0;
    }
    if(phase==4){pointer.MoveToClientPoint(580,410);pointer.Motion(40,30);phase=5;at=GetTickCount64();return 0;}
    if(phase==5){
        pointer.SetRightButton(false);
        if(!check(!closeEnough(yaw,At<float>(multi,0x250))&&!closeEnough(pitch,At<float>(multi,0x254)),"placement right drag rotates native yaw and pitch")||
           !check(SameContent(baseline,draft),"camera adjustment preserves every sticker anchor"))return -1;
        const auto frame=CaptureScene(L"00-right-drag");if(frame<1)return frame;
        yaw=At<float>(multi,0x250);pitch=At<float>(multi,0x254);
        phase=6;at=GetTickCount64();return 0;
    }
    if(phase==6){
        if(!check(closeEnough(yaw,At<float>(multi,0x250))&&closeEnough(pitch,At<float>(multi,0x254)),"release stops orbit without a stuck drag"))return -1;
        if(!click(L"SurfaceOrbit"))return -1;
        pointer.SetLeftButton(true);phase=7;at=GetTickCount64();return 0;
    }
    if(phase==7){pointer.MoveToClientPoint(540,380);pointer.Motion(-40,-30);phase=8;at=GetTickCount64();return 0;}
    if(phase==8){
        pointer.SetLeftButton(false);
        if(!check(!closeEnough(yaw,At<float>(multi,0x250))&&!closeEnough(pitch,At<float>(multi,0x254)),"orbit mode retains native left drag with pitch"))return -1;
        if(!check(closeEnough(framing,At<float>(model,0x94)),"orbit preserves factory preview scale instead of applying thumbnail zoom limits"))return -1;
        pointer.Clear();phase=9;ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface native pointer camera regression pass");return 1;
    }
    return 1;
}
int __cdecl ProbeCase(const SdgoGameTestContextV1* context) {
    using namespace sdgo::surface_decal;
    static unsigned step=0,tabClicks=0;static std::uint64_t clicked=0;
    static unsigned visits=0;static bool exiting=false;
    static const bool lifecycle=GetEnvironmentVariableW(L"SDGO_SURFACE_LIFECYCLE_TEST",nullptr,0)>0;
    using Ui=sdgo::hangar::ui::Bindings;using String=sdgo::hangar::ui::NativeString;
    using Event=void(__stdcall*)(void*,std::uint32_t,std::uintptr_t,void*);
    const auto find=reinterpret_cast<Ui::Find>(ownerApi->Symbol(ownerMod,"hangar.ui_find_control"));
    if(!find)return SDGO_GAME_TEST_FAIL;
    if(context->elapsed_ms>16000){std::lock_guard<std::mutex> hold(gate);captureReady=true;}
    const auto app=reinterpret_cast<void*(__cdecl*)()>(ownerApi->Symbol(ownerMod,"game.app"));
    const auto state=app&&app()?At<unsigned>(app(),0x4a0):0;
    if(exiting){
        auto* factory=Service("surface_decal.factory_vtable");
        if(factory&&SafeAt<std::uint8_t>(factory,0xac))return SDGO_GAME_TEST_WAIT;
        exiting=false;step=0;clicked=GetTickCount64();context->Observe("native factory exit completed before re-entry");
        return SDGO_GAME_TEST_WAIT;
    }
    static bool openedHangar=false;
    if(!step&&state==6&&!openedHangar){
        auto* menu=Service("flow.hangar_downmenu_vtable");
        if(menu){auto* root=At<void*>(menu,0x44);auto* radio=At<void*>(menu,0x54);
            String key;key.data.pointer=L"MyRoomRadio";key.length=11;key.capacity=11;
            if(root&&radio&&find(root,&key,12)==radio&&At<std::uint8_t>(radio,0x167)){
                auto event=At<Event>(At<void*>(menu),14*4);event(menu,0x301,reinterpret_cast<std::uintptr_t>(radio)+0x118,radio);
                openedHangar=true;context->Observe("native MyRoomRadio invoked");
            }
        }
        return SDGO_GAME_TEST_WAIT;
    }
    if(context->tick%300==0){char message[160];std::snprintf(message,sizeof message,"surface navigation step=%u app=%u unitmgr=%u factory_menu=%u",step,state,Service("flow.battleship_lobby_manager_vtable")?1:0,Service("test.factory_tab_menu_vtable")?1:0);context->Observe(message);}
    if(!step){
        auto* service=Service("flow.battleship_lobby_manager_vtable");if(!service)return SDGO_GAME_TEST_WAIT;
        auto* root=At<void*>(service,0x64);if(!root||!At<std::uint8_t>(root,0x162))return SDGO_GAME_TEST_WAIT;
        if(reloadTest){
            const auto runtime=GetModuleHandleW(L"unit_appearance_client.dll");
            const auto observe=reinterpret_cast<test::Observe>(GetProcAddress(runtime,test::observeExport));
            test::Snapshot snapshot;
            if(!observe||!observe(&snapshot))return SDGO_GAME_TEST_FAIL;
            if(!snapshot.positionedModels||!snapshot.draws){
                if(context->tick%120==0){char text[100];std::snprintf(text,sizeof text,"hangar placement waiting; bound=%u positioned=%u draws=%u",snapshot.boundModels,snapshot.positionedModels,snapshot.draws);context->Observe(text);}
                return SDGO_GAME_TEST_WAIT;
            }
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface native hangar reload draw pass");
        }
        String key;key.data.pointer=L"FactoryBtn";key.length=10;key.capacity=10;
        auto* control=find(root,&key,4);if(!control||!At<std::uint8_t>(control,0x162)||!At<std::uint8_t>(control,0x167))return SDGO_GAME_TEST_WAIT;
        auto event=At<Event>(At<void*>(service),14*4);event(service,0x101,reinterpret_cast<std::uintptr_t>(control)+0x118,control);
        clicked=GetTickCount64();step=1;context->Observe("native factory button invoked");return SDGO_GAME_TEST_WAIT;
    }
    if(step==2&&GetTickCount64()-clicked>5000&&tabClicks<3)step=1;
    if(step==1){
        auto* service=Service("test.factory_tab_menu_vtable");if(!service||!At<std::uint8_t>(service,0xac)||GetTickCount64()-clicked<1500)return SDGO_GAME_TEST_WAIT;
        auto* root=At<void*>(service,0xd0);
        // The shipped page captions call PaintRadio the sticker/emblem tab;
        // StickerRadio is the seven-colour paint page in this native layout.
        String key;key.data.pointer=L"PaintRadio";key.length=10;key.capacity=10;
        auto* control=root?find(root,&key,12):nullptr;
        if(!root||!control||At<void*>(control,0x15c)!=root||!At<std::uint8_t>(control,0x167))return SDGO_GAME_TEST_WAIT;
        auto event=At<Event>(At<void*>(service),14*4);event(service,0x301,reinterpret_cast<std::uintptr_t>(control)+0x118,control);
        clicked=GetTickCount64();step=2;++tabClicks;context->Observe("native sticker tab invoked");return SDGO_GAME_TEST_WAIT;
    }
    auto* service=Service("surface_decal.sticker_vtable");if(!service||!At<std::uint8_t>(service,0xac))return SDGO_GAME_TEST_WAIT;
    auto* root=At<void*>(service,0xd0);if(!root||!At<std::uint8_t>(root,0x162)||GetTickCount64()-clicked<1000)return SDGO_GAME_TEST_WAIT;
    if(runtimeMode){
        static bool runtimeOpened=false;
        String key;key.data.pointer=L"SurfaceOpen";key.length=11;key.capacity=11;
        auto* control=find(root,&key,4);
        if(!control)return SDGO_GAME_TEST_WAIT;
        if(expectBackend&&!runtimeOpened){
            if(!At<std::uint8_t>(control,0x162)||!At<std::uint8_t>(control,0x167))return SDGO_GAME_TEST_WAIT;
            auto event=At<Event>(At<void*>(service),14*4);event(service,0x101,reinterpret_cast<std::uintptr_t>(control)+0x118,control);
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface test unit=%llu type=%u stickers=%u,%u,%u",static_cast<unsigned long long>(At<std::uint64_t>(service,0x100)),At<unsigned>(service,0xf8),At<unsigned>(service,0x134),At<unsigned>(service,0x138),At<unsigned>(service,0x13c));
            runtimeOpened=true;return SDGO_GAME_TEST_WAIT;
        }
        if(!expectBackend&&(!At<std::uint8_t>(control,0x162)||At<std::uint8_t>(control,0x167))){
            context->Assert(0,"runtime entry remains visible but unavailable without the server companion");return SDGO_GAME_TEST_FAIL;
        }
        void* cancel=nullptr;
        if(expectBackend){
            key.data.pointer=L"SurfaceCancel";key.length=13;key.capacity=13;cancel=find(root,&key,4);
            if(!cancel||!At<std::uint8_t>(cancel,0x162))return SDGO_GAME_TEST_WAIT;
        }
        if(GetEnvironmentVariableW(L"SDGO_SURFACE_CAMERA_TEST",nullptr,0)){
            const auto result=PreviewInputCase(context,service);
            if(result<0)return SDGO_GAME_TEST_FAIL;
            if(!result)return SDGO_GAME_TEST_WAIT;
        }
        if(saveTest||reloadTest){
            static unsigned phase=0;static Appearance baseline,proposal;
            const auto runtime=GetModuleHandleW(L"unit_appearance_client.dll");
            const auto observe=reinterpret_cast<test::Observe>(GetProcAddress(runtime,test::observeExport));
            const auto place=reinterpret_cast<test::Place>(GetProcAddress(runtime,test::placeExport));
            test::Snapshot snapshot;Appearance authority,draft;
            if(!observe||!place||!observe(&snapshot)||!wire::Decode(snapshot.saved.data(),snapshot.saved.size(),authority)||
               !wire::Decode(snapshot.preview.data(),snapshot.preview.size(),draft))return SDGO_GAME_TEST_FAIL;
            const auto click=[&](const wchar_t* name){
                String text;text.data.pointer=name;text.length=static_cast<unsigned>(std::wcslen(name));text.capacity=text.length;
                auto* button=find(root,&text,4);if(!button||!At<std::uint8_t>(button,0x162)||!At<std::uint8_t>(button,0x167))return false;
                auto event=At<Event>(At<void*>(service),14*4);event(service,0x101,reinterpret_cast<std::uintptr_t>(button)+0x118,button);return true;
            };
            if(!phase){
                if(!authority.slots[0].sticker){context->Assert(0,"native test requires an existing equipped sticker");return SDGO_GAME_TEST_FAIL;}
                if(reloadTest&&!authority.slots[0].positioned){context->Assert(0,"a new client process reads the previously saved surface placement");return SDGO_GAME_TEST_FAIL;}
                const auto captured=CaptureScene(L"01-editor");if(captured<1)return captured<0?SDGO_GAME_TEST_FAIL:SDGO_GAME_TEST_WAIT;
                baseline=authority;if(!click(L"SurfaceSlot0")||!click(L"SurfacePlace"))return SDGO_GAME_TEST_WAIT;
                phase=1;return SDGO_GAME_TEST_WAIT;
            }
            if(phase==1){
                if(GetEnvironmentVariableW(L"SDGO_SURFACE_SAVE_BACK",nullptr,0)){
                    static ShopPointerInput pointer;static unsigned turn=0;
                    static std::uint64_t at=0;static float yaw=0;
                    auto* multi=editorPage.Preview(Service("surface_decal.factory_vtable"));
                    if(!multi)return SDGO_GAME_TEST_WAIT;
                    if(!turn){
                        if(!pointer.Install(ownerApi,ownerMod)||!pointer.MoveToClientPoint(500,350))return SDGO_GAME_TEST_FAIL;
                        yaw=At<float>(multi,0x250);pointer.SetRightButton(true);turn=1;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;
                    }
                    if(GetTickCount64()-at<350)return SDGO_GAME_TEST_WAIT;
                    if(turn==1){pointer.Motion(444,0);turn=2;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;}
                    if(turn==2){
                        pointer.SetRightButton(false);pointer.Clear();
                        const float delta=std::abs(At<float>(multi,0x250)-yaw);
                        if(delta<3.0f||delta>3.2f){context->Assert(0,"native right drag turns the preview to its back");return SDGO_GAME_TEST_FAIL;}
                        turn=3;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;
                    }
                }
                bool placed=false;
                if(GetEnvironmentVariableW(L"SDGO_SURFACE_SAVE_BACK",nullptr,0))placed=place(500,340)!=0;
                for(int y=340;y<=420&&!placed;y+=20)for(int x=480;x<=540&&!placed;x+=20)placed=place(x,y)!=0;
                if(!placed){if(context->tick%120==0){char text[120];std::snprintf(text,sizeof text,"runtime placement waiting; fresh views=%u triangles=%u",snapshot.freshViews,snapshot.triangles);context->Observe(text);}return SDGO_GAME_TEST_WAIT;}
                phase=2;return SDGO_GAME_TEST_WAIT;
            }
            if(phase==2){
                if(!snapshot.triangles||!snapshot.draws)return SDGO_GAME_TEST_WAIT;
                const auto captured=CaptureScene(L"02-placement");if(captured<1)return captured<0?SDGO_GAME_TEST_FAIL:SDGO_GAME_TEST_WAIT;
                if(reloadTest){
                    if(!click(L"SurfaceCancel"))return SDGO_GAME_TEST_WAIT;
                    test::Snapshot after;if(!observe(&after)||after.editing)return SDGO_GAME_TEST_FAIL;
                    Appearance unchanged;if(!wire::Decode(after.saved.data(),after.saved.size(),unchanged)||!SameContent(unchanged,baseline)||unchanged.revision!=baseline.revision)return SDGO_GAME_TEST_FAIL;
                    ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface runtime reload and cancellation pass");phase=5;
                    // Reopen through the native entry to capture the saved view.
                    runtimeOpened=false;return SDGO_GAME_TEST_WAIT;
                }
                if(GetEnvironmentVariableW(L"SDGO_SURFACE_SAVE_BACK",nullptr,0))
                    for(unsigned i=0;i<4;++i)if(!click(L"SurfaceLarger"))return SDGO_GAME_TEST_FAIL;
                if(!click(L"SurfaceRight")||!click(L"SurfaceLarger")||!click(L"SurfaceSave"))return SDGO_GAME_TEST_WAIT;
                phase=3;return SDGO_GAME_TEST_WAIT;
            }
            if(phase==3){
                const auto captured=CaptureScene(L"03-confirm");if(captured<1)return captured<0?SDGO_GAME_TEST_FAIL:SDGO_GAME_TEST_WAIT;
                if(snapshot.pending||authority.revision!=baseline.revision||!SameContent(authority,baseline)){
                    context->Assert(0,"first save click previews without mutating the server");return SDGO_GAME_TEST_FAIL;
                }
                proposal=draft;if(!click(L"SurfaceSave"))return SDGO_GAME_TEST_WAIT;
                test::Snapshot sent;if(!observe(&sent))return SDGO_GAME_TEST_FAIL;
                // A native click can arrive while the shared transport is
                // occupied. Require an actual submission before awaiting its
                // acknowledgement; an unchanged editable draft is not a reply.
                Appearance updated;
                if(!sent.pending&&sent.state==static_cast<unsigned>(EditState::editing)&&
                   wire::Decode(sent.saved.data(),sent.saved.size(),updated)&&updated.revision==baseline.revision)
                    return SDGO_GAME_TEST_WAIT;
                phase=4;return SDGO_GAME_TEST_WAIT;
            }
            if(phase==4){
                if(snapshot.pending||snapshot.state!=static_cast<unsigned>(EditState::editing))return SDGO_GAME_TEST_WAIT;
                if(authority.revision!=baseline.revision+1||!SameContent(authority,proposal)){
                    context->Assert(0,"save acknowledgement carries the exact draft with one newer revision");return SDGO_GAME_TEST_FAIL;
                }
                const auto captured=CaptureScene(L"04-saved");if(captured<1)return captured<0?SDGO_GAME_TEST_FAIL:SDGO_GAME_TEST_WAIT;
                wchar_t recordPath[MAX_PATH]{};
                if(GetEnvironmentVariableW(L"SDGO_SURFACE_SAVED_RECORD",recordPath,MAX_PATH)){
                    FILE* record=nullptr;if(_wfopen_s(&record,recordPath,L"wb")||!record)return SDGO_GAME_TEST_FAIL;
                    const bool written=std::fwrite(snapshot.saved.data(),1,snapshot.saved.size(),record)==snapshot.saved.size();std::fclose(record);
                    if(!written)return SDGO_GAME_TEST_FAIL;
                }
                ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface runtime save acknowledgement pass; triangles=%u draws=%u",snapshot.triangles,snapshot.draws);phase=5;
            }
        }
        const auto test=GetModuleHandleW(L"game_test_client.dll");
        const auto request=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(test,"SdgoGameTestCaptureFrameV1"));
        const auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(test,"SdgoGameTestCaptureStatusV1"));
        if(!request||!status)return SDGO_GAME_TEST_FAIL;
        if(!captureAt){wchar_t path[MAX_PATH]{};if(GetEnvironmentVariableW(L"SDGO_SURFACE_TEST_CAPTURE",path,MAX_PATH)&&request(path))captureAt=GetTickCount64();}
        HRESULT hr=E_PENDING;const auto capture=status(&hr);
        if(capture==3&&SUCCEEDED(hr)){
            if(expectBackend){auto event=At<Event>(At<void*>(service),14*4);event(service,0x101,reinterpret_cast<std::uintptr_t>(cancel)+0x118,cancel);}
            if(lifecycle&&visits<2){
                if(!ExitFactory())return SDGO_GAME_TEST_WAIT;
                ++visits;exiting=true;runtimeOpened=false;tabClicks=0;
                context->Observe("native editor cancelled and factory exit invoked");return SDGO_GAME_TEST_WAIT;
            }
            if(lifecycle)context->Assert(visits==2,"three genuine factory visits reopen the sticker editor after native exits");
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface runtime editor pass; backend=%u",expectBackend?1u:0u);
            context->Assert(1,expectBackend?"real server read enables the native editor and native cancel returns without saving":"real runtime prevents unacknowledged editing when the companion is absent");return SDGO_GAME_TEST_PASS;
        }
        if(capture==4)return SDGO_GAME_TEST_FAIL;
        return SDGO_GAME_TEST_WAIT;
    }
    if(step==2){
        ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface sticker page root=%.0f,%.0f,%d,%d record_type=%u",
            At<float>(root,0x148),At<float>(root,0x14c),At<int>(root,0x154),At<int>(root,0x158),At<unsigned>(service,0xf8));
        auto* head=At<void*>(root,0x23c);auto* link=head?At<void*>(head):nullptr;
        for(unsigned i=0;link&&link!=head&&i<128;++i){
            auto* c=At<void*>(link,8);const auto name=At<String>(c,0x118);
            if(name.length<128)ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface control=%.*ls rect=%.0f,%.0f,%d,%d kind=%u",static_cast<int>(name.length),name.capacity<8?name.data.small:name.data.pointer,At<float>(c,0x148),At<float>(c,0x14c),At<int>(c,0x154),At<int>(c,0x158),At<unsigned>(c,0x134));
            link=At<void*>(link);
        }
        auto* factory=Service("surface_decal.factory_vtable");
        if(!factory||!editorPage.Attach(service,At<void*>(factory,0xe4))){context->Assert(0,"native editor attaches to the sticker page using native button skins");return SDGO_GAME_TEST_FAIL;}
        sdgo::surface_decal::Appearance appearance;
        appearance.unitType=At<unsigned>(service,0xf8);appearance.unitSerial=At<std::uint64_t>(service,0x100);appearance.revision=1;
        for(unsigned i=0;i<3;++i)appearance.slots[i].sticker=At<unsigned>(service,0x134+i*4);
        editorPage.Render(true,true,0,false,false,false,&appearance,L"选择贴纸后在机体表面放置");
        if(editorPage.Click(editorPage.Control(sdgo::surface_decal::ui::Action::cancel))!=sdgo::surface_decal::ui::Action::cancel){context->Assert(0,"native editor exposes cancel");return SDGO_GAME_TEST_FAIL;}
        std::lock_guard<std::mutex> hold(gate);captureReady=true;drawn=0;step=3;
    }
    std::lock_guard<std::mutex> hold(gate);
    if(captureComplete){
        editorPage.Render(false,false,0,false,false,false,nullptr,L"");
        context->Assert(!At<std::uint8_t>(editorPage.Control(sdgo::surface_decal::ui::Action::cancel),0x162),"closing the editor hides its native controls");
        context->Assert(1,"the native sticker page draws a projected test decal and its editor on the real backbuffer");return SDGO_GAME_TEST_PASS;
    }
    return SDGO_GAME_TEST_WAIT;
}
void Identity(void* model) {
    __try {
        const auto* vtable=*reinterpret_cast<const std::uint32_t**>(model);
        const auto col=reinterpret_cast<const std::uint32_t*>(vtable[-1]);
        const auto type=reinterpret_cast<const char*>(col[3])+8;
        char name[128]{};
        for(unsigned i=0;i<sizeof name-1&&type[i];++i)name[i]=type[i];
        ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface model native type=%s vtable_rva=%08X",name,
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(vtable)-reinterpret_cast<std::uintptr_t>(ownerApi->host_base)));
        const auto* bytes=static_cast<const std::uint8_t*>(model);
        const auto* first=*reinterpret_cast<const std::uint8_t*const*const*>(bytes+0x124);
        const auto* end=*reinterpret_cast<const std::uint8_t*const*const*>(bytes+0x128);
        ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface native node count=%u",static_cast<unsigned>(end-first));
        for(unsigned i=0;first+i<end&&i<8;++i) {
            const auto* node=reinterpret_cast<const std::uint8_t*>(first[i]);
            const auto* common=*reinterpret_cast<const std::uint8_t*const*>(node+0x11c);
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface node=%u vtable_rva=%08X hidden=%u kind=%u vertices=%u faces=%u groups=%u vcapacity=%u icapacity=%u",
                i,*reinterpret_cast<const unsigned*>(node)-static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(ownerApi->host_base)),node[0x108],
                *reinterpret_cast<const unsigned*>(common+0x70),*reinterpret_cast<const unsigned*>(common+0xa4),
                *reinterpret_cast<const unsigned*>(common+0xa8),*reinterpret_cast<const unsigned*>(common+0x194),
                *reinterpret_cast<const unsigned*>(common+0x38),*reinterpret_cast<const unsigned*>(common+0x5c));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ownerApi->Log(ownerMod,SDGO_LOG_WARN,"surface model identity unavailable");
    }
}
void __cdecl Observe(void* model,const std::uint16_t* colors,std::uint16_t shine,const std::uint32_t* stickers) {
    original(model,colors,shine,stickers);
    const auto last=GetLastError();
    if(observed++<24) {
        try {
            using namespace sdgo::surface_decal;
            std::vector<native::Node> nodes;
            const auto result=reader.Read(model,nodes);
            if(result==native::ReadResult::ok){std::lock_guard<std::mutex> hold(gate);target=model;if(modelCache.size()>64)modelCache.clear();modelCache[model]=nodes;}
            if(result!=native::ReadResult::ok)Identity(model);
            unsigned projected=0;std::size_t vertices=0,triangles=0;
            for(const auto& node:nodes) {
                vertices+=node.mesh.vertices.size();triangles+=node.mesh.triangles.size();
                Placement p;Hit hit{0,{{1.0/3,1.0/3,1.0/3}},0};Patch patch;
                if(MakePlacement(node.mesh,hit,node.geometry,node.index,{1,0,0},2,2,0.5,p)==Result::ok&&
                   Project(node.mesh,node.geometry,node.index,p,patch)==Result::ok)++projected;
            }
            ownerApi->Log(ownerMod,SDGO_LOG_INFO,"surface geometry read=%u nodes=%u vertices=%u triangles=%u projected=%u",
                static_cast<unsigned>(result),static_cast<unsigned>(nodes.size()),static_cast<unsigned>(vertices),
                static_cast<unsigned>(triangles),projected);
        } catch(const std::exception&) {ownerApi->Log(ownerMod,SDGO_LOG_ERROR,"surface geometry observer failed");}
    }
    SetLastError(last);
}
}
SDGO_MOD_ENTRY {
    wchar_t run[64]{};
    if(!GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",run,64)||!api||!api->HookCode||!api->Log||!reader.Bind(api,mod)||!editorPage.Bind(api,mod))return 0;
    ownerApi=api;ownerMod=mod;
    wchar_t runtime[8]{};runtimeMode=GetEnvironmentVariableW(L"SDGO_SURFACE_REAL_RUNTIME",runtime,8)&&runtime[0]==L'1';
    expectBackend=GetEnvironmentVariableW(L"SDGO_SURFACE_EXPECT_BACKEND",runtime,8)&&runtime[0]==L'1';
    saveTest=GetEnvironmentVariableW(L"SDGO_SURFACE_SAVE_TEST",runtime,8)&&runtime[0]==L'1';
    reloadTest=GetEnvironmentVariableW(L"SDGO_SURFACE_RELOAD_TEST",runtime,8)&&runtime[0]==L'1';
    battleTest=GetEnvironmentVariableW(L"SDGO_SURFACE_BATTLE_TEST",runtime,8)&&runtime[0]==L'1';
    if(runtimeMode)return 1; // the real Mod owns every hook; the provider drives native UI
    renderer=static_cast<void**>(api->Symbol(mod,"surface_decal.renderer"));
    return renderer&&api->HookCode(mod,"surface_decal.unit_style",reinterpret_cast<void*>(&Observe),reinterpret_cast<void**>(&original))&&
        api->HookCode(mod,"surface_decal.render_direct",reinterpret_cast<void*>(&Direct),reinterpret_cast<void**>(&direct))&&
        api->HookCode(mod,"surface_decal.render_shader",reinterpret_cast<void*>(&Shader),reinterpret_cast<void**>(&shader))&&
        api->HookCode(mod,"surface_decal.render_callback",reinterpret_cast<void*>(&Callback),reinterpret_cast<void**>(&callback));
}
SDGO_GAME_TEST_PROVIDER_ENTRY {
    static std::array<SdgoGameTestCaseV1,6> cases{};
    const auto runtime=GetModuleHandleW(L"game_test_client.dll");
    const auto get=reinterpret_cast<SdgoGameTestGetProviderV1Fn>(GetProcAddress(runtime,SDGO_GAME_TEST_PROVIDER_EXPORT));
    const auto originalProvider=get?get():nullptr;
    if(!originalProvider||originalProvider->count<3)return nullptr;
    for(unsigned i=0;i<3;++i)cases[i]=originalProvider->cases[i];
    if(GetEnvironmentVariableW(L"SDGO_SURFACE_PEER_UID",nullptr,0)){
        for(unsigned i=0;i<originalProvider->count;++i)if(!std::strcmp(originalProvider->cases[i].id,"friends.session"))friendPoll=originalProvider->cases[i].poll;
        if(!friendPoll)return nullptr;
        cases[3]={"friends.session",600000,&PeerCase};
        static const SdgoGameTestProviderV1 peer{sizeof peer,SDGO_GAME_TEST_VERSION,"surface-decals","client",4,cases.data()};return &peer;
    }
    if(battleTest){
        for(unsigned i=0;i<originalProvider->count;++i){const auto& c=originalProvider->cases[i];
            if(!std::strcmp(c.id,"mission.room"))cases[3]=c;
            if(!std::strcmp(c.id,"mission.start"))cases[4]=c;
        }
        if(!cases[3].id||!cases[4].id)return nullptr;
        cases[5]={"surface_decal.native_battle",120000,&BattleCase};
        static const SdgoGameTestProviderV1 battle{sizeof battle,SDGO_GAME_TEST_VERSION,"surface-decals","client",6,cases.data()};return &battle;
    }
    unsigned count=3;
    for(unsigned i=0;i<originalProvider->count;++i)if(!std::strcmp(originalProvider->cases[i].id,"setup.active_unit"))cases[count++]=originalProvider->cases[i];
    if(!GetEnvironmentVariableW(L"SDGO_SURFACE_SELECT_ONLY",nullptr,0))cases[count++]={"surface_decal.native_projection",120000,&ProbeCase};
    static SdgoGameTestProviderV1 provider{sizeof provider,SDGO_GAME_TEST_VERSION,"surface-decals","client",0,cases.data()};provider.count=count;return &provider;
}
