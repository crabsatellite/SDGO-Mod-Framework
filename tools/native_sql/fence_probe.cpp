#include <windows.h>
#include <string>
int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 2;
    HMODULE module=LoadLibraryW(argv[1]);if(!module)return 3;
    auto initialize=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(module,"InitializeSqlRegistry"));
    if(!initialize||initialize(nullptr)!=ERROR_SUCCESS)return 4;
    HANDLE file=CreateFileW(argv[2],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 5;
    DWORD count=0;const char value[]="owned test fence";const bool ok=WriteFile(file,value,sizeof(value),&count,nullptr)&&count==sizeof(value);CloseHandle(file);
    if(!ok||GetFileAttributesW(argv[3])==INVALID_FILE_ATTRIBUTES)return 6;
    return 0;
}
