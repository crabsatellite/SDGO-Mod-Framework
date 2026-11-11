#include "sdgo/sdgo_api.h"
#include "../battle_ai/ace_policy.h"
#include "../ace_ai/pilot_catalog.h"

namespace {
bool g_active = false;
battle_ai::ace::PilotCatalog g_catalog;
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiActiveV1()
{
    return g_active ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiUnitEligibleV1(uint32_t unitId)
{
    return g_active && g_catalog.Find(unitId) ? 1 : 0;
}

SDGO_MOD_ENTRY
{
    g_active = false;
    std::string error;
    if (!g_catalog.Load(api, mod, error, L"ace_ai_server")) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "Ace pilot catalog is unavailable: %s; Ace selection disabled",
                 error.c_str());
        return 0;
    }
    g_active = true;
    const battle_ai::ace::Settings settings = battle_ai::ace::LoadSettings(api, mod);
    api->Log(mod, SDGO_LOG_INFO,
             "ready: %u pilot identities; Ace policy chances Easy/Medium/Hard %d/%d/%d%%; Hard HP %d%%",
             static_cast<unsigned>(g_catalog.Size()),
             settings.byDifficulty[0].chancePercent,
             settings.byDifficulty[1].chancePercent,
             settings.byDifficulty[2].chancePercent,
             settings.byDifficulty[2].hpPercent);
    return 1;
}
