#pragma once
#include <cstdint>
#include <cmath>

// New local height extrema count as traversal. Repeating an in-place hop
// cannot refresh either bound. Rebase after substantial horizontal travel,
// not collision jitter at the same wall.
class BattleAiVerticalProgress {
    bool known_=false;
    float anchorX_=0,anchorZ_=0,lowestY_=0,highestY_=0;
public:
    bool Observe(float x,float y,float z,bool autonomous) {
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)){known_=false;return false;}
        const float dx=x-anchorX_,dz=z-anchorZ_;
        if(!known_||!autonomous||dx*dx+dz*dz>=30*30) {
            known_=true;anchorX_=x;anchorZ_=z;lowestY_=highestY_=y;return false;
        }
        if(y<lowestY_-2){lowestY_=y;return true;}
        if(y>highestY_+2){highestY_=y;return true;}
        return false;
    }
};

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
