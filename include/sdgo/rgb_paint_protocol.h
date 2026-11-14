#pragma once
#include "sdgo/rgb_paint.h"
#include <cstring>
#include <climits>
namespace sdgo::rgb_paint {
struct Record {
    std::uint32_t type=0;
    Snapshot state;
    std::array<std::uint16_t,6> native{};
};
namespace wire {
inline constexpr std::uint16_t get=0x1030,save=0x1031;
inline constexpr std::size_t bytes=60;
using Bytes=std::array<std::uint8_t,bytes>;
inline void Put(std::uint8_t* p,std::uint64_t x,unsigned n){for(unsigned i=0;i<n;++i){p[i]=static_cast<std::uint8_t>(x);x>>=8;}}
inline std::uint64_t Get(const std::uint8_t* p,unsigned n){std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(p[i])<<(8*i);return x;}
inline bool Encode(const Record& r,Bytes& out){
    if(!r.type||r.type>INT32_MAX||!r.state.identity.unit||r.state.identity.unit>INT64_MAX||!r.state.revision||r.state.revision>INT64_MAX)return false;
    Bytes b{};Put(b.data(),1,4);Put(b.data()+4,r.type,4);Put(b.data()+8,r.state.identity.unit,8);Put(b.data()+16,r.state.revision,8);
    unsigned mask=0;for(unsigned i=0;i<6;++i){Put(b.data()+24+i*2,r.native[i],2);if(const auto c=r.state.colors[i]){mask|=1u<<i;b[40+i*3]=c->r;b[41+i*3]=c->g;b[42+i*3]=c->b;}}
    Put(b.data()+36,mask,4);out=b;return true;
}
inline bool Decode(const std::uint8_t* b,std::size_t n,Record& out){
    if(!b||n!=bytes||Get(b,4)!=1||Get(b+36,4)>63||b[58]||b[59])return false;
    Record r;r.type=static_cast<std::uint32_t>(Get(b+4,4));r.state.identity.unit=Get(b+8,8);r.state.revision=Get(b+16,8);
    for(unsigned i=0;i<6;++i){r.native[i]=static_cast<std::uint16_t>(Get(b+24+i*2,2));if(b[36]&(1u<<i))r.state.colors[i]=Rgb{b[40+i*3],b[41+i*3],b[42+i*3]};else if(b[40+i*3]||b[41+i*3]||b[42+i*3])return false;}
    Bytes canonical;if(!Encode(r,canonical)||std::memcmp(b,canonical.data(),bytes))return false;out=r;return true;
}
}
}
