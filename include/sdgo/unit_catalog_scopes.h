#pragma once
#include "unit_catalog_format.h"

namespace sdgo::unit_catalog {
// Each scope retains its original bytes/hash for its own sidecars. Only the
// native registration inventory is combined; a preset cannot replace a row.
inline bool Combine(const Catalog& primary,const Catalog& presets,Catalog& combined,std::string& error){
    std::vector<Entry> rows=primary.records;rows.insert(rows.end(),presets.records.begin(),presets.records.end());
    std::sort(rows.begin(),rows.end(),[](const Entry& a,const Entry& b){return a.id<b.id;});
    if(rows.empty()||rows.size()>kMaxRecords){error="combined unit scope record budget";return false;}
    std::vector<std::uint8_t> bytes(sizeof(Header));std::uint32_t previous=0;
    for(const auto& row:rows){
        if(row.id==previous){error="unit preset collides with an installed catalog record";return false;}previous=row.id;
        EntryHeader entry={row.kind,row.id,static_cast<std::uint32_t>(row.bytes.size()),row.recordHash,0,0};
        const auto* raw=reinterpret_cast<const std::uint8_t*>(&entry);bytes.insert(bytes.end(),raw,raw+sizeof(entry));
        bytes.insert(bytes.end(),row.bytes.begin(),row.bytes.end());
        if(bytes.size()-sizeof(Header)>kMaxPayloadBytes){error="combined unit scope payload budget";return false;}
    }
    Header header={};std::memcpy(header.magic,"SDGOUN1\0",8);header.version=1;header.headerBytes=sizeof(Header);
    header.count=static_cast<std::uint32_t>(rows.size());header.payloadBytes=static_cast<std::uint32_t>(bytes.size()-sizeof(Header));
    header.payloadHash=Hash32(bytes.data()+sizeof(Header),header.payloadBytes);std::memcpy(bytes.data(),&header,sizeof(header));
    return combined.Parse(bytes,error);
}
}
