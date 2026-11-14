#include "sdgo/surface_decal_native.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>

namespace sdgo::surface_decal::native {
static_assert(sizeof(void*)==4, "The native GOnline mesh reader requires Win32");
namespace {
// GOnline-54BE1568-00C97000: CModel::ExpandVertex4Lightmap (0x00BEEB20)
// walks +0x124/+0x128, CNode::GetNodeCommon (0x00C13820) reads +0x11C,
// and the native index expansion reads common +0x34/+0x58, stride 32/2.
// Native CNode::GetWorldTM at 0x00C13530 is called through a verified symbol.
bool Copy(const void* p, std::size_t offset, void* out, std::size_t bytes) noexcept {
    if (!p || !out || !bytes || offset>std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(p)) return false;
    const auto address=reinterpret_cast<std::uintptr_t>(p)+offset;
    if (bytes>std::numeric_limits<std::uintptr_t>::max()-address) return false;
    __try {std::memcpy(out,reinterpret_cast<const void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
template<class T> bool ReadAt(const void* p,std::size_t offset,T& out) {return Copy(p,offset,&out,sizeof out);}
bool Matrix(void* node,const float*(__thiscall* world)(void*),std::array<float,16>& matrix) noexcept {
    __try {
        const auto* source=world(node);
        return Copy(source,0,matrix.data(),sizeof(float)*matrix.size());
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
struct NativeVertex {float x,y,z,nx,ny,nz,u,v;};
struct Group {std::uint32_t start,triangles,material;};
static_assert(sizeof(NativeVertex)==32 && sizeof(Group)==12);
std::uint64_t Fingerprint(const Mesh& mesh) {
    // Stable geometry identity, independent of allocation, pose and UV seams.
    // This is a cache compatibility check, not an authenticity/security digest.
    std::uint64_t hash=14695981039346656037ull;
    const auto word=[&hash](std::uint64_t v) {
        for(unsigned i=0;i<8;++i) {hash^=v&255;hash*=1099511628211ull;v>>=8;}
    };
    word(mesh.vertices.size());word(mesh.triangles.size());
    for(const auto v:mesh.vertices) for(double coordinate:{v.x,v.y,v.z}) {
        std::uint64_t bits=0;
        if(coordinate!=0)std::memcpy(&bits,&coordinate,sizeof bits);
        word(bits);
    }
    for(const auto& t:mesh.triangles)for(auto index:t.vertex)word(index);
    return hash?hash:1;
}
}
bool GeometryReader::Bind(const SdgoApi* api,SdgoMod* mod) {
    modelVtable_=modelObsVtable_=rigidMeshVtable_=nullptr;world_=nullptr;
    if(!api || api->version!=SDGO_API_VERSION || !api->Symbol)return false;
    modelVtable_=api->Symbol(mod,"surface_decal.model_vtable");
    modelObsVtable_=api->Symbol(mod,"surface_decal.model_obs_vtable");
    rigidMeshVtable_=api->Symbol(mod,"surface_decal.rigid_mesh_vtable");
    world_=reinterpret_cast<World>(api->Symbol(mod,"surface_decal.node_world"));
    return modelVtable_ && rigidMeshVtable_ && world_;
}
ReadResult GeometryReader::Read(void* model,std::vector<Node>& output) const {
    if(!modelVtable_ || !rigidMeshVtable_ || !world_)return ReadResult::unbound;
    const void* actual=nullptr;
    if(!ReadAt(model,0,actual) || !actual || (actual!=modelVtable_&&actual!=modelObsVtable_))return ReadResult::wrongModel;
    std::uintptr_t first=0,last=0,capacity=0;
    if(!ReadAt(model,0x124,first)||!ReadAt(model,0x128,last)||!ReadAt(model,0x12c,capacity)||
       last<first||capacity<last||first%4||last%4||capacity%4)return ReadResult::malformed;
    if(first==last)return ReadResult::unavailable;
    if(!first || (last-first)/4>4096)return ReadResult::budgetExceeded;
    std::vector<void*> nodes((last-first)/4);
    if(!Copy(reinterpret_cast<const void*>(first),0,nodes.data(),nodes.size()*4))return ReadResult::malformed;
    std::vector<Node> result;
    std::size_t totalVertices=0,totalTriangles=0;
    for(std::uint32_t index=0;index<nodes.size();++index) {
        void* owner=nullptr;void* common=nullptr;std::uint8_t hidden=0;
        if(!ReadAt(nodes[index],0,actual)||!ReadAt(nodes[index],0x118,owner)||owner!=model||
           !ReadAt(nodes[index],0x11c,common)||!ReadAt(nodes[index],0x108,hidden))return ReadResult::malformed;
        if(actual!=rigidMeshVtable_ || hidden)continue; // bones/hidden nodes are not paint surfaces
        std::uint32_t vertices=0,faces=0,kind=0,groupCount=0,vertexCapacity=0,indexCapacity=0;
        void* vertexData=nullptr;void* indexData=nullptr;void* groupData=nullptr;
        if(!ReadAt(common,0xa4,vertices)||!ReadAt(common,0xa8,faces)||!ReadAt(common,0x70,kind))return ReadResult::malformed;
        if(!vertices||!faces)continue;
        if(kind!=2)return ReadResult::unavailable;
        if(vertices>65536 || faces>262144 || totalVertices+vertices>262144 || totalTriangles+faces>262144)
            return ReadResult::budgetExceeded;
        if(!ReadAt(common,0x34,vertexData)||!ReadAt(common,0x38,vertexCapacity)||
           !ReadAt(common,0x58,indexData)||!ReadAt(common,0x5c,indexCapacity)||
           !ReadAt(common,0x194,groupCount)||!ReadAt(common,0x198,groupData)||
           vertexCapacity<vertices||indexCapacity<faces*3||!groupCount||groupCount>4096)return ReadResult::malformed;
        std::vector<NativeVertex> nativeVertices(vertices);
        std::vector<std::uint16_t> indices(faces*3);
        std::vector<Group> groups(groupCount);
        if(!Copy(vertexData,0,nativeVertices.data(),nativeVertices.size()*sizeof(NativeVertex))||
           !Copy(indexData,0,indices.data(),indices.size()*sizeof(std::uint16_t))||
           !Copy(groupData,0,groups.data(),groups.size()*sizeof(Group)))return ReadResult::malformed;
        Node node;node.index=index;
        if(!Matrix(nodes[index],world_,node.world))return ReadResult::malformed;
        for(float x:node.world)if(!std::isfinite(x))return ReadResult::malformed;
        node.mesh.vertices.reserve(vertices);
        std::map<std::tuple<float,float,float>,std::uint32_t> unique;
        std::vector<std::uint32_t> remap;
        remap.reserve(vertices);
        for(const auto& v:nativeVertices) {
            if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||
               std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z)})>1000000)return ReadResult::malformed;
            // Native unit meshes expand the same corner once per triangle.
            // Weld exact positions within this rigid node, independently of
            // texture seams. Non-manifold coincident edges remain blocked by
            // the projector instead of joining overlapping surface sheets.
            const auto found=unique.emplace(std::make_tuple(v.x,v.y,v.z),static_cast<std::uint32_t>(node.mesh.vertices.size()));
            if(found.second)node.mesh.vertices.push_back({v.x,v.y,v.z});
            remap.push_back(found.first->second);
        }
        std::vector<std::uint8_t> used(faces,0);
        for(std::uint32_t groupIndex=0;groupIndex<groups.size();++groupIndex) {
            const auto& group=groups[groupIndex];
            if(group.start%3||group.start>indices.size()||group.triangles>(indices.size()-group.start)/3)
                return ReadResult::malformed;
            for(std::uint32_t face=0;face<group.triangles;++face) {
                const auto offset=group.start+face*3;
                if(used[offset/3]++)return ReadResult::malformed;
                Triangle t{{indices[offset],indices[offset+1],indices[offset+2]}};
                for(auto v:t.vertex)if(v>=vertices)return ReadResult::malformed;
                // Existing native sticker meshes must not become surfaces for
                // another decal. Their textures remain owned by the native path.
                if(group.material>=7&&group.material<=9)continue;
                for(auto& v:t.vertex)v=remap[v];
                node.mesh.triangles.push_back(t);
                node.triangleGroups.push_back(groupIndex);
            }
        }
        totalVertices+=vertices;totalTriangles+=faces;
        if(node.mesh.triangles.empty())continue;
        node.geometry=Fingerprint(node.mesh);
        result.push_back(std::move(node));
    }
    if(result.empty())return ReadResult::unavailable;
    output=std::move(result);return ReadResult::ok;
}
} // namespace sdgo::surface_decal::native
