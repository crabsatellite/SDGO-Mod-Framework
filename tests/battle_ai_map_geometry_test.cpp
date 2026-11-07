#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include "../mods/battle_ai/native_map_geometry.h"
#include "../mods/battle_ai/map_path_search.h"
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
float groundY=0,groundSlopeX=0,renderOffset=0;int objectKind=0;
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
    const float denominator=ray->direction.y-groundSlopeX*ray->direction.x;
    if(std::abs(denominator)>1e-6f&&!missingGround) {
        *distance=(groundY+renderOffset+groundSlopeX*ray->origin.x-ray->origin.y)/denominator;
        if(*distance>=0)return true;
    }
    if(terrainWall&&std::abs(ray->direction.x)>0.001f) {
        *distance=-ray->origin.x/ray->direction.x;return *distance>=0;
    }
    return false;
}
bool __cdecl ShapeRay(const Ray3*,const void*,float*){return false;}
TerrainSupportStatus PhysicsSupport(void*,Vec3 point,TerrainSupportSample& out) {
    if(missingGround)return TerrainSupportStatus::Missing;
    out.height=groundY+groundSlopeX*point.x;
    out.normal=Normalize(Vec3{-groundSlopeX,1,0});out.spacing=25;
    if(terrainWall&&std::abs(point.x)<100)out.normal={1,0,0};
    return TerrainSupportStatus::Valid;
}
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
    api.support=&PhysicsSupport;
    MapGeometry geometry{api,document.data(),owner.data(),{-1000,0,0},{{-1025,0,-25},{-975,100,25}},{{-1025,25,-25},{-975,75,25}}};
    geometry.bodySphere={{0,50,0},25,{-1000,50,0},25};geometry.sphereObb=&SphereObb;geometry.sphereObbContact=&SphereContact;
    return geometry;
}
}
int main() {
    int errors=0,checks=0;auto check=[&](bool ok){++checks;if(!ok){++errors;std::printf("FAIL geometry check %d\n",checks);}};
    auto geometry=Geometry();Vec3 grounded{};
    check(geometry.Step({-1000,0,0},{-500,600,0},grounded)&&grounded.x==-500&&grounded.y==0);
    renderOffset=-50;
    check(geometry.Ground({-500,0,0},80,-4096,grounded)&&grounded.y==0);
    renderOffset=0;
    groundY=100;groundSlopeX=0.1f;
    check(geometry.ClearBody({-1000,0,0},{-500,0,0}));
    check(geometry.Step({-1000,0,0},{-500,0,0},grounded)&&std::abs(grounded.y-50)<0.01f);
    groundY=0;groundSlopeX=0;
    missingGround=true;check(!geometry.Step({-1000,0,0},{-500,0,0},grounded));missingGround=false;
    groundY=100;check(geometry.Step({-1000,0,0},{-500,0,0},grounded)&&grounded.y==100);groundY=-200;
    check(geometry.Step({-1000,0,0},{-500,0,0},grounded)&&grounded.y==-200);groundY=0;
    groundY=1000;groundSlopeX=1;
    check(geometry.Step({-1000,0,0},{-500,0,0},grounded)&&grounded.y==500);
    groundSlopeX=2;groundY=2000;
    check(!geometry.Step({-1000,0,0},{-500,0,0},grounded));
    groundSlopeX=0;groundY=0;
    terrainWall=true;check(!geometry.Step({-200,0,0},{200,0,0},grounded));terrainWall=false;
    haveObstacle=true;objectKind=0x30;check(!geometry.Step({-200,0,0},{200,0,0},grounded));
    objectKind=7;
    check(geometry.Step({-200,0,0},{200,0,0},grounded));
    check(geometry.Ground({0,0,0},500,-1000,grounded)&&grounded.y==0);
    objectKind=0x30;
    Put(obstacle.data(),detail::kSceneBackPointer,static_cast<uint8_t*>(nullptr));
    check(!geometry.Step({-200,0,0},{200,0,0},grounded));
    Put(obstacle.data(),detail::kSceneBackPointer,object.data());
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
    Put(obstacleObb.data(),0,Aabb{{-10,0,-1000},{10,300,1000}});
    const Aabb mapBounds{{-1000,-1000000,-1000},{1000,1000000,1000}};
    check(!geometry.ClearBody({-200,0,0},{200,0,0})&&
          geometry.ClearForcedTravel({-200,0,0},{200,0,0},mapBounds));
    check(!geometry.ClearForcedTravel({-200,0,0},{1200,0,0},mapBounds)&&
          geometry.lastReject==10);
    Put(obstacle.data(),detail::kSceneAabb,Aabb{{-300,300,-200},{100,400,200}});
    Put(obstacleObb.data(),0,Aabb{{-300,300,-200},{100,400,200}});
    check(geometry.Step({-100,400,0},{-50,400,0},grounded)&&grounded.y==400);
    missingGround=true;
    check(geometry.Step({-100,400,0},{-50,400,0},grounded)&&grounded.y==400);
    check(!geometry.Step({-100,400,0},{300,400,0},grounded));missingGround=false;
    check(geometry.Step({-100,400,0},{300,400,0},grounded)&&grounded.y==0);
    MapPathSearch route;route.Begin({-100,400,0},{-100,0,0});
    const auto step=[](void* context,Vec3 from,Vec3 to,Vec3& result) {
        return static_cast<MapGeometry*>(context)->Step(from,to,result);
    };
    check(route.Pump(step,&geometry,1)==MapPathSearch::Status::Pending);
    for(int i=0;i<300&&route.State()==MapPathSearch::Status::Pending;++i)route.Pump(step,&geometry,4);
    check(route.State()==MapPathSearch::Status::Found);
    Vec3 previous={-100,400,0};bool leftPlatform=false;
    for(Vec3 point:route.Path()) {
        Vec3 result{};check(geometry.Step(previous,point,result)&&std::abs(result.y-point.y)<=5);
        if(point.x>100||point.x<-300||std::abs(point.z)>200)leftPlatform=true;
        previous=point;
    }
    check(leftPlatform&&Length(previous-Vec3{-100,0,0})<1);
    haveObstacle=false;
    haveHazard=true;
    Put(hazardScene.data(),detail::kSceneAabb,Aabb{{150,-100,-100},{250,200,100}});
    check(!geometry.Step({-100,400,0},{300,400,0},grounded));
    check(!geometry.ClearForcedTravel({-100,0,0},{300,0,0},mapBounds)&&
          geometry.lastReject==5);
    // Broad-phase rejection must not bypass ClearObb's non-geometric sample
    // budget. The scene envelope crosses the route; its only box is far away.
    geometry=Geometry();haveHazard=false;haveObstacle=true;objectKind=0x30;
    geometry.bodySphere.radius=geometry.bodySphere.localRadius=1;
    geometry.bodyBounds={{-1001,49,-1},{-999,51,1}};
    Put(obstacle.data(),detail::kSceneAabb,Aabb{{-2000,0,-10000},{10000,300,10000}});
    Put(obstacleObb.data(),0,Aabb{{2000,20,4990},{2020,120,5010}});
    Put(obstacleObb.data(),0x3C,Vec3{2010,70,5000});
    Put(obstacleObb.data(),0x48,Vec3{1,0,0});
    Put(obstacleObb.data(),0x54,Vec3{0,1,0});
    Put(obstacleObb.data(),0x60,Vec3{0,0,1});
    Put(obstacleObb.data(),0x78,Vec3{10,50,10});
    check(!geometry.ClearBody({-1000,0,0},{4000,0,0}));
    auto shapeBounds=std::make_unique<NativeShapeBounds>();
    geometry.shapeBounds=shapeBounds.get();
    check(!geometry.ClearBody({-1000,0,0},{4000,0,0}));
    check(geometry.ClearBody({-1000,0,0},{3000,0,0})&&shapeBounds->stats.culled>0);
    shapeBounds->SetAudit(true);
    check(!geometry.ClearBody({-1000,0,0},{4000,0,0})&&shapeBounds->stats.mismatches==0);
    // Deliberately make the exact predicate disagree with the broad phase.
    // The audit must report this synchronously, before any periodic summary.
    static int immediateReports=0;
    shapeBounds->SetMismatchSink([](){++immediateReports;});
    geometry.api.rayObb=[](const Ray3*,const void*,float* distance){*distance=1;return true;};
    check(!geometry.ClearBody({-1000,0,0},{3000,0,0})&&
          shapeBounds->stats.mismatches==1&&immediateReports==1);
    check(!geometry.ClearBody({-1000,0,0},{3000,0,0})&&
          shapeBounds->stats.mismatches==2&&immediateReports==1);
    std::printf("%d map geometry checks, %d failures\n",checks,errors);return errors?1:0;
}
