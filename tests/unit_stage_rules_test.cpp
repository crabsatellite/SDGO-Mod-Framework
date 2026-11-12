#include "sdgo/unit_stage_rules.h"
#include <cstdio>
using namespace sdgo;
void Put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value){std::memcpy(bytes.data()+at,&value,4);}
int main(){
    unit_catalog::Catalog catalog;
    for(const auto pair:{std::pair<unsigned,unsigned>{29001,2},{29002,2},{74001,13}}){
        unit_catalog::Entry row;row.id=pair.first;row.kind=pair.second;row.bytes.resize(pair.second==2?4976:1132);
        Put(row.bytes,0,row.id);catalog.records.push_back(row);
    }
    Put(catalog.records[0].bytes,316,29002);Put(catalog.records[0].bytes,575,74001);
    catalog.records[2].bytes[155]=2;catalog.records[2].bytes[156]=60;
    std::vector<std::uint8_t> bytes(96);std::memcpy(bytes.data(),"SDGOST1\0",8);
    Put(bytes,8,1);Put(bytes,12,80);Put(bytes,16,1);Put(bytes,20,16);
    for(unsigned i=24;i<56;++i)bytes[i]=0xaa;
    Put(bytes,80,29001);Put(bytes,84,29002);Put(bytes,88,74001);Put(bytes,92,60);
    Put(bytes,56,unit_catalog::Hash32(bytes.data()+80,16));
    unit_stages::Policy policy;std::string error;const std::string hash(64,'a');
    if(!policy.Parse(bytes,hash,catalog,error)||policy.rules.size()!=1||policy.managed.size()!=2)return 1;
    for(unsigned at:{0u,8u,12u,16u,20u,24u,56u,60u,80u,84u,88u,92u}){
        auto corrupt=bytes;corrupt[at]^=1;
        if(policy.Parse(corrupt,hash,catalog,error)||!policy.rules.empty()||!policy.managed.empty())return 2;
    }
    for(unsigned offset:{155u,156u,1131u}){
        auto corrupt=catalog;corrupt.records[2].bytes[offset]^=1;
        if(policy.Parse(bytes,hash,corrupt,error))return 3;
    }
    auto special=catalog;special.records[1].bytes[320]=3;
    if(policy.Parse(bytes,hash,special,error))return 4;
    if(policy.Parse(bytes,std::string(64,'b'),catalog,error))return 5;
    std::puts("PASS stage policy: complete native edge and skill, catalog binding, corruption and special-condition refusal");
    return 0;
}
