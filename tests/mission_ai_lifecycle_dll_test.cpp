#include <windows.h>
#include <cstdio>
#include "sdgo/mission_ai_forced_pad_observer.h"
#include "sdgo/mission_ai_lifecycle.h"

namespace {
uint32_t __cdecl Object(void*, uint32_t) { return 0; }
int __cdecl Slot(void*, uint32_t) { return -1; }
int __cdecl Keep(uint8_t*) { return 0; }
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) return 2;
    auto share = reinterpret_cast<SdgoRegisterMissionClientV1>(
        GetProcAddress(module, "SdgoMatchAiRegisterMissionV1"));
    auto tick = reinterpret_cast<SdgoMatchAiMissionTickV1Fn>(
        GetProcAddress(
            module,
            SDGO_MATCH_AI_MISSION_TICK_EXPORT));
    auto forcedPadStatus =
        reinterpret_cast<
            SdgoMissionAiClientGetForcedPadStatusV1Fn>(
            GetProcAddress(
                module,
                SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_EXPORT));
    if (!share || !tick || !forcedPadStatus) {
        std::fprintf(
            stderr,
            "match_ai is missing a mission lifecycle/device export\n");
        return 3;
    }
    if (tick(nullptr)) return 4;
    SdgoMissionAiClientForcedPadStatusV1 status{};
    status.size = sizeof(status);
    status.version =
        SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_VERSION;
    if (!forcedPadStatus(&status) ||
        status.size != sizeof(status) ||
        status.version !=
            SDGO_MISSION_AI_CLIENT_FORCED_PAD_STATUS_VERSION ||
        status.bot_count != 0 ||
        status.bridge_available != 0)
        return 5;
    SdgoMissionClientLifecycleV1 hooks = {sizeof(hooks), 1, Object, Slot, Keep};
    auto invalid = hooks;
    invalid.version = 2;
    if (share(nullptr) || share(&invalid)) return 6;
    invalid = hooks;
    invalid.size = 0;
    if (share(&invalid)) return 7;
    invalid = hooks;
    invalid.keep_dead = nullptr;
    if (share(&invalid)) return 8;
    if (!share(&hooks) || !share(&hooks)) return 9;
    auto duplicate = hooks;
    if (share(&duplicate)) return 10;
    std::puts(
        "Production match_ai DLL accepts the mission lifecycle ABI, "
        "exports the mission forced-pad bridge, and protects registration");
    FreeLibrary(module);
    return 0;
}
