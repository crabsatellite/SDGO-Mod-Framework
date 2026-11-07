#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
static size_t memoryQueries=0;
static SIZE_T WINAPI CountedVirtualQuery(LPCVOID address,PMEMORY_BASIC_INFORMATION info,SIZE_T size) {
    ++memoryQueries;return ::VirtualQuery(address,info,size);
}
#define VirtualQuery CountedVirtualQuery
#include "../mods/battle_ai/native_terrain_support.h"
#undef VirtualQuery
namespace {
using namespace battle_ai;
using namespace battle_ai::native;
alignas(16) std::array<uint8_t,0x8500> document{};
alignas(16) std::array<uint8_t,0xD200> terrain{};
std::array<float,9> heights{};
int heightCalls=0,normalCalls=0;
bool throwHeight=false,nanHeight=false,nanNormal=false;
float normalScale=1;
Vec3 observed{};
template<class T>void Put(uint8_t* base,size_t offset,T value){memcpy(base+offset,&value,sizeof(value));}
uint32_t Cache(){uint32_t value;memcpy(&value,terrain.data()+0xD1E8,4);return value;}
template<class T>void* Address(T method){static_assert(sizeof(T)==sizeof(void*));void* value;memcpy(&value,&method,sizeof(value));return value;}
struct Native {
    __declspec(noinline) float Height(float x,float z) {
        ++heightCalls;Put(reinterpret_cast<uint8_t*>(this),0xD1E8,321.0f);
        if(throwHeight)RaiseException(0xE0424242,0,0,nullptr);
        return nanHeight?NAN:10+x*0.01f-z*0.02f;
    }
    __declspec(noinline) Vec3* Normal(Vec3* out,const Vec3* point) {
        ++normalCalls;observed=*point;*out=nanNormal?Vec3{NAN,1,0}:Vec3{0,normalScale,0};return out;
    }
};
void Reset() {
    document.fill(0);terrain.fill(0);heights.fill(1);
    Put(document.data(),0x82C0,terrain.data());Put(terrain.data(),0x10,heights.data());
    Put(terrain.data(),0x24,int32_t(2));Put(terrain.data(),0x28,int32_t(2));
    Put(terrain.data(),0x3C,100.0f);Put(terrain.data(),0x44,100.0f);Put(terrain.data(),0xD1EC,uint32_t(8));
    Put(terrain.data(),0xD1E8,uint32_t(0x7FC12345));
    heightCalls=normalCalls=0;throwHeight=nanHeight=nanNormal=false;normalScale=1;
}
}
int main() {
    int failures=0,checks=0;auto check=[&](bool ok,const char* text){++checks;std::printf("%s %s\n",ok?"PASS":"FAIL",text);if(!ok)++failures;};
    TerrainSupportSampler sampler;
    sampler.Bind(reinterpret_cast<TerrainHeightFn>(Address(&Native::Height)),reinterpret_cast<TerrainNormalFn>(Address(&Native::Normal)));
    Reset();TerrainSupportSample out{};
    check(sampler.Sample(document.data(),{25,999,-25},out)==TerrainSupportStatus::Valid&&std::abs(out.height-10.75f)<0.001f&&
          observed.x==25&&observed.y==out.height&&observed.z==-25&&out.normal.y==1,
          "native thiscall height and normal use supported root coordinates");
    check(Cache()==0x7FC12345&&heightCalls==1&&normalCalls==1,"a valid query restores the exact native cache payload including NaN bits");
    Reset();memoryQueries=0;
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Valid&&memoryQueries>0&&memoryQueries<=3,
          "a standalone native terrain sample validates each live memory region once");
    std::printf("standalone sample memory queries=%zu\n",memoryQueries);
    Reset();memoryQueries=0;Aabb bounds{};
    check(sampler.Bounds(document.data(),bounds)&&memoryQueries>0&&memoryQueries<=3,
          "standalone terrain bounds validation avoids repeated region syscalls");
    std::printf("standalone bounds memory queries=%zu\n",memoryQueries);
    Reset();
    {
        detail::ReadableRegionCache parent{};detail::ScopedReadableRegionCache scope(parent);
        sampler.Sample(document.data(),{},out);memoryQueries=0;
        check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Valid&&memoryQueries==0,
              "terrain sampling retains an existing synchronous caller cache");
    }
    check(detail::g_readableRegionCache==nullptr,"standalone and nested terrain calls restore the caller cache scope");
    Reset();sampler.Bounds(document.data(),bounds);Put(terrain.data(),0x3C,50.0f);
    check(sampler.Bounds(document.data(),bounds)&&bounds.max.x==50&&
          sampler.Sample(document.data(),{75,0,0},out)==TerrainSupportStatus::Outside&&heightCalls==0,
          "terrain dimensions and support limits remain live between calls");
    Reset();auto page=static_cast<float*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    check(page!=nullptr,"terrain protection fixture owns a separate memory region");
    if(page) {
        for(size_t i=0;i<9;++i)page[i]=1;
        Put(terrain.data(),0x10,page);
        check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Valid,"a readable terrain page reaches the native sampler");
        DWORD prior=0;check(VirtualProtect(page,4096,PAGE_NOACCESS,&prior)!=0,"terrain page becomes unreadable between sampling calls");
        const int before=heightCalls;
        check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&heightCalls==before,
              "a later sampling call rejects changed memory protection before native execution");
        DWORD ignored=0;VirtualProtect(page,4096,PAGE_READWRITE|PAGE_GUARD,&ignored);
        check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&heightCalls==before,
              "guarded terrain remains unavailable to the native sampler");
        MEMORY_BASIC_INFORMATION info{};VirtualQuery(page,&info,sizeof(info));
        check((info.Protect&PAGE_GUARD)!=0,"terrain validation preserves the one-shot page guard");
        VirtualProtect(page,4096,prior,&ignored);
        check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Valid&&heightCalls==before+1,
              "restored terrain protection is observed by the next sampling call");
        page[4]=NAN;
        check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&heightCalls==before+1,
              "sampled height values remain live after earlier successful calls");
        VirtualFree(page,0,MEM_RELEASE);
    }
    for(Vec3 point:{Vec3{-100.01f,0,0},Vec3{100,0,0},Vec3{0,0,-100},Vec3{0,0,100.01f}}) {
        Reset();check(sampler.Sample(document.data(),point,out)==TerrainSupportStatus::Outside&&heightCalls==0&&normalCalls==0,
                      "outside and exclusive upper cell boundaries never invoke native fallback or normal");
    }
    Reset();check(sampler.Sample(document.data(),{-100,0,100},out)==TerrainSupportStatus::Valid,"inclusive lower cell boundaries retain valid four-vertex storage");
    Reset();check(sampler.Sample(document.data(),{std::nextafter(100.0f,-INFINITY),0,0},out)==TerrainSupportStatus::Outside&&heightCalls==0,
                  "a world point rounded onto the native exclusive edge cannot reach unchecked normal access");
    Reset();heights[8]=NAN;
    check(sampler.Sample(document.data(),{-0.000001f,0,0},out)==TerrainSupportStatus::Invalid&&heightCalls==0,
          "native float rounding selects and validates the actual adjacent cell");
    Reset();heights[8]=NAN;
    check(sampler.Sample(document.data(),{25,0,-25},out)==TerrainSupportStatus::Invalid&&heightCalls==0,"nonfinite selected height data is rejected before native execution");
    Reset();Put(terrain.data(),0xD1EC,uint32_t(7));
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&heightCalls==0,"mismatched native vertex extent fails closed");
    Reset();Put(terrain.data(),0x24,int32_t(-1));
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&heightCalls==0,"negative native dimensions fail closed");
    Reset();Put(terrain.data(),0x44,0.0f);
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&heightCalls==0,"invalid spacing cannot reach native index arithmetic");
    Reset();nanHeight=true;
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&normalCalls==0&&Cache()==0x7FC12345,"nonfinite native height still restores the cache and cannot reach normal");
    Reset();nanNormal=true;
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&Cache()==0x7FC12345,"nonfinite native normal still preserves engine state");
    Reset();normalScale=0.7f;
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&Cache()==0x7FC12345,"a materially non-unit normal cannot alter the native slope threshold");
    Reset();throwHeight=true;
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Invalid&&Cache()==0x7FC12345&&detail::g_readableRegionCache==nullptr,
          "native execution fault restores engine state and the caller memory-check scope");
    Reset();Put(document.data(),0x82C0,uint32_t(0));
    check(sampler.Sample(document.data(),{},out)==TerrainSupportStatus::Missing&&heightCalls==0,"model-only worlds have an explicit absent-terrain state");
    std::printf("%d native support checks, %d failures\n",checks,failures);return failures?1:0;
}
