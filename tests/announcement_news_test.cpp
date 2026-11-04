#include "../mods/announcements/news_runtime.h"
#include <cstdio>
#include <cstdlib>
using namespace sdgo::announcements;
namespace n=sdgo::announcements::news;
void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"%s\n",text);std::exit(1);}}
template<class F> void Reject(F fn,const char* text){try{fn();}catch(const std::exception&){return;}Check(false,text);}
int main(int argc,char** argv){
    if(argc==3 && std::string(argv[1])=="--live"){
        const auto base=n::Wide(argv[2]);Check(n::Endpoint(base),"valid live source");const auto current=n::Now();
        for(size_t i=0;i<n::kinds.size();++i){auto items=n::Normalize(i,json::Parser(n::Fetch(base+n::paths[i],GetTickCount64()+15000)).Parse(),current,48*3600);std::printf("native live %s: %u valid items\n",n::kinds[i].c_str(),static_cast<unsigned>(items.size()));}
        return 0;
    }
    const uint64_t now=n::Date("2026-11-04")+12*3600;
    const std::string raw=R"({"code":200,"data":{"date":"2026-11-04","news":["测试新闻一","科技 &amp; 游戏 <b>快报</b>","测试新闻一"],"link":"https://example.com/news"}})";
    auto daily=n::Normalize(0,json::Parser(raw).Parse(),now,48*3600);
    Check(daily.size()==2 && daily[1].title==L"科技 & 游戏 快报","normalization preserves Unicode and removes duplicate/markup");
    Check(daily[0].date=="2026-11-04","publication date is retained");
    Check(n::Identity("news",daily[0].title,daily[0].url)==daily[0].id,"article identity is stable");
    Reject([&]{n::Normalize(0,json::Parser(raw).Parse(),now+3*86400,48*3600);},"stale daily feed cannot be displayed");
    Reject([]{json::Parser(R"({"x":1,"x":2})").Parse();},"duplicate object members fail closed");
    Reject([]{json::Parser(R"({"x":"\ud800"})").Parse();},"unpaired escaped surrogates fail closed");
    Reject([]{json::Parser("[01]").Parse();},"invalid JSON numbers are rejected");
    Reject([]{json::Parser("[1,]").Parse();},"trailing array comma is rejected");
    Check(n::Wide(json::Parser(R"("\ud83d\ude80")").Parse().string).size()==2,"paired escaped Unicode is preserved");
    Check(n::Endpoint(L"https://example.com") && n::Endpoint(L"http://127.0.0.1:4444"),"HTTPS and explicit loopback fixtures are accepted");
    Check(!n::Endpoint(L"http://127.attacker.test") && !n::Endpoint(L"https://name:password@example.com") && !n::Endpoint(L"https://example.com/?key=value"),"insecure or credential-bearing sources are rejected");
    std::string epic="{\"code\":200,\"data\":[{\"title\":\"live\",\"link\":\"https://example.com/live\",\"original_price\":10,\"free_start_at\":"+std::to_string((now-6*86400)*1000)+",\"free_end_at\":"+std::to_string((now+86400)*1000)+"}]}";
    auto games=n::Normalize(2,json::Parser(epic).Parse(),now,48*3600);Check(games.size()==1,"week-long promotions remain eligible while active");
    n::Cache cache;cache.attempted=now;cache.feeds[0]={now,"https://example.com",daily};cache.feeds[2]={now,"https://example.com",games};
    const auto serialized=n::WriteCache(cache);auto reread=n::ReadCache(serialized);
    Check(reread.feeds[0].items[0].id==daily[0].id && reread.feeds[2].items[0].expires==now+86400,"cache roundtrip retains expiry and identity");
    n::Settings settings;settings.enabled=true;settings.sources={L"https://example.com"};Scheduler scheduler;
    Clock clock{20261104,20261103,2,720,now+n::kWindowsEpoch};
    auto rules=n::Rules(settings,cache,scheduler,now);
    Check(rules.size()>=4 && rules[0].surfaces==1,"digest and individual native cards are generated");
    scheduler.Shown(rules[0],clock);
    auto after=n::Rules(settings,cache,scheduler,now);
    Check(std::none_of(after.begin(),after.end(),[&](const Rule& r){return r.id==daily[0].id;}),"digest headlines cannot repeat in the native panel");
    auto game=std::find_if(after.begin(),after.end(),[](const Rule& r){return r.surfaces==2;});Check(game!=after.end(),"unseen promotion remains available");
    Rule card=*game;card.group_max_day=1;std::vector<Rule> only{card};
    Check(scheduler.Select(only,2,Timer,clock)==0,"eligible cached card can be selected");
    scheduler.Shown(card,clock);only[0].id=L"different_card";
    Check(scheduler.Select(only,2,Timer,clock)==-1,"category changes cannot bypass the aggregate daily limit");
    Clock expired=clock;expired.seconds+=3*86400;
    Check(n::Rules(settings,cache,Scheduler{},now+3*86400).empty(),"expired offline cache yields no announcements");
    Rule capped=card;capped.group.clear();capped.expires_at=clock.seconds;
    Check(!InWindow(capped,clock),"absolute expiry is exclusive");
    Check(n::Escape(L"<script>&\"")==L"&lt;script&gt;&amp;&quot;","login page text is escaped");
    std::puts("announcement news model: passed");return 0;
}
