#include "sync.h"
#include "identity.h"
#include "sdgo/rgb_paint_native.h"
#include "sdgo/friend_unit_gift_protocol.h"
#include <windows.h>
#include <map>
#include <set>
#include <mutex>
namespace sdgo::rgb_paint::sync {
namespace {
const SdgoApi* api=nullptr;SdgoMod* mod=nullptr;
template<class T>T At(const void* p,unsigned off=0){T v{};std::memcpy(&v,static_cast<const std::uint8_t*>(p)+off,sizeof v);return v;}
bool Copy(const void* p,void* out,std::size_t n){__try{if(!p)return false;std::memcpy(out,p,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
std::recursive_mutex gate;
native::Renderer materials;
using NativeSession=void*(__cdecl*)();NativeSession nativeSession=nullptr;
using Draw=void(__thiscall*)(void*,void*);Draw draw=nullptr;
void *styleSite=nullptr,*humanSite=nullptr,*battleSite=nullptr,*destroySite=nullptr,*queueSite=nullptr;
using App=void*(__cdecl*)();App app=nullptr;
using RoomInfo=const std::uint8_t*(__thiscall*)(void*);RoomInfo roomInfo=nullptr;
void *roomVtable=nullptr,*roomLoadVtable=nullptr,*roomActiveVtable=nullptr;
constexpr unsigned mxcsr=0x1f80;
std::uint64_t sessionKey=0,priority=0,previewUid=0,retryAt=0;
Palette previewColors;
struct Cached{Record record;std::uint64_t fetched=0;unsigned reads=0;};
struct Model{Record record;std::uint64_t seen=0;unsigned draws=0,regions=0,matched=0,battleDraws=0;bool battle=false,room=false;};
std::map<std::uint64_t,Cached> cache;
std::map<void*,Model> models;
std::set<std::uint64_t> wanted;
struct Pilot{std::array<Record,4> slots;unsigned count=0;};
std::map<unsigned,Pilot> pilots;
std::map<std::uintptr_t,Record> battleStyles;
struct RoomStyle{Record record;std::uint64_t at=0;};
std::map<std::uintptr_t,RoomStyle> roomStyles;
sdgo::friend_unit_gift::ClientExtensionSendFn send=nullptr;
sdgo::friend_unit_gift::ClientExtensionStateFn transport=nullptr;
struct Pending{std::uintptr_t token=0;std::uint16_t command=0;std::uint64_t unit=0,session=0,at=0;Saved callback=nullptr;} pending;
struct Queued{Record record;Saved callback=nullptr;std::uint64_t at=0;};
std::optional<Queued> queued;
std::uintptr_t nextToken=0;
UINT_PTR timer=0;HHOOK messages=nullptr;
void __cdecl Received(void* token,std::uint16_t command,std::int32_t status,const std::uint8_t* data,std::uint32_t bytes){
    std::lock_guard<std::recursive_mutex> hold(gate);
    if(!pending.unit||pending.token!=reinterpret_cast<std::uintptr_t>(token)||pending.command!=command||pending.session!=sessionKey)return;
    const auto completed=pending;pending={};Record record;
    bool valid=status==0&&data&&bytes==wire::bytes+4&&wire::Decode(data+4,bytes-4,record)&&record.state.identity.unit==completed.unit;
    const auto result=valid?wire::Get(data,4):99;
    valid=valid&&(result==0||result==12);
    auto found=cache.find(completed.unit);
    if(valid&&found!=cache.end()&&(record.state.revision<found->second.record.state.revision||(record.state.revision==found->second.record.state.revision&&record.state.colors!=found->second.record.state.colors)))valid=false;
    if(valid){record.state.identity.session=sessionKey;auto& c=cache[completed.unit];const auto reads=c.reads+1;c={record,GetTickCount64(),reads};}
    else {wanted.insert(completed.unit);retryAt=GetTickCount64()+3000;}
    if(completed.callback)completed.callback(completed.session,completed.unit,valid&&result==0,valid?&record:nullptr);
}
void Render(void* model){
    std::lock_guard<std::recursive_mutex> hold(gate);const auto found=models.find(model);if(found==models.end())return;
    auto& m=found->second;m.seen=GetTickCount64();const auto uid=m.record.state.identity.unit;
    const auto c=cache.find(uid);const Palette* colors=nullptr;
    if(uid==previewUid)colors=&previewColors;
    else if(c!=cache.end()&&c->second.record.type==m.record.type&&c->second.record.native==m.record.native)colors=&c->second.record.state.colors;
    if(!colors)return;
    unsigned count=0,matched=0;for(unsigned i=0;i<6;++i)if(materials.Apply(model,i,(*colors)[i])){++count;Rgb actual;if((*colors)[i]&&materials.ReadRendered(model,i,actual)&&actual==*(*colors)[i])matched|=1u<<i;}
    m.regions=count;m.matched=matched;++m.draws;if(m.battle)++m.battleDraws;
}
void __fastcall DrawHook(void* self,void*,void* context){Render(self);draw(self,context);}
void __stdcall CaptureDestroy(const std::uint8_t* frame){const auto error=GetLastError();void* model=nullptr;if(Copy(frame-16,&model,sizeof model)){std::lock_guard<std::recursive_mutex> hold(gate);models.erase(model);}SetLastError(error);}
void __stdcall CaptureStyle(const std::uint8_t* frame){
    const auto error=GetLastError();try{
        void* model=nullptr;if(Copy(frame+8,&model,sizeof model)){std::lock_guard<std::recursive_mutex> hold(gate);models.erase(model);}
    }catch(...){ }SetLastError(error);
}
// Observe after the original CALL at verified native callers. Hooking the CALL
// itself would relocate its return address and break other appearance Mods'
// caller identification. At this point cdecl arguments are still on the stack.
void __stdcall CaptureReturn(const std::uint8_t* stack,unsigned battle){
    const auto error=GetLastError();try{
        std::array<std::uint8_t,16> args{};if(!Copy(stack,args.data(),args.size()))return;
        auto* model=At<void*>(args.data());const auto* colors=At<const std::uint8_t*>(args.data(),4);const auto* decals=At<const std::uint8_t*>(args.data(),12);
        Record record;bool bound=false;
        if(!battle&&decals==colors+14)bound=ReadNative(colors-46,record);
        std::lock_guard<std::recursive_mutex> hold(gate);
        if(battle==1&&decals==colors+20){const auto f=battleStyles.find(reinterpret_cast<std::uintptr_t>(colors));if(f!=battleStyles.end()){record=f->second;bound=true;}}
        if(battle==2&&decals==colors+16){const auto f=roomStyles.find(reinterpret_cast<std::uintptr_t>(colors));if(f!=roomStyles.end()&&GetTickCount64()-f->second.at<30000){record=f->second.record;bound=true;}}
        if(bound){BindModel(model,record);models[model].battle=battle==1;models[model].room=battle==2;}else models.erase(model);
    }catch(...){ }SetLastError(error);
}
void __stdcall CaptureHuman(const std::uint8_t* frame){
    const auto error=GetLastError();try{
        std::array<std::uint8_t,24> args{};if(!Copy(frame,args.data(),args.size()))return;
        const auto* source=At<const std::uint8_t*>(args.data(),12);const auto* entry=At<const std::uint8_t*>(args.data(),16);const unsigned deck=At<unsigned>(args.data(),20)&255;
        unsigned owner=0,sourceOwner=0;if(!Copy(entry,&owner,4)||!owner||(owner&0x80000000u))return;
        Pilot p;if(source&&deck<=1&&Copy(source+0x429,&sourceOwner,4)&&sourceOwner==owner){p.count=deck?4:1;for(unsigned i=0;i<p.count;++i)ReadNative(source+(deck?0x19e+i*0x99:0x49),p.slots[i]);}
        std::lock_guard<std::recursive_mutex> hold(gate);if(pilots.size()>12){pilots.clear();battleStyles.clear();}pilots[owner]=p;
        for(unsigned i=0;i<p.count;++i)Want(p.slots[i].state.identity.unit);
    }catch(...){ }SetLastError(error);
}
void __stdcall CaptureBattle(const std::uint8_t* frame){
    const auto error=GetLastError();try{
        std::array<std::uint8_t,20> args{};if(!Copy(frame,args.data(),args.size()))return;
        const auto* record=At<const std::uint8_t*>(args.data(),12);const auto* alternate=At<const std::uint8_t*>(args.data(),16);
        const auto key=reinterpret_cast<std::uintptr_t>(alternate?alternate:record)+(alternate?0x10:0x54);
        std::lock_guard<std::recursive_mutex> hold(gate);battleStyles.erase(key);
        std::array<std::uint8_t,0x344> bytes{};if(!Copy(record,bytes.data(),bytes.size()))return;
        unsigned slot=0;if(alternate){const auto start=reinterpret_cast<std::uintptr_t>(record)+0x44,at=reinterpret_cast<std::uintptr_t>(alternate);if(at<start||(at-start)%0xb4||(at-start)/0xb4>=4)return;slot=static_cast<unsigned>((at-start)/0xb4);}
        const auto p=pilots.find(At<unsigned>(bytes.data()));if(p==pilots.end()||slot>=p->second.count)return;
        const auto& r=p->second.slots[slot];const auto offset=0x44+slot*0xb4;
        if(!r.state.identity.unit||r.type!=At<unsigned>(bytes.data(),offset))return;
        for(unsigned i=0;i<6;++i)if(r.native[i]!=At<std::uint16_t>(bytes.data(),offset+0x10+i*2))return;
        if(battleStyles.size()>=64)battleStyles.clear();battleStyles[key]=r;
    }catch(...){ }SetLastError(error);
}
#define RGB_SITE(Name,Observer,Original) \
__declspec(naked) void Name(){ \
__asm { pushfd } __asm { pushad } __asm { mov eax,esp } \
__asm { sub esp,210h } __asm { and esp,0FFFFFFF0h } \
__asm { mov [esp+200h],eax } __asm { fxsave [esp] } \
__asm { fninit } __asm { ldmxcsr dword ptr [mxcsr] } __asm { cld } \
__asm { push ebp } __asm { call Observer } __asm { fxrstor [esp] } \
__asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd } \
__asm { jmp dword ptr [Original] } }
RGB_SITE(StyleSite,CaptureStyle,styleSite)
RGB_SITE(HumanSite,CaptureHuman,humanSite)
RGB_SITE(BattleSite,CaptureBattle,battleSite)
RGB_SITE(DestroySite,CaptureDestroy,destroySite)
bool RoomEntry(unsigned seat,std::uint8_t* out){
    __try{
        auto* a=app?app():nullptr;if(!a||At<unsigned>(a,0x4a0)!=18)return false;
        auto* state=At<void*>(a,0x4a4);if(!state||At<void*>(state)!=roomVtable)return false;
        auto* manager=At<void*>(state,4);auto* model=manager?At<void*>(manager,0x38):nullptr;
        auto* table=model?At<void*>(model):nullptr;if(!table||At<RoomInfo>(table,0x1c)!=roomInfo)return false;
        const auto* room=roomInfo(model);if(!room||!At<unsigned>(room))return false;
        const auto mode=room[5];const bool mission=mode==5||mode==8;
        if(seat>=(mission?4u:12u))return false;
        return Copy(room+(mission?0x5c6:0x533)+seat*0x48e,out,0x48e);
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// The queue copies only the compact appearance. Capture the seat-qualified
// identity while the original typed message is still available, then key the
// copy used by the worker thread. Never retain the message or roster pointers.
void __stdcall CaptureQueue(const unsigned* regs){
    struct RestoreError{DWORD value=GetLastError();~RestoreError(){SetLastError(value);}} restoreError;
    try{
        std::array<unsigned,8> saved{};if(!Copy(regs,saved.data(),sizeof saved))return;
        const auto* frame=reinterpret_cast<const std::uint8_t*>(saved[2]);
        const auto key=std::uintptr_t(saved[7])+4;
        std::lock_guard<std::recursive_mutex> hold(gate);roomStyles.erase(key);
        const std::uint8_t* source=nullptr;std::array<unsigned,3> header{};
        if(!saved[7]||!Copy(frame+8,&source,4)||!source||!Copy(source-12,header.data(),sizeof header))return;
        if(!((reinterpret_cast<void*>(header[0])==roomLoadVtable&&header[1]==1)||
             (reinterpret_cast<void*>(header[0])==roomActiveVtable&&header[1]==10)))return;
        std::array<std::uint8_t,0x48e> entry{};std::array<std::uint8_t,16> style{};Record r;
        if(!RoomEntry(header[2],entry.data())||!Copy(reinterpret_cast<void*>(saved[7]),style.data(),style.size())||
           !identity::RoomRecord(entry.data(),entry.size(),style.data(),style.size(),sessionKey,r))return;
        if(roomStyles.size()>=64)roomStyles.clear();roomStyles[key]={r,GetTickCount64()};Want(r.state.identity.unit);
    }catch(...){ }
}
__declspec(naked) void QueueSite(){
__asm { pushfd } __asm { pushad } __asm { mov eax,esp }
__asm { sub esp,210h } __asm { and esp,0FFFFFFF0h }
__asm { mov [esp+200h],eax } __asm { fxsave [esp] }
__asm { fninit } __asm { ldmxcsr dword ptr [mxcsr] } __asm { cld }
__asm { push eax } __asm { call CaptureQueue } __asm { fxrstor [esp] }
__asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd }
__asm { jmp dword ptr [queueSite] } }
#undef RGB_SITE
#include "style_returns.h"
void CALLBACK Timer(HWND,UINT,UINT_PTR,DWORD){try{Pump();}catch(...){}}
LRESULT CALLBACK Message(int code,WPARAM w,LPARAM l){const auto error=GetLastError();if(code>=0&&!timer){timer=SetTimer(nullptr,0,100,&Timer);if(timer&&messages){auto h=messages;messages=nullptr;UnhookWindowsHookEx(h);}}SetLastError(error);return CallNextHookEx(nullptr,code,w,l);}
}
std::uint64_t Session(){std::lock_guard<std::recursive_mutex> hold(gate);return sessionKey;}
bool ReadNative(const void* source,Record& out){std::array<std::uint8_t,0x99> bytes{};if(!Copy(source,bytes.data(),bytes.size()))return false;Record r;r.type=At<unsigned>(bytes.data());r.state.identity={Session(),At<std::uint64_t>(bytes.data(),8)};r.state.revision=1;for(unsigned i=0;i<6;++i){r.native[i]=At<std::uint16_t>(bytes.data(),46+i*2);r.state.colors[i]=FromNative(r.native[i]);}wire::Bytes valid;if(!wire::Encode(r,valid))return false;out=r;return true;}
void BindModel(void* model,const Record& r){if(!model||!r.state.identity.unit)return;std::lock_guard<std::recursive_mutex> hold(gate);if(models.size()>=512&&models.find(model)==models.end())models.erase(models.begin());auto& m=models[model];if(m.record.state.identity.unit!=r.state.identity.unit)m={};m.record=r;m.seen=GetTickCount64();Want(r.state.identity.unit);}
void Want(std::uint64_t uid,bool first){if(!uid)return;std::lock_guard<std::recursive_mutex> hold(gate);if(first)priority=uid;const auto found=cache.find(uid);if(found==cache.end()||GetTickCount64()-found->second.fetched>=1500)wanted.insert(uid);}
bool Current(std::uint64_t uid,Record& out){std::lock_guard<std::recursive_mutex> hold(gate);const auto i=cache.find(uid);if(i==cache.end())return false;out=i->second.record;return true;}
void Preview(std::uint64_t uid,const Palette* colors){std::lock_guard<std::recursive_mutex> hold(gate);previewUid=colors?uid:0;if(colors)previewColors=*colors;}
void Pump(){
    std::lock_guard<std::recursive_mutex> hold(gate);auto* session=nativeSession?nativeSession():nullptr;const int connection=session?At<int>(session,0x14):-1;
    const auto key=connection<0?0:(std::uint64_t(reinterpret_cast<std::uintptr_t>(session))<<32)|static_cast<unsigned>(connection);
    if(key!=sessionKey){sessionKey=key;cache.clear();wanted.clear();models.clear();pilots.clear();battleStyles.clear();roomStyles.clear();pending={};queued.reset();previewUid=priority=0;retryAt=0;}
    const auto now=GetTickCount64();
    // Alternate forms can remain dormant for an entire battle. Keep their
    // instance identity until the native destructor/session reset removes it;
    // only network refreshes, not identity, depend on recent rendering.
    for(const auto& pair:models)if(now-pair.second.seen<5000)Want(pair.second.record.state.identity.unit);
    while(cache.size()>512){auto oldest=cache.end();for(auto i=cache.begin();i!=cache.end();++i)if(i->first!=priority&&(oldest==cache.end()||i->second.fetched<oldest->second.fetched))oldest=i;if(oldest==cache.end())break;cache.erase(oldest);}
    while(wanted.size()>512){auto i=std::prev(wanted.end());if(*i==priority){if(i==wanted.begin())break;--i;}wanted.erase(i);}
    if(queued&&now-queued->at>20000){const auto old=*queued;queued.reset();if(old.callback)old.callback(old.record.state.identity.session,old.record.state.identity.unit,false,nullptr);}
    if(pending.unit){if(now-pending.at>20000){const auto old=pending;pending={};wanted.insert(old.unit);retryAt=now+1000;if(old.callback)old.callback(old.session,old.unit,false,nullptr);}else return;}
    if(!send||!transport){const auto carrier=GetModuleHandleW(L"friend_unit_gift_client.dll");if(carrier){send=reinterpret_cast<sdgo::friend_unit_gift::ClientExtensionSendFn>(GetProcAddress(carrier,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));transport=reinterpret_cast<sdgo::friend_unit_gift::ClientExtensionStateFn>(GetProcAddress(carrier,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT));}}
    if(!key||!send||!transport||transport()!=1||now<retryAt)return;
    if(queued){wire::Bytes bytes;const auto old=*queued;if(!wire::Encode(old.record,bytes))return;pending={++nextToken,wire::save,old.record.state.identity.unit,key,now,old.callback};if(send(wire::save,bytes.data(),static_cast<unsigned>(bytes.size()),&Received,reinterpret_cast<void*>(pending.token)))queued.reset();else{pending={};retryAt=now+500;}return;}
    if(wanted.empty())return;
    const auto uid=wanted.count(priority)?priority:*wanted.begin();wanted.erase(uid);std::uint8_t data[8];wire::Put(data,uid,8);pending={++nextToken,wire::get,uid,key,now,nullptr};
    if(!send(wire::get,data,8,&Received,reinterpret_cast<void*>(pending.token))){pending={};wanted.insert(uid);retryAt=now+500;}
}
bool SaveRecord(const Record& record,Saved callback){
    std::lock_guard<std::recursive_mutex> hold(gate);wire::Bytes bytes;
    if(!sessionKey||record.state.identity.session!=sessionKey||queued||pending.command==wire::save||!send||!transport||transport()!=1||!wire::Encode(record,bytes))return false;
    queued=Queued{record,callback,GetTickCount64()};return true;
}
bool Observe(std::uint64_t uid,Observation& out){std::lock_guard<std::recursive_mutex> hold(gate);const auto c=cache.find(uid);if(c==cache.end())return false;Observation o;o.revision=c->second.record.state.revision;o.colors=c->second.record.state.colors;o.reads=c->second.reads;for(const auto& pair:models)if(pair.second.record.state.identity.unit==uid){o.draws+=pair.second.draws;o.regions=(std::max)(o.regions,pair.second.regions);o.matched|=pair.second.matched;if(pair.second.battle){o.battleDraws+=pair.second.battleDraws;o.battleMatched|=pair.second.matched;}}out=o;return true;}
bool Bind(const SdgoApi* owner,SdgoMod* module){
    api=owner;mod=module;if(!api||api->size<offsetof(SdgoApi,process_initial_thread_id)+sizeof(DWORD)||!api->HookSite||!materials.Bind(api,mod))return false;
    nativeSession=reinterpret_cast<NativeSession>(api->Symbol(mod,"storage.session"));if(!nativeSession)return false;
    app=reinterpret_cast<App>(api->Symbol(mod,"game.app"));roomInfo=reinterpret_cast<RoomInfo>(api->Symbol(mod,"flow.mission_room_info"));
    roomVtable=api->Symbol(mod,"flow.three_d_wait_room_vtable");roomLoadVtable=api->Symbol(mod,"rgb_paint.room_load_vtable");roomActiveVtable=api->Symbol(mod,"rgb_paint.room_active_vtable");
    if(!app||!roomInfo||!roomVtable||!roomLoadVtable||!roomActiveVtable)return false;
    if(!BindStyleReturns())return false;
    if(!api->HookSite(mod,"rgb_paint.room_queue",reinterpret_cast<void*>(&QueueSite),&queueSite))return false;
    if(!api->HookSite(mod,"rgb_paint.style_identity",reinterpret_cast<void*>(&StyleSite),&styleSite)||!api->HookSite(mod,"rgb_paint.human_identity",reinterpret_cast<void*>(&HumanSite),&humanSite)||!api->HookSite(mod,"rgb_paint.battle_identity",reinterpret_cast<void*>(&BattleSite),&battleSite)||!api->HookSite(mod,"rgb_paint.model_destroy",reinterpret_cast<void*>(&DestroySite),&destroySite)||!api->HookCode(mod,"rgb_paint.model_draw",reinterpret_cast<void*>(&DrawHook),reinterpret_cast<void**>(&draw)))return false;
    messages=SetWindowsHookExW(WH_GETMESSAGE,&Message,nullptr,api->process_initial_thread_id);return messages!=nullptr;
}
void AbortLoad(){if(messages){UnhookWindowsHookEx(messages);messages=nullptr;}}
bool ObserveScene(SdgoRgbPaintSceneObservationV1& out){
    std::lock_guard<std::recursive_mutex> hold(gate);const auto c=cache.find(out.unit);if(c==cache.end())return false;
    SdgoRgbPaintSceneObservationV1 value;value.unit=out.unit;value.revision=c->second.record.state.revision;const auto now=GetTickCount64();
    for(const auto& pair:models){const auto& m=pair.second;if(m.record.state.identity.unit!=out.unit||!m.draws||now-m.seen>250)continue;
        if(m.room){value.room_draws+=m.draws;value.room_mask|=m.matched;}
        if(m.battle){value.battle_draws+=m.battleDraws;value.battle_mask|=m.matched;}}
    out=value;return true;
}
}
