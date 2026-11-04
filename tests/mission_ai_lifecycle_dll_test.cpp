#include <windows.h>
#include <cstdio>
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
    if (!share) {
        std::fprintf(stderr, "match_ai is missing the mission lifecycle export\n");
        return 3;
    }
    SdgoMissionClientLifecycleV1 hooks = {sizeof(hooks), 1, Object, Slot, Keep};
    auto invalid = hooks;
    invalid.version = 2;
    if (share(nullptr) || share(&invalid)) return 4;
    invalid = hooks;
    invalid.size = 0;
    if (share(&invalid)) return 5;
    invalid = hooks;
    invalid.keep_dead = nullptr;
    if (share(&invalid)) return 6;
    if (!share(&hooks) || !share(&hooks)) return 7;
    auto duplicate = hooks;
    if (share(&duplicate)) return 8;
    std::puts("Production match_ai DLL accepts the mission lifecycle ABI and protects registration");
    FreeLibrary(module);
    return 0;
}
