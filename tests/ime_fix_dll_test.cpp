#include <windows.h>
#include <imm.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"

namespace {
using ReadFn = LONG(WINAPI*)(HIMC,DWORD,void*,DWORD);
using Caller = LONG(__cdecl*)(HIMC,DWORD,void*,DWORD);
using Filter = bool(__stdcall*)(void*,HWND*,UINT*,WPARAM*,LPARAM*);
struct NativeString { union { wchar_t small[8]; wchar_t* pointer; }; uint32_t size,capacity; };
static_assert(sizeof(NativeString)==24);
struct Code { uint8_t* memory; uint8_t* call; Caller invoke; };
Code known{},foreign{};
ReadFn readHook=nullptr;
Filter filterHook=nullptr;
std::wstring payload;
LONG reported=0;
unsigned nativeReads=0,nativeMessages=0,nativeClears=0,hooks=0;
bool missing=false;
std::vector<std::string> logs;
void Check(bool condition,const char* text) {if(!condition)throw std::runtime_error(text);printf("PASS %s\n",text);}
LONG WINAPI NativeRead(HIMC,DWORD,void* buffer,DWORD size)
{
    ++nativeReads;
    if(buffer&&size&&!payload.empty())memcpy(buffer,payload.data(),(std::min)(static_cast<size_t>(size),payload.size()*2));
    return reported;
}
void SetCall(Code& code,void* target)
{
    const int32_t rel=static_cast<int32_t>(reinterpret_cast<uintptr_t>(target)-reinterpret_cast<uintptr_t>(code.call+5));
    memcpy(code.call+1,&rel,4);FlushInstructionCache(GetCurrentProcess(),code.memory,64);
}
Code MakeCode()
{
    auto* p=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    if(!p)throw std::runtime_error("fixture allocation failed");
    p[0]=p[1]=0x90;
    for(size_t at=2;at<18;at+=4){p[at]=0xFF;p[at+1]=0x74;p[at+2]=0x24;p[at+3]=0x10;}
    p[18]=0xE8;p[23]=0xC3;
    Code c{p,p+18,reinterpret_cast<Caller>(p)};SetCall(c,reinterpret_cast<void*>(&NativeRead));return c;
}
bool __stdcall NativeFilter(void* object,HWND*,UINT* message,WPARAM*,LPARAM* flags)
{
    ++nativeMessages;auto* p=static_cast<uint8_t*>(object);
    // Exact mask and END behavior independently observed in the supported EXE.
    if(*message==WM_IME_SETCONTEXT&&p[0x24]==0)*flags&=0x3FFFFFF0;
    if(*message==WM_IME_ENDCOMPOSITION)p[0x29]=p[0x2A]=0;
    return false;
}
void* __fastcall NativeAssign(void* self,void*,const wchar_t* value,uint32_t count)
{
    if(count||!value||*value)throw std::runtime_error("unexpected native assignment");
    auto* s=static_cast<NativeString*>(self);++nativeClears;
    if(s->capacity<8)s->small[0]=0;else s->pointer[0]=0;
    s->size=0;return self;
}
void* __fastcall NativeReplace(void* self,void*,uint32_t,uint32_t,const void*,uint32_t,uint32_t){return self;}
void* Symbol(SdgoMod*,const char* name)
{
    if(!strcmp(name,"ime.result_call"))return known.memory;
    if(!strcmp(name,"ime.message_filter"))return missing?nullptr:reinterpret_cast<void*>(&NativeFilter);
    if(!strcmp(name,"ime.native_string_assign"))return reinterpret_cast<void*>(&NativeAssign);
    if(!strcmp(name,"ime.native_replace"))return reinterpret_cast<void*>(&NativeReplace);
    if(!strcmp(name,"ime.result_replace_call"))return known.call;
    return nullptr;
}
int HookImport(SdgoMod*,const char* dll,const char* name,void* detour,void** original)
{
    if(strcmp(dll,"imm32.dll")||strcmp(name,"ImmGetCompositionStringW"))return 0;
    readHook=reinterpret_cast<ReadFn>(detour);*original=reinterpret_cast<void*>(&NativeRead);
    SetCall(known,detour);SetCall(foreign,detour);++hooks;return 1;
}
int HookCode(SdgoMod*,const char* name,void* detour,void** original)
{
    if(!strcmp(name,"ime.native_replace")){*original=reinterpret_cast<void*>(&NativeReplace);++hooks;return 1;}
    if(strcmp(name,"ime.message_filter"))return 0;
    filterHook=reinterpret_cast<Filter>(detour);*original=reinterpret_cast<void*>(&NativeFilter);++hooks;return 1;
}
int Setting(SdgoMod*,const char*,int fallback){return fallback;}
void __cdecl Log(SdgoMod*,int,const char* format,...)
{
    char text[1024]{};va_list args;va_start(args,format);vsnprintf_s(text,sizeof(text),_TRUNCATE,format,args);va_end(args);logs.emplace_back(text);
}
using Object=std::array<uint8_t,0x148>;
template<typename T>T& At(Object& p,size_t offset){return *reinterpret_cast<T*>(p.data()+offset);}
Object Handler()
{
    Object p{};At<uint32_t>(p,0x60)=0x08040804;At<HWND>(p,0x134)=reinterpret_cast<HWND>(0x1234);
    auto& text=At<NativeString>(p,0xA8);text.capacity=7;text.size=5;wcscpy_s(text.small,L"en'lo");
    At<uint32_t>(p,0x54)=5;At<uint32_t>(p,0x58)=0x11223344;At<uint32_t>(p,0x5C)=5;
    memset(p.data()+0x70,0x73,24);memset(p.data()+0xC4,0xC7,24);return p;
}
void Dispatch(Object& p,UINT message,LPARAM& flags,HWND window=nullptr)
{
    if(!window)window=At<HWND>(p,0x134);WPARAM active=1;
    const unsigned count=nativeMessages;Check(!filterHook(p.data(),&window,&message,&active,&flags),"native dispatcher return and stdcall stack retained");
    Check(nativeMessages==count+1,"native dispatcher executes exactly once before postprocessing");
}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc!=3)return 2;
    try {
        known=MakeCode();foreign=MakeCode();
        HMODULE dll=LoadLibraryW(argv[1]);auto entry=dll?reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll,"SdgoModLoad")):nullptr;
        Check(entry!=nullptr,"actual IME Mod DLL export loaded");
        SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Symbol=Symbol;api.HookImport=HookImport;api.HookCode=HookCode;api.SettingInt=Setting;api.Log=Log;
        missing=!wcscmp(argv[2],L"missing_symbol");
        if(!wcscmp(argv[2],L"invalid_api")){api.HookCode=nullptr;Check(!entry(nullptr,nullptr)&&!entry(&api,nullptr)&&hooks==0,"invalid API refuses before hooks");return 0;}
        const int loaded=entry(&api,nullptr);
        if(missing){Check(!loaded&&hooks==0,"missing verified dispatcher refuses before installing either hook");return 0;}
        Check(loaded&&hooks==3&&readHook&&filterHook,"actual Mod registers only its three declared hooks");
        std::array<wchar_t,262> buffer;buffer.fill(L'X');payload=L"神龙";reported=4;
        Check(known.invoke(nullptr,GCS_RESULTSTR,buffer.data(),520)==2&&buffer[2]==0&&buffer[260]==L'X',"known native result call receives UTF-16 count and bounded terminator");
        buffer.fill(L'X');Check(foreign.invoke(nullptr,GCS_RESULTSTR,buffer.data(),520)==4&&buffer[2]==L'X',"foreign result caller keeps exact Win32 byte count and buffer contract");
        Check(known.invoke(nullptr,GCS_COMPSTR,buffer.data(),520)==4,"composition caller keeps bytes so its native shift is not applied twice");
        payload.clear();reported=-1;buffer.fill(L'X');Check(known.invoke(nullptr,GCS_RESULTSTR,buffer.data(),520)==0&&buffer[0]==0,"negative result becomes safe empty commit");
        payload.assign(260,L'中');reported=520;buffer.fill(L'X');Check(known.invoke(nullptr,GCS_RESULTSTR,buffer.data(),520)==259&&buffer[259]==0&&buffer[260]==L'X',"full native result buffer reserves the terminator and preserves outside canary");
        auto p=Handler();const auto initial=p;LPARAM flags=static_cast<LPARAM>(0xC000000F);
        Dispatch(p,WM_IME_SETCONTEXT,flags);Check(flags==0xF&&p==initial,"only incoming candidate bits restored; native composition/guide policy and handler preserved");
        flags=0x80000000;Dispatch(p,WM_IME_SETCONTEXT,flags);Check(flags==0,"a window that did not request candidates is not forced to show them");
        flags=0;Dispatch(p,WM_IME_ENDCOMPOSITION,flags);
        Check(At<NativeString>(p,0xA8).size==0&&At<NativeString>(p,0xA8).small[0]==0&&At<uint32_t>(p,0x54)==0&&At<uint32_t>(p,0x5C)==0,"END without a result clears stale pinyin preedit and composition counters");
        Check(!memcmp(p.data()+0x70,initial.data()+0x70,24)&&!memcmp(p.data()+0xC4,initial.data()+0xC4,24)&&At<uint32_t>(p,0x58)==0x11223344,"committed strings and unrelated delta canary remain unchanged");
        const unsigned once=nativeClears;Dispatch(p,WM_IME_ENDCOMPOSITION,flags);Check(nativeClears==once+1&&At<NativeString>(p,0xA8).size==0,"repeated END remains idempotent");
        p=Handler();std::array<wchar_t,32> heap{};wcscpy_s(heap.data(),heap.size(),L"shen'long");auto& hs=At<NativeString>(p,0xA8);hs.pointer=heap.data();hs.capacity=31;hs.size=9;
        Dispatch(p,WM_IME_ENDCOMPOSITION,flags);Check(hs.pointer==heap.data()&&hs.capacity==31&&hs.size==0&&heap[0]==0,"native heap preedit is cleared without freeing or replacing its allocation");
        p=Handler();At<uint32_t>(p,0x60)=0x04110411;auto old=p;flags=0xF;Dispatch(p,WM_IME_SETCONTEXT,flags);Check(flags==0&&p==old,"Japanese input retains its native candidate behavior");
        Dispatch(p,WM_IME_ENDCOMPOSITION,flags);Check(At<NativeString>(p,0xA8).size==5,"non-Chinese preedit is not modified by Chinese cleanup");
        p=Handler();flags=0;Dispatch(p,WM_IME_ENDCOMPOSITION,flags,reinterpret_cast<HWND>(0x9999));Check(At<NativeString>(p,0xA8).size==5,"foreign HWND cannot clear this handler's preedit");
        p=Handler();At<NativeString>(p,0xA8).size=8;old=p;Dispatch(p,WM_IME_ENDCOMPOSITION,flags);Check(p==old,"invalid native string shape refuses cleanup without changing cached or committed text");
        Check(std::none_of(logs.begin(),logs.end(),[](const std::string& s){return s.find("shen")!=std::string::npos||s.find("en'lo")!=std::string::npos;}),"diagnostic receipts never contain typed text");
        puts("ACTUAL IME DLL FIXTURE PASS; Windows Pinyin candidate rendering and game typing remain manual acceptance");return 0;
    } catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
