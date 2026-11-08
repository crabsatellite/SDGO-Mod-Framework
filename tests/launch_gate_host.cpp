// A process boundary fixture. The shipped loader runs before this entry point.
#include <windows.h>
extern "C" __declspec(dllimport) DWORD WINAPI SdgoLaunchGateProtocol();
int wmain() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    if (SdgoLaunchGateProtocol() != 1) return 2;
    HANDLE file = CreateFileW(L"native-entry.txt", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    const wchar_t* command = GetCommandLineW(); DWORD n = 0;
    bool ok = WriteFile(file, command, DWORD(wcslen(command)*2), &n, nullptr) != FALSE;
    CloseHandle(file); return ok ? 0 : 4;
}
