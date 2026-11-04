#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <mshtml.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>
#include <intrin.h>
#include "sdgo/sdgo_api.h"
#include "sdgo/announcement_policy.h"
#include "embedded_browser.h"
#include "news_runtime.h"

namespace {
namespace policy=sdgo::announcements;
const SdgoApi* api;
SdgoMod* mod;
using Update=void(__thiscall*)(uint8_t*);
using Navigate=void(__thiscall*)(void*,const wchar_t*);
using Visible=void(__thiscall*)(void*,uint8_t);
using LoginEvent=int(__stdcall*)(void*,uint32_t,void*,void*);
using Getter=uint8_t*(__cdecl*)();
using Enqueue=void(__thiscall*)(void*,const void*);
Update originalLogin,originalGame;
Navigate navigate,originalNavigate;
Visible visible;
Visible originalVisible;
using BrowserInitialize=uint8_t(__thiscall*)(void*);
using BrowserCreate=uint32_t(__thiscall*)(void*);
BrowserInitialize originalBrowserInitialize;
BrowserCreate originalBrowserCreate;
void* loginBrowserReturn=nullptr;
thread_local bool embeddedLogin=false;
LoginEvent loginEvent;
Getter app;
Enqueue enqueue;
uint8_t** profilePointer;
std::wstring directory,runtime,statePath,originalUrl;
std::vector<policy::Rule> rules;
std::vector<policy::Rule> authoredRules;
policy::news::Engine news;
policy::Scheduler scheduler;
FILETIME fileStamp{};
int offset=0,lastState=-1;
bool serverLocalTime=true;
uint64_t lastPoll=0,loginUntil=0,gameUntil=0;
bool inLogin=false,loginManaged=false;
void* lastViewer=nullptr;
uint32_t loginShown=0,gameShown=0,nativeGameConsumed=0;
std::wstring lastLoginId,lastGameId;
std::wstring pendingLogin,pendingGame;
uint64_t pendingLoginSince=0;
std::vector<std::wstring> gameEvents;
uint64_t lastGameEvaluation=0;

std::wstring Read(const std::wstring& file,const std::wstring& section,const wchar_t* key) {
    std::array<wchar_t,2048> text{};
    GetPrivateProfileStringW(section.c_str(),key,L"",text.data(),static_cast<DWORD>(text.size()),file.c_str());
    return text.data();
}
int Number(const std::wstring& file,const std::wstring& section,const wchar_t* key,int fallback=0) {
    const auto text=Read(file,section,key);if(text.empty())return fallback;
    wchar_t* end=nullptr;const long value=wcstol(text.c_str(),&end,10);
    return end && !*end && value>=INT_MIN && value<=INT_MAX?static_cast<int>(value):fallback;
}
bool Identifier(const std::wstring& text) {
    return !text.empty() && text.size()<=48 && text[0]>=L'a' && text[0]<=L'z' &&
        std::all_of(text.begin(),text.end(),[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'0'&&c<=L'9')||c==L'_';});
}
const policy::Rule* Find(const std::wstring& id) {
    const auto p=std::find_if(rules.begin(),rules.end(),[&](const auto& r){return r.id==id;});
    return p==rules.end()?nullptr:&*p;
}
policy::Clock Clock() {
    FILETIME utc{};GetSystemTimeAsFileTime(&utc);
    ULARGE_INTEGER time{};time.LowPart=utc.dwLowDateTime;time.HighPart=utc.dwHighDateTime;
    policy::Clock clock;clock.seconds=time.QuadPart/10000000;
    SYSTEMTIME date{};
    if(serverLocalTime){FILETIME local{};FileTimeToLocalFileTime(&utc,&local);FileTimeToSystemTime(&local,&date);}
    else {
        time.QuadPart=static_cast<uint64_t>(static_cast<int64_t>(time.QuadPart)+static_cast<int64_t>(offset)*600000000);
        FILETIME shifted{time.LowPart,time.HighPart};FileTimeToSystemTime(&shifted,&date);
    }
    clock.date=date.wYear*10000+date.wMonth*100+date.wDay;
    clock.weekday=(date.wDayOfWeek+6)%7;clock.minute=date.wHour*60+date.wMinute;
    FILETIME local{};SystemTimeToFileTime(&date,&local);time.LowPart=local.dwLowDateTime;time.HighPart=local.dwHighDateTime;
    time.QuadPart-=864000000000;local={time.LowPart,time.HighPart};FileTimeToSystemTime(&local,&date);
    clock.previous_date=date.wYear*10000+date.wMonth*100+date.wDay;
    return clock;
}
bool SaveCounts() {
    std::wstring text=L"; Local announcement counts. Not a shared rule file.\r\n";
    for(const auto& entry:scheduler.counts) {
        const auto& c=entry.second;
        text+=L"["+entry.first+L"]\r\ndate="+std::to_wstring(c.date)+L"\r\ndaily="+std::to_wstring(c.daily)+
            L"\r\ntotal="+std::to_wstring(c.total)+L"\r\nlast="+std::to_wstring(c.last)+L"\r\n";
    }
    const auto temp=statePath+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const wchar_t bom=0xfeff;
    const DWORD size=static_cast<DWORD>(text.size()*sizeof(wchar_t));
    const bool ok=WriteFile(file,&bom,2,&written,nullptr) && written==2 &&
        WriteFile(file,text.data(),size,&written,nullptr) && written==size && FlushFileBuffers(file);
    CloseHandle(file);
    if(!ok || !MoveFileExW(temp.c_str(),statePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());return false;
    }
    return true;
}
void Load() {
    const uint64_t now=GetTickCount64();if(now-lastPoll<500)return;lastPoll=now;
    news.Poll();
    // Cached articles are filtered again against absolute expiry and persisted
    // read state on every decision boundary, including while completely offline.
    const auto loadCount=[](const std::wstring& id){
        if(scheduler.counts.count(id))return;
        policy::Count count;count.date=Number(statePath,id,L"date");
        count.daily=static_cast<uint32_t>(std::max(0,Number(statePath,id,L"daily")));
        count.total=static_cast<uint32_t>(std::max(0,Number(statePath,id,L"total")));
        count.last=_wcstoui64(Read(statePath,id,L"last").c_str(),nullptr,10);scheduler.counts[id]=count;
    };
    for(const auto& feed:news.Snapshot().feeds)for(const auto& item:feed.items)loadCount(item.id);
    loadCount(L"@news.login");loadCount(L"@news.game");
    auto dynamic=policy::news::Rules(news.Config(),news.Snapshot(),scheduler,policy::news::Now());
    for(const auto& r:dynamic)loadCount(r.id);
    rules=authoredRules;rules.insert(rules.end(),dynamic.begin(),dynamic.end());
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(runtime.c_str(),GetFileExInfoStandard,&data))return;
    if(CompareFileTime(&data.ftLastWriteTime,&fileStamp)==0)return;
    fileStamp=data.ftLastWriteTime;
    std::vector<policy::Rule> next;
    const int count=Number(runtime,L"announcements",L"count",-1);
    const auto rawOffset=Read(runtime,L"announcements",L"offset");
    const bool nextServerLocal=rawOffset.empty() || _wcsicmp(rawOffset.c_str(),L"local")==0;
    const int nextOffset=nextServerLocal?0:Number(runtime,L"announcements",L"offset",9999);
    if(Number(runtime,L"announcements",L"schema")!=1 || count<0 || count>128 || nextOffset<-720 || nextOffset>840)return;
    for(int i=0;i<count;++i) {
        const auto section=L"rule"+std::to_wstring(i);policy::Rule r;
        r.id=Read(runtime,section,L"id");r.title=Read(runtime,section,L"title");r.page=Read(runtime,section,L"page");
        r.enabled=Number(runtime,section,L"enabled")!=0;
        r.surfaces=Number(runtime,section,L"surfaces");r.triggers=Number(runtime,section,L"triggers");
        r.days=Number(runtime,section,L"days");r.start=Number(runtime,section,L"start");r.end=Number(runtime,section,L"end");
        r.date_start=Number(runtime,section,L"date_start");r.date_end=Number(runtime,section,L"date_end");
        r.duration=static_cast<uint32_t>(Number(runtime,section,L"duration_seconds"));
        r.interval=static_cast<uint32_t>(Number(runtime,section,L"interval_seconds"));
        r.max_session=static_cast<uint32_t>(Number(runtime,section,L"max_session"));
        r.max_day=static_cast<uint32_t>(Number(runtime,section,L"max_day"));
        r.max_total=static_cast<uint32_t>(Number(runtime,section,L"max_total"));
        r.priority=static_cast<uint32_t>(Number(runtime,section,L"priority"));
        const int lineCount=Number(runtime,section,L"lines");
        if(!Identifier(r.id) || r.title.empty() || r.title.size()>160 || r.duration<3 || r.duration>120 ||
           r.interval<3 || r.interval>86400 || r.start<0 || r.start>1439 || r.end<0 || r.end>1439 ||
           !r.surfaces || (r.surfaces&~3) || !r.triggers || (r.triggers&~127) || !r.days || (r.days&~127) ||
           lineCount<1 || lineCount>64 || r.priority>100 || r.max_session>1000000 || r.max_day>1000000 || r.max_total>1000000 ||
           r.page.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789_.")!=std::wstring::npos ||
           r.page.find(L"..")!=std::wstring::npos || r.page.find(r.id+L"_")!=0)return;
        for(int j=0;j<lineCount;++j)r.lines.push_back(Read(runtime,section,(L"line"+std::to_wstring(j)).c_str()));
        if(std::any_of(next.begin(),next.end(),[&](const auto& x){return x.id==r.id;}))return;
        if(!scheduler.counts.count(r.id)) {
            policy::Count c;c.date=Number(statePath,r.id,L"date");
            c.daily=static_cast<uint32_t>(std::max(0,Number(statePath,r.id,L"daily")));
            c.total=static_cast<uint32_t>(std::max(0,Number(statePath,r.id,L"total")));
            c.last=_wcstoui64(Read(statePath,r.id,L"last").c_str(),nullptr,10);scheduler.counts[r.id]=c;
        }
        next.push_back(std::move(r));
    }
    authoredRules=std::move(next);rules=authoredRules;rules.insert(rules.end(),dynamic.begin(),dynamic.end());offset=nextOffset;serverLocalTime=nextServerLocal;
    api->Log(mod,SDGO_LOG_INFO,"loaded %u authored announcement rules",static_cast<unsigned>(authoredRules.size()));
}
// Native MSVC strings have a 16-byte SSO buffer, then size/capacity. Vector
// elements additionally retain the host allocator word. These are non-owning
// sources only: the verified queue constructor deep-copies into host allocations.
struct NativeString { union { wchar_t buffer[8]; const wchar_t* pointer; };uint32_t size,capacity; };
struct NativeLine { NativeString text;uint32_t allocator=0; };
NativeString Borrow(const std::wstring& text) {
    NativeString s{};s.size=static_cast<uint32_t>(text.size());s.capacity=s.size>7?s.size:7;
    if(s.size>7)s.pointer=text.c_str();else std::copy(text.begin(),text.end(),s.buffer);
    return s;
}
bool Same(const NativeString& value,const std::wstring& expected) {
    const wchar_t* text=value.capacity>7?value.pointer:value.buffer;
    return value.size==expected.size() && text && std::equal(expected.begin(),expected.end(),text);
}
struct Record { uint8_t kind=1,seconds=10;uint16_t unused=0;NativeString title{};uint32_t allocator=0;
    NativeLine* begin=nullptr;NativeLine* end=nullptr;NativeLine* capacity=nullptr;uint32_t vectorAllocator=0; };
