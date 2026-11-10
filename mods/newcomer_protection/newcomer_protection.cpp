#include <windows.h>
#include <array>
#include <cstddef>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "sdgo/newcomer_protection_observer.h"
#include "policy.h"

#if !defined(_M_IX86)
#error newcomer protection requires the verified x86 Battle host
#endif
namespace {
using namespace sdgo::newcomer;
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
Policy g_policy;
void* g_ready=nullptr;
void* g_hitReady=nullptr;
void* g_partReady=nullptr;
std::uintptr_t g_partCaller=0;
const DWORD g_defaultMxcsr=0x1f80;
SRWLOCK g_receiptLock=SRWLOCK_INIT;
constexpr unsigned kCapacity=64;
SdgoNewcomerProtectionReceiptV1 g_receipts[kCapacity]{};
std::uint32_t g_sequence=0;
SdgoNewcomerProtectionHitV1 g_hits[kCapacity]{};
std::uint32_t g_hitSequence=0;
volatile LONG g_hitCalls=0,g_humanCalls=0,g_rosterCalls=0,g_ordinaryRosterCalls=0,g_membershipRejected=0,g_lastAccount=0,g_lastType=0;

template<class T> T Load(const void* base,std::ptrdiff_t offset) {
    T value{};std::memcpy(&value,static_cast<const std::uint8_t*>(base)+offset,sizeof(value));return value;
}
struct Build {
    std::uint8_t* units[4]{};
    Unit before[4]{},after[4]{};
    unsigned decks[4]{},count=0;
    std::uint32_t account=0,mode=0;
};
bool Capture(const std::uint8_t* frame,Build* build) noexcept {
    __try {
        auto* room=Load<std::uint8_t*>(frame,8);
        auto* source=Load<std::uint8_t*>(frame,12);
        auto* player=Load<std::uint8_t*>(frame,16);
        const auto mode=Load<std::uint8_t>(frame,24);
        if(!room||!source||!player||room[5]!=mode)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(room)+0x3ce4;
        const auto current=reinterpret_cast<std::uintptr_t>(player);
        if(current<base || (current-base)%0x1f54 || (current-base)/0x1f54>=12)return false;
        const auto slot=(current-base)/0x1f54;
        const auto account=Load<std::uint32_t>(source,0x429);
        if(!g_policy.protects(account) || Load<std::uint32_t>(player,0)!=account ||
           Load<std::uint32_t>(room,0x533+slot*0x48e+0x429)!=account)return false;
        build->account=account;build->mode=mode;
        const bool deck=mode==6||mode==7;
        const unsigned selected=player[0x33c];
        if(selected>=4)return false;
        for(unsigned i=0;i<(deck?4u:1u);++i){
            const auto index=deck?i:selected;
            const auto id=Load<std::uint32_t>(source,deck?0x19e+i*0x99:0x49);
            if(!id){if(!deck)return false;continue;}
            auto* unit=player+0x340+index*kUnitBytes;
            if(Load<std::uint32_t>(unit,0)!=id)return false;
            const auto at=build->count++;
            build->units[at]=unit;build->decks[at]=index;
            std::memcpy(build->before[at].data(),unit,kUnitBytes);
        }
        return build->count!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void WriteFields(std::uint8_t* target,const Unit& values) noexcept {
    std::memcpy(target+4,values.data()+4,4);
    std::memcpy(target+0x34,values.data()+0x34,4);
}
bool Commit(Build* build) noexcept {
    __try {
        for(unsigned i=0;i<build->count;++i)WriteFields(build->units[i],build->after[i]);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        __try {
            for(unsigned i=0;i<build->count;++i)WriteFields(build->units[i],build->before[i]);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }
}
void __stdcall Apply(const std::uint8_t* frame) noexcept {
    const auto error=GetLastError();
    Build build;
    if(Capture(frame,&build)){
        bool valid=true;
        for(unsigned i=0;i<build.count;++i)valid=Plan(build.before[i],g_policy,build.after[i])&&valid;
        if(valid&&Commit(&build))for(unsigned i=0;i<build.count;++i){
            SdgoNewcomerProtectionReceiptV1 receipt{};
            receipt.size=sizeof(receipt);receipt.version=1;
            receipt.account=build.account;receipt.mode=build.mode;receipt.deck=build.decks[i];
            receipt.unit=Read<std::uint32_t>(build.before[i],0);
            receipt.maximum_before=Read<std::uint32_t>(build.before[i],4);
            receipt.maximum_after=Read<std::uint32_t>(build.after[i],4);
            receipt.current_before=Read<std::uint32_t>(build.before[i],0x34);
            receipt.current_after=Read<std::uint32_t>(build.after[i],0x34);
            for(unsigned form=0;form<kForms;++form){
                receipt.attack_before[form]=Read<std::uint16_t>(build.before[i],0xe+form*kFormStride);
                receipt.attack_after[form]=Read<std::uint16_t>(build.after[i],0xe+form*kFormStride);
                receipt.special_before[form]=Read<std::uint32_t>(build.before[i],0x12+form*kFormStride);
                receipt.special_after[form]=Read<std::uint32_t>(build.after[i],0x12+form*kFormStride);
            }
            AcquireSRWLockExclusive(&g_receiptLock);
            receipt.sequence=++g_sequence;g_receipts[(g_sequence-1)%kCapacity]=receipt;
            ReleaseSRWLockExclusive(&g_receiptLock);
            g_api->Log(g_mod,SDGO_LOG_INFO,"protected native player=%lu unit=%lu mode=%lu deck=%lu HP=%lu/%lu -> %lu/%lu; ordinary hits scaled at server entry; special unchanged",
                static_cast<unsigned long>(receipt.account),static_cast<unsigned long>(receipt.unit),
                static_cast<unsigned long>(receipt.mode),static_cast<unsigned long>(receipt.deck),
                static_cast<unsigned long>(receipt.current_before),static_cast<unsigned long>(receipt.maximum_before),
                static_cast<unsigned long>(receipt.current_after),static_cast<unsigned long>(receipt.maximum_after));
        }
    }
    SetLastError(error);
}
void ApplyHit(std::uint8_t* frame,bool part) noexcept {
    const auto error=GetLastError();
    InterlockedIncrement(&g_hitCalls);
    __try {
        auto* room=Load<std::uint8_t*>(frame,part?-0x14:-0x480);
        auto* attacker=Load<std::uint8_t*>(frame,part?0xc:0x10);
        const auto slot=Load<std::uint32_t>(frame,part?8:0xc);
        if(!room||!attacker||slot>=12){InterlockedIncrement(&g_membershipRejected);goto done;}
        {
            const auto account=Load<std::uint32_t>(attacker,0);
            std::uint32_t attackType=0;
            if(part){
                const auto* parent=Load<std::uint8_t*>(frame,0);
                if(Load<std::uintptr_t>(frame,4)!=g_partCaller||!parent||
                   Load<std::uint8_t*>(parent,-0x15c)!=room||Load<std::uint8_t*>(parent,0x10)!=attacker)goto done;
                attackType=Load<std::uint32_t>(parent,0x34);
            }else attackType=Load<std::uint32_t>(frame,0x38);
            InterlockedExchange(&g_lastAccount,static_cast<LONG>(account));InterlockedExchange(&g_lastType,static_cast<LONG>(attackType));
            if(account&&!(account&0x80000000u))InterlockedIncrement(&g_humanCalls);
            if(g_policy.protects(account)){
                InterlockedIncrement(&g_rosterCalls);
                if(attackType==0)InterlockedIncrement(&g_ordinaryRosterCalls);
            }
            if(attacker!=room+0x3ce4+slot*0x1f54||Load<std::uint32_t>(room,0x533+slot*0x48e+0x429)!=account){InterlockedIncrement(&g_membershipRejected);goto done;}
            const auto damageOffset=part?0x1c:0x20;
            const auto capOffset=part?0x2c:0x30;
            const auto before=Load<std::uint32_t>(frame,damageOffset);
            const auto cap=Load<std::uint32_t>(frame,capOffset);
            std::uint32_t after=0,capAfter=0;
            if(!Hit(g_policy,account,attackType,before,cap,after,capAfter))goto done;
            SdgoNewcomerProtectionHitV1 receipt{};
            receipt.size=sizeof(receipt);receipt.version=1;receipt.account=account;
            receipt.victim=Load<std::uint32_t>(frame,part?0x18:0x1c);receipt.mode=room[5];
            const auto* weapon=Load<std::uint8_t*>(frame,part?0x10:0x14);
            receipt.weapon=weapon?Load<std::uint32_t>(weapon,0):0;
            receipt.proposed_before=before;receipt.proposed_after=after;receipt.cap_before=cap;receipt.cap_after=capAfter;
            // These sites run after the original cap and input diagnostics.
            // Keep original input/cap fields for native abuse checks; only
            // the admitted damage continues at the server-owned multiplier.
            std::memcpy(frame+damageOffset,&after,4);
            AcquireSRWLockExclusive(&g_receiptLock);
            receipt.sequence=++g_hitSequence;g_hits[(g_hitSequence-1)%kCapacity]=receipt;
            ReleaseSRWLockExclusive(&g_receiptLock);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
done:
    SetLastError(error);
}
void __stdcall ApplyBodyHit(std::uint8_t* frame) noexcept {ApplyHit(frame,false);}
void __stdcall ApplyPartHit(std::uint8_t* frame) noexcept {ApplyHit(frame,true);}
__declspec(naked) void Ready() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,220h
        and esp,0fffffff0h
        mov [esp+200h],eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_defaultMxcsr]
        cld
        push ebp
        call Apply
        fxrstor [esp]
        mov esp,[esp+200h]
        popad
        popfd
        jmp dword ptr [g_ready]
    }
}
__declspec(naked) void HitReady() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,220h
        and esp,0fffffff0h
        mov [esp+200h],eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_defaultMxcsr]
        cld
        push ebp
        call ApplyBodyHit
        fxrstor [esp]
        mov esp,[esp+200h]
        popad
        popfd
        jmp dword ptr [g_hitReady]
    }
}
__declspec(naked) void PartReady() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,220h
        and esp,0fffffff0h
        mov [esp+200h],eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_defaultMxcsr]
        cld
        push ebp
        call ApplyPartHit
        fxrstor [esp]
        mov esp,[esp+200h]
        popad
        popfd
        jmp dword ptr [g_partReady]
    }
}
}
extern "C" __declspec(dllexport) std::uint32_t __cdecl SdgoNewcomerProtectionRecentV1(
    SdgoNewcomerProtectionReceiptV1* output,std::uint32_t capacity) {
    if(!output||!capacity)return 0;
    AcquireSRWLockShared(&g_receiptLock);
    const auto count=(std::min)(capacity,(std::min)(g_sequence,kCapacity));
    for(std::uint32_t i=0;i<count;++i)output[i]=g_receipts[(g_sequence-count+i)%kCapacity];
    ReleaseSRWLockShared(&g_receiptLock);
    return count;
}
extern "C" __declspec(dllexport) std::uint32_t __cdecl SdgoNewcomerProtectionHitsV1(
    SdgoNewcomerProtectionHitV1* output,std::uint32_t capacity) {
    if(!output||!capacity)return 0;
    AcquireSRWLockShared(&g_receiptLock);
    const auto count=(std::min)(capacity,(std::min)(g_hitSequence,kCapacity));
    for(std::uint32_t i=0;i<count;++i)output[i]=g_hits[(g_hitSequence-count+i)%kCapacity];
    ReleaseSRWLockShared(&g_receiptLock);
    return count;
}
extern "C" __declspec(dllexport) int __cdecl SdgoNewcomerProtectionGetStatusV1(SdgoNewcomerProtectionStatusV1* output) {
    if(!output||output->size!=sizeof(*output)||output->version!=1)return 0;
    const SdgoNewcomerProtectionStatusV1 value{sizeof(*output),1,
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_hitCalls,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_humanCalls,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_rosterCalls,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_ordinaryRosterCalls,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_membershipRejected,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_lastAccount,0,0)),
        static_cast<std::uint32_t>(InterlockedCompareExchange(&g_lastType,0,0))};
    *output=value;return 1;
}
SDGO_MOD_ENTRY {
    if(!api||!mod||api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite))return 0;
    if(api->version!=SDGO_API_VERSION||!api->HookSite||!api->Log||!api->ModDirectory||
       !api->host_base||!api->host_key||std::strcmp(api->host_key,"BattleServer-548A891D-0046C000")!=0)return 0;
    g_api=api;g_mod=mod;
    g_partCaller=reinterpret_cast<std::uintptr_t>(api->host_base)+0xa0b79;
    const sdgo::ModSettings settings(api,mod);
    char enabled[16]{},health[40]{},attack[40]{},accounts[2048]{};
    settings.String("Enabled","0",enabled,sizeof(enabled));
    if(std::strcmp(enabled,"0")==0){api->Log(mod,SDGO_LOG_INFO,"newcomer protection disabled; native stats retained");return 1;}
    const auto n=settings.String("ProtectedAccounts","",accounts,sizeof(accounts));
    settings.String("HealthMultiplier","2",health,sizeof(health));
    settings.String("AttackMultiplier","2",attack,sizeof(attack));
    if(std::strcmp(enabled,"1")!=0||n>=sizeof(accounts)-1||
       !Multiplier(health,g_policy.health)||!Multiplier(attack,g_policy.attack)||
       !Accounts(accounts,g_policy.accounts)){
        api->Log(mod,SDGO_LOG_ERROR,"invalid newcomer protection policy; original stats retained");return 0;
    }
    g_policy.enabled=true;
    if(g_policy.accounts.empty()){api->Log(mod,SDGO_LOG_INFO,"newcomer roster empty; native stats retained");return 1;}
    if(api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite)||
       !api->HookSite(mod,"newcomer_protection.human_ready",reinterpret_cast<void*>(&Ready),&g_ready)||
       !api->HookSite(mod,"newcomer_protection.normal_hit",reinterpret_cast<void*>(&HitReady),&g_hitReady)||
       !api->HookSite(mod,"newcomer_protection.part_hit",reinterpret_cast<void*>(&PartReady),&g_partReady))return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: %u protected players; HP x%.3f; ordinary attack x%.3f; special and database unchanged",
        static_cast<unsigned>(g_policy.accounts.size()),g_policy.health,g_policy.attack);
    return 1;
}
