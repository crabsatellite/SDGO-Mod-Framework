#include "sdgo/surface_decal.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace sdgo::surface_decal;
namespace {
unsigned checks=0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) {std::cerr << "FAIL: " << message << '\n'; std::exit(1);}
}
bool Near(double a,double b) {return std::abs(a-b)<1e-7;}
Mesh Plane() {
    return {{{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0}},{{{{0,1,2}}},{{{0,2,3}}}}};
}
Placement Place(const Mesh& m, double width=2, double height=2) {
    Hit hit;
    Check(Pick(m,{{0,0,10},{0,0,-2}},hit)==Result::ok,"pick a front-facing surface");
    Check(Near(hit.distance,10),"non-unit rays return physical distance");
    Placement p;
    Check(MakePlacement(m,hit,123,9,{1,0,0},width,height,0.2,p)==Result::ok,"make surface placement");
    return p;
}
double Area(const Patch& p) {
    double total=0;
    for (std::size_t i=0;i<p.vertices.size();i+=3) {
        const auto a=p.vertices[i].position,b=p.vertices[i+1].position,c=p.vertices[i+2].position;
        total+=std::abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))/2;
    }
    return total;
}
void ValidatePatch(const Mesh& m,const Patch& p) {
    Check(!p.vertices.empty()&&p.vertices.size()%3==0,"patch has whole triangles");
    for (const auto& v:p.vertices) {
        Check(v.uv.x>=0&&v.uv.x<=1&&v.uv.y>=0&&v.uv.y<=1,"clipped UV remains inside texture");
        Check(v.triangle<m.triangles.size(),"source triangle retained");
        Check(Near(v.barycentric[0]+v.barycentric[1]+v.barycentric[2],1),"clipped barycentric coordinates sum to one");
        Vec3 reconstructed{};
        for (std::size_t i=0;i<3;++i) {
            const auto source=m.vertices[m.triangles[v.triangle].vertex[i]];
            reconstructed.x+=source.x*v.barycentric[i];
            reconstructed.y+=source.y*v.barycentric[i];
            reconstructed.z+=source.z*v.barycentric[i];
        }
        Check(Near(v.position.x,reconstructed.x)&&Near(v.position.y,reconstructed.y)&&Near(v.position.z,reconstructed.z),
              "clipped vertex stays on original surface");
    }
}
void Geometry() {
    auto m=Plane(); auto p=Place(m); Patch patch;
    Check(Project(m,123,9,p,patch)==Result::ok,"project across connected surface triangles");
    ValidatePatch(m,patch);
    Check(Near(Area(patch),4),"projected area matches decal rectangle");
    for (const auto& v:patch.vertices) {
        Check(Near(v.uv.x,0.5-v.position.x/2)&&Near(v.uv.y,0.5-v.position.y/2),"native left-handed texture orientation preserves lettering");
    }
    p.width=1;p.height=2;p.rotation=3.14159265358979323846/2;
    Check(Project(m,123,9,p,patch)==Result::ok,"rotate and scale");
    ValidatePatch(m,patch);
    Check(Near(Area(patch),2),"rotation preserves decal area");
    for (const auto& v:patch.vertices) {
        Check(std::abs(v.position.x)<=1.0000001&&std::abs(v.position.y)<=0.5000001,"rotated rectangle has expected extents");
    }
    const auto sentinel=patch.vertices.size();
    Check(Project(m,124,9,p,patch)==Result::wrongGeometry&&patch.vertices.size()==sentinel,"model replacement cannot reuse stale triangle anchor");
    Check(Project(m,123,10,p,patch)==Result::wrongGeometry,"different node rejected");
    p.width=0;
    Check(Project(m,123,9,p,patch)==Result::invalidPlacement,"zero width rejected");
    p=Place(m);p.rotation=std::numeric_limits<double>::quiet_NaN();
    Check(Project(m,123,9,p,patch)==Result::invalidPlacement,"NaN rotation rejected");
    p=Place(m);p.anchor.barycentric={0.5,0.5,0.5};
    Check(Project(m,123,9,p,patch)==Result::invalidPlacement,"invalid barycentric sum rejected");
    p=Place(m);p.anchor.tangent={0,0,1};
    Check(Project(m,123,9,p,patch)==Result::invalidPlacement,"surface-normal tangent rejected");
    p=Place(m);Limits l;l.outputTriangles=1;
    Check(Project(m,123,9,p,patch,l)==Result::budgetExceeded&&patch.vertices.size()==sentinel,"budget failure never emits partial geometry");
    Hit hit{77,{{1,0,0}},42};
    Check(Pick(m,{{0,0,-10},{0,0,1}},hit)==Result::miss&&hit.triangle==77,"back side is not selectable");
    Check(Pick(m,{{0,0,10},{0,0,0}},hit)==Result::invalidRay,"zero ray rejected");
    Check(Pick(m,{{0,0,10},{0,0,std::numeric_limits<double>::infinity()}},hit)==Result::invalidRay,"infinite ray rejected");
    m.triangles[0].vertex[2]=999;
    Check(Pick(m,{{0,0,10},{0,0,-1}},hit)==Result::invalidMesh,"out-of-range vertex rejected");
    m=Plane();m.vertices[0].x=std::numeric_limits<double>::quiet_NaN();
    Check(Project(m,123,9,p,patch)==Result::invalidMesh,"invalid model coordinates rejected");

    m=Plane(); p=Place(m);
    // A second, disconnected plate occupies the same projection volume.
    m.vertices.insert(m.vertices.end(),{{-2,-2,-0.05},{2,-2,-0.05},{2,2,-0.05},{-2,2,-0.05}});
    m.triangles.push_back({{{4,5,6}}});m.triangles.push_back({{{4,6,7}}});
    Check(Project(m,123,9,p,patch)==Result::ok&&Near(Area(patch),4),"decal does not copy onto disconnected armor");
    for(const auto& v:patch.vertices)Check(v.triangle<2,"only selected connected surface receives decal");
    Check(Pick(m,{{0,0,10},{0,0,-1}},hit)==Result::ok&&Near(hit.distance,10),"nearest surface wins");

    // A folded edge remains surface-conforming but is clipped at projection depth.
    m=Plane();m.vertices[3].z=1;p=Place(m);p.depth=0.05;
    Check(Project(m,123,9,p,patch)==Result::ok,"project onto bent surface");
    ValidatePatch(m,patch);
    for(const auto& v:patch.vertices)Check(std::abs(v.position.z)<=0.0500001,"projection depth clips folded surface");
    Check(Area(patch)<4,"depth clipping reduces covered area");

    m=Plane();m.vertices[3]={0,-3,0};p=Place(m);
    Check(Project(m,123,9,p,patch)==Result::ok,"back-facing neighbor does not abort front face");
    for(const auto& v:patch.vertices)Check(v.triangle==0,"back-facing neighbor not painted");
    m=Plane();m.vertices.push_back({-3,3,0});m.triangles.push_back({{{0,2,4}}});p=Place(m);
    Check(Project(m,123,9,p,patch)==Result::ok,"non-manifold edge bounded");
    for(const auto& v:patch.vertices)Check(v.triangle==0,"ambiguous seam not traversed");
}
void Editing() {
    Appearance original;original.unitSerial=10;original.unitType=100;original.revision=5;
    original.slots[0].sticker=1234;original.slots[1].sticker=2345;
    Editor editor; const auto placement=Place(Plane());Appearance proposal;
    Check(editor.Open(original),"open authoritative unit appearance");
    Check(!editor.Place(3,placement)&&!editor.Place(2,placement),"cannot place absent sticker or fourth slot");
    Check(editor.Place(0,placement)&&editor.Dirty(),"preview changes draft");
    Check(!editor.Saved().slots[0].positioned,"preview cannot mutate confirmed appearance");
    auto other=original;other.unitSerial=20;
    Check(!editor.Open(other),"changing units cannot discard unconfirmed work");
    Check(editor.Cancel()&&!editor.Dirty()&&editor.State()==EditState::closed,"cancel restores saved placement");
    Check(editor.Open(original)&&editor.Place(0,placement),"reopen and preview");
    Check(editor.BeginSave(1,proposal)&&proposal.revision==5,"request retains expected revision");
    Check(!editor.Place(1,placement)&&!editor.Cancel()&&!editor.Open(other),"in-flight operation cannot be changed or abandoned");
    auto saved=proposal;++saved.revision;
    Check(!editor.Acknowledge(2,Reply::saved,saved),"unrelated receipt ignored");
    Check(!editor.Acknowledge(1,Reply::saved,proposal),"success must advance revision");
    auto altered=saved;altered.slots[1].sticker=777;
    Check(!editor.Acknowledge(1,Reply::saved,altered),"success receipt must match requested appearance");
    Check(editor.Acknowledge(1,Reply::saved,saved)&&!editor.Dirty(),"exact server receipt commits appearance");
    Check(editor.ResetPosition(0),"reset restores original native slot behavior");
    Check(!editor.BeginSave(1,proposal),"request IDs cannot be reused");
    Check(editor.BeginSave(2,proposal),"send new request");editor.Timeout();
    Check(editor.State()==EditState::uncertain&&!editor.BeginSave(3,proposal)&&!editor.Cancel(),"timeout cannot masquerade as rollback or cause duplicate save");
    Check(!editor.Reconcile(other),"another unit cannot reconcile timed-out operation");
    auto inconsistent=saved;inconsistent.slots[1].sticker=999;
    Check(!editor.Reconcile(inconsistent),"same revision cannot have different authoritative content");
    Check(editor.Reconcile(saved)&&!editor.Dirty(),"authoritative refresh resolves uncertainty");
    Check(editor.ResetPosition(0)&&editor.BeginSave(3,proposal),"new request after reconciliation");
    auto conflict=saved;++conflict.revision;
    Check(editor.Acknowledge(3,Reply::conflict,conflict)&&!editor.Dirty(),"conflict restores newer server state");
    Check(editor.ResetPosition(0)&&editor.BeginSave(4,proposal),"request to reject");
    Check(editor.Acknowledge(4,Reply::rejected,conflict)&&!editor.Dirty(),"rejection restores authoritative state");
}
}
int main() {Geometry();Editing();std::cout << checks << " checks passed\n";}
