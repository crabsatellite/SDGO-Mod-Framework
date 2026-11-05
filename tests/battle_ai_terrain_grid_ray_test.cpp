#include "../mods/battle_ai/terrain_grid_ray.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {
using battle_ai::TerrainGridRay;
using battle_ai::Vec3;
constexpr std::size_t kStride=0x2C;

struct Grid {
    std::uint32_t width=64,height=48;
    Vec3 base={-140.0f,0,75.0f};
    Vec3 column={2.4f,0,0.85f};
    Vec3 row={-0.65f,0,1.8f};
    std::vector<std::uint8_t> bytes;
    Grid() : bytes((std::size_t(width)+1)*(height+1)*kStride,0xA5) {
        for(std::uint32_t z=0;z<=height;++z)for(std::uint32_t x=0;x<=width;++x) {
            Vec3 point={
                base.x+column.x*x+row.x*z,
                float(18.0*std::sin(double(x)*0.19)+11.0*std::cos(double(z)*0.23)+0.025*x*z),
                base.z+column.z*x+row.z*z};
            std::memcpy(bytes.data()+(std::size_t(z)*(width+1)+x)*kStride,&point,sizeof(point));
        }
    }
    const Vec3* Vertex(std::uint32_t x,std::uint32_t z)const {
        return reinterpret_cast<const Vec3*>(bytes.data()+(std::size_t(z)*(width+1)+x)*kStride);
    }
    Vec3 Point(double x,double y,double z)const {
        return {float(base.x+column.x*x+row.x*z),float(y),
                float(base.z+column.z*x+row.z*z)};
    }
    Vec3 Direction(double x,double y,double z)const {
        return battle_ai::Normalize(
            {float(column.x*x+row.x*z),float(y),float(column.z*x+row.z*z)});
    }
};

struct RayContext {
    Vec3 origin{},direction{};
    const std::uint8_t* base=nullptr;
    std::size_t bytes=0,stride=0;
    std::uint64_t calls=0,pointerErrors=0;
};
Vec3 Cross(Vec3 a,Vec3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float DotFloat(Vec3 a,Vec3 b) {
    return a.x*b.x+a.y*b.y+a.z*b.z;
}
bool Moller(void* opaque,const Vec3* a,const Vec3* b,const Vec3* c,float* distance) {
    auto& ray=*static_cast<RayContext*>(opaque);++ray.calls;
    for(const Vec3* point:{a,b,c}) {
        const auto address=reinterpret_cast<const std::uint8_t*>(point);
        if(address<ray.base||address>=ray.base+ray.bytes||
           std::size_t(address-ray.base)%ray.stride!=0)
            ++ray.pointerErrors;
    }
    const Vec3 edge1=*b-*a,edge2=*c-*a;
    const Vec3 p=Cross(ray.direction,edge2);
    const float determinant=DotFloat(edge1,p);
    if(std::abs(determinant)<1.0e-4f)return false;
    const float inverse=1.0f/determinant;
    const Vec3 translated=ray.origin-*a;
    const float u=DotFloat(translated,p)*inverse;
    if(u<0||u>1)return false;
    const Vec3 q=Cross(translated,edge1);
    const float v=DotFloat(ray.direction,q)*inverse;
    if(v<0||u+v>1)return false;
    *distance=DotFloat(edge2,q)*inverse;
    return true;
}
bool Brute(const Grid& grid,RayContext& ray,float& distance,std::uint64_t& tested) {
    float closest=100000000.0f;bool hit=false;tested=0;
    const auto test=[&](const Vec3* a,const Vec3* b,const Vec3* c) {
        float candidate=0;++tested;
        if(Moller(&ray,a,b,c,&candidate)&&candidate>0&&candidate<closest) {
            closest=candidate;hit=true;
        }
    };
    for(std::uint32_t z=0;z<grid.height;++z)for(std::uint32_t x=0;x<grid.width;++x) {
        const Vec3* p00=grid.Vertex(x,z);
        const Vec3* p01=grid.Vertex(x+1,z);
        const Vec3* p10=grid.Vertex(x,z+1);
        const Vec3* p11=grid.Vertex(x+1,z+1);
        test(p00,p01,p10);test(p01,p10,p11);
    }
    distance=closest;return hit;
}
bool Same(bool acceleratedHit,float acceleratedDistance,bool bruteHit,float bruteDistance) {
    if(acceleratedHit!=bruteHit)return false;
    if(!acceleratedHit)return acceleratedDistance==100000000.0f;
    const float tolerance=(std::max)(1.0e-4f,std::abs(bruteDistance)*2.0e-5f);
    return std::abs(acceleratedDistance-bruteDistance)<=tolerance;
}
}

