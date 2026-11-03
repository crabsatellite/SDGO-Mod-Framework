#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include "sdgo/friend_unit_gift_protocol.h"
namespace sdgo::friend_unit_gift::client {
// The network decoder can run on a worker. Only copy our fixed-size namespace
// there; the existing UI timer owns transport/model/control callbacks.
class NativeReplyQueue {
public:
    bool Capture(const std::uint8_t* packet,std::uint32_t size) noexcept {
        if(!packet || size!=kCarrierBytes || wire::Read16(packet)!=kNativeReplyOpcode ||
           std::memcmp(packet+kReplyEnvelopeOffset,kEnvelopeMagic.data(),kEnvelopeMagic.size())) return false;
        AcquireSRWLockExclusive(&lock_);
        if(count_<packets_.size()) {
            std::memcpy(packets_[(first_+count_)%packets_.size()].data(),packet,kCarrierBytes);++count_;
        } else ++dropped_;
        ReleaseSRWLockExclusive(&lock_);
        return true; // Even malformed/stale packets in our namespace bypass native mail parsing.
    }
    bool Pop(std::array<std::uint8_t,kCarrierBytes>* output) noexcept {
        if(!output)return false;
        AcquireSRWLockExclusive(&lock_);
        const bool present=count_!=0;
        if(present){*output=packets_[first_];first_=(first_+1)%packets_.size();--count_;}
        ReleaseSRWLockExclusive(&lock_);return present;
    }
    std::uint32_t dropped() const noexcept {
        AcquireSRWLockShared(&lock_);const auto n=dropped_;ReleaseSRWLockShared(&lock_);return n;
    }
private:
    mutable SRWLOCK lock_=SRWLOCK_INIT;
    std::array<std::array<std::uint8_t,kCarrierBytes>,8> packets_{};
    std::size_t first_=0,count_=0;
    std::uint32_t dropped_=0;
};

// The normal 0x0699 detail reply carries the authentic packed 153-byte unit
// at offset 14. Stock handlers only synchronize it on the active hangar page.
// Retain only our outstanding read and let the UI thread call the same native
// cache writer. Observing this reply never consumes or rewrites the packet.
class NativeUnitDetailQueue {
public:
    using Record=std::array<std::uint8_t,153>;
    void Expect(std::uint64_t session,std::uint64_t uid) noexcept {
        AcquireSRWLockExclusive(&lock_);
        session_=session;uid_=uid;queued_=false;
        ReleaseSRWLockExclusive(&lock_);
    }
    bool Observe(const std::uint8_t* packet,std::uint32_t size) noexcept {
        if (!packet || size!=167 || wire::Read16(packet)!=0x0699) return false;
        const auto* record=packet+14;
        const auto uid=wire::Read64(record+8);
        const auto slot=wire::Read16(record+4);
        if (!wire::Read32(record) || !uid || !slot || slot>1200) return false;
        AcquireSRWLockExclusive(&lock_);
        const bool matches=session_ && uid_ && uid_==uid;
        if (matches) { std::memcpy(record_.data(),record,record_.size());queued_=true; }
        ReleaseSRWLockExclusive(&lock_);return matches;
    }
    bool Pop(std::uint64_t session,Record* output) noexcept {
        if (!output) return false;
        AcquireSRWLockExclusive(&lock_);
        const bool present=queued_ && session && session==session_;
        if (present) { *output=record_;queued_=false;uid_=0; }
        if (session!=session_) { queued_=false;uid_=0;session_=0; }
        ReleaseSRWLockExclusive(&lock_);return present;
    }
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    Record record_{};
    std::uint64_t session_=0,uid_=0;
    bool queued_=false;
};
}
