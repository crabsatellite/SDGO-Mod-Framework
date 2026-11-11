#pragma once

#include <stdint.h>

// Read-only access to one authoritative AI unit copied from the native battle
// start packet.  Callers provide the exact encoded pilot, NPC and form.  An
// expected_generation of zero is reserved for the first actor binding; later
// calls must pass the generation returned by that successful binding.
// Query on the native game thread. The provider never writes a packet or actor.
#define SDGO_AI_START_UNIT_VERSION 1u
#define SDGO_AI_START_UNIT_EXPORT "SdgoMatchAiGetStartUnitV1"

#pragma pack(push, 4)
struct SdgoAiStartUnitV1 {
    uint32_t size;
    uint32_t version;
    uint32_t pilot_id;
    uint32_t npc_id;
    int32_t form;
    uint32_t expected_generation;
    uint32_t generation;
    uint32_t max_hp;
};
#pragma pack(pop)

static_assert(sizeof(SdgoAiStartUnitV1) == 32, "AI start-unit ABI layout changed");

using SdgoAiStartUnitV1Fn = int(__cdecl*)(SdgoAiStartUnitV1* request);
