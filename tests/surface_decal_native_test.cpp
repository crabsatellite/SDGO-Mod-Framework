#include "sdgo/surface_decal_native.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

using namespace sdgo::surface_decal;
namespace {
unsigned checks=0;
void Check(bool value,const char* message) {
    ++checks;if(!value){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}
}
int modelTable=0,obsTable=0,nodeTable=0;
std::array<float,16> matrix{{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1}};
const float* __fastcall World(void*,void*) {return matrix.data();}
void* Symbol(SdgoMod*,const char* name) {
    if(!std::strcmp(name,"surface_decal.model_vtable"))return &modelTable;
    if(!std::strcmp(name,"surface_decal.model_obs_vtable"))return &obsTable;
    if(!std::strcmp(name,"surface_decal.rigid_mesh_vtable"))return &nodeTable;
    if(!std::strcmp(name,"surface_decal.node_world"))return reinterpret_cast<void*>(&World);
    return nullptr;
}
template<class T,class A>void Put(A& bytes,std::size_t offset,T value) {std::memcpy(bytes.data()+offset,&value,sizeof value);}
struct Fixture {
    std::array<std::uint8_t,0x400> model{},node{},common{};
    std::array<void*,1> nodes{{node.data()}};
    std::array<std::array<float,8>,6> vertices{{
        {{-1,-1,0,0,0,1,0,0}},{{1,-1,0,0,0,1,1,0}},{{0,1,0,0,0,1,0.5f,1}},
        {{-1,-1,0.1f,0,0,1,0,0}},{{1,-1,0.1f,0,0,1,1,0}},{{0,1,0.1f,0,0,1,0.5f,1}}
    }};
    std::array<std::uint16_t,6> indices{{0,1,2,3,4,5}};
    std::array<std::uint32_t,6> groups{{0,1,0,3,1,7}};
    Fixture() {
        Put(model,0,&modelTable);Put(model,0x124,nodes.data());Put(model,0x128,nodes.data()+1);Put(model,0x12c,nodes.data()+1);
        Put(node,0,&nodeTable);Put(node,0x118,model.data());Put(node,0x11c,common.data());
        Put(common,0x34,vertices.data());Put(common,0x38,6u);
        Put(common,0x58,indices.data());Put(common,0x5c,6u);
        Put(common,0x70,2u);Put(common,0xa4,6u);Put(common,0xa8,2u);
        Put(common,0x194,2u);Put(common,0x198,groups.data());
    }
};
}
int main() {
    native::GeometryReader reader;std::vector<native::Node> nodes;
    Fixture f;
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::unbound,"unbound reader never touches native pointers");
    SdgoApi api{};api.version=1;api.Symbol=Symbol;
    Check(reader.Bind(&api,nullptr),"bind verified symbols");
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::ok,"read native-shaped geometry");
    Check(nodes.size()==1&&nodes[0].mesh.triangles.size()==1,"fixed decal material group excluded from paint surfaces");
    Check(nodes[0].triangleGroups==std::vector<std::uint32_t>{0},"surface triangles retain their draw group after excluding native stickers");
    f.groups={{0,1,7,3,1,0}};
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::ok&&nodes[0].triangleGroups==std::vector<std::uint32_t>{1},
          "excluding an earlier sticker group does not shift native surface group identity");
    f.groups={{0,1,0,3,1,7}};reader.Read(f.model.data(),nodes);
    Check(nodes[0].world==matrix&&nodes[0].index==0,"native transform and stable node index retained");
    const auto identity=nodes[0].geometry;
    Put(f.model,0,&obsTable);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::ok,"verified CModelObs shares the model-node contract");
    Put(f.model,0,&modelTable);
    f.vertices[0][0]=-2;
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::ok&&nodes[0].geometry!=identity,"topology cache rejects changed vertex positions");
    f.vertices[0][0]=-1;
    f.vertices[0][6]=0.25f;matrix[12]=40;
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::ok&&nodes[0].geometry==identity,"pose and UV do not change surface identity");
    Check(nodes[0].world[12]==40,"current animation transform copied each read");
    Check(reader.Read(reinterpret_cast<void*>(1),nodes)==native::ReadResult::wrongModel,"unreadable model fails safely");
    Put(f.model,0,&nodeTable);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::wrongModel,"other native class rejected");
    Put(f.model,0,&modelTable);
    Put(f.model,0x128,reinterpret_cast<std::uintptr_t>(f.nodes.data())+1);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"misaligned node vector rejected");
    Put(f.model,0x128,f.nodes.data()+1);
    Put(f.node,0x118,reinterpret_cast<void*>(1));
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"node must belong to selected model");
    Put(f.node,0x118,f.model.data());Put<std::uint8_t>(f.node,0x108,1);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::unavailable,"hidden node not selectable");
    Put<std::uint8_t>(f.node,0x108,0);
    Put(f.common,0x38,5u);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"vertex count cannot exceed allocation");
    Put(f.common,0x38,6u);Put(f.common,0x5c,5u);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"index count cannot exceed allocation");
    Put(f.common,0x5c,6u);f.groups[3]=0;
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"overlapping draw groups rejected");
    f.groups[3]=3;f.groups[4]=2;
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"draw group past index buffer rejected");
    f.groups[4]=1;f.indices[1]=65535;
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"triangle indices cannot escape vertex buffer");
    f.indices[1]=1;Put(f.common,0x34,reinterpret_cast<void*>(1));
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"unreadable vertex buffer fails safely");
    Put(f.common,0x34,f.vertices.data());matrix[0]=std::numeric_limits<float>::infinity();
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::malformed,"invalid animation matrix rejected");
    matrix[0]=1;
    Check(nodes.size()==1&&nodes[0].geometry==identity,"failures leave previous output untouched");
    Put(f.common,0xa4,1000000u);
    Check(reader.Read(f.model.data(),nodes)==native::ReadResult::budgetExceeded,"native geometry allocation is bounded");
    Fixture expanded;
    expanded.vertices[2]={{1,1,0,0,0,1,1,1}};
    expanded.vertices[3]={{-1,-1,0,0,0,1,0.25f,0.25f}};
    expanded.vertices[4]={{1,1,0,0,0,1,0.75f,0.75f}};
    expanded.vertices[5]={{-1,1,0,0,0,1,0,1}};
    expanded.groups[5]=0;
    Check(reader.Read(expanded.model.data(),nodes)==native::ReadResult::ok&&nodes[0].mesh.vertices.size()==4,
          "native expanded triangle corners are welded across UV seams");
    Placement p;Patch patch;Hit hit{0,{{0.2,0.2,0.6}},0};
    Check(MakePlacement(nodes[0].mesh,hit,nodes[0].geometry,0,{1,0,0},4,4,0.5,p)==Result::ok&&
          Project(nodes[0].mesh,nodes[0].geometry,0,p,patch)==Result::ok&&patch.vertices.size()==6,
          "a projected sticker crosses adjacent expanded native faces");
    std::cout<<checks<<" checks passed\n";
}
