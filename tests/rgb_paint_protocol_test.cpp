#include "sdgo/rgb_paint_sql.h"
#include "../mods/unit_appearance_client/rgb/identity.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
using namespace sdgo::rgb_paint;
unsigned checks=0;
void Check(bool v){++checks;if(!v){std::fprintf(stderr,"RGB protocol check %u failed\n",checks);std::exit(1);}}
int main(int argc,char** argv){
    // The isolated SQL integration driver supplies authenticated fixture IDs
    // and a canonical wire record. This utility never opens a database.
    if(argc>=4&&std::string(argv[1])=="--query"){
        const auto actor=std::strtoull(argv[2],nullptr,10),uid=std::strtoull(argv[3],nullptr,10);Record r;Record* proposal=nullptr;
        if(actor>UINT32_MAX)return 2;
        if(argc==5){const std::string hex=argv[4];wire::Bytes b{};if(!sql::Unhex(std::wstring(hex.begin(),hex.end()),b.data(),b.size())||!wire::Decode(b.data(),b.size(),r))return 2;proposal=&r;}
        const auto query=sql::Request(static_cast<std::uint32_t>(actor),uid,proposal);if(query.empty())return 2;
        std::wcout<<query;return 0;
    }
    Record r;r.type=20001;r.state.identity={7,123456};r.state.revision=9;
    for(unsigned i=0;i<6;++i){r.native[i]=static_cast<std::uint16_t>(i*999);r.state.colors[i]=Rgb{static_cast<std::uint8_t>(i*41),static_cast<std::uint8_t>(255-i),static_cast<std::uint8_t>(i)};}
    for(unsigned mask=0;mask<64;++mask){auto a=r;for(unsigned i=0;i<6;++i)if(!(mask&(1u<<i)))a.state.colors[i].reset();wire::Bytes b;Check(wire::Encode(a,b));Record copy;Check(wire::Decode(b.data(),b.size(),copy));Check(copy.state.colors==a.state.colors&&copy.native==a.native&&copy.type==a.type&&copy.state.identity.unit==a.state.identity.unit&&copy.state.revision==a.state.revision);}
    wire::Bytes b;Check(wire::Encode(r,b));Record out;
    for(unsigned n=0;n<wire::bytes;++n)Check(!wire::Decode(b.data(),n,out));
    for(unsigned at:{0u,36u,58u,59u}){auto bad=b;bad[at]=128;Check(!wire::Decode(bad.data(),bad.size(),out));}
    auto bad=b;bad[36]&=~1;bad[40]=1;Check(!wire::Decode(bad.data(),bad.size(),out));
    r.state.colors[0]=Rgb{0,0,0};Check(wire::Encode(r,b));Check(wire::Decode(b.data(),b.size(),out));Check(out.state.colors[0].has_value());
    r.state.colors[0].reset();Check(wire::Encode(r,b));Check(wire::Decode(b.data(),b.size(),out));Check(!out.state.colors[0]);
    Check(sql::Request(0,123,nullptr).empty());Check(sql::Request(42,0,nullptr).empty());Check(sql::Request(42,INT64_MAX+1ull,nullptr).empty());
    r.state.revision=INT64_MAX;Check(sql::Request(42,r.state.identity.unit,&r).empty());
    // Equal types and stock colors must remain distinct owned instances.
    std::array<std::uint8_t,0x48e> seatA{},seatB{};std::array<std::uint8_t,16> style{};
    wire::Put(style.data(),20001,4);
    for(auto* seat:{&seatA,&seatB}){wire::Put(seat->data()+0x429,42,4);wire::Put(seat->data()+0x49,20001,4);}
    wire::Put(seatA.data()+0x51,1234,8);wire::Put(seatB.data()+0x51,5678,8);
    Check(identity::RoomRecord(seatA.data(),seatA.size(),style.data(),style.size(),7,out)&&out.state.identity.unit==1234&&out.state.identity.session==7);
    Check(identity::RoomRecord(seatB.data(),seatB.size(),style.data(),style.size(),7,out)&&out.state.identity.unit==5678);
    style[4]=1;Check(!identity::RoomRecord(seatA.data(),seatA.size(),style.data(),style.size(),7,out));style[4]=0;
    style[0]^=1;Check(!identity::RoomRecord(seatA.data(),seatA.size(),style.data(),style.size(),7,out));style[0]^=1;
    Check(!identity::RoomRecord(seatA.data(),seatA.size()-1,style.data(),style.size(),7,out));
    Check(!identity::RoomRecord(seatA.data(),seatA.size(),style.data(),15,7,out));
    Check(!identity::RoomRecord(seatA.data(),seatA.size(),style.data(),style.size(),0,out));
    for(auto pilot:{0u,0x80000001u}){wire::Put(seatA.data()+0x429,pilot,4);Check(!identity::RoomRecord(seatA.data(),seatA.size(),style.data(),style.size(),7,out));}
    wire::Put(seatA.data()+0x429,42,4);wire::Put(seatA.data()+0x51,0,8);
    Check(!identity::RoomRecord(seatA.data(),seatA.size(),style.data(),style.size(),7,out));
    std::printf("RGB wire and room identity: %u checks passed\n",checks);
}
