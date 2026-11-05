#pragma once
#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <cstdint>
#include "sdgo/battle_human_unit_observer.h"

namespace sdgo::unit_forms {

// State belongs to exactly one hook-owning DLL. Linking/including this helper
// in two DLLs does not create a shared provider; consumers use the export above.
class HumanUnitObserverProvider {
public:
    int Register(const SdgoBattleHumanUnitObserverSinkV1* input) {
        SdgoBattleHumanUnitObserverSinkV1 sink{};
        if (!ReadSink(input, &sink)) return 0;
        const auto previous = InterlockedCompareExchange(&registration_, 1, 0);
        if (previous != 0) return previous == 2 &&
            InterlockedCompareExchangePointer(&callback_, nullptr, nullptr) == reinterpret_cast<void*>(sink.callback) &&
            InterlockedCompareExchangePointer(&user_, nullptr, nullptr) == sink.user;
        InterlockedExchangePointer(&user_, sink.user);
        InterlockedExchangePointer(&callback_, reinterpret_cast<void*>(sink.callback));
        InterlockedExchange(&registration_, 2);return 1;
    }
    std::uint64_t Next() { return static_cast<std::uint64_t>(InterlockedIncrement64(&sequence_)); }
    int ReadStatus(SdgoBattleHumanUnitObserverStatusV1* status) {
        __try {
            if (!status || status->size != sizeof(*status) || status->version != 1) return 0;
            status->registration_state = static_cast<std::uint32_t>(InterlockedCompareExchange(&registration_, 0, 0));
            status->reserved = 0;
            status->last_sequence = static_cast<std::uint64_t>(InterlockedCompareExchange64(&sequence_, 0, 0));
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    // Returns a callback exception only once, for the hook owner's diagnostics.
    // A fault disables this sink for the remainder of the process lifetime.
    std::uint32_t Publish(std::uint32_t type, std::uint64_t sequence, const void* room,
                          const void* source, const void* player, std::uint8_t rule,
                          std::uint8_t mode, std::int32_t nativeResult) {
        if (InterlockedCompareExchange(&registration_, 2, 2) != 2) return 0;
        const auto callback = reinterpret_cast<SdgoBattleHumanUnitObserverCallbackV1>(
            InterlockedCompareExchangePointer(&callback_, nullptr, nullptr));
        if (!callback) return 0;
        void* user = InterlockedCompareExchangePointer(&user_, nullptr, nullptr);
        const DWORD error = GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
        SdgoBattleHumanUnitEventV1 event{};
        event.size = sizeof(event);event.version = SDGO_BATTLE_HUMAN_UNIT_OBSERVER_VERSION;
        event.type = type;event.native_result = nativeResult;event.sequence = sequence;
        event.thread_id = GetCurrentThreadId();event.room = room;event.source = source;event.player = player;
        event.rule = rule;event.mode = mode;
        __asm { fninit }
        _mm_setcsr(0x1f80);
        __asm { cld }
        const auto fault = Invoke(callback, &event, user);
        __asm { cld }
        if (fault) { InterlockedExchange(&registration_, 3);InterlockedExchangePointer(&callback_, nullptr); }
        _fxrstor(fp);SetLastError(error);
        return fault && InterlockedCompareExchange(&faultReported_, 1, 0) == 0 ? fault : 0;
    }
private:
    static bool ReadSink(const SdgoBattleHumanUnitObserverSinkV1* input, SdgoBattleHumanUnitObserverSinkV1* output) {
        __try {
            if (!input || input->size != sizeof(*input) || input->version != SDGO_BATTLE_HUMAN_UNIT_OBSERVER_VERSION || !input->callback)
                return false;
            *output = *input;return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static std::uint32_t Invoke(SdgoBattleHumanUnitObserverCallbackV1 callback, const SdgoBattleHumanUnitEventV1* event, void* user) {
        __try { callback(event, user);return 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    }
    PVOID volatile callback_ = nullptr;
    PVOID volatile user_ = nullptr;
    volatile LONG registration_ = 0, faultReported_ = 0;
    alignas(8) volatile LONG64 sequence_ = 0;
};
} // namespace sdgo::unit_forms