static_assert(sizeof(NativeString)==24 && sizeof(NativeLine)==28 && sizeof(Record)==48,"native announcement ABI");
bool GameShow(const policy::Rule& r) {
    uint8_t* profile=*profilePointer;if(!profile || profile[0x62d] || *reinterpret_cast<uint32_t*>(profile+0x5f0))return false;
    std::vector<NativeLine> lines;for(const auto& text:r.lines)lines.push_back({Borrow(text),0});
    Record record;record.seconds=static_cast<uint8_t>(r.duration);record.title=Borrow(r.title);
    record.begin=lines.data();record.end=lines.data()+lines.size();record.capacity=record.end;
    enqueue(profile+0x5e0,&record);
    return true;
}
bool HasLoginRules(){return (news.Config().enabled&&news.Config().login) || std::any_of(rules.begin(),rules.end(),[](const auto& r){return r.enabled&&(r.surfaces&1);});}
uint32_t __fastcall BrowserCreateHook(void* native,void*) {
    if(!embeddedLogin)return originalBrowserCreate(native);
    const HRESULT result=announcement_browser::Create(native);
    api->Log(mod,SUCCEEDED(result)?SDGO_LOG_INFO:SDGO_LOG_WARN,"embedded login browser initialization hr=%08lX",static_cast<unsigned long>(result));
    return SUCCEEDED(result)?1u:0u;
}
uint8_t __fastcall BrowserInitializeHook(void* native,void*) {
    Load();const bool previous=embeddedLogin;
    embeddedLogin=_ReturnAddress()==loginBrowserReturn && HasLoginRules();
    const uint8_t result=originalBrowserInitialize(native);embeddedLogin=previous;return result;
}
void __fastcall VisibleHook(void* native,void*,uint8_t shown) {
    originalVisible(native,shown);announcement_browser::Visible(native,shown!=0);
}
void __fastcall NavigateHook(void* viewer,void*,const wchar_t* url) {
    if(url && wcsstr(url,L"/Announce/")){
        originalUrl=url;lastViewer=viewer;Load();
        if(HasLoginRules()){
            originalNavigate(viewer,L"about:blank");return;
        }
    }
    originalNavigate(viewer,url);
}
void Mark(const policy::Rule& r,const policy::Clock& clock,int surface) {
    scheduler.Shown(r,clock);
    if(!SaveCounts())api->Log(mod,SDGO_LOG_WARN,"announcement counts could not be persisted");
    if(surface==1){++loginShown;lastLoginId=r.id;}else {++gameShown;lastGameId=r.id;}
    const std::string id(r.id.begin(),r.id.end());
    uint8_t* application=app();const int nativeState=application?*reinterpret_cast<int*>(application+0x4a0):-1;
    api->Log(mod,SDGO_LOG_INFO,"announcement shown surface=%s rule=%s total=%u native_state=%d",
        surface==1?"login":"game",id.c_str(),scheduler.counts[r.id].total,nativeState);
}
bool BrowserContains(void* viewer,const policy::Rule& rule) {
    auto* browser=*reinterpret_cast<IWebBrowser2**>(static_cast<uint8_t*>(viewer)+8);
    if(!browser)return false;
    IDispatch* dispatch=nullptr;IHTMLDocument2* document=nullptr;IHTMLElement* body=nullptr;BSTR text=nullptr;
    BSTR location=nullptr;
    const bool correct=SUCCEEDED(browser->get_LocationURL(&location)) && location && wcsstr(location,rule.page.c_str());
    if(location)SysFreeString(location);if(!correct)return false;
    bool found=false;
    if(SUCCEEDED(browser->get_Document(&dispatch)) && dispatch &&
       SUCCEEDED(dispatch->QueryInterface(__uuidof(IHTMLDocument2),reinterpret_cast<void**>(&document))) &&
       SUCCEEDED(document->get_body(&body)) && body && SUCCEEDED(body->get_innerText(&text)) && text)
        found=wcsstr(text,rule.title.c_str())!=nullptr;
    if(text)SysFreeString(text);if(body)body->Release();if(document)document->Release();if(dispatch)dispatch->Release();
    return found;
}
void __fastcall LoginUpdate(uint8_t* self,void*) {
    originalLogin(self);Load();
    auto* viewer=*reinterpret_cast<void**>(self+0xc4);if(!viewer || self[0xf4])return;
    // The service can receive its first update while the native browser wrapper
    // exists but its COM interface has not yet been initialized.
    if(!*reinterpret_cast<void**>(static_cast<uint8_t*>(viewer)+8))return;
    const bool managed=HasLoginRules();
    const uint64_t now=GetTickCount64();
    const bool entered=!inLogin || viewer!=lastViewer;inLogin=true;lastViewer=viewer;
    if(!pendingLogin.empty()) {
        const auto* rule=Find(pendingLogin);
        if(rule && static_cast<uint8_t*>(viewer)[0x24] && BrowserContains(viewer,*rule)) {
            Mark(*rule,Clock(),1);loginUntil=now+rule->duration*1000;pendingLogin.clear();
        } else if(!rule || now-pendingLoginSince>30000) {
            api->Log(mod,SDGO_LOG_WARN,"native login announcement did not become visible");pendingLogin.clear();loginUntil=now+30000;
        }
    }
    if(!managed) {
        if(loginManaged){loginEvent(self,0x101,nullptr,*reinterpret_cast<void**>(self+0xd8));
            if(!originalUrl.empty())originalNavigate(viewer,originalUrl.c_str());loginManaged=false;loginUntil=0;}
        return;
    }
    if(loginUntil && now>=loginUntil){loginEvent(self,0x101,nullptr,*reinterpret_cast<void**>(self+0xd8));loginUntil=0;}
    if(!loginUntil && pendingLogin.empty()) {
        const auto clock=Clock();const int selected=scheduler.Select(rules,1,policy::Timer|(entered?policy::Enter:0),clock);
        if(selected>=0) {
            const auto& r=rules[static_cast<size_t>(selected)];
            if(r.dynamic && !news.WritePage(r))return;
            const auto page=r.dynamic?news.PagePath(r.page):directory+L"\\pages\\"+r.page;
            if(GetFileAttributesW(page.c_str())==INVALID_FILE_ATTRIBUTES)return;
            originalNavigate(viewer,page.c_str());loginEvent(self,0x101,nullptr,*reinterpret_cast<void**>(self+0xd4));
            pendingLogin=r.id;pendingLoginSince=now;loginManaged=true;
        } else if(!loginManaged)visible(viewer,0);
    }
}
void __fastcall GameUpdate(uint8_t* self,void*) {
    Load();const uint64_t now=GetTickCount64();
    uint8_t* application=app();const int state=application?*reinterpret_cast<int*>(application+0x4a0):-1;
    if(state<5 || state==25){originalGame(self);return;}
    if(now-lastGameEvaluation<250 && state==lastState && !inLogin && pendingGame.empty()){
        originalGame(self);return;
    }
    lastGameEvaluation=now;
    uint32_t events=policy::Timer;
    if(inLogin || lastState<0){events|=policy::Enter|policy::Login;inLogin=false;loginUntil=0;loginManaged=false;pendingLogin.clear();announcement_browser::Close();}
    if(state!=lastState) {
        if(state==6)events|=policy::Lobby;
        if(state==18 || state==20)events|=policy::Room;
        if(state==22)events|=policy::BattleStart;
        if(lastState==22 && state!=22)events|=policy::BattleEnd;
        lastState=state;
    }
    const auto clock=Clock();
    for(const auto& r:rules) {
        if((r.surfaces&2) && (r.triggers&events&~policy::Timer) && policy::InWindow(r,clock) &&
           std::find(gameEvents.begin(),gameEvents.end(),r.id)==gameEvents.end())gameEvents.push_back(r.id);
    }
    gameEvents.erase(std::remove_if(gameEvents.begin(),gameEvents.end(),[&](const auto& id){
        const auto* r=Find(id);return !r || !policy::InWindow(*r,clock);}),gameEvents.end());
    auto candidates=rules;
    for(auto& r:candidates)if(!(r.triggers&policy::Timer) &&
        std::find(gameEvents.begin(),gameEvents.end(),r.id)==gameEvents.end())r.enabled=false;
    for(auto& r:candidates)if(r.lobby_only && state!=6)r.enabled=false;
    int chosen=-1;
    if(now>=gameUntil && pendingGame.empty()){chosen=scheduler.Select(candidates,2,127,clock);
        if(chosen>=0 && !GameShow(rules[static_cast<size_t>(chosen)]))chosen=-1;}
    if(chosen>=0){pendingGame=rules[static_cast<size_t>(chosen)].id;
        gameEvents.erase(std::remove(gameEvents.begin(),gameEvents.end(),pendingGame),gameEvents.end());}
    originalGame(self);
    if(!pendingGame.empty()) {
        const auto* selected=Find(pendingGame);if(!selected){pendingGame.clear();return;}
        const auto& r=*selected;
        // The original renderer has consumed this queue entry and set the
        // genuine native title, body and active flag before a receipt is counted.
        auto* profile=*profilePointer;
        if(profile && profile[0x62d] && Same(*reinterpret_cast<NativeString*>(profile+0x600),r.title)){
            ++nativeGameConsumed;Mark(r,Clock(),2);pendingGame.clear();
            gameUntil=now+static_cast<uint64_t>(r.duration)*((r.lines.size()+2)/3)*1000+1000;}
    }
}
}

