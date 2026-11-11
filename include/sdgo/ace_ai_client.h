#pragma once
#include <stdint.h>

// Called on the native game thread after binding an authoritative Ace actor.
// The battle-start generation is shared by every slot in that native packet.
// The provider owns the text and uses the original game nameplate renderer.
#define SDGO_ACE_AI_SET_NAMEPLATE_EXPORT "SdgoAceAiSetNameplateV1"
using SdgoAceAiSetNameplateV1Fn = int(__cdecl*)(
    uint8_t* unit, const wchar_t* pilot_name, uint32_t start_generation);
