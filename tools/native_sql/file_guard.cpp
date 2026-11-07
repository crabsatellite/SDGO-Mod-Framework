// Confine copied SQL database files to its marked test root.
// Registry redirection alone does not relocate paths stored in a copied master.
#include "file_guard.h"
#include <winternl.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "MinHook.h"

namespace {
using NtCreate = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtOpen = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK, ULONG, ULONG);
using NtQuery = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
using NtDelete = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
using NtSet = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
NtCreate original_create;
NtOpen original_open;
NtQuery original_query, original_query_full;
NtSet original_set;
NtDelete original_delete;
std::wstring task_root, resource_root, volume_device, volume_drive;
thread_local bool resolving = false;
volatile LONG denied_count = 0, redirected_count = 0;
HANDLE audit_file = INVALID_HANDLE_VALUE;
constexpr NTSTATUS denied = static_cast<NTSTATUS>(0xC0000022L);

struct Resolution {
    bool previous = resolving;
    Resolution() { resolving = true; }
    ~Resolution() { resolving = previous; }
};

bool starts(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() &&
        _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
}
bool inside(const std::wstring& value, const std::wstring& directory) {
    return starts(value, directory) && (value.size() == directory.size() ||
        value[directory.size()] == L'\\');
}
std::wstring leaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    auto name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    const auto stream = name.find(L':');
    if (stream != std::wstring::npos) name.resize(stream);
    return name;
}
bool database_file(const std::wstring& path) {
    const auto name = leaf(path);
    return name.size() >= 4 && (_wcsicmp(name.c_str() + name.size() - 4, L".mdf") == 0 ||
        _wcsicmp(name.c_str() + name.size() - 4, L".ndf") == 0 ||
        _wcsicmp(name.c_str() + name.size() - 4, L".ldf") == 0);
}
bool resource_file(const std::wstring& path) {
    const auto name = leaf(path);
    return _wcsicmp(name.c_str(), L"mssqlsystemresource.mdf") == 0 ||
        _wcsicmp(name.c_str(), L"mssqlsystemresource.ldf") == 0;
}

