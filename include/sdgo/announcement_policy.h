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
};
struct Count { uint32_t session=0, daily=0, total=0; int date=0; uint64_t last=0; };
inline bool InWindow(const Rule& r, const Clock& c) {
    int date=c.date, weekday=c.weekday;
    if(r.start<r.end && (c.minute<r.start || c.minute>=r.end))return false;
    if(r.start>r.end) {
        if(c.minute>=r.end && c.minute<r.start)return false;
        if(c.minute<r.end){date=c.previous_date;weekday=(weekday+6)%7;}
    }
    return r.enabled && (r.days & (1<<weekday)) && (!r.date_start || date>=r.date_start) &&
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
            if(best<0 || rules[static_cast<size_t>(best)].priority<r.priority)best=static_cast<int>(i);
        }
        return best;
    }
    void Shown(const Rule& r,const Clock& clock) {
        auto& c=counts[r.id];if(c.date!=clock.date){c.date=clock.date;c.daily=0;}
        ++c.session;++c.daily;++c.total;c.last=clock.seconds;
    }
};
}
