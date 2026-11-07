#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace capsule_catalog {
struct AcquisitionHints {
    std::unordered_set<uint32_t> units;
    std::wstring text, brief;

    bool Decode(const std::vector<uint8_t>& raw) {
        if (raw.size()<20 || std::memcmp(raw.data(),"SDGOACQ1",8)) return false;
        auto u32=[&](size_t at){uint32_t n;std::memcpy(&n,raw.data()+at,4);return n;};
        const auto count=u32(8),full=u32(12),shorter=u32(16);
        if (!count || count>20000 || !full || full>=100 || !shorter || shorter>full ||
            raw.size()!=20u+4u*count+2u*(full+shorter)) return false;
        AcquisitionHints result;
        uint32_t previous=0;
        for(uint32_t i=0;i<count;++i){
            const uint32_t id=u32(20+4*i);
            if(id<=previous || id>0x7fffffff) return false;
            result.units.insert(id);previous=id;
        }
        const auto* words=reinterpret_cast<const wchar_t*>(raw.data()+20+4*count);
        result.text.assign(words,full);result.brief.assign(words+full,shorter);
        if(result.text.find(L'\0')!=std::wstring::npos || result.brief.find(L'\0')!=std::wstring::npos) return false;
        *this=std::move(result);return true;
    }

    bool Apply(void* record,uint32_t size) const {
        if (!record || size!=614) return false;
        auto* row=static_cast<uint8_t*>(record);uint32_t unit=0;
        std::memcpy(&unit,row+6,4);
        if(row[4]!=0 || row[5]!=1 || !units.count(unit)) return false;
        wchar_t buffer[100]{};std::memcpy(buffer,row+214,sizeof(buffer));
        size_t length=0;while(length<100 && buffer[length])++length;
        if(length==100) return false;
        const std::wstring original(buffer,length);
        auto ends=[&](const std::wstring& suffix){return original.size()>=suffix.size() &&
            original.compare(original.size()-suffix.size(),suffix.size(),suffix)==0;};
        if(ends(text)||ends(brief)) return true;
        const auto prefix=original.empty()?L"":L"\n";
        std::wstring value=original+prefix+text;
        if(value.size()>=100)value=original+prefix+brief;
        if(value.size()>=100)return false; // retain every original source byte
        std::memset(row+214,0,200);std::memcpy(row+214,value.data(),value.size()*2);
        return true;
    }
};
}
