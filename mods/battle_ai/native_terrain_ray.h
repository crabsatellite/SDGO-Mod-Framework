#pragma once
#include "native_navigation.h"
#include "native_capabilities.h"
#include "terrain_grid_ray.h"
#include <unordered_map>

namespace battle_ai::native {
using TerrainTriangleFn=bool(__cdecl*)(const Ray3*,const Vec3*,const Vec3*,const Vec3*,float*);

// The native terrain ray tests both triangles of every grid cell. The index
// narrows only which cells are submitted to that same native triangle test.
// Heights and all rays remain live; no visibility result is cached.
class TerrainRayIndex {
    struct Layout {
        uint8_t* vertices=nullptr;
        uint8_t* definition=nullptr;
        uint32_t width=0,height=0;
        Vec3 corners[4]{};
    };
    struct Entry {Layout layout{};TerrainGridRay grid;bool supported=false;unsigned audits=0;};
    std::unordered_map<void*,Entry> entries_;
    size_t cachedCells_=0;
    TerrainRayFn original_=nullptr;
    TerrainTriangleFn triangle_=nullptr;
    static bool ReadLayout(void* opaque,Layout& result) {
        // Keep standalone ray callbacks from repeating the same region query.
        // End this scope before any unguarded native collision callback.
        detail::ReadableRegionCache regions{};
        detail::ScopedReadableRegionCache scope(
            detail::g_readableRegionCache?*detail::g_readableRegionCache:regions);
        auto scene=static_cast<uint8_t*>(opaque);
        if(!detail::ReadPointer(scene,0x194,result.vertices)||!detail::ReadPointer(scene,0x1D0,result.definition)||
           !detail::Read(result.definition,0x34,result.width)||!detail::Read(result.definition,0x38,result.height)||
           !result.width||!result.height||result.width>4096||result.height>4096||
           uint64_t(result.width)*result.height>1048576)return false;
        const size_t row=size_t(result.width)+1;
        if(!detail::Readable(result.vertices,row*(size_t(result.height)+1)*0x2C))return false;
        const size_t offsets[4]={0,size_t(result.width),size_t(result.height)*row,size_t(result.height)*row+result.width};
        for(size_t i=0;i<4;++i)
            if(!detail::Read(result.vertices,offsets[i]*0x2C,result.corners[i])||!Finite(result.corners[i]))return false;
        return true;
    }
    static bool SameLayout(const Layout& a,const Layout& b) {
        if(a.vertices!=b.vertices||a.definition!=b.definition||a.width!=b.width||a.height!=b.height)return false;
        for(size_t i=0;i<4;++i)
            if(a.corners[i].x!=b.corners[i].x||a.corners[i].z!=b.corners[i].z)return false;
        return true;
    }
    struct Context {TerrainTriangleFn triangle;const Ray3* ray;};
    static bool Triangle(void* opaque,const Vec3* a,const Vec3* b,const Vec3* c,float* distance) {
        const auto& context=*static_cast<const Context*>(opaque);
        return context.triangle(context.ray,a,b,c,distance);
    }
public:
    struct Stats {uint64_t indexed=0,native=0,triangles=0,audits=0,mismatches=0,layouts=0,rejected=0;} stats;
    void Bind(TerrainRayFn original,TerrainTriangleFn triangle) {original_=original;triangle_=triangle;}
    void ClearLayouts() {entries_.clear();cachedCells_=0;}
    bool Query(void* scene,const Ray3* ray,float* distance,bool audit) {
        if(!original_)return false;
        Layout layout{};
        if(!triangle_||!ray||!distance||!Finite(ray->origin)||!Finite(ray->direction)||!ReadLayout(scene,layout)) {
            ++stats.native;return original_(scene,ray,distance);
        }
        auto found=entries_.find(scene);
        if(found==entries_.end()||!SameLayout(found->second.layout,layout)) {
            if(found!=entries_.end()) {cachedCells_-=size_t(found->second.layout.width)*found->second.layout.height;entries_.erase(found);}
            const size_t cells=size_t(layout.width)*layout.height;
            if(entries_.size()>=256||cachedCells_+cells>8*1024*1024)ClearLayouts();
            auto inserted=entries_.try_emplace(scene);
            auto& entry=inserted.first->second;entry.layout=layout;
            cachedCells_+=cells;
            entry.supported=entry.grid.Initialize(layout.vertices,0x2C,layout.width,layout.height);
            ++stats.layouts;if(!entry.supported)++stats.rejected;
            found=inserted.first;
        }
        auto& entry=found->second;
        if(!entry.supported){++stats.native;return original_(scene,ray,distance);}
        Context context{triangle_,ray};uint64_t tested=0;
        const bool hit=entry.grid.Intersect(ray->origin,ray->direction,&Triangle,&context,*distance,tested);
        ++stats.indexed;stats.triangles+=tested;
        // Developer diagnostics compare actual game rays against the original
        // full-grid query. A disagreement remains visible and uses native truth.
        if(audit&&entry.audits<4) {
            ++entry.audits;++stats.audits;float originalDistance=0;
            const bool originalHit=original_(scene,ray,&originalDistance);
            if(hit!=originalHit||(hit&&std::abs(*distance-originalDistance)>(std::max)(0.02f,std::abs(originalDistance)*0.00001f))) {
                ++stats.mismatches;entry.supported=false;*distance=originalDistance;return originalHit;
            }
        }
        return hit;
    }
};
}
