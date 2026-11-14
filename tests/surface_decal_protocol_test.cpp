#include "sdgo/surface_decal_protocol.h"
#include "sdgo/surface_decal_sql.h"
#include "sdgo/surface_decal_view.h"
#include "sdgo/surface_decal_battle.h"
#include "sdgo/surface_decal_room.h"
#include <map>
#include <cstring>
#include <cstdlib>
#include <iostream>

using namespace sdgo::surface_decal;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
bool Near(double a,double b){return std::abs(a-b)<1e-8;}
void RoomIdentity(){
    const RoomBindings b{101,102,103,104,105,106,107,108,109};
    std::map<std::uint32_t,std::uint32_t> memory{
        {0x1000+0x4a0,18},{0x1000+0x4a4,0x2000},{0x2000,101},{0x2004,0x3000},
        {0x3000,0x4000},{0x4010,102},{0x3038,0x5000},{0x3040,0x6000},
        {0x5000,0x7000},{0x701c,103},{0x7020,104},{0x7088,106},{0x5028,2},
        {0x6008,2},{0x600c,0x8000},{0x8000,0x10000},{0x8004,0x20000},
        {0x10000,108},{0x1001c,109},{0x20000,108},{0x2001c,109}};
    auto read=[&](std::uint32_t address,void* output,std::size_t size){
        const auto it=memory.find(address);if(it==memory.end()||size!=4)return false;
        std::memcpy(output,&it->second,4);return true;
    };
    std::array<std::uint32_t,2> records{{0x30000,0x40000}};unsigned calls=0;
    auto get=[&](std::uint32_t getter,std::uint32_t room,std::uint32_t slot){
        ++calls;Check((getter==106||getter==107)&&room==0x5000&&slot<2,"only verified native room getter is called");return records[slot];
    };
    auto resolve=[&](std::uint32_t model){return b.Resolve(0x1000,model,read,get);};
    Check(resolve(0x117f4)==records[0]&&resolve(0x217f4)==records[1],"same cosmetic units bind distinct visual slots without guessing from appearance");
    Check(resolve(0x21c7c)==records[1],"transformed room model retains its exact owned slot");
    memory[0x6008]=12;
    memory[0x5028]=8;
    Check(resolve(0x117f4)==records[0]&&resolve(0x21c7c)==records[1],"spare native visual capacity does not reject real room slots");
    memory[0x5028]=2;
    memory[0x8008]=0x30000;memory[0x30000]=108;memory[0x3001c]=109;
    const auto spareCalls=calls;
    Check(!resolve(0x317f4)&&calls==spareCalls,"spare visual objects never invoke an out-of-room native unit getter");
    memory[0x6008]=1;
    Check(!resolve(0x117f4),"incomplete visual pool is not read beyond its capacity");
    memory[0x6008]=2;
    records[1]=0x50000;Check(resolve(0x217f4)==records[1],"active unit changes resolve fresh owned records");
    memory[0x7020]=105;memory[0x7088]=107;
    Check(resolve(0x117f4)==records[0],"mission room uses its verified native unit getter");
    memory[0x7088]=106;const auto before=calls;
    Check(!resolve(0x117f4)&&calls==before,"mixed room vtables rejected before native calls");
    memory[0x7088]=107;memory[0x1000+0x4a0]=22;
    Check(!resolve(0x117f4),"stale room pointers outside waiting scene rejected");
    memory[0x1000+0x4a0]=18;memory[0x6008]=13;
    Check(!resolve(0x117f4),"invalid visual array bounds rejected");
    memory[0x6008]=2;memory[0x2001c]=999;
    Check(!resolve(0x217f4),"unknown visual data rejected");memory[0x2001c]=109;
    Check(!resolve(0x217f8),"nearby subobject is not a valid model");
    auto replaced=[&](std::uint32_t,std::uint32_t,std::uint32_t){memory[0x8004]=0x10000;return records[1];};
    Check(!b.Resolve(0x1000,0x217f4,read,replaced),"slot replaced during asynchronous resolution cannot inherit identity");
}
void Wire(){
    Appearance a;a.unitSerial=0x123456789ab;a.unitType=10001;a.revision=17;
    auto& s=a.slots[1];s.sticker=3020001;s.positioned=true;s.placement.anchor.geometry=0xFEDCBA9876543210;
    s.placement.anchor.node=5;s.placement.anchor.triangle=317;s.placement.anchor.barycentric={{0.2,0.3,0.5}};
    s.placement.width=1.234567890123;s.placement.rotation=-0.732;s.placement.depth=0.31;
    wire::Bytes b;Check(wire::Encode(a,b),"valid geometry placement encoded");Appearance decoded;
    Check(wire::Decode(b.data(),b.size(),decoded)&&SameContent(a,decoded)&&a.revision==decoded.revision,"wire preserves exact double values and 64-bit identities");
    for(std::size_t n=0;n<b.size();++n)Check(!wire::Decode(b.data(),n,decoded),"every truncated payload rejected");
    auto bad=b;bad[0]=2;Check(!wire::Decode(bad.data(),bad.size(),decoded),"future version rejected");
    bad=b;bad[wire::headerBytes+4]=2;Check(!wire::Decode(bad.data(),bad.size(),decoded),"unknown position mode rejected");
    bad=b;bad[wire::headerBytes+8]=1;Check(!wire::Decode(bad.data(),bad.size(),decoded),"unpositioned slots have canonical zero payload");
    bad=b;wire::Real(bad.data()+wire::headerBytes+wire::slotBytes+72,std::numeric_limits<double>::infinity());
    Check(!wire::Decode(bad.data(),bad.size(),decoded),"non-finite remote width rejected");
    Check(SameContent(a,decoded),"failed decode leaves last authoritative appearance unchanged");
    wire::Bytes exact;Check(wire::Encode(decoded,exact)&&exact==b,"wire canonical form stable");
    const auto hex=sql::Hex(b.data(),b.size());wire::Bytes unhex;
    Check(sql::Unhex(hex.substr(2),unhex.data(),unhex.size())&&unhex==b,"SQL binary representation round trips all bytes");
    Check(!sql::Unhex(L"' OR 1=1--",unhex.data(),unhex.size()),"malformed stored binary rejected");
    Check(sql::Request(0,a.unitSerial,&a).empty()&&sql::Request(1,a.unitSerial+1,&a).empty(),"save requires authenticated actor and exact unit identity");
    a.revision=INT64_MAX;Check(sql::Request(1,a.unitSerial,&a).empty(),"revision cannot overflow SQL bigint");
}
void Picking(){
    const Matrix identity{{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
    View view{identity,identity,identity,10,20,400,200};Ray ray;
    Check(ScreenRay(view,210,120,ray)&&Near(ray.origin.x,0)&&Near(ray.origin.y,0)&&Near(ray.direction.z,1),"orthographic viewport center ray");
    Check(ScreenRay(view,10,20,ray)&&Near(ray.origin.x,-1)&&Near(ray.origin.y,1),"screen Y is inverted for native camera");
    Check(!ScreenRay(view,410,120,ray)&&!ScreenRay(view,210,220,ray),"outside preview cannot place sticker");
    view.world[12]=5;view.world[0]=2;
    Check(ScreenRay(view,210,120,ray)&&Near(ray.origin.x,-2.5),"world translation and scale inverted for node-local hit test");
    view.world[0]=0;Check(!ScreenRay(view,210,120,ray),"singular transform fails without fabricated ray");
    view.world=identity;view.projection={{2,0,0,0,0,2,0,0,0,0,10.0/9,1,0,0,-10.0/9,0}};
    Check(ScreenRay(view,210,120,ray)&&Near(ray.origin.z,1)&&Near(ray.direction.z,9),"perspective near and far planes produce correct node ray");
    view.projection[0]=std::numeric_limits<double>::quiet_NaN();Check(!ScreenRay(view,210,120,ray),"invalid camera rejected");
}
void LetterOrientation(){
    // The actual factory uses a left-handed orthographic camera and a unit
    // facing the camera (180 degrees about Y). Check screen-space ordering,
    // rather than merely repeating the projector's local UV formula.
    const Matrix identity{{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}};
    View view{identity,identity,identity,0,0,400,400};
    view.world[0]=-1;view.world[10]=-1;view.world[14]=0.5;
    Mesh mesh{{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}},{{{{0,1,2}}},{{{0,2,3}}}}};
    Ray ray;Hit hit;Placement placement;Patch patch;
    Check(ScreenRay(view,200,200,ray)&&Pick(mesh,ray,hit)==Result::ok,"native front-facing view selects the surface");
    Check(MakePlacement(mesh,hit,1,0,{1,0,0},2,2,0.1,placement)==Result::ok,"build native-view lettering placement");
    for(unsigned quarter=0;quarter<2;++quarter){
        placement.rotation=quarter*1.5707963267948966;
        Check(Project(mesh,1,0,placement,patch)==Result::ok,"project native lettering rectangle");
        for(const auto& vertex:patch.vertices){
            Vec3 clip;Check(Transform(vertex.position,view.world,clip),"project lettering into native screen space");
            const double screenX=(clip.x+1)/2,screenY=(1-clip.y)/2;
            if(!quarter)Check(Near(vertex.uv.x,screenX)&&Near(vertex.uv.y,screenY),"letter columns run left to right and rows run top to bottom");
            else Check(Near(vertex.uv.x,screenY)&&Near(vertex.uv.y,1-screenX),"positive sticker rotation is clockwise on screen");
        }
    }
}
void BattleIdentity(){
    BattleBindings cache;std::array<Appearance,4> deck{};
    for(unsigned i=0;i<4;++i){deck[i].unitSerial=9000+i;deck[i].unitType=10001;deck[i].slots[0].sticker=240001;}
    const std::array<std::uint32_t,3> stickers{{240001,0,0}};Appearance out;
    cache.Replace(101,deck,4);
    Check(cache.Prepare(101,2,10001,stickers,1000)&&cache.Resolve(1000,out)&&out.unitSerial==9002,"identical deck cosmetics retain the exact owned slot identity");
    Check(!cache.Prepare(102,2,10001,stickers,1000)&&!cache.Resolve(1000,out),"another participant cannot inherit a reused appearance pointer");
    Check(cache.Prepare(101,3,10001,stickers,1000),"last valid deck slot binds");
    cache.Replace(101,{},0);Check(!cache.Resolve(1000,out)&&!cache.Prepare(101,3,10001,stickers,1000),"failed construction invalidates the previous participant and styling");
    cache.Replace(101,deck,1);
    Check(!cache.Prepare(101,1,10001,stickers,1000),"single-unit mode cannot select an unannounced deck slot");
    Check(!cache.Prepare(101,0,10002,stickers,1000),"different native model cannot borrow a saved placement");
    Check(!cache.Prepare(101,0,10001,{{240002,0,0}},1000),"different native sticker cannot borrow a placement");
    Check(cache.Prepare(101,0,10001,stickers,1000),"matching native participant binds");
    cache.ForgetStyle(1000);Check(!cache.Resolve(1000,out),"invalid native style inputs clear an older pointer binding");
    cache.Replace(0x80000001u,deck,1);Check(!cache.Prepare(0x80000001u,0,10001,stickers,1000),"AI identifiers do not acquire human inventory identity");
    cache.Clear();Check(!cache.Prepare(101,0,10001,stickers,1000),"session boundary clears native participant identities");
}
}
int main(){Wire();Picking();LetterOrientation();BattleIdentity();RoomIdentity();std::cout<<checks<<" checks passed\n";}
