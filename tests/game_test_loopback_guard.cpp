// Test-fixture-only import guard for owned native monitor processes. It reuses
// the same socket policy as the background GameTest client and is never added
// to the normal client/server distribution catalog.
#include "../mods/game_test_client/quiet_sockets.h"

namespace { QuietSockets sockets; }

SDGO_MOD_ENTRY
{
    if (api->SettingInt(mod, "OwnedGameTest", 0) != 1)
        return 0;
    if (!sockets.Install(api, mod))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "owned native fixture: all imported binds remain on loopback");
    return 1;
}
