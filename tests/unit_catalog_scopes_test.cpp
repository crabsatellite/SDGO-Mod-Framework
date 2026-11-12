#include "sdgo/unit_catalog_scopes.h"
#include <cstdio>
using namespace sdgo::unit_catalog;
Entry Skill(unsigned id){Entry row;row.kind=13;row.id=id;row.bytes.resize(1132);Put32(row.bytes.data(),id);row.bytes[5]='T';row.recordHash=Hash32(row.bytes.data(),row.bytes.size());return row;}
int main(){
    Catalog owner,presets,combined;owner.records.push_back(Skill(74001));presets.records.push_back(Skill(74002));
    const auto original=owner.records[0].bytes;std::string error;
    if(!Combine(owner,presets,combined,error)||combined.records.size()!=2||owner.records[0].bytes!=original){std::puts(error.c_str());return 1;}
    if(!combined.Contains(74001,13)||!combined.Contains(74002,13))return 2;
    presets.records[0]=Skill(74001);
    if(Combine(owner,presets,combined,error))return 3;
    Catalog empty;if(!Combine(empty,owner,combined,error)||combined.records[0].bytes!=original)return 4;
    std::puts("PASS independently sealed unit scopes combine without replacement; duplicate IDs refuse");return 0;
}
