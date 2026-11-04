#pragma once
#include <stdint.h>

#define SDGO_PVP_AI_GATE_STATUS_EXPORT "SdgoPvpAiClientGetGateStatusV1"
struct SdgoPvpAiClientGateStatusV1 {
    uint32_t size,version,battle_start_sequence,authority_count;
    uint32_t last_attacker_pilot,last_attacker_object,last_gate_object,last_generation;
};
static_assert(sizeof(SdgoPvpAiClientGateStatusV1)==32,"gate witness ABI changed");
typedef int(__cdecl* SdgoPvpAiClientGetGateStatusV1Fn)(SdgoPvpAiClientGateStatusV1*);
