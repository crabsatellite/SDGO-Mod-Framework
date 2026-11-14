#pragma once
#include "sdgo/rgb_paint_protocol.h"

namespace sdgo::rgb_paint::identity {
// A room load message names a seat, not a unit type or a pilot ID. Resolve
// that seat's owned unit before the native asynchronous queue drops the seat.
inline bool RoomRecord(const std::uint8_t* entry,std::size_t length,
                       const std::uint8_t* style,std::size_t styleLength,
                       std::uint64_t session,Record& out){
    if(!entry||length!=0x48e||!style||styleLength<16||!session)return false;
    const auto pilot=wire::Get(entry+0x429,4);
    if(!pilot||(pilot&0x80000000u))return false;
    const auto* unit=entry+0x49;
    Record r;r.type=static_cast<std::uint32_t>(wire::Get(unit,4));
    r.state.identity={session,wire::Get(unit+8,8)};r.state.revision=1;
    if(r.type!=wire::Get(style,4))return false;
    for(unsigned i=0;i<6;++i){
        r.native[i]=static_cast<std::uint16_t>(wire::Get(unit+46+i*2,2));
        if(r.native[i]!=wire::Get(style+4+i*2,2))return false;
        r.state.colors[i]=FromNative(r.native[i]);
    }
    wire::Bytes valid;if(!wire::Encode(r,valid))return false;
    out=r;return true;
}
}
