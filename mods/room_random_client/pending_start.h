#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "cache_sync.h"

namespace room_random_client {
constexpr size_t kStartPacketSize=0xCD4;

struct PendingCall {
    void* parser=nullptr;
    std::array<uint8_t,kStartPacketSize> packet{};
    explicit operator bool() const {return parser!=nullptr;}
};

// Own the native start packet while its room generation is still waiting for
// complete human loadout records. The stored packet never aliases the network
// frame, and Take clears every retained native pointer before the caller
// invokes the original parser.
class PendingStart {
    bool active_=false;
    void* parser_=nullptr;
    uint8_t* profile_=nullptr;
    uint8_t* room_=nullptr;
    uint8_t* model_=nullptr;
    uint32_t key_=0;
    uint32_t local_=0;
    std::array<uint8_t,kStartPacketSize> packet_{};

    bool Same(const Context& c,void* parser) const {
        return active_ && c.profile && c.room && c.local &&
            c.profile==profile_ && c.room==room_ && c.model==model_ &&
            c.local==local_ && Read<uint32_t>(c.room,0)==key_ &&
            (!parser || parser==parser_);
    }
public:
    void Clear() {
        active_=false;parser_=nullptr;profile_=nullptr;room_=nullptr;
        model_=nullptr;key_=0;local_=0;packet_={};
    }
    bool Hold(const Context& c,void* parser,const uint8_t* packet) {
        Clear();
        if(!c.profile || !c.room || !c.local || !parser || !packet ||
           Read<uint16_t>(packet,0)!=0x06C3) return false;
        active_=true;parser_=parser;profile_=c.profile;room_=c.room;
        model_=c.model;key_=Read<uint32_t>(c.room,0);local_=c.local;
        std::memcpy(packet_.data(),packet,packet_.size());
        return true;
    }
    bool Active() const {return active_;}
    void* Parser() const {return parser_;}
    const uint8_t* Packet() const {return active_ ? packet_.data() : nullptr;}
    bool Matches(const Context& c,void* parser=nullptr) const {
        return Same(c,parser);
    }
    bool Revalidate(const Context& c) {
        if(active_ && !Same(c,nullptr))Clear();
        return active_;
    }
    PendingCall Take(const Context& c,void* parser=nullptr) {
        PendingCall call;
        if(!Same(c,parser)) {
            Clear();
            return call;
        }
        call.parser=parser_;
        call.packet=packet_;
        Clear();
        return call;
    }
};
}
