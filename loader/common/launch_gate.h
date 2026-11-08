#pragma once

// Called only from early boot, outside DllMain and before loading the core.
// A bound player client hands off to Manager or consumes its one-use permit.
void ClientLaunchGate(const char* loaderName);
