#pragma once
#include <stdint.h>

// Optional read-only main-thread fanout from match_ai. The borrowed native
// pointers are callback-local. Consumers must copy facts before returning,
// perform no network I/O here and never modify mode/packet state.
#define SDGO_MATCH_AI_EVENTS_EXPORT "SdgoMatchAiRegisterEventsV1"
struct SdgoMatchAiEventsV1 {
    uint32_t size,version;
    void (__cdecl* tick)(uint8_t* mode,uint32_t battle_sequence,void* user);
    void (__cdecl* packet)(uint16_t opcode,const uint8_t* packet,void* user);
    void* user;
};
typedef int(__cdecl* SdgoMatchAiRegisterEventsV1Fn)(const SdgoMatchAiEventsV1*);
#define SDGO_MATCH_AI_LIFE_COST_EXPORT "SdgoMatchAiGetLifeCostV1"
typedef uint32_t(__cdecl* SdgoMatchAiGetLifeCostV1Fn)(uint32_t pilot);
