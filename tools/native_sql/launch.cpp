// Launch one copied native SQL engine with private instance configuration.
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>

std::wstring quote(const wchar_t* value) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (; *value; ++value) {
        if (*value == L'\\') { ++slashes; continue; }
        if (*value == L'\"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        out += *value; slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2; // registry DLL, copied sqlservr.exe, native arguments
    wchar_t dll[MAX_PATH];
    if (!GetFullPathNameW(argv[1], MAX_PATH, dll, nullptr)) return 3;
    std::wstring command;
    for (int i = 2; i < argc; ++i) command += (i == 2 ? L"" : L" ") + quote(argv[i]);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[2], command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 4;
    auto fail = [&](int code) {
        TerminateProcess(process.hProcess, code);
        WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        std::cerr << "Private native SQL launch failed at step " << code << "\n";
        return code;
    };
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* memory = VirtualAllocEx(process.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory || !WriteProcessMemory(process.hProcess, memory, dll, bytes, nullptr)) return fail(5);
    HANDLE thread = CreateRemoteThread(process.hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")), memory, 0, nullptr);
    if (!thread || WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) return fail(6);
    CloseHandle(thread);
    VirtualFreeEx(process.hProcess, memory, 0, MEM_RELEASE);
    uintptr_t remote = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.dwProcessId);
    if (snapshot == INVALID_HANDLE_VALUE) return fail(7);
    MODULEENTRY32W entry{sizeof(entry)};
    if (Module32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExePath, dll) == 0) remote = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    HMODULE local = LoadLibraryExW(dll, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    auto init = local ? GetProcAddress(local, "InitializeSqlRegistry") : nullptr;
    if (!remote || !init) return fail(8);
    auto address = remote + (reinterpret_cast<uintptr_t>(init) - reinterpret_cast<uintptr_t>(local));
    FreeLibrary(local);
    thread = CreateRemoteThread(process.hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), nullptr, 0, nullptr);
    if (!thread || WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) return fail(9);
    DWORD status = ERROR_GEN_FAILURE;
    GetExitCodeThread(thread, &status); CloseHandle(thread);
    if (status != ERROR_SUCCESS) return fail(10);
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) return fail(11);
    std::cout << process.dwProcessId << std::endl;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return 0;
}
