#include "../mods/battle_ai/map_path_search.h"
#include <cstdio>
using battle_ai::Vec3;
namespace {
struct World {bool closed=false;int calls=0;};
bool Step(void* opaque,Vec3 from,Vec3 desired,Vec3& grounded) {
    auto& world=*static_cast<World*>(opaque);++world.calls;
    if(std::abs(desired.x)>2000||std::abs(desired.z)>2000)return false;
    if((from.x<0&&desired.x>=0)||(desired.x<0&&from.x>=0)) {
        const float t=-from.x/(desired.x-from.x),crossing=from.z+t*(desired.z-from.z);
        if(world.closed||crossing<700||crossing>1300)return false;
    }
    grounded=desired;grounded.y=0;return true;
}
bool TwoFloors(void*,Vec3 from,Vec3 desired,Vec3& grounded) {
    if(std::abs(desired.x)>1500||std::abs(desired.z)>1500)return false;
    grounded=desired;
    // The upper deck has open ledges; the ground floor continues underneath.
    grounded.y=from.y>300&&std::abs(desired.x)<=500&&std::abs(desired.z)<=500?400.0f:0.0f;
    return true;
}
}
int main() {
    int errors=0;auto check=[&](bool ok){if(!ok)++errors;};
    World world; battle_ai::MapPathSearch search;
    search.Begin({-1000,0,0},{1000,0,0});
    check(search.Pump(Step,&world,1)==battle_ai::MapPathSearch::Status::Pending);
    check(world.calls<=9);
    auto prefix=search.Prefix();check(!prefix.empty());
    Vec3 from={-1000,0,0};for(Vec3 point:prefix){Vec3 out{};check(Step(&world,from,point,out));from=point;}
    for(int i=0;i<200&&search.State()==battle_ai::MapPathSearch::Status::Pending;++i)search.Pump(Step,&world,4);
    check(search.State()==battle_ai::MapPathSearch::Status::Found);
    from={-1000,0,0};bool usedOpening=false;
    for(Vec3 point:search.Path()) {Vec3 out{};check(Step(&world,from,point,out));if(point.z>=700)usedOpening=true;from=point;}
    check(usedOpening&&from.x==1000&&from.z==0&&from.y==0);
    search.Begin({0,400,0},{0,0,0});
    check(search.Pump(TwoFloors,nullptr,1)==battle_ai::MapPathSearch::Status::Pending);
    for(int i=0;i<500&&search.State()==battle_ai::MapPathSearch::Status::Pending;++i)search.Pump(TwoFloors,nullptr,4);
    check(search.State()==battle_ai::MapPathSearch::Status::Found);
    bool leftDeck=false,descended=false;from={0,400,0};
    for(Vec3 point:search.Path()) {
        Vec3 out{};check(TwoFloors(nullptr,from,point,out)&&std::abs(out.y-point.y)<=5);
        if(std::abs(point.x)>500||std::abs(point.z)>500)leftDeck=true;
        if(from.y>300&&point.y<100)descended=true;
        from=point;
    }
    check(leftDeck&&descended&&Length(from-Vec3{0,0,0})<1);
    search.Begin({0,0,0},{0,400,0});
    for(int i=0;i<500&&search.State()==battle_ai::MapPathSearch::Status::Pending;++i)search.Pump(TwoFloors,nullptr,4);
    check(search.State()==battle_ai::MapPathSearch::Status::Unreachable);
    world.closed=true;search.Begin({-1000,0,0},{1000,0,0},200,500);
    for(int i=0;i<200&&search.State()==battle_ai::MapPathSearch::Status::Pending;++i)search.Pump(Step,&world,4);
    check(search.State()==battle_ai::MapPathSearch::Status::Unreachable&&search.Path().empty()&&search.Nodes()<=500);
    search.Begin({0,0,0},{NAN,0,0});check(search.State()==battle_ai::MapPathSearch::Status::Unreachable);
    std::printf("map geometry path tests: %d failures\n",errors);return errors?1:0;
}
