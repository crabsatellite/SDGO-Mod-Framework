// Redirect only this SQL process's instance registry into its private HKCU tree.
// All other processes, including production SQL, retain the real HKLM view.
#include <windows.h>
#include <string>
#include <cwchar>
#include <winternl.h>
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
std::wstring data_root;
using NtCreate=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
using NtOpen=NTSTATUS(NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
using NtDelete=NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES);
using NtQuery=NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES,PVOID);
using VolumePath=BOOL(WINAPI*)(LPCWSTR,LPWSTR,DWORD);
NtCreate original_file_create=nullptr;
NtOpen original_file_open=nullptr;
NtDelete original_file_delete=nullptr;
NtQuery original_attributes=nullptr,original_full_attributes=nullptr;
VolumePath original_volume_path=nullptr;

bool data_attributes(POBJECT_ATTRIBUTES source,OBJECT_ATTRIBUTES& target,UNICODE_STRING& name,std::wstring& path) {
    if(data_root.empty()||!source||!source->ObjectName||!source->ObjectName->Buffer)return false;
    const auto* text=source->ObjectName;
    if(text->Length%sizeof(wchar_t)||text->Length>60000)return false;
    std::wstring original(text->Buffer,text->Length/sizeof(wchar_t));
    const auto at=original.find_last_of(L"\\/");const auto file=original.substr(at==std::wstring::npos?0:at+1);
    const auto dot=file.find_last_of(L'.');if(dot==std::wstring::npos)return false;
    const auto extension=file.substr(dot);
    if(_wcsicmp(extension.c_str(),L".mdf")&&_wcsicmp(extension.c_str(),L".ndf")&&_wcsicmp(extension.c_str(),L".ldf"))return false;
    if(file.find_first_of(L":\\/")!=std::wstring::npos)return false;
    // Cold system catalogs may retain original physical paths. Every database
    // file open, including a relative NT open, is confined to this test copy.
    path=L"\\??\\"+data_root+L"\\"+file;
    name.Length=static_cast<USHORT>(path.size()*sizeof(wchar_t));name.MaximumLength=name.Length+sizeof(wchar_t);name.Buffer=path.data();
    target=*source;target.RootDirectory=nullptr;target.ObjectName=&name;return true;
}
NTSTATUS NTAPI create_file(PHANDLE file,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes,PIO_STATUS_BLOCK io,
                          PLARGE_INTEGER allocation,ULONG flags,ULONG sharing,ULONG disposition,ULONG options,PVOID ea,ULONG ea_size) {
    try {
        OBJECT_ATTRIBUTES scoped{};UNICODE_STRING name{};std::wstring path;
        return original_file_create(file,access,data_attributes(attributes,scoped,name,path)?&scoped:attributes,io,allocation,flags,sharing,disposition,options,ea,ea_size);
    }catch(...){return static_cast<NTSTATUS>(0xC0000022L);}
}
NTSTATUS NTAPI open_file(PHANDLE file,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes,PIO_STATUS_BLOCK io,ULONG sharing,ULONG options) {
    try {
        OBJECT_ATTRIBUTES scoped{};UNICODE_STRING name{};std::wstring path;
        return original_file_open(file,access,data_attributes(attributes,scoped,name,path)?&scoped:attributes,io,sharing,options);
    }catch(...){return static_cast<NTSTATUS>(0xC0000022L);}
}
NTSTATUS NTAPI delete_file(POBJECT_ATTRIBUTES attributes) {
    try {
        OBJECT_ATTRIBUTES scoped{};UNICODE_STRING name{};std::wstring path;
        return original_file_delete(data_attributes(attributes,scoped,name,path)?&scoped:attributes);
    }catch(...){return static_cast<NTSTATUS>(0xC0000022L);}
}
NTSTATUS NTAPI query_attributes(POBJECT_ATTRIBUTES attributes,PVOID info) {
    try{OBJECT_ATTRIBUTES scoped{};UNICODE_STRING name{};std::wstring path;return original_attributes(data_attributes(attributes,scoped,name,path)?&scoped:attributes,info);}catch(...){return static_cast<NTSTATUS>(0xC0000022L);}
}
NTSTATUS NTAPI query_full_attributes(POBJECT_ATTRIBUTES attributes,PVOID info) {
    try{OBJECT_ATTRIBUTES scoped{};UNICODE_STRING name{};std::wstring path;return original_full_attributes(data_attributes(attributes,scoped,name,path)?&scoped:attributes,info);}catch(...){return static_cast<NTSTATUS>(0xC0000022L);}
}
BOOL WINAPI volume_path(LPCWSTR file,LPWSTR volume,DWORD count) {
    if(file&&!data_root.empty()) {
        const std::wstring original=file;const auto at=original.find_last_of(L"\\/");const auto name=original.substr(at==std::wstring::npos?0:at+1);const auto dot=name.find_last_of(L'.');
        if(dot!=std::wstring::npos){const auto extension=name.substr(dot);if(!_wcsicmp(extension.c_str(),L".mdf")||!_wcsicmp(extension.c_str(),L".ndf")||!_wcsicmp(extension.c_str(),L".ldf"))return original_volume_path((data_root+L"\\"+name).c_str(),volume,count);}
    }
    return original_volume_path(file,volume,count);
}

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
    wchar_t data[32768]{};
    const DWORD data_length=GetEnvironmentVariableW(L"SDGO_TEST_SQL_DATA_ROOT",data,32768);
    if(data_length) {
        if(data_length>=32768)return ERROR_BAD_ENVIRONMENT;
        HKEY key=nullptr;wchar_t test[32768]{};DWORD bytes=sizeof(test),kind=0;
        if(RegOpenKeyExW(HKEY_CURRENT_USER,root,0,KEY_READ,&key)!=ERROR_SUCCESS)return ERROR_BAD_ENVIRONMENT;
        const auto status=RegQueryValueExW(key,L"TestRoot",nullptr,&kind,reinterpret_cast<LPBYTE>(test),&bytes);RegCloseKey(key);
        const std::wstring expected=std::wstring(test)+L"\\server\\LiteSQL\\MSSQL\\DATA";
        if(status!=ERROR_SUCCESS||kind!=REG_SZ||_wcsicmp(expected.c_str(),data)||GetFileAttributesW((std::wstring(test)+L"\\TEST-ONLY.txt").c_str())==INVALID_FILE_ATTRIBUTES)return ERROR_BAD_ENVIRONMENT;
        data_root=data;
    }
    if (MH_Initialize() != MH_OK) return ERROR_INVALID_FUNCTION;
    if(!data_root.empty()&&(MH_CreateHookApi(L"ntdll.dll","NtCreateFile",reinterpret_cast<void*>(create_file),reinterpret_cast<void**>(&original_file_create))!=MH_OK||
       MH_CreateHookApi(L"ntdll.dll","NtOpenFile",reinterpret_cast<void*>(open_file),reinterpret_cast<void**>(&original_file_open))!=MH_OK||
       MH_CreateHookApi(L"ntdll.dll","NtDeleteFile",reinterpret_cast<void*>(delete_file),reinterpret_cast<void**>(&original_file_delete))!=MH_OK||
       MH_CreateHookApi(L"ntdll.dll","NtQueryAttributesFile",reinterpret_cast<void*>(query_attributes),reinterpret_cast<void**>(&original_attributes))!=MH_OK||
       MH_CreateHookApi(L"ntdll.dll","NtQueryFullAttributesFile",reinterpret_cast<void*>(query_full_attributes),reinterpret_cast<void**>(&original_full_attributes))!=MH_OK||
       MH_CreateHookApi(L"kernel32.dll","GetVolumePathNameW",reinterpret_cast<void*>(volume_path),reinterpret_cast<void**>(&original_volume_path))!=MH_OK))return ERROR_INVALID_FUNCTION;
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
