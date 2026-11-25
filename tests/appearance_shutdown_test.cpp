#include <windows.h>
#include <iostream>
#include <string>
struct Counts { unsigned created=0,released=0; };
int wmain(int argc,wchar_t** argv){
    wchar_t executable[32768]{};if(!GetModuleFileNameW(nullptr,executable,32768))return 1;
    const std::wstring self=executable;
    if(argc==3){
        Counts counts;
        const auto path=self.substr(0,self.find_last_of(L"\\/"))+L"\\appearance_shutdown_fixture.dll";
        const auto library=LoadLibraryW(path.c_str());if(!library)return 2;
        const auto exercise=reinterpret_cast<bool(__cdecl*)(Counts*,bool)>(GetProcAddress(library,"Exercise"));
        if(!exercise||!exercise(&counts,std::wstring(argv[2])==L"mask"))return 3;
        if(std::wstring(argv[1])==L"exit")ExitProcess(0);
        if(!FreeLibrary(library)||counts.created!=counts.released)return 4;
        return 0;
    }
    for(const auto mode:{L"unload",L"exit"})for(const auto resource:{L"texture",L"mask"}){
        std::wstring command=L"\""+self+L"\" "+mode+L" "+resource;
        STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 5;
        CloseHandle(process.hThread);
        const auto wait=WaitForSingleObject(process.hProcess,10000);
        if(wait!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,92);WaitForSingleObject(process.hProcess,5000);}
        DWORD code=93;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
        std::wcout<<mode<<L" "<<resource<<L": "<<code<<L"\n";
        if(wait!=WAIT_OBJECT_0||code)return 6;
    }
    return 0;
}
