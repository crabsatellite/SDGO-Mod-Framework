#pragma once

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <type_traits>

#include "sdgo/sdgo_api.h"

class NativePacketIngressProbe;

namespace native_packet_ingress_bridge {

using IngressFn = int(__cdecl*)(
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    void*,
    std::uint32_t);

inline NativePacketIngressProbe* active = nullptr;
inline IngressFn original = nullptr;
inline const DWORD defaultMxcsr = 0x1F80;

void __cdecl Dispatch(
    std::uint32_t arg0,
    std::uint32_t arg1,
    std::uint32_t sourceHandle,
    const void* packet,
    std::uint32_t size,
    std::uintptr_t returnAddress) noexcept;

// Preserve the exact native entry state around passive instrumentation, then
// tail-jump to the original. The original therefore runs exactly once with its
// five untouched cdecl arguments and returns directly to the native caller.
__declspec(naked) inline int __cdecl Hook()
{
    __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        mov [esp + 200h], eax
        mov edx, dword ptr fs:[34h]
        mov [esp + 204h], edx
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [defaultMxcsr]
        cld

        // After pushfd/pushad, eax points 36 bytes below the native return
        // address. Pass that entry return address plus the original arguments
        // to the POD-only observer.
        push dword ptr [eax + 36]
        push dword ptr [eax + 56]
        push dword ptr [eax + 52]
        push dword ptr [eax + 48]
        push dword ptr [eax + 44]
        push dword ptr [eax + 40]
        call Dispatch
        add esp, 18h

        fxrstor [esp]
        mov edx, [esp + 204h]
        mov dword ptr fs:[34h], edx
        mov esp, [esp + 200h]
        popad
        popfd
        jmp dword ptr [original]
    }
}

} // namespace native_packet_ingress_bridge

// Passive observation at the client's common native packet ingress.
//
// Verified native ABI at RVA 00090CF0:
//   int __cdecl ingress(
//       uint32 arg0, uint32 arg1, uint32 sourceHandle,
//       void* packet, uint32 size)
//
// At most the first eight packet bytes are copied. Short packets retain zeroes
// only as unavailable placeholders, reported through copied_bytes and the
// per-byte availability mask. In particular, the final uint16_t is raw data:
// for opcode 069C it is not interpreted as an error code.
class NativePacketIngressProbe
{
public:
    static constexpr std::uint32_t StackCapacity = 8;
    static constexpr std::uint32_t EventCapacity = 32;

#pragma pack(push, 1)
    struct PacketPrefix
    {
        std::uint16_t opcode;
        std::uint32_t recipient;
        std::uint16_t raw16;
    };
#pragma pack(pop)

    static_assert(sizeof(PacketPrefix) == 8, "native packet prefix changed");
    static_assert(
        std::is_trivial<PacketPrefix>::value &&
            std::is_standard_layout<PacketPrefix>::value,
        "native packet prefix must remain POD");

    // Optional byte-sequence search (OC room systems): every inbound packet is
    // searched within its native size; the first match per opcode is kept.
    struct NeedleMatch
    {
        std::uint16_t opcode;
        std::uint32_t size;
        std::uint32_t offset;
        std::uint32_t count;
    };
    static constexpr std::uint32_t NeedleCapacity = 16;

    void SetNeedle(const std::uint8_t* bytes, std::uint32_t length)
    {
        AcquireSRWLockExclusive(&needleLock_);
        needleLength_ = length <= sizeof(needle_) ? length : 0;
        if (needleLength_)
            std::memcpy(needle_, bytes, needleLength_);
        matchCount_ = 0;
        ReleaseSRWLockExclusive(&needleLock_);
    }

    std::uint32_t NeedleMatches(NeedleMatch* out, std::uint32_t capacity)
    {
        AcquireSRWLockShared(&needleLock_);
        const std::uint32_t count = matchCount_ < capacity ? matchCount_ : capacity;
        std::memcpy(out, matches_, count * sizeof(NeedleMatch));
        ReleaseSRWLockShared(&needleLock_);
        return count;
    }

    bool Installed() const { return api_ != nullptr; }

    bool Install(
        const SdgoApi* api,
        SdgoMod* mod,
        const char* symbol = "test.native_packet_ingress")
    {
        using namespace native_packet_ingress_bridge;
        if (!api || !api->HookCode || !mod || !symbol || active)
            return false;

        api_ = api;
        mod_ = mod;
        active = this;
        original = nullptr;
        if (!api->HookCode(
                mod,
                symbol,
                reinterpret_cast<void*>(&Hook),
                reinterpret_cast<void**>(&original)) ||
            !original) {
            active = nullptr;
            original = nullptr;
            api_ = nullptr;
            mod_ = nullptr;
            return false;
        }
        return true;
    }

private:
    friend void __cdecl native_packet_ingress_bridge::Dispatch(
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        const void*,
        std::uint32_t,
        std::uintptr_t) noexcept;

    static bool Tracked(std::uint16_t opcode)
    {
        return opcode == 0x0880 ||
            opcode == 0x0A2D ||
            opcode == 0x069C ||
            opcode == 0x0883 ||
            opcode == 0x096F ||
            opcode == 0x087D ||
            opcode == 0x087E;
    }

