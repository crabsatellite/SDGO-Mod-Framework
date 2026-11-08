#pragma once
#include <windows.h>
#include <charconv>
#include <cstdint>
#include <cstring>

// Test DLLs only: controlled roster sampling without changing the production
// random source, candidate inventory, NPC records, weapons or native stats.
inline bool NativePerformanceSeed(uint64_t& seed) {
    char role[16]{},run[40]{},value[32]{};
    if(GetEnvironmentVariableA("SDGO_GAME_TEST_ROLE",role,sizeof(role))!=6||strcmp(role,"server")||
       GetEnvironmentVariableA("SDGO_GAME_TEST_RUN",run,sizeof(run))!=36)return false;
    const DWORD length=GetEnvironmentVariableA("SDGO_GAME_TEST_PERF_SEED",value,sizeof(value));
    if(!length||length>16)return false;
    const auto parsed=std::from_chars(value,value+length,seed,16);
    return parsed.ec==std::errc{}&&parsed.ptr==value+length&&seed!=0;
}