std::wstring dos(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (starts(path, L"\\??\\")) path.erase(0, 4);
    else if (starts(path, L"\\\\?\\")) path.erase(0, 4);
    else if (starts(path, L"\\DosDevices\\")) path.erase(0, 12);
    else if (!volume_device.empty() && inside(path, volume_device))
        path = volume_drive + path.substr(volume_device.size());
    return path;
}
bool full_path(const std::wstring& input, std::wstring& output) {
    const auto path = dos(input);
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return false;
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size || size > 32767) return false;
    std::vector<wchar_t> buffer(size);
    if (!GetFullPathNameW(path.c_str(), size, buffer.data(), nullptr)) return false;
    output.assign(buffer.data());
    while (output.size() > 3 && output.back() == L'\\') output.pop_back();
    return true;
}
bool final_path(HANDLE handle, std::wstring& output) {
    DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size > 32767) return false;
    std::vector<wchar_t> buffer(size + 1);
    if (!GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) return false;
    return full_path(buffer.data(), output);
}
bool directory_path(const std::wstring& input, std::wstring& output) {
    Resolution scope;
    HANDLE handle = CreateFileW(input.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = final_path(handle, output);
    CloseHandle(handle);
    return ok;
}
bool confined(const std::wstring& input) {
    std::wstring path;
    if (!full_path(input, path) || !inside(path, task_root)) return false;
    const auto slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    std::wstring parent;
    if (!directory_path(path.substr(0, slash), parent) || !inside(parent, task_root)) return false;
    Resolution scope;
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    BY_HANDLE_FILE_INFORMATION information{};
    std::wstring actual;
    const bool ok = GetFileInformationByHandle(handle, &information) &&
        information.nNumberOfLinks == 1 && final_path(handle, actual) && inside(actual, task_root);
    CloseHandle(handle);
    return ok;
}
bool writes(ACCESS_MASK access) {
    constexpr ACCESS_MASK mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
        GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | ACCESS_SYSTEM_SECURITY;
    return (access & mask) != 0;
}
bool ipc(const std::wstring& input) {
    return inside(input, L"\\Device\\Afd") || inside(input, L"\\Device\\NamedPipe") ||
        inside(input, L"\\??\\pipe") || inside(input, L"\\DosDevices\\pipe") ||
        inside(input, L"\\Device\\ConDrv") || inside(input, L"\\Device\\Tcp") ||
        inside(input, L"\\Device\\Udp") || _wcsicmp(input.c_str(), L"\\??\\NUL") == 0 ||
        // BCrypt uses this kernel device; it is not a filesystem location.
        _wcsicmp(input.c_str(), L"\\Device\\KsecDD") == 0;
}
bool object_path(POBJECT_ATTRIBUTES attributes, std::wstring& path) {
    if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer ||
        (attributes->ObjectName->Length % sizeof(wchar_t)) != 0) return false;
    path.assign(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
    if (attributes->RootDirectory) {
        std::wstring parent;
        if (!final_path(attributes->RootDirectory, parent) || path.empty() || path.front() == L'\\') return false;
        path = parent + L"\\" + path;
    }
    return !path.empty();
}
struct Object {
    OBJECT_ATTRIBUTES value{};
    UNICODE_STRING name{};
    std::wstring storage;
};
bool check(POBJECT_ATTRIBUTES attributes, bool write, Object& target, bool map_resource = true,
    bool database_source = false) {
    std::wstring input;
    if (!object_path(attributes, input)) return !write && !database_file(input);
    if (ipc(input)) return !database_file(input);
    if (map_resource && resource_file(input)) {
        input = resource_root + L"\\" + leaf(input);
        InterlockedIncrement(&redirected_count);
    } else if (!database_file(input) && !database_source) return true;
    std::wstring path;
    if (!full_path(input, path) || !confined(path)) return false;
    target.storage = L"\\??\\" + path;
    if (target.storage.size() >= 32767) return false;
    target.name.Buffer = target.storage.data();
    target.name.Length = static_cast<USHORT>(target.storage.size() * sizeof(wchar_t));
    target.name.MaximumLength = target.name.Length;
    target.value = *attributes;
    target.value.RootDirectory = nullptr;
    target.value.ObjectName = &target.name;
    return true;
}
POBJECT_ATTRIBUTES result_object(POBJECT_ATTRIBUTES original, Object& target) {
    return target.storage.empty() ? original : &target.value;
}
void audit(const char* operation, POBJECT_ATTRIBUTES attributes, ACCESS_MASK access) {
    if (audit_file == INVALID_HANDLE_VALUE) return;
    Resolution scope;
    std::wstring path;
    if (!object_path(attributes, path)) path = L"<unresolved> " + path;
    if (path.size() > 4096) path.resize(4096);
    const int count = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    std::string encoded(count > 0 ? count : 0, '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        encoded.data(), count, nullptr, nullptr);
    for (char& value : encoded) if (value == '\r' || value == '\n') value = ' ';
    char header[128];
    std::snprintf(header, sizeof(header), "pid=%lu op=%s access=0x%08lX path=",
        GetCurrentProcessId(), operation, static_cast<unsigned long>(access));
    const std::string line = std::string(header) + encoded + "\r\n";
    DWORD written = 0;
    WriteFile(audit_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}
NTSTATUS reject(PHANDLE handle, PIO_STATUS_BLOCK status,
    const char* operation = "SetInformationFile", POBJECT_ATTRIBUTES attributes = nullptr,
    ACCESS_MASK access = 0) {
    InterlockedIncrement(&denied_count);
    audit(operation, attributes, access);
    if (handle) *handle = nullptr;
    if (status) { status->Status = denied; status->Information = 0; }
    return denied;
}
NTSTATUS observe_database(NTSTATUS result, PHANDLE handle, PIO_STATUS_BLOCK status,
    POBJECT_ATTRIBUTES attributes, ACCESS_MASK access) {
    if (resolving || result != 0 || !handle || !*handle) return result;
    std::wstring requested;
    if (!object_path(attributes, requested) || !database_file(requested)) return result;
    std::wstring actual;
    BY_HANDLE_FILE_INFORMATION information{};
    bool owned = false;
    {
        Resolution scope;
        const auto inspect = [&](HANDLE value) {
            return final_path(value, actual) && inside(actual, task_root) &&
                GetFileInformationByHandle(value, &information) && information.nNumberOfLinks == 1;
        };
        owned = inspect(*handle);
        if (!owned) {
            // DELETE-only opens need not grant name/attribute queries. Reopen
            // the same file object for metadata, not by a second pathname.
            HANDLE metadata = ReOpenFile(*handle, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
            if (metadata != INVALID_HANDLE_VALUE) {
                owned = inspect(metadata);
                CloseHandle(metadata);
            }
        }
    }
    if (!owned) {
        CloseHandle(*handle);
        return reject(handle, status, "DatabaseHandleDenied", attributes, access);
    }
    UNICODE_STRING name{static_cast<USHORT>(actual.size() * 2), static_cast<USHORT>(actual.size() * 2), actual.data()};
    OBJECT_ATTRIBUTES physical{sizeof(physical), nullptr, &name};
    audit("DatabaseOpened", &physical, access);
    return result;
}
NTSTATUS NTAPI create_file(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
    PIO_STATUS_BLOCK status, PLARGE_INTEGER allocation, ULONG flags, ULONG share,
    ULONG disposition, ULONG options, PVOID ea, ULONG ea_length) {
    Object target;
    if (!resolving && !check(attributes, writes(access) || disposition != 1, target)) {
        // CAPI uses OPEN_IF even for read-only directory discovery. Remove
        // creation semantics rather than grant writes to an outside directory.
        if (!writes(access) && disposition == 3 && check(attributes, false, target))
            disposition = 1; // FILE_OPEN: a missing external object stays missing
        else if (!writes(access) && disposition == 2 && (options & 1) &&
            check(attributes, false, target)) {
            std::wstring input, existing;
            if (object_path(attributes, input) && directory_path(dos(input), existing)) {
                // CreateDirectory on an existing read-only directory normally
                // reports ALREADY_EXISTS. Preserve that result without creating
                // or changing anything outside the task copy.
                constexpr auto collision = static_cast<NTSTATUS>(0xC0000035L);
                if (handle) *handle = nullptr;
                if (status) { status->Status = collision; status->Information = 0; }
                return collision;
            }
            return reject(handle, status, "CreateDirectory", attributes, access);
        }
        else return reject(handle, status, "CreateFile", attributes, access);
    }
    auto* original = result_object(attributes, target);
    const NTSTATUS result = original_create(handle, access, original, status,
        allocation, flags, share, disposition, options, ea, ea_length);
    return observe_database(result, handle, status, original, access);
}
NTSTATUS NTAPI open_file(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
    PIO_STATUS_BLOCK status, ULONG share, ULONG options) {
    Object target;
    if (!resolving && !check(attributes, writes(access), target))
        return reject(handle, status, "OpenFile", attributes, access);
    auto* original = result_object(attributes, target);
    const NTSTATUS result = original_open(handle, access, original, status, share, options);
    return observe_database(result, handle, status, original, access);
}
NTSTATUS NTAPI query_file(POBJECT_ATTRIBUTES attributes, PVOID information) {
    Object target;
    if (!resolving && !check(attributes, false, target))
        return reject(nullptr, nullptr, "QueryAttributesFile", attributes);
    return original_query(result_object(attributes, target), information);
}
NTSTATUS NTAPI query_full(POBJECT_ATTRIBUTES attributes, PVOID information) {
    Object target;
    if (!resolving && !check(attributes, false, target))
        return reject(nullptr, nullptr, "QueryFullAttributesFile", attributes);
    return original_query_full(result_object(attributes, target), information);
}
NTSTATUS NTAPI delete_file(POBJECT_ATTRIBUTES attributes) {
    Object target;
    if (!resolving && !check(attributes, true, target, false))
        return reject(nullptr, nullptr, "DeleteFile", attributes);
    return original_delete(result_object(attributes, target));
}
NTSTATUS NTAPI set_file(HANDLE handle, PIO_STATUS_BLOCK status, PVOID information,
    ULONG length, FILE_INFORMATION_CLASS kind) {
    const int type = static_cast<int>(kind);
    if (!resolving && (type == 11 || type == 57 || type == 72 || type == 73))
        return reject(nullptr, status); // SQL never needs to create database hard links
    if (!resolving && (type == 10 || type == 56 || type == 65 || type == 66)) {
        // Both native rename layouts use an aligned HANDLE, ULONG byte count,
        // then UTF-16 bytes. Validate the destination before any rename occurs.
        struct Rename { ULONG flags; HANDLE directory; ULONG bytes; WCHAR name[1]; };
        constexpr size_t offset = offsetof(Rename, name);
        if (!information || length < offset) return reject(nullptr, status);
        auto* rename = static_cast<Rename*>(information);
        if (rename->bytes > length - offset || rename->bytes > 65532 || rename->bytes % 2)
            return reject(nullptr, status);
        UNICODE_STRING name{static_cast<USHORT>(rename->bytes), static_cast<USHORT>(rename->bytes), rename->name};
        OBJECT_ATTRIBUTES attributes{sizeof(attributes), rename->directory, &name};
        Object target;
        std::wstring source;
        {
            Resolution scope;
            if (!final_path(handle, source)) return reject(nullptr, status, "RenameSourceUnknown");
        }
        if (!check(&attributes, true, target, false, database_file(source)))
            return reject(nullptr, status, "RenameFile", &attributes, DELETE);
        // Destination validation is enough; leave the caller's native layout intact.
    }
    return original_set(handle, status, information, length, kind);
}
bool environment(const wchar_t* key, std::wstring& value) {
    const DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
    if (!count || count > 32767) return false;
    std::vector<wchar_t> buffer(count);
    if (!GetEnvironmentVariableW(key, buffer.data(), count)) return false;
    value.assign(buffer.data());
    return true;
}
}

DWORD PrepareSqlFileGuard() {
    std::wstring root, resources;
    if (!environment(L"SDGO_TEST_SQL_ROOT", root) || !directory_path(root, task_root))
        return ERROR_BAD_ENVIRONMENT;
    const DWORD marker = GetFileAttributesW((task_root + L"\\TEST-ONLY.txt").c_str());
    if (marker == INVALID_FILE_ATTRIBUTES || (marker & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_BAD_ENVIRONMENT;
    if (!environment(L"SDGO_TEST_SQL_RESOURCE_DIR", resources) ||
        !directory_path(resources, resource_root) || !inside(resource_root, task_root))
        return ERROR_BAD_ENVIRONMENT;
    volume_drive = task_root.substr(0, 2);
    wchar_t device[1024]{};
    if (!QueryDosDeviceW(volume_drive.c_str(), device, 1024)) return ERROR_BAD_ENVIRONMENT;
    volume_device = device;
    for (const auto* name : {L"mssqlsystemresource.mdf", L"mssqlsystemresource.ldf"})
        if (GetFileAttributesW((resource_root + L"\\" + name).c_str()) == INVALID_FILE_ATTRIBUTES ||
            !confined(resource_root + L"\\" + name)) return ERROR_BAD_ENVIRONMENT;
    const auto log = task_root + L"\\sql-file-guard-denied.log";
    if (!confined(log)) return ERROR_BAD_ENVIRONMENT;
    audit_file = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (audit_file == INVALID_HANDLE_VALUE) return GetLastError();
    if (MH_CreateHookApi(L"ntdll.dll", "NtCreateFile", reinterpret_cast<void*>(create_file),
        reinterpret_cast<void**>(&original_create)) != MH_OK ||
        MH_CreateHookApi(L"ntdll.dll", "NtOpenFile", reinterpret_cast<void*>(open_file),
        reinterpret_cast<void**>(&original_open)) != MH_OK ||
        MH_CreateHookApi(L"ntdll.dll", "NtQueryAttributesFile", reinterpret_cast<void*>(query_file),
        reinterpret_cast<void**>(&original_query)) != MH_OK ||
        MH_CreateHookApi(L"ntdll.dll", "NtQueryFullAttributesFile", reinterpret_cast<void*>(query_full),
        reinterpret_cast<void**>(&original_query_full)) != MH_OK ||
        MH_CreateHookApi(L"ntdll.dll", "NtSetInformationFile", reinterpret_cast<void*>(set_file),
        reinterpret_cast<void**>(&original_set)) != MH_OK ||
        MH_CreateHookApi(L"ntdll.dll", "NtDeleteFile", reinterpret_cast<void*>(delete_file),
        reinterpret_cast<void**>(&original_delete)) != MH_OK) return ERROR_INVALID_FUNCTION;
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD WINAPI SqlFileGuardDeniedCount(void*) {
    return static_cast<DWORD>(InterlockedCompareExchange(&denied_count, 0, 0));
}
extern "C" __declspec(dllexport) DWORD WINAPI SqlFileGuardRedirectCount(void*) {
    return static_cast<DWORD>(InterlockedCompareExchange(&redirected_count, 0, 0));
}
