#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include "sdgo/sdgo_api.h"
#include "../mods/shop_localization/dictionary.h"
#include "../mods/shop_localization/sites.h"

namespace {
using namespace sdgo::shop_localization;
using AssignHook = void*(__fastcall*)(void*, void*, const wchar_t*, uint32_t);
using CopyHook = void*(__fastcall*)(void*, void*, const void*, uint32_t, uint32_t);
using Caller = void*(__cdecl*)(void*, const void*, uint32_t, uint32_t);
struct Destination { uint32_t id = 1010465, price = 9900, payment = 2, validity = 7; std::wstring text; };
struct NativeString { union { wchar_t small[8]; const wchar_t* ptr; }; uint32_t size; uint32_t capacity; };
static_assert(sizeof(NativeString) == 24);
AssignHook g_assign = nullptr; CopyHook g_copy = nullptr;
unsigned g_hooks = 0, g_nativeCalls = 0;
int g_mode = 0;
std::wstring g_directory;
struct Code { uint8_t* bytes; uint8_t* site; Caller call; };
Code g_normal{}, g_preview{}, g_option{}, g_copyCode{}, g_outside{};

void* __fastcall NativeAssign(void* self, void*, const wchar_t* text, uint32_t length)
{
    ++g_nativeCalls; static_cast<Destination*>(self)->text.assign(text, length); return self;
}
void* __fastcall NativeCopy(void* self, void*, const void* value, uint32_t pos, uint32_t count)
{
    const auto& s = *static_cast<const NativeString*>(value);
    const auto* text = s.capacity < 8 ? s.small : s.ptr;
    return NativeAssign(self, nullptr, text + pos, (std::min)(count, s.size - pos));
}
void SetCall(Code& code, void* function)
{
    int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(code.site + 5));
    memcpy(code.site + 1, &relative, 4); FlushInstructionCache(GetCurrentProcess(), code.bytes, 64);
}
Code MakeCode(bool copy)
{
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) throw std::runtime_error("fixture executable allocation failed");
    size_t at = 0;
    // cdecl fixture(dest, source, count/pos, count); native thiscall target.
    const uint8_t ecx[] = {0x8B,0x4C,0x24,0x04}; memcpy(p+at,ecx,4);at+=4;
    auto pushArgument = [&](uint8_t offset) { p[at++]=0xFF;p[at++]=0x74;p[at++]=0x24;p[at++]=offset; };
    if (copy) {pushArgument(16);pushArgument(16);pushArgument(16);}
    else {pushArgument(12);pushArgument(12);}
    Code result{p,p+at,reinterpret_cast<Caller>(p)};p[at++]=0xE8;at+=4;p[at++]=0xC3;
    SetCall(result,copy?reinterpret_cast<void*>(&NativeCopy):reinterpret_cast<void*>(&NativeAssign));return result;
}
void* Symbol(SdgoMod*, const char* name)
{
    if (g_mode == 5) return nullptr;
    for (const auto& s : kSites) {
        if (strcmp(s.name,name)) continue;
        if (!strcmp(s.name, "shop_localization.call.006EF404")) return g_preview.site;
        return s.operation==Operation::Copy ? g_copyCode.site : s.option ? g_option.site : g_normal.site;
    }
    return nullptr;
}
int Hook(SdgoMod*, const char* name, void* detour, void** original)
{
    if (!strcmp(name,"shop_localization.assign")) {g_assign=reinterpret_cast<AssignHook>(detour);*original=reinterpret_cast<void*>(&NativeAssign);SetCall(g_normal,detour);SetCall(g_preview,detour);SetCall(g_option,detour);SetCall(g_outside,detour);}
    else if (!strcmp(name,"shop_localization.copy")) {g_copy=reinterpret_cast<CopyHook>(detour);*original=reinterpret_cast<void*>(&NativeCopy);SetCall(g_copyCode,detour);}
    else return 0;
    ++g_hooks;return 1;
}
int Setting(SdgoMod*,const char* key,int fallback)
{
    if (g_mode==1&&!strcmp(key,"TranslateNames"))return 0;
    if (g_mode==2&&!strcmp(key,"TranslateDescriptions"))return 0;
    if (g_mode==3&&!strcmp(key,"TranslateOptions"))return 0;
    return fallback;
}
const wchar_t* Directory(SdgoMod*) {return g_directory.c_str();}
void __cdecl Log(SdgoMod*,int,const char*,...) {}
void Check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);printf("PASS %s\n",message);}
void Call(Code& code,Destination& d,const wchar_t* source) {Check(code.call(&d,source,static_cast<uint32_t>(wcslen(source)),0)==&d,"native thiscall return and stack retained");}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc!=4&&argc!=5)return 2;
    try {
        g_directory=argv[2];g_mode=_wtoi(argv[3]);CreateDirectoryW(g_directory.c_str(),nullptr);
        if(g_mode==6) {
            if(argc!=5||!CopyFileW(argv[4],(g_directory+L"\\zh-CN.ini").c_str(),FALSE))throw std::runtime_error("production dictionary fixture copy failed");
        } else {
        std::ofstream file(g_directory+L"\\zh-CN.ini",std::ios::binary);
        file << (g_mode==4 ? "[entry:bad]\nKind=name\nSource=GB\nText=wrong\n" :
            "[entry:unit]\nKind=name\nSource=GUNDAM\nText=\xE9\xAB\x98\xE8\xBE\xBE\n"
            "[entry:desc]\nKind=description\nSource=Provides ammunition.\nText=\xE8\xA1\xA5\xE5\x85\x85\xE5\xBC\xB9\xE8\x8D\xAF\xE3\x80\x82\n"
            "[entry:term]\nKind=option\nSource=Permanent\nText=\xE6\xB0\xB8\xE4\xB9\x85\n");file.close();
        }
        g_normal=MakeCode(false);g_preview=MakeCode(false);g_option=MakeCode(false);g_copyCode=MakeCode(true);g_outside=MakeCode(false);
        HMODULE dll=LoadLibraryW(argv[1]);auto load=dll?reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll,"SdgoModLoad")):nullptr;
        SdgoApi api{};api.size=sizeof(api);api.version=1;api.Symbol=Symbol;api.HookCode=Hook;api.SettingInt=Setting;api.ModDirectory=Directory;api.Log=Log;
        int result=load?load(&api,nullptr):0;
        if(g_mode==4||g_mode==5) {Check(!result&&g_hooks==0,"invalid dictionary or unverified caller refuses before installing hooks");return 0;}
        Check(result&&g_hooks==2,"actual DLL loaded, converter/populator hooks not taken from shop_labels");
        if(g_mode==6) {
            Destination actual;Call(g_normal,actual,L"Unknown custom host product");Check(actual.text==L"Unknown custom host product","complete production dictionary loads and preserves unknown custom text");
            Call(g_preview,actual,L"BUILD STRIKE GUNDAM (ENHANCED BEAM RIFLE EQUIP)");Check(actual.text==L"创世强袭高达（强化光束步枪装备）","shared preview leaf translates the reported shop blueprint title");
            Call(g_preview,actual,L"BUILD STRIKE GUNDAM (ENHANCED BEAM RIFLE)");Check(actual.text==L"创世强袭高达（强化光束步枪装备）","shared preview translates the alternate native blueprint title spelling");
            Call(g_preview,actual,L"JEGAN (TYPE R)");Check(actual.text==L"杰刚 A型","independent shared preview caller translates the reported normal-capsule title");
            Call(g_preview,actual,L"Unknown future preview unit");Check(actual.text==L"Unknown future preview unit","unknown shared-preview titles remain original");
            Call(g_outside,actual,L"JEGAN (TYPE R)");Check(actual.text==L"JEGAN (TYPE R)","the same known unit name remains original outside verified display callers");
            Call(g_option,actual,L"30วัน");Check(actual.text==L"30 天","production dictionary retains numeric option grammar");return 0;
        }
        Destination d;const wchar_t source[]=L"GUNDAM";Call(g_normal,d,source);Check(d.text==(g_mode==1?L"GUNDAM":L"高达"),"name translation and switch");Check(!wcscmp(source,L"GUNDAM"),"original source memory untouched");
        Call(g_preview,d,source);Check(d.text==(g_mode==1?L"GUNDAM":L"高达"),"independent shared preview caller obeys the existing name switch");
        Call(g_normal,d,L"Provides ammunition.");Check(d.text==(g_mode==2?L"Provides ammunition.":L"补充弹药。"),"description translation and switch");
        Call(g_option,d,L"7วัน");Check(d.text==(g_mode==3?L"7วัน":L"7 天"),"Thai option grammar and switch");
        Call(g_option,d,L"30ชิ้");Check(d.text==(g_mode==3?L"30ชิ้":L"30 件（原文截断）"),"known truncated count option is translated without hiding the damaged unit text");
        Call(g_option,d,L"20น");Check(d.text==(g_mode==3?L"20น":L"20（原文单位不完整）"),"unidentifiable native option unit is explicitly marked rather than invented");
        Call(g_option,d,L"");Check(d.text==(g_mode==3?L"":L"未提供选项文字"),"blank sales caption is marked without inventing an amount or rental term");
        Call(g_normal,d,L"7วัน");Check(d.text==L"7วัน","option grammar cannot reinterpret a price or other name call");
        Call(g_normal,d,L"Unknown custom product");Check(d.text==L"Unknown custom product","unknown host text preserved");
        Call(g_normal,d,L"9,900");Check(d.text==L"9,900","numeric price text unchanged");
        Call(g_normal,d,L"租赁 7 天");Check(d.text==L"租赁 7 天","authoritative shop_labels rental output preserved");
        Call(g_outside,d,L"GUNDAM");Check(d.text==L"GUNDAM","non-shop native caller bypasses localization");
        NativeString small{};wcscpy_s(small.small,L"GUNDAM");small.size=6;small.capacity=7;
        Check(g_copyCode.call(&d,&small,0,UINT32_MAX)==&d,"copy overload retains native return");Check(d.text==(g_mode==1?L"GUNDAM":L"高达"),"native SSO copy translated safely");
        NativeString heap{};heap.ptr=L"Provides ammunition.";heap.size=static_cast<uint32_t>(wcslen(heap.ptr));heap.capacity=heap.size;
        g_copyCode.call(&d,&heap,0,UINT32_MAX);Check(d.text==(g_mode==2?L"Provides ammunition.":L"补充弹药。"),"native heap string copy translated safely");
        Check(d.id==1010465&&d.price==9900&&d.payment==2&&d.validity==7,"business canaries unchanged across real DLL calls");
        printf("SHOP LOCALIZATION ACTUAL DLL PASS mode=%d native copies=%u\n",g_mode,g_nativeCalls);return 0;
    } catch(const std::exception& e) {fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
