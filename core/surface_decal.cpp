#include "sdgo/surface_decal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace sdgo::surface_decal {
namespace {
constexpr double epsilon = 1e-9;
constexpr double pi = 3.14159265358979323846;
Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 Mul(Vec3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }
double Dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
bool Finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
bool Normalize(Vec3 a, Vec3& out) {
    const double length = std::hypot(a.x, a.y, a.z);
    if (!std::isfinite(length) || length < epsilon) return false;
    out = Mul(a, 1/length); return true;
}
bool ValidLimits(const Limits& l) {
    return l.vertices && l.triangles && l.outputTriangles &&
        l.vertices <= 1048576 && l.triangles <= 1048576 &&
        l.outputTriangles <= 1048576 && std::isfinite(l.maxCoordinate) &&
        l.maxCoordinate > 0 && l.maxCoordinate <= 1e12 &&
        std::isfinite(l.minSize) && std::isfinite(l.maxSize) &&
        l.minSize >= 1e-6 && l.maxSize >= l.minSize && l.maxSize <= 1e12 &&
        std::isfinite(l.minNormalDot) && l.minNormalDot > 0 && l.minNormalDot <= 1;
}
Result ValidateMesh(const Mesh& m, const Limits& l) {
    if (!ValidLimits(l) || m.vertices.empty() || m.triangles.empty()) return Result::invalidMesh;
    if (m.vertices.size()>l.vertices || m.triangles.size()>l.triangles) return Result::budgetExceeded;
    for (const auto v : m.vertices)
        if (!Finite(v) || std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z)})>l.maxCoordinate)
            return Result::invalidMesh;
    for (const auto& t : m.triangles)
        for (auto v : t.vertex) if (v >= m.vertices.size()) return Result::invalidMesh;
    return Result::ok;
}
bool Barycentric(const std::array<double,3>& b) {
    for (double x : b) if (!std::isfinite(x) || x < 0 || x > 1) return false;
    return std::abs(b[0]+b[1]+b[2]-1) <= epsilon;
}
Vec3 Position(const Mesh& m, std::uint32_t triangle, const std::array<double,3>& b) {
    const auto& t = m.triangles[triangle];
    return Add(Add(Mul(m.vertices[t.vertex[0]],b[0]),Mul(m.vertices[t.vertex[1]],b[1])),
               Mul(m.vertices[t.vertex[2]],b[2]));
}
bool Normal(const Mesh& m, std::uint32_t triangle, Vec3& n) {
    const auto& t = m.triangles[triangle];
    return Normalize(Cross(Sub(m.vertices[t.vertex[1]],m.vertices[t.vertex[0]]),
                           Sub(m.vertices[t.vertex[2]],m.vertices[t.vertex[0]])),n);
}
struct Projected {
    Vertex vertex;
    double u = 0, v = 0, z = 0;
};
Projected Interpolate(const Projected& a, const Projected& b, double t) {
    Projected out;
    out.u = a.u+(b.u-a.u)*t; out.v = a.v+(b.v-a.v)*t; out.z = a.z+(b.z-a.z)*t;
    out.vertex.position = Add(a.vertex.position,Mul(Sub(b.vertex.position,a.vertex.position),t));
    out.vertex.triangle = a.vertex.triangle;
    for (std::size_t i=0;i<3;++i)
        out.vertex.barycentric[i] = a.vertex.barycentric[i]+(b.vertex.barycentric[i]-a.vertex.barycentric[i])*t;
    return out;
}
double Distance(const Projected& p, unsigned plane, double depth) {
    switch (plane) {
    case 0: return p.u;
    case 1: return 1-p.u;
    case 2: return p.v;
    case 3: return 1-p.v;
    case 4: return depth-p.z;
    default: return depth+p.z;
    }
}
void Clip(std::vector<Projected>& polygon, unsigned plane, double depth) {
    if (polygon.empty()) return;
    std::vector<Projected> out;
    out.reserve(polygon.size()+1);
    auto a = polygon.back(); double da = Distance(a,plane,depth);
    for (const auto& b : polygon) {
        const double db = Distance(b,plane,depth);
        if ((da>=0)!=(db>=0)) out.push_back(Interpolate(a,b,da/(da-db)));
        if (db>=0) out.push_back(b);
        a=b; da=db;
    }
    polygon.swap(out);
}
bool EdgeInside(Projected a, Projected b, double depth) {
    for (unsigned plane=0;plane<6;++plane) {
        const double da=Distance(a,plane,depth),db=Distance(b,plane,depth);
        if (da<0 && db<0) return false;
        if ((da<0)!=(db<0)) {
            const auto cut=Interpolate(a,b,da/(da-db));
            if (da<0) a=cut; else b=cut;
        }
    }
    return true;
}
bool Same(const Placement& a, const Placement& b) {
    return a.anchor.geometry==b.anchor.geometry && a.anchor.node==b.anchor.node &&
        a.anchor.triangle==b.anchor.triangle && a.anchor.barycentric==b.anchor.barycentric &&
        a.anchor.tangent.x==b.anchor.tangent.x && a.anchor.tangent.y==b.anchor.tangent.y &&
        a.anchor.tangent.z==b.anchor.tangent.z && a.width==b.width && a.height==b.height &&
        a.rotation==b.rotation && a.depth==b.depth;
}
bool SameUnit(const Appearance& a, const Appearance& b) {
    return a.unitSerial==b.unitSerial && a.unitType==b.unitType;
}
bool NewAuthority(const Appearance& saved, const Appearance& authority) {
    return Valid(authority) && SameUnit(saved,authority) &&
        (authority.revision>saved.revision ||
         (authority.revision==saved.revision && SameContent(saved,authority)));
}
} // namespace

