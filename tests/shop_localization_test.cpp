#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include "sdgo/acquisition_sources.h"
#include "sdgo/content_localization_boundary.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/shop_labels.h"
#include "acquisition_sources_fixture.h"
#include "../mods/shop_localization/dictionary.h"
#include "../mods/shop_localization/sites.h"

namespace {
using namespace sdgo::shop_localization;
using AssignHook = void*(__fastcall*)(void*, void*, const wchar_t*, uint32_t);
using CopyHook = void*(__fastcall*)(void*, void*, const void*, uint32_t, uint32_t);
using InitialPopupHook = void(__fastcall*)(void*, void*);
using Caller = void*(__cdecl*)(void*, const void*, uint32_t, uint32_t);
using UnitNameHook =
    void(__fastcall*)(void*, void*, void*, const std::uint8_t*);
using UnitNameCaller =
    void(__cdecl*)(void*, void*, const std::uint8_t*);
using Service =
    uint32_t(__cdecl*)(uint32_t, uint32_t, const wchar_t*, uint32_t,
                       wchar_t*, uint32_t);
using OriginSet = void(__cdecl*)(uint32_t, uint32_t);
struct Destination { uint32_t id = 1010465, price = 9900, payment = 2, validity = 7; std::wstring text; };
struct NativeString { union { wchar_t small[8]; const wchar_t* ptr; }; uint32_t size; uint32_t capacity; };
static_assert(sizeof(NativeString) == 24);
AssignHook g_assign = nullptr; CopyHook g_copy = nullptr;
InitialPopupHook g_buyOptions = nullptr, g_giftOptions = nullptr;
unsigned g_hooks = 0, g_nativeCalls = 0;
int g_mode = 0;
std::wstring g_directory;
struct Code { uint8_t* bytes; uint8_t* site; Caller call; };
struct UnitNameCode {
    uint8_t* bytes;
    uint8_t* site;
    UnitNameCaller call;
};
Code g_normal{}, g_preview{}, g_option{}, g_copyCode{}, g_outside{};
UnitNameCode g_previewOuter{}, g_unknownOuter{};
UnitNameHook g_unitPlain = nullptr;
UnitNameHook g_unitPreview = nullptr;
UnitNameHook g_unitDisplay = nullptr;
const wchar_t* g_unitAssignedSource = nullptr;
Destination* g_popupDestination = nullptr;
const wchar_t* g_popupSource = nullptr;
uint32_t g_popupLength = 0;

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
void __fastcall NativeInitialPopup(void*, void*)
{
    if (g_popupDestination && g_popupSource)
        g_normal.call(g_popupDestination, g_popupSource, g_popupLength, 0);
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
void SetCall(UnitNameCode& code, void* function)
{
    const auto next = reinterpret_cast<std::intptr_t>(code.site + 5);
    const auto distance = reinterpret_cast<std::intptr_t>(function) - next;
    if (distance < (std::numeric_limits<std::int32_t>::min)() ||
        distance > (std::numeric_limits<std::int32_t>::max)())
        throw std::runtime_error("unit-name fixture call is out of range");
    const auto relative = static_cast<std::int32_t>(distance);
    memcpy(code.site + 1, &relative, sizeof(relative));
    FlushInstructionCache(GetCurrentProcess(), code.bytes, 64);
}
UnitNameCode MakeUnitNameCode()
{
    auto* p = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) throw std::runtime_error("unit-name fixture allocation failed");
    const uint8_t body[] = {
        0x8B, 0x4C, 0x24, 0x04,        // mov ecx,[esp+4]
        0x33, 0xD2,                    // xor edx,edx
        0x8B, 0x44, 0x24, 0x0C,        // mov eax,[esp+12] record
        0x50,                          // push eax
        0x8B, 0x44, 0x24, 0x0C,        // mov eax,[esp+12] control
        0x50,                          // push eax
        0xE8, 0x00, 0x00, 0x00, 0x00, // call rel32
        0xC3                           // ret
    };
    memcpy(p, body, sizeof(body));
    return {p, p + 16, reinterpret_cast<UnitNameCaller>(p)};
}
void __fastcall NativeUnitName(void* self, void*, void*,
                               const std::uint8_t* record)
{
    if (g_unitAssignedSource) {
        g_preview.call(self, g_unitAssignedSource,
                       static_cast<uint32_t>(wcslen(g_unitAssignedSource)),
                       0);
        return;
    }
    uint32_t length = 0;
    while (length < 50) {
        std::uint16_t code = 0;
        memcpy(&code, record + 5 + length * sizeof(code), sizeof(code));
        if (!code) break;
        ++length;
    }
    g_preview.call(self, record + 5, length, 0);
}
void* Symbol(SdgoMod*, const char* name)
{
    if (g_mode == 5) return nullptr;
    if (!strcmp(name,"shop_localization.search.submit") || !strcmp(name,"shop_localization.search.wide_to_cp")) return reinterpret_cast<void*>(&NativeAssign);
    if (!strcmp(name,"shop_localization.search.resource_to_string") || !strcmp(name,"shop_localization.assign")) return reinterpret_cast<void*>(&NativeAssign);
    if (!strcmp(name,"shop_localization.search.edit_text")) return reinterpret_cast<void*>(&NativeAssign);
    if (!strcmp(name,"shop_localization.search.call.submit_convert") || !strcmp(name,"shop_localization.search.call.page_convert") || !strcmp(name,"shop_localization.search.call.rank_all") || !strcmp(name,"shop_localization.search.call.payment_all")) return g_outside.site;
    if (g_mode==7 && (!strcmp(name,"shop_localization.grid_data") || !strcmp(name,"shop_localization.grid_find") ||
        !strcmp(name,"shop_localization.native_new") || !strcmp(name,"shop_localization.native_delete") ||
        !strcmp(name,"shop_localization.grid_make_node") || !strcmp(name,"shop_localization.grid_insert") ||
        !strcmp(name,"shop_localization.grid_register") || !strcmp(name,"shop_localization.grid_map_find")))
        return reinterpret_cast<void*>(&NativeAssign);
    if (!strcmp(name, "shop_localization.unit_name_display.call.0083A5A4") ||
        !strcmp(name, "shop_localization.unit_name_display.call.00970764") ||
        !strcmp(name, "shop_localization.unit_name_display.call.00812654") ||
        !strcmp(name, "shop_localization.unit_name_display.call.0089751A") ||
        !strcmp(name, "shop_localization.unit_name_display.call.008C051C") ||
        !strcmp(name, "shop_localization.unit_name_display.call.009CA9D4") ||
        !strcmp(name, "shop_localization.unit_name_display.call.005F6BFD") ||
        !strcmp(name, "shop_localization.unit_name_display.call.00620E58"))
        return g_previewOuter.site;
    if (!strcmp(name, "shop_localization.unit_name_display.assign_call.006EF1F4") ||
        !strcmp(name, "shop_localization.unit_name_display.assign_call.006EF31D") ||
        !strcmp(name, "shop_localization.unit_name_display.assign_call.006EF404"))
        return g_preview.site;
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
    else if (!strcmp(name,"shop_localization.search.submit") || !strcmp(name,"shop_localization.search.wide_to_cp") || !strcmp(name,"shop_localization.search.resource_to_string")) {*original=reinterpret_cast<void*>(&NativeAssign);++g_hooks;return 1;}
    else if (!strcmp(name, "shop_localization.unit_name_display.unit_name_5")) {
        g_unitPlain = reinterpret_cast<UnitNameHook>(detour);
        *original = reinterpret_cast<void*>(&NativeUnitName);
    }
    else if (!strcmp(name, "shop_localization.unit_name_display.unit_name_105")) {
        g_unitPreview = reinterpret_cast<UnitNameHook>(detour);
        *original = reinterpret_cast<void*>(&NativeUnitName);
    }
    else if (!strcmp(name, "shop_localization.unit_name_display.unit_name_205")) {
        g_unitDisplay = reinterpret_cast<UnitNameHook>(detour);
        *original = reinterpret_cast<void*>(&NativeUnitName);
    }
    else if (g_mode==7 && !strcmp(name,"shop_localization.buy_options")) {g_buyOptions=reinterpret_cast<InitialPopupHook>(detour);*original=reinterpret_cast<void*>(&NativeInitialPopup);}
    else if (g_mode==7 && !strcmp(name,"shop_localization.gift_options")) {g_giftOptions=reinterpret_cast<InitialPopupHook>(detour);*original=reinterpret_cast<void*>(&NativeInitialPopup);}
    else if (g_mode==7 && (!strcmp(name,"shop_localization.load_store") || !strcmp(name,"shop_localization.render_list") ||
        !strcmp(name,"shop_localization.buy_popup") || !strcmp(name,"shop_localization.gift_popup") ||
        !strcmp(name,"shop_localization.hover") || !strcmp(name,"shop_localization.hover_alt")))
        *original=reinterpret_cast<void*>(&NativeInitialPopup);
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
std::array<std::uint8_t, 107> UnitRecord(std::uint32_t id,
                                         const wchar_t* source)
{
    std::array<std::uint8_t, 107> record{};
    memcpy(record.data(), &id, sizeof(id));
    const auto length = (std::min<std::size_t>)(wcslen(source), 49);
    for (std::size_t index = 0; index < length; ++index) {
        const auto code = static_cast<std::uint16_t>(source[index]);
        memcpy(record.data() + 5 + index * sizeof(code), &code,
               sizeof(code));
    }
    return record;
}
void CallUnitName(UnitNameCode& code, Destination& destination,
                  std::uint32_t id, const wchar_t* source)
{
    const auto record = UnitRecord(id, source);
    code.call(&destination, nullptr, record.data());
}
void CallUnitNameAs(UnitNameCode& code, Destination& destination,
                    std::uint32_t id, const wchar_t* primary,
                    const wchar_t* assigned)
{
    const auto record = UnitRecord(id, primary);
    g_unitAssignedSource = assigned;
    code.call(&destination, nullptr, record.data());
    g_unitAssignedSource = nullptr;
}
HMODULE LoadOriginProvider(const wchar_t* path)
{
    Check(GetModuleHandleW(L"unit_catalog.dll") == nullptr,
          "content-origin fixture begins without another provider loaded");
    const HMODULE module = LoadLibraryW(path);
    Check(module != nullptr &&
              GetModuleHandleW(L"unit_catalog.dll") == module,
          "content-origin fixture loaded under the production basename");
    return module;
}
OriginSet OriginControl(HMODULE module)
{
    const auto control = reinterpret_cast<OriginSet>(
        GetProcAddress(module, "SdgoShopLocalizationOriginFixtureSet"));
    Check(control != nullptr,
          "modern content-origin fixture exposes its test-only control");
    return control;
}
void UnloadOriginProvider(HMODULE module)
{
    Check(FreeLibrary(module) != 0 &&
              GetModuleHandleW(L"unit_catalog.dll") == nullptr,
          "content-origin fixture unloaded without leaking provider state");
}
}
int wmain(int argc,wchar_t** argv)
{
    if (argc != 6 && argc != 7) return 2;
    try {
        g_directory=argv[2];g_mode=_wtoi(argv[3]);std::filesystem::create_directories(g_directory);
        if ((g_mode == 6 && argc != 7) || (g_mode != 6 && argc != 6))
            return 2;
        const wchar_t* originFixture = argv[g_mode == 6 ? 5 : 4];
        const wchar_t* legacyOriginFixture = argv[g_mode == 6 ? 6 : 5];
        if(g_mode==6) {
            if(!CopyFileW(argv[4],(g_directory+L"\\zh-CN.ini").c_str(),FALSE))throw std::runtime_error("production dictionary fixture copy failed");
        } else {
        std::ofstream file(g_directory+L"\\zh-CN.ini",std::ios::binary);
        file << (g_mode==4 ? "[entry:bad]\nKind=name\nSource=GB\nText=wrong\n" :
            "[entry:unit]\nKind=name\nSource=GUNDAM\nText=\xE9\xAB\x98\xE8\xBE\xBE\n"
            "[entry:stock]\nKind=name\nSource=ZAKU PHANTOM (SLASH WIZARD) (YZAK CUSTOM)\n"
            "Text=\xE6\x96\xA9\xE5\x87\xBB\xE5\x9E\x8B\xE6\x89\x8E\xE5\x8F\xA4"
            "\xE5\xB9\xBB\xE5\xBD\xB1\xEF\xBC\x88\xE4\xBC\x8A\xE6\x89\x8E\xE5"
            "\x85\x8B\xE4\xB8\x93\xE7\x94\xA8\xEF\xBC\x89\n"
            "[entry:desc]\nKind=description\nSource=Provides ammunition.\nText=\xE8\xA1\xA5\xE5\x85\x85\xE5\xBC\xB9\xE8\x8D\xAF\xE3\x80\x82\n"
            "[entry:term]\nKind=option\nSource=Permanent\nText=\xE6\xB0\xB8\xE4\xB9\x85\n");file.close();
        }
        if(g_mode==7) {
            const auto labels=std::filesystem::path(g_directory).parent_path()/L"shop_labels";
            std::filesystem::create_directories(labels);
            const std::wstring profile=L"\xFEFF[product:1039001]\r\nName=美竹兰副官\r\nTemplateId=1030001\r\nCategory=1219047\r\nPrice=100\r\nPayment=1\r\nValidity=1\r\n";
            std::ofstream display(labels/L"display.ini",std::ios::binary);
            display.write(reinterpret_cast<const char*>(profile.data()),static_cast<std::streamsize>(profile.size()*sizeof(wchar_t)));
        }
        if(g_mode==0) {
            Check(acquisition_fixture::Write(
                std::filesystem::path(g_directory)/"acquisition-sources.bin",1,
                {{SDGO_ACQUISITION_UNIT,10001,"shop:unit:10001:purchase",L"shop fixture purchase"},
                 {SDGO_ACQUISITION_BLUEPRINT,200001,"shop:blueprint:200001",L"shop fixture blueprint purchase"}}),
                "shop acquisition fixture written");
        }
        g_normal=MakeCode(false);g_preview=MakeCode(false);g_option=MakeCode(false);g_copyCode=MakeCode(true);g_outside=MakeCode(false);
        g_previewOuter=MakeUnitNameCode();g_unknownOuter=MakeUnitNameCode();
        HMODULE dll=LoadLibraryW(argv[1]);auto load=dll?reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll,"SdgoModLoad")):nullptr;
        const auto acquisition=dll?reinterpret_cast<SdgoAcquisitionSourcesV1Fn>(GetProcAddress(dll,SDGO_ACQUISITION_SOURCES_EXPORT)):nullptr;
        Check(acquisition!=nullptr,"shop provider exports optional acquisition ABI");
        const SdgoAcquisitionRequestV1 acquisitionRequest{sizeof(SdgoAcquisitionRequestV1),1,SDGO_ACQUISITION_UNIT,10001};
        acquisition_fixture::Capture acquisitionCapture;
        const SdgoAcquisitionSinkV1 acquisitionSink{sizeof(SdgoAcquisitionSinkV1),1,&acquisitionCapture,&acquisition_fixture::Emit};
        Check(acquisition(&acquisitionRequest,&acquisitionSink)==SDGO_ACQUISITION_UNAVAILABLE&&acquisitionCapture.rows.empty(),"shop acquisition provider is unavailable before Mod initialization");
        SdgoApi api{};api.size=sizeof(api);api.version=1;api.Symbol=Symbol;api.HookCode=Hook;api.SettingInt=Setting;api.ModDirectory=Directory;api.Log=Log;
        int result=load?load(&api,nullptr):0;
        acquisitionCapture.rows.clear();
        if(g_mode==0) {
            Check(acquisition(&acquisitionRequest,&acquisitionSink)==SDGO_ACQUISITION_READY&&
                  acquisitionCapture.rows.size()==1&&
                  acquisitionCapture.rows[0].first=="shop:unit:10001:purchase"&&
                  acquisitionCapture.rows[0].second==L"shop fixture purchase",
                  "loaded shop provider publishes the exact unit acquisition source");
            acquisitionCapture.rows.clear();
            const SdgoAcquisitionRequestV1 blueprintRequest{sizeof(SdgoAcquisitionRequestV1),1,SDGO_ACQUISITION_BLUEPRINT,200001};
            Check(acquisition(&blueprintRequest,&acquisitionSink)==SDGO_ACQUISITION_READY&&
                  acquisitionCapture.rows.size()==1&&
                  acquisitionCapture.rows[0].first=="shop:blueprint:200001"&&
                  acquisitionCapture.rows[0].second==L"shop fixture blueprint purchase",
                  "loaded shop provider publishes the exact blueprint acquisition source");
        } else {
            Check(acquisition(&acquisitionRequest,&acquisitionSink)==SDGO_ACQUISITION_UNAVAILABLE&&acquisitionCapture.rows.empty(),"missing optional shop source sidecar never publishes stale claims");
        }
        if(g_mode==4||g_mode==5) {Check(!result&&g_hooks==0,"invalid dictionary or unverified caller refuses before installing hooks");return 0;}
        Check(result&&g_hooks==(g_mode==7?16U:8U),"actual DLL loaded only the scoped hooks required by its current display profile");
        Check(g_unitPlain && g_unitPreview && g_unitDisplay,
              "actual DLL installed all three narrow unit-name context hooks");
        SetCall(g_previewOuter, reinterpret_cast<void*>(g_unitPreview));
        SetCall(g_unknownOuter, reinterpret_cast<void*>(g_unitPreview));
        const auto service=reinterpret_cast<Service>(GetProcAddress(dll,"SdgoLocalizationLookupV1"));
        Check(service!=nullptr,"optional localization C ABI is exported by the actual DLL");
        wchar_t shared[128];std::fill(std::begin(shared),std::end(shared),L'X');
        if(g_mode!=6) {
            const auto required=service(1,10001,L"GUNDAM",6,nullptr,0);
            Check(required==(g_mode==1?0U:3U),"name service follows the existing name switch");
            if(required) {
                Check(service(1,10001,L"GUNDAM",6,shared,2)==3&&shared[0]==L'X',"undersized C ABI output remains untouched");
                Check(service(1,10001,L"GUNDAM",6,shared,128)==3&&!wcscmp(shared,L"高达"),"caller-owned C ABI output receives the complete translation");
            }
            Check(service(4,0,L"GUNDAM",6,shared,128)==0&&service(1,0,nullptr,1,shared,128)==0,"invalid C ABI kind or source refuses safely");
        }
        if(g_mode==6) {
            Check(service(1,10001,L"Gundam",6,shared,128)>0&&!wcscmp(shared,L"高达"),"stable UnitID resolves independently of the English alias casing");
            Check(service(2,2220008,L"unknown reward alias",20,shared,128)>0&&wcsstr(shared,L"200"),"stable ItemID resolves reward names without rewriting quantity");
            constexpr wchar_t updatingNotice[]=L"The room title is still updating. Your text was kept; reopen the title editor to retry.";
            Check(service(3,0,updatingNotice,static_cast<uint32_t>(wcslen(updatingNotice)),shared,128)>0&&
                  !wcscmp(shared,L"房间标题仍在更新。已保留你输入的文字；请重新打开标题编辑器后重试。"),
                  "production UI dictionary resolves the title-update notice");
            constexpr wchar_t queueNotice[]=L"The room-title control could not queue the change. Your text was kept; reopen the title editor to retry.";
            Check(service(3,0,queueNotice,static_cast<uint32_t>(wcslen(queueNotice)),shared,128)>0&&
                  !wcscmp(shared,L"房间标题控件无法将本次更改加入待处理队列。已保留你输入的文字；请重新打开标题编辑器后重试。"),
                  "production UI dictionary resolves the title-queue notice");
            Destination actual;Call(g_normal,actual,L"Unknown custom host product");Check(actual.text==L"Unknown custom host product","complete production dictionary loads and preserves unknown custom text");
            Call(g_preview,actual,L"BUILD STRIKE GUNDAM (ENHANCED BEAM RIFLE EQUIP)");
            Check(actual.text==L"BUILD STRIKE GUNDAM (ENHANCED BEAM RIFLE EQUIP)",
                  "shared assignment site alone cannot translate without a verified getter context");
            CallUnitName(g_previewOuter,actual,1900000001,L"BUILD STRIKE GUNDAM (ENHANCED BEAM RIFLE EQUIP)");Check(actual.text==L"创世强袭高达（强化光束步枪装备）","shared preview leaf translates the reported shop blueprint title through the real getter context");
            CallUnitName(g_previewOuter,actual,1900000002,L"BUILD STRIKE GUNDAM (ENHANCED BEAM RIFLE)");Check(actual.text==L"创世强袭高达（强化光束步枪装备）","shared preview translates the alternate native blueprint title spelling through the real getter context");
            CallUnitName(g_previewOuter,actual,1900000003,L"JEGAN (TYPE R)");Check(actual.text==L"杰刚 A型","independent shared preview caller translates the reported normal-capsule title");
            CallUnitName(g_previewOuter,actual,1900000004,L"Unknown future preview unit");Check(actual.text==L"Unknown future preview unit","unknown shared-preview titles remain original");
            CallUnitName(g_unknownOuter,actual,1900000003,L"JEGAN (TYPE R)");
            Check(actual.text==L"JEGAN (TYPE R)",
                  "unknown getter callers execute the native assignment without borrowing TLS context");
            Call(g_outside,actual,L"JEGAN (TYPE R)");Check(actual.text==L"JEGAN (TYPE R)","the same known unit name remains original outside verified display callers");
            constexpr wchar_t dynamesDescription[]=L"รูปแบบที่ซ่อมแซมและเปลี่ยนแปลงของ DYNAMES ที่ถูกทำลายจากการต่อสู้กับ United Nations Army (国連軍)";
            Call(g_normal,actual,dynamesDescription);
            Check(actual.text==L"在与联合国军的战斗中被摧毁的力天使高达的修复改造型。",
                  "production dictionary translates the previously uncovered Dynames description");
            Call(g_normal,actual,L"Happy Newyear 2012  Festival");
            Check(actual.text==L"2012新年庆典",
                  "exact runtime lookup reuses the existing event-name translation in native prose slots");
            constexpr wchar_t stockUnitName[]=L"ZAKU PHANTOM (SLASH WIZARD) (YZAK CUSTOM)";
            Call(g_normal,actual,stockUnitName);
            Check(actual.text==L"斩击型扎古幻影（伊扎克专用）",
                  "production dictionary translates one reviewed stock unit label");
            CallUnitName(g_previewOuter,actual,1900000005,stockUnitName);
            Check(actual.text==L"斩击型扎古幻影（伊扎克专用）",
                  "shared preview getter context translates the same reviewed stock unit label");
            Call(g_outside,actual,stockUnitName);
            Check(actual.text==stockUnitName,
                  "reviewed stock unit labels remain original outside verified display callers");
            Call(g_option,actual,L"30วัน");Check(actual.text==L"30 天","production dictionary retains numeric option grammar");return 0;
        }
        if(g_mode==7) {
            Check(g_buyOptions&&g_giftOptions,"actual DLL installed both initial options-dialog verification hooks");
            std::array<uint8_t,128> raw{};const uint32_t productId=1039001;memcpy(raw.data()+5,&productId,sizeof(productId));
            const wchar_t neutral[]=L"自定义商品";memcpy(raw.data()+9,neutral,sizeof(neutral));
            g_popupSource=reinterpret_cast<const wchar_t*>(raw.data()+9);g_popupLength=static_cast<uint32_t>(wcslen(g_popupSource));
            sdgo::shop::Option option{};option.productId=1039001;option.category=1219047;option.price=100;option.payment=1;option.validity=1;
            const auto optionBefore=option;Destination actual;g_popupDestination=&actual;
            std::array<uint8_t,0xC0> buy{};*reinterpret_cast<uintptr_t*>(buy.data()+0xB0)=reinterpret_cast<uintptr_t>(&option);*reinterpret_cast<uintptr_t*>(buy.data()+0xB4)=reinterpret_cast<uintptr_t>(&option+1);
            const HMODULE ownedProvider=LoadOriginProvider(originFixture);
            OriginControl(ownedProvider)(SDGO_CONTENT_LOCALIZATION_OWNED,
                                         SDGO_CONTENT_LOCALIZATION_OWNED);
            g_buyOptions(buy.data(),nullptr);Check(actual.text==L"美竹兰副官","authored product title retains priority when the content provider owns the raw title");
            UnloadOriginProvider(ownedProvider);
            Call(g_normal,actual,g_popupSource);Check(actual.text==L"自定义商品","initial BUY verification clears before unrelated later title copies");
            std::array<uint8_t,0xC0> gift{};*reinterpret_cast<uintptr_t*>(gift.data()+0xB4)=reinterpret_cast<uintptr_t>(&option);*reinterpret_cast<uintptr_t*>(gift.data()+0xB8)=reinterpret_cast<uintptr_t>(&option+1);
            g_giftOptions(gift.data(),nullptr);Check(actual.text==L"美竹兰副官","initial GIFT frame receives the authored title from its distinct native vector offset");
            Check(!memcmp(&option,&optionBefore,sizeof(option)),"matching initial popup hooks preserve every live option byte");
            ++option.price;const auto mismatchedBefore=option;g_buyOptions(buy.data(),nullptr);Check(actual.text==L"自定义商品","mismatched live price keeps the raw neutral title");
            Check(!memcmp(&option,&mismatchedBefore,sizeof(option)),"mismatched initial popup hook preserves every live option byte");
            Check(!wcscmp(g_popupSource,L"自定义商品"),"initial popup hooks preserve the raw neutral product record");
            Check(actual.id==1010465&&actual.price==9900&&actual.payment==2&&actual.validity==7,"initial popup title hooks leave destination business canaries unchanged");
            return 0;
        }
        Destination d;const wchar_t source[]=L"GUNDAM";Call(g_normal,d,source);Check(d.text==(g_mode==1?L"GUNDAM":L"高达"),"name translation and switch");Check(!wcscmp(source,L"GUNDAM"),"original source memory untouched");
        Call(g_preview,d,source);
        Check(d.text==L"GUNDAM",
              "direct shared assignment cannot bypass the getter-context gate");
        CallUnitName(g_previewOuter,d,10001,source);Check(d.text==(g_mode==1?L"GUNDAM":L"高达"),"independent shared preview caller obeys the existing name switch through TLS context");
        constexpr wchar_t stockUnitName[]=L"ZAKU PHANTOM (SLASH WIZARD) (YZAK CUSTOM)";
        Call(g_normal,d,stockUnitName);
        Check(d.text==(g_mode==1?stockUnitName:L"斩击型扎古幻影（伊扎克专用）"),
              "reviewed stock unit label obeys the existing name switch");
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
        if(g_mode==0) {
            constexpr wchar_t reviewedStock[]=L"ZAKU PHANTOM (SLASH WIZARD) (YZAK CUSTOM)";
            CallUnitNameAs(g_previewOuter,d,1900000101,L"原生中文主名称",
                           L"UNMAPPED SECONDARY NAME");
            Check(d.text==L"原生中文主名称",
                  "unmapped foreign secondary text falls back to the native clean-Chinese primary name");
            CallUnitNameAs(g_previewOuter,d,1900000102,L"另一个中文主名称",
                           L"已有中文显示");
            Check(d.text==L"已有中文显示",
                  "an already clean-Chinese assigned name remains unchanged");
            const HMODULE modern=LoadOriginProvider(originFixture);
            Call(g_normal,d,reviewedStock);
            Check(d.text==reviewedStock,
                  "an uninitialized modern content provider declines stock translation");
            const OriginSet setOrigin=OriginControl(modern);
            setOrigin(SDGO_CONTENT_LOCALIZATION_NOT_OWNED,
                      SDGO_CONTENT_LOCALIZATION_NOT_OWNED);
            Call(g_normal,d,reviewedStock);
            Check(d.text==L"斩击型扎古幻影（伊扎克专用）",
                  "a modern provider can explicitly leave reviewed stock text unowned");
            setOrigin(SDGO_CONTENT_LOCALIZATION_OWNED,
                      SDGO_CONTENT_LOCALIZATION_NOT_OWNED);
            Call(g_normal,d,reviewedStock);
            Check(d.text==reviewedStock,
                  "content ownership of the same source text blocks the public dictionary");
            setOrigin(SDGO_CONTENT_LOCALIZATION_NOT_OWNED,
                      SDGO_CONTENT_LOCALIZATION_OWNED);
            CallUnitName(g_previewOuter,d,1900000001,reviewedStock);
            Check(d.text==reviewedStock,
                  "content ownership of the same unit identity blocks preview translation");
            Check(service(1,10001,reviewedStock,
                          static_cast<uint32_t>(wcslen(reviewedStock)),
                          shared,128)==0,
                  "content ownership of the same unit identity blocks the stable lookup service");
            UnloadOriginProvider(modern);

            const HMODULE legacy=LoadOriginProvider(legacyOriginFixture);
            Call(g_normal,d,reviewedStock);
            Check(d.text==reviewedStock,
                  "a loaded provider without the origin contract declines translation");
            UnloadOriginProvider(legacy);
        }
        printf("SHOP LOCALIZATION ACTUAL DLL PASS mode=%d native copies=%u\n",g_mode,g_nativeCalls);return 0;
    } catch(const std::exception& e) {fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
