// Exercise the actual production DLLs against owned native-ABI fixtures.
// No game, input method, window, server, account or player file is opened.
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "sdgo/sdgo_api.h"

namespace {
unsigned checks=0;
void Check(bool ok,const char* text) {
    ++checks;
    if(!ok)throw std::runtime_error(text);
}
void Log(SdgoMod*,int,const char*,...) {}
int Setting(SdgoMod*,const char*,int value) {return value;}
template<class T> void Put(void* object,size_t offset,T value) {
    std::memcpy(static_cast<uint8_t*>(object)+offset,&value,sizeof(value));
}
template<class T> T Read(const void* object,size_t offset=0) {
    T value;std::memcpy(&value,static_cast<const uint8_t*>(object)+offset,sizeof(value));return value;
}

namespace gate {
using HookFn=void(__fastcall*)(void*,void*,int,const uint8_t*);
HookFn hook=nullptr;
std::map<uint32_t,std::array<uint8_t,9>> sets;
std::set<uint32_t> units;
std::array<uint8_t,8> unitRecord{};
std::array<uint8_t,0x20E4> screen{};
unsigned originalCalls=0;
const uint8_t* shown=nullptr;
void* shownScreen=nullptr;
int shownArg=0;
bool missingUnitPointer=false;
void* Grid() {return &sets;}
int __fastcall Find(void* grid,void*,const uint8_t** out,uint32_t id,int type,char logging) {
    Check(grid==&sets && logging==0,"production loading lookup uses its bound native grid");
    *out=nullptr;
    if(type==2 && units.count(id)) {*out=missingUnitPointer ? nullptr : unitRecord.data();return 1;}
    if(type==5) {auto it=sets.find(id);if(it!=sets.end()){*out=it->second.data();return 1;}}
    return 0;
}
void __fastcall Original(void* self,void*,int arg,const uint8_t* set) {
    ++originalCalls;shown=set;shownScreen=self;shownArg=arg;
}
void* Symbol(SdgoMod*,const char* name) {
    if(!std::strcmp(name,"grid.data"))return reinterpret_cast<void*>(&Grid);
    if(!std::strcmp(name,"grid.find"))return reinterpret_cast<void*>(&Find);
    return nullptr;
}
int Hook(SdgoMod*,const char* name,void* detour,void** original) {
    Check(!std::strcmp(name,"gate.show_set_unit"),"startup fix owns only the verified loading-display function");
    hook=reinterpret_cast<HookFn>(detour);*original=reinterpret_cast<void*>(&Original);return 1;
}
const uint8_t* Set(uint32_t id,uint32_t unit) {
    auto& set=sets[id];set={};Put(set.data(),0,id);Put(set.data(),5,unit);return set.data();
}
void Reset() {sets.clear();units.clear();screen.fill(0x5A);originalCalls=0;shown=nullptr;missingUnitPointer=false;}
void Show(const uint8_t* set) {hook(screen.data(),nullptr,73,set);}
void Run(SdgoModLoadFn entry) {
    SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;
    api.Symbol=Symbol;api.HookCode=Hook;api.Log=Log;
    Check(entry(&api,nullptr)!=0 && hook,"actual startup-fix DLL registers native display detour");
    Reset();const auto* valid=Set(27101,11);units.insert(11);Put(screen.data(),0x20E0,uint32_t{27101});
    const auto originalScreen=screen;Show(valid);
    Check(originalCalls==1 && shown==valid && screen==originalScreen && shownScreen==screen.data() && shownArg==73,
          "known-unit loading set forwards once with its original screen, argument and selection");
    Reset();const auto* empty=Set(27101,0);Show(empty);
    Check(originalCalls==1 && shown==empty,"unit-free loading set retains original native behavior");
    Reset();valid=Set(27101,11);Set(27102,0);const auto* broken=Set(27103,99);Set(27104,12);
    units={11,12};Put(screen.data(),0x20E0,uint32_t{27103});Show(broken);
    Check(originalCalls==1 && shown==valid && Read<uint32_t>(screen.data(),0x20E0)==27101,
          "broken selection uses nearest preceding known unit and updates the future animation selection");
    Reset();broken=Set(27101,99);Set(27102,0);valid=Set(27103,11);units.insert(11);
    Put(screen.data(),0x20E0,uint32_t{27101});Show(broken);
    Check(originalCalls==0 && Read<uint32_t>(screen.data(),0x20E0)==27103,
          "later replacement is selected without creating its model before the native walk reaches it");
    Show(valid);
    Check(originalCalls==1 && shown==valid,"native walk shows the deferred valid replacement exactly once");
    Reset();valid=Set(27101,11);broken=Set(27102,99);units.insert(11);missingUnitPointer=true;
    Put(screen.data(),0x20E0,uint32_t{27102});Show(broken);
    Check(originalCalls==1 && shown==broken && Read<uint32_t>(screen.data(),0x20E0)==27102,
          "lookup success with null unit record is not accepted as a valid fallback; no-candidate branch remains native");
    Reset();Show(nullptr);
    Check(originalCalls==0,"unreadable null set is contained without forwarding a null native display call");
}
}

namespace relay {
char* format=nullptr;
void* Symbol(SdgoMod*,const char* name) {return !std::strcmp(name,"agent.relay_log_format") ? format : nullptr;}
void Run(SdgoModLoadFn entry) {
    constexpr char unsafe[]="UDP_RELAY_DATA : SEND TO DATA.. to p:%d %s:%d\n";
    constexpr char safe[]="UDP_RELAY_DATA : SEND TO DATA.. to p:%d %u:%d\n";
    auto* memory=static_cast<char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Check(memory!=nullptr,"owned relay format allocation");
    std::memset(memory,0x5A,4096);format=memory+16;std::memcpy(format,unsafe,sizeof(unsafe));
    const std::array<char,sizeof(unsafe)> before=[&]{std::array<char,sizeof(unsafe)> value{};std::memcpy(value.data(),format,value.size());return value;}();
    DWORD old=0;Check(VirtualProtect(memory,4096,PAGE_READONLY,&old)!=0,"fixture native format starts read-only");
    SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Symbol=Symbol;api.Log=Log;
    Check(entry(&api,nullptr)!=0 && !std::strcmp(format,safe),"actual relay-fix DLL replaces pointer conversion with an unsigned integer conversion");
    unsigned changed=0;for(size_t n=0;n<before.size();++n)changed+=before[n]!=format[n] ? 1u : 0u;
    MEMORY_BASIC_INFORMATION protection{};VirtualQuery(memory,&protection,sizeof(protection));
    Check(changed==1 && memory[15]==0x5A && memory[16+sizeof(unsafe)]==0x5A && protection.Protect==PAGE_READONLY,
          "only the verified conversion character changes and original page protection is restored");
    Check(entry(&api,nullptr)!=0,"already-safe native format is idempotent");
    char rendered[160]{};std::snprintf(rendered,sizeof(rendered),format,7,2130706433u,19001);
    Check(!std::strcmp(rendered,"UDP_RELAY_DATA : SEND TO DATA.. to p:7 2130706433:19001\n"),
          "corrected actual format consumes the native integer IP without dereferencing it");
    Check(VirtualProtect(memory,4096,PAGE_READWRITE,&old)!=0,"fixture can prepare an unrelated native string");
    std::strcpy(format,"unrelated %s");VirtualProtect(memory,4096,PAGE_READONLY,&old);
    Check(entry(&api,nullptr)==0 && !std::strcmp(format,"unrelated %s"),"unknown native format is rejected without modification");
    format=nullptr;Check(entry(&api,nullptr)==0,"missing verified native format is rejected");
    VirtualFree(memory,0,MEM_RELEASE);
}
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    try {
        HMODULE dll=LoadLibraryW(argv[2]);
        const auto entry=dll ? reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll,"SdgoModLoad")) : nullptr;
        Check(entry!=nullptr,"actual production DLL exports SdgoModLoad");
        if(!wcscmp(argv[1],L"gate"))gate::Run(entry);
        else if(!wcscmp(argv[1],L"relay"))relay::Run(entry);
        else return 2;
        FreeLibrary(dll);std::printf("%u production DLL ABI checks passed; no game process or UI\n",checks);return 0;
    } catch(const std::exception& e) {std::printf("FAIL: %s\n",e.what());return 1;}
}
