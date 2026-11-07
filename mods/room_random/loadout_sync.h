#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Native client notifications with a bounded random-round marker in unused
// route bits. Patched clients restore the native route before dispatch.
// The broadcaster replaces header +2 with each recipient's pilot ID.
namespace room_random_sync {
constexpr size_t kUnitSize=0x99, kShipSize=0x2BC, kPayload=0x12;
constexpr uint16_t kUnitOpcode=0x0861, kShipOpcode=0x098B;
constexpr size_t kStartEpochOffset=0x1F;
constexpr uint16_t kRoundSignature=0xA500;

struct Route {
    uint16_t room=0;
    uint8_t epoch=0;
    bool tagged=false;
};

constexpr uint32_t TaggedRoute(uint16_t room,uint8_t epoch) {
    return (static_cast<uint32_t>(kRoundSignature|epoch)<<16)|room;
}
constexpr bool HasRoundSignature(uint32_t value) {
    return static_cast<uint8_t>(value>>24)==static_cast<uint8_t>(kRoundSignature>>8);
}
constexpr Route DecodeRoute(uint32_t value) {
    const auto tag=static_cast<uint16_t>(value>>16);
    const auto epoch=static_cast<uint8_t>(tag);
    return {static_cast<uint16_t>(value),epoch,
            static_cast<uint8_t>(tag>>8)==static_cast<uint8_t>(kRoundSignature>>8)
                && epoch!=0};
}
template<size_t N> std::array<uint8_t,kPayload+N> Packet(
    uint16_t opcode,uint32_t owner,uint32_t slot,uint32_t route,const std::array<uint8_t,N>& data) {
    std::array<uint8_t,kPayload+N> out{};
    std::memcpy(out.data(),&opcode,sizeof(opcode));
    std::memcpy(out.data()+6,&owner,sizeof(owner));
    std::memcpy(out.data()+10,&slot,sizeof(slot));
    std::memcpy(out.data()+14,&route,sizeof(route));
    std::memcpy(out.data()+kPayload,data.data(),data.size());
    return out;
}
}
