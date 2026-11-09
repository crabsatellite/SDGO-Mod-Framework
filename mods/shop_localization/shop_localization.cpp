#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/content_localization_boundary.h"
#include "dictionary.h"
#include "sites.h"
#include "custom_products.h"
#include "shop_search.h"
#include "unit_name_display.h"

namespace {
using namespace sdgo::shop_localization;
using AssignFn = void*(__thiscall*)(void*, const wchar_t*, uint32_t);
using CopyFn = void*(__thiscall*)(void*, const void*, uint32_t, uint32_t);
AssignFn g_assign = nullptr;
CopyFn g_copy = nullptr;
Dictionary g_words;
std::vector<uintptr_t> g_returns, g_optionReturns, g_productReturns;
bool g_names = true, g_descriptions = true, g_options = true;
bool g_interfaces = true, g_ready = false, g_searchChinese = true;
thread_local std::wstring g_optionText;
CustomProducts g_custom;
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
using LoadFn=uint8_t(__thiscall*)(void*,const wchar_t*,int);
using RenderFn=void(__thiscall*)(void*);
LoadFn g_load=nullptr;RenderFn g_render=nullptr;
using InitialPopupFn=void(__thiscall*)(void*);
using PopupFn=void(__thiscall*)(void*,uint32_t);
using HoverFn=void(__stdcall*)(void*,void*);
InitialPopupFn g_buyOptions=nullptr,g_giftOptions=nullptr;
PopupFn g_buyPopup=nullptr,g_giftPopup=nullptr;
HoverFn g_hover=nullptr,g_hoverAlt=nullptr;
bool g_registrationWarning=false;

void RegisterCustom()
{
    try {if(!g_custom.Install()&&!g_registrationWarning){g_registrationWarning=true;g_api->Log(g_mod,SDGO_LOG_WARN,"custom product display registration deferred or refused (native exception %08lX); no foreign rows overwritten",g_custom.ExceptionCode());}}
    catch(const std::exception& e){g_api->Log(g_mod,SDGO_LOG_ERROR,"custom product display registration failed: %s",e.what());}
}
uint8_t __fastcall LoadStore(void* self,void*,const wchar_t* filename,int type)
{
    const auto result=g_load(self,filename,type);if(result&&type==0x23)RegisterCustom();return result;
}
void ObserveSafely(void* self,size_t offset)
{
    try {g_custom.Observe(self,offset);}
    catch(const std::exception&){CustomProducts::Clear();}
}
void ObservePopupSafely(void* self)
{
    try {g_custom.ObservePopup(self);}
    catch(const std::exception&){CustomProducts::Clear();}
}
void __fastcall RenderList(void* self,void*)
{
    RegisterCustom();ObserveSafely(self,0x88);__try {g_render(self);}__finally {CustomProducts::Clear();}
}
void InitialPopup(void* self,size_t offset,InitialPopupFn original)
{
    ObserveSafely(self,offset);__try {original(self);}__finally {CustomProducts::Clear();}
}
void __fastcall BuyOptions(void* self,void*){InitialPopup(self,0xB0,g_buyOptions);}
void __fastcall GiftOptions(void* self,void*){InitialPopup(self,0xB4,g_giftOptions);}
void Popup(void* self,uint32_t argument,PopupFn original)
{
    ObservePopupSafely(self);__try {original(self,argument);}__finally {CustomProducts::Clear();}
}
void __fastcall BuyPopup(void* self,void*,uint32_t argument){Popup(self,argument,g_buyPopup);}
void __fastcall GiftPopup(void* self,void*,uint32_t argument){Popup(self,argument,g_giftPopup);}
void Hover(void* self,void* options,HoverFn original)
{
    ObserveSafely(options,0);__try {original(self,options);}__finally {CustomProducts::Clear();}
}
void __stdcall HoverTooltip(void* self,void* options){Hover(self,options,g_hover);}
void __stdcall HoverTooltipAlt(void* self,void* options){Hover(self,options,g_hoverAlt);}

bool CopyText(const wchar_t* src, uint32_t size, wchar_t* target)
{
    __try { memcpy(target, src, size * sizeof(wchar_t)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NativeView(const void* value, uint32_t pos, uint32_t count, const wchar_t** text, uint32_t* size)
{
    __try {
        const auto* words = static_cast<const uint32_t*>(value);
        if (words[4] > words[5] || words[4] > kMaxText || pos > words[4]) return false;
        *size = (std::min)(count, words[4] - pos);
        const auto* data = words[5] < 8 ? static_cast<const wchar_t*>(value) : *static_cast<const wchar_t* const*>(value);
        *text = data + pos; return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const std::wstring* Lookup(const wchar_t* source, uint32_t length, uintptr_t caller)
{
    if (!source || length > kMaxText) return nullptr;
    if(!length) {
        if(g_options&&std::binary_search(g_optionReturns.begin(),g_optionReturns.end(),caller)) {
            g_optionText=L"未提供选项文字";return &g_optionText;
        }
        return nullptr;
    }
    std::wstring text(length, L'\0');
    if (!CopyText(source, length, text.data()) || text.find(L'\0') != std::wstring::npos) return nullptr;
    const bool productCaller=std::binary_search(g_productReturns.begin(),g_productReturns.end(),caller);
    uint32_t product=0;
    const bool hasProduct=productCaller && ProductIdBeforeName(source,&product);
    // Authored content supplies its own display text before the stock dictionary.
    if(hasProduct) {
        if(const auto* custom=g_custom.Title(product))return custom;
    }
    if(sdgo::content_localization_boundary::Query(0,0,text.data(),length)!=
       SDGO_CONTENT_LOCALIZATION_NOT_OWNED)return nullptr;
    if(g_names) {
        uint32_t category=0;
        if(CategoryIdBeforeName(source,&category)) {
            const auto exact=g_words.find(L"@category:"+std::to_wstring(category)+L":"+text);
            if(exact!=g_words.end())return &exact->second.text;
        }
    }
    if(productCaller) {
        if(hasProduct) {
            if(g_names) {
                const auto localized=g_words.find(L"@product:"+std::to_wstring(product)+L":"+text);
                if(localized!=g_words.end())return &localized->second.text;
            }
        }
    }
    const auto found = g_words.find(text);
    if (found != g_words.end()) {
        const auto& value = found->second;
        if ((value.kind == Kind::Name && g_names) || (value.kind == Kind::Description && g_descriptions) ||
            (value.kind == Kind::Option && g_options)) return &value.text;
    }
    // A numeric grammar is admitted only at the three native sales-option name
    // copies. Price copies and every other UI string retain their native value.
    if (g_options && std::binary_search(g_optionReturns.begin(), g_optionReturns.end(), caller)) {
        g_optionText = OptionLabel(text);
        if (!g_optionText.empty()) return &g_optionText;
    }
    return nullptr;
}
const std::wstring* StableText(uint32_t kind, uint32_t id, const std::wstring& source);

bool CleanChinese(const wchar_t* text,uint32_t length)
{
    bool chinese=false;
    for(uint32_t i=0;i<length;++i) {
        const auto ch=static_cast<uint16_t>(text[i]);
        if((ch>=0x0E00&&ch<=0x0E7F)||(ch>=0x1100&&ch<=0x11FF)||
           (ch>=0x3040&&ch<=0x30FF)||(ch>=0x3130&&ch<=0x318F)||
           (ch>=0xAC00&&ch<=0xD7AF)||(ch>=0xFF65&&ch<=0xFFDC))return false;
        chinese=chinese||(ch>=0x3400&&ch<=0x9FFF);
    }
    return chinese;
}

void* __fastcall Assign(void* self, void*, const wchar_t* source, uint32_t length)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    sdgo::unit_name_display::ContextView context{};
    if(sdgo::unit_name_display::CurrentUnitNameContext(
           reinterpret_cast<const void*>(caller),&context)) {
        if(g_names&&source&&length&&length<=kMaxText) {
            std::wstring original(length,L'\0');
            if(CopyText(source,length,original.data())&&
               original.find(L'\0')==std::wstring::npos&&
               sdgo::content_localization_boundary::Query(2,context.unit_id,original.data(),length)==
                   SDGO_CONTENT_LOCALIZATION_NOT_OWNED&&
               !CleanChinese(original.data(),length)) {
                if(const auto* translated=StableText(1,context.unit_id,original))
                    return g_assign(self,translated->data(),static_cast<uint32_t>(translated->size()));
                if(CleanChinese(context.primary_name,context.primary_chars))
                    return g_assign(self,context.primary_name,context.primary_chars);
            }
        }
        return g_assign(self,source,length);
    }
    if (std::binary_search(g_returns.begin(), g_returns.end(), caller)) {
        if (const auto* text = Lookup(source, length, caller)) return g_assign(self, text->data(), static_cast<uint32_t>(text->size()));
    }
    return g_assign(self, source, length);
}
void* __fastcall Copy(void* self, void*, const void* source, uint32_t pos, uint32_t count)
{
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (std::binary_search(g_returns.begin(), g_returns.end(), caller)) {
        const wchar_t* data = nullptr; uint32_t size = 0;
        if (NativeView(source, pos, count, &data, &size)) {
            if (const auto* text = Lookup(data, size, caller)) return g_assign(self, text->data(), static_cast<uint32_t>(text->size()));
        }
    }
    return g_copy(self, source, pos, count);
}
Dictionary Load(const wchar_t* directory)
{
    const auto file = std::wstring(directory) + L"\\zh-CN.ini";
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("shop localization dictionary missing");
    stream.seekg(0, std::ios::end); const auto bytes = stream.tellg();
    if (bytes <= 0 || bytes > 8 * 1024 * 1024) throw std::runtime_error("dictionary size outside supported range");
    stream.seekg(0); std::string raw(static_cast<size_t>(bytes), '\0');
    if (!stream.read(raw.data(), static_cast<std::streamsize>(raw.size()))) throw std::runtime_error("dictionary read failed");
    if (raw.rfind("\xEF\xBB\xBF", 0) == 0) raw.erase(0, 3);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (!length) throw std::runtime_error("dictionary must be valid UTF-8");
    std::wstring text(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), text.data(), length);
    return Parse(text);
}

const std::wstring* StableText(uint32_t kind, uint32_t id, const std::wstring& source)
{
    if ((kind == 1 || kind == 2) && !g_names) return nullptr;
    if (kind == 3 && !g_interfaces) return nullptr;
    if(kind==1 || kind==2) {
        const uint32_t recordKind=kind==1?2u:0x22u;
        if(sdgo::content_localization_boundary::Query(
               id?recordKind:0u,id,source.data(),static_cast<uint32_t>(source.size()))!=
           SDGO_CONTENT_LOCALIZATION_NOT_OWNED)return nullptr;
    }
    auto get = [&](const std::wstring& key, Kind type) -> const std::wstring* {
        const auto found = g_words.find(key);
        return found != g_words.end() && found->second.kind == type ? &found->second.text : nullptr;
    };
    if (kind == 3) return get(L"@ui:" + source, Kind::Option);
    if (id) {
        if (const auto* text = get((kind == 1 ? L"@unit:" : L"@item:") + std::to_wstring(id), Kind::Name))
            return text;
        if (kind == 2) {
            if (const auto* text = get(L"@category:" + std::to_wstring(id) + L":" + source, Kind::Name))
                return text;
        }
    }
    return get(source, Kind::Name);
}

bool WriteTranslation(wchar_t* output, const std::wstring& value)
{
    __try { memcpy(output, value.c_str(), (value.size() + 1) * sizeof(wchar_t)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

// Optional C ABI for other Mods. Required capacity includes NUL; a query or
// undersized buffer is never written. No C++ object crosses the DLL boundary.
extern "C" __declspec(dllexport) uint32_t __cdecl SdgoLocalizationLookupV1(
    uint32_t kind, uint32_t stableId, const wchar_t* source, uint32_t sourceChars,
    wchar_t* output, uint32_t outputChars)
{
    if (!g_ready || kind < 1 || kind > 3 || sourceChars > kMaxText || (!source && sourceChars)) return 0;
    try {
        std::wstring original(sourceChars, L'\0');
        if (sourceChars && (!CopyText(source, sourceChars, original.data()) || original.find(L'\0') != std::wstring::npos)) return 0;
        const auto* value = StableText(kind, stableId, original);
        if (!value) return 0;
        const auto required = static_cast<uint32_t>(value->size() + 1);
        if (output && outputChars >= required && !WriteTranslation(output, *value)) return 0;
        return required;
    } catch (const std::exception&) { return 0; }
}

SDGO_MOD_ENTRY
{
    g_ready = false;
    if (!api || api->version != 1 || !api->Symbol || !api->HookCode || !api->ModDirectory || !api->SettingInt || !api->Log) return 0;
    try {
        g_words = Load(api->ModDirectory(mod));
        g_api=api;g_mod=mod;g_custom.Read(api->ModDirectory(mod));
        if(!g_custom.Empty()&&!g_custom.Bind(api,mod))return 0;
        g_names = api->SettingInt(mod, "TranslateNames", 1) != 0;
        g_descriptions = api->SettingInt(mod, "TranslateDescriptions", 1) != 0;
        g_options = api->SettingInt(mod, "TranslateOptions", 1) != 0;
        g_interfaces = api->SettingInt(mod, "TranslateInterface", 1) != 0;
        g_searchChinese = api->SettingInt(mod, "SearchChinese", 1) != 0;
        g_returns.clear(); g_optionReturns.clear();g_productReturns.clear();
        for (const auto& site : kSites) {
            const auto address = reinterpret_cast<uintptr_t>(api->Symbol(mod, site.name));
            if (!address) { api->Log(mod, SDGO_LOG_ERROR, "localization caller is not verified: %s", site.name); return 0; }
            g_returns.push_back(address + 5);
            if (site.option) g_optionReturns.push_back(address + 5);
            if(site.product)g_productReturns.push_back(address+5);
        }
        std::sort(g_returns.begin(), g_returns.end()); std::sort(g_optionReturns.begin(), g_optionReturns.end());std::sort(g_productReturns.begin(),g_productReturns.end());
        if (!api->HookCode(mod, "shop_localization.assign", reinterpret_cast<void*>(&Assign), reinterpret_cast<void**>(&g_assign)) ||
            !api->HookCode(mod, "shop_localization.copy", reinterpret_cast<void*>(&Copy), reinterpret_cast<void**>(&g_copy))) return 0;
        if (!sdgo::unit_name_display::Attach(api,mod)) return 0;
        if (!BindSearchHooks(api, mod, &g_searchChinese, &g_interfaces)) return 0;
        if(!g_custom.Empty()&&(!api->HookCode(mod,"shop_localization.load_store",reinterpret_cast<void*>(&LoadStore),reinterpret_cast<void**>(&g_load))||
           !api->HookCode(mod,"shop_localization.render_list",reinterpret_cast<void*>(&RenderList),reinterpret_cast<void**>(&g_render))||
           !api->HookCode(mod,"shop_localization.buy_options",reinterpret_cast<void*>(&BuyOptions),reinterpret_cast<void**>(&g_buyOptions))||
           !api->HookCode(mod,"shop_localization.gift_options",reinterpret_cast<void*>(&GiftOptions),reinterpret_cast<void**>(&g_giftOptions))||
           !api->HookCode(mod,"shop_localization.buy_popup",reinterpret_cast<void*>(&BuyPopup),reinterpret_cast<void**>(&g_buyPopup))||
           !api->HookCode(mod,"shop_localization.gift_popup",reinterpret_cast<void*>(&GiftPopup),reinterpret_cast<void**>(&g_giftPopup))||
           !api->HookCode(mod,"shop_localization.hover",reinterpret_cast<void*>(&HoverTooltip),reinterpret_cast<void**>(&g_hover))||
           !api->HookCode(mod,"shop_localization.hover_alt",reinterpret_cast<void*>(&HoverTooltipAlt),reinterpret_cast<void**>(&g_hoverAlt))))return 0;
        api->Log(mod, SDGO_LOG_INFO, "Chinese shop localization ready: %u dictionary entries, %u verified shop callers; original records and billing untouched",
                 static_cast<unsigned>(g_words.size()), static_cast<unsigned>(g_returns.size()));
        g_ready = true;
        return 1;
    } catch (const std::exception& error) {
        api->Log(mod, SDGO_LOG_ERROR, "shop localization refused: %s", error.what()); return 0;
    }
}
