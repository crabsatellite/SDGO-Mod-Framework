#include "sdgo/native_skill_identity.h"
#include "sdgo/script_registration.h"
#include <windows.h>
#include <cstring>

namespace sdgo::skill_identity {
namespace {
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
std::shared_ptr<const Bindings> g_bindings;
const Lookup* g_lookup=nullptr;
void* g_enableOriginal=nullptr;void* g_disableOriginal=nullptr;
void* g_enableContinue=nullptr;void* g_disableContinue=nullptr;
bool g_attempted=false;
volatile LONG g_rejected=0;
volatile LONG g_enableAliases=0,g_disableAliases=0;
struct Registers {std::uint32_t edi,esi,ebp,savedEsp,ebx,edx,ecx,eax,flags;};
alignas(16) const std::uint32_t kDefaultMxcsr=0x1f80;

bool Read(const void* source,void* target,std::size_t size){
    __try{std::memcpy(target,source,size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Context(Registers* r,const void** definition,std::uint32_t* id){
    __try{
        *definition=reinterpret_cast<const void*>(r->eax);
        if(!*definition||*reinterpret_cast<const void**>(r->ebp+8)!=*definition||
           !*reinterpret_cast<void**>(r->ebp+12))return false;
        std::memcpy(id,*definition,4);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Selector(Registers* r,std::uint32_t value){
    __try{
        // Emulate only the overwritten load/store using the virtual selector.
        // The borrowed definition and its real ID remain untouched.
        r->ecx=value;*reinterpret_cast<std::uint32_t*>(r->ebp-0x1c)=value;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
int __cdecl Resolve(Registers* r,int enabling){
    const DWORD incoming=GetLastError();const void* definition=nullptr;std::uint32_t id=0;
    int action=0;
    if(Context(r,&definition,&id)){
        try{
            const auto result=g_bindings->resolve(definition,id,*g_lookup);
            if(result.resolution==Resolution::Aliased){
                action=Selector(r,result.selector)?1:2;
                if(action==1)InterlockedIncrement(enabling?&g_enableAliases:&g_disableAliases);
            }
            else if(result.resolution==Resolution::Rejected){
                action=2;
                if(InterlockedIncrement(&g_rejected)<=4)g_api->Log(g_mod,SDGO_LOG_ERROR,"imported skill identity rejected: %s",result.reason);
            }
        }catch(...){action=2;}
    }
    SetLastError(incoming);return action;
}

#define IDENTITY_THUNK(name,original,continuation,enabling) \
__declspec(naked) void name(){ \
    __asm{pushfd} \
    __asm{pushad} \
    __asm{mov eax,esp} \
    __asm{sub esp,210h} \
    __asm{and esp,0fffffff0h} \
    __asm{mov [esp+200h],eax} \
    __asm{fxsave [esp]} \
    __asm{fninit} \
    __asm{ldmxcsr dword ptr [kDefaultMxcsr]} \
    __asm{cld} \
    __asm{push enabling} \
    __asm{push eax} \
    __asm{call Resolve} \
    __asm{add esp,8} \
    __asm{cmp eax,1} \
    __asm{je aliased} \
    __asm{cmp eax,2} \
    __asm{je rejected} \
    __asm{fxrstor [esp]} \
    __asm{mov esp,[esp+200h]} \
    __asm{popad} \
    __asm{popfd} \
    __asm{jmp dword ptr [original]} \
aliased: \
    __asm{fxrstor [esp]} \
    __asm{mov esp,[esp+200h]} \
    __asm{popad} \
    __asm{popfd} \
    __asm{jmp dword ptr [continuation]} \
rejected: \
    __asm{fxrstor [esp]} \
    __asm{mov esp,[esp+200h]} \
    __asm{popad} \
    __asm{popfd} \
    __asm{xor eax,eax} \
    __asm{mov esp,ebp} \
    __asm{pop ebp} \
    __asm{ret 8} \
}
IDENTITY_THUNK(Enable,g_enableOriginal,g_enableContinue,1)
IDENTITY_THUNK(Disable,g_disableOriginal,g_disableContinue,0)
#undef IDENTITY_THUNK

bool Region(const std::uint8_t* base,std::uint32_t rva,std::size_t size,const std::uint32_t* relocations,
            std::size_t count,const char* expected){
    std::vector<std::uint8_t> bytes(size);
    if(!Read(base+rva,bytes.data(),size))return false;
    const auto delta=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(base)-0x400000u);
    for(std::size_t i=0;i<count;++i){
        std::uint32_t value=0;std::memcpy(&value,bytes.data()+relocations[i],4);value-=delta;
        std::memcpy(bytes.data()+relocations[i],&value,4);
    }
    return sdgo::scripts::Sha256(bytes)==expected;
}
}

bool InstallNative(const SdgoApi* api,SdgoMod* mod,std::shared_ptr<const Bindings> bindings,const Lookup* lookup,std::string& error){
    error.clear();
    if(g_attempted){error="skill identity is process-lifetime and cannot be installed twice";return false;}
    if(!api||api->version!=SDGO_API_VERSION||api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite)||!api->HookSite||!api->Symbol||!api->Log||
       !api->host_key||std::strcmp(api->host_key,"BattleServer-548A891D-0046C000")||!api->host_base||!bindings||!lookup){
        error="verified skill identity host/API/bindings unavailable";return false;
    }
    auto* base=static_cast<const std::uint8_t*>(api->host_base);
    const std::uint32_t enableRelocs[]={0x83,0x8a,0x23c,0x240,0x244,0x248,0x24c,0x250,0x254,0x258};
    const std::uint32_t disableRelocs[]={0x83,0x8a,0x238,0x23c,0x240,0x244,0x248,0x24c,0x250,0x254};
    if(!Region(base,0x89400,828,enableRelocs,10,"34ede550895c195d07c08c4052b1c4e9c014cc13f88e75ece675dbde2bbd6b89")||
       !Region(base,0x89740,824,disableRelocs,10,"a5b9883011ed49fa2bd1f52e258567969baaa59c31ad81fb22f05822f9f920e3")){
        error="original skill dispatchers/selector tables differ";return false;
    }
    const auto enable=api->Symbol(mod,"skill_identity.enable_selector");
    const auto disable=api->Symbol(mod,"skill_identity.disable_selector");
    auto* enableNext=api->Symbol(mod,"skill_identity.enable_continue");
    auto* disableNext=api->Symbol(mod,"skill_identity.disable_continue");
    if(enable!=base+0x89449||disable!=base+0x89789||enableNext!=base+0x8944e||disableNext!=base+0x8978e){
        error="verified skill selector boundaries differ";return false;
    }
    g_attempted=true;g_api=api;g_mod=mod;g_bindings=std::move(bindings);g_lookup=lookup;
    g_enableContinue=enableNext;g_disableContinue=disableNext;
    if(!api->HookSite(mod,"skill_identity.enable_selector",reinterpret_cast<void*>(&Enable),&g_enableOriginal)||
       !api->HookSite(mod,"skill_identity.disable_selector",reinterpret_cast<void*>(&Disable),&g_disableOriginal)){
        error="both skill identity dispatcher hooks must install together";return false;
    }
    return true;
}
NativeStatus ReadNativeStatus(){
    return {static_cast<std::uint32_t>(InterlockedCompareExchange(&g_enableAliases,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_disableAliases,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_rejected,0,0))};
}
}
