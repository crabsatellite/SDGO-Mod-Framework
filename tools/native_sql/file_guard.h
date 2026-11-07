#pragma once
#include <windows.h>

// Register the native file hooks before resuming the copied SQL process.
// Missing or invalid task roots fail initialization, not just later I/O.
DWORD PrepareSqlFileGuard();
