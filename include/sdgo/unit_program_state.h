#pragma once
#include <cstdint>

// Read-only process-lifetime observation. A registered program is not actor or
// gameplay acceptance. Callers initialize size/version before requesting a copy.
struct SdgoUnitProgramStateV1 {
    std::uint32_t size, version;
    std::uint32_t records_ready, root_calls, active, fatal;
    std::uint32_t owner_count, program_count, export_count;
    std::uint32_t initial_thread_id, root_thread_id, vm;
    std::uint32_t first_export_function;
    char first_export_name[256];
    char bundle_sha256[65];
};
using SdgoGetUnitProgramStateV1 = int(__cdecl*)(SdgoUnitProgramStateV1*);
