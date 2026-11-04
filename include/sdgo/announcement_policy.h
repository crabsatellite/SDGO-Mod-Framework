#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sdgo::announcements {
enum Event : uint32_t { Enter=1, Timer=2, Login=4, Lobby=8, Room=16, BattleStart=32, BattleEnd=64 };
struct Clock { int date=0, previous_date=0, weekday=0, minute=0; uint64_t seconds=0; };
struct Rule {
    std::wstring id, title, page;
    std::vector<std::wstring> lines;
    bool enabled=false;
    int surfaces=0, triggers=0, days=127, start=0, end=0, date_start=0, date_end=0;
    uint32_t duration=10, interval=60, max_session=0, max_day=0, max_total=0, priority=0;
    // Dynamic feed constraints are internal; authored rules retain their ABI.
    uint64_t expires_at=0;
    bool dynamic=false,lobby_only=false;
    std::wstring group;
    uint32_t group_max_day=0,group_max_session=0,group_interval=0;
    std::vector<std::wstring> consumes;
};
struct Count { uint32_t session=0, daily=0, total=0; int date=0; uint64_t last=0; };
inline bool InWindow(const Rule& r, const Clock& c) {
    int date=c.date, weekday=c.weekday;
    if(r.start<r.end && (c.minute<r.start || c.minute>=r.end))return false;
    if(r.start>r.end) {
        if(c.minute>=r.end && c.minute<r.start)return false;
        if(c.minute<r.end){date=c.previous_date;weekday=(weekday+6)%7;}
    }
    return r.enabled && (!r.expires_at || c.seconds<r.expires_at) && (r.days & (1<<weekday)) && (!r.date_start || date>=r.date_start) &&
        (!r.date_end || date<=r.date_end);
}
class Scheduler {
public:
    std::map<std::wstring,Count> counts;
    // Rules are evaluated in stable priority order; equal priorities keep editor order.
    int Select(const std::vector<Rule>& rules, int surface, uint32_t events, const Clock& clock) const {
        int best=-1;
        for(size_t i=0;i<rules.size();++i) {
            const auto& r=rules[i];
            if(!(r.surfaces&surface) || !(r.triggers&events) || !InWindow(r,clock))continue;
            auto p=counts.find(r.id);const Count count=p==counts.end()?Count{}:p->second;
            const uint32_t daily=count.date==clock.date?count.daily:0;
            if((r.max_session && count.session>=r.max_session) || (r.max_day && daily>=r.max_day) ||
               (r.max_total && count.total>=r.max_total) ||
               (count.last && (clock.seconds<count.last || clock.seconds-count.last<r.interval)))continue;
            if(!r.group.empty()) {
                auto g=counts.find(r.group);const Count group=g==counts.end()?Count{}:g->second;
                if((r.group_max_day && group.date==clock.date && group.daily>=r.group_max_day) ||
                   (r.group_max_session && group.session>=r.group_max_session) ||
                   (group.last && (clock.seconds<group.last || clock.seconds-group.last<r.group_interval)))continue;
            }
            if(best<0 || rules[static_cast<size_t>(best)].priority<r.priority)best=static_cast<int>(i);
        }
        return best;
    }
    void Record(const std::wstring& id,const Clock& clock) {
        auto& c=counts[id];if(c.date!=clock.date){c.date=clock.date;c.daily=0;}
        ++c.session;++c.daily;++c.total;c.last=clock.seconds;
    }
    void Shown(const Rule& r,const Clock& clock) {
        Record(r.id,clock);if(!r.group.empty())Record(r.group,clock);
        for(const auto& id:r.consumes)if(id!=r.id)Record(id,clock);
    }
    bool Seen(const std::wstring& id) const {const auto p=counts.find(id);return p!=counts.end() && p->second.total>0;}
};
}
