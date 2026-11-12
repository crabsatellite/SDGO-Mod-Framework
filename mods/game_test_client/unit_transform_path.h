#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sdgo_test::transforms {
inline constexpr unsigned MaxSteps=16;
struct Record { const unsigned char* data=nullptr; std::size_t bytes=0; };
struct Path {
    unsigned steps=0;
    std::uint32_t ids[MaxSteps+1]{};
    bool contains(std::uint32_t id)const {
        for(unsigned i=0;i<=steps;++i)if(ids[i]==id)return true;
        return false;
    }
};

// Read the actual native records. A bounded path may enter a cycle without
// returning to its entry variant. Nothing supplies an invented reverse edge.
template<class Lookup>
bool Build(std::uint32_t root,unsigned steps,bool requireReciprocal,Lookup lookup,Path& output,const char*& error,bool allowSelf=false){
    output={};error=nullptr;
    if(!root||!steps||steps>MaxSteps||(requireReciprocal&&steps!=2)){
        error="native transform path step count/root is invalid";return false;
    }
    Path candidate{};candidate.steps=steps;candidate.ids[0]=root;
    for(unsigned i=0;i<=steps;++i){
        const auto id=candidate.ids[i];const Record record=lookup(id);std::uint32_t actual=0,target=0;
        if(!record.data||record.bytes!=4976){error="native transform path record is missing or incomplete";return false;}
        std::memcpy(&actual,record.data,4);
        if(actual!=id||record.data[4]){error="native transform path record identity/security differs";return false;}
        if(i==steps)break;
        std::memcpy(&target,record.data+316,4);
        if(!target||(target==id&&!allowSelf)){error="native transform path has no distinct next form";return false;}
        if(allowSelf&&(target!=root||id!=root||!requireReciprocal)){
            error="self-transform inspection requires one complete self-referencing record";return false;
        }
        candidate.ids[i+1]=target;
    }
    if(requireReciprocal&&candidate.ids[2]!=root){
        error="two-form roundtrip requires reciprocal original transform references";return false;
    }
    output=candidate;return true;
}
}
