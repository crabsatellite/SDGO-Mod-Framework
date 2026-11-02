// Redirect only this SQL process's instance registry into its private HKCU tree.
// All other processes, including production SQL, retain the real HKLM view.
#include <windows.h>
#include <string>
#include <cwchar>
#include "MinHook.h"

namespace {
using Open = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using Create = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                              const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
Open original_open;
Create original_create;
using OpenA = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using CreateA = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
                               const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
OpenA original_open_a;
CreateA original_create_a;
std::wstring private_root;

bool sql_path(HKEY key, LPCWSTR path, std::wstring& redirected) {
    if (key != HKEY_LOCAL_MACHINE || !path) return false;
    const wchar_t* roots[] = {L"SOFTWARE\\Microsoft\\Microsoft SQL Server",
                             L"SOFTWARE\\Microsoft\\MSSQLServer",
                             L"SOFTWARE\\WOW6432Node\\Microsoft\\Microsoft SQL Server",
                             L"SOFTWARE\\WOW6432Node\\Microsoft\\MSSQLServer"};
    for (unsigned i = 0; i < 4; ++i) {
        size_t n = wcslen(roots[i]);
        if (_wcsnicmp(path, roots[i], n) == 0 && (path[n] == 0 || path[n] == L'\\')) {
            redirected = private_root + (i % 2 == 0 ? L"\\Microsoft SQL Server" : L"\\MSSQLServer") + (path + n);
            return true;
        }
    }
    return false;
}
LSTATUS WINAPI open_key(HKEY key, LPCWSTR path, DWORD options, REGSAM access, PHKEY result) {
    std::wstring redirected;
    if (sql_path(key, path, redirected))
        return original_open(HKEY_CURRENT_USER, redirected.c_str(), options, access, result);
    return original_open(key, path, options, access, result);
}
LSTATUS WINAPI create_key(HKEY key, LPCWSTR path, DWORD reserved, LPWSTR cls, DWORD options,
                         REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    std::wstring redirected;
    if (sql_path(key, path, redirected))
        return original_create(HKEY_CURRENT_USER, redirected.c_str(), reserved, cls, options,
                               access, security, result, disposition);
    return original_create(key, path, reserved, cls, options, access, security, result, disposition);
}
std::wstring wide(LPCSTR text) {
    if (!text) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), n);
    return result;
}
LSTATUS WINAPI open_key_a(HKEY key, LPCSTR path, DWORD options, REGSAM access, PHKEY result) {
    std::wstring converted = wide(path), redirected;
    if (path && sql_path(key, converted.c_str(), redirected))
        return original_open(HKEY_CURRENT_USER, redirected.c_str(), options, access, result);
    return original_open_a(key, path, options, access, result);
}
LSTATUS WINAPI create_key_a(HKEY key, LPCSTR path, DWORD reserved, LPSTR cls, DWORD options,
                           REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    std::wstring converted = wide(path), redirected, cls_w = wide(cls);
    if (path && sql_path(key, converted.c_str(), redirected))
        return original_create(HKEY_CURRENT_USER, redirected.c_str(), reserved,
            cls ? cls_w.data() : nullptr, options, access, security, result, disposition);
    return original_create_a(key, path, reserved, cls, options, access, security, result, disposition);
}
}
extern "C" __declspec(dllexport) DWORD WINAPI InitializeSqlRegistry(void*) {
    wchar_t root[512];
    DWORD length = GetEnvironmentVariableW(L"SDGO_TEST_SQL_REGISTRY", root, 512);
    if (!length || length >= 512 || wcsncmp(root, L"Software\\SDGO\\NativeTest\\", 25) != 0)
        return ERROR_BAD_ENVIRONMENT;
    private_root = root;
    if (MH_Initialize() != MH_OK) return ERROR_INVALID_FUNCTION;
    if (MH_CreateHookApi(L"advapi32.dll", "RegOpenKeyExW", reinterpret_cast<void*>(open_key),
                         reinterpret_cast<void**>(&original_open)) != MH_OK ||
        MH_CreateHookApi(L"advapi32.dll", "RegCreateKeyExW", reinterpret_cast<void*>(create_key),
                         reinterpret_cast<void**>(&original_create)) != MH_OK ||
        MH_CreateHookApi(L"advapi32.dll", "RegOpenKeyExA", reinterpret_cast<void*>(open_key_a),
                         reinterpret_cast<void**>(&original_open_a)) != MH_OK ||
        MH_CreateHookApi(L"advapi32.dll", "RegCreateKeyExA", reinterpret_cast<void*>(create_key_a),
                         reinterpret_cast<void**>(&original_create_a)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return ERROR_INVALID_FUNCTION;
    return ERROR_SUCCESS;
}