bool Valid(const Placement& p, const Limits& l) {
    return ValidLimits(l) && p.anchor.geometry && Barycentric(p.anchor.barycentric) &&
        Finite(p.anchor.tangent) && std::abs(std::hypot(p.anchor.tangent.x,p.anchor.tangent.y,p.anchor.tangent.z)-1)<1e-6 &&
        std::isfinite(p.width) && p.width>=l.minSize && p.width<=l.maxSize &&
        std::isfinite(p.height) && p.height>=l.minSize && p.height<=l.maxSize &&
        std::isfinite(p.depth) && p.depth>=l.minSize && p.depth<=l.maxSize &&
        std::isfinite(p.rotation) && p.rotation>=-pi && p.rotation<=pi;
}

Result Pick(const Mesh& m, const Ray& ray, Hit& output, const Limits& l) {
    const auto valid = ValidateMesh(m,l);
    if (valid!=Result::ok) return valid;
    Vec3 direction;
    if (!Finite(ray.origin) || !Normalize(ray.direction,direction)) return Result::invalidRay;
    Hit best; best.distance=std::numeric_limits<double>::infinity();
    bool found=false;
    for (std::uint32_t i=0;i<m.triangles.size();++i) {
        const auto& t=m.triangles[i];
        const Vec3 a=m.vertices[t.vertex[0]], e1=Sub(m.vertices[t.vertex[1]],a), e2=Sub(m.vertices[t.vertex[2]],a);
        const Vec3 p=Cross(direction,e2);
        const double determinant=Dot(e1,p);
        // Match front-facing native model triangles; do not select the far
        // side of an open shell by accepting its back face.
        if (determinant<=epsilon) continue;
        const Vec3 delta=Sub(ray.origin,a);
        const double u=Dot(delta,p)/determinant;
        const Vec3 q=Cross(delta,e1);
        const double v=Dot(direction,q)/determinant, distance=Dot(e2,q)/determinant;
        if (u<0 || v<0 || u+v>1 || distance<0 || !std::isfinite(distance) || distance>=best.distance) continue;
        best={i,{{1-u-v,u,v}},distance}; found=true;
    }
    if (!found) return Result::miss;
    output=best; return Result::ok;
}

