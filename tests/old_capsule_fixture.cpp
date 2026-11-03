#include <windows.h>

extern "C" __declspec(dllexport) int __cdecl OldCapsuleFixtureMarker()
{
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
