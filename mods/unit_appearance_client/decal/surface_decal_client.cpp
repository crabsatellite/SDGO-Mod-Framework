#include "native_ui.h"
#include "render.h"
#include "visibility.h"
#include "sdgo/surface_decal_native.h"
#include "sdgo/surface_decal_protocol.h"
#include "sdgo/surface_decal_observer.h"
#include "sdgo/surface_decal_battle.h"
#include "sdgo/surface_decal_room.h"
#include "sdgo/friend_unit_gift_protocol.h"
#include <windows.h>
#include <intrin.h>
#include <map>
#include <set>
#include <mutex>
#include <cstring>
#include <limits>

namespace {
using namespace sdgo::surface_decal;
using namespace sdgo::friend_unit_gift;
namespace decalwire=sdgo::surface_decal::wire;
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
native::GeometryReader geometry;
ui::Page page;
std::recursive_mutex gate;
BattleBindings battleBindings;
RoomBindings roomBindings;
void* humanSiteOriginal=nullptr;void* battleStyleOriginal=nullptr;void* previewInputOriginal=nullptr;
void* previewZoomOriginal=nullptr;void* previewZoomEnd=nullptr;
const unsigned defaultMxcsr=0x1f80;
struct LastErrorGuard {DWORD value=GetLastError();~LastErrorGuard(){SetLastError(value);}};
template<class T>T Read(const void* p,unsigned offset=0){T v{};std::memcpy(&v,static_cast<const std::uint8_t*>(p)+offset,sizeof v);return v;}
template<class T>void Put(void* p,unsigned offset,T v){std::memcpy(static_cast<std::uint8_t*>(p)+offset,&v,sizeof v);}
bool Copy(const void* p,void* out,std::size_t bytes) noexcept {__try{if(!p)return false;std::memcpy(out,p,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
using Style=void(__cdecl*)(void*,const std::uint16_t*,std::uint16_t,const std::uint32_t*);
using Callback=void(__thiscall*)(void*,void*,unsigned);
using Simple=void(__thiscall*)(void*);
using Destroy=void*(__thiscall*)(void*,unsigned);
using UiEvent=void(__stdcall*)(void*,unsigned,std::uintptr_t,void*);
using Material=void*(__thiscall*)(void*,unsigned,int,int);
using Indexed=bool(__thiscall*)(void*,unsigned,int,unsigned,unsigned,unsigned,unsigned);
Indexed indexedOriginal=nullptr;
thread_local bool suppressNative=false;
Style styleOriginal=nullptr;Callback callbackOriginal=nullptr;
Simple modelDestroyOriginal=nullptr,pageUpdateOriginal=nullptr;
Simple previewLock=nullptr,previewUnlock=nullptr;
Destroy pageDestroyOriginal=nullptr;UiEvent eventOriginal=nullptr;
Material material=nullptr;
void **renderer=nullptr,**services=nullptr;
void *stickerTable=nullptr,*factoryTable=nullptr;
using App=std::uint8_t*(__cdecl*)();App app=nullptr;
ClientExtensionSendFn send=nullptr;ClientExtensionStateFn transport=nullptr;
using Session=void*(__cdecl*)();Session session=nullptr;
std::uint64_t sessionKey=0;
struct Node {native::Node surface;View view;std::uint64_t viewed=0;};
struct Model {
    bool battle=false,room=false,geometryPending=false;
    std::uint32_t draws=0;
    std::uint64_t uid=0,geometryRetry=0;
    std::uint32_t type=0;
    std::array<std::uint32_t,3> stickers{};
    std::vector<Node> nodes;
    std::array<Patch,3> patches{};
    Appearance projected{};
};
struct Cached {Appearance appearance;std::uint64_t fetched=0;};
std::map<void*,Model> models;
std::map<std::uint64_t,Cached> saved;
std::set<std::uint64_t> wanted;
Editor editor;
Appearance pageRecord{};
std::wstring notice=L"正在连接贴花服务…";
bool editing=false,placing=false,armed=false,dragging=false,mouseDown=false;
unsigned selected=0;
std::uint64_t sequence=0,pendingRequest=0,pendingUid=0,pendingAt=0,retryAt=0;
std::uint16_t pendingCommand=0;
std::uintptr_t pendingToken=0,nextToken=0;
UINT_PTR timer=0;
HHOOK messageHook=nullptr;
bool observing=false;
std::uint32_t drawCount=0;
std::uint32_t battleDrawCount=0;
void* preview=nullptr;std::uint8_t previewInput=0,previewPitch=0;bool inputHeld=false;
struct Footer {void* factory=nullptr;std::array<void*,2> buttons{};std::array<std::uint8_t,2> visible{};} footer;

bool Record(const void* source,Appearance& out){
    std::array<std::uint8_t,0x99> bytes{};if(!Copy(source,bytes.data(),bytes.size()))return false;
    Appearance a;a.unitType=Read<std::uint32_t>(bytes.data());a.unitSerial=Read<std::uint64_t>(bytes.data(),8);
    for(unsigned i=0;i<3;++i)a.slots[i].sticker=Read<std::uint32_t>(bytes.data(),60+i*4);
    decalwire::Bytes encoded;if(!decalwire::Encode(a,encoded))return false;out=a;return true;
}
std::uint32_t RoomUnit(std::uint32_t getter,std::uint32_t room,std::uint32_t slot) noexcept {
    using Get=std::uint8_t*(__thiscall*)(void*,int);
    __try{return reinterpret_cast<std::uint32_t>(reinterpret_cast<Get>(getter)(reinterpret_cast<void*>(room),static_cast<int>(slot)));}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
bool RoomRecord(void* model,const std::uint16_t* colors,const std::array<std::uint32_t,3>& ids,Appearance& out){
    const auto source=roomBindings.Resolve(reinterpret_cast<std::uint32_t>(app()),reinterpret_cast<std::uint32_t>(model),
        [](std::uint32_t address,void* target,std::size_t size){return Copy(reinterpret_cast<void*>(address),target,size);},RoomUnit);
    Appearance record;std::uint32_t type=0;
    if(!source||!Record(reinterpret_cast<void*>(source),record)||
       !Copy(reinterpret_cast<const std::uint8_t*>(colors)-4,&type,sizeof type)||type!=record.unitType)return false;
    for(unsigned i=0;i<3;++i)if(record.slots[i].sticker!=ids[i])return false;
    out=record;return true;
}
void* Service(void* table){
    if(!services||!*services)return nullptr;auto* head=Read<void*>(*services,0x44);auto* link=head?Read<void*>(head):nullptr;
    for(unsigned i=0;link&&link!=head&&i<512;++i){auto* s=Read<void*>(link,8);if(s&&Read<void*>(s)==table)return s;link=Read<void*>(link);}return nullptr;
}
void RestoreInput(){
    // Resolve the live factory each time. Never touch the saved pointer after
    // a page transition, model destruction or a native factory reload.
    if(inputHeld){auto* factory=Service(factoryTable);if(preview&&page.Preview(factory)==preview){
        Put<std::uint8_t>(preview,0x263,previewInput);Put<std::uint8_t>(preview,0x264,previewPitch);
        Put<std::uint8_t>(preview,0x261,0);Put<std::uint8_t>(preview,0x262,0);
    }}
    inputHeld=false;preview=nullptr;dragging=false;mouseDown=false;
}
void RestoreFooter(){
    auto* factory=Service(factoryTable);
    if(factory&&factory==footer.factory)for(unsigned i=0;i<2;++i)
        if(footer.buttons[i]&&page.Footer(factory,i)==footer.buttons[i])Put<std::uint8_t>(footer.buttons[i],0x162,footer.visible[i]);
    footer={};
}
void Close(){
    if(editor.State()==EditState::editing)editor.Cancel();
    editing=placing=armed=false;RestoreInput();RestoreFooter();
}
const Appearance* Authority(std::uint64_t uid){const auto i=saved.find(uid);return i==saved.end()?nullptr:&i->second.appearance;}
const Appearance* AppearanceFor(const Model& m){
    if(editing&&editor.Preview().unitSerial==m.uid)return &editor.Preview();return Authority(m.uid);
}
bool SameStickers(const Appearance& a,const Model& m){
    if(a.unitSerial!=m.uid||a.unitType!=m.type)return false;
    for(unsigned i=0;i<3;++i)if(a.slots[i].sticker!=m.stickers[i])return false;return true;
}
void Rebuild(Model& m,const Appearance& a){
    if(SameContent(m.projected,a))return;
    m.patches={};m.projected=a;
    if(!SameStickers(a,m))return;
    for(unsigned i=0;i<3;++i){const auto& s=a.slots[i];if(!s.positioned)continue;
        for(const auto& n:m.nodes)if(n.surface.index==s.placement.anchor.node){
            Project(n.surface.mesh,n.surface.geometry,n.surface.index,s.placement,m.patches[i]);break;
        }
    }
}
IDirect3DBaseTexture9* Texture(void* owner,unsigned slot){
    auto* m=material(owner,slot+7,0,1);if(!m)return nullptr;
    auto* texture=Read<void*>(m,0x58);return texture?Read<IDirect3DBaseTexture9*>(texture,0x14):nullptr;
}
void __cdecl ReplyReceived(void* token,std::uint16_t command,std::int32_t status,const std::uint8_t* data,std::uint32_t bytes){
    std::lock_guard<std::recursive_mutex> hold(gate);
    if(!pendingUid||pendingCommand!=command||reinterpret_cast<std::uintptr_t>(token)!=pendingToken)return;
    const auto uid=pendingUid,request=pendingRequest;pendingUid=0;pendingCommand=0;
    Appearance authority;
    const bool valid=status==0&&data&&bytes==decalwire::appearanceBytes+4&&decalwire::Decode(data+4,bytes-4,authority)&&authority.unitSerial==uid;
    if(!valid){
        if(command==decalwire::save){editor.Timeout();wanted.insert(uid);notice=L"保存结果待确认，正在重新读取";}
        else {notice=L"服务器未就绪，稍后自动重试";wanted.insert(uid);retryAt=GetTickCount64()+10000;}
        if(status==static_cast<int>(WireStatus::unit_unavailable)||status==static_cast<int>(WireStatus::not_authorized)){
            saved.erase(uid);wanted.erase(uid);
            if(editor.Preview().unitSerial==uid){Close();editor=Editor{};}
            notice=L"机体归属已变化，请重新选择";
        }
        return;
    }
    const auto result=decalwire::Get(data,4);
    if(result!=0&&result!=12){if(command==decalwire::save){editor.Timeout();wanted.insert(uid);}notice=L"服务器拒绝了位置请求";return;}
    const auto previous=saved.find(uid);
    if(previous!=saved.end()&&(authority.revision<previous->second.appearance.revision||
       (authority.revision==previous->second.appearance.revision&&!SameContent(authority,previous->second.appearance)))){
        if(command==decalwire::save)editor.Timeout();notice=L"位置版本已变更，正在重新读取";wanted.insert(uid);retryAt=GetTickCount64()+1000;return;
    }
    saved[uid]={authority,GetTickCount64()};
    if(command==decalwire::save){
        if(!editor.Acknowledge(request,result==0?sdgo::surface_decal::Reply::saved:sdgo::surface_decal::Reply::conflict,authority)){editor.Timeout();wanted.insert(uid);notice=L"正在确认服务器位置版本";}
        else {notice=result==0?L"贴花位置已保存":L"贴纸或位置已变更，已重新读取";armed=false;}
    }else if(editor.State()==EditState::uncertain&&editor.Preview().unitSerial==uid){
        if(editor.Reconcile(authority))notice=L"已读取服务器确认的位置";
    }else if(pageRecord.unitSerial==uid){
        if(!editing)notice=L"已有贴纸可自由调整位置";
        else if(!editor.Dirty())editor.Open(authority);
    }
}
void Pump(){
    auto* nativeSession=session?session():nullptr;
    const int connection=nativeSession?Read<int>(nativeSession,0x14):-1;
    const auto key=connection<0?0:(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(nativeSession))<<32)|static_cast<std::uint32_t>(connection);
    if(key!=sessionKey){
        Close();editor=Editor{};saved.clear();wanted.clear();battleBindings.Clear();pageRecord={};pendingUid=0;pendingCommand=0;sessionKey=key;
        for(auto& item:models){item.second.uid=0;item.second.projected={};item.second.patches={};}
        notice=L"连接已变化，正在重新读取";
    }
    while(saved.size()>256){
        auto oldest=saved.end();for(auto i=saved.begin();i!=saved.end();++i)if(i->first!=pageRecord.unitSerial&&i->first!=editor.Preview().unitSerial&&(oldest==saved.end()||i->second.fetched<oldest->second.fetched))oldest=i;
        if(oldest==saved.end())break;saved.erase(oldest);
    }
    while(wanted.size()>256){auto i=std::prev(wanted.end());if(*i==pageRecord.unitSerial){if(i==wanted.begin())break;--i;}wanted.erase(i);}
    if(pendingUid)return;
    if(!send||!transport){const auto carrier=GetModuleHandleW(L"friend_unit_gift_client.dll");if(carrier){send=reinterpret_cast<ClientExtensionSendFn>(GetProcAddress(carrier,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));transport=reinterpret_cast<ClientExtensionStateFn>(GetProcAddress(carrier,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT));}}
    if(!send||!transport||transport()!=1||wanted.empty()||GetTickCount64()<retryAt)return;
    const auto uid=*wanted.begin();wanted.erase(wanted.begin());std::uint8_t bytes[8];decalwire::Put(bytes,uid,8);
    pendingUid=uid;pendingCommand=decalwire::get;pendingAt=GetTickCount64();pendingToken=++nextToken;
    if(!send(decalwire::get,bytes,sizeof bytes,&ReplyReceived,reinterpret_cast<void*>(pendingToken))){pendingUid=0;pendingCommand=0;wanted.insert(uid);retryAt=GetTickCount64()+500;}
}
void Save(){
    if(!send||!transport||transport()!=1||pendingUid){notice=L"正在通信，请稍后再保存";return;}
    Appearance proposal;const auto request=++sequence;if(!editor.BeginSave(request,proposal))return;
    decalwire::Bytes encoded;if(!decalwire::Encode(proposal,encoded)){editor.Timeout();return;}
    pendingUid=proposal.unitSerial;pendingRequest=request;pendingCommand=decalwire::save;pendingAt=GetTickCount64();pendingToken=++nextToken;
    if(!send(decalwire::save,encoded.data(),static_cast<std::uint32_t>(encoded.size()),&ReplyReceived,reinterpret_cast<void*>(pendingToken))){
        pendingUid=0;pendingCommand=0;editor.Timeout();wanted.insert(proposal.unitSerial);notice=L"发送未确认，正在重新读取";
    }else notice=L"等待服务器保存确认…";
}
bool PlaceAt(double x,double y){
    if(!editing||!placing||editor.State()!=EditState::editing||selected>=3||!editor.Preview().slots[selected].sticker)return false;
    const auto now=GetTickCount64();double nearest=std::numeric_limits<double>::infinity();Placement best;bool found=false;
    for(auto& item:models){auto& m=item.second;if(!SameStickers(editor.Preview(),m))continue;
        for(auto& n:m.nodes){if(now-n.viewed>150)continue;Ray ray;Hit hit;
            if(!ScreenRay(n.view,x,y,ray)||Pick(n.surface.mesh,ray,hit)!=Result::ok)continue;
            const auto point=Vec3{ray.origin.x+ray.direction.x/std::hypot(ray.direction.x,ray.direction.y,ray.direction.z)*hit.distance,
                ray.origin.y+ray.direction.y/std::hypot(ray.direction.x,ray.direction.y,ray.direction.z)*hit.distance,
                ray.origin.z+ray.direction.z/std::hypot(ray.direction.x,ray.direction.y,ray.direction.z)*hit.distance};
            Vec3 projected;if(!Transform(point,Multiply(Multiply(n.view.world,n.view.camera),n.view.projection),projected)||projected.z<0||projected.z>1||projected.z>=nearest)continue;
            const auto& old=editor.Preview().slots[selected];const double width=old.positioned?old.placement.width:2,height=old.positioned?old.placement.height:2,depth=old.positioned?old.placement.depth:0.6;
            Placement p;if(MakePlacement(n.surface.mesh,hit,n.surface.geometry,n.surface.index,{1,0,0},width,height,depth,p)!=Result::ok)continue;
            if(old.positioned)p.rotation=old.placement.rotation;
            Patch test;if(Project(n.surface.mesh,n.surface.geometry,n.surface.index,p,test)!=Result::ok)continue;
            best=p;nearest=projected.z;found=true;
        }
    }
    if(!found)return false;armed=false;notice=L"预览中，保存后生效";return editor.Place(selected,best);
}
void Action(ui::Action action){
    const auto* current=Authority(pageRecord.unitSerial);
    if(action==ui::Action::open){
        if(!current||pendingUid||editor.State()==EditState::uncertain)return;
        if(!editor.Open(*current))return;editing=true;placing=false;armed=false;selected=0;
        while(selected<2&&!current->slots[selected].sticker)++selected;
        notice=L"选择贴纸后切换到放置模式";return;
    }
    if(!editing||editor.State()!=EditState::editing)return;
    if(action>=ui::Action::slot0&&action<=ui::Action::slot2){const auto i=static_cast<unsigned>(action)-static_cast<unsigned>(ui::Action::slot0);if(editor.Preview().slots[i].sticker){selected=i;armed=false;}return;}
    if(action==ui::Action::place){placing=true;notice=L"左键放置，右键拖动调整视角";return;}
    if(action==ui::Action::orbit){placing=false;dragging=mouseDown=false;notice=L"左右拖动转向，上下拖动俯仰";return;}
    if(action==ui::Action::cancel){Close();notice=L"已取消本次位置调整";return;}
    if(action==ui::Action::save){if(armed)Save();else{armed=true;placing=false;RestoreInput();notice=L"请核对预览，再次点击确认保存";}return;}
    if(action==ui::Action::reset){editor.ResetPosition(selected);armed=false;notice=L"预览恢复原生位置，保存后生效";return;}
    auto p=editor.Preview().slots[selected].placement;if(!editor.Preview().slots[selected].positioned)return;
    if(action==ui::Action::left)p.rotation-=0.08726646259971647;
    else if(action==ui::Action::right)p.rotation+=0.08726646259971647;
    else if(action==ui::Action::smaller){p.width*=0.9;p.height*=0.9;}
    else if(action==ui::Action::larger){p.width*=1.1;p.height*=1.1;}
    else return;
    p.rotation=std::remainder(p.rotation,6.283185307179586);
    if(p.width<0.05||p.height<0.05||p.width>30||p.height>30)return;
    if(editor.Place(selected,p)){armed=false;notice=L"预览中，保存后生效";}
}
void BindFactory(void* factory,const Appearance& record){
    auto* multi=page.Preview(factory);if(!multi)return;
    auto** first=Read<void**>(multi,0x21c);auto** end=Read<void**>(multi,0x220);
    if(!first||end<first||end-first>8)return;
    for(auto** p=first;p!=end;++p){if(!*p)continue;auto* model=Read<void*>(*p,0x34);auto found=models.find(model);if(found==models.end())continue;
        auto& m=found->second;if(m.uid!=record.unitSerial||m.type!=record.unitType){m.uid=record.unitSerial;m.type=record.unitType;m.projected={};wanted.insert(m.uid);}
    }
    if(editing&&!inputHeld){preview=multi;previewInput=Read<std::uint8_t>(multi,0x263);previewPitch=Read<std::uint8_t>(multi,0x264);
        Put<std::uint8_t>(multi,0x263,1);Put<std::uint8_t>(multi,0x264,1);inputHeld=true;}
}
void TickPage(void* service){
    auto* factory=Service(factoryTable);if(!factory)return;
    auto* root=Read<void*>(service,0xd0);if(!root||!Read<std::uint8_t>(service,0xac)||!Read<std::uint8_t>(root,0x162)){
        if(page.Owner()==service&&editing){Close();page.Render(false,false,selected,false,false,false,Authority(pageRecord.unitSerial),L"未保存的位置已取消");}return;
    }
    auto* source=page.Footer(factory,0);
    if(!page.Attach(service,source))return;
    Appearance record;if(!Record(static_cast<std::uint8_t*>(service)+0xf8,record))return;
    if(pageRecord.unitSerial!=record.unitSerial||!SameContent(pageRecord,record)){
        Close();pageRecord=record;wanted.insert(record.unitSerial);notice=L"正在读取机体贴花位置…";
    }
    BindFactory(factory,record);
    const auto* current=Authority(record.unitSerial);
    // Native selection/replacement remains native. A draft never uses another
    // sticker's texture, nor can it silently become a save for the new unit.
    if(editing&&current){for(unsigned i=0;i<3;++i)if(record.slots[i].sticker!=current->slots[i].sticker){Close();wanted.insert(record.unitSerial);notice=L"请先完成原生贴纸保存";break;}}
    if(editing&&placing){
        auto* application=app?app():nullptr;const HWND window=application?Read<HWND>(application,0x3c):nullptr;
        DWORD owner=0;GetWindowThreadProcessId(GetForegroundWindow(),&owner);
        if(window&&owner==GetCurrentProcessId()){
            POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);
            const bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
            const bool orbit=(GetAsyncKeyState(VK_RBUTTON)&0x8000)!=0;
            if(orbit)dragging=false;
            else if(down&&!mouseDown&&!page.Contains(static_cast<float>(point.x),static_cast<float>(point.y)))dragging=PlaceAt(point.x,point.y);
            else if(down&&dragging)PlaceAt(point.x,point.y);
            if(!down)dragging=false;mouseDown=down;
        }else{dragging=false;mouseDown=false;}
    }
    const bool busy=pendingCommand==decalwire::save||editor.State()==EditState::uncertain;
    if(editing){
        if(footer.factory!=factory){
            RestoreFooter();footer.factory=factory;
            for(unsigned i=0;i<2;++i){footer.buttons[i]=page.Footer(factory,i);if(footer.buttons[i])footer.visible[i]=Read<std::uint8_t>(footer.buttons[i],0x162);}
        }
        for(auto* button:footer.buttons)if(button)Put<std::uint8_t>(button,0x162,0);
    }
    page.Render(editing,placing,selected,busy,editor.Dirty(),armed,editing?&editor.Preview():current,notice);
}
void CALLBACK Timer(HWND,UINT,UINT_PTR,DWORD){
    try{std::lock_guard<std::recursive_mutex> hold(gate);Pump();
        if(pendingUid&&GetTickCount64()-pendingAt>15000&&pendingCommand==decalwire::save){editor.Timeout();notice=L"保存结果待确认，请保持连接";}
    }catch(...){ }
}
void ArmTimer(){
    if(!timer&&g_api->size>=offsetof(SdgoApi,process_initial_thread_id)+sizeof(DWORD)&&g_api->process_initial_thread_id==GetCurrentThreadId())timer=SetTimer(nullptr,0,100,&Timer);
}
void __stdcall CaptureHuman(const std::uint8_t* frame){
    LastErrorGuard error;
    try{
        std::array<std::uint8_t,24> args{};if(!Copy(frame,args.data(),args.size()))return;
        const auto* source=Read<const std::uint8_t*>(args.data(),12);
        const auto* entry=Read<const std::uint8_t*>(args.data(),16);
        const unsigned deck=Read<unsigned>(args.data(),20)&255;
        std::uint32_t owner=0,sourceOwner=0;
        if(!Copy(entry,&owner,4)||!owner||(owner&0x80000000u))return;
        std::array<Appearance,4> records{};unsigned count=0;
        if(source&&Copy(source+0x429,&sourceOwner,4)&&sourceOwner==owner&&deck<=1){
            count=deck?4:1;
            for(unsigned i=0;i<count;++i)Record(source+(deck?0x19e+i*0x99:0x49),records[i]);
        }
        std::lock_guard<std::recursive_mutex> hold(gate);battleBindings.Replace(owner,records,count);
        for(unsigned i=0;i<count;++i)if(records[i].unitSerial){
            if(records[i].slots[0].sticker||records[i].slots[1].sticker||records[i].slots[2].sticker)wanted.insert(records[i].unitSerial);
        }
        if(observing)g_api->Log(g_mod,SDGO_LOG_INFO,"surface native human identities count=%u",count);
    }catch(...){ }
}
void __stdcall CaptureBattleStyle(const std::uint8_t* frame){
    LastErrorGuard error;
    try{
        std::array<std::uint8_t,20> args{};if(!Copy(frame,args.data(),args.size()))return;
        const auto* record=Read<const std::uint8_t*>(args.data(),12);
        const auto* alternate=Read<const std::uint8_t*>(args.data(),16);
        const auto styleKey=reinterpret_cast<std::uintptr_t>(alternate?alternate:record)+(alternate?0x10:0x54);
        {std::lock_guard<std::recursive_mutex> hold(gate);battleBindings.ForgetStyle(styleKey);}
        std::array<std::uint8_t,0x344> bytes{};if(!Copy(record,bytes.data(),bytes.size()))return;
        unsigned slot=0;
        if(alternate){
            const auto start=reinterpret_cast<std::uintptr_t>(record)+0x44;
            const auto at=reinterpret_cast<std::uintptr_t>(alternate);
            if(at<start||(at-start)%0xb4||(at-start)/0xb4>=4)return;
            slot=static_cast<unsigned>((at-start)/0xb4);
        }
        const auto offset=0x44+slot*0xb4;
        const auto type=Read<unsigned>(bytes.data(),offset);
        const auto ids=Read<std::array<std::uint32_t,3>>(bytes.data(),offset+0x24);
        const auto colors=reinterpret_cast<std::uintptr_t>(record)+offset+0x10;
        std::lock_guard<std::recursive_mutex> hold(gate);
        const bool bound=battleBindings.Prepare(Read<unsigned>(bytes.data()),slot,type,ids,colors);
        if(observing)g_api->Log(g_mod,SDGO_LOG_INFO,"surface native battle identity slot=%u bound=%u",slot,bound?1u:0u);
    }catch(...){ }
}
#define SURFACE_CONTEXT_SITE(Name, Observe, Original) \
__declspec(naked) void Name(){ \
    __asm { pushfd } __asm { pushad } __asm { mov eax,esp } \
    __asm { sub esp,210h } __asm { and esp,0FFFFFFF0h } \
    __asm { mov [esp+200h],eax } __asm { fxsave [esp] } \
    __asm { fninit } __asm { ldmxcsr dword ptr [defaultMxcsr] } __asm { cld } \
    __asm { push ebp } __asm { call Observe } __asm { fxrstor [esp] } \
    __asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd } \
    __asm { jmp dword ptr [Original] } \
}
SURFACE_CONTEXT_SITE(HumanSite,CaptureHuman,humanSiteOriginal)
SURFACE_CONTEXT_SITE(BattleStyleSite,CaptureBattleStyle,battleStyleOriginal)
#undef SURFACE_CONTEXT_SITE
// Native CUIMulti3DRender has already latched its mouse buttons here, before
// updating the model transform under its native lock. Reuse that operation,
// including pitch, instead of writing model transforms.
void __stdcall PreviewInput(void* control){
    LastErrorGuard error;
    try{std::lock_guard<std::recursive_mutex> hold(gate);
        if(!editing||!inputHeld||control!=preview)return;
        const bool right=Read<std::uint8_t>(control,0x261)!=0;
        const bool left=Read<std::uint8_t>(control,0x262)!=0;
        Put<std::uint8_t>(control,0x262,(right||(!placing&&left))?1:0);
    }catch(...){ }
}
__declspec(naked) void PreviewInputSite(){
    __asm { pushfd } __asm { pushad } __asm { mov eax,esp }
    __asm { sub esp,210h } __asm { and esp,0FFFFFFF0h }
    __asm { mov [esp+200h],eax } __asm { fxsave [esp] }
    __asm { fninit } __asm { ldmxcsr dword ptr [defaultMxcsr] } __asm { cld }
    __asm { push esi } __asm { call PreviewInput } __asm { fxrstor [esp] }
    __asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd }
    __asm { jmp dword ptr [previewInputOriginal] }
}
int __stdcall OwnPreviewInput(void* control){
    LastErrorGuard error;
    try{std::lock_guard<std::recursive_mutex> hold(gate);return editing&&inputHeld&&control==preview;}catch(...){return 0;}
}
// This generic widget's native wheel branch clamps model scale to 5.5..9.5
// on every hovered frame, even with no wheel movement. Factory previews use
// their own larger framing. Enable native orbit without that unrelated clamp.
__declspec(naked) void PreviewZoomSite(){
    __asm { pushfd } __asm { pushad } __asm { mov eax,esp }
    __asm { sub esp,210h } __asm { and esp,0FFFFFFF0h }
    __asm { mov [esp+200h],eax } __asm { fxsave [esp] }
    __asm { fninit } __asm { ldmxcsr dword ptr [defaultMxcsr] } __asm { cld }
    __asm { push esi } __asm { call OwnPreviewInput } __asm { test eax,eax } __asm { jz nativeZoom }
    __asm { fxrstor [esp] } __asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd }
    __asm { jmp dword ptr [previewZoomEnd] }
    __asm { nativeZoom: }
    __asm { fxrstor [esp] } __asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd }
    __asm { jmp dword ptr [previewZoomOriginal] }
}
LRESULT CALLBACK GameThreadMessage(int code,WPARAM wparam,LPARAM lparam){
    const auto last=GetLastError();
    // A thread-local message hook establishes the timer without taking over
    // another Mod's game.app detour. Remove it once the UI timer is running.
    if(code>=0){ArmTimer();if(timer&&messageHook){auto hook=messageHook;messageHook=nullptr;UnhookWindowsHookEx(hook);}}
    SetLastError(last);return CallNextHookEx(nullptr,code,wparam,lparam);
}
bool PackedStyleCaller(std::uintptr_t caller){
    // These native previews pass a complete packed 0x99-byte owned-unit
    // record. Battle actors use a different appearance layout and must never
    // infer a serial from bytes preceding their colour array.
    switch(caller){
    case 0x1f64ad:
    // Ctor 0x61b7a0 copies all 0x99 bytes to this+0x68; its model
    // renderer passes colours at +0x96, not a condensed battle record.
    case 0x21c55f:
    case 0x369a6e:case 0x36b7d4:case 0x36c22c:
    case 0x3c896e:case 0x3c90a6:case 0x3c95eb:
    // The shared preview job copies 0x99 bytes to job+0x10. The unit
    // inventory renderer obtains the same record through CUnitInfo::GetData.
    case 0x3f5650:case 0x56ebb4:
    case 0x445054:case 0x4457c6:case 0x445d0b:
    case 0x461b53:case 0x472760:
    case 0x4b32b8:case 0x4b5b82:
    case 0x53aae6:case 0x53b02b:case 0x53bdf4:
    case 0x614dd6:case 0x61cb47:
        return true;
    default:return false;
    }
}
void __cdecl StyleHook(void* model,const std::uint16_t* colors,std::uint16_t shine,const std::uint32_t* decals){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress())-reinterpret_cast<std::uintptr_t>(g_api->host_base);
    styleOriginal(model,colors,shine,decals);const auto last=GetLastError();
    try{
        std::vector<native::Node> nodes;const auto result=geometry.Read(model,nodes);
        std::array<std::uint32_t,3> ids{};if(!Copy(decals,ids.data(),sizeof ids)){SetLastError(last);return;}
        Appearance record;bool bound=false,battle=false,room=false;
        if(PackedStyleCaller(caller)&&reinterpret_cast<const std::uint8_t*>(decals)==reinterpret_cast<const std::uint8_t*>(colors)+14)
            bound=Record(reinterpret_cast<const std::uint8_t*>(colors)-0x2e,record);
        std::lock_guard<std::recursive_mutex> hold(gate);
        if(caller==0x326104&&reinterpret_cast<const std::uint8_t*>(decals)==reinterpret_cast<const std::uint8_t*>(colors)+16)
            room=bound=RoomRecord(model,colors,ids,record);
        if((caller==0x6c9d31||caller==0x6c9d4a||caller==0x103af6||caller==0x103b13)&&
           reinterpret_cast<const std::uint8_t*>(decals)==reinterpret_cast<const std::uint8_t*>(colors)+20)
            battle=bound=battleBindings.Resolve(reinterpret_cast<std::uintptr_t>(colors),record);
        if(observing)g_api->Log(g_mod,SDGO_LOG_INFO,"surface style caller=%08X bound=%u battle=%u room=%u geometry=%u",static_cast<unsigned>(caller),bound?1u:0u,battle?1u:0u,room?1u:0u,static_cast<unsigned>(result));
        Model value;value.stickers=ids;value.geometryPending=battle||room||result!=native::ReadResult::ok;
        value.battle=battle;value.room=room;
        if(bound){value.uid=record.unitSerial;value.type=record.unitType;if(ids[0]||ids[1]||ids[2])wanted.insert(value.uid);}
        if(result==native::ReadResult::ok)for(auto& n:nodes)value.nodes.push_back({std::move(n),{},0});
        if(models.size()>=256&&models.find(model)==models.end())models.erase(models.begin());
        models[model]=std::move(value);ArmTimer();
    }catch(...){ }
    SetLastError(last);
}
void __fastcall ModelDestroyHook(void* self,void*){{std::lock_guard<std::recursive_mutex> hold(gate);models.erase(self);}modelDestroyOriginal(self);}
bool BattleSurfaceReady(void* owner,const Model& model,const Appearance& a,unsigned slot){
    if(!model.battle)return true;
    auto** first=Read<void**>(owner,0x124);auto** end=Read<void**>(owner,0x128);
    const auto index=a.slots[slot].placement.anchor.node;
    if(!first||end<first||end-first>4096||index>=static_cast<unsigned>(end-first))return false;
    auto* node=first[index];
    if(!node||!render::OpaqueNode(Read<std::uint8_t>(node,0x108)!=0,Read<unsigned>(node,0x204)))return false;
    auto* common=Read<void*>(node,0x11c);if(!common)return false;
    const auto count=Read<unsigned>(common,0x194);const auto* groups=Read<std::uint8_t*>(common,0x198);
    if(!groups||count>256)return false;
    const auto surface=std::find_if(model.nodes.begin(),model.nodes.end(),[&](const Node& n){return n.surface.index==index;});
    if(surface==model.nodes.end())return false;
    return render::OpaqueCoverage(model.patches[slot],surface->surface.triangleGroups,count,
        [&](unsigned group,float& alpha){
            const auto id=Read<unsigned>(groups,group*12+8);if(id>=7&&id<=9)return false;
            auto* m=material(owner,id,0,1);if(!m)return false;
            alpha=Read<float>(m,0x10);return true;
        });
}
void RenderNode(void* self,void* entry,unsigned,bool before,bool& suppress){
    // The callback's texture bit is a state-change optimization, not visibility.
    if(Read<std::uint8_t>(self,0x108))return;
    auto* owner=Read<void*>(self,0x118);auto found=models.find(owner);if(found==models.end())return;
    auto& m=found->second;const auto* appearance=AppearanceFor(m);if(!appearance)return;
    // Battle and waiting-room styling can precede asynchronous model loading.
    // Resolve geometry on the native rendering thread once the model actually
    // draws; never freeze an empty projection from its construction callback.
    const auto now=GetTickCount64();
    if(m.geometryPending&&now>=m.geometryRetry){
        m.geometryRetry=now+250;std::vector<native::Node> nodes;
        if(geometry.Read(owner,nodes)==native::ReadResult::ok){
            m.nodes.clear();for(auto& n:nodes)m.nodes.push_back({std::move(n),{},0});
            m.geometryPending=false;m.projected={};m.patches={};
        }
    }
    Rebuild(m,*appearance);
    if(!SameStickers(*appearance,m))return;
    auto* common=Read<void*>(self,0x11c);const auto group=Read<std::uint8_t>(entry,0x14);
    if(group>=Read<unsigned>(common,0x194))return;
    const auto* groups=Read<std::uint8_t*>(common,0x198);const auto mat=Read<unsigned>(groups,group*12+8);
    if(before){
        if(mat>=7&&mat<=9&&!m.patches[mat-7].vertices.empty()&&Texture(owner,mat-7)&&
           BattleSurfaceReady(owner,m,*appearance,mat-7))suppress=true;
        return;
    }
    auto** first=Read<void**>(owner,0x124);auto** end=Read<void**>(owner,0x128);if(!first||end<first||end-first>4096)return;
    unsigned index=0;while(first+index!=end&&first[index]!=self)++index;
    auto n=std::find_if(m.nodes.begin(),m.nodes.end(),[&](const Node& node){return node.surface.index==index;});if(n==m.nodes.end())return;
    auto* device=renderer&&*renderer?Read<IDirect3DDevice9*>(*renderer,8):nullptr;
    if(!render::Capture(device,Read<std::array<float,16>>(self,0x130),n->view))return;
    n->viewed=GetTickCount64();
    for(unsigned i=0;i<3;++i)if(!m.patches[i].vertices.empty()&&appearance->slots[i].placement.anchor.node==index){
        if(!BattleSurfaceReady(owner,m,*appearance,i))continue;
        // Draw each clipped triangle once, directly after its own native
        // surface group. Repeated alpha blending across unrelated groups would
        // change opacity and let later surfaces overwrite the sticker.
        Patch part;for(const auto& vertex:m.patches[i].vertices)
            if(vertex.triangle<n->surface.triangleGroups.size()&&n->surface.triangleGroups[vertex.triangle]==group)part.vertices.push_back(vertex);
        if(!part.vertices.empty())if(auto* texture=Texture(owner,i))if(render::Draw(device,texture,part,n->view)){++drawCount;++m.draws;if(m.battle)++battleDrawCount;}
    }
}
bool __fastcall IndexedHook(void* self,void*,unsigned primitive,int base,unsigned minimum,unsigned vertices,unsigned first,unsigned count){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress())-reinterpret_cast<std::uintptr_t>(g_api->host_base);
    if(suppressNative&&caller==0x817e8a)return true;
    return indexedOriginal(self,primitive,base,minimum,vertices,first,count);
}
void __fastcall CallbackHook(void* self,void*,void* entry,unsigned flags){
    bool suppress=false;
    try{std::lock_guard<std::recursive_mutex> hold(gate);ArmTimer();RenderNode(self,entry,flags,true,suppress);}catch(...){suppress=false;}
    const bool previous=suppressNative;suppressNative=suppress;
    callbackOriginal(self,entry,flags);suppressNative=previous;
    const auto last=GetLastError();
    try{std::lock_guard<std::recursive_mutex> hold(gate);RenderNode(self,entry,flags,false,suppress);}catch(...){ }
    SetLastError(last);
}
void __fastcall UpdateHook(void* self,void*){
    pageUpdateOriginal(self);const auto last=GetLastError();
    if(!page.Ready(self)){
        std::lock_guard<std::recursive_mutex> hold(gate);
        if(page.Owner()==self)Close();
        SetLastError(last);return;
    }
    // Model loading/rendering holds this native lock before entering our
    // styling callback. Keep the same lock order while reading its vector.
    auto* factory=Service(factoryTable);auto* multi=page.Preview(factory);
    if(!multi){SetLastError(last);return;}
    std::array<void*,8> locked{};unsigned count=0;
    if(multi){auto** first=Read<void**>(multi,0x21c);auto** end=Read<void**>(multi,0x220);
        if(first&&end>=first&&end-first<=8)for(auto** p=first;p!=end;++p)if(*p){previewLock(*p);locked[count++]=*p;}
    }
    try{std::lock_guard<std::recursive_mutex> hold(gate);ArmTimer();TickPage(self);}catch(...){Close();page.Detach();}
    while(count)previewUnlock(locked[--count]);SetLastError(last);
}
void* __fastcall PageDestroyHook(void* self,void*,unsigned flags){{std::lock_guard<std::recursive_mutex> hold(gate);if(page.Owner()==self){Close();page.Detach();pageRecord={};}}return pageDestroyOriginal(self,flags);}
void __stdcall EventHook(void* service,unsigned event,std::uintptr_t second,void* control){
    {std::lock_guard<std::recursive_mutex> hold(gate);if(page.Owner()==service){const auto action=page.Click(control);if(action!=ui::Action::none){if(event==0x101||event==0x803)Action(action);return;}}}
    eventOriginal(service,event,second,control);
}
}
namespace {
bool NativeTest(){
    wchar_t run[64]{};
    return g_api&&g_api->size>=offsetof(SdgoApi,process_initial_thread_id)+sizeof(DWORD)&&
        g_api->process_initial_thread_id==GetCurrentThreadId()&&GetModuleHandleW(L"game_test_client.dll")&&
        GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",run,64)>0;
}
}
extern "C" __declspec(dllexport) int __cdecl SdgoSurfaceDecalTestObserveV1(test::Snapshot* output){
    if(!NativeTest()||!output||output->size!=sizeof(*output)||output->version!=1)return 0;
    std::lock_guard<std::recursive_mutex> hold(gate);test::Snapshot s;
    s.editing=editing;s.placing=placing;s.state=static_cast<unsigned>(editor.State());s.pending=pendingCommand;s.selected=selected;s.draws=drawCount;
    s.battleDraws=battleDrawCount;
    if(const auto* a=Authority(pageRecord.unitSerial))decalwire::Encode(*a,s.saved);
    if(editing)decalwire::Encode(editor.Preview(),s.preview);
    const auto now=GetTickCount64();for(const auto& item:models)if(item.second.uid==pageRecord.unitSerial){
        for(const auto& n:item.second.nodes)if(now-n.viewed<150)++s.freshViews;
        for(const auto& patch:item.second.patches)s.triangles+=static_cast<unsigned>(patch.vertices.size()/3);
    }
    for(const auto& item:models)if(item.second.uid){
        ++s.boundModels;
        if(item.second.battle)++s.battleModels;
        const auto* a=Authority(item.second.uid);
        if(a&&SameStickers(*a,item.second))for(unsigned i=0;i<3;++i)if(a->slots[i].positioned){++s.positionedModels;if(item.second.battle)++s.battlePositioned;}
    }
    static std::uint64_t diagnostic=0;
    if(s.battleModels&&now-diagnostic>2000){diagnostic=now;
        for(const auto& item:models)if(item.second.battle){const auto* a=Authority(item.second.uid);if(!a)continue;
            auto** first=Read<void**>(item.first,0x124);auto** end=Read<void**>(item.first,0x128);
            for(unsigned i=0;i<3;++i)if(a->slots[i].positioned){const auto index=a->slots[i].placement.anchor.node;
                if(!first||end<first||end-first>4096||index>=static_cast<unsigned>(end-first)||!first[index])continue;
                g_api->Log(g_mod,SDGO_LOG_INFO,"surface battle anchor node=%u hidden=%u flags=%08X triangles=%u ready=%u",
                    index,Read<std::uint8_t>(first[index],0x108),Read<unsigned>(first[index],0x204),
                    static_cast<unsigned>(item.second.patches[i].vertices.size()/3),BattleSurfaceReady(item.first,item.second,*a,i)?1u:0u);
            }
        }
    }
    *output=s;return 1;
}
extern "C" __declspec(dllexport) int __cdecl SdgoSurfaceDecalTestPlaceV1(double x,double y){
    if(!NativeTest())return 0;std::lock_guard<std::recursive_mutex> hold(gate);return PlaceAt(x,y)?1:0;
}
extern "C" __declspec(dllexport) int __cdecl SdgoSurfaceDecalTestTargetV1(std::uint64_t uid,test::TargetSnapshot* out){
    if(!NativeTest()||!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    std::lock_guard<std::recursive_mutex> hold(gate);test::TargetSnapshot value;
    const auto* a=Authority(uid);if(a)decalwire::Encode(*a,value.appearance);
    for(const auto& item:models)if(item.second.battle&&item.second.uid==uid){
        ++value.battleModels;value.draws+=item.second.draws;
        value.nodes+=static_cast<unsigned>(item.second.nodes.size());value.pendingGeometry+=item.second.geometryPending?1u:0u;
        value.matchingStickers+=a&&SameStickers(*a,item.second)?1u:0u;
        for(const auto& p:item.second.patches)value.triangles+=static_cast<unsigned>(p.vertices.size()/3);
    }
    *out=value;return 1;
}
extern "C" __declspec(dllexport) int __cdecl SdgoSurfaceDecalTestRoomV1(std::uint64_t uid,test::RoomSnapshot* out){
    if(!NativeTest()||!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    std::lock_guard<std::recursive_mutex> hold(gate);test::RoomSnapshot value;
    const auto* a=Authority(uid);if(a)decalwire::Encode(*a,value.appearance);
    for(const auto& item:models)if(item.second.room&&item.second.uid==uid){
        ++value.models;value.draws+=item.second.draws;
        value.nodes+=static_cast<unsigned>(item.second.nodes.size());value.pendingGeometry+=item.second.geometryPending?1u:0u;
        value.matchingStickers+=a&&SameStickers(*a,item.second)?1u:0u;
        for(const auto& p:item.second.patches)value.triangles+=static_cast<unsigned>(p.vertices.size()/3);
    }
    *out=value;return 1;
}
int LoadDecals(const SdgoApi* api, SdgoMod* mod) {
    if(!api||!mod||!api->Symbol||!api->HookCode||!api->SettingInt||!api->host_key||std::strcmp(api->host_key,"GOnline-54BE1568-00C97000")||!api->SettingInt(mod,"DecalsEnabled",1))return 0;
    g_api=api;g_mod=mod;
    wchar_t run[64]{};observing=GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",run,64)>0;
    if(!geometry.Bind(api,mod)||!page.Bind(api,mod))return 0;
    renderer=static_cast<void**>(api->Symbol(mod,"surface_decal.renderer"));services=static_cast<void**>(api->Symbol(mod,"flow.service_managed_pointer"));
    stickerTable=api->Symbol(mod,"surface_decal.sticker_vtable");factoryTable=api->Symbol(mod,"surface_decal.factory_vtable");
    app=reinterpret_cast<App>(api->Symbol(mod,"game.app"));material=reinterpret_cast<Material>(api->Symbol(mod,"surface_decal.material"));
    const auto roomSymbol=[&](const char* name){return reinterpret_cast<std::uint32_t>(api->Symbol(mod,name));};
    roomBindings={roomSymbol("surface_decal.room_service"),roomSymbol("surface_decal.room_update"),roomSymbol("surface_decal.room_info"),
        roomSymbol("surface_decal.room_normal_owner"),roomSymbol("surface_decal.room_mission_owner"),
        roomSymbol("surface_decal.room_normal_unit"),roomSymbol("surface_decal.room_mission_unit"),
        roomSymbol("surface_decal.room_object"),roomSymbol("surface_decal.room_data")};
    if(!roomBindings.Valid())return 0;
    session=reinterpret_cast<Session>(api->Symbol(mod,"storage.session"));
    previewLock=reinterpret_cast<Simple>(api->Symbol(mod,"surface_decal.preview_lock"));
    previewUnlock=reinterpret_cast<Simple>(api->Symbol(mod,"surface_decal.preview_unlock"));
    previewZoomEnd=api->Symbol(mod,"surface_decal.preview_zoom_end");
    if(!renderer||!services||!stickerTable||!factoryTable||!app||!material||!session||!previewLock||!previewUnlock||!previewZoomEnd)return 0;
    if(api->size<offsetof(SdgoApi,process_initial_thread_id)+sizeof(DWORD)||!api->process_initial_thread_id||!api->HookSite)return 0;
    const bool hooks=api->HookCode(mod,"surface_decal.unit_style",reinterpret_cast<void*>(&StyleHook),reinterpret_cast<void**>(&styleOriginal))&&
        api->HookCode(mod,"surface_decal.model_destroy",reinterpret_cast<void*>(&ModelDestroyHook),reinterpret_cast<void**>(&modelDestroyOriginal))&&
        api->HookCode(mod,"surface_decal.render_callback",reinterpret_cast<void*>(&CallbackHook),reinterpret_cast<void**>(&callbackOriginal))&&
        api->HookCode(mod,"surface_decal.draw_indexed",reinterpret_cast<void*>(&IndexedHook),reinterpret_cast<void**>(&indexedOriginal))&&
        api->HookCode(mod,"surface_decal.sticker_update",reinterpret_cast<void*>(&UpdateHook),reinterpret_cast<void**>(&pageUpdateOriginal))&&
        api->HookCode(mod,"surface_decal.sticker_event",reinterpret_cast<void*>(&EventHook),reinterpret_cast<void**>(&eventOriginal))&&
        api->HookCode(mod,"surface_decal.sticker_destroy",reinterpret_cast<void*>(&PageDestroyHook),reinterpret_cast<void**>(&pageDestroyOriginal))&&
        api->HookSite(mod,"surface_decal.human_identity",reinterpret_cast<void*>(&HumanSite),&humanSiteOriginal)&&
        api->HookSite(mod,"surface_decal.battle_identity",reinterpret_cast<void*>(&BattleStyleSite),&battleStyleOriginal)&&
        api->HookSite(mod,"surface_decal.preview_input",reinterpret_cast<void*>(&PreviewInputSite),&previewInputOriginal)&&
        api->HookSite(mod,"surface_decal.preview_zoom",reinterpret_cast<void*>(&PreviewZoomSite),&previewZoomOriginal);
    if(!hooks)return 0;
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&GameThreadMessage),&module))return 0;
    messageHook=SetWindowsHookExW(WH_GETMESSAGE,&GameThreadMessage,module,api->process_initial_thread_id);
    return messageHook!=nullptr;
}

void AbortDecalsLoad(){if(messageHook){UnhookWindowsHookEx(messageHook);messageHook=nullptr;}}