Result MakePlacement(const Mesh& m, const Hit& hit, std::uint64_t geometry,
                     std::uint32_t node, Vec3 preferredTangent,
                     double width, double height, double depth,
                     Placement& output, const Limits& l) {
    const auto valid=ValidateMesh(m,l);
    if (valid!=Result::ok) return valid;
    if (hit.triangle>=m.triangles.size() || !Barycentric(hit.barycentric) || !Finite(preferredTangent))
        return Result::invalidPlacement;
    Vec3 normal,tangent;
    if (!Normal(m,hit.triangle,normal)) return Result::invalidPlacement;
    if (!Normalize(Sub(preferredTangent,Mul(normal,Dot(preferredTangent,normal))),tangent)) {
        const Vec3 fallback=std::abs(normal.x)<0.8?Vec3{1,0,0}:Vec3{0,1,0};
        if (!Normalize(Sub(fallback,Mul(normal,Dot(fallback,normal))),tangent)) return Result::invalidPlacement;
    }
    Placement result{{geometry,node,hit.triangle,hit.barycentric,tangent},width,height,0,depth};
    if (!Valid(result,l)) return Result::invalidPlacement;
    output=result; return Result::ok;
}

Result Project(const Mesh& m, std::uint64_t geometry, std::uint32_t node,
               const Placement& p, Patch& output, const Limits& l) {
    const auto valid=ValidateMesh(m,l);
    if (valid!=Result::ok) return valid;
    if (!Valid(p,l) || p.anchor.triangle>=m.triangles.size()) return Result::invalidPlacement;
    if (p.anchor.geometry!=geometry || p.anchor.node!=node) return Result::wrongGeometry;
    Vec3 normal,tangent;
    if (!Normal(m,p.anchor.triangle,normal) ||
        !Normalize(Sub(p.anchor.tangent,Mul(normal,Dot(p.anchor.tangent,normal))),tangent))
        return Result::invalidPlacement;
    const Vec3 center=Position(m,p.anchor.triangle,p.anchor.barycentric);
    const Vec3 bitangent=Cross(normal,tangent);
    const double c=std::cos(p.rotation),s=std::sin(p.rotation);
    const Vec3 right=Add(Mul(tangent,c),Mul(bitangent,s));
    const Vec3 up=Sub(Mul(bitangent,c),Mul(tangent,s));

    // Traverse shared edges from the hit triangle, only through triangles
    // reached by the projected volume. Disconnected overlapping armor and
    // surfaces reached only around the back of a part cannot receive copies.
    using Edge=std::pair<std::uint32_t,std::uint32_t>;
    std::map<Edge,std::vector<std::uint32_t>> edges;
    for (std::uint32_t i=0;i<m.triangles.size();++i)
        for (std::size_t j=0;j<3;++j) {
            const auto a=m.triangles[i].vertex[j],b=m.triangles[i].vertex[(j+1)%3];
            edges[{std::min(a,b),std::max(a,b)}].push_back(i);
        }
    std::vector<std::uint8_t> seen(m.triangles.size(),0);
    std::vector<std::uint32_t> queue{p.anchor.triangle}; seen[p.anchor.triangle]=1;
    Patch result;
    for (std::size_t q=0;q<queue.size();++q) {
        const auto id=queue[q]; Vec3 faceNormal;
        if (!Normal(m,id,faceNormal) || Dot(normal,faceNormal)<l.minNormalDot) continue;
        const auto& face=m.triangles[id];
        std::vector<Projected> polygon;
        for (std::size_t j=0;j<3;++j) {
            const Vec3 position=m.vertices[face.vertex[j]],delta=Sub(position,center);
            Projected vertex; vertex.vertex.position=position; vertex.vertex.triangle=id;
            vertex.vertex.barycentric[j]=1;
            // Native D3D views are left-handed. The front-facing triangle
            // basis (normal x tangent) has screen-left as its horizontal
            // axis; increasing U along it mirrors lettering. Reverse only
            // U, preserving anchors, clipping, rotation and persisted data.
            vertex.u=0.5-Dot(delta,right)/p.width;
            vertex.v=0.5-Dot(delta,up)/p.height;
            vertex.z=Dot(delta,normal); polygon.push_back(vertex);
        }
        const auto original=polygon;
        for (unsigned plane=0;plane<6;++plane) Clip(polygon,plane,p.depth);
        if (polygon.size()<3) continue;
        for (std::size_t j=1;j+1<polygon.size();++j) {
            const std::array<Projected,3> triangle{{polygon[0],polygon[j],polygon[j+1]}};
            Vec3 unused;
            if (!Normalize(Cross(Sub(triangle[1].vertex.position,triangle[0].vertex.position),
                                 Sub(triangle[2].vertex.position,triangle[0].vertex.position)),unused)) continue;
            if (result.vertices.size()/3>=l.outputTriangles) return Result::budgetExceeded;
            for (const auto& v:triangle) {
                auto vertex=v.vertex;
                vertex.uv={std::clamp(v.u,0.0,1.0),std::clamp(v.v,0.0,1.0)};
                result.vertices.push_back(vertex);
            }
        }
        for (std::size_t j=0;j<3;++j) {
            if (!EdgeInside(original[j],original[(j+1)%3],p.depth)) continue;
            const auto a=face.vertex[j],b=face.vertex[(j+1)%3];
            const auto& adjacent=edges.at({std::min(a,b),std::max(a,b)});
            // Ambiguous/non-manifold seams are not a safe continuation.
            if (adjacent.size()!=2) continue;
            for (auto next:adjacent) if (!seen[next]) {seen[next]=1;queue.push_back(next);}
        }
    }
    if (result.vertices.empty()) return Result::empty;
    output=std::move(result); return Result::ok;
}

