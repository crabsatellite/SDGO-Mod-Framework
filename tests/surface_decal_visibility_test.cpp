#include "../mods/unit_appearance_client/decal/visibility.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace sdgo::surface_decal;
namespace {
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
Patch PatchOn(std::initializer_list<unsigned> triangles){
    Patch p;for(auto triangle:triangles)for(unsigned i=0;i<3;++i){Vertex v;v.triangle=triangle;p.vertices.push_back(v);}return p;
}
}
int main(){
    Check(render::OpaqueNode(false,0x10),"reflective head armor remains eligible in battle");
    Check(!render::OpaqueNode(true,0x10),"hidden reflective armor remains hidden");
    for(unsigned flag:{1u,4u,8u,64u})
        Check(!render::OpaqueNode(false,0x10|flag),"reflection does not bypass depth, blend or alpha overrides");
    const std::vector<std::uint32_t> groups{2,0,2,1};
    std::array<float,3> alpha{1.f,.2f,1.f};
    std::array<unsigned,3> reads{};
    auto read=[&](unsigned group,float& out){++reads[group];out=alpha[group];return true;};
    Check(render::OpaqueCoverage(PatchOn({0,1,2}),groups,3,read),
          "an unrelated transparent lens cannot send an opaque sticker back to its native slot");
    Check(reads[0]==1&&reads[1]==0&&reads[2]==1,"only covered groups are checked, once per group");
    Check(!render::OpaqueCoverage(PatchOn({0,3}),groups,3,read),"a patch that actually crosses a transparent group retains native rendering");
    alpha[2]=.2f;
    Check(!render::OpaqueCoverage(PatchOn({0}),groups,3,read),"fade or cloak alpha on the covered surface is still rejected");
    alpha[2]=std::numeric_limits<float>::quiet_NaN();
    Check(!render::OpaqueCoverage(PatchOn({0}),groups,3,read),"invalid covered opacity is rejected");
    alpha[2]=1.f;
    Check(!render::OpaqueCoverage(PatchOn({0}),groups,3,[](unsigned,float&){return false;}),"missing covered materials retain native rendering");
    Check(!render::OpaqueCoverage(PatchOn({4}),groups,3,read),"stale triangle indices cannot index the native group array");
    Check(!render::OpaqueCoverage(PatchOn({0}),groups,2,read),"stale group indices are rejected");
    Check(!render::OpaqueCoverage({},groups,3,read),"an empty projection cannot suppress a native sticker");
    auto partial=PatchOn({0});partial.vertices.pop_back();
    Check(!render::OpaqueCoverage(partial,groups,3,read),"incomplete triangles cannot suppress native rendering");
    Check(!render::OpaqueCoverage(PatchOn({0}),groups,257,read),"native group reads stay bounded");
    std::cout<<checks<<" checks passed\n";
}
