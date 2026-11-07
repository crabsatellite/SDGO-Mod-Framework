#pragma once
#include <stdint.h>

// Read-only, credential-free diagnostics. Observing this interface never starts
// a login, changes a native state or completes a recovery action.
#define SDGO_LOGIN_RECOVERY_SNAPSHOT_EXPORT "SdgoLoginRecoveryGetSnapshotV1"
struct SdgoLoginRecoverySnapshotV1 {
    uint32_t size, version;
    uint64_t generation;
    uint32_t state, outcome, automatic_retries, action_id;
    uint32_t connection_generation, rebuilds, retry_sends, auth_replies;
    uint32_t last_reject, app_state, gate_state;
    uint32_t purged_packets, dropped_callbacks, credentials_present;
    uint32_t return_ready, recovery_active, interaction_wait;
};
using SdgoLoginRecoveryGetSnapshotV1Fn = int(__cdecl*)(SdgoLoginRecoverySnapshotV1*);
