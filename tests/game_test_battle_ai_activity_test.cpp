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
    std::printf("Battle AI one-second activity clock: %d failures\n",failures);
    return failures?1:0;
}
