#pragma once
#include "native_navigation.h"
#include "native_map_gate.h"

namespace battle_ai::native {

struct MapBodySphere {Vec3 localCenter{};float localRadius=0;Vec3 center{};float radius=0;};
static_assert(sizeof(MapBodySphere)==0x20);
using MapSphereObbOverlapFn=bool(__cdecl*)(const MapBodySphere*,const void*);
using MapSphereObbContactFn=bool(__cdecl*)(const MapBodySphere*,const void*,const Vec3*,Vec3*,float*);

// Candidate walking edges for maps without authored navigation. Native motor
// collision remains authoritative. Ground is always measured from the loaded
// terrain; authored spawn points are goals, never assumed standable surfaces.
struct MapGeometry {
    NavigationApi api{};
    uint8_t* document=nullptr;
    uint8_t* owner=nullptr;
    Vec3 ownerPosition{};
    Aabb ownerBounds{};
    Aabb bodyBounds{};
    MapBodySphere bodySphere{};
    MapSphereObbOverlapFn sphereObb=nullptr;
    MapSphereObbContactFn sphereObbContact=nullptr;
    bool deferDestroyables=true;
    mutable uint32_t projections=0,groundMisses=0,blocked=0,lastReject=0;
    mutable uint8_t* rejectedScene=nullptr;
    mutable uint8_t* rejectedObject=nullptr;
    mutable uint32_t rejectedShape=0,shapeReject=0;
    mutable float initialDepth=0,rejectedDepth=0,rejectedRayDistance=0;
    mutable Vec3 rejectedSphereCenter{};
    mutable float rejectedSphereRadius=0;
    bool Valid()const {
        return api.query&&api.segmentAabb&&api.terrainRay&&api.rayObb&&api.raySphere&&sphereObb&&sphereObbContact&&document&&owner&&
            Finite(ownerPosition)&&detail::ValidAabb(ownerBounds)&&detail::ValidAabb(bodyBounds)&&
            bodyBounds.max.x>bodyBounds.min.x&&bodyBounds.max.y>bodyBounds.min.y&&bodyBounds.max.z>bodyBounds.min.z&&
            Finite(bodySphere.center)&&std::isfinite(bodySphere.radius)&&bodySphere.radius>0;
    }
    bool ClearObb(Vec3 from,Vec3 to,const void* obb)const {
        shapeReject=0;initialDepth=rejectedDepth=rejectedRayDistance=0;
        const Vec3 delta=to-from;const double length=Length(delta);
        if(!std::isfinite(length))return false;
        const Vec3 direction=Normalize(delta);
        MapBodySphere start=bodySphere;start.center=start.center+(from-ownerPosition);
        const auto penetration=[&](const MapBodySphere& sphere,float& depth) {
            if(!sphereObb(&sphere,obb)){depth=0;return true;}
            Vec3 contact{};float distance=0;
            if(!sphereObbContact(&sphere,obb,&direction,&contact,&distance)){depth=0;return true;}
            if(!std::isfinite(distance))return false;
            depth=(std::max)(0.0f,sphere.radius-distance);return true;
        };
        float previous=0;if(!penetration(start,previous)){shapeReject=1;return false;}
        initialDepth=previous;
        // A body already touching/intersecting geometry must be able to leave
        // it or slide with no increased penetration. Reject crossing its core.
        if(length>1e-6&&previous<bodySphere.radius-0.001f) {
            const Ray3 ray={start.center,direction};float distance=0;
            const bool hit=api.rayObb(&ray,obb,&distance);
            if(detail::BlockingHit(hit,distance,length)){shapeReject=2;rejectedRayDistance=distance;return false;}
        }
        const float interval=(std::max)(1.0f,bodySphere.radius*0.25f);
        const int samples=(std::max)(1,static_cast<int>(std::ceil(length/interval)));
        if(samples>4096)return false;
        for(int sample=1;sample<=samples;++sample) {
            MapBodySphere sphere=start;sphere.center=sphere.center+delta*(static_cast<float>(sample)/static_cast<float>(samples));
            float depth=0;if(!penetration(sphere,depth)){shapeReject=3;return false;}
            if(depth>previous+0.01f){shapeReject=4;rejectedDepth=depth;return false;}
            previous=depth;
        }
        return true;
    }
    bool ClearSphere(Vec3 from,Vec3 to,const MapBodySphere& obstacle)const {
        if(!Finite(obstacle.center)||!std::isfinite(obstacle.radius)||obstacle.radius<=0)return false;
        const Vec3 start=bodySphere.center+(from-ownerPosition),delta=to-from,offset=start-obstacle.center;
        const double radius=double(bodySphere.radius)+obstacle.radius,initial=Dot(offset,offset),travel=Dot(delta,delta);
        if(initial<radius*radius) return Dot(offset,delta)>=0; // only separating/sliding movement
        const double factor=travel>1e-12?(std::max)(0.0,(std::min)(1.0,-Dot(offset,delta)/travel)):0;
        const Vec3 nearest=offset+delta*static_cast<float>(factor);
        return Dot(nearest,nearest)>=radius*radius;
    }
    bool Ground(Vec3 desired,float ceiling,float floor,Vec3& grounded,bool terrainRise=false)const {
        ++projections;
        if(!Valid()||!Finite(desired)||!std::isfinite(ceiling)||!std::isfinite(floor)||ceiling<=floor)return false;
        auto tree=*reinterpret_cast<void**>(document+detail::kDocumentQuadTree);
        if(!tree)return false;
        TerrainSupportSample support{};
        const auto status=api.support?api.support(api.supportContext,desired,support):TerrainSupportStatus::Missing;
        if(status==TerrainSupportStatus::Outside||status==TerrainSupportStatus::Invalid){lastReject=10;return false;}
        bool hit=status==TerrainSupportStatus::Valid&&support.height>=floor&&(terrainRise||support.height<=ceiling);
        float best=hit?support.height:floor;
        const Aabb bounds={{desired.x-1,floor,desired.z-1},{desired.x+1,ceiling,desired.z+1}};
        void* scenes[detail::kSceneCapacity]{};int count=0;
        api.query(tree,scenes,&count,detail::kSceneCapacity,&bounds);
        if(count<0||count>=detail::kSceneCapacity)return false;
        const Ray3 ray={{desired.x,ceiling,desired.z},{0,-1,0}};
        const auto recordHit=[&](bool contact,float distance) {
            const float y=ceiling-distance;
            if(contact&&std::isfinite(distance)&&distance>=0&&y>=floor&&y<=ceiling&&(!hit||y>best)){best=y;hit=true;}
        };
        for(int i=0;i<count;++i) {
            auto scene=static_cast<uint8_t*>(scenes[i]);if(!scene)return false;
            auto table=*reinterpret_cast<void***>(scene);
            if(!table||!table[5])return false;
            const auto isScene=reinterpret_cast<detail::SceneIsAFn>(table[5]);
            float distance=0;bool contact=false;
            if(isScene(scene,3))continue; // physics support is bilinear, not the display mesh
            if(!isScene(scene,2))continue;
            auto object=*reinterpret_cast<uint8_t**>(scene+detail::kSceneBackPointer);
            if(object==owner)continue;
            if(object) {
                auto objectTable=*reinterpret_cast<void***>(object);if(!objectTable||!objectTable[1])return false;
                const auto isObject=reinterpret_cast<detail::ObjectIsAFn>(objectTable[1]);
                if(object[0x1C]||isObject(object,7)||isObject(object,0x15)||isObject(object,9)||isObject(object,0x21)||isObject(object,0x22))continue;
            }
            const int obbCount=*reinterpret_cast<int*>(scene+detail::kObbCount);
            auto obbs=*reinterpret_cast<uint8_t**>(scene+detail::kObbArray);
            const int sphereCount=scene[detail::kSphereEnabled]?*reinterpret_cast<int*>(scene+detail::kSphereCount):0;
            auto spheres=*reinterpret_cast<uint8_t**>(scene+detail::kSphereArray);
            if(!detail::ValidShapeArray(obbs,obbCount,detail::kObbStride)||
               !detail::ValidShapeArray(spheres,sphereCount,detail::kSphereStride))return false;
            for(int shape=0;shape<obbCount;++shape) {
                distance=0;contact=api.rayObb(&ray,obbs+size_t(shape)*detail::kObbStride,&distance);recordHit(contact,distance);
            }
            for(int shape=0;shape<sphereCount;++shape) {
                distance=0;contact=api.raySphere(&ray,spheres+size_t(shape)*detail::kSphereStride,&distance);recordHit(contact,distance);
            }
        }
        if(!hit){++groundMisses;lastReject=3;return false;}
        grounded={desired.x,best,desired.z};return Finite(grounded);
    }
    bool ClearBody(Vec3 from,Vec3 to,bool hazardsOnly=false)const {
        using namespace detail;
        if(!Valid()||!Finite(from)||!Finite(to))return false;
        auto tree=*reinterpret_cast<void**>(document+kDocumentQuadTree);if(!tree)return false;
        auto ownScene=*reinterpret_cast<void**>(owner+kOwnerScene);
        const Vec3 displacement=from-ownerPosition;
        const Aabb start={bodyBounds.min+displacement,bodyBounds.max+displacement};
        const Aabb hazardStart={ownerBounds.min+displacement,ownerBounds.max+displacement};
        const Vec3 delta=to-from,half=(start.max-start.min)*0.5f;
        const Aabb finish={start.min+delta,start.max+delta};
        const Aabb hazardFinish={hazardStart.min+delta,hazardStart.max+delta};
        const Aabb query={
            {(std::min)(hazardStart.min.x,hazardFinish.min.x),(std::min)(hazardStart.min.y,hazardFinish.min.y),(std::min)(hazardStart.min.z,hazardFinish.min.z)},
            {(std::max)(hazardStart.max.x,hazardFinish.max.x),(std::max)(hazardStart.max.y,hazardFinish.max.y),(std::max)(hazardStart.max.z,hazardFinish.max.z)}};
        void* scenes[kSceneCapacity]{};int count=0;
        api.query(tree,scenes,&count,kSceneCapacity,&query);
        if(count<0||count>=kSceneCapacity)return false;
        const double length=Length(delta);if(!std::isfinite(length))return false;
        if(length<1e-6)return true;
        const Vec3 segmentCenter=(start.min+start.max)*0.5f+delta*0.5f,halfDelta=delta*0.5f;
        const float low=(std::max)(start.min.y+10,from.y+10);
        if(!hazardsOnly&&api.support) {
            TerrainSupportSample support{};
            auto status=api.support(api.supportContext,to,support);
            if(status==TerrainSupportStatus::Outside||status==TerrainSupportStatus::Invalid){lastReject=10;return false;}
            const float spacing=status==TerrainSupportStatus::Valid?support.spacing:25;
            const double horizontal=std::hypot(double(delta.x),double(delta.z));
            const int samples=(std::max)(1,static_cast<int>(std::ceil(horizontal/(std::max)(1.0f,spacing))));
            if(samples>4096)return false;
            for(int sample=1;sample<=samples;++sample) {
                const Vec3 point=from+delta*(static_cast<float>(sample)/samples);
                status=api.support(api.supportContext,point,support);
                if(status==TerrainSupportStatus::Outside||status==TerrainSupportStatus::Invalid){lastReject=10;return false;}
                // CUnit::UpdateProc tests the root's native height/normal near
                // contact. Sphere-box footprint corners are not terrain walls.
                if(status==TerrainSupportStatus::Valid&&support.height>point.y-10&&support.normal.y<=0.6f) {
                    ++blocked;lastReject=7;return false;
                }
            }
        }
        for(int i=0;i<count;++i) {
            auto scene=static_cast<uint8_t*>(scenes[i]);if(!scene)return false;
            if(scene==ownScene)continue;
            auto object=*reinterpret_cast<uint8_t**>(scene+kSceneBackPointer);
            if(object==owner)continue;
            auto sceneTable=*reinterpret_cast<void***>(scene);
            if(!sceneTable||!sceneTable[5])return false;
            const bool terrain=reinterpret_cast<SceneIsAFn>(sceneTable[5])(scene,3);
            const bool solid=reinterpret_cast<SceneIsAFn>(sceneTable[5])(scene,2);
            if(object) {
                auto table=*reinterpret_cast<void***>(object);if(!table||!table[1])return false;
                auto isA=reinterpret_cast<ObjectIsAFn>(table[1]);
                if(isA(object,0x22)) {
                    const Aabb zone=*reinterpret_cast<Aabb*>(scene+kSceneAabb);
                    if(!ValidAabb(zone))return false;
                    const Vec3 hazardHalf=(hazardStart.max-hazardStart.min)*0.5f;
                    const Vec3 hazardCenter=(hazardStart.min+hazardStart.max)*0.5f+delta*0.5f;
                    const Aabb expanded={zone.min-hazardHalf,zone.max+hazardHalf};
                    if(api.segmentAabb(&hazardCenter,&halfDelta,&expanded)&&
                       !(Intersects(hazardStart,zone)&&!Intersects(hazardFinish,zone))){++blocked;lastReject=5;return false;}
                    continue;
                }
                // Native actors move independently. Destructible blockers are
                // route actions: the existing live gate observer must breach
                // them before the native body can pass, never remove them here.
                if(isA(object,7)||isA(object,0x15)||isA(object,9)||isA(object,0x21))continue;
                if(deferDestroyables&&isA(object,kDestroyableBlockKind)) {
                    using CanAttackFn=bool(__thiscall*)(void*,void*);
                    if(*reinterpret_cast<int32_t*>(object+0x38)>0&&!object[0x1C]&&table[10]&&
                       reinterpret_cast<CanAttackFn>(table[10])(object,owner))continue;
                }
            }
            if(hazardsOnly)continue;
            if(!terrain&&!solid)continue;
            const Aabb sceneBounds=*reinterpret_cast<Aabb*>(scene+kSceneAabb);
            if(!ValidAabb(sceneBounds))return false;
            const Aabb expanded={sceneBounds.min-half,sceneBounds.max+half};
            if(!api.segmentAabb(&segmentCenter,&halfDelta,&expanded))continue;
            if(!terrain) {
                const int obbCount=*reinterpret_cast<int*>(scene+kObbCount);
                const int sphereCount=scene[kSphereEnabled]?*reinterpret_cast<int*>(scene+kSphereCount):0;
                const auto obbs=*reinterpret_cast<uint8_t**>(scene+kObbArray);
                const auto spheres=*reinterpret_cast<uint8_t**>(scene+kSphereArray);
                if(!ValidShapeArray(obbs,obbCount,kObbStride)||!ValidShapeArray(spheres,sphereCount,kSphereStride))return false;
                if(!obbCount&&!sphereCount)continue;
                // The native wall body is the first sphere. Scene AABBs are
                // broad-phase envelopes, including empty/rotated model space.
                if(sceneBounds.max.y<=low)continue;
                for(int shape=0;shape<obbCount;++shape)
                    if(!ClearObb(from,to,obbs+size_t(shape)*kObbStride)){
                        ++blocked;lastReject=6;rejectedScene=scene;rejectedObject=object;rejectedShape=static_cast<uint32_t>(shape);return false;
                    }
                for(int shape=0;shape<sphereCount;++shape)
                    if(!ClearSphere(from,to,*reinterpret_cast<MapBodySphere*>(spheres+size_t(shape)*kSphereStride)))
                        {
                            const auto& sphere=*reinterpret_cast<MapBodySphere*>(spheres+size_t(shape)*kSphereStride);
                            ++blocked;lastReject=6;rejectedScene=scene;rejectedObject=object;rejectedShape=static_cast<uint32_t>(shape);
                            shapeReject=5;rejectedSphereCenter=sphere.center;rejectedSphereRadius=sphere.radius;return false;
                        }
                continue;
            }
            // Terrain support/steep contact was checked at native root samples.
        }
        return true;
    }
    bool ClearForcedTravel(Vec3 from,Vec3 to,const Aabb& mapBounds)const {
        if(!Valid()||!Finite(from)||!Finite(to)||!detail::ValidAabb(mapBounds))return false;
        // CUnit::UpdateProc runs the native solid-contact correction after the
        // Move/Jump FSMs, including ForceMove. A walking-style no-contact sweep
        // would reject authored launch corridors which the native motor slides
        // or projects through. The terrain layout still defines the valid X/Z
        // domain, and DeathZone contact remains unsafe for the complete body.
        const auto inside=[&](Vec3 point) {
            return point.x>=mapBounds.min.x&&point.x<mapBounds.max.x&&
                   point.z>=mapBounds.min.z&&point.z<mapBounds.max.z;
        };
        if(!inside(from)||!inside(to)){lastReject=10;return false;}
        return ClearBody(from,to,true);
    }
    bool Step(Vec3 from,Vec3 desired,Vec3& grounded)const {
        if(!Valid()||!Finite(from)||!Finite(desired))return false;
        Vec3 delta=desired-from;delta.y=0;const double length=Length(delta);
        if(!std::isfinite(length)||length>1000)return false;
        const int samples=(std::max)(1,static_cast<int>(std::ceil(length/100)));
        Vec3 previous=from,landing=from;bool dropping=false;
        for(int sample=1;sample<=samples;++sample) {
            Vec3 point=from+delta*(static_cast<float>(sample)/samples),floor{};
            if(!Ground(point,previous.y+60,previous.y-4096,floor,true))return false;
            if(dropping||floor.y<previous.y-80) {
                // Walk clear of a ledge before descending. Check every possible
                // fall column for death zones, then the actual landing column.
                const Vec3 high={point.x,previous.y,point.z};
                if(!ClearBody(previous,high)||!ClearBody(high,floor,true))return false;
                previous=high;landing=floor;dropping=true;
            } else {
                if(!ClearBody(previous,floor))return false;
                previous=floor;landing=floor;
            }
        }
        if(dropping&&!ClearBody(previous,landing))return false;
        grounded=landing;return true;
    }
};
}
