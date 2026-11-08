#pragma once
#include "native_capabilities.h"
#include "combat_math.h"
#include <array>
#include <algorithm>

namespace battle_ai::native {
// BFA4C0 uses the row-slab box; BF9640/BF9880 use its inverse-transform
// counterpart. They differ under shear/scale, so culling contains BOTH boxes.
// The native ray's parallel-slab epsilon also grows with tested travel length.
class NativeShapeBounds {
    struct World {Vec3 center,rows[3];float spare[3];Vec3 extents;};
    static_assert(sizeof(World)==0x48);
    struct Bounds {double q[3]{},r[3]{},radius[3]{},parallel[3]{},condition=0;};
    struct Entry {const void* address=nullptr;World world{};Bounds bounds{};bool valid=false;};
    static constexpr size_t Capacity=4096;
    std::array<Entry,Capacity> entries_{};
    bool audit_=false;
    void (*mismatchSink_)()=nullptr;
    static bool ReadWorld(const void* shape,World& world)noexcept {
#if defined(_MSC_VER) && defined(_WIN32)
        // Native shape predicates read this same live world record under the
        // caller's SEH boundary. Avoid a new VirtualQuery syscall per box.
        __try {memcpy(&world,static_cast<const uint8_t*>(shape)+0x3C,sizeof(world));return true;}
        __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#else
        return detail::Read(static_cast<const uint8_t*>(shape),0x3C,world);
#endif
    }
    static double Axis(Vec3 v,size_t i){return i==0?v.x:i==1?v.y:v.z;}
    static bool Build(const World& world,Bounds& result) {
        if(!Finite(world.center)||!Finite(world.extents))return false;
        double a[3][3]{},e[3]{};double norm=0;
        for(size_t i=0;i<3;++i) {
            if(!Finite(world.rows[i]))return false;
            e[i]=Axis(world.extents,i);if(e[i]<0)return false;
            double row=0;for(size_t j=0;j<3;++j){a[i][j]=Axis(world.rows[i],j);row+=std::abs(a[i][j]);}
            norm=(std::max)(norm,row);
        }
        const double det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-
            a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
        // Native inverse failure leaves its destination unwritten. Preserve
        // the original predicate for poorly conditioned or nonfinite layouts.
        if(!std::isfinite(det)||norm<=0||std::abs(det)<=1e-6*norm*norm*norm||
           std::abs(det)<double((std::numeric_limits<float>::min)())*256||
           std::abs(det)>double((std::numeric_limits<float>::max)())/256)return false;
        double inv[3][3]{};
        inv[0][0]=(a[1][1]*a[2][2]-a[1][2]*a[2][1])/det;
        inv[0][1]=(a[0][2]*a[2][1]-a[0][1]*a[2][2])/det;
        inv[0][2]=(a[0][1]*a[1][2]-a[0][2]*a[1][1])/det;
        inv[1][0]=(a[1][2]*a[2][0]-a[1][0]*a[2][2])/det;
        inv[1][1]=(a[0][0]*a[2][2]-a[0][2]*a[2][0])/det;
        inv[1][2]=(a[0][2]*a[1][0]-a[0][0]*a[1][2])/det;
        inv[2][0]=(a[1][0]*a[2][1]-a[1][1]*a[2][0])/det;
        inv[2][1]=(a[0][1]*a[2][0]-a[0][0]*a[2][1])/det;
        inv[2][2]=(a[0][0]*a[1][1]-a[0][1]*a[1][0])/det;
        double inverseNorm=0;
        for(size_t j=0;j<3;++j) {
            double row=0;
            for(size_t i=0;i<3;++i) {
                if(!std::isfinite(inv[j][i]))return false;
                result.q[j]+=std::abs(a[i][j])*e[i];
                result.r[j]+=std::abs(inv[j][i])*e[i];
                result.radius[j]+=std::abs(a[i][j]);
                result.parallel[j]+=std::abs(inv[j][i]);row+=std::abs(inv[j][i]);
            }
            inverseNorm=(std::max)(inverseNorm,row);
        }
        result.condition=norm*inverseNorm;
        return std::isfinite(result.condition)&&result.condition<=16;
    }
public:
    struct Stats {uint64_t tested=0,culled=0,rebuilt=0,unsupported=0,audited=0,mismatches=0;} stats;
    void SetAudit(bool enabled){audit_=enabled;}
    void SetMismatchSink(void (*sink)()){mismatchSink_=sink;}
    bool Audit()const{return audit_;}
    void Audited(bool mismatch){
        ++stats.audited;
        if(mismatch&&++stats.mismatches==1&&mismatchSink_)mismatchSink_();
    }
    bool MayAffect(const void* shape,Vec3 start,Vec3 finish,float radius=0) {
        ++stats.tested;
        if(!shape||!Finite(start)||!Finite(finish)||!std::isfinite(radius)||radius<0)return true;
        World world{};
        if(!ReadWorld(shape,world))return true;
        auto& entry=entries_[((reinterpret_cast<uintptr_t>(shape)>>2)*2654435761u)%Capacity];
        if(entry.address!=shape||memcmp(&entry.world,&world,sizeof(world))) {
            ++stats.rebuilt;
            entry={};entry.address=shape;entry.world=world;entry.valid=Build(world,entry.bounds);
        }
        if(!entry.valid){++stats.unsupported;return true;}
        const double length=Length(finish-start);if(!std::isfinite(length))return true;
        constexpr double nativeParallelEpsilon=9.999999747378752e-5;
        double enter=0,leave=1;
        for(size_t j=0;j<3;++j) {
            const double center=Axis(world.center,j),origin=Axis(start,j),end=Axis(finish,j);
            const double half=(std::max)(entry.bounds.q[j]+double(radius)*entry.bounds.radius[j],
                entry.bounds.r[j]+nativeParallelEpsilon*(1+length)*entry.bounds.parallel[j]);
            const double scale=1+std::abs(center)+std::abs(origin)+std::abs(end)+half;
            const double margin=256*double(std::numeric_limits<float>::epsilon())*scale*(1+entry.bounds.condition);
            const double low=std::nextafter(center-half-margin,-std::numeric_limits<double>::infinity());
            const double high=std::nextafter(center+half+margin,std::numeric_limits<double>::infinity());
            if(!std::isfinite(low)||!std::isfinite(high))return true;
            const double delta=end-origin;
            if(delta==0){if(origin<low||origin>high){++stats.culled;return false;}continue;}
            double nearHit=(low-origin)/delta,farHit=(high-origin)/delta;
            if(nearHit>farHit)std::swap(nearHit,farHit);
            enter=(std::max)(enter,nearHit);leave=(std::min)(leave,farHit);
            if(enter>leave){++stats.culled;return false;}
        }
        return true;
    }
};
}
