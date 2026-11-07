#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include "sdgo/game_test.h"
#include "sdgo/sdgo_api.h"

#pragma intrinsic(_ReturnAddress)

// Passive observation of the client's native server-error modal helper.
//
// Current native ABI at 006FD480:
//   uint32_t __cdecl ShowServerError(uint16_t opcode, int32_t error, uint8_t flag)
//
// The helper reads the arguments from [ebp+8], [ebp+0C] and [ebp+10],
// returns its modal result in EAX and ends in a plain ret. The hook records
// before calling the original because the modal may block the calling thread.
// It never suppresses, retries or changes an event.
class NativeErrorPopupProbe {
public:
    static constexpr uint32_t StackCapacity = 8;
    static constexpr uint32_t EventCapacity = 64;

    struct Observation {
        uint32_t sequence = 0;
        uint16_t opcode = 0;
        uint8_t flag = 0;
        uint8_t frame_count = 0;
        int32_t error = 0;
        uint32_t entry_last_error = 0;
        uint32_t thread_id = 0;
        uintptr_t caller = 0;
        uintptr_t stack[StackCapacity]{};
    };

    bool Install(
        const SdgoApi* api,
        SdgoMod* mod,
        const char* symbol = "test.native_error_popup") {
        if (!api || !api->HookCode || !mod || !symbol || active_) return false;
        api_ = api;
        mod_ = mod;
        active_ = this;
        if (!api->HookCode(
                mod,
                symbol,
                reinterpret_cast<void*>(&OnPopup),
                reinterpret_cast<void**>(&original_))) {
            active_ = nullptr;
            original_ = nullptr;
            api_ = nullptr;
            mod_ = nullptr;
            return false;
        }
        if (!original_) {
            active_ = nullptr;
            api_ = nullptr;
            mod_ = nullptr;
            return false;
        }
        return true;
    }

    uint32_t Sequence() const {
        AcquireSRWLockShared(&lock_);
        const uint32_t value = sequence_;
        ReleaseSRWLockShared(&lock_);
        return value;
    }

    bool Latest(Observation& out) const {
        AcquireSRWLockShared(&lock_);
        const uint32_t sequence = sequence_;
        if (sequence) out = events_[(sequence - 1) % EventCapacity];
        ReleaseSRWLockShared(&lock_);
        return sequence != 0;
    }

    // Copies retained events newer than `after` in sequence order. The
    // monotonic newest/oldest outputs let a test detect ring-buffer overwrite
    // without treating repeated opcode/error pairs as one event.
    uint32_t CopyAfter(
        uint32_t after,
        Observation* out,
        uint32_t capacity,
        uint32_t* oldest_retained = nullptr,
        uint32_t* newest = nullptr) const {
        if (!out || !capacity) return 0;
        AcquireSRWLockShared(&lock_);
        const uint32_t latest = sequence_;
        const uint32_t oldest =
            latest > EventCapacity ? latest - EventCapacity + 1 : (latest ? 1u : 0u);
        uint32_t first = after + 1;
        if (first < oldest) first = oldest;
        uint32_t count = 0;
        for (uint32_t sequence = first;
             sequence && sequence <= latest && count < capacity;
             ++sequence) {
            out[count++] = events_[(sequence - 1) % EventCapacity];
        }
        if (oldest_retained) *oldest_retained = oldest;
        if (newest) *newest = latest;
        ReleaseSRWLockShared(&lock_);
        return count;
    }

