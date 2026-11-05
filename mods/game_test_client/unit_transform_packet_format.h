#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sdgo_test::transforms {
struct Packet {
    std::uint32_t kind=0,pilot=0,target=0;
    std::uint16_t opcode=0,error=0;
    std::uint8_t direction=0;
};
inline bool DecodeReply(const unsigned char* raw,std::size_t bytes,Packet& output){
    output={};if(!raw||bytes<8)return false;
    output.kind=1;std::memcpy(&output.opcode,raw,2);std::memcpy(&output.error,raw+6,2);return true;
}
inline bool DecodeNotice(const unsigned char* raw,std::size_t bytes,Packet& output){
    output={};if(!raw||bytes<19)return false;
    output.kind=2;std::memcpy(&output.opcode,raw,2);std::memcpy(&output.pilot,raw+6,4);
    std::memcpy(&output.target,raw+10,4);output.direction=raw[14];return true;
}
}
