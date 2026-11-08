#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/match_ai_events.h"
#include "sdgo/ai_dialogue_observer.h"
#include "sdgo/native_text_decoders.h"
#include "../battle_ai/native_capabilities.h"
#include "protocol.h"
#include "policy.h"

namespace {
using namespace sdgo::ai_dialogue;
using battle_ai::native::detail::ReadPointer;
template<class T>bool Read(const void* pointer,size_t offset,T& value) {
    return battle_ai::native::detail::Read(static_cast<const uint8_t*>(pointer),offset,value);
}
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
using App=uint8_t*(__cdecl*)();App app=nullptr;
void** units=nullptr;
using ObjectForPilot=uint32_t(__thiscall*)(void*,uint32_t);
using FindObject=uint8_t*(__thiscall*)(void*,uint32_t);
using Slot=uint8_t*(__thiscall*)(void*,int);
ObjectForPilot objectForPilot=nullptr;FindObject findObject=nullptr;Slot playerSlot=nullptr;
SdgoMatchAiGetLifeCostV1Fn actualLifeCost=nullptr;
using Step=void(__thiscall*)(void*);Step battleLineCtor=nullptr,battleLineDtor=nullptr;
using StringCtor=void*(__thiscall*)(void*,const wchar_t*,size_t);StringCtor stringCtor=nullptr;
using AppendLine=void(__thiscall*)(void*,const void*);AppendLine appendLine=nullptr;
using PacketCtor=void(__thiscall*)(void*,uint32_t);PacketCtor packetCtor=nullptr;
using PacketFill=int(__thiscall*)(void*,uint16_t,uint8_t,const char*,uint8_t,const char*);PacketFill packetFill=nullptr;
using Network=uint8_t*(__cdecl*)();Network network=nullptr;
using Send=int(__thiscall*)(void*,void*,const void*,int,int);Send send=nullptr;
using NativeWide=const wchar_t*(__cdecl*)(unsigned,const char*);NativeWide originalWide=nullptr;
std::wstring config,folder;uint64_t session=0,started=0,checked=0,pollAt=0,lastLine=0;
uint32_t battle=0,nativeSequence=0,sequence=0,requestCount=0,completed=0,failures=0,lastFocus=0;
uint32_t received=0;
bool enabled=false,roomOutput=false,teamTalk=true,always=false;
unsigned probability=35,cooldown=8,maxLines=24;
Request snapshot{},pending{};bool waiting=false;uint8_t* currentMode=nullptr;
std::map<uint32_t,uint32_t> deaths;
std::deque<Request> events;
std::wstring lastDisplayed;
std::deque<std::wstring> decodedLines;

template<class T>bool Bind(T& value,const char* name) {value=reinterpret_cast<T>(g_api->Symbol(g_mod,name));return value!=nullptr;}
bool CopyWide(const wchar_t* source,wchar_t* out,size_t capacity) {
    if(!source||!out||!capacity)return false;
    for(size_t i=0;i+1<capacity;++i){wchar_t c=0;if(!Read(source,i*sizeof(wchar_t),c))return false;out[i]=c;if(!c)return true;}
    out[capacity-1]=0;return true;
}
bool OldString(const uint8_t* object,wchar_t* out,size_t capacity) {
    uint32_t length=0,cap=0;if(!Read(object,0x10,length)||!Read(object,0x14,cap)||length>cap||cap>0x10000)return false;
    const wchar_t* value=reinterpret_cast<const wchar_t*>(object);
    if(cap>7&&!Read(object,0,value))return false;
    return CopyWide(value,out,capacity);
}
bool Write(const std::wstring& file,const void* data,DWORD bytes) {
    const auto temp=file+L".tmp";
    HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    DWORD count=0;const bool ok=WriteFile(h,data,bytes,&count,nullptr)&&count==bytes;CloseHandle(h);
    if(ok&&MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING))return true;
    DeleteFileW(temp.c_str());return false;
}
std::vector<uint8_t> Load(const std::wstring& file,DWORD limit) {
    std::vector<uint8_t> result;
    HANDLE h=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return result;
    const DWORD size=GetFileSize(h,nullptr);
    if(size>0&&size<=limit){result.resize(size);DWORD count=0;if(!ReadFile(h,result.data(),size,&count,nullptr)||count!=size)result.clear();}
    CloseHandle(h);return result;
}
void Settings(uint64_t now) {
    if(now-checked<500)return;checked=now;
    char revision[65]{};
    const bool receipt=g_api->size>=offsetof(SdgoApi,ConfigurationApplied)+sizeof(g_api->ConfigurationApplied)&&
        g_api->ConfigurationRevision&&g_api->ConfigurationApplied&&g_api->ConfigurationRevision(g_mod,revision,sizeof(revision))>0;
    auto number=[&](const wchar_t* key,int fallback,int lo,int hi){return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"settings",key,fallback,config.c_str())),lo,hi);};
    enabled=number(L"Enabled",0,0,1)!=0;teamTalk=number(L"TeamTalk",1,0,1)!=0;
    probability=number(L"ProbabilityPercent",35,0,100);cooldown=number(L"CooldownSeconds",8,0,300);maxLines=number(L"MaxLines",24,1,1000);
    wchar_t value[32]{};GetPrivateProfileStringW(L"settings",L"Visibility",L"local",value,32,config.c_str());roomOutput=wcscmp(value,L"room")==0;
    GetPrivateProfileStringW(L"settings",L"Trigger",L"probability",value,32,config.c_str());always=wcscmp(value,L"always")==0;
    if(receipt)g_api->ConfigurationApplied(g_mod,revision);
}
bool Active() {uint32_t state=0;auto* game=app?app():nullptr;return game&&Read(game,0x4A0,state)&&state==22;}
bool Capture(uint8_t* mode,Request& output) {
    battle_ai::native::detail::ReadableRegionCache regions{};
    battle_ai::native::detail::ScopedReadableRegionCache scope(regions);
    if(!mode||!Active()||!units||!*units)return false;
    Request result{};memcpy(result.magic,"SDGODLG1",8);result.size=sizeof(result);result.pid=GetCurrentProcessId();result.session=session;result.battle=battle;
    uint8_t* room=nullptr;if(!ReadPointer(mode,8,room)||!Read(room,0x68,result.local)||!result.local)return false;
    uint16_t roomMode=0;if(!Read(room,0x66,roomMode))return false;result.mode=roomMode;
    result.limitedLives=(roomMode==3||roomMode==14||roomMode==7)?1u:(roomMode==1||roomMode==9)?0u:2u;
    result.elapsedSeconds=static_cast<uint32_t>((GetTickCount64()-started)/1000);
    for(unsigned group=0;group<2;++group) {
        uint8_t* first=nullptr;uint8_t* last=nullptr;
        if(!ReadPointer(room,group?0x3C:0x1C,first)||!ReadPointer(room,group?0x40:0x20,last))return false;
        const auto begin=reinterpret_cast<uintptr_t>(first),end=reinterpret_cast<uintptr_t>(last);
        if(end<begin||(end-begin)%0x344||(end-begin)/0x344>12)return false;
        for(auto record=begin;record<end;record+=0x344) {
            Participant row{};if(!Read(reinterpret_cast<void*>(record),0,row.pilot)||!row.pilot)continue;
            row.object=objectForPilot(*units,row.pilot);auto* unit=row.object?findObject(*units,row.object):nullptr;
            if(!unit)continue;
            uint32_t own=0;if(!Read(unit,4,own)||own!=row.object||!Read(unit,0x2B68,own)||own!=row.pilot)continue;
            int32_t hp=0;uint8_t* definition=nullptr;uint8_t roomTeam=0;
            if(!ReadPointer(unit,0x9C4,definition)||!Read(definition,0,row.unit)||!row.unit||
               !Read(reinterpret_cast<void*>(record),0x19,roomTeam)||roomTeam<1||roomTeam>2||!Read(unit,0x38,hp))continue;
            // CalculateTeam (0049E0F0) and the original HUD group participants
            // by the battle player record, not an NPC's transient actor team.
            row.team=roomTeam;
            row.ai=group?1u:0u;row.alive=hp>0?1u:0u;row.stockPoints=-1;
            row.deaths=deaths[row.pilot];row.lifeIndex=std::max(1u,row.deaths+(row.alive?1u:0u));
            uint8_t* slot=nullptr;
            for(int index=0;index<12;++index) {
                auto* candidate=playerSlot(*units,index);uint32_t object=0,pilot=0;uint8_t slotTeam=0;uint8_t* copy=nullptr;
                if(candidate&&Read(candidate,4,object)&&object==row.object&&ReadPointer(candidate,0x44,copy)&&
                   Read(copy,0,pilot)&&pilot==row.pilot&&Read(copy,0x19,slotTeam)&&slotTeam==roomTeam){slot=candidate;break;}
            }
            if(!slot||!OldString(slot+8,row.name,32)||!CopyWide(reinterpret_cast<const wchar_t*>(definition+5),row.unitName,50))continue;
            if(!row.name[0]||!row.unitName[0])continue;
            if(result.limitedLives==1) {
                if(!Read(slot,0x48,row.stockPoints))continue;
                if(row.stockPoints<0)row.stockPoints=0;
                uint16_t cost=0;Read(definition,0x137,cost);row.lifeCost=cost;
                if(row.ai)row.lifeCost=actualLifeCost?actualLifeCost(row.pilot):0;
                if(roomMode==7){uint8_t* copy=nullptr;uint32_t points=0;if(ReadPointer(slot,0x44,copy)&&Read(copy,0x30,points)&&points>0&&points<=1000)row.lifeCost=1000/points;}
            }
            row.respawnState=row.alive?1u:result.limitedLives==0?2u:
                result.limitedLives==1&&row.lifeCost?(row.stockPoints<static_cast<int32_t>(row.lifeCost)?3u:2u):0u;
            if(row.ai)Read(unit,0x3130,row.target);
            uint8_t* attack=nullptr;if(ReadPointer(unit,0x9CC,attack))Read(attack,0x240,row.weaponSlot);
            if(result.count<kParticipants)result.roster[result.count++]=row;
        }
    }
    for(unsigned i=0;i<result.count;++i)for(unsigned j=0;j<result.count;++j)
        if(result.roster[j].alive&&result.roster[j].team!=result.roster[i].team&&result.roster[j].target==result.roster[i].object)++result.roster[i].targetedBy;
    int32_t localIndex=-1;uint8_t* localRecord=nullptr;uint32_t localPilot=0;
    if(!Read(*units,0x90,localIndex)||localIndex<0||localIndex>=12)return false;
    auto* localSlot=playerSlot(*units,localIndex);
    if(!localSlot||!ReadPointer(localSlot,0x44,localRecord)||!Read(localRecord,0,localPilot)||localPilot!=result.local)return false;
    output=result;return result.count>=2;
}
const Participant* Find(const Request& event,uint32_t pilot) {for(unsigned i=0;i<event.count;++i)if(event.roster[i].pilot==pilot)return &event.roster[i];return nullptr;}
bool Allowed(uint64_t now) {
    if(!enabled||waiting||requestCount>=maxLines||(lastLine&&now-lastLine<cooldown*1000ULL))return false;
    if(always)return true;
    uint32_t sample=0;if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&sample),sizeof(sample),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
    return probability==100||(probability&&sample<static_cast<uint32_t>((0x100000000ULL*probability)/100));
}
void Queue(Request event,uint32_t kind,uint32_t speaker,uint32_t killer,uint32_t victim,uint32_t focus=0) {
    event.kind=kind;event.speaker=speaker;event.target=event.local;event.killer=killer;event.victim=victim;event.focus=focus;
    const auto now=GetTickCount64();if(!CanAddress(event,snapshot)||!Allowed(now))return;
    if(const auto* who=Find(event,killer)) {
        auto* unit=findObject(*units,who->object);battle_ai::native::Capabilities caps{};
        if(unit&&battle_ai::native::ReadCapabilities(unit,caps)&&caps.selectedSlot>=0&&caps.selectedSlot<4) {
            event.weaponSlot=static_cast<uint32_t>(caps.selectedSlot);
            event.weapon=caps.specialBusy?3u:caps.weapons[caps.selectedSlot].valid?
                caps.weapons[caps.selectedSlot].kind==battle_ai::native::WeaponKind::Melee?1u:2u:0u;
        }
    }
    if(event.created&&now-event.created>10000)return;
    event.sequence=++sequence;if(!event.created)event.created=now;
    if(Write(folder+L"\\request.bin",&event,sizeof(event))){pending=event;waiting=true;lastLine=now;++requestCount;}
}
bool LocalLine(const std::wstring& value) {
    // The battle HUD has its own chat list. ServiceChattingBar is the lobby
    // log and is not rendered in GAME_MAIN. Match the native 004BC620 record
    // path: three original strings, native copy construction and destructor.
    auto* game=app();uint8_t* service=nullptr;uint8_t* mode=nullptr;
    uint32_t count=0;void* head=nullptr;
    if(!game||!ReadPointer(game,0x4A4,service)||!ReadPointer(service,0x0C,mode)||mode!=currentMode||
       !Read(service,0x94,head)||!head||!Read(service,0x98,count)||count>256)return false;
    alignas(4)uint8_t record[0x68]{};battleLineCtor(record);
    stringCtor(record+0x38,value.c_str(),value.size());
    *reinterpret_cast<uint32_t*>(record+0x54)=1000;
    *reinterpret_cast<uint32_t*>(record+0x58)=0x70D9FF;
    *reinterpret_cast<uint32_t*>(record+0x5C)=0x70D9FF;
    *reinterpret_cast<uint32_t*>(record+0x60)=0x70D9FF;
    *reinterpret_cast<uint16_t*>(record+0x64)=10;
    appendLine(service+0x94,record);battleLineDtor(record);
    lastDisplayed=value;
    return true;
}
std::wstring Caption(uint32_t speaker,uint32_t target) {
    const auto* who=Find(snapshot,speaker);const auto* to=Find(snapshot,target);if(!who||!who->ai||!to)return {};
    return std::wstring(who->unitName)+L"：";
}
// Native chat uses a legacy single-byte encoding. The scoped marker carries
// only this Mod's UTF-8 line, preserving ordinary player chat conversion.
const wchar_t* __cdecl DecodeChat(unsigned page,const char* input) {
    static thread_local wchar_t decoded[384]{};
    if(page!=874||!input||strncmp(input,"~AI1:",5)!=0)return originalWide?originalWide(page,input):nullptr;
    if(!Active())return L"";
    const size_t length=strnlen_s(input,81);if(length<24||length>80||input[13]!=':'||input[22]!=':')return originalWide?originalWide(page,input):nullptr;
    unsigned speaker=0,target=0;char tail=0;
    if(sscanf_s(input,"~AI1:%8x:%8x:%c",&speaker,&target,&tail,1)!=3)return originalWide?originalWide(page,input):nullptr;
    const auto caption=Caption(speaker,target);if(caption.empty())return L"";
    wchar_t body[80]{};const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input+23,static_cast<int>(length-23),body,79);
    if(count<=0)return originalWide?originalWide(page,input):nullptr;
    for(int i=0;i<count;++i)if(body[i]<32||body[i]==L'<'||body[i]==L'>')return originalWide?originalWide(page,input):nullptr;
    body[count]=0;const auto* addressee=Find(snapshot,target);if(!addressee||addressee->ai)return L"";
    const auto addressed=AddressBody(addressee->name,body);if(addressed.empty())return L"";
    const auto joined=caption+addressed;if(joined.size()>=384)return originalWide?originalWide(page,input):nullptr;
    wcscpy_s(decoded,joined.c_str());lastDisplayed=joined;++received;
    decodedLines.push_back(joined);if(decodedLines.size()>16)decodedLines.pop_front();return decoded;
}
const wchar_t* __cdecl DecodeRegistered(unsigned page,const char* input,void*) {return DecodeChat(page,input);}
void NormalizeDecodedLines() {
    if(decodedLines.empty())return;
    auto* game=app();uint8_t* service=nullptr;void* head=nullptr;uint8_t* node=nullptr;
    if(!game||!ReadPointer(game,0x4A4,service)||!Read(service,0x94,head)||!head||!Read(head,4,node))return;
    for(unsigned visited=0;node&&node!=head&&visited<256;++visited) {
        auto* record=node+8;wchar_t body[384]{};
        if(OldString(record+0x38,body,384)) {
            const auto found=std::find(decodedLines.begin(),decodedLines.end(),std::wstring(body));
            if(found!=decodedLines.end()) {
                // Preserve the real room-chat envelope. Only the qualified
                // Mod line's battle presentation uses the concise unit voice.
                stringCtor(record,L"",0);stringCtor(record+0x1C,L"",0);
                *reinterpret_cast<uint16_t*>(record+0x64)=10;
                decodedLines.erase(found);
            }
        }
        uint8_t* previous=nullptr;if(!Read(node,4,previous))break;node=previous;
    }
}
bool RoomLine(uint32_t speaker,uint32_t target,std::wstring line) {
    char prefix[32]{};sprintf_s(prefix,"~AI1:%08x:%08x:",speaker,target);
    std::string encoded;
    while(!line.empty()) {
        const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,line.data(),static_cast<int>(line.size()),nullptr,0,nullptr,nullptr);
        if(count>0&&count<=57){encoded.resize(count);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,line.data(),static_cast<int>(line.size()),encoded.data(),count,nullptr,nullptr);break;}
        line.pop_back();if(!line.empty()&&line.back()>=0xD800&&line.back()<=0xDBFF)line.pop_back();
    }
    if(encoded.empty())return false;encoded.insert(0,prefix);
    uint8_t packet[0x6C]{};packetCtor(packet,0);
    if(!packetFill(packet,0,static_cast<uint8_t>(encoded.size()),encoded.c_str(),0,nullptr))return false;
    auto* net=network();void* socket=nullptr;if(!net||!Read(net,0x14,socket)||!socket)return false;
    ++*reinterpret_cast<uint32_t*>(net+0x58);*reinterpret_cast<uint32_t*>(net+0x5C)+=sizeof(packet);
    return send(net,socket,packet,sizeof(packet),0)!=0;
}
void Replies(uint64_t now) {
    if(!waiting)return;
    if(!enabled||now-pending.created>10000||!CanAddress(pending,snapshot)){
        waiting=false;DeleteFileW((folder+L"\\request.bin").c_str());DeleteFileW((folder+L"\\reply.bin").c_str());return;
    }
    const auto raw=Load(folder+L"\\reply.bin",124);if(raw.size()<sizeof(Reply))return;
    Reply reply{};memcpy(&reply,raw.data(),sizeof(reply));
    if(memcmp(reply.magic,"SDGODLR1",8)||reply.session!=session||reply.sequence!=pending.sequence||reply.battle!=battle||reply.length<1||reply.length>48||raw.size()!=sizeof(reply)+reply.length*2)return;
    std::wstring body(reply.length,L'\0');memcpy(body.data(),raw.data()+sizeof(reply),reply.length*2);
    for(wchar_t c:body)if(c<32||c==L'<'||c==L'>')return;
    if(body.front()==L'!'||body.front()==L'/')return;
    const auto caption=Caption(pending.speaker,pending.target);
    if(!caption.empty()) {
        if(!roomOutput){if(LocalLine(caption+body))++completed;else ++failures;}
        else if(RoomLine(pending.speaker,pending.target,body))++completed;
        else ++failures;
    }
    waiting=false;DeleteFileW((folder+L"\\request.bin").c_str());DeleteFileW((folder+L"\\reply.bin").c_str());
}
void Tick(uint8_t* mode,uint32_t sequenceId,void*) {
    const auto now=GetTickCount64();Settings(now);
    if(!enabled){events.clear();waiting=false;DeleteFileW((folder+L"\\request.bin").c_str());}
    if(!Active())return;
    if(mode!=currentMode||nativeSequence!=sequenceId) {
        currentMode=mode;nativeSequence=sequenceId;++battle;started=now;requestCount=0;lastLine=0;waiting=false;deaths.clear();events.clear();decodedLines.clear();lastFocus=0;
        DeleteFileW((folder+L"\\request.bin").c_str());DeleteFileW((folder+L"\\reply.bin").c_str());
    }
    if(now-pollAt<200)return;pollAt=now;
    Request fresh{};if(!Capture(mode,fresh))return;snapshot=fresh;
    NormalizeDecodedLines();
    if(enabled)Write(folder+L"\\heartbeat.bin",&now,sizeof(now));
    Replies(now);
    for(auto& event:events)if(event.sequence==0) {
        // The authoritative notice decrements the native stock before the
        // next mode Update. Capture that post-notice value, not the pre-kill
        // budget that was visible when the notification arrived.
        for(unsigned i=0;i<event.count;++i)if(const auto* current=Find(snapshot,event.roster[i].pilot)) {
            event.roster[i].stockPoints=current->stockPoints;
            event.roster[i].respawnState=current->respawnState;
            event.roster[i].lifeCost=current->lifeCost;
        }
        event.sequence=UINT32_MAX; // post-notice facts copied; not a wire sequence
    }
    while(!events.empty()&&!waiting) {
        if(now-events.front().created>10000){events.pop_front();continue;}
        if(lastLine&&now-lastLine<cooldown*1000ULL)break;
        auto event=events.front();events.pop_front();
        Queue(event,event.kind,event.speaker,event.killer,event.victim,event.focus);
    }
    if(!enabled||!teamTalk||waiting)return;
    const auto* local=Find(snapshot,snapshot.local);if(!local||!local->alive)return;
    for(unsigned i=0;i<snapshot.count;++i) {
        const auto& bot=snapshot.roster[i];if(!bot.ai||!bot.alive||bot.team!=local->team||!bot.target)continue;
        // Deterministic assignment among allied humans prevents every client
        // from generating the same autonomous teammate line.
        std::vector<uint32_t> humans;for(unsigned j=0;j<snapshot.count;++j)if(!snapshot.roster[j].ai&&snapshot.roster[j].team==bot.team)humans.push_back(snapshot.roster[j].pilot);
        std::sort(humans.begin(),humans.end());if(humans.empty()||humans[bot.object%humans.size()]!=snapshot.local)continue;
        for(unsigned j=0;j<snapshot.count;++j)if(snapshot.roster[j].object==bot.target&&snapshot.roster[j].team!=bot.team&&snapshot.roster[j].alive&&bot.target!=lastFocus) {
            lastFocus=bot.target;Queue(snapshot,3,bot.pilot,0,0,snapshot.roster[j].pilot);return;
        }
    }
}
void Packet(uint16_t opcode,const uint8_t* packet,void*) {
    if(opcode!=0x0570||!packet||!currentMode||!Active())return;
    uint32_t killer=0;uint8_t count=0;if(!Read(packet,6,killer)||!Read(packet,0xB,count)||count>12)return;
    Request event{};if(!Capture(currentMode,event))return;
    const auto* who=Find(event,killer);if(!who)return;
    for(unsigned i=0;i<count;++i) {
        uint32_t victim=0;if(!Read(packet,0xC+i*0xB,victim))return;
        const auto* dead=Find(event,victim);if(!dead)continue;
        ++deaths[victim];
        for(unsigned j=0;j<event.count;++j)if(event.roster[j].pilot==victim){event.roster[j].alive=0;event.roster[j].deaths=deaths[victim];event.roster[j].lifeIndex=deaths[victim];}
        event.kind=0;
        if(killer==event.local&&dead->ai&&dead->team!=who->team){event.kind=2;event.speaker=victim;}
        else if(victim==event.local&&who->ai&&dead->team!=who->team){event.kind=1;event.speaker=killer;}
        if(event.kind&&events.size()<12){event.killer=killer;event.victim=victim;event.target=event.local;event.created=GetTickCount64();events.push_back(event);}
    }
}
void TickCpp(uint8_t* mode,uint32_t seq,void* user) {try{Tick(mode,seq,user);}catch(...){++failures;}}
void PacketCpp(uint16_t op,const uint8_t* data,void* user) {try{Packet(op,data,user);}catch(...){++failures;}}
void TickGuard(uint8_t* mode,uint32_t seq,void* user) {__try{TickCpp(mode,seq,user);}__except(EXCEPTION_EXECUTE_HANDLER){++failures;enabled=false;}}
void PacketGuard(uint16_t op,const uint8_t* data,void* user) {__try{PacketCpp(op,data,user);}__except(EXCEPTION_EXECUTE_HANDLER){++failures;}}
}

