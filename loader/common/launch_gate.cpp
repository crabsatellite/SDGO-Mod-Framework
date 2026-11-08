#include "launch_gate.h"
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <cstring>

namespace {
constexpr wchar_t kPermitEnv[] = L"SDGO_LAUNCH_PERMIT";
constexpr char kProtocol[] = "SDGO_LAUNCH_GATE_V1";
#pragma pack(push, 1)
struct Request { char magic[8]; DWORD pid, reserved; ULONGLONG created; DWORD rootChars, commandChars; };
struct Permit { char magic[8]; ULONGLONG expires; BYTE receipt[32], source[32]; DWORD rootChars; };
#pragma pack(pop)

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
ULONGLONG Now() { FILETIME f; GetSystemTimeAsFileTime(&f); return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
[[noreturn]] void Fail(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"SDGO Mod Manager", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    ExitProcess(ERROR_CANCELLED);
}
bool Read(HANDLE file, void* data, DWORD size) { DWORD n = 0; return ReadFile(file, data, size, &n, nullptr) && n == size; }
bool Write(HANDLE file, const void* data, DWORD size) { DWORD n = 0; return WriteFile(file, data, size, &n, nullptr) && n == size; }
bool Hash(const std::wstring& path, BYTE out[32]) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    BYTE bytes[65536]; DWORD n = 0;
    while (ok) {
        if (!ReadFile(file, bytes, sizeof(bytes), &n, nullptr)) { ok = false; break; }
        if (!n) break;
        ok = BCryptHashData(hash, bytes, n, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, out, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file); return ok;
}
bool Token(const std::wstring& token) {
    if (token.size() != 32) return false;
    for (wchar_t c : token) if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}
bool Owner(const std::wstring& root) {
    HANDLE file = CreateFileW((root + L"\\sdgo\\local-state\\owner-maintainer.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char bytes[256]{}; DWORD n = 0; LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < sizeof(bytes) && ReadFile(file, bytes, sizeof(bytes)-1, &n, nullptr);
    CloseHandle(file); std::string compact;
    if (ok) for (DWORD i = 0; i < n; ++i) if (bytes[i] != ' ' && bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\t') compact += bytes[i];
    return compact == "{\"schema_version\":1,\"mode\":\"owner\"}" || compact == "{\"mode\":\"owner\",\"schema_version\":1}";
}
bool Consume(const std::wstring& root, const std::wstring& folder, const std::wstring& token) {
    if (!Token(token)) return false;
    auto path = folder + L"\\" + token + L".permit";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Permit p{}; LARGE_INTEGER size{}; bool ok = GetFileSizeEx(file, &size) && Read(file, &p, sizeof(p));
    std::wstring expected;
    if (ok && memcmp(p.magic, "SDGOLP01", 8) == 0 && p.rootChars > 0 && p.rootChars < 32768 &&
        size.QuadPart == sizeof(p) + p.rootChars * sizeof(wchar_t) && p.expires >= Now() && p.expires - Now() <= 120ULL * 10000000) {
        expected.resize(p.rootChars);
        ok = Read(file, expected.data(), p.rootChars * sizeof(wchar_t)) && _wcsicmp(root.c_str(), expected.c_str()) == 0;
        BYTE receipt[32]{}, source[32]{};
        ok = ok && Hash(root + L"\\sdgo\\local-state\\client-runtime.json", receipt) &&
            Hash(root + L"\\sdgo\\client-source.json", source) && memcmp(p.receipt, receipt, 32) == 0 && memcmp(p.source, source, 32) == 0;
    } else ok = false;
    CloseHandle(file);
    if (ok) {
        HANDLE ack = CreateFileW((folder + L"\\" + token + L".ack").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok = ack != INVALID_HANDLE_VALUE;
        if (ok) { ok = Write(ack, kProtocol, sizeof(kProtocol)-1); CloseHandle(ack); }
    }
    return ok;
}
}

void ClientLaunchGate(const char* loaderName) {
    if (strcmp(loaderName, "d3d9") != 0) return;
    std::vector<wchar_t> path(32768); DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!n || n >= path.size()) Fail(L"无法核对客户端路径，启动已停止。");
    std::wstring exe(path.data(), n); auto slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos || _wcsicmp(exe.c_str() + slash + 1, L"GOnline.exe") != 0) return;
    std::wstring root = exe.substr(0, slash), folder = root + L"\\sdgo\\local-state\\launch";
    wchar_t tokenEnv[128]{};
    DWORD supplied = GetEnvironmentVariableW(kPermitEnv, tokenEnv, 128);
    SetEnvironmentVariableW(kPermitEnv, nullptr);
    if (supplied) {
        if (supplied >= 128 || !Consume(root, folder, tokenEnv)) Fail(L"启动许可无效或已使用。请重新打开 GOnline.exe，让 Manager 完成校验。");
        return;
    }
    if (!Exists(root + L"\\sdgo\\client-source.json") || Owner(root)) return;
    // Sealed native tests retain their candidate inputs instead of installing
    // the owner's published runtime over the test. Startup tests use a permit.
    wchar_t run[256]{}, expected[256]{}, throughManager[256]{};
    if (GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN", run, 256) > 0) {
        GetPrivateProfileStringW(L"run", L"run_id", L"", expected, 256, (root + L"\\sdgo\\gametest\\run.ini").c_str());
        GetEnvironmentVariableW(L"SDGO_GAME_TEST_LAUNCH_GATE", throughManager, 256);
        if (run[0] && wcscmp(run, expected) == 0 && wcscmp(run, throughManager) != 0) return;
    }
    CreateDirectoryW((root + L"\\sdgo\\local-state").c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    BYTE random[16]{};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) Fail(L"无法生成启动请求。");
    std::wstring token;
    for (BYTE b : random) { token += L"0123456789abcdef"[b >> 4]; token += L"0123456789abcdef"[b & 15]; }
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user)) Fail(L"无法核对启动进程。");
    std::wstring command = GetCommandLineW();
    Request r{}; memcpy(r.magic, "SDGOLQ01", 8); r.pid = GetCurrentProcessId();
    r.created = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    r.rootChars = DWORD(root.size()); r.commandChars = DWORD(command.size());
    auto requestPath = folder + L"\\" + token + L".request";
    HANDLE file = CreateFileW(requestPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) { ok = Write(file, &r, sizeof(r)) && Write(file, root.data(), r.rootChars*2) && Write(file, command.data(), r.commandChars*2) && FlushFileBuffers(file); CloseHandle(file); }
    if (!ok) Fail(L"无法保存启动请求，请检查客户端目录的写入权限。");
    auto manager = root + L"\\SDGO Mod Manager.exe";
    std::wstring args = L"\"" + manager + L"\" --launch-game \"" + root + L"\" --launch-request " + token;
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(manager.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &si, &pi)) {
        DeleteFileW(requestPath.c_str());
        Fail(L"无法启动 SDGO Mod Manager。请安装配套 Manager 后重新打开游戏。");
    }
    CloseHandle(pi.hThread);
    // Only the local handoff has a deadline. Downloads have no total-time or
    // speed deadline and can continue for as long as the player needs.
    auto accepted = folder + L"\\" + token + L".accepted";
    DWORD start = GetTickCount();
    while (!Exists(accepted) && GetTickCount() - start < 90000) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
    }
    CloseHandle(pi.hProcess);
    if (!Exists(accepted)) { DeleteFileW(requestPath.c_str()); Fail(L"Manager 未接受启动请求。请更新配套 Manager，并查看其提示后重试。"); }
    ExitProcess(0);
}
