#include <windows.h>
#include "sdgo/native_unit_form_commit.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace sdgo::unit_forms;
void Put(Slot& data,std::size_t at,std::uint32_t value){std::memcpy(data.data()+at,&value,4);}
int main(){
    auto* memory=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));assert(memory);
    for(unsigned active:{0u,1u}){
        Slot before;for(std::size_t i=0;i<before.size();++i)before[i]=static_cast<std::uint8_t>(i*31+5);
        Put(before,0,15001);Put(before,0x34,100);Put(before,kActiveBankOffset,active);
        const auto at=kBankOffset+(active^1)*kBankBytes;Put(before,at,15001);Put(before,at+8,0x10000000);
        auto staged=before;Put(staged,at,15003);Put(staged,at+8,0x20000000);
        std::memcpy(memory,before.data(),before.size());SetLastError(123);
        assert(CommitEquivalentIdentity(memory,before,staged)==IdentityCommit::committed&&GetLastError()==123);
        assert(!std::memcmp(memory,staged.data(),staged.size()));
        std::memcpy(memory,before.data(),before.size());memory[0x30]^=1;const auto changed=memory[0x30];
        assert(CommitEquivalentIdentity(memory,before,staged)==IdentityCommit::refused&&memory[0x30]==changed);
        std::memcpy(memory,before.data(),before.size());auto invalid=staged;invalid[0x30]^=1;
        assert(CommitEquivalentIdentity(memory,before,invalid)==IdentityCommit::refused&&!std::memcmp(memory,before.data(),before.size()));
        assert(CommitEquivalentIdentity(nullptr,before,staged)==IdentityCommit::refused);
        assert(CommitEquivalentIdentity(before.data(),before,staged)==IdentityCommit::refused);
        DWORD protection=0;assert(VirtualProtect(memory,4096,PAGE_READONLY,&protection));
        assert(CommitEquivalentIdentity(memory,before,staged)==IdentityCommit::refused);
        assert(VirtualProtect(memory,4096,PAGE_READWRITE,&protection));
    }
    assert(VirtualFree(memory,0,MEM_RELEASE));
}