extern "C" __declspec(dllexport) int __cdecl SdgoAiDialogueGetStatusV1(SdgoAiDialogueStatusV1* output) {
    if(!output||output->size!=sizeof(*output)||output->version!=1)return 0;
    SdgoAiDialogueStatusV1 status{};status.size=sizeof(status);status.version=1;status.battle=battle;
    status.requests=requestCount;status.completed=completed;status.failures=failures;status.waiting=waiting?1u:0u;status.participants=snapshot.count;
    status.last_kind=pending.kind;status.last_speaker=pending.speaker;status.last_target=pending.target;
    status.enabled=enabled?1u:0u;
    status.received=received;status.room_visibility=roomOutput?1u:0u;
    wcsncpy_s(status.last_line,lastDisplayed.c_str(),_TRUNCATE);*output=status;return 1;
}

SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    if(!Bind(app,"game.app")||!Bind(units,"game.unit_manager")||!Bind(objectForPilot,"um.object_id_from_pilot")||!Bind(findObject,"um.find_object")||!Bind(playerSlot,"um.player_slot")||
       !Bind(battleLineCtor,"dialogue.battle_chat.ctor")||!Bind(battleLineDtor,"dialogue.battle_chat.dtor")||!Bind(appendLine,"dialogue.battle_chat.append")||!Bind(stringCtor,"flow.native_wstring_ctor")||
       !Bind(packetCtor,"dialogue.chat.packet_ctor")||!Bind(packetFill,"dialogue.chat.packet_fill")||!Bind(network,"flow.network")||!Bind(send,"flow.network_send"))return 0;
    const auto provider=GetModuleHandleW(L"match_ai.dll");
    const auto attach=provider?reinterpret_cast<SdgoMatchAiRegisterEventsV1Fn>(GetProcAddress(provider,SDGO_MATCH_AI_EVENTS_EXPORT)):nullptr;
    actualLifeCost=provider?reinterpret_cast<SdgoMatchAiGetLifeCostV1Fn>(GetProcAddress(provider,SDGO_MATCH_AI_LIFE_COST_EXPORT)):nullptr;
    if(!attach)return 0;
    config=(std::filesystem::path(g_api->ModDirectory(g_mod))/L"config.ini").wstring();
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&session),sizeof(session),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0||!session)return 0;
    wchar_t identity[80]{};swprintf_s(identity,L"%lu-%016llx",GetCurrentProcessId(),session);
    folder=(std::filesystem::path(g_api->ModDirectory(g_mod)).parent_path().parent_path()/L"local-state"/L"ai-dialogue"/identity).wstring();
    try{std::filesystem::create_directories(folder);}catch(...){return 0;}
    const auto labels=GetModuleHandleW(L"shop_labels.dll");
    if(labels) {
        const auto registerDecoder=reinterpret_cast<SdgoRegisterNativeTextDecoderV1Fn>(GetProcAddress(labels,SDGO_NATIVE_TEXT_DECODER_EXPORT));
        const SdgoNativeTextDecoderV1 decoder{sizeof(decoder),1,&DecodeRegistered,nullptr};
        if(!registerDecoder||!registerDecoder(&decoder))return 0;
    } else if(!g_api->HookCode(g_mod,"dialogue.chat.decode",reinterpret_cast<void*>(&DecodeChat),reinterpret_cast<void**>(&originalWide)))return 0;
    const SdgoMatchAiEventsV1 hooks{sizeof(hooks),1,&TickGuard,&PacketGuard,nullptr};
    return attach(&hooks);
}