    void Observe(
        std::uint32_t sourceHandle,
        const void* packet,
        std::uint32_t size,
        std::uintptr_t returnAddress) noexcept
    {
        if (!packet || size < sizeof(std::uint16_t))
            return;

        PacketPrefix prefix = {};
        const std::uint8_t copiedBytes = static_cast<std::uint8_t>(
            size < sizeof(prefix) ? size : sizeof(prefix));
        const std::uint8_t availabilityMask = static_cast<std::uint8_t>(
            copiedBytes == sizeof(prefix)
                ? 0xFFu
                : (1u << copiedBytes) - 1u);
        // This is the only packet-data copy performed by the probe. Never read
        // beyond the native size, including the short two-byte 0880 case.
        std::memcpy(&prefix, packet, copiedBytes);
        SearchNeedle(static_cast<const std::uint8_t*>(packet), size, prefix.opcode);
        if (!Tracked(prefix.opcode))
            return;

        const LONG sequence = InterlockedIncrement(&sequence_);
        if (sequence <= 0 ||
            sequence > static_cast<LONG>(EventCapacity))
            return;

        PVOID frames[StackCapacity] = {};
        const USHORT frameCount =
            CaptureStackBackTrace(0, StackCapacity, frames, nullptr);
        const std::uint8_t retainedFrames = static_cast<std::uint8_t>(
            frameCount < StackCapacity ? frameCount : StackCapacity);
        if (api_ && api_->Log) {
            const std::uint8_t* const bytes =
                reinterpret_cast<const std::uint8_t*>(&prefix);
            char recipientText[32] = "unavailable";
            char raw16Text[24] = "unavailable";
            if (copiedBytes >= 6) {
                std::snprintf(
                    recipientText,
                    sizeof(recipientText),
                    "%lu/0x%08lX",
                    static_cast<unsigned long>(prefix.recipient),
                    static_cast<unsigned long>(prefix.recipient));
            }
            if (copiedBytes >= sizeof(PacketPrefix)) {
                std::snprintf(
                    raw16Text,
                    sizeof(raw16Text),
                    "%u/0x%04X",
                    static_cast<unsigned>(prefix.raw16),
                    static_cast<unsigned>(prefix.raw16));
            }
            api_->Log(
                mod_,
                SDGO_LOG_INFO,
                "native packet ingress sequence=%u opcode=%04X size=%lu "
                "copied_bytes=%u availability_mask=%02X "
                "bytes=%02X/%02X/%02X/%02X/%02X/%02X/%02X/%02X "
                "recipient32=%s error16_raw=%s "
                "packet=%p source_handle=%lu/0x%08lX "
                "return_address=%p frames=%u "
                "stack=%p/%p/%p/%p/%p/%p/%p/%p",
                static_cast<unsigned>(sequence),
                static_cast<unsigned>(prefix.opcode),
                static_cast<unsigned long>(size),
                static_cast<unsigned>(copiedBytes),
                static_cast<unsigned>(availabilityMask),
                static_cast<unsigned>(bytes[0]),
                static_cast<unsigned>(bytes[1]),
                static_cast<unsigned>(bytes[2]),
                static_cast<unsigned>(bytes[3]),
                static_cast<unsigned>(bytes[4]),
                static_cast<unsigned>(bytes[5]),
                static_cast<unsigned>(bytes[6]),
                static_cast<unsigned>(bytes[7]),
                recipientText,
                raw16Text,
                const_cast<void*>(packet),
                static_cast<unsigned long>(sourceHandle),
                static_cast<unsigned long>(sourceHandle),
                reinterpret_cast<void*>(returnAddress),
                static_cast<unsigned>(retainedFrames),
                frames[0],
                frames[1],
                frames[2],
                frames[3],
                frames[4],
                frames[5],
                frames[6],
                frames[7]);
        }
    }

    void SearchNeedle(const std::uint8_t* packet, std::uint32_t size, std::uint16_t opcode) noexcept
    {
        AcquireSRWLockExclusive(&needleLock_);
        if (needleLength_ && size >= needleLength_) {
            for (std::uint32_t offset = 0; offset + needleLength_ <= size; ++offset) {
                if (std::memcmp(packet + offset, needle_, needleLength_) != 0)
                    continue;
                std::uint32_t slot = 0;
                while (slot < matchCount_ && matches_[slot].opcode != opcode)
                    ++slot;
                if (slot < matchCount_) {
                    ++matches_[slot].count;
                } else if (matchCount_ < NeedleCapacity) {
                    matches_[matchCount_++] = NeedleMatch{opcode, size, offset, 1};
                }
                break;
            }
        }
        ReleaseSRWLockExclusive(&needleLock_);
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    volatile LONG sequence_ = 0;
    SRWLOCK needleLock_ = SRWLOCK_INIT;
    std::uint8_t needle_[16] = {};
    std::uint32_t needleLength_ = 0;
    NeedleMatch matches_[NeedleCapacity] = {};
    std::uint32_t matchCount_ = 0;
};

inline void __cdecl native_packet_ingress_bridge::Dispatch(
    std::uint32_t arg0,
    std::uint32_t arg1,
    std::uint32_t sourceHandle,
    const void* packet,
    std::uint32_t size,
    std::uintptr_t returnAddress) noexcept
{
    (void)arg0;
    (void)arg1;
    NativePacketIngressProbe* const owner = active;
    if (owner)
        owner->Observe(sourceHandle, packet, size, returnAddress);
}
