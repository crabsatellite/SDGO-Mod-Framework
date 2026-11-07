#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mods/login_showcase/selection.h"
#include <cassert>
#include <set>
using namespace sdgo::login_showcase;
int main() {
    const std::vector<Unit> rows={{90000,1,4,2},{10001,2,4,3},{10002,3,4,4},
        {10003,4,4,5},{99999,5,1,2},{99998,6,4,1},{99997,7,0,0},{10003,4,4,5}};
    Config c;
    Mode mode=Mode::All;assert(ParseMode(" GRADES ",mode)&&mode==Mode::Grades);
    assert(ParseMode("all",mode)&&mode==Mode::All);
    assert(ParseMode("recent",mode)&&mode==Mode::Recent);
    assert(ParseMode("units",mode)&&mode==Mode::Units);
    assert(!ParseMode("unit",mode)&&!ParseMode("",mode));
    assert((Candidates(c,rows)==std::vector<uint32_t>{10003,10002,10001}));
    c.mode=Mode::Recent;c.recent_count=2;
    assert((Candidates(c,rows)==std::vector<uint32_t>{99999,10003}));
    c.mode=Mode::All;assert(Candidates(c,rows).size()==5);
    c.mode=Mode::Recent;c.recent_count=0;assert(Candidates(c,rows).empty());
    c.mode=Mode::Units;c.unit_ids={10002,99998,12345,10002,10001};
    assert((Candidates(c,rows)==std::vector<uint32_t>{10002,10001}));
    uint32_t mask=0;assert(ParseGrades("ss, SR,SU",mask)&&mask==kDefaultGrades);
    assert(ParseGrades("C,AR,S",mask)&&mask==(GradeBit(1,2)|GradeBit(3,4)|GradeBit(4,2)));
    for(auto bad:{"","XS","SS,","SS,,SR","SSS","S-training"})assert(!ParseGrades(bad,mask));
    std::vector<uint32_t> ids;assert(ParseUnits("12, 15,12",ids)&&(ids==std::vector<uint32_t>{12,15}));
    assert(ParseUnits("",ids)&&ids.empty());
    for(auto bad:{"0","-1","1,","2,,3","4294967296","1.0","true"})assert(!ParseUnits(bad,ids));
    Rotation rotation;rotation.SetPool({1,2,3});uint32_t seed=0;
    auto random=[&](){return ++seed*1664525u+1013904223u;};
    std::set<uint32_t> seen;uint32_t last=0;
    for(int round=0;round<10;++round) {
        seen.clear();
        for(int i=0;i<3;++i){auto n=rotation.Next(random);assert(n!=last);seen.insert(n);last=n;}
        assert(seen.size()==3);rotation.SetPool({1,2,3});
    }
    rotation.SetPool({7});assert(rotation.Next(random)==7&&rotation.Next(random)==7);
    rotation.SetPool({});assert(!rotation.Next(random));
}
