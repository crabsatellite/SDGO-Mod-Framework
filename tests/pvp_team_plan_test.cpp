#include "sdgo/pvp_team_plan.h"
#include <cstdio>
using namespace sdgo::pvp_teams;
int failures=0;
void Check(bool value,const char* label) {if(!value){++failures;std::printf("FAIL %s\n",label);}}
int main() {
    std::array<uint32_t,kStorageSlots> slots{};
    slots[0]=31;slots[6]=0x80000001u;
    Check(SelectHumanHost(0x80000001u,31,slots)==31,
          "an AI MVP cannot replace the sole human room host");
    slots[0]=0;slots[11]=31;slots[1]=32;
    Check(SelectHumanHost(0x80000001u,31,slots)==31,
          "AI winner retains the previous human regardless of storage slot");
    Check(SelectHumanHost(32,31,slots)==32,
          "ordinary native transfer to an existing human keeps its original behavior");
    slots[11]=0;
    Check(SelectHumanHost(0x80000001u,31,slots)==32,
          "departed previous host falls back to a real remaining human");
    Check(SelectHumanHost(999,0x80000001u,slots)==32,
          "absent candidates and AI identities are not eligible for ownership");
    slots[1]=0;
    Check(SelectHumanHost(0x80000001u,31,slots)==0,
          "no humans means no invented or AI replacement host");
    for(int position=0;position<kStorageSlots;++position) {
        slots.fill(0x80000001u);slots[position]=31;
        Check(SelectHumanHost(0x80000001u,0,slots)==31,
              "every native room storage position can retain the real human host");
    }
    Room room;room.hostTeam=1;room.humans[1]=1;
    auto plan=Resolve({0,kFillRoom},room);
    Check(plan.error==Error::None && plan.allies==0 && plan.enemies==7,"one host versus seven AI within eight total slots");
    room.humans[1]=3;plan=Resolve({0,kFillRoom},room);
    Check(plan.error==Error::None && plan.enemies==5,"three manually allied humans versus five AI");
    room.humans[1]=4;plan=Resolve({0,kFillRoom},room);
    Check(plan.enemies==4 && !plan.allies,"four manually allied humans versus four AI");
    room.hostTeam=2;room.humans={0,0,3};plan=Resolve({0,5},room);
    Check(plan.error==Error::None && plan.allyTeam==2 && plan.enemyTeam==1 && plan.enemies==5,"host can choose either native side");
    room.humans={0,1,2};plan=Resolve({1,4},room);
    Check(plan.error==Error::None && plan.allies==1 && plan.enemies==4,"exact AI counts remain asymmetric with humans on both sides");
    Check(Resolve({2,4},room).error==Error::RoomFull,"oversubscribed exact counts are rejected, never silently clamped");
    room.hostTeam=1;room.humans={0,6,0};plan=Resolve({},room);
    Check(plan.error==Error::None && !plan.allies && plan.enemies==2,"automatic fill preserves an existing six-human team rather than demanding a balanced layout");
    room.hostTeam=0;Check(Resolve({},room).error==Error::MissingHost,"missing host cannot use a majority-team fallback");
    room.hostTeam=2;room.perTeamCapacity=6;
    Check(Resolve({0,kFillRoom},Room{8,6,1,{0,1,0},{}}).error==Error::TeamFull,"native side limit is explicit rather than hidden clipping");
    for(int capacity=2;capacity<=12;++capacity)for(int first=0;first<=capacity;++first)
      for(int second=0;first+second<=capacity;++second)for(uint8_t host:{uint8_t{1},uint8_t{2}}) {
        Room input{capacity,capacity,host,{0,first,second},{}};
        if(!input.humans[host])continue;
        for(int allies=0;allies<=capacity;++allies)for(int enemies=0;enemies<=capacity;++enemies) {
          auto result=Resolve({allies,enemies},input);
          if(result.error==Error::None)Check(result.allies==allies && result.enemies==enemies &&
             first+second+allies+enemies<=capacity,"every admitted exact plan preserves its counts and room capacity");
          else if(allies+enemies<=capacity-first-second && (input.humans[3-host]+enemies>0 || !allies))
             Check(false,"feasible exact native teams are not rejected");
        }
        auto fill=Resolve({0,kFillRoom},input);
        Check(fill.error==Error::None || (first+second==capacity && input.humans[3-host]==0),"enemy-only fill accepts all available native room places");
        if(fill.error==Error::None)Check(fill.allies==0 && fill.enemies==capacity-first-second,"enemy fill never adds an unwanted ally");
        const auto both=Resolve({},input);
        if(first+second<capacity || input.humans[3-host]>0)
            Check(both.error==Error::None && first+second+both.allies+both.enemies==capacity,
                "automatic room fill uses all vacancies even when humans chose an uneven layout");
      }
    std::printf("Native AI population policy: %d failures\n",failures);return failures?1:0;
}
