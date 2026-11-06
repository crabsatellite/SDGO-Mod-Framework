// Exercise real Win32 and NT file I/O in an owned fixture, without running SQL.
#include <windows.h>
#include <winternl.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
int failures = 0;
void expect(bool condition, const char* label) {
    if (!condition) { ++failures; std::cerr << "FAIL " << label << " error=" << GetLastError() << "\n"; }
}
HANDLE open(const fs::path& path, DWORD access, DWORD disposition = OPEN_EXISTING) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void blocked(const fs::path& path, DWORD access, const char* label, DWORD disposition = OPEN_EXISTING) {
    HANDLE file = open(path, access, disposition);
    expect(file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, label);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    const fs::path base = fs::absolute(argv[2]), allowed = base / L"allowed", outside = base / L"outside";
    SetEnvironmentVariableW(L"SDGO_TEST_SQL_REGISTRY", L"Software\\SDGO\\NativeTest\\SDGOGUARDFIXTURE");
    SetEnvironmentVariableW(L"SDGO_TEST_SQL_ROOT", allowed.c_str());
    SetEnvironmentVariableW(L"SDGO_TEST_SQL_RESOURCE_DIR", (allowed / L"resources").c_str());
    const bool invalid = argc == 4;
    if (invalid && _wcsicmp(argv[3], L"missing-marker") == 0)
        SetEnvironmentVariableW(L"SDGO_TEST_SQL_ROOT", (allowed / L"unmarked").c_str());
    else if (invalid)
        SetEnvironmentVariableW(L"SDGO_TEST_SQL_RESOURCE_DIR", outside.c_str());
    // Preserve an outside directory handle to test RootDirectory-relative NT opens.
    HANDLE directory = CreateFileW(outside.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return 3;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return 4;
    using Initialize = DWORD(WINAPI*)(void*);
    const auto initialize = reinterpret_cast<Initialize>(GetProcAddress(dll, "InitializeSqlRegistry"));
    if (!initialize) return 5;
    // SQL imports Advapi32; this small file-I/O host does not otherwise use it.
    if (!LoadLibraryW(L"advapi32.dll")) return 5;
    const DWORD initialized = initialize(nullptr);
    if (invalid) {
        CloseHandle(directory);
        expect(initialized == ERROR_BAD_ENVIRONMENT, "invalid file boundary fails before hooks activate");
        return failures ? 1 : 0;
    }
    if (initialized != ERROR_SUCCESS) { std::cerr << "Initialize failed " << initialized << "\n"; return 6; }

    HANDLE local = open(allowed / L"local.mdf", GENERIC_READ | GENERIC_WRITE);
    expect(local != INVALID_HANDLE_VALUE, "local database read/write");
    if (local != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        expect(WriteFile(local, "clone", 5, &written, nullptr) && written == 5, "local database write completes");
        CloseHandle(local);
    }
    HANDLE created = open(allowed / L"new.ndf", GENERIC_WRITE, CREATE_NEW);
    expect(created != INVALID_HANDLE_VALUE, "new local database file");
    if (created != INVALID_HANDLE_VALUE) CloseHandle(created);
    blocked(outside / L"model.mdf", GENERIC_WRITE, "outside write denied");
    blocked(outside / L"model.mdf", GENERIC_READ, "outside database read denied");
    blocked(outside / L"absent.ldf", GENERIC_WRITE, "outside creation denied", CREATE_ALWAYS);
    blocked(base / L"allowed-other" / L"model.mdf", GENERIC_WRITE, "sibling prefix denied");
    blocked(allowed / L"junction" / L"model.mdf", GENERIC_WRITE, "junction escape denied");
    blocked(allowed / L"shared.mdf", GENERIC_WRITE, "shared hardlink denied");
    blocked(outside / L"note.txt", GENERIC_WRITE, "outside ordinary write denied");
    const auto denial_counter = reinterpret_cast<Initialize>(GetProcAddress(dll, "SqlFileGuardDeniedCount"));
    const DWORD before_security = denial_counter ? denial_counter(nullptr) : 0;
    blocked(outside / L"note.txt", GENERIC_READ | ACCESS_SYSTEM_SECURITY, "outside SACL access denied");
    expect(denial_counter && denial_counter(nullptr) == before_security + 1, "SACL request rejected by guard");
    HANDLE text = open(outside / L"note.txt", GENERIC_READ);
    expect(text != INVALID_HANDLE_VALUE, "ordinary outside read remains usable");
    if (text != INVALID_HANDLE_VALUE) CloseHandle(text);
    blocked(fs::path(L"\\\\?\\" + (outside / L"model.mdf").wstring()), GENERIC_WRITE, "extended DOS path denied");

    using NtOpen = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
    const auto nt_open = reinterpret_cast<NtOpen>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenFile"));
    std::wstring relative = L"model.mdf";
    UNICODE_STRING name{static_cast<USHORT>(relative.size() * 2), static_cast<USHORT>(relative.size() * 2), relative.data()};
    OBJECT_ATTRIBUTES attributes{sizeof(attributes), directory, &name};
    IO_STATUS_BLOCK status{};
    HANDLE native = nullptr;
    const NTSTATUS code = nt_open(&native, FILE_READ_DATA, &attributes, &status, FILE_SHARE_READ, 0);
    expect(code == static_cast<NTSTATUS>(0xC0000022L) && native == nullptr, "relative direct NT open denied");
    if (native) CloseHandle(native);
    using NtCreate = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    const auto nt_create = reinterpret_cast<NtCreate>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    relative = L"new-relative.ndf";
    name = {static_cast<USHORT>(relative.size() * 2), static_cast<USHORT>(relative.size() * 2), relative.data()};
    native = nullptr;
    expect(nt_create(&native, FILE_WRITE_DATA, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, 2, 0, nullptr, 0) == static_cast<NTSTATUS>(0xC0000022L) && !native,
        "relative direct NT creation denied");
    using NtDelete = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
    const auto nt_delete = reinterpret_cast<NtDelete>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeleteFile"));
    relative = L"note.txt";
    name = {static_cast<USHORT>(relative.size() * 2), static_cast<USHORT>(relative.size() * 2), relative.data()};
    expect(nt_delete(&attributes) == static_cast<NTSTATUS>(0xC0000022L), "relative direct NT deletion denied");
    std::wstring absolute = L"\\??\\" + (outside / L"note.txt").wstring();
    name = {static_cast<USHORT>(absolute.size() * 2), static_cast<USHORT>(absolute.size() * 2), absolute.data()};
    attributes.RootDirectory = nullptr;
    expect(nt_delete(&attributes) == static_cast<NTSTATUS>(0xC0000022L), "absolute direct NT deletion denied");
    CloseHandle(directory);

    // Native resource metadata probes must resolve to the task copy too.
    expect(GetFileAttributesW(L"Z:\\missing\\mssqlsystemresource.mdf") != INVALID_FILE_ATTRIBUTES,
        "stale resource metadata path redirects locally");
    HANDLE resource = open(L"Z:\\missing\\mssqlsystemresource.mdf", GENERIC_READ);
    expect(resource != INVALID_HANDLE_VALUE, "stale resource open redirects locally");
    if (resource != INVALID_HANDLE_VALUE) CloseHandle(resource);
    std::wstring device = L"\\Device\\KsecDD";
    UNICODE_STRING device_name{static_cast<USHORT>(device.size() * 2), static_cast<USHORT>(device.size() * 2), device.data()};
    OBJECT_ATTRIBUTES device_attributes{sizeof(device_attributes), nullptr, &device_name};
    HANDLE crypto_device = nullptr;
    expect(nt_open(&crypto_device, 0x00100003, &device_attributes, &status,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0) == 0 && crypto_device,
        "observed CNG kernel device access remains usable");
    if (crypto_device) CloseHandle(crypto_device);
    HMODULE crypto = LoadLibraryW(L"bcrypt.dll");
    using Random = LONG(WINAPI*)(PVOID, PUCHAR, ULONG, ULONG);
    auto random = crypto ? reinterpret_cast<Random>(GetProcAddress(crypto, "BCryptGenRandom")) : nullptr;
    unsigned char entropy[16]{};
    expect(random && random(nullptr, entropy, sizeof(entropy), 2) == 0, "CNG initialization and RNG");
    if (crypto) FreeLibrary(crypto);
    const auto pipe_name = L"\\\\.\\pipe\\SDGOFileGuard-" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_NOWAIT, 1, 1024, 1024, 0, nullptr);
    expect(pipe != INVALID_HANDLE_VALUE, "local IPC server");
    HANDLE client = open(pipe_name, GENERIC_READ | GENERIC_WRITE);
    expect(client != INVALID_HANDLE_VALUE, "local IPC read/write remains usable");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    expect(!MoveFileExW((allowed / L"local.mdf").c_str(), (outside / L"renamed.tmp").c_str(), 0) &&
        GetLastError() == ERROR_ACCESS_DENIED, "rename outside root denied");
    using NtSet = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    const auto nt_set = reinterpret_cast<NtSet>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    struct Rename { ULONG flags; HANDLE directory; ULONG bytes; WCHAR name[1024]; };
    Rename rename{};
    const auto destination = L"\\??\\" + (outside / L"bypass.tmp").wstring();
    rename.bytes = static_cast<ULONG>(destination.size() * sizeof(wchar_t));
    std::copy(destination.begin(), destination.end(), rename.name);
    HANDLE moving = open(allowed / L"local.mdf", GENERIC_READ | DELETE);
    expect(moving != INVALID_HANDLE_VALUE, "rename fixture source");
    for (const int kind : {56, 57, 66, 73})
        expect(nt_set(moving, &status, &rename, static_cast<ULONG>(offsetof(Rename, name)) + rename.bytes,
            static_cast<FILE_INFORMATION_CLASS>(kind)) == static_cast<NTSTATUS>(0xC0000022L),
            "native rename/link bypass denied");
    if (moving != INVALID_HANDLE_VALUE) CloseHandle(moving);
    expect(MoveFileExW((allowed / L"rename-inside.mdf").c_str(),
        (allowed / L"renamed-inside.mdf").c_str(), 0) != 0, "rename inside root remains usable");
    expect(DeleteFileW((allowed / L"delete-inside.mdf").c_str()) != 0, "delete inside root remains usable");
    expect(!CreateHardLinkW((allowed / L"new-link.mdf").c_str(), (allowed / L"local.mdf").c_str(), nullptr) &&
        GetLastError() == ERROR_ACCESS_DENIED, "database hardlink creation denied");
    const auto count = reinterpret_cast<Initialize>(GetProcAddress(dll, "SqlFileGuardDeniedCount"));
    const auto redirects = reinterpret_cast<Initialize>(GetProcAddress(dll, "SqlFileGuardRedirectCount"));
    expect(count && count(nullptr) >= 11, "denial receipts present");
    expect(redirects && redirects(nullptr) >= 2, "resource redirection receipts present");
    std::cout << "Native file guard failures=" << failures << "\n";
    return failures ? 1 : 0;
}
