#pragma once
#include "sdgo/surface_decal.h"
#include <cstring>
#include <limits>

namespace sdgo::surface_decal::wire {
inline constexpr std::uint16_t get = 0x1020, save = 0x1021;
inline constexpr std::size_t headerBytes = 24, slotBytes = 104, appearanceBytes = headerBytes + 3*slotBytes;
using Bytes = std::array<std::uint8_t,appearanceBytes>;
inline void Put(std::uint8_t* p,std::uint64_t x,unsigned n) {for(unsigned i=0;i<n;++i){p[i]=static_cast<std::uint8_t>(x);x>>=8;}}
inline std::uint64_t Get(const std::uint8_t* p,unsigned n) {std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(p[i])<<(8*i);return x;}
inline void Real(std::uint8_t* p,double x) {std::uint64_t bits=0;static_assert(sizeof x==sizeof bits);std::memcpy(&bits,&x,8);Put(p,bits,8);}
inline double Real(const std::uint8_t* p) {const auto bits=Get(p,8);double x;std::memcpy(&x,&bits,8);return x;}
inline bool Encode(const Appearance& a,Bytes& out) {
    if(!Valid(a)||a.unitType>INT32_MAX||a.unitSerial>INT64_MAX||a.revision>INT64_MAX)return false;
    for(const auto& slot:a.slots)if(slot.sticker>INT32_MAX)return false;
    Bytes b{};Put(b.data(),1,4);Put(b.data()+4,a.unitType,4);Put(b.data()+8,a.unitSerial,8);Put(b.data()+16,a.revision,8);
    for(std::size_t i=0;i<3;++i){
        auto* s=b.data()+headerBytes+i*slotBytes;const auto& v=a.slots[i];
        Put(s,v.sticker,4);Put(s+4,v.positioned?1:0,4);
        if(!v.positioned)continue;
        const auto& p=v.placement;Put(s+8,p.anchor.geometry,8);Put(s+16,p.anchor.node,4);Put(s+20,p.anchor.triangle,4);
        for(unsigned j=0;j<3;++j)Real(s+24+j*8,p.anchor.barycentric[j]);
        Real(s+48,p.anchor.tangent.x);Real(s+56,p.anchor.tangent.y);Real(s+64,p.anchor.tangent.z);
        Real(s+72,p.width);Real(s+80,p.height);Real(s+88,p.rotation);Real(s+96,p.depth);
    }
    out=b;return true;
}
inline bool Decode(const std::uint8_t* b,std::size_t n,Appearance& out) {
    if(!b||n!=appearanceBytes||Get(b,4)!=1)return false;
    Appearance a;a.unitType=static_cast<std::uint32_t>(Get(b+4,4));a.unitSerial=Get(b+8,8);a.revision=Get(b+16,8);
    if(a.unitType>INT32_MAX||a.unitSerial>INT64_MAX||a.revision>INT64_MAX)return false;
    for(std::size_t i=0;i<3;++i){
        const auto* s=b+headerBytes+i*slotBytes;auto& v=a.slots[i];
        v.sticker=static_cast<std::uint32_t>(Get(s,4));const auto positioned=Get(s+4,4);if(v.sticker>INT32_MAX||positioned>1)return false;v.positioned=positioned!=0;
        if(!v.positioned){for(unsigned j=8;j<slotBytes;++j)if(s[j])return false;continue;}
        auto& p=v.placement;p.anchor.geometry=Get(s+8,8);p.anchor.node=static_cast<std::uint32_t>(Get(s+16,4));p.anchor.triangle=static_cast<std::uint32_t>(Get(s+20,4));
        for(unsigned j=0;j<3;++j)p.anchor.barycentric[j]=Real(s+24+j*8);
        p.anchor.tangent={Real(s+48),Real(s+56),Real(s+64)};
        p.width=Real(s+72);p.height=Real(s+80);p.rotation=Real(s+88);p.depth=Real(s+96);
    }
    if(!Valid(a))return false;out=a;return true;
}
} // namespace sdgo::surface_decal::wire