int main() {
    int errors=0,checks=0;
    const auto check=[&](bool ok,const char* label) {
        ++checks;if(!ok){++errors;std::printf("FAIL %d: %s\n",checks,label);}
    };
    Grid grid;TerrainGridRay accelerated;
    check(accelerated.Initialize(grid.bytes.data(),kStride,grid.width,grid.height),"valid affine grid");
    TerrainGridRay invalid;
    check(!invalid.Initialize(nullptr,kStride,grid.width,grid.height),"null vertices rejected");
    check(!invalid.Initialize(grid.bytes.data(),sizeof(Vec3)-1,grid.width,grid.height),"short stride rejected");
    check(!invalid.Initialize(grid.bytes.data(),kStride,0,grid.height),"zero width rejected");
    check(!invalid.Initialize(grid.bytes.data(),kStride,5000,1),"oversized axis rejected");
    auto damaged=grid.bytes;
    auto corrupt=[&](std::uint32_t x,std::uint32_t z,Vec3 value) {
        std::memcpy(damaged.data()+(std::size_t(z)*(grid.width+1)+x)*kStride,&value,sizeof(value));
    };
    Vec3 changed=*grid.Vertex(20,15);changed.x+=0.1f;corrupt(20,15,changed);
    check(!invalid.Initialize(damaged.data(),kStride,grid.width,grid.height),"non-affine XZ rejected");
    damaged=grid.bytes;changed=*grid.Vertex(20,15);changed.y=std::numeric_limits<float>::quiet_NaN();
    corrupt(20,15,changed);
    check(!invalid.Initialize(damaged.data(),kStride,grid.width,grid.height),"non-finite height rejected");
    damaged=grid.bytes;
    for(std::uint32_t z=0;z<=grid.height;++z)for(std::uint32_t x=0;x<=grid.width;++x) {
        Vec3 point=*grid.Vertex(x,z);point.x=grid.base.x+x;point.z=grid.base.z;
        corrupt(x,z,point);
    }
    check(!invalid.Initialize(damaged.data(),kStride,grid.width,grid.height),"degenerate grid rejected");

    Grid edgeGrid=grid;
    Vec3 outward=*edgeGrid.Vertex(0,20);
    outward.x-=grid.column.x*0.0008f;
    outward.z-=grid.column.z*0.0008f;
    std::memcpy(edgeGrid.bytes.data()+std::size_t(20)*(grid.width+1)*kStride,
                &outward,sizeof(outward));
    TerrainGridRay edgeAccelerated;
    check(edgeAccelerated.Initialize(edgeGrid.bytes.data(),kStride,edgeGrid.width,edgeGrid.height),
          "bounded outer-edge deviation accepted");
    RayContext edgeRay{grid.Point(-0.0004,100,20.2),grid.Direction(0,-1,0),
                       edgeGrid.bytes.data(),edgeGrid.bytes.size(),kStride};
    RayContext edgeBrute=edgeRay;
    float edgeDistance=0,edgeBruteDistance=0;
    std::uint64_t edgeTested=0,edgeBruteTested=0;
    const bool edgeHit=edgeAccelerated.Intersect(
        edgeRay.origin,edgeRay.direction,&Moller,&edgeRay,edgeDistance,edgeTested);
    const bool edgeReference=Brute(edgeGrid,edgeBrute,edgeBruteDistance,edgeBruteTested);
    check(edgeReference&&Same(edgeHit,edgeDistance,edgeReference,edgeBruteDistance),
          "outer-edge padding preserves native triangle hit");

    std::vector<std::pair<Vec3,Vec3>> rays={
        {grid.Point(20.25,130,17.75),grid.Direction(0,-1,0)},
        {grid.Point(20.25,-130,17.75),grid.Direction(0,1,0)},
        {grid.Point(-8,20,12),grid.Direction(1,0,0)},
        {grid.Point(-8,40,12),grid.Direction(1,-0.2,0)},
        {grid.Point(-8,30,-8),grid.Direction(1,-0.1,1)},
        {grid.Point(12,120,-10),grid.Direction(0.2,-1,1)},
        {grid.Point(32,80,24),grid.Direction(0,-1,0)},
        {grid.Point(32,-80,24),grid.Direction(0,1,0)},
        {grid.Point(0,60,0),grid.Direction(1,-0.3,0)},
        {grid.Point(grid.width,60,grid.height),grid.Direction(-1,-0.3,-1)},
        {*grid.Vertex(20,15),grid.Direction(0,-1,0)},
        {*grid.Vertex(20,15),grid.Direction(0,1,0)},
        {grid.Point(-5,50,-5),grid.Direction(-1,-0.2,-1)},
        {grid.Point(10,50,10),{0,0,0}},
    };
    std::mt19937 random(0x33C0111u);
    std::uniform_real_distribution<double> xz(-18.0,82.0);
    std::uniform_real_distribution<double> y(-120.0,180.0);
    std::uniform_real_distribution<double> direction(-1.0,1.0);
    for(int i=0;i<8000;++i) {
        const Vec3 origin=grid.Point(xz(random),y(random),xz(random));
        Vec3 rayDirection{};
        if(i%19==0)rayDirection=grid.Direction(0,i%38?-1:1,0);
        else if(i%23==0)rayDirection=grid.Direction(1,direction(random),0);
        else if(i%29==0)rayDirection=grid.Direction(1,direction(random),1);
        else rayDirection=grid.Direction(direction(random),direction(random),direction(random));
        rays.push_back({origin,rayDirection});
    }

    std::uint64_t acceleratedTriangles=0,bruteTriangles=0;
    for(std::size_t index=0;index<rays.size();++index) {
        RayContext acceleratedRay{rays[index].first,rays[index].second,grid.bytes.data(),grid.bytes.size(),kStride};
        RayContext bruteRay=acceleratedRay;
        float acceleratedDistance=0,bruteDistance=0;
        std::uint64_t acceleratedTested=0,bruteTested=0;
        const bool acceleratedHit=accelerated.Intersect(
            acceleratedRay.origin,acceleratedRay.direction,&Moller,&acceleratedRay,
            acceleratedDistance,acceleratedTested);
        const bool bruteHit=Brute(grid,bruteRay,bruteDistance,bruteTested);
        if(!Same(acceleratedHit,acceleratedDistance,bruteHit,bruteDistance)) {
            ++errors;
            if(errors<12)std::printf(
                "FAIL ray %zu hit=%d/%d distance=%.9g/%.9g tested=%llu/%llu\n",
                index,acceleratedHit,bruteHit,acceleratedDistance,bruteDistance,
                static_cast<unsigned long long>(acceleratedTested),
                static_cast<unsigned long long>(bruteTested));
        }
        if(acceleratedRay.pointerErrors||bruteRay.pointerErrors) {
            ++errors;
            if(errors<12)std::printf("FAIL ray %zu received copied or unaligned vertices\n",index);
        }
        if(acceleratedRay.calls!=acceleratedTested||bruteRay.calls!=bruteTested) {
            ++errors;
            if(errors<12)std::printf("FAIL ray %zu callback count mismatch\n",index);
        }
        acceleratedTriangles+=acceleratedTested;bruteTriangles+=bruteTested;
    }
    check(acceleratedTriangles*3<bruteTriangles,"DDA substantially reduces triangle callbacks");
    std::printf("%d checks and %zu rays, %d failures; triangles %llu/%llu (%.2f%%)\n",
        checks,rays.size(),errors,
        static_cast<unsigned long long>(acceleratedTriangles),
        static_cast<unsigned long long>(bruteTriangles),
        bruteTriangles?100.0*double(acceleratedTriangles)/double(bruteTriangles):0.0);
    return errors?1:0;
}
