#pragma once
#include <stdint.h>
#define SDGO_AI_DIALOGUE_OBSERVER_EXPORT "SdgoAiDialogueGetStatusV1"
struct SdgoAiDialogueStatusV1 {
    uint32_t size,version,battle,requests,completed,failures,waiting,participants;
    uint32_t last_kind,last_speaker,last_target,enabled,received,room_visibility;
    wchar_t last_line[256];
};
typedef int(__cdecl* SdgoAiDialogueGetStatusV1Fn)(SdgoAiDialogueStatusV1*);
