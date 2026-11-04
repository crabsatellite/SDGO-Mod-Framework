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
}
int main() {
    int errors=0;auto check=[&](bool ok){if(!ok)++errors;};
    World world; battle_ai::MapPathSearch search;
    search.Begin({-1000,0,0},{1000,600,0});
    check(search.Pump(Step,&world,1)==battle_ai::MapPathSearch::Status::Pending);
    check(world.calls<=9);
    auto prefix=search.Prefix();check(!prefix.empty());
    Vec3 from={-1000,0,0};for(Vec3 point:prefix){Vec3 out{};check(Step(&world,from,point,out));from=point;}
    for(int i=0;i<200&&search.State()==battle_ai::MapPathSearch::Status::Pending;++i)search.Pump(Step,&world,4);
    check(search.State()==battle_ai::MapPathSearch::Status::Found);
    from={-1000,0,0};bool usedOpening=false;
    for(Vec3 point:search.Path()) {Vec3 out{};check(Step(&world,from,point,out));if(point.z>=700)usedOpening=true;from=point;}
    check(usedOpening&&from.x==1000&&from.z==0&&from.y==0);
    world.closed=true;search.Begin({-1000,0,0},{1000,0,0},200,500);
    for(int i=0;i<200&&search.State()==battle_ai::MapPathSearch::Status::Pending;++i)search.Pump(Step,&world,4);
    check(search.State()==battle_ai::MapPathSearch::Status::Unreachable&&search.Path().empty()&&search.Nodes()<=500);
    search.Begin({0,0,0},{NAN,0,0});check(search.State()==battle_ai::MapPathSearch::Status::Unreachable);
    std::printf("map geometry path tests: %d failures\n",errors);return errors?1:0;
}
