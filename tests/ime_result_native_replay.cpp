#include <windows.h>
#include <imm.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"

namespace {
using Filter=bool(__stdcall*)(void*,HWND*,UINT*,WPARAM*,LPARAM*);
using Replay=void*(__cdecl*)(void*);
struct NativeString {union {wchar_t small[8];wchar_t* data;};uint32_t size,capacity;};
static_assert(sizeof(NativeString)==24);
struct State {
    alignas(4) std::array<uint8_t,0x148> object{};
    std::array<wchar_t,128> preedit{},display{},committed{};
    std::wstring edit;
};
struct Code {uint8_t* bytes;uint8_t* site;Replay run;};
// The 29 bytes at supported GOnline VA C48EC0..C48EDC. The test can read
// the exact block from a private local EXE; no game artifact is redistributed.
constexpr uint8_t kNative[] = {0x8B,0x86,0xB8,0,0,0,0x8B,0x4E,0x48,0x6A,0xFF,0x6A,0,0x57,0x50,
    0x8D,0x9E,0xC4,0,0,0,0x51,0x8B,0xCB,0xE8,0x33,0xE8,0xFF,0xFF};
std::vector<uint8_t> block(std::begin(kNative),std::end(kNative));
Code known{},outside{};
Filter hook=nullptr;
State* active=nullptr;
std::wstring resultText;
uint32_t erased=0,sourcePosition=0;
bool useOutside=false,mutatePosition=false,continueComposition=false,nested=false;
std::vector<std::string> logs;
template<typename T>T& Field(State& s,size_t offset){return *reinterpret_cast<T*>(s.object.data()+offset);}
std::wstring Text(const NativeString& s){return std::wstring(s.capacity<8?s.small:s.data,s.size);}
void Put(NativeString& s,const std::wstring& text)
{
    if(text.size()>s.capacity)throw std::runtime_error("fixture native capacity exceeded");
    auto* target=s.capacity<8?s.small:s.data;memcpy(target,text.data(),text.size()*2);target[text.size()]=0;s.size=static_cast<uint32_t>(text.size());
}
void Init(State& s,const std::wstring& preedit,const std::wstring& prefix=L"",const std::wstring& suffix=L"",bool heap=true)
{
    s.object.fill(0);Field<uint32_t>(s,0x60)=0x08040804;Field<HWND>(s,0x134)=reinterpret_cast<HWND>(0x1234);
    Field<uint32_t>(s,0x48)=static_cast<uint32_t>(prefix.size());
    for(auto item:{std::pair<size_t,wchar_t*>(0xA8,s.preedit.data()),{0xC4,s.display.data()},{0x70,s.committed.data()}}) {
        auto& str=Field<NativeString>(s,item.first);str.capacity=heap?127:7;if(heap)str.data=item.second;
    }
    Put(Field<NativeString>(s,0xA8),preedit);Put(Field<NativeString>(s,0xC4),prefix+preedit+suffix);
    Put(Field<NativeString>(s,0x70),prefix+suffix);s.edit=prefix+suffix;
}
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);printf("PASS %s\n",message);}
void* __fastcall NativeReplace(void* self,void*,uint32_t position,uint32_t count,const void* source,uint32_t subpos,uint32_t subcount)
{
    erased=count;sourcePosition=subpos;
    auto& target=*static_cast<NativeString*>(self);const auto value=Text(*static_cast<const NativeString*>(source));
    auto text=Text(target);text.replace(position,count,value.substr(subpos,subcount));Put(target,text);return self;
}
void SetCall(Code& c,void* target)
{
    const int32_t displacement=static_cast<int32_t>(reinterpret_cast<uintptr_t>(target)-reinterpret_cast<uintptr_t>(c.site+5));
    memcpy(c.site+1,&displacement,4);FlushInstructionCache(GetCurrentProcess(),c.bytes,128);
}
Code MakeCode()
{
    auto* p=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!p)throw std::runtime_error("executable fixture allocation failed");
    // cdecl wrapper preserves the host's nonvolatile registers, supplies ESI
    // and EDI, then executes the original native argument-construction block.
    const uint8_t enter[]={0x53,0x56,0x57,0x8B,0x74,0x24,0x10,0x8D,0xBE,0xA8,0,0,0};
    memcpy(p,enter,sizeof(enter));memcpy(p+sizeof(enter),block.data(),block.size());
    const uint8_t leave[]={0x5F,0x5E,0x5B,0xC3};memcpy(p+sizeof(enter)+block.size(),leave,sizeof(leave));
    Code c{p,p+sizeof(enter)+24,reinterpret_cast<Replay>(p)};SetCall(c,reinterpret_cast<void*>(&NativeReplace));return c;
}
void ReadNative(const wchar_t* path)
{
    std::ifstream f(path,std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
    auto u32=[&](size_t at){if(at+4>bytes.size())throw std::runtime_error("truncated PE");uint32_t n;memcpy(&n,bytes.data()+at,4);return n;};
    auto u16=[&](size_t at){if(at+2>bytes.size())throw std::runtime_error("truncated PE");uint16_t n;memcpy(&n,bytes.data()+at,2);return n;};
    const auto pe=u32(0x3C);Check(u32(pe)==0x4550&&u16(pe+4)==0x14C&&u32(pe+8)==0x54BE1568&&u32(pe+24+56)==0xC97000,"supplied private executable matches supported x86 host identity");
    const auto sections=pe+24+u16(pe+20);constexpr uint32_t rva=0x848EC0;bool found=false;
    for(uint16_t i=0;i<u16(pe+6);++i){const size_t at=sections+i*40;const auto va=u32(at+12),raw=u32(at+16),pointer=u32(at+20);
        if(rva>=va&&rva-va+sizeof(kNative)<=raw){const auto file=pointer+(rva-va);Check(file+sizeof(kNative)<=bytes.size()&&!memcmp(bytes.data()+file,kNative,sizeof(kNative)),"original result replace argument block matches all 29 bytes on disk");block.assign(bytes.begin()+file,bytes.begin()+file+sizeof(kNative));found=true;break;}}
    Check(found,"private native callsite extracted read-only");
}
bool __stdcall OriginalFilter(void* object,HWND*,UINT* message,WPARAM*,LPARAM* flags)
{
    if(*message!=WM_IME_COMPOSITION||!((*flags&GCS_RESULTSTR)||!*flags))return false;
    if(nested){nested=false;State inner;Init(inner,L"shen'long");auto* saved=active;const auto savedText=resultText;active=&inner;resultText=L"神龙";
        HWND w=Field<HWND>(inner,0x134);UINT msg=WM_IME_COMPOSITION;WPARAM wp=0;LPARAM lp=GCS_RESULTSTR;hook(inner.object.data(),&w,&msg,&wp,&lp);
        Check(inner.edit==L"神龙","nested native result has its own matching preedit scope");active=saved;resultText=savedText;}
    auto& s=*active;Check(object==s.object.data(),"fixture callback receives the current native handler");
    Put(Field<NativeString>(s,0xA8),resultText);
    if(mutatePosition)++Field<uint32_t>(s,0x48);
    (useOutside?outside:known).run(object);
    s.edit=Text(Field<NativeString>(s,0xC4));Put(Field<NativeString>(s,0x70),s.edit);
    Put(Field<NativeString>(s,0xA8),L"");
    if(continueComposition){
        // A later COMPSTR replace is an ordinary different caller, even in the
        // same message. Its native erase argument must not use the old snapshot.
        Put(Field<NativeString>(s,0xA8),L"next");outside.run(object);
    }
    return false;
}
void* __fastcall Assign(void* self,void*,const wchar_t* text,uint32_t count){Put(*static_cast<NativeString*>(self),std::wstring(text,count));return self;}
void* Symbol(SdgoMod*,const char* name)
{
    if(!strcmp(name,"ime.result_call"))return known.bytes;
    if(!strcmp(name,"ime.message_filter"))return reinterpret_cast<void*>(&OriginalFilter);
    if(!strcmp(name,"ime.native_string_assign"))return reinterpret_cast<void*>(&Assign);
    if(!strcmp(name,"ime.native_replace"))return reinterpret_cast<void*>(&NativeReplace);
    if(!strcmp(name,"ime.result_replace_call"))return known.site;
    return nullptr;
}
int HookCode(SdgoMod*,const char* name,void* detour,void** original)
{
    if(!strcmp(name,"ime.message_filter")){hook=reinterpret_cast<Filter>(detour);*original=reinterpret_cast<void*>(&OriginalFilter);return 1;}
    if(!strcmp(name,"ime.native_replace")){*original=reinterpret_cast<void*>(&NativeReplace);SetCall(known,detour);SetCall(outside,detour);return 1;}
    return 0;
}
LONG WINAPI Read(HIMC,DWORD,void*,DWORD){return 0;}
int HookImport(SdgoMod*,const char*,const char*,void*,void** original){*original=reinterpret_cast<void*>(&Read);return 1;}
int Setting(SdgoMod*,const char*,int fallback){return fallback;}
void __cdecl Log(SdgoMod*,int,const char* format,...){char text[1024]{};va_list a;va_start(a,format);vsnprintf_s(text,sizeof(text),_TRUNCATE,format,a);va_end(a);logs.emplace_back(text);}
void Run(State& s,const std::wstring& result,LPARAM flags=GCS_RESULTSTR,HWND window=nullptr)
{
    active=&s;resultText=result;if(!window)window=Field<HWND>(s,0x134);UINT msg=WM_IME_COMPOSITION;WPARAM wp=0;
    Check(!hook(s.object.data(),&window,&msg,&wp,&flags),"native dispatcher return retained during real callsite replay");
}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc!=2&&argc!=3)return 2;
    try {
        if(argc==3)ReadNative(argv[2]);known=MakeCode();outside=MakeCode();
        State s;Init(s,L"zi'yo",L"",L"",false);active=&s;resultText=L"自由";HWND window=Field<HWND>(s,0x134);UINT msg=WM_IME_COMPOSITION;WPARAM wp=0;LPARAM flags=GCS_RESULTSTR;
        OriginalFilter(s.object.data(),&window,&msg,&wp,&flags);Check(s.edit==L"自由'yo"&&erased==2,"unpatched original x86 argument block reproduces the exact reported residual");
        HMODULE dll=LoadLibraryW(argv[1]);auto entry=dll?reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll,"SdgoModLoad")):nullptr;
        SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Symbol=Symbol;api.HookCode=HookCode;api.HookImport=HookImport;api.SettingInt=Setting;api.Log=Log;
        Check(entry&&entry(&api,nullptr)&&hook,"actual patched DLL and native replace hook loaded");
        Init(s,L"zi'yo",L"",L"",false);Run(s,L"自由");Check(s.edit==L"自由"&&erased==5,"patched actual DLL erases the old five-unit preedit instead of the new two-unit result");
        Init(s,L"shen'long",L"ABC123 ",L" xyz9");Run(s,L"神龙");Check(s.edit==L"ABC123 神龙 xyz9"&&erased==9,"legitimate English/numeric prefix and suffix survive exact range replacement");
        Init(s,L"ab",L"",L"tail");Run(s,L"输入法");Check(s.edit==L"输入法tail"&&erased==2,"result longer than preedit does not erase legitimate following text");
        Init(s,L"ab");Run(s,L"自由");Check(s.edit==L"自由"&&erased==2,"equal UTF-16 lengths retain native behavior");
        Init(s,std::wstring({static_cast<wchar_t>(0xD83D),static_cast<wchar_t>(0xDE00),L'x'}));Run(s,L"字");Check(s.edit==L"字"&&erased==3,"preedit erase length counts UTF-16 units including surrogate pairs");
        Init(s,L"zi'yo");Run(s,L"",0);Check(s.edit.empty()&&erased==5,"zero-flag cancellation removes only the exact old preedit range");
        Init(s,L"zi'yo");continueComposition=true;Run(s,L"自由",GCS_RESULTSTR|GCS_COMPSTR);continueComposition=false;Check(s.edit==L"自由"&&erased==4,"later ordinary composition replace keeps its own count in a combined message");
        Init(s,L"zi'yo");nested=true;Run(s,L"自由");Check(s.edit==L"自由"&&erased==5,"nested dispatch restores the outer preedit snapshot");
        Init(s,L"zi'yo");Put(Field<NativeString>(s,0xC4),L"unchanged");Run(s,L"自由");Check(erased==2,"unmatched display/preedit content cannot authorize deleting extra text");
        Init(s,L"zi'yo");useOutside=true;Run(s,L"自由");useOutside=false;Check(erased==2,"different native caller bypasses result count correction");
        Init(s,L"zi'yo");mutatePosition=true;Run(s,L"自由");mutatePosition=false;Check(erased==2,"changed insertion position refuses old range substitution");
        Init(s,L"zi'yo");const uint32_t foreignSelf=0x70;memcpy(known.bytes+13+17,&foreignSelf,4);Run(s,L"自由");Check(erased==2,"correct return address with foreign string object is rejected");const uint32_t displayOffset=0xC4;memcpy(known.bytes+13+17,&displayOffset,4);
        Init(s,L"zi'yo");const uint32_t foreignSource=0x70;memcpy(known.bytes+9,&foreignSource,4);Run(s,L"自由");Check(erased==2,"correct caller with foreign source string is rejected");const uint32_t preeditOffset=0xA8;memcpy(known.bytes+9,&preeditOffset,4);
        Init(s,L"zi'yo");Run(s,L"自由",GCS_RESULTSTR,reinterpret_cast<HWND>(0x5678));Check(erased==2,"another HWND cannot authorize preedit replacement");
        Init(s,L"zi'yo");Field<uint32_t>(s,0x60)=0x04110411;Run(s,L"自由");Check(erased==2,"non-Chinese dispatch keeps native replacement arguments");
        Check(std::any_of(logs.begin(),logs.end(),[](const std::string& x){return x.find("erase count corrected")!=std::string::npos;}),"a successful correction emits a length-only runtime receipt");
        Check(std::none_of(logs.begin(),logs.end(),[](const std::string& x){return x.find("zi'yo")!=std::string::npos||x.find("shen'long")!=std::string::npos;}),"replay diagnostics never include the typed preedit");
        puts("REAL NATIVE ARGUMENT BLOCK + ACTUAL MOD DLL REPLAY PASS; full Windows/game IME acceptance remains manual");return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
