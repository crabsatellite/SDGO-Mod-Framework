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
}
