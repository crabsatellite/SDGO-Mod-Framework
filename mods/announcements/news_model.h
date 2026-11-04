#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "news_json.h"

namespace sdgo::announcements::news {
constexpr uint64_t kWindowsEpoch=11644473600ULL;
inline const std::array<std::string,4> kinds={"news","tech","epic","history"};
inline const std::array<std::wstring,4> paths={L"/v2/60s",L"/v2/it-news",L"/v2/epic",L"/v2/today_in_history"};
inline std::wstring Wide(const std::string& text){
    if(text.empty())return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(n<=0)throw std::runtime_error("invalid feed UTF-8");std::wstring result(static_cast<size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n);return result;
}
inline std::string Utf8(const std::wstring& text){
    if(text.empty())return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)throw std::runtime_error("invalid feed Unicode");std::string result(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);return result;
}
inline uint64_t Now(){FILETIME ft{};GetSystemTimeAsFileTime(&ft);ULARGE_INTEGER value{};value.LowPart=ft.dwLowDateTime;value.HighPart=ft.dwHighDateTime;return value.QuadPart/10000000-kWindowsEpoch;}
inline std::wstring Clean(const std::string& input,size_t maximum=180){
    std::wstring text=Wide(input),out;bool tag=false,space=false;
    for(wchar_t c:text){if(c==L'<'){tag=true;space=true;continue;}if(tag){if(c==L'>')tag=false;continue;}
        if(c==L'\r'||c==L'\n'||c==L'\t'||c==L' '||c==0xa0){space=!out.empty();continue;}
        if(c<32)throw std::runtime_error("invalid news text");
        if(space && !out.empty())out+=L' ';space=false;out+=c;}
    for(const auto& pair:std::array<std::pair<std::wstring,std::wstring>,6>{{{L"&amp;",L"&"},{L"&quot;",L"\""},{L"&#39;",L"'"},{L"&lt;",L"<"},{L"&gt;",L">"},{L"&nbsp;",L" "}}}){
        size_t at=0;while((at=out.find(pair.first,at))!=std::wstring::npos){out.replace(at,pair.first.size(),pair.second);at+=pair.second.size();}}
    if(out.empty() || out.size()>maximum)throw std::runtime_error("news text exceeds limit");return out;
}
inline bool ArticleUrl(const std::string& url){
    if(url.size()>2048 || url.rfind("https://",0)!=0)return false;
    const auto end=url.find('/',8);const auto authority=url.substr(8,end==std::string::npos?std::string::npos:end-8);
    return !authority.empty() && authority.find('@')==std::string::npos &&
        std::all_of(url.begin(),url.end(),[](unsigned char c){return c>32 && c!=127;});
}
inline std::wstring Identity(const std::string& kind,const std::wstring& title,const std::string& url){
    uint64_t value=14695981039346656037ULL;
    for(unsigned char byte:kind+'\n'+Utf8(title)+'\n'+url)value=(value^byte)*1099511628211ULL;
    wchar_t hex[17]{};swprintf_s(hex,L"%016llx",static_cast<unsigned long long>(value));
    return L"dailynews_"+Wide(kind)+L"_"+hex;
}
inline uint64_t Date(const std::string& date){
    if(date.size()!=10 || date[4]!='-'||date[7]!='-')return 0;
    for(size_t i=0;i<date.size();++i)if(i!=4 && i!=7 && (date[i]<'0'||date[i]>'9'))return 0;
    SYSTEMTIME st{};
    int year=0,month=0,day=0;
    if(sscanf_s(date.c_str(),"%4d-%2d-%2d",&year,&month,&day)!=3 || year<1970 || year>2100 || month<1 || month>12 || day<1 || day>31)return 0;
    st.wYear=static_cast<WORD>(year);st.wMonth=static_cast<WORD>(month);st.wDay=static_cast<WORD>(day);
    FILETIME ft{};if(!SystemTimeToFileTime(&st,&ft))return 0;ULARGE_INTEGER value{};value.LowPart=ft.dwLowDateTime;value.HighPart=ft.dwHighDateTime;
    return value.QuadPart/10000000-kWindowsEpoch-8*3600;
}
inline std::string DateText(uint64_t seconds){
    ULARGE_INTEGER value{};value.QuadPart=(seconds+kWindowsEpoch+8*3600)*10000000;
    FILETIME ft{value.LowPart,value.HighPart};SYSTEMTIME st{};FileTimeToSystemTime(&ft,&st);
    char out[16]{};sprintf_s(out,"%04u-%02u-%02u",st.wYear,st.wMonth,st.wDay);return out;
}
inline std::wstring DateTimeText(uint64_t seconds){
    ULARGE_INTEGER value{};value.QuadPart=(seconds+kWindowsEpoch+8*3600)*10000000;
    FILETIME ft{value.LowPart,value.HighPart};SYSTEMTIME st{};FileTimeToSystemTime(&ft,&st);
    wchar_t out[32]{};swprintf_s(out,L"%04u-%02u-%02u %02u:%02u",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute);return out;
}
struct Item {
    std::wstring id,title,source;
    std::string kind,url,date;
    uint64_t published=0,expires=0,offerEnd=0;
};
struct Feed { uint64_t fetched=0;std::string source;std::vector<Item> items; };
struct Cache { uint64_t attempted=0;std::array<Feed,4> feeds;std::vector<std::string> errors; };
inline void Add(std::vector<Item>& items,const std::string& kind,const std::string& title,const std::string& url,
                const std::wstring& source,uint64_t posted,uint64_t expires,const std::string& date,uint64_t now,uint64_t maxAge,uint64_t offerEnd=0){
    if(!posted || posted>now+300 || expires<=now || !ArticleUrl(url))return;
    expires=std::min(expires,posted+maxAge);if(expires<=now)return;
    Item item;item.kind=kind;item.title=Clean(title);item.url=url;item.source=source;item.published=posted;item.expires=expires;item.date=date;item.offerEnd=offerEnd;
    item.id=Identity(kind,item.title,url);
    if(std::none_of(items.begin(),items.end(),[&](const Item& old){return old.id==item.id;}))items.push_back(std::move(item));
}
inline std::vector<Item> Normalize(size_t index,const json::Value& payload,uint64_t now,uint64_t maxAge){
    if(payload["code"].Integer()!=200 || index>=kinds.size())throw std::runtime_error("feed response rejected");
    const auto& data=payload["data"];std::vector<Item> items;const auto& kind=kinds[index];
    if(index==0){
        if(data.kind!=json::Value::Object || data["news"].kind!=json::Value::Array)throw std::runtime_error("invalid daily feed");
        const auto date=data["date"].string;const auto posted=Date(date);if(!posted)throw std::runtime_error("invalid daily date");
        for(const auto& text:data["news"].array){if(items.size()>=20)break;try{Add(items,kind,text.string,data["link"].string,L"60秒读懂世界",posted,posted+maxAge,date,now,maxAge);}catch(const std::exception&) {}}
    } else if(index==1){
        if(data.kind!=json::Value::Array)throw std::runtime_error("invalid technology feed");
        for(const auto& row:data.array){if(items.size()>=20)break;try{
            const auto raw=row["created_at"].Integer();if(raw<=0)continue;const auto posted=static_cast<uint64_t>(raw)/1000;
            Add(items,kind,row["title"].string,row["link"].string,L"IT之家",posted,posted+maxAge,DateText(posted),now,maxAge);
        }catch(const std::exception&) {}}
    } else if(index==2){
        if(data.kind!=json::Value::Array)throw std::runtime_error("invalid game promotion feed");
        for(const auto& row:data.array){if(items.size()>=20)break;try{
            const auto begin=row["free_start_at"].Integer(),end=row["free_end_at"].Integer();
            if(begin<=0 || end<=0 || row["original_price"].kind!=json::Value::Number || row["original_price"].number<=0)continue;
            const auto start=static_cast<uint64_t>(begin)/1000,finish=static_cast<uint64_t>(end)/1000;
            if(start>now || finish<=now)continue;
            Add(items,kind,row["title"].string,row["link"].string,L"Epic Games",now,finish,DateText(start),now,maxAge,finish);
        }catch(const std::exception&) {}}
    } else {
        if(data.kind!=json::Value::Object || data["items"].kind!=json::Value::Array)throw std::runtime_error("invalid history feed");
        const auto date=DateText(now);int year=0,month=0,day=0;sscanf_s(date.c_str(),"%d-%d-%d",&year,&month,&day);
        if(data["month"].Integer()!=month || data["day"].Integer()!=day)throw std::runtime_error("history date mismatch");
        auto rows=data["items"].array;
        auto interesting=[](const json::Value& row){const auto title=Wide(row["title"].string);for(const auto* word:{L"科学",L"发现",L"发明",L"卫星",L"计算机",L"通讯",L"游戏",L"电影"})if(title.find(word)!=std::wstring::npos)return true;return false;};
        std::stable_sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b){return interesting(a)>interesting(b);});
        const auto posted=Date(date);
        for(const auto& row:rows){if(items.size()>=20)break;try{
            Add(items,kind,row["year"].string+"年："+row["title"].string,row["link"].string,L"历史上的今天",posted,posted+86400,date,now,maxAge);
        }catch(const std::exception&) {}}
    }
    if(index!=2 && items.empty())throw std::runtime_error("feed has no recent items");return items;
}
inline Cache ReadCache(const std::string& raw){
    const auto root=json::Parser(raw).Parse();if(root["schema_version"].Integer()!=1)throw std::runtime_error("invalid cache version");Cache cache;
    cache.attempted=static_cast<uint64_t>(std::max<int64_t>(0,root["attempted_at"].Integer()));
    for(size_t i=0;i<kinds.size();++i){auto& feed=cache.feeds[i];const auto& source=root["feeds"][kinds[i]];
        feed.fetched=static_cast<uint64_t>(std::max<int64_t>(0,source["fetched_at"].Integer()));feed.source=source["source"].string;
        for(const auto& row:source["items"].array){if(feed.items.size()>=20)break;try{
            Item item;item.kind=kinds[i];item.title=Clean(row["title"].string);item.source=Clean(row["source"].string,60);
            item.url=row["url"].string;item.date=row["date"].string;if(!ArticleUrl(item.url)||!Date(item.date))continue;
            item.id=Identity(item.kind,item.title,item.url);
            item.published=static_cast<uint64_t>(std::max<int64_t>(0,row["published_at"].Integer()));
            item.expires=static_cast<uint64_t>(std::max<int64_t>(0,row["expires_at"].Integer()));
            item.offerEnd=static_cast<uint64_t>(std::max<int64_t>(0,row["offer_end_at"].Integer()));feed.items.push_back(std::move(item));
        }catch(const std::exception&) {}}
    }
    for(const auto& error:root["errors"].array)if(std::find(kinds.begin(),kinds.end(),error.string)!=kinds.end())cache.errors.push_back(error.string);
    return cache;
}
inline std::string WriteCache(const Cache& cache){
    std::string out="{\"schema_version\":1,\"attempted_at\":"+std::to_string(cache.attempted)+",\"feeds\":{";
    for(size_t i=0;i<kinds.size();++i){if(i)out+=',';const auto& feed=cache.feeds[i];
        out+=json::Quote(kinds[i])+":{\"fetched_at\":"+std::to_string(feed.fetched)+",\"source\":"+json::Quote(feed.source)+",\"items\":[";
        for(size_t j=0;j<feed.items.size();++j){if(j)out+=',';const auto& item=feed.items[j];
            out+="{\"id\":"+json::Quote(Utf8(item.id))+",\"kind\":"+json::Quote(item.kind)+",\"title\":"+json::Quote(Utf8(item.title))+
                ",\"url\":"+json::Quote(item.url)+",\"source\":"+json::Quote(Utf8(item.source))+",\"date\":"+json::Quote(item.date)+
                ",\"published_at\":"+std::to_string(item.published)+",\"expires_at\":"+std::to_string(item.expires)+",\"offer_end_at\":"+std::to_string(item.offerEnd)+"}";
        }out+="]}";
    }out+="},\"errors\":[";for(size_t i=0;i<cache.errors.size();++i){if(i)out+=',';out+=json::Quote(cache.errors[i]);}return out+"]}";
}
inline std::vector<std::wstring> Lines(const std::wstring& text){
    std::vector<std::wstring> lines;std::wstring line;size_t cells=0;
    for(size_t i=0;i<text.size();++i){wchar_t c=text[i];const bool pair=c>=0xd800&&c<=0xdbff&&i+1<text.size()&&text[i+1]>=0xdc00&&text[i+1]<=0xdfff;
        const size_t width=c>=0x1100?2u:1u;if(c==L'\n'||cells+width>40){lines.push_back(line.empty()?L" ":line);line.clear();cells=0;if(c==L'\n')continue;}line+=c;if(pair)line+=text[++i];cells+=width;}
    if(!line.empty())lines.push_back(line);return lines;
}
inline std::wstring Escape(const std::wstring& text){std::wstring out;for(wchar_t c:text){switch(c){case L'&':out+=L"&amp;";break;case L'<':out+=L"&lt;";break;case L'>':out+=L"&gt;";break;case L'\"':out+=L"&quot;";break;default:out+=c;}}return out;}
}
