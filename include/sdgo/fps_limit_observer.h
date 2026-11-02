#pragma once
#include <stdint.h>

#define SDGO_FPS_LIMIT_SNAPSHOT_EXPORT "SdgoFpsLimitGetSnapshotV1"
struct SdgoFpsLimitSnapshotV1 {
    uint32_t size,version,render_cap,update_hz,present_calls,waited_calls;
    uint64_t qpc_frequency,period_ticks;
};
// Passive main-thread diagnostics; update_hz stays at native 60 independently
// of render_cap. No settings or native state are modified by the observer.
typedef int (__cdecl* SdgoFpsLimitGetSnapshotV1Fn)(SdgoFpsLimitSnapshotV1*);
