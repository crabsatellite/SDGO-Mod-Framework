#include "native_performance_seed.h"
#include <bcrypt.h>
#include <cstring>
static NTSTATUS WINAPI PerformanceGenRandom(BCRYPT_ALG_HANDLE,PUCHAR bytes,ULONG length,ULONG) {
    uint64_t seed=0;
    if(!bytes||length!=sizeof(seed)||!NativePerformanceSeed(seed))return static_cast<NTSTATUS>(0xC000000Du);
    memcpy(bytes,&seed,sizeof(seed));return 0;
}
#define BCryptGenRandom PerformanceGenRandom
#include "../mods/room_random/room_random.cpp"
#undef BCryptGenRandom
