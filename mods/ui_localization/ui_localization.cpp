#include "dictionary.h"
#include "system_text.h"
#include "acquisition_text.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/content_localization_boundary.h"
#include "sdgo/ui_localization_observer.h"
#include <windows.h>
#include <intrin.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
struct NativeString {
    union { wchar_t small[8]; const wchar_t* pointer; } data{};
    std::uint32_t length=0,capacity=7;
};
static_assert(sizeof(NativeString)==24,"native UTF-16 string ABI");
using ControlItem=void(__thiscall*)(void*,int,const NativeString*,int,int,int,int,
                                  std::uint32_t,std::uint32_t,std::uint8_t,std::uint32_t,int);
ControlItem originalItem=nullptr;
const SdgoApi* ownerApi=nullptr;
SdgoMod* ownerMod=nullptr;
void* itemCaller=nullptr;
std::uintptr_t hostBase=0;
bool traceKnown=false;
volatile LONG installed=0,traceCount=0,lastPhrase=0,lastCaller=0;
alignas(8) volatile LONG64 captions=0,items=0,passthrough=0,invalid=0;
volatile LONG seenPhrases[32]{};
static_assert(sdgo::ui_localization::kPhraseCount<=1024,"native phrase observation capacity");

bool CopySource(const NativeString* source,wchar_t* output,std::size_t* size) noexcept {
    __try {
        if(!source || source->length>sdgo::ui_localization::kMaximumText ||
           source->length>source->capacity || (source->capacity<8 && source->length>7))return false;
        const auto* text=source->capacity<8?source->data.small:source->data.pointer;
        if(!text)return false;
        std::memcpy(output,text,source->length*sizeof(wchar_t));
        output[source->length]=0;*size=source->length;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

NativeString View(const wchar_t* text) {
    NativeString result;result.length=static_cast<std::uint32_t>(std::wcslen(text));
    if(result.length<8)std::memcpy(result.data.small,text,(result.length+1)*sizeof(wchar_t));
    else{result.data.pointer=text;result.capacity=result.length;}
    return result;
}

const sdgo::ui_localization::Phrase* Resolve(const NativeString* source,void* caller,bool eligible) {
    if(!eligible && !traceKnown){InterlockedIncrement64(&passthrough);return nullptr;}
    wchar_t text[sdgo::ui_localization::kMaximumText+1]{};std::size_t size=0;
    if(!CopySource(source,text,&size)){InterlockedIncrement64(&invalid);return nullptr;}
    const auto* found=sdgo::ui_localization::Find(std::wstring_view(text,size));
    if(!found)return nullptr;
    if(sdgo::content_localization_boundary::Query(0,0,text,static_cast<std::uint32_t>(size))!=
       SDGO_CONTENT_LOCALIZATION_NOT_OWNED) {
        InterlockedIncrement64(&passthrough);return nullptr;
    }
    const auto address=reinterpret_cast<std::uintptr_t>(caller);
    const auto phrase=static_cast<unsigned>(found-sdgo::ui_localization::kPhrases)+1;
    const auto relative=address>=hostBase && address-hostBase<0x00C97000 ?
        static_cast<unsigned>(address-hostBase):0u;
    InterlockedExchange(&lastPhrase,static_cast<LONG>(phrase));
    InterlockedExchange(&lastCaller,static_cast<LONG>(relative));
    if(traceKnown && relative && InterlockedIncrement(&traceCount)<=128)
        ownerApi->Log(ownerMod,SDGO_LOG_INFO,"known UI phrase=%u caller_rva=%08X static_scope=%u",
                      phrase,relative,eligible?1u:0u);
    if(!eligible){InterlockedIncrement64(&passthrough);return nullptr;}
    return found;
}

void __fastcall SetItem(void* control,void*,int index,const NativeString* text,int x,int y,
                        int width,int height,std::uint32_t color,std::uint32_t shadow,
                        std::uint8_t unknown,std::uint32_t font,int alignment) {
    const DWORD incoming=GetLastError();
    // Only the verified native layout deserializer may supply static label text.
    void* caller=_ReturnAddress();
    const auto* word=Resolve(text,caller,caller==itemCaller);
    NativeString translated;
    if(word){
        translated=View(word->target);InterlockedIncrement64(&items);
        const auto slot=static_cast<unsigned>(word-sdgo::ui_localization::kPhrases);
        const LONG bit=static_cast<LONG>(1u<<(slot%32));
        if(!(InterlockedOr(&seenPhrases[slot/32],bit)&bit))InterlockedIncrement64(&captions);
    }
    SetLastError(incoming);
    originalItem(control,index,word?&translated:text,x,y,width,height,color,shadow,unknown,font,alignment);
}

std::uint64_t ReadCounter(volatile LONG64* value) {
    return static_cast<std::uint64_t>(InterlockedCompareExchange64(value,0,0));
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoUiLocalizationGetSnapshotV1(SdgoUiLocalizationSnapshotV1* output) {
    if(!output || output->size<sizeof(*output) || output->version!=1)return 0;
    const auto system=sdgo::ui_localization::system_text::Snapshot();
    SdgoUiLocalizationSnapshotV1 result{};result.size=sizeof(result);result.version=1;
    result.installed=static_cast<std::uint32_t>(InterlockedCompareExchange(&installed,0,0));
    result.phrase_count=static_cast<std::uint32_t>(sdgo::ui_localization::kPhraseCount);
    result.distinct_phrases=ReadCounter(&captions);result.control_items=ReadCounter(&items);
    result.system_lookups=system.lookups;result.system_replaced=system.replaced;
    result.guarded_passthrough=ReadCounter(&passthrough);result.invalid_inputs=ReadCounter(&invalid)+system.invalid;
    result.last_phrase=static_cast<std::uint32_t>(InterlockedCompareExchange(&lastPhrase,0,0));
    result.last_caller_rva=static_cast<std::uint32_t>(InterlockedCompareExchange(&lastCaller,0,0));
    result.trace_enabled=traceKnown?1u:0u;
    result.system_phrase_count=static_cast<std::uint32_t>(sdgo::ui_localization::kSystemPhraseCount);
    *output=result;return 1;
}

SDGO_MOD_ENTRY {
    InterlockedExchange(&installed,0);
    sdgo::ui_localization::acquisition_text::Deactivate();
    if(!api || api->version!=SDGO_API_VERSION || !api->Symbol || !api->HookCode || !api->ModDirectory)return 0;
    ownerApi=api;ownerMod=mod;hostBase=reinterpret_cast<std::uintptr_t>(api->host_base);
    const std::wstring ini=std::wstring(api->ModDirectory(mod))+L"\\mod.ini";
    traceKnown=GetPrivateProfileIntW(L"settings",L"TraceKnownSources",0,ini.c_str())!=0;
    itemCaller=api->Symbol(mod,"ui_localization.control_item_layout_caller");
    if(!itemCaller || !sdgo::ui_localization::system_text::Install(api,mod) ||
       !sdgo::ui_localization::acquisition_text::Install(api,mod) ||
       !api->HookCode(mod,"ui_localization.control_item",reinterpret_cast<void*>(&SetItem),reinterpret_cast<void**>(&originalItem)))return 0;
    InterlockedExchange(&installed,1);
    sdgo::ui_localization::acquisition_text::Activate();
    api->Log(mod,SDGO_LOG_INFO,"native UI localization: %u layout phrases, %u system text rows",
             static_cast<unsigned>(sdgo::ui_localization::kPhraseCount),
             static_cast<unsigned>(sdgo::ui_localization::kSystemPhraseCount));
    return 1;
}
