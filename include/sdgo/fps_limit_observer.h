#pragma once
#include <stdint.h>

#define SDGO_FPS_LIMIT_SNAPSHOT_EXPORT "SdgoFpsLimitGetSnapshotV1"
struct SdgoFpsLimitSnapshotV1 {
    uint32_t size,version,render_cap,update_hz,present_calls,waited_calls;
    uint64_t qpc_frequency,period_ticks;
};
// Passive main-thread diagnostics; no settings or native state are modified.
typedef int (__cdecl* SdgoFpsLimitGetSnapshotV1Fn)(SdgoFpsLimitSnapshotV1*);
