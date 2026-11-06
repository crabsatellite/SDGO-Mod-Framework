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
struct SdgoMissionClientLifecycleV1 {
    uint32_t size;
    uint32_t version;
    uint32_t (__cdecl* object_for_pilot)(void* um, uint32_t pilot);
    int (__cdecl* slot_for_object)(void* um, uint32_t object);
    int (__cdecl* keep_dead)(uint8_t* unit);
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
