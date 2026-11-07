// Controlled original-instruction replay, never an original server entry point
// or a gameplay actor. The test owns all record/actor storage and hook lifetime.
#include "sdgo/native_skill_identity.h"
#include "sdgo/script_registration.h"
#include <windows.h>
#include <algorithm>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <xmmintrin.h>

using namespace sdgo::skill_identity;
namespace {
constexpr std::size_t kImageSize=0x46c000;
constexpr char kCookie[]="SDGO_CONTROLLED_SERVER_SKILL_REPLAY";
std::uintptr_t requestedBase=0x400000;
std::uint8_t* image=nullptr;
int failures=0,hookCalls=0;
bool wrongBoundary=false;
std::uint32_t target=9800123,source=70207;
void Check(bool ok,const char* text){std::printf("%s %s\n",ok?"PASS":"FAIL",text);if(!ok)++failures;}
struct Allocation final:Authority{
    std::string revision(const std::string&)const override{return std::string(64,'a');}
    bool originalSkill(std::uint32_t id)const override{return id==source;}
    bool templateSkill(std::uint32_t id,const std::string& hash)const override{return id==source&&hash==std::string(64,'b');}
    bool allocatedSkill(const std::string& owner,std::uint32_t id,const std::string& client,const std::string& server)const override{
        return owner=="example"&&id==target&&client==std::string(64,'c')&&server==std::string(64,'d');
    }
};
struct Registry final:Lookup{
    bool throwOnLookup=false;
    const void* find(std::uint32_t id)const override{
        if(throwOnLookup)throw std::runtime_error("controlled failed definition lookup");
        SetLastError(0xdead);
        __asm{fninit}
        _mm_setcsr(0x5f80);
        return id==target?&target:id==source?&source:nullptr;
    }
};
void Log(SdgoMod*,int,const char*,...){}
void* Symbol(SdgoMod*,const char* name){
    const std::map<std::string,std::uint32_t> rvas={
        {"skill_identity.enable_selector",0x89449},{"skill_identity.disable_selector",0x89789},
        {"skill_identity.enable_continue",0x8944e},{"skill_identity.disable_continue",0x8978e}};
    const auto found=rvas.find(name);if(found==rvas.end())return nullptr;
    return image+found->second+(wrongBoundary&&found->first=="skill_identity.disable_continue"?1:0);
}
int Hook(SdgoMod*,const char* name,void* detour,void** original){
    ++hookCalls;void* site=Symbol(nullptr,name);
    return site&&MH_CreateHook(site,detour,original)==MH_OK&&MH_EnableHook(site)==MH_OK;
}
void Write(std::uint32_t rva,std::uint8_t value){
    DWORD before=0;if(!VirtualProtect(image+rva,1,PAGE_EXECUTE_READWRITE,&before))throw std::runtime_error("owned replay write protection");
    image[rva]=value;DWORD unused=0;VirtualProtect(image+rva,1,before,&unused);FlushInstructionCache(GetCurrentProcess(),image+rva,1);
}
void Load(const char* path){
    MEMORY_BASIC_INFORMATION memory={};
    if(VirtualQuery(reinterpret_cast<void*>(requestedBase),&memory,sizeof(memory))!=sizeof(memory)||
       memory.AllocationBase!=reinterpret_cast<void*>(requestedBase)||memory.Type!=MEM_PRIVATE||
       memory.State!=MEM_COMMIT||memory.RegionSize<kImageSize||memory.Protect!=PAGE_READWRITE||
       std::memcmp(reinterpret_cast<void*>(requestedBase),kCookie,sizeof(kCookie)))throw std::runtime_error("owned child source reservation differs");
    image=reinterpret_cast<std::uint8_t*>(requestedBase);
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("original server replay input unavailable");
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(stream),{});
    if(sdgo::scripts::Sha256(bytes)!="f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4")throw std::runtime_error("original server source SHA differs");
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if(bytes.size()<sizeof(*dos)||dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||
       static_cast<std::size_t>(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS32)>bytes.size())throw std::runtime_error("replay DOS/PE extent");
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data()+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||
       nt->OptionalHeader.ImageBase!=0x400000||nt->OptionalHeader.SizeOfImage!=kImageSize||
       nt->OptionalHeader.SizeOfHeaders>bytes.size())throw std::runtime_error("replay source image layout differs");
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    if(reinterpret_cast<const std::uint8_t*>(sections+nt->FileHeader.NumberOfSections)>bytes.data()+bytes.size())throw std::runtime_error("replay section table extent");
    std::memcpy(image,bytes.data(),nt->OptionalHeader.SizeOfHeaders);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& section=sections[i];
        if(section.VirtualAddress>kImageSize||section.SizeOfRawData>kImageSize-section.VirtualAddress||
           section.PointerToRawData>bytes.size()||section.SizeOfRawData>bytes.size()-section.PointerToRawData)throw std::runtime_error("replay section source/destination extent");
        std::memcpy(image+section.VirtualAddress,bytes.data()+section.PointerToRawData,section.SizeOfRawData);
    }
    if(requestedBase!=0x400000){
        const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if(!directory.Size||directory.VirtualAddress>kImageSize||directory.Size>kImageSize-directory.VirtualAddress)throw std::runtime_error("replay relocation directory extent");
        std::size_t cursor=directory.VirtualAddress,end=cursor+directory.Size;
        while(cursor<end){
            if(end-cursor<sizeof(IMAGE_BASE_RELOCATION))throw std::runtime_error("replay relocation block header");
            const auto* block=reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image+cursor);
            if(block->SizeOfBlock<sizeof(*block)||block->SizeOfBlock>end-cursor||block->SizeOfBlock%2)throw std::runtime_error("replay relocation block extent");
            const auto* entries=reinterpret_cast<const WORD*>(image+cursor+sizeof(*block));
            for(std::size_t i=0;i<(block->SizeOfBlock-sizeof(*block))/2;++i){
                const auto type=entries[i]>>12;if(!type)continue;
                const auto rva=block->VirtualAddress+(entries[i]&0xfff);
                if(type!=IMAGE_REL_BASED_HIGHLOW||rva>kImageSize-4)throw std::runtime_error("unsupported replay relocation entry");
                std::uint32_t value=0;std::memcpy(&value,image+rva,4);value+=static_cast<std::uint32_t>(requestedBase-0x400000);
                std::memcpy(image+rva,&value,4);
            }
            cursor+=block->SizeOfBlock;
        }
    }
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& section=sections[i];
        const auto extent=(std::max)(section.Misc.VirtualSize,section.SizeOfRawData);if(!extent)continue;
        if(extent>kImageSize-section.VirtualAddress)throw std::runtime_error("replay section virtual extent");
        const DWORD protection=section.Characteristics&IMAGE_SCN_MEM_EXECUTE?PAGE_EXECUTE_READ:section.Characteristics&IMAGE_SCN_MEM_WRITE?PAGE_READWRITE:PAGE_READONLY;
        DWORD unused=0;if(!VirtualProtect(image+section.VirtualAddress,extent,protection,&unused))throw std::runtime_error("replay final protection");
    }
    FlushInstructionCache(GetCurrentProcess(),image,kImageSize);
}
int Child(const char* path){
    Load(path);using Dispatch=int(__thiscall*)(void*,const void*,void*);
    auto enable=reinterpret_cast<Dispatch>(image+0x89400),disable=reinterpret_cast<Dispatch>(image+0x89740);
    std::vector<std::uint8_t> actor(0x1d00,0x5a);actor[0x1cde]=0;
    Check(enable(nullptr,&source,actor.data())==1&&actor[0x1cde]==1,"original source ID reaches the original flag-enable case");
    Check(disable(nullptr,&source,actor.data())==1&&actor[0x1cde]==0,"original source ID reaches the original flag-disable case");
    const auto unchanged=actor;
    Check(enable(nullptr,&target,actor.data())==1&&actor==unchanged,"unadapted allocated ID reproduces the original default-case omission");
    Allocation authority;Registry lookup;std::string error;
    auto bindings=Bindings::Build({{"example",std::string(64,'a'),{{target,source,std::string(64,'c'),std::string(64,'d'),std::string(64,'b')}}}},authority,error);
    if(!bindings)throw std::runtime_error(error);
    SdgoApi api={};api.size=sizeof(api);api.version=1;api.host_key="BattleServer-548A891D-0046C000";api.host_base=image;api.Log=Log;api.Symbol=Symbol;api.HookSite=Hook;
    if(MH_Initialize()!=MH_OK)throw std::runtime_error("owned replay hook initialization");
    const auto old=image[0x89998+0xc4];Write(0x89998+0xc4,old^1);
    Check(!InstallNative(&api,nullptr,bindings,&lookup,error)&&hookCalls==0,"changed selector table refuses before either hook");Write(0x89998+0xc4,old);
    wrongBoundary=true;Check(!InstallNative(&api,nullptr,bindings,&lookup,error)&&hookCalls==0,"wrong continuation refuses before either hook");wrongBoundary=false;
    Check(InstallNative(&api,nullptr,bindings,&lookup,error),"both exact original instruction sites install together");
    SetLastError(0x1234);const auto csr=_mm_getcsr();unsigned short controlBefore=0,controlAfter=0;
    __asm{fnstcw controlBefore}
    const auto on=enable(nullptr,&target,actor.data());
    __asm{fnstcw controlAfter}
    const auto lastError=GetLastError();const auto csrAfter=_mm_getcsr();
    Check(on==1&&actor[0x1cde]==1&&target==9800123,"allocated ID uses the original enable case without mutating the real record");
    Check(lastError==0x1234&&csrAfter==csr&&controlBefore==controlAfter,"selector callback preserves last error and incoming x87/SSE control state");
    auto expected=unchanged;expected[0x1cde]=1;Check(actor==expected,"no scratch actor byte outside the original case flag changed");
    Check(disable(nullptr,&target,actor.data())==1&&actor==unchanged&&target==9800123,"allocated ID uses the original disable case and preserves its real ID");
    std::uint32_t copy=target;
    Check(enable(nullptr,&copy,actor.data())==0&&actor==unchanged&&copy==target,"an unowned copy with the same ID is rejected through the native false return");
    lookup.throwOnLookup=true;
    Check(enable(nullptr,&target,actor.data())==0&&actor==unchanged&&target==9800123,"lookup exceptions refuse through the native false return without crossing the naked hook");
    lookup.throwOnLookup=false;
    Check(enable(nullptr,&source,actor.data())==1&&actor[0x1cde]==1,"stock source still runs its unmodified trampoline path");
    Check(!InstallNative(&api,nullptr,bindings,&lookup,error),"live replacement is refused");
    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
    std::printf("controlled original instruction replay at0x%08lX; no original process, room, actor or network packet was executed\n",static_cast<DWORD>(requestedBase));
    return failures?1:0;
}
int Parent(const char* path){
    wchar_t executable[32768]={};GetModuleFileNameW(nullptr,executable,32768);
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,nullptr,0);if(!length)throw std::runtime_error("source path encoding");
    std::vector<wchar_t> sourcePath(length);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,sourcePath.data(),length);
    std::wstring command=L"\""+std::wstring(executable)+(requestedBase==0x400000?L"\" --native-child \"":L"\" --native-child-rebased \"")+sourcePath.data()+L"\"";
    STARTUPINFOW startup={};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child={};
    if(!CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))throw std::runtime_error("owned replay child creation");
    int result=1;
    try{
        void* region=VirtualAllocEx(child.hProcess,reinterpret_cast<void*>(requestedBase),kImageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);SIZE_T copied=0;
        if(region!=reinterpret_cast<void*>(requestedBase)||!WriteProcessMemory(child.hProcess,region,kCookie,sizeof(kCookie),&copied)||copied!=sizeof(kCookie))throw std::runtime_error("owned child early source reservation");
        if(ResumeThread(child.hThread)==static_cast<DWORD>(-1)||WaitForSingleObject(child.hProcess,30000)!=WAIT_OBJECT_0)throw std::runtime_error("owned replay child completion");
        DWORD code=1;if(!GetExitCodeProcess(child.hProcess,&code))throw std::runtime_error("owned replay exit code");result=static_cast<int>(code);
    }catch(...){TerminateProcess(child.hProcess,124);WaitForSingleObject(child.hProcess,5000);CloseHandle(child.hThread);CloseHandle(child.hProcess);throw;}
    CloseHandle(child.hThread);CloseHandle(child.hProcess);return result;
}
}
int main(int argc,char** argv){try{
    if(argc!=3)return 2;const std::string mode=argv[1];
    if(mode=="--native-child-rebased"||mode=="--native-server-rebased")requestedBase=0x6b0000;
    return mode=="--native-child"||mode=="--native-child-rebased"?Child(argv[2]):Parent(argv[2]);
}catch(const std::exception& error){std::printf("FAIL %s\n",error.what());return 1;}}
