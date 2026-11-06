#include <windows.h>
#include <string>
#include "sdgo/sdgo_api.h"
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    const std::wstring root=argv[1];
    HMODULE core=LoadLibraryW((root+L"\\sdgo\\core.dll").c_str());if(!core)return 3;
    auto start=reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit"));if(!start)return 4;
    SdgoBoot boot{sizeof(boot),"owned-safety-fixture",GetModuleHandleW(nullptr)};
    if(!start(&boot))return 5;
    HANDLE ready=CreateFileW((root+L"\\ready").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(ready==INVALID_HANDLE_VALUE)return 6;CloseHandle(ready);
    for(unsigned tick=0;tick<1200;++tick) {
        if(GetFileAttributesW((root+L"\\stop").c_str())!=INVALID_FILE_ATTRIBUTES)return 0;
        if(GetFileAttributesW((root+L"\\reload").c_str())!=INVALID_FILE_ATTRIBUTES) {
            DeleteFileW((root+L"\\reload").c_str());
            HMODULE module=GetModuleHandleW((root+L"\\sdgo\\mods\\consumer\\consumer.dll").c_str());
            auto reload=module?reinterpret_cast<int(__cdecl*)()>(GetProcAddress(module,"SdgoSafetyReload")):nullptr;
            const int value=reload?reload():0;
            HANDLE result=CreateFileW((root+L"\\reload-result").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(result!=INVALID_HANDLE_VALUE){DWORD n=0;const char c=value?'1':'0';WriteFile(result,&c,1,&n,nullptr);CloseHandle(result);}
        }
        Sleep(50);
    }
    return 7;
}
