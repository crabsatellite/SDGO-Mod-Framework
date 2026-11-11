#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include "sdgo_api.h"

// Process-lifetime callbacks share hook sites already owned by the PvP Mods.
// Registration happens during Mod initialization, before native gameplay starts.
struct SdgoMissionServerLifecycleV1 {
    uint32_t size;
    uint32_t version;
    void (__cdecl* tick)(uint8_t* room, uint32_t now);
    void (__cdecl* end)(uint8_t* room);
};
struct SdgoMissionServerLifecycleV2 {
    // V1 prefix. Registrars accept V1 callers and inspect appended fields only
    // when version and size prove that they are present.
    uint32_t size;
    uint32_t version;
    void (__cdecl* tick)(uint8_t* room, uint32_t now);
    void (__cdecl* end)(uint8_t* room);
    uint32_t (__cdecl* accepted_damage)(
        uint8_t* room, uint32_t attacker_slot, uint8_t* attacker_pilot,
        uint32_t proposed_damage, uint32_t attack_type);
};
struct SdgoMissionClientLifecycleV1 {
    uint32_t size;
    uint32_t version;
    uint32_t (__cdecl* object_for_pilot)(void* um, uint32_t pilot);
    int (__cdecl* slot_for_object)(void* um, uint32_t object);
    int (__cdecl* keep_dead)(uint8_t* unit);
};
struct SdgoMissionClientLifecycleV2 {
    // V1 prefix. respawn_complete is dispatched only after native PlaybackUnit
    // has completed the exact mission object selected by SetSlotColour.
    uint32_t size;
    uint32_t version;
    uint32_t (__cdecl* object_for_pilot)(void* um, uint32_t pilot);
    int (__cdecl* slot_for_object)(void* um, uint32_t object);
    int (__cdecl* keep_dead)(uint8_t* unit);
    void (__cdecl* respawn_complete)(void* um, uint32_t object);
    int (__cdecl* ready)(uint8_t* mode);
};
using SdgoRegisterMissionServerV1 = int (__cdecl*)(const SdgoMissionServerLifecycleV1*);
using SdgoRegisterMissionClientV1 = int (__cdecl*)(const SdgoMissionClientLifecycleV1*);

#define SDGO_MATCH_AI_MISSION_TICK_EXPORT "SdgoMatchAiMissionTickV1"
using SdgoMatchAiMissionTickV1Fn = int (__cdecl*)(uint8_t* mode);

namespace sdgo {
inline HMODULE MissionSibling(const SdgoApi* api, SdgoMod* mod, const wchar_t* name) {
    if (!api || !api->ModDirectory) return nullptr;
    std::wstring path = api->ModDirectory(mod);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return nullptr;
    path.resize(slash + 1);
    path += name;
    return GetModuleHandleW(path.c_str());
}
}