// Read-only native GameTest observation; no synthetic trigger or display entry point.
extern "C" __declspec(dllexport) uint32_t __cdecl SdgoAnnouncementObserved(uint32_t surface) {
    return surface==1?loginShown:surface==2?gameShown:surface==3?nativeGameConsumed:0;
}
SDGO_MOD_ENTRY {
    if(!api || api->version!=SDGO_API_VERSION)return 0;
    ::api=api;::mod=mod;directory=api->ModDirectory(mod);runtime=directory+L"\\runtime.ini";news.Initialize(directory);
    const auto root=directory.substr(0,directory.find_last_of(L"\\/"));
    statePath=root+L"\\..\\local-state";CreateDirectoryW(statePath.c_str(),nullptr);statePath+=L"\\announcement-counts.ini";
    app=reinterpret_cast<Getter>(api->Symbol(mod,"game.app"));
    profilePointer=reinterpret_cast<uint8_t**>(api->Symbol(mod,"flow.profile_pointer"));
    navigate=reinterpret_cast<Navigate>(api->Symbol(mod,"announcements.navigate"));
    visible=reinterpret_cast<Visible>(api->Symbol(mod,"announcements.web_visible"));
    loginEvent=reinterpret_cast<LoginEvent>(api->Symbol(mod,"announcements.login_event"));
    enqueue=reinterpret_cast<Enqueue>(api->Symbol(mod,"announcements.enqueue"));
    auto* browserCall=static_cast<uint8_t*>(api->Symbol(mod,"announcements.login_browser_call"));
    loginBrowserReturn=browserCall?browserCall+5:nullptr;
    if(!app || !profilePointer || !navigate || !visible || !loginEvent || !enqueue || !loginBrowserReturn)return 0;
    return api->HookCode(mod,"announcements.login_update",reinterpret_cast<void*>(&LoginUpdate),reinterpret_cast<void**>(&originalLogin)) &&
        api->HookCode(mod,"announcements.game_update",reinterpret_cast<void*>(&GameUpdate),reinterpret_cast<void**>(&originalGame)) &&
        api->HookCode(mod,"announcements.navigate",reinterpret_cast<void*>(&NavigateHook),reinterpret_cast<void**>(&originalNavigate)) &&
        api->HookCode(mod,"announcements.browser_initialize",reinterpret_cast<void*>(&BrowserInitializeHook),reinterpret_cast<void**>(&originalBrowserInitialize)) &&
        api->HookCode(mod,"announcements.browser_create",reinterpret_cast<void*>(&BrowserCreateHook),reinterpret_cast<void**>(&originalBrowserCreate)) &&
        api->HookCode(mod,"announcements.web_visible",reinterpret_cast<void*>(&VisibleHook),reinterpret_cast<void**>(&originalVisible));
}
