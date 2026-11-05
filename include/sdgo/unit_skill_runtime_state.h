#pragma once
#include <cstdint>

struct SdgoUnitSkillRuntimeStateV1 {
    std::uint32_t size,version,installed,bindings,enable_aliases,disable_aliases,rejected;
    char candidate_sha256[65],authority_sha256[65],server_fingerprint[65];
};
using SdgoReadUnitSkillRuntimeStateV1Fn=int(__cdecl*)(SdgoUnitSkillRuntimeStateV1*);
