#pragma once
#include "unit_catalog_format.h"
#include "script_registration.h"
#include <map>
#include <set>

namespace sdgo::unit_stages {
constexpr bool OrdinarySkill(std::uint32_t id){
    return (id&&id<75001)||(id>3220000&&id<3225001)||(id>3473000&&id<3478001)||
           (id>3558000&&id<3559001)||(id>3613001&&id<3616000);
}
struct Rule {std::uint32_t source=0,target=0,skill=0,percent=0;};
struct Policy {
    std::map<std::uint32_t,Rule> rules;
    std::set<std::uint32_t> managed;
    bool Parse(const std::vector<std::uint8_t>& bytes,const std::string& hash,
               const unit_catalog::Catalog& catalog,std::string& error){
        rules.clear();managed.clear();error.clear();
        auto fail=[&](const char* message){rules.clear();managed.clear();error=message;return false;};
        using unit_catalog::U32;
        if(bytes.size()<80||std::memcmp(bytes.data(),"SDGOST1\0",8)||U32(bytes.data()+8)!=1||U32(bytes.data()+12)!=80)
            return fail("stage rules header differs");
        const auto count=U32(bytes.data()+16),extent=U32(bytes.data()+20);
        if(count>128||extent!=count*16||bytes.size()!=80+extent)return fail("stage rules extent differs");
        const char digits[]="0123456789abcdef";std::string sealed;
        for(unsigned i=24;i<56;++i){sealed+=digits[bytes[i]>>4];sealed+=digits[bytes[i]&15];}
        if(sealed!=hash||unit_catalog::Hash32(bytes.data()+80,extent)!=U32(bytes.data()+56))return fail("stage rules catalog/payload hash differs");
        for(unsigned i=60;i<80;++i)if(bytes[i])return fail("stage rules reserved bytes differ");
        auto record=[&](std::uint32_t id,std::uint32_t kind,std::size_t size)->const std::uint8_t*{
            const auto slot=catalog.Find(id);if(slot==catalog.records.size())return nullptr;
            const auto& row=catalog.records[slot];return row.kind==kind&&row.bytes.size()==size?row.bytes.data():nullptr;
        };
        for(unsigned i=0;i<count;++i){const auto* p=bytes.data()+80+16*i;Rule row{U32(p),U32(p+4),U32(p+8),U32(p+12)};
            const auto* source=record(row.source,2,4976);const auto* target=record(row.target,2,4976);const auto* skill=record(row.skill,13,1132);
            if(!source||!target||!skill||!OrdinarySkill(row.skill)||row.source==row.target||!row.percent||row.percent>99||U32(source+316)!=row.target||
               (U32(source+575)!=row.skill&&U32(source+579)!=row.skill)||source[320]||target[320]||skill[155]!=2||
               (static_cast<unsigned>(skill[156])|(static_cast<unsigned>(skill[157])<<8))!=row.percent||
               skill[1131]||!rules.emplace(row.source,row).second)
                return fail("stage rules do not match complete native records");
            managed.insert(row.source);managed.insert(row.target);
        }
        return true;
    }
};
}