bool Valid(const Appearance& a, const Limits& l) {
    if (!a.unitSerial || !a.unitType || !ValidLimits(l)) return false;
    for (const auto& slot:a.slots)
        if (slot.positioned && (!slot.sticker || !Valid(slot.placement,l))) return false;
    return true;
}
bool SameContent(const Appearance& a, const Appearance& b) {
    if (!SameUnit(a,b)) return false;
    for (std::size_t i=0;i<a.slots.size();++i) {
        const auto& x=a.slots[i]; const auto& y=b.slots[i];
        if (x.sticker!=y.sticker || x.positioned!=y.positioned ||
            (x.positioned&&!Same(x.placement,y.placement))) return false;
    }
    return true;
}
bool Editor::Open(const Appearance& a) {
    if (state_==EditState::awaitingReply || state_==EditState::uncertain ||
        (state_==EditState::editing&&Dirty()) || !Valid(a)) return false;
    saved_=draft_=a; state_=EditState::editing; request_=0; return true;
}
bool Editor::Place(std::size_t slot, const Placement& p) {
    if (state_!=EditState::editing || slot>=draft_.slots.size() ||
        !draft_.slots[slot].sticker || !Valid(p)) return false;
    draft_.slots[slot].placement=p; draft_.slots[slot].positioned=true; return true;
}
bool Editor::ResetPosition(std::size_t slot) {
    if (state_!=EditState::editing || slot>=draft_.slots.size()) return false;
    draft_.slots[slot].positioned=false; draft_.slots[slot].placement={}; return true;
}
bool Editor::Cancel() {
    if (state_!=EditState::editing) return false;
    draft_=saved_; state_=EditState::closed; return true;
}
bool Editor::BeginSave(std::uint64_t request, Appearance& proposal) {
    if (state_!=EditState::editing || !Dirty() || !request || request<=lastRequest_ || !Valid(draft_)) return false;
    proposal=draft_; request_=lastRequest_=request; state_=EditState::awaitingReply; return true;
}
bool Editor::Acknowledge(std::uint64_t request, Reply reply, const Appearance& authority) {
    if ((state_!=EditState::awaitingReply&&state_!=EditState::uncertain) || request!=request_ ||
        !NewAuthority(saved_,authority)) return false;
    if (reply==Reply::saved) {
        if (authority.revision<=saved_.revision || !SameContent(draft_,authority)) return false;
    } else if (reply!=Reply::rejected && reply!=Reply::conflict) return false;
    saved_=draft_=authority; request_=0; state_=EditState::editing; return true;
}
void Editor::Timeout() {
    if (state_==EditState::awaitingReply) state_=EditState::uncertain;
}
bool Editor::Reconcile(const Appearance& authority) {
    if (state_!=EditState::uncertain || !NewAuthority(saved_,authority)) return false;
    saved_=draft_=authority; request_=0; state_=EditState::editing; return true;
}
} // namespace sdgo::surface_decal
