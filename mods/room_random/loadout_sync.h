#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Native client notifications carry a bounded random-round marker in route
// bits that the patched client restores before native dispatch. Native 06C3
// byte +0x1F contains a 0..12 participant count. Its otherwise-unused high
// nibble carries the same 1..15 round only on the wire; epoch zero is reserved
// for post-settlement restore notices. Patched clients retain that identity
// privately and clear the added bits before native consumers.
// The first 0x10F-byte participant record follows at +0x20.
// The broadcaster replaces header +2 with each recipient's pilot ID.
namespace room_random_sync {
constexpr size_t kUnitSize=0x99, kShipSize=0x2BC, kPayload=0x12;
constexpr uint16_t kUnitOpcode=0x0861, kShipOpcode=0x098B;
constexpr size_t kStartParticipantCountOffset=0x1F;
constexpr uint8_t kStartParticipantCountMask=0x0F;
constexpr uint8_t kStartEpochMask=0xF0;
constexpr uint16_t kRoundSignature=0xA500;

inline uint8_t StartParticipantCount(uint8_t value) {
    return value&kStartParticipantCountMask;
}
inline uint8_t StartEpoch(uint8_t value) {
    return static_cast<uint8_t>((value&kStartEpochMask)>>4);
}
inline uint8_t TaggedStartCount(uint8_t participants,uint8_t epoch) {
    return static_cast<uint8_t>(
        (participants&kStartParticipantCountMask)|
        ((epoch&kStartParticipantCountMask)<<4));
}

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
constexpr bool IsRestoreRoute(uint32_t value) {
    return static_cast<uint16_t>(value>>16)==kRoundSignature;
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
