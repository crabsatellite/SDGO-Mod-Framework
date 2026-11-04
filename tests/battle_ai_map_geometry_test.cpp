#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include "../mods/battle_ai/native_map_geometry.h"
namespace {
using namespace battle_ai;
using namespace battle_ai::native;
std::array<uint8_t,0x8500> document{};
std::array<uint8_t,0x100> owner{};
std::array<uint8_t,0x240> terrain{},obstacle{},ownScene{};
std::array<uint8_t,0x240> hazardScene{};
std::array<uint8_t,0x40> hazardObject{};
std::array<uint8_t,0x40> object{};
std::array<uint8_t,0x84> obstacleObb{};
std::array<void*,8> terrainVtable{},solidVtable{};
std::array<void*,12> objectVtable{};
std::array<void*,12> hazardVtable{};
bool missingGround=false,terrainWall=false,haveObstacle=false;
bool haveHazard=false;
float groundY=0;int objectKind=0;
bool destroyableAttackable=true;
template<class T>void Put(void* at,size_t offset,const T& value){memcpy(static_cast<uint8_t*>(at)+offset,&value,sizeof(value));}
bool __fastcall TerrainKind(void*,void*,int kind){return kind==3;}
bool __fastcall SolidKind(void*,void*,int kind){return kind==2;}
bool __fastcall ObjectKind(void*,void*,int kind){return kind==objectKind;}
bool __fastcall CanAttack(void*,void*,void*){return destroyableAttackable;}
bool __fastcall HazardKind(void*,void*,int kind){return kind==0x22;}
void __fastcall Query(void*,void*,void** scenes,int* count,int,const Aabb*) {
    scenes[0]=terrain.data();*count=1;if(haveObstacle)scenes[(*count)++]=obstacle.data();
    if(haveHazard)scenes[(*count)++]=hazardScene.data();
}
bool __cdecl Segment(const Vec3* center,const Vec3* halfDelta,const Aabb* box) {
    const float a[3]={center->x-halfDelta->x,center->y-halfDelta->y,center->z-halfDelta->z};
    const float d[3]={halfDelta->x*2,halfDelta->y*2,halfDelta->z*2};
    const float low[3]={box->min.x,box->min.y,box->min.z},high[3]={box->max.x,box->max.y,box->max.z};
    float enter=0,leave=1;
    for(int i=0;i<3;++i) {
        if(std::abs(d[i])<1e-6f){if(a[i]<low[i]||a[i]>high[i])return false;continue;}
        float nearHit=(low[i]-a[i])/d[i],farHit=(high[i]-a[i])/d[i];if(nearHit>farHit)std::swap(nearHit,farHit);
        enter=(std::max)(enter,nearHit);leave=(std::min)(leave,farHit);if(enter>leave)return false;
    }
    return true;
}
bool __fastcall TerrainRay(void*,void*,const Ray3* ray,float* distance) {
    if(ray->direction.y<-0.99f&&!missingGround) {
        *distance=ray->origin.y-groundY;return *distance>=0;
    }
    if(terrainWall&&std::abs(ray->direction.x)>0.001f) {
        *distance=-ray->origin.x/ray->direction.x;return *distance>=0;
    }
    return false;
}
bool __cdecl ShapeRay(const Ray3*,const void*,float*){return false;}
float SphereDistance(const MapBodySphere& sphere,const Aabb& box) {
    const Vec3 point={(std::max)(box.min.x,(std::min)(sphere.center.x,box.max.x)),
                      (std::max)(box.min.y,(std::min)(sphere.center.y,box.max.y)),
                      (std::max)(box.min.z,(std::min)(sphere.center.z,box.max.z))};
    float distance=static_cast<float>(Length(point-sphere.center));
    if(distance==0)distance=-(std::min)({sphere.center.x-box.min.x,box.max.x-sphere.center.x,
        sphere.center.y-box.min.y,box.max.y-sphere.center.y,sphere.center.z-box.min.z,box.max.z-sphere.center.z});
    return distance;
}
bool __cdecl SphereObb(const MapBodySphere* sphere,const void* shape) {
    return SphereDistance(*sphere,*static_cast<const Aabb*>(shape))<=sphere->radius;
}
bool __cdecl SphereContact(const MapBodySphere* sphere,const void* shape,const Vec3*,Vec3* contact,float* distance) {
    *contact={1,0,0};*distance=SphereDistance(*sphere,*static_cast<const Aabb*>(shape));return *distance<=sphere->radius;
}
bool __cdecl ObbRay(const Ray3* ray,const void* shape,float* distance) {
    const auto& box=*static_cast<const Aabb*>(shape);
    const float origin[3]={ray->origin.x,ray->origin.y,ray->origin.z},delta[3]={ray->direction.x,ray->direction.y,ray->direction.z};
    const float low[3]={box.min.x,box.min.y,box.min.z},high[3]={box.max.x,box.max.y,box.max.z};
    float entry=-1e30f,exit=1e30f;
    for(int axis=0;axis<3;++axis) {
        if(std::abs(delta[axis])<1e-6f){if(origin[axis]<low[axis]||origin[axis]>high[axis])return false;continue;}
        float first=(low[axis]-origin[axis])/delta[axis],last=(high[axis]-origin[axis])/delta[axis];
        if(first>last)std::swap(first,last);entry=(std::max)(entry,first);exit=(std::min)(exit,last);
        if(entry>exit)return false;
    }
    if(exit<0)return false;*distance=entry>=0?entry:exit;return true;
}
MapGeometry Geometry() {
    terrainVtable[5]=reinterpret_cast<void*>(&TerrainKind);solidVtable[5]=reinterpret_cast<void*>(&SolidKind);
    objectVtable[1]=reinterpret_cast<void*>(&ObjectKind);
    objectVtable[10]=reinterpret_cast<void*>(&CanAttack);
    hazardVtable[1]=reinterpret_cast<void*>(&HazardKind);
    Put(hazardScene.data(),0,solidVtable.data());Put(hazardObject.data(),0,hazardVtable.data());
    Put(hazardScene.data(),detail::kSceneBackPointer,hazardObject.data());
    Put(document.data(),detail::kDocumentQuadTree,document.data());Put(owner.data(),detail::kOwnerScene,ownScene.data());
    Put(terrain.data(),0,terrainVtable.data());Put(obstacle.data(),0,solidVtable.data());Put(object.data(),0,objectVtable.data());
    Put(obstacle.data(),detail::kSceneBackPointer,object.data());Put(obstacle.data(),detail::kObbCount,int(1));
    Put(object.data(),0x38,int32_t(1000));
    Put(obstacle.data(),detail::kObbArray,obstacleObb.data());
    Put(terrain.data(),detail::kSceneAabb,Aabb{{-10000,-1000,-10000},{10000,1000,10000}});
    Put(obstacle.data(),detail::kSceneAabb,Aabb{{-10,0,-1000},{10,300,1000}});
    Put(obstacleObb.data(),0,Aabb{{-10,0,-1000},{10,300,1000}});
    NavigationApi api{reinterpret_cast<QueryFn>(&Query),&Segment,reinterpret_cast<TerrainRayFn>(&TerrainRay),&ObbRay,&ShapeRay};
    MapGeometry geometry{api,document.data(),owner.data(),{-1000,0,0},{{-1025,0,-25},{-975,100,25}},{{-1025,25,-25},{-975,75,25}}};
    geometry.bodySphere={{0,50,0},25,{-1000,50,0},25};geometry.sphereObb=&SphereObb;geometry.sphereObbContact=&SphereContact;
    return geometry;
}
}
int main() {
    int errors=0,checks=0;auto check=[&](bool ok){++checks;if(!ok){++errors;std::printf("FAIL geometry check %d\n",checks);}};
    auto geometry=Geometry();Vec3 grounded{};
    check(geometry.Step({-1000,0,0},{-500,600,0},grounded)&&grounded.x==-500&&grounded.y==0);
    missingGround=true;check(!geometry.Step({-1000,0,0},{-500,0,0},grounded));missingGround=false;
    groundY=100;check(!geometry.Step({-1000,0,0},{-500,0,0},grounded));groundY=-200;
    check(geometry.Step({-1000,0,0},{-500,0,0},grounded)&&grounded.y==-200);groundY=0;
    terrainWall=true;check(!geometry.Step({-200,0,0},{200,0,0},grounded));terrainWall=false;
    haveObstacle=true;objectKind=0x30;check(!geometry.Step({-200,0,0},{200,0,0},grounded));
    objectKind=0x12;check(geometry.Step({-200,0,0},{200,0,0},grounded));
    Put(object.data(),0x38,int32_t(0));check(!geometry.Step({-200,0,0},{200,0,0},grounded));
    Put(object.data(),0x38,int32_t(1000));destroyableAttackable=false;
    check(!geometry.Step({-200,0,0},{200,0,0},grounded));destroyableAttackable=true;
    geometry.deferDestroyables=false;check(!geometry.ClearBody({-200,0,0},{200,0,0}));geometry.deferDestroyables=true;
    objectKind=0x22;check(!geometry.Step({-200,0,0},{200,0,0},grounded));
    Put(obstacle.data(),detail::kSceneAabb,Aabb{{4200,0,-1000},{4210,300,1000}});
    check(!geometry.ClearBody({4000,0,0},{4500,0,0}));
    check(geometry.ClearBody({4000,0,2000},{4500,0,2000}));
    objectKind=0x30;Put(obstacle.data(),detail::kSceneAabb,Aabb{{-100,-100,-100},{100,100,100}});
    Put(obstacleObb.data(),0,Aabb{{-100,-100,-100},{100,100,100}});
    check(geometry.ClearBody({-110,0,0},{-200,0,0}));
    check(geometry.ClearBody({-100,0,0},{-200,0,0}));
    check(geometry.ClearBody({-125,0,-50},{-125,0,50}));
    check(!geometry.ClearBody({-110,0,0},{0,0,0}));
    Put(obstacle.data(),detail::kSceneAabb,Aabb{{-10,0,-1000},{10,300,1000}});
    Put(obstacleObb.data(),0,Aabb{{-10,0,900},{10,300,1000}});
    check(geometry.ClearBody({-200,0,0},{200,0,0}));
    Put(obstacle.data(),detail::kSceneAabb,Aabb{{-300,300,-200},{100,400,200}});
    Put(obstacleObb.data(),0,Aabb{{-300,300,-200},{100,400,200}});
    check(geometry.Step({-100,400,0},{-50,400,0},grounded)&&grounded.y==400);
    missingGround=true;
    check(geometry.Step({-100,400,0},{-50,400,0},grounded)&&grounded.y==400);
    check(!geometry.Step({-100,400,0},{300,400,0},grounded));missingGround=false;
    check(geometry.Step({-100,400,0},{300,400,0},grounded)&&grounded.y==0);
    haveHazard=true;
    Put(hazardScene.data(),detail::kSceneAabb,Aabb{{150,-100,-100},{250,200,100}});
    check(!geometry.Step({-100,400,0},{300,400,0},grounded));
    std::printf("%d map geometry checks, %d failures\n",checks,errors);return errors?1:0;
}
