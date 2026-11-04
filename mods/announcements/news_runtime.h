#pragma once
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <memory>
#include "news_model.h"
#include "sdgo/announcement_policy.h"

namespace sdgo::announcements::news {
struct Settings {
    bool enabled=false,login=true,game=false,lobbyOnly=true;
    uint32_t refresh=360,age=48,newsCount=3,techCount=2,loginDuration=25;
    uint32_t gameDay=5,gameSession=3,gameInterval=600,gameDuration=10;
    int days=127,start=0,end=0;
    uint32_t gameTriggers=0;
    std::array<bool,4> categories{{true,true,true,true}};
    std::vector<std::wstring> sources;
};
inline std::wstring Ini(const std::wstring& file,const wchar_t* name,const wchar_t* fallback=L""){
    wchar_t text[2048]{};GetPrivateProfileStringW(L"news",name,fallback,text,2048,file.c_str());return text;
}
inline uint32_t Setting(const std::wstring& file,const wchar_t* key,uint32_t fallback,uint32_t low,uint32_t high){
    const auto text=Ini(file,key);if(text.empty())return fallback;wchar_t* end=nullptr;const auto n=wcstoul(text.c_str(),&end,10);
    if(!end||*end||n<low||n>high)throw std::runtime_error("invalid news configuration");return static_cast<uint32_t>(n);
}
inline std::vector<std::wstring> Split(const std::wstring& text){std::vector<std::wstring> out;size_t begin=0;for(;;){const auto end=text.find(',',begin);out.push_back(text.substr(begin,end==std::wstring::npos?std::wstring::npos:end-begin));if(end==std::wstring::npos)break;begin=end+1;}return out;}
inline bool Endpoint(const std::wstring& value){
    if(value.empty()||value.size()>400||value.find_first_of(L"\r\n\t ?#@")!=std::wstring::npos)return false;
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=static_cast<DWORD>(-1);parts.dwUrlPathLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(value.c_str(),0,0,&parts)||!parts.dwHostNameLength)return false;
    const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    bool loopback=host==L"::1"||host==L"[::1]";
    unsigned a=0,b=0,c=0,d=0;wchar_t trailing=0;
    if(swscanf_s(host.c_str(),L"%u.%u.%u.%u%c",&a,&b,&c,&d,&trailing,1)==4 && a==127 && b<=255 && c<=255 && d<=255)loopback=true;
    if(parts.nScheme!=INTERNET_SCHEME_HTTPS && !(parts.nScheme==INTERNET_SCHEME_HTTP &&
       loopback))return false;
    const auto segments=std::wstring(parts.lpszUrlPath?parts.lpszUrlPath:L"",parts.dwUrlPathLength);
    return segments.find(L"/../")==std::wstring::npos && segments.find(L"/./")==std::wstring::npos &&
        (segments.size()<3 || segments.substr(segments.size()-3)!=L"/..");
}
inline Settings ReadSettings(const std::wstring& path){
    Settings s;if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return s;
    if(Setting(path,L"schema_version",0,1,1)!=1)return s;
    s.enabled=Setting(path,L"enabled",1,0,1)!=0;s.login=Setting(path,L"login_enabled",1,0,1)!=0;s.game=Setting(path,L"game_enabled",0,0,1)!=0;
    s.refresh=Setting(path,L"refresh_minutes",360,30,1440);s.age=Setting(path,L"max_age_hours",48,1,72);
    s.newsCount=Setting(path,L"login_news_count",3,0,5);s.techCount=Setting(path,L"login_tech_count",2,0,5);
    s.loginDuration=Setting(path,L"login_duration_seconds",25,5,120);
    s.gameDay=Setting(path,L"game_max_day",5,1,20);s.gameSession=Setting(path,L"game_max_session",3,1,10);
    s.gameInterval=Setting(path,L"game_interval_seconds",600,30,86400);s.gameDuration=Setting(path,L"game_duration_seconds",10,3,60);
    s.lobbyOnly=Setting(path,L"game_lobby_only",1,0,1)!=0;
    auto minute=[](const std::wstring& text){
        if(text.size()!=5||text[2]!=L':'||text[0]<L'0'||text[0]>L'2'||text[1]<L'0'||text[1]>L'9'||text[3]<L'0'||text[3]>L'5'||text[4]<L'0'||text[4]>L'9')throw std::runtime_error("invalid news window");
        const int hour=(text[0]-L'0')*10+text[1]-L'0';if(hour>23)throw std::runtime_error("invalid news window");return hour*60+(text[3]-L'0')*10+text[4]-L'0';};
    s.start=minute(Ini(path,L"start_time",L"00:00"));s.end=minute(Ini(path,L"end_time",L"00:00"));s.days=0;
    for(const auto& day:Split(Ini(path,L"weekdays",L"0,1,2,3,4,5,6"))){if(day.size()!=1 || day[0]<L'0'||day[0]>L'6')throw std::runtime_error("invalid news weekdays");s.days|=1<<(day[0]-L'0');}
    s.gameTriggers=0;
    const std::array<std::wstring,7> eventNames={L"enter",L"timer",L"login",L"lobby",L"room",L"battle_start",L"battle_end"};
    const auto events=Ini(path,L"game_triggers");
    if(!events.empty())for(const auto& event:Split(events)){const auto found=std::find(eventNames.begin(),eventNames.end(),event);if(found==eventNames.end())throw std::runtime_error("invalid news trigger");s.gameTriggers|=1u<<static_cast<unsigned>(found-eventNames.begin());}
    if(s.game && !s.gameTriggers)throw std::runtime_error("select an in-game news trigger");
    s.sources=Split(Ini(path,L"sources",L"https://60s.crystelf.top,https://60s.okbaike.com"));
    if(s.sources.empty()||s.sources.size()>3)throw std::runtime_error("invalid news sources");
    for(const auto& source:s.sources)if(!Endpoint(source))throw std::runtime_error("invalid news source address");
    s.categories.fill(false);for(const auto& name:Split(Ini(path,L"categories",L"news,tech,epic,history"))){
        auto i=std::find(kinds.begin(),kinds.end(),Utf8(name));if(i==kinds.end())throw std::runtime_error("invalid news category");s.categories[static_cast<size_t>(i-kinds.begin())]=true;}
    return s;
}
inline std::string ReadFile(const std::wstring& path){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("news cache unavailable");
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size)||size.QuadPart<=0||size.QuadPart>524288){CloseHandle(file);throw std::runtime_error("invalid news cache size");}
    std::string out(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;const bool ok=::ReadFile(file,out.data(),static_cast<DWORD>(out.size()),&read,nullptr)!=FALSE;CloseHandle(file);
    if(!ok||read!=out.size())throw std::runtime_error("news cache read failed");return out;
}
inline bool WriteFile(const std::wstring& path,const std::string& raw){
    const auto temp=path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const bool ok=::WriteFile(file,raw.data(),static_cast<DWORD>(raw.size()),&written,nullptr)&&written==raw.size()&&FlushFileBuffers(file);CloseHandle(file);
    if(!ok||!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());return false;}return true;
}
inline uint64_t Stamp(const std::wstring& path){WIN32_FILE_ATTRIBUTE_DATA data{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return 0;ULARGE_INTEGER value{};value.LowPart=data.ftLastWriteTime.dwLowDateTime;value.HighPart=data.ftLastWriteTime.dwHighDateTime;return value.QuadPart;}
struct Internet {
    HINTERNET handle=nullptr;explicit Internet(HINTERNET value):handle(value){}
    ~Internet(){if(handle)WinHttpCloseHandle(handle);}Internet(const Internet&)=delete;Internet& operator=(const Internet&)=delete;
};
inline std::string Fetch(const std::wstring& address,uint64_t deadline){
    URL_COMPONENTS url{};url.dwStructSize=sizeof(url);url.dwHostNameLength=url.dwUrlPathLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(address.c_str(),0,0,&url))throw std::runtime_error("invalid news endpoint");
    const std::wstring host(url.lpszHostName,url.dwHostNameLength),path(url.lpszUrlPath,url.dwUrlPathLength);
    Internet session(WinHttpOpen(L"SDGO-Announcements/0.2 (+https://github.com/crabsatellite/SDGO-Mod-Framework)",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.handle)throw std::runtime_error("news network unavailable");WinHttpSetTimeouts(session.handle,2000,2000,3000,5000);
    Internet connection(WinHttpConnect(session.handle,host.c_str(),url.nPort,0));if(!connection.handle)throw std::runtime_error("news connection failed");
    Internet request(WinHttpOpenRequest(connection.handle,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,url.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0));
    if(!request.handle)throw std::runtime_error("news request failed");
    DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
    if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)))throw std::runtime_error("news request policy unavailable");
    if(!WinHttpSendRequest(request.handle,L"Accept: application/json\r\n",static_cast<DWORD>(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.handle,nullptr))throw std::runtime_error("news request timeout");
    DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status!=200)throw std::runtime_error("news provider unavailable");
    std::string out;char buffer[8192];DWORD read=0;
    for(;;){if(GetTickCount64()>deadline)throw std::runtime_error("news refresh deadline reached");
        if(!WinHttpReadData(request.handle,buffer,sizeof(buffer),&read))throw std::runtime_error("news response interrupted");if(!read)break;
        if(out.size()+read>524288)throw std::runtime_error("news response exceeds limit");out.append(buffer,read);}
    return out;
}
class Engine {
    std::wstring directory,cacheDirectory,cacheFile,configFile;
    uint64_t checked=0,settingsStamp=0,cacheStamp=0,nextAttempt=0;
    Settings settings;Cache cache;volatile LONG busy=0;
    struct Work { Engine* owner;Settings settings;std::wstring folder,file; };
    static DWORD WINAPI Refresh(void* argument){
        std::unique_ptr<Work> work(static_cast<Work*>(argument));auto* owner=work->owner;
        HANDLE lease=INVALID_HANDLE_VALUE;
        try {
            std::filesystem::create_directories(work->folder);
            lease=CreateFileW((work->folder+L"\\.refresh.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(lease==INVALID_HANDLE_VALUE)throw std::runtime_error("news refresh already owned");
            Cache result;try{result=ReadCache(ReadFile(work->file));}catch(const std::exception&){}
            const auto now=Now();const uint64_t interval=result.errors.empty()?work->settings.refresh*60ULL:900ULL;
            if(result.attempted<=now && now-result.attempted<interval){CloseHandle(lease);InterlockedExchange(&owner->busy,0);return 0;}
            result.errors.clear();const auto deadline=GetTickCount64()+45000;
            for(size_t i=0;i<kinds.size();++i){if(!work->settings.categories[i])continue;bool success=false;
                for(const auto& base:work->settings.sources){if(GetTickCount64()>deadline)break;
                    try{auto items=Normalize(i,json::Parser(Fetch(base+paths[i],deadline)).Parse(),now,work->settings.age*3600ULL);
                        result.feeds[i]={now,Utf8(base),std::move(items)};success=true;break;
                    }catch(const std::exception&) {}}
                if(!success)result.errors.push_back(kinds[i]);}
            result.attempted=now;WriteFile(work->file,WriteCache(result));
        }catch(const std::exception&){}
        if(lease!=INVALID_HANDLE_VALUE)CloseHandle(lease);InterlockedExchange(&owner->busy,0);return 0;
    }
public:
    void Initialize(const std::wstring& modDirectory){
        directory=modDirectory;configFile=directory+L"\\news.ini";
        cacheDirectory=(std::filesystem::path(directory).parent_path().parent_path()/L"local-state"/L"announcements-news").wstring();
        cacheFile=cacheDirectory+L"\\cache.json";
    }
    const Settings& Config() const{return settings;}
    const Cache& Snapshot() const{return cache;}
    std::wstring PagePath(const std::wstring& page) const{return cacheDirectory+L"\\"+page;}
    bool Poll(){
        const auto tick=GetTickCount64();if(tick-checked<1000)return false;checked=tick;bool changed=false;
        const auto configStamp=Stamp(configFile);
        if(configStamp!=settingsStamp){settingsStamp=configStamp;try{settings=ReadSettings(configFile);}catch(const std::exception&){settings=Settings{};}changed=true;nextAttempt=0;}
        const auto fileStamp=Stamp(cacheFile);
        if(fileStamp!=cacheStamp){cacheStamp=fileStamp;try{cache=ReadCache(ReadFile(cacheFile));}catch(const std::exception&){cache=Cache{};}changed=true;}
        const auto now=Now();const auto interval=cache.errors.empty()?settings.refresh*60ULL:900ULL;
        if(settings.enabled && now>=nextAttempt && (cache.attempted>now || now-cache.attempted>=interval) &&
           InterlockedCompareExchange(&busy,1,0)==0){
            nextAttempt=now+900;auto* work=new Work{this,settings,cacheDirectory,cacheFile};HANDLE thread=CreateThread(nullptr,0,Refresh,work,0,nullptr);
            if(thread)CloseHandle(thread);else {delete work;InterlockedExchange(&busy,0);}
        }
        return changed;
    }
    bool WritePage(const Rule& rule) const {
        std::wstring html=L"<!doctype html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">"
            L"<style>body{margin:0;padding:12px;background:#101b29;color:#edf3fa;font:14px/1.55 'Microsoft YaHei',sans-serif;word-wrap:break-word}h1{font-size:19px;color:#69dce8;margin:0 0 10px}p{margin:0 0 7px}</style></head><body><h1>"+Escape(rule.title)+L"</h1>";
        for(const auto& line:rule.lines)html+=L"<p>"+Escape(line)+L"</p>";html+=L"</body></html>";
        try{std::filesystem::create_directories(cacheDirectory);return WriteFile(PagePath(rule.page),Utf8(html));}catch(const std::exception&){return false;}
    }
};
inline std::vector<Rule> Rules(const Settings& settings,const Cache& cache,const Scheduler& scheduler,uint64_t now){
    std::vector<Rule> rules;if(!settings.enabled)return rules;
    std::array<std::vector<const Item*>,4> available;
    for(size_t i=0;i<kinds.size();++i){if(!settings.categories[i])continue;
        for(const auto& item:cache.feeds[i].items)
            if(item.published<=now && item.expires>now && now-item.published<settings.age*3600ULL && !scheduler.Seen(item.id))available[i].push_back(&item);
    }
    std::vector<const Item*> digest;
    for(size_t i=0;i<std::min<size_t>(settings.newsCount,available[0].size());++i)digest.push_back(available[0][i]);
    for(size_t i=0;i<std::min<size_t>(settings.techCount,available[1].size());++i)digest.push_back(available[1][i]);
    // Login digest and later native headlines share article identities. A
    // headline that was already visible in the digest is not repeated in-game.
    if(settings.login && !digest.empty()) {
        Rule rule;rule.enabled=rule.dynamic=true;rule.surfaces=1;rule.triggers=Enter|Timer;
        rule.start=settings.start;rule.end=settings.end;rule.days=settings.days;
        rule.duration=settings.loginDuration;rule.interval=60;rule.max_total=1;rule.priority=0;
        rule.group=L"@news.login";rule.group_max_day=1;rule.group_max_session=1;
        rule.title=L"每日资讯简报";std::wstring key;
        for(const auto* item:digest){rule.lines.push_back(L"["+Wide(item->date)+L"] "+item->title+L"（"+item->source+L"）");
            rule.consumes.push_back(item->id);key+=item->id;rule.expires_at=rule.expires_at?std::min(rule.expires_at,item->expires+kWindowsEpoch):item->expires+kWindowsEpoch;}
        rule.id=Identity("brief",key,"");rule.page=rule.id+L".html";rules.push_back(std::move(rule));
    }
    if(settings.game) {
        // Interleave categories instead of allowing a long daily list to starve
        // the occasional history/game update under the installation's daily cap.
        std::array<size_t,4> position{};
        for(size_t round=0;round<20;++round)for(size_t index:{size_t{2},size_t{0},size_t{1},size_t{3}}){
            if(position[index]>=available[index].size() || (index==3 && position[index]>=1))continue;
            const auto& item=*available[index][position[index]++];Rule rule;rule.id=item.id;
            rule.dynamic=rule.enabled=true;rule.lobby_only=settings.lobbyOnly;rule.surfaces=2;rule.triggers=settings.gameTriggers;
            rule.start=settings.start;rule.end=settings.end;rule.days=settings.days;
            rule.duration=settings.gameDuration;rule.interval=30;rule.max_total=1;rule.priority=0;
            rule.expires_at=item.expires+kWindowsEpoch;rule.group=L"@news.game";
            rule.group_max_day=settings.gameDay;rule.group_max_session=settings.gameSession;rule.group_interval=settings.gameInterval;
            const std::array<std::wstring,4> labels={L"每日新闻",L"科技资讯",L"游戏限免",L"历史上的今天"};
            rule.title=labels[index]+L" · "+Wide(item.date);
            auto body=item.title+L"\n来源："+item.source;
            if(index==2)body+=L"\n领取截止："+DateTimeText(item.offerEnd?item.offerEnd:item.expires)+L"（UTC+8）";
            rule.lines=Lines(body);rules.push_back(std::move(rule));
        }
    }
    return rules;
}
}
