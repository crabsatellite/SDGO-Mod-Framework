#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include "../mods/battle_ai/native_shape_bounds.h"
using battle_ai::Vec3;
using battle_ai::native::NativeShapeBounds;
namespace {
static NativeShapeBounds bounds;
std::array<uint8_t,0x84> shape{};
int checks=0,failed=0;
void Check(bool value,const char* name){++checks;if(!value){++failed;std::printf("FAIL %s\n",name);}}
void World(Vec3 center,Vec3 a,Vec3 b,Vec3 c,Vec3 extents) {
    shape={};memcpy(shape.data()+0x3C,&center,sizeof(center));memcpy(shape.data()+0x48,&a,sizeof(a));
    memcpy(shape.data()+0x54,&b,sizeof(b));memcpy(shape.data()+0x60,&c,sizeof(c));memcpy(shape.data()+0x78,&extents,sizeof(extents));
}
}
int main(){
    World({0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1});
    Check(!bounds.MayAffect(shape.data(),{100,0,0},{101,0,0}),"distant box is excluded");
    Check(bounds.MayAffect(shape.data(),{-5,0,0},{5,0,0}),"crossed box is retained");
    Check(bounds.MayAffect(shape.data(),{0,0,0},{0,0,0}),"initial overlap is retained");
    Check(bounds.MayAffect(shape.data(),{-10000,-0.9998f,0},{10000,-1.9998f,0}),"native near-parallel accepted ray envelope grows with travel");
    World({0,0,0},{2,0,0},{0,1,0},{0,0,1},{1,1,1});
    Check(bounds.MayAffect(shape.data(),{1.9f,0,0},{1.9f,0,0}),"inverse-transform box is retained under scale");
    Check(bounds.MayAffect(shape.data(),{3.9f,0,0},{3.9f,0,0},1),"native local sphere metric expands with row scale");
    World({0,0,0},{0.5f,0,0},{0,1,0},{0,0,1},{1,1,1});
    Check(bounds.MayAffect(shape.data(),{1.9f,0,0},{1.9f,0,0}),"distinct ray slab box is retained under scale");
    World({0,0,0},{1,0.5f,0},{0,1,0},{0,0,1},{1,1,1});
    Check(bounds.MayAffect(shape.data(),{1.4f,-1,0},{1.4f,-1,0}),"sheared native slab box remains in the union");
    Check(bounds.MayAffect(shape.data(),{1,1.4f,0},{1,1.4f,0}),"sheared inverse-transform contact box remains in the union");
    World({100,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1});
    Check(!bounds.MayAffect(shape.data(),{0,0,0},{1,0,0}),"changed pose invalidates cached bounds");
    World({0,0,0},{1,0,0},{1,0,0},{0,0,1},{1,1,1});
    Check(bounds.MayAffect(shape.data(),{100,0,0},{101,0,0}),"singular native inverse keeps the exact predicate");
    World({0,0,0},{1,0,0},{0,1,0},{0,0,1},{-1,1,1});
    Check(bounds.MayAffect(shape.data(),{100,0,0},{101,0,0}),"invalid extents keep the exact predicate");
    Check(bounds.MayAffect(shape.data(),{std::numeric_limits<float>::infinity(),0,0},{0,0,0}),"nonfinite query cannot be culled");
    std::mt19937 random(0x2673419u);std::uniform_real_distribution<float> signedUnit(-1,1),size(0.2f,3),where(-30000,30000);
    unsigned misses=0;
    for(unsigned sample=0;sample<10000;++sample){
        const float angle=signedUnit(random)*3.14159265f,cs=std::cos(angle),sn=std::sin(angle);
        const float sx=size(random),sy=size(random),sz=size(random),radius=size(random);
        const Vec3 rows[3]={{cs*sx,0,sn*sx},{0,sy,0},{-sn*sz,0,cs*sz}};
        const Vec3 center{where(random),where(random),where(random)},extents{size(random)*30,size(random)*30,size(random)*30};
        World(center,rows[0],rows[1],rows[2],extents);
        // Independent guaranteed native sphere-overlap construction: local
        // point is in the box plus a local-radius cube, then map by A^T.
        const Vec3 local{signedUnit(random)*extents.x,signedUnit(random)*extents.y,signedUnit(random)*extents.z};
        const Vec3 contact=center+rows[0]*local.x+rows[1]*local.y+rows[2]*local.z;
        if(!bounds.MayAffect(shape.data(),contact,contact,radius))++misses;
        // Ray-slab point uses A^-1, independently constructed for this rotated
        // diagonal matrix (not the implementation's cached inverse routine).
        const Vec3 rayPoint=center+Vec3{cs*local.x/sx-sn*local.z/sz,local.y/sy,sn*local.x/sx+cs*local.z/sz};
        if(!bounds.MayAffect(shape.data(),rayPoint+Vec3{50,0,0},rayPoint-Vec3{50,0,0}))++misses;
    }
    Check(misses==0,"20000 guaranteed transformed native true cases are retained");
    Check(bounds.stats.culled>0,"fixture actually exercised shape rejection");
    std::printf("%d shape-bound checks, %d failures; guaranteed-case misses=%u\n",checks,failed,misses);
    return failed?1:0;
}
