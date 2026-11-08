#pragma once
#include "native_navigation.h"
#include "native_capabilities.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace battle_ai::native {
using TerrainHeightFn=float(__thiscall*)(void*,float,float);
using TerrainNormalFn=Vec3*(__thiscall*)(void*,Vec3*,const Vec3*);

// Planning uses the exact heightmap/normal APIs used by CUnit::UpdateProc.
// Rendered triangle height is not the physics support height. Native height
// also owns an OOB cache; validate cells first and preserve its raw payload.
class TerrainSupportSampler {
    TerrainHeightFn height_=nullptr;
    TerrainNormalFn normal_=nullptr;
    struct Layout {
        uint8_t* terrain=nullptr;
        uint8_t* heights=nullptr;
        int32_t width=0,height=0;
        float spacingX=0,spacingZ=0;
        uint32_t maximum=0;
    };
    static TerrainSupportStatus ReadLayout(uint8_t* document,Layout& out) noexcept {
        uint32_t terrain=0,heights=0;
        if(!document||!detail::Read(document,0x82C0,terrain))return TerrainSupportStatus::Invalid;
        if(!terrain)return TerrainSupportStatus::Missing;
        if(!detail::ReadPointer(document,0x82C0,out.terrain))return TerrainSupportStatus::Invalid;
        if(!detail::Read(out.terrain,0x10,heights)||
           !detail::Read(out.terrain,0x24,out.width)||!detail::Read(out.terrain,0x28,out.height))return TerrainSupportStatus::Invalid;
        if(!heights&&out.width==0&&out.height==0)return TerrainSupportStatus::Missing;
        if(!detail::ReadPointer(out.terrain,0x10,out.heights)||out.width<=0||out.height<=0||out.width>4096||out.height>4096||
           !detail::Read(out.terrain,0x3C,out.spacingX)||!detail::Read(out.terrain,0x44,out.spacingZ)||
           !detail::Read(out.terrain,0xD1EC,out.maximum)||!std::isfinite(out.spacingX)||!std::isfinite(out.spacingZ)||
           out.spacingX<=0||out.spacingZ<=0)return TerrainSupportStatus::Invalid;
        const uint64_t count=uint64_t(out.width+1)*uint64_t(out.height+1);
        if(out.maximum!=count-1||!detail::Readable(out.heights,static_cast<size_t>(count*sizeof(float)))||
           !detail::Readable(out.terrain+0xD1E8,sizeof(uint32_t)))return TerrainSupportStatus::Invalid;
        return TerrainSupportStatus::Valid;
    }
    bool Invoke(uint8_t* terrain,Vec3 point,TerrainSupportSample& out)const noexcept {
#if defined(_MSC_VER) && defined(_WIN32)
        __try {
            const uint32_t prior=*reinterpret_cast<const uint32_t*>(terrain+0xD1E8);
            bool valid=false;
            __try {
                out.height=height_(terrain,point.x,point.z);
                if(std::isfinite(out.height)) {
                    point.y=out.height;
                    const auto result=normal_(terrain,&out.normal,&point);
                    const double length=Dot(out.normal,out.normal);
                    valid=result==&out.normal&&Finite(out.normal)&&std::isfinite(length)&&std::abs(length-1.0)<=0.001;
                }
            } __finally {*reinterpret_cast<uint32_t*>(terrain+0xD1E8)=prior;}
            return valid;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#else
        (void)terrain;(void)point;(void)out;return false;
#endif
    }
public:
    void Bind(TerrainHeightFn height,TerrainNormalFn normal)noexcept {height_=height;normal_=normal;}
    bool Bounds(uint8_t* document,Aabb& output)const noexcept {
        detail::ReadableRegionCache regions{};
        detail::ScopedReadableRegionCache scope(
            detail::g_readableRegionCache?*detail::g_readableRegionCache:regions);
        Layout layout{};if(ReadLayout(document,layout)!=TerrainSupportStatus::Valid)return false;
        const double x=double(layout.spacingX)*layout.width*0.5,z=double(layout.spacingZ)*layout.height*0.5;
        if(!std::isfinite(x)||!std::isfinite(z)||x<=0||z<=0||x>1000000||z>1000000)return false;
        output={{-static_cast<float>(x),-1000000,-static_cast<float>(z)},
                {static_cast<float>(x),1000000,static_cast<float>(z)}};return true;
    }
    TerrainSupportStatus Sample(uint8_t* document,Vec3 point,TerrainSupportSample& output)const noexcept {
        if(!height_||!normal_||!Finite(point))return TerrainSupportStatus::Invalid;
        // Calls outside perception/path-search scopes still validate the same
        // live regions repeatedly. Reuse only this synchronous call's region
        // checks (or its existing caller scope), never terrain values/results.
        detail::ReadableRegionCache regions{};
        detail::ScopedReadableRegionCache scope(
            detail::g_readableRegionCache?*detail::g_readableRegionCache:regions);
        Layout layout{};const auto status=ReadLayout(document,layout);
        if(status!=TerrainSupportStatus::Valid)return status;
        // The original stores both numerator and quotient as float before
        // truncating. Validate that exact cell, including rounded map edges.
        const volatile float numeratorX=static_cast<float>(double(point.x)+double(layout.spacingX)*layout.width*0.5);
        const volatile float numeratorZ=static_cast<float>(double(layout.spacingZ)*layout.height*0.5-double(point.z));
        const volatile float gx=numeratorX/layout.spacingX;
        const volatile float gz=numeratorZ/layout.spacingZ;
        if(!std::isfinite(gx)||!std::isfinite(gz)||gx<0||gz<0||gx>=layout.width||gz>=layout.height)return TerrainSupportStatus::Outside;
        const size_t x=static_cast<size_t>(gx),z=static_cast<size_t>(gz),stride=size_t(layout.width)+1;
        const size_t indices[4]={z*stride+x,z*stride+x+1,(z+1)*stride+x,(z+1)*stride+x+1};
        for(size_t index:indices) {
            float value=0;
            if(index>layout.maximum||!detail::Read(layout.heights,index*sizeof(float),value)||!std::isfinite(value))return TerrainSupportStatus::Invalid;
        }
        TerrainSupportSample sampled{};
        sampled.spacing=(std::max)(1.0f,(std::min)(25.0f,(std::min)(layout.spacingX,layout.spacingZ)*0.25f));
        if(!Invoke(layout.terrain,point,sampled))return TerrainSupportStatus::Invalid;
        output=sampled;return TerrainSupportStatus::Valid;
    }
};
}
