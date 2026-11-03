#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"

namespace sdgo::unit_skill_presentation {
using GridData = void*(__cdecl*)();
using GridFind = bool(__thiscall*)(void*, const std::uint8_t**, std::uint32_t, int, char);
using SetText = void(__thiscall*)(void*, const void*);
struct Registers {std::uint32_t edi,esi,ebp,savedEsp,ebx,edx,ecx,eax,flags;};
struct EmptyWideString {wchar_t data[8];std::uint32_t length,capacity;};
static_assert(sizeof(EmptyWideString)==24 && sizeof(void*)==4);
inline constexpr EmptyWideString kEmpty={{},0,7};
inline const SdgoApi* api=nullptr;
inline SdgoMod* mod=nullptr;
inline const unit_catalog::Catalog* catalog=nullptr;
inline GridData data=nullptr;
inline GridFind find=nullptr;
inline void* original=nullptr;
inline void* second=nullptr;
inline volatile LONG repaired=0;
alignas(16) inline const std::uint32_t defaultMxcsr=0x1f80;

inline bool ReadContext(Registers* registers,const std::uint8_t** record,void** name,void** description,std::uint8_t** icon) {
    __try {
        *record=reinterpret_cast<const std::uint8_t*>(registers->ecx);
        const std::uint8_t* argument=nullptr;
        std::memcpy(&argument,reinterpret_cast<const void*>(registers->ebp-0x58),4);
        if(argument!=*record || !argument || *reinterpret_cast<const int*>(registers->ebp-4)!=-1)return false;
        const auto index=catalog->Find(unit_catalog::U32(argument));
        if(index==catalog->records.size())return false;
        const auto& entry=catalog->records[index];
        if(entry.kind!=unit_catalog::kUnitType || entry.bytes.size()!=unit_catalog::kUnitBytes ||
            std::memcmp(entry.bytes.data(),argument,entry.bytes.size()) ||
            unit_catalog::U32(argument+575)!=0 || !unit_catalog::U32(argument+579))return false;
        auto* view=reinterpret_cast<const std::uint8_t*>(registers->esi);
        std::memcpy(name,view+0x19c,4);std::memcpy(description,view+0x1a8,4);std::memcpy(icon,view+0x180,4);
        return *name && *description && *icon;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

inline bool ClearMissingFirst(void* name,void* description,std::uint8_t* icon) {
    __try {
        const auto setName=reinterpret_cast<SetText>((*static_cast<void***>(name))[0x88/4]);
        const auto setDescription=reinterpret_cast<SetText>((*static_cast<void***>(description))[0x88/4]);
        if(!setName || !setDescription)return false;
        setName(name,&kEmpty);setDescription(description,&kEmpty);
        icon[0x162]=0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

inline int __cdecl ContinueSecond(Registers* registers) {
    const DWORD error=GetLastError();
    const std::uint8_t* record=nullptr;void* name=nullptr;void* description=nullptr;std::uint8_t* icon=nullptr;
    bool accepted=false;
    if(ReadContext(registers,&record,&name,&description,&icon)) {
        const std::uint8_t* zero=nullptr;const std::uint8_t* remaining=nullptr;
        void* grid=data();
        if(grid && !find(grid,&zero,0,13,0) && find(grid,&remaining,unit_catalog::U32(record+579),13,0) && remaining &&
            ClearMissingFirst(name,description,icon)) {
            // The original second-slot block begins with MOV EBX,[EDX+243].
            // The skipped first block would have loaded this same unit pointer.
            registers->edx=reinterpret_cast<std::uint32_t>(record);
            accepted=true;
            if(InterlockedIncrement(&repaired)<=4)api->Log(mod,SDGO_LOG_INFO,
                "encyclopedia retained the existing second skill after an intentionally empty imported first slot");
        }
    }
    SetLastError(error);return accepted ? 1 : 0;
}

__declspec(naked) inline void FirstSkillRead() {
    __asm {pushfd}
    __asm {pushad}
    __asm {mov eax,esp}
    __asm {sub esp,210h}
    __asm {and esp,0fffffff0h}
    __asm {mov [esp+200h],eax}
    __asm {fxsave [esp]}
    __asm {fninit}
    __asm {ldmxcsr dword ptr [defaultMxcsr]}
    __asm {cld}
    __asm {push eax}
    __asm {call ContinueSecond}
    __asm {add esp,4}
    __asm {test eax,eax}
    __asm {jz passthrough}
    __asm {fxrstor [esp]}
    __asm {mov esp,[esp+200h]}
    __asm {popad}
    __asm {popfd}
    __asm {jmp dword ptr [second]}
passthrough:
    __asm {fxrstor [esp]}
    __asm {mov esp,[esp+200h]}
    __asm {popad}
    __asm {popfd}
    __asm {jmp dword ptr [original]}
}

inline bool Attach(const SdgoApi* host,SdgoMod* instance,const unit_catalog::Catalog& entries,GridData gridData,GridFind gridFind) {
    bool gap=false;
    for(const auto& entry:entries.records)if(entry.kind==unit_catalog::kUnitType &&
        unit_catalog::U32(entry.bytes.data()+575)==0 && unit_catalog::U32(entry.bytes.data()+579)!=0)gap=true;
    if(!gap)return true;
    if(!host || host->size<offsetof(SdgoApi,HookSite)+sizeof(host->HookSite) || !host->HookSite)return false;
    auto* body=static_cast<const std::uint8_t*>(host->Symbol(instance,"unit_catalog.ui_skill_body"));
    auto* site=static_cast<const std::uint8_t*>(host->Symbol(instance,"unit_catalog.ui_skill_read"));
    auto* next=static_cast<const std::uint8_t*>(host->Symbol(instance,"unit_catalog.ui_skill_second"));
    if(!body || site!=body+0x290 || next!=body+0x42a || unit_catalog::Hash32(body,1529)!=0xd9ae9e1a)return false;
    api=host;mod=instance;catalog=&entries;data=gridData;find=gridFind;second=const_cast<std::uint8_t*>(next);
    return host->HookSite(instance,"unit_catalog.ui_skill_read",reinterpret_cast<void*>(&FirstSkillRead),&original)!=0;
}
}
