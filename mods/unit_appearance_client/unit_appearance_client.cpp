#include "sdgo/sdgo_api.h"
int LoadRgb(const SdgoApi*, SdgoMod*);
int LoadDecals(const SdgoApi*, SdgoMod*);
void AbortRgbLoad();
void AbortDecalsLoad();
SDGO_MOD_ENTRY {
    if (!api || !mod || !api->SettingInt || !api->SettingInt(mod, "Enabled", 0)) return 0;
    // Disabled components neither hook native pages nor register storage handlers.
    const bool loaded=(!api->SettingInt(mod,"DecalsEnabled",1)||LoadDecals(api,mod))&&
                      (!api->SettingInt(mod,"RgbEnabled",1)||LoadRgb(api,mod));
    if(!loaded){AbortRgbLoad();AbortDecalsLoad();return 0;}
    api->Log(mod, SDGO_LOG_INFO, "Unit appearance ready: RGB=%d decals=%d", api->SettingInt(mod,"RgbEnabled",1), api->SettingInt(mod,"DecalsEnabled",1));
    return 1;
}
