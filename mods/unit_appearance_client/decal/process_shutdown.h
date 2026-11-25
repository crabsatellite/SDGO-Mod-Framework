#pragma once
#include <windows.h>

namespace sdgo::surface_decal {
// Resolve while the DLL is initialized, never by loading a module from a
// destructor under the loader lock. Windows has already stopped other threads
// when this becomes true; D3D/NVIDIA cleanup may then wait on a dead worker.
using ShutdownQuery = BOOLEAN (NTAPI*)();
inline const auto shutdownQuery = reinterpret_cast<ShutdownQuery>(
    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));
inline bool ProcessShutdown() noexcept {
    // If the query is unavailable, leave final resource reclamation to the OS.
    // Explicit cache eviction/clear during normal operation still releases it.
    return !shutdownQuery || shutdownQuery() != FALSE;
}
}
