#include "../mods/game_test_client/unit_transform_path.h"
#include "../mods/game_test_client/unit_transform_packet_format.h"
#include <array>
#include <cstdio>
#include <map>

using namespace sdgo_test::transforms;
namespace {
int failures=0;
void Check(bool valid,const char* name){std::printf("%s %s\n",valid?"PASS":"FAIL",name);if(!valid)++failures;}
using Bytes=std::array<unsigned char,4976>;
Bytes Unit(unsigned id,unsigned target){Bytes raw{};std::memcpy(raw.data(),&id,4);std::memcpy(raw.data()+316,&target,4);return raw;}
}
int main(){
    unsigned char reply[8]{0x52,0x06,0,0,0,0,0x34,0x12};Packet packet{};
    Check(DecodeReply(reply,sizeof(reply),packet)&&packet.kind==1&&packet.opcode==0x652&&packet.error==0x1234,
        "reply decoding retains the original unsigned error word");
    Check(!DecodeReply(reply,7,packet)&&!packet.kind,"short replies fail without retaining stale fields");
    unsigned char notice[19]{0x53,0x06};unsigned pilot=0xfedcba98,target=0x89abcdef;
    std::memcpy(notice+6,&pilot,4);std::memcpy(notice+10,&target,4);notice[14]=1;
    Check(DecodeNotice(notice,sizeof(notice),packet)&&packet.pilot==pilot&&packet.target==target&&packet.raw14==1,
        "notification decoding keeps both full-width identities separate");
    Check(!DecodeNotice(notice,18,packet)&&!packet.target,"short notifications fail without reading target data");
    Check(!DecodeNotice(nullptr,19,packet),"null packet is never evidence");
    std::map<unsigned,Bytes> records{{901,Unit(901,902)},{902,Unit(902,903)},{903,Unit(903,902)}};
    auto lookup=[&](unsigned id){const auto found=records.find(id);return found==records.end()?Record{}:Record{found->second.data(),4976};};
    Path path{};const char* error=nullptr;
    Check(Build(901,3,false,lookup,path,error)&&path.ids[0]==901&&path.ids[1]==902&&path.ids[2]==903&&path.ids[3]==902,
        "one-way entry and downstream cycle preserve the actual three-step path");
    Check(path.contains(901)&&path.contains(903)&&!path.contains(904),"actor admission is confined to record-derived identities");
    Check(!Build(901,2,true,lookup,path,error)&&!path.steps,"legacy roundtrip still refuses a nonreciprocal pair atomically");
    Check(Build(902,2,true,lookup,path,error)&&path.ids[2]==902,"unchanged reciprocal pair is still accepted");
    Check(Build(901,MaxSteps,false,lookup,path,error)&&path.steps==MaxSteps,"bounded repeated cycles preserve every step");
    for(unsigned steps:{0u,MaxSteps+1})Check(!Build(901,steps,false,lookup,path,error),"invalid path budgets fail");
    records[903]=Unit(903,904);Check(!Build(901,3,false,lookup,path,error),"missing next form fails instead of truncating the test");
    records[903]=Unit(903,903);Check(!Build(901,3,false,lookup,path,error),"self-loop is not mistaken for a changed native form");
    Check(Build(903,2,true,lookup,path,error,true)&&path.ids[0]==903&&path.ids[1]==903&&path.ids[2]==903,
        "explicit self-transform inspection retains the real same-ID route; the runtime must prove bank flips");
    Check(!Build(901,2,true,lookup,path,error,true),"self-transform inspection cannot admit unrelated multi-record edges");
    Check(!Build(903,2,false,lookup,path,error,true),"self-transform inspection is confined to its explicit roundtrip scope");
    records[903]=Unit(903,0);Check(!Build(901,3,false,lookup,path,error),"terminal path cannot synthesize a reverse transform");
    records[903]=Unit(904,902);Check(!Build(901,3,false,lookup,path,error),"returned record identity must match the lookup");
    records[903]=Unit(903,902);records[903][4]=1;Check(!Build(901,3,false,lookup,path,error),"unpublished native records fail");
    Check(!Build(901,2,false,[](unsigned){return Record{nullptr,4976};},path,error),"null record cannot satisfy the path");
    Check(!Build(901,2,false,[&](unsigned){return Record{records[901].data(),4975};},path,error),"partial record cannot satisfy the path");
    return failures?1:0;
}