    // Convenience drain for a GameTest poll. `cursor` is advanced to the
    // newest observed invocation, including repeated native errors.
    void ObserveSince(const SdgoGameTestContextV1* context, uint32_t& cursor) const {
        if (!context || !context->Observe) return;
        Observation records[EventCapacity]{};
        uint32_t oldest = 0;
        uint32_t newest = 0;
        const uint32_t count =
            CopyAfter(cursor, records, EventCapacity, &oldest, &newest);
        if (oldest && cursor + 1 < oldest) {
            char message[128];
            sprintf_s(
                message,
                "native error popup probe retained events from sequence=%u; requested after=%u",
                oldest,
                cursor);
            context->Observe(message);
        }
        for (uint32_t index = 0; index < count; ++index) {
            const auto& event = records[index];
            char message[512];
            sprintf_s(
                message,
                "native error popup sequence=%u opcode=%04X error=%d/0x%08X "
                "flag=%u caller=%p thread=%lu entry_last_error=%lu frames=%u "
                "stack=%p/%p/%p/%p/%p/%p/%p/%p",
                event.sequence,
                static_cast<unsigned>(event.opcode),
                event.error,
                static_cast<unsigned>(event.error),
                static_cast<unsigned>(event.flag),
                reinterpret_cast<void*>(event.caller),
                static_cast<unsigned long>(event.thread_id),
                static_cast<unsigned long>(event.entry_last_error),
                static_cast<unsigned>(event.frame_count),
                reinterpret_cast<void*>(event.stack[0]),
                reinterpret_cast<void*>(event.stack[1]),
                reinterpret_cast<void*>(event.stack[2]),
                reinterpret_cast<void*>(event.stack[3]),
                reinterpret_cast<void*>(event.stack[4]),
                reinterpret_cast<void*>(event.stack[5]),
                reinterpret_cast<void*>(event.stack[6]),
                reinterpret_cast<void*>(event.stack[7]));
            context->Observe(message);
        }
        if (newest > cursor) cursor = newest;
    }

private:
    using PopupFn = uint32_t(__cdecl*)(uint16_t, int32_t, uint8_t);

    static uint32_t __cdecl OnPopup(uint16_t opcode, int32_t error, uint8_t flag) {
        NativeErrorPopupProbe* const owner = active_;
        PopupFn const original = owner ? owner->original_ : nullptr;
        if (!original) return 0;

        const DWORD entryLastError = GetLastError();
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        PVOID stack[StackCapacity]{};
        const USHORT frameCount =
            CaptureStackBackTrace(0, StackCapacity, stack, nullptr);
        const DWORD threadId = GetCurrentThreadId();

        owner->Record(
            opcode,
            error,
            flag,
            entryLastError,
            threadId,
            caller,
            stack,
            frameCount);

        // Instrumentation may change the thread's last-error value. Restore the
        // exact entry value immediately before the one original invocation.
        // Do no work after it, so the original result and last-error state pass
        // straight back to the native caller.
        SetLastError(entryLastError);
        return original(opcode, error, flag);
    }

    void Record(
        uint16_t opcode,
        int32_t error,
        uint8_t flag,
        DWORD entryLastError,
        DWORD threadId,
        uintptr_t caller,
        PVOID const* stack,
        USHORT frameCount) {
        Observation event{};
        event.opcode = opcode;
        event.flag = flag;
        event.error = error;
        event.entry_last_error = entryLastError;
        event.thread_id = threadId;
        event.caller = caller;
        event.frame_count = static_cast<uint8_t>(
            frameCount < StackCapacity ? frameCount : StackCapacity);
        for (uint32_t index = 0; index < event.frame_count; ++index)
            event.stack[index] = reinterpret_cast<uintptr_t>(stack[index]);

        AcquireSRWLockExclusive(&lock_);
        event.sequence = ++sequence_;
        events_[(event.sequence - 1) % EventCapacity] = event;
        ReleaseSRWLockExclusive(&lock_);

        // The original call can synchronously block in its modal. Persist the
        // evidence now rather than depending on a later GameTest poll.
        if (api_ && api_->Log)
            api_->Log(
                mod_,
                SDGO_LOG_WARN,
                "native error popup sequence=%u opcode=%04X error=%d/0x%08X "
                "flag=%u caller=%p thread=%lu entry_last_error=%lu frames=%u "
                "stack=%p/%p/%p/%p/%p/%p/%p/%p",
                event.sequence,
                static_cast<unsigned>(event.opcode),
                event.error,
                static_cast<unsigned>(event.error),
                static_cast<unsigned>(event.flag),
                reinterpret_cast<void*>(event.caller),
                static_cast<unsigned long>(event.thread_id),
                static_cast<unsigned long>(event.entry_last_error),
                static_cast<unsigned>(event.frame_count),
                reinterpret_cast<void*>(event.stack[0]),
                reinterpret_cast<void*>(event.stack[1]),
                reinterpret_cast<void*>(event.stack[2]),
                reinterpret_cast<void*>(event.stack[3]),
                reinterpret_cast<void*>(event.stack[4]),
                reinterpret_cast<void*>(event.stack[5]),
                reinterpret_cast<void*>(event.stack[6]),
                reinterpret_cast<void*>(event.stack[7]));
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    PopupFn original_ = nullptr;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    uint32_t sequence_ = 0;
    Observation events_[EventCapacity]{};
    inline static NativeErrorPopupProbe* active_ = nullptr;
};
