#pragma once
#include <cstdint>

// Passive wall-clock accounting. A requested route or held input is not
// activity: callers supply witnessed displacement or a native attack action.
class BattleAiActivityClock {
    uint64_t previousAt_=0, idleSince_=0;
    bool observed_=false, previousEligible_=false, idle_=false;
public:
    uint64_t maxIdleMs=0, maxSampleGapMs=0;
    uint64_t Observe(uint64_t now,bool eligible,bool active) {
        if(observed_&&eligible&&previousEligible_&&now>=previousAt_) {
            const auto gap=now-previousAt_;
            if(gap>maxSampleGapMs)maxSampleGapMs=gap;
        }
        uint64_t elapsed=0;
        if(eligible) {
            if(!active&&!idle_) {
                idleSince_=observed_&&previousEligible_?previousAt_:now;
                idle_=true;
            }
            if(idle_&&now>=idleSince_) {
                elapsed=now-idleSince_;
                if(elapsed>maxIdleMs)maxIdleMs=elapsed;
            }
            if(active)idle_=false;
        } else idle_=false;
        observed_=true;previousEligible_=eligible;previousAt_=now;
        return elapsed;
    }
};
