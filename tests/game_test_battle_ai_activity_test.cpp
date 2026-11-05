#include "../mods/game_test_client/battle_ai_activity_clock.h"
#include <cstdio>

int main() {
    int failures=0;
    const auto check=[&](bool ok,const char* message){if(!ok){std::printf("FAIL: %s\n",message);++failures;}};
    BattleAiActivityClock idle;
    idle.Observe(0,true,true);
    check(idle.Observe(10,true,false)==10,"idle starts at the last observed activity, including clock zero");
    check(idle.Observe(999,true,false)==999,"sub-second inactivity is retained");
    check(idle.Observe(1000,true,false)==1000,"exactly one second is measured without rounding");
    check(idle.Observe(1001,true,true)==1001&&idle.maxIdleMs==1001,"late activity cannot erase an over-one-second idle interval");
    idle.Observe(1010,true,true);
    check(idle.Observe(1020,true,false)==10,"a new idle interval starts after actual activity");
    check(idle.maxIdleMs==1001,"maximum inactivity remains latched after recovery");
    BattleAiActivityClock absent;
    absent.Observe(0,false,false);absent.Observe(60000,false,false);
    check(absent.maxIdleMs==0,"no live opponents permits genuine standby");
    absent.Observe(60010,true,false);
    check(absent.Observe(61011,true,false)==1001,"an opponent appearing starts the strict timer");
    BattleAiActivityClock locked;
    locked.Observe(0,false,false);locked.Observe(10000,false,false);
    locked.Observe(10001,true,false);locked.Observe(10500,true,true);
    check(locked.maxIdleMs==499,"native control release starts a fresh controllable interval");
    BattleAiActivityClock gap;
    gap.Observe(1,true,true);gap.Observe(1502,true,true);
    check(gap.maxSampleGapMs==1501,"a sampling gap is retained even if the next sample is active");
    check(gap.maxIdleMs==0,"missing samples are not invented idle behavior");
    BattleAiVerticalProgress descent;
    check(!descent.Observe(0,300,0,true),"the first position only establishes a descent baseline");
    check(descent.Observe(0,290,0,true)&&descent.Observe(0,250,0,true),"new lower native positions are real vertical traversal without horizontal displacement");
    check(descent.Observe(0,320,0,true),"a new higher native position is real vertical traversal");
    BattleAiVerticalProgress hop;
    BattleAiActivityClock hopping;
    for(uint64_t t=0;t<=2000;t+=100) {
        const float height=float(t%1000<=500?t%1000:1000-t%1000);
        const bool progress=hop.Observe(0,height,0,true);
        if(t>=1000)check(!progress,"repeating the same in-place hop never refreshes either height bound");
        hopping.Observe(t,true,progress);
    }
    check(hopping.maxIdleMs==1500,"in-place vertical hopping still fails the unchanged one-second idle threshold after its first attempt");
    check(!hop.Observe(0,-1,0,true),"small ground-height jitter is not traversal progress");
    check(!descent.Observe(0,200,0,false),"forced, special, hurt or unconfirmed movement cannot supply autonomous descent evidence");
    check(!descent.Observe(50,500,0,true)&&descent.Observe(50,400,0,true),"substantial horizontal travel establishes a new local floor for another real drop");
    std::printf("Battle AI one-second activity clock: %d failures\n",failures);
    return failures?1:0;
}
