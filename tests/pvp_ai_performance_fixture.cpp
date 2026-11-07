#include "native_performance_seed.h"
#define SdgoModLoad SdgoPerformanceOriginalLoad
#include "../mods/pvp_ai/pvp_ai.cpp"
#undef SdgoModLoad

extern "C" __declspec(dllexport) int __cdecl SdgoModLoad(const SdgoApi* api,SdgoMod* mod) {
    uint64_t seed=0;if(!NativePerformanceSeed(seed))return 0;
    const int loaded=SdgoPerformanceOriginalLoad(api,mod);
    if(loaded) {
        g_random=static_cast<unsigned>(seed)|1u;
        api->Log(mod,SDGO_LOG_INFO,"native performance fixture: fixed initial roster seed=%llu",
                 static_cast<unsigned long long>(seed));
    }
    return loaded;
}
