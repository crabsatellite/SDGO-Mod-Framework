#include "sdgo/announcement_policy.h"
#include <cstdio>
#include <cstdlib>
using namespace sdgo::announcements;
void Check(bool condition,const char* message){if(!condition){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
int main(){
 Rule r;r.id=L"event";r.enabled=true;r.surfaces=3;r.triggers=Enter|Timer;r.start=22*60;r.end=2*60;r.days=1;
 Clock monday{20261102,20261101,0,23*60,1000};
 Clock tuesday{20261103,20261102,1,60,1100};
 Check(InWindow(r,monday)&&InWindow(r,tuesday),"overnight window anchors to its start weekday");
 tuesday.minute=120;Check(!InWindow(r,tuesday),"window end is exclusive");
 r.date_start=r.date_end=20261102;tuesday.minute=60;Check(InWindow(r,tuesday),"overnight end uses previous date");
 r.start=r.end=0;r.days=127;r.date_start=r.date_end=0;
 Scheduler s;std::vector<Rule> rules{r};rules[0].max_session=2;rules[0].max_day=1;
 Check(s.Select(rules,1,Enter,monday)==0,"enter can display an eligible rule");
 s.Shown(rules[0],monday);Check(s.Select(rules,2,Enter,monday)==-1,"both surfaces share daily and cooldown limits");
 Check(s.Select(rules,2,Timer,tuesday)==0,"daily allowance resets with the configured calendar date");
 s.Shown(rules[0],tuesday);tuesday.date=20261104;tuesday.seconds+=86400;
 Check(s.Select(rules,1,Timer,tuesday)==-1,"session cap survives date rollover");
 rules[0].max_session=0;rules[0].max_total=2;
 Check(s.Select(rules,1,Timer,tuesday)==-1,"lifetime cap survives date rollover");
 rules[0].max_total=0;rules[0].max_day=0;rules[0].priority=1;
 Rule high=rules[0];high.id=L"higher";high.priority=9;rules.push_back(high);
 Check(s.Select(rules,1,Timer,tuesday)==1,"eligible higher priority rule wins");
 rules[1].triggers=BattleStart;Check(s.Select(rules,1,Enter,tuesday)==0,"unmatched events cannot display");
 rules[0].enabled=false;Check(s.Select(rules,1,Enter,tuesday)==-1,"disabled rules cannot display");
 std::puts("announcement policy: passed");
}
