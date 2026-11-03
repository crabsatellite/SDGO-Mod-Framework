#include "sdgo/random_loadout.h"
#include <cstdio>
#include <functional>
#include <set>
namespace policy=sdgo::random_loadout;
int failures=0;
void Check(bool value,const char* label) {if(!value){++failures;std::printf("FAIL %s\n",label);}}
policy::Candidate Unit(uint32_t id,uint64_t serial,uint8_t rank,uint8_t points,uint32_t owner=31) {
    policy::Candidate unit;unit.owner=owner;unit.unit=id;unit.serial=serial;
    unit.rank=rank;unit.points=points;unit.eligible=true;return unit;
}
// Independent bounded enumeration is the oracle; it does not use the selector's
// DP, probability counts, or candidate grouping implementation.
std::set<std::vector<uint32_t>> Legal(const std::vector<policy::Candidate>& inventory,
                                     policy::Restrictions restriction) {
    std::vector<policy::Candidate> pool;std::set<uint64_t> serials;
    for(const auto& unit:inventory)
        if(unit.owner==31 && unit.eligible && unit.serial && unit.unit && unit.points>=1 && unit.points<=10 &&
           unit.rank>=restriction.minimumRank && unit.rank<=restriction.maximumRank &&
           (!restriction.onlyC || unit.rank==1) && serials.insert(unit.serial).second)pool.push_back(unit);
    std::set<std::vector<uint32_t>> result;
    for(unsigned mask=1;mask<(1u<<pool.size());++mask) {
        unsigned cost=0;std::vector<uint32_t> models;
        for(size_t i=0;i<pool.size();++i)if(mask&(1u<<i)){cost+=pool[i].points;models.push_back(pool[i].unit);}
        if(cost==10 && models.size()<=4){std::sort(models.begin(),models.end());result.insert(models);}
    }
    return result;
}
int main() {
    std::mt19937_64 rng(0x5D605D60);unsigned accepted=0,impossible=0;
    for(unsigned trial=0;trial<160;++trial) {
        std::vector<policy::Candidate> inventory;
        const unsigned count=3+static_cast<unsigned>(rng()%8);
        for(unsigned i=0;i<count;++i) {
            const uint8_t cost=static_cast<uint8_t>(1+rng()%10);
            inventory.push_back(Unit(100+i,10000+i,static_cast<uint8_t>(1+rng()%4),cost));
        }
        inventory.push_back(Unit(999,99999,4,10,32)); // another pilot's tempting one-unit deck
        auto disabled=Unit(998,99998,4,10);disabled.eligible=false;inventory.push_back(disabled);
        policy::Restrictions restrictions;
        if(trial%3==0)restrictions={1,2,false};
        if(trial%5==0)restrictions={1,1,true};
        const auto legal=Legal(inventory,restrictions);std::vector<uint32_t> previous;
        for(unsigned round=0;round<40;++round) {
            const auto selection=policy::Choose(inventory,31,restrictions,true,previous,rng);
            if(legal.empty()) {
                ++impossible;Check(selection.error==policy::Error::NoExactDeck || selection.error==policy::Error::NoEligibleUnit,
                    "an impossible budget cannot silently fall back to a partial deck");break;
            }
            ++accepted;Check(selection.error==policy::Error::None,"every feasible ten-point budget is selectable");
            unsigned cost=0;std::set<uint64_t> serials;
            for(const auto& unit:selection.units) {
                cost+=unit.points;Check(unit.owner==31 && unit.eligible,"only the current pilot's usable copies may spawn");
                Check(serials.insert(unit.serial).second,"an owned serial cannot occupy two deck slots");
            }
            const auto identity=policy::Identity(selection.units);
            Check(cost==10 && selection.units.size()<=4 && legal.count(identity),"selected deck agrees with independent exhaustive budget oracle");
            if(legal.size()>1 && !previous.empty())Check(identity!=previous,"a different model combination is required when alternatives exist");
            previous=identity;
        }
    }
    std::vector<policy::Candidate> variants={Unit(201,1,1,5),Unit(202,2,3,3),Unit(203,3,2,2)};
    auto selected=policy::Choose(variants,31,{},true,{},rng);
    Check(selected.error==policy::Error::None && selected.units.size()==3,"grade variants use their native points instead of assumed grade costs");
    std::vector<policy::Candidate> cVariants={Unit(501,51,1,3),Unit(502,52,1,3),Unit(503,53,1,2),Unit(504,54,1,2)};
    selected=policy::Choose(cVariants,31,{1,1,true},true,{},rng);
    Check(selected.error==policy::Error::None && selected.units.size()==4,
        "C-only variants with an actual legal ten-point combination are not categorically rejected");
    std::vector<policy::Candidate> sameModel={Unit(401,41,4,5),Unit(401,42,4,5)};
    selected=policy::Choose(sameModel,31,{},true,{401,401},rng);
    Check(selected.error==policy::Error::None && !selected.alternativeToPrevious && selected.units.size()==2,
        "one feasible model combination can repeat without fabricating a new unit");
    Check(accepted>100 && impossible>0,"both feasible and impossible budgets were exercised");
    std::printf("Random native-point regression: %u accepted rounds, %u impossible inventories, %d failures\n",accepted,impossible,failures);
    return failures?1:0;
}
