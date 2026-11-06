#include <windows.h>
#include "sdgo/native_unit_form_commit.h"
namespace sdgo::unit_forms {
namespace {
bool Writable(const std::uint8_t* at){
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(at,&info,sizeof(info))||info.State!=MEM_COMMIT||
       (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
       !((info.Protect&0xff)==PAGE_READWRITE||(info.Protect&0xff)==PAGE_WRITECOPY))return false;
    const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
    return reinterpret_cast<std::uintptr_t>(at)<=end&&end-reinterpret_cast<std::uintptr_t>(at)>=12;
}
bool Restore(std::uint8_t* at,std::uint32_t id,std::uint32_t definition){
    __try{
        if(!Writable(at))return false;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(at),static_cast<LONG>(id));
        InterlockedExchange(reinterpret_cast<volatile LONG*>(at+8),static_cast<LONG>(definition));
        return Read32(at)==id&&Read32(at+8)==definition;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
IdentityCommit Execute(void* address,const Slot* before,const Slot* staged){
    if(!address||address==before||address==staged)return IdentityCommit::refused;
    const auto active=Read32(before->data()+kActiveBankOffset);
    if(active>1||Read32(before->data())<10001||Read32(before->data())>30000||!Read32(before->data()+0x34))return IdentityCommit::refused;
    const auto at=kBankOffset+(active^1)*kBankBytes;
    const auto oldId=Read32(before->data()+at),newId=Read32(staged->data()+at);
    const auto oldPointer=Read32(before->data()+at+8),newPointer=Read32(staged->data()+at+8);
    if(oldId<10001||oldId>30000||newId<10001||newId>30000||oldId==newId||!oldPointer||!newPointer||oldPointer==newPointer)
        return IdentityCommit::refused;
    for(std::size_t i=0;i<kSlotBytes;++i)
        if(!((i>=at&&i<at+4)||(i>=at+8&&i<at+12))&&(*before)[i]!=(*staged)[i])return IdentityCommit::refused;
    auto* live=static_cast<std::uint8_t*>(address);auto* header=live+at;
    if(reinterpret_cast<std::uintptr_t>(header)%4||!Writable(header))return IdentityCommit::refused;
    bool wrote=false;
    __try{
        if(std::memcmp(live,before->data(),kSlotBytes))return IdentityCommit::refused;
        wrote=true;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(header),static_cast<LONG>(newId));
        InterlockedExchange(reinterpret_cast<volatile LONG*>(header+8),static_cast<LONG>(newPointer));
        if(!std::memcmp(live,staged->data(),kSlotBytes))return IdentityCommit::committed;
    }__except(EXCEPTION_EXECUTE_HANDLER){if(!wrote)return IdentityCommit::refused;}
    return Restore(header,oldId,oldPointer)?IdentityCommit::restored:IdentityCommit::unrecoverable;
}
}
IdentityCommit CommitEquivalentIdentity(void* live,const Slot& before,const Slot& staged) noexcept{
    const auto error=GetLastError();const auto result=Execute(live,&before,&staged);SetLastError(error);return result;
}

namespace {
bool WritableRange(const std::uint8_t* at,std::size_t bytes){
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(at,&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
       !((info.Protect&0xff)==PAGE_READWRITE||(info.Protect&0xff)==PAGE_WRITECOPY))return false;
    const auto end=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
    return reinterpret_cast<std::uintptr_t>(at)<=end&&end-reinterpret_cast<std::uintptr_t>(at)>=bytes;
}
bool CopyRange(std::uint8_t* at,const std::uint8_t* from,std::size_t bytes){
    __try{if(!WritableRange(at,bytes))return false;std::memcpy(at,from,bytes);return std::memcmp(at,from,bytes)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
IdentityCommit ExecuteBank(void* address,const Slot* before,const Slot* staged){
    if(!address||address==before||address==staged)return IdentityCommit::refused;
    const auto active=Read32(before->data()+kActiveBankOffset);
    if(active>1||Read32(before->data())<10001||Read32(before->data())>30000||!Read32(before->data()+0x34))return IdentityCommit::refused;
    const auto bank=kBankOffset+(active^1)*kBankBytes,stats=kStatsOffset+(active^1)*kStatsBytes;
    const auto oldId=Read32(before->data()+bank),newId=Read32(staged->data()+bank);
    if(oldId<10001||oldId>30000||newId<10001||newId>30000||oldId==newId||!Read32(staged->data()+bank+8)||
       newId==Read32(before->data()+kBankOffset+active*kBankBytes))return IdentityCommit::refused;
    for(std::size_t i=0;i<kSlotBytes;++i){
        const bool inBank=i>=bank&&i<bank+kBankBytes,inStats=i>=stats&&i<stats+kStatsBytes;
        if(!inBank&&!inStats&&(*before)[i]!=(*staged)[i])return IdentityCommit::refused;
    }
    auto* live=static_cast<std::uint8_t*>(address);
    if(reinterpret_cast<std::uintptr_t>(live+bank)%4||!WritableRange(live+bank,kBankBytes)||!WritableRange(live+stats,kStatsBytes))return IdentityCommit::refused;
    bool wrote=false;
    __try{
        if(std::memcmp(live,before->data(),kSlotBytes))return IdentityCommit::refused;
        wrote=true;
        std::memcpy(live+stats,staged->data()+stats,kStatsBytes);
        std::memcpy(live+bank,staged->data()+bank,kBankBytes);
        if(!std::memcmp(live,staged->data(),kSlotBytes))return IdentityCommit::committed;
    }__except(EXCEPTION_EXECUTE_HANDLER){if(!wrote)return IdentityCommit::refused;}
    return CopyRange(live+bank,before->data()+bank,kBankBytes)&&CopyRange(live+stats,before->data()+stats,kStatsBytes)
        ?IdentityCommit::restored:IdentityCommit::unrecoverable;
}
}
IdentityCommit CommitInactiveBank(void* live,const Slot& before,const Slot& staged) noexcept{
    const auto error=GetLastError();const auto result=ExecuteBank(live,&before,&staged);SetLastError(error);return result;
}
} // namespace sdgo::unit_forms
