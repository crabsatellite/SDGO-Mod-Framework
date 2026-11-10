#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mods/newcomer_protection/policy.h"
#include <cassert>
#include <climits>
#include <iostream>
using namespace sdgo::newcomer;
int main() {
    Policy policy;double multiplier=0;
    for(const auto* bad:{"","nan","inf","2x","-2","0","10.1","1e3","2\n"})assert(!Multiplier(bad,multiplier));
    assert(Multiplier("1.5",multiplier)&&multiplier==1.5);
    assert(Accounts(" 43,7,43 ",policy.accounts));
    policy.enabled=true;assert(policy.protects(7)&&!policy.protects(8)&&!policy.protects(0x80000007));
    for(const auto* bad:{"0","-1","7,","7,,8","2147483648","7 8"})assert(!Accounts(bad,policy.accounts));
    Unit original;original.fill(0xa5);
    Write(original,0,std::uint32_t{123});Write(original,4,std::uint32_t{10001});Write(original,0x34,std::uint32_t{3999});
    Write(original,0xe,std::uint16_t{1234});Write(original,0x1a,std::uint16_t{4321});
    Unit planned;assert(Plan(original,policy,planned));
    assert(Read<std::uint32_t>(planned,4)==20002&&Read<std::uint32_t>(planned,0x34)==7998);
    assert(Read<std::uint16_t>(planned,0xe)==1234&&Read<std::uint16_t>(planned,0x1a)==4321);
    for(std::size_t i=0;i<original.size();++i){
        const bool allowed=(i>=4&&i<8)||(i>=0x34&&i<0x38);
        assert(allowed||planned[i]==original[i]);
    }
    assert(Read<std::uint32_t>(planned,0x12)==Read<std::uint32_t>(original,0x12));
    assert(Read<std::uint32_t>(planned,0x1e)==Read<std::uint32_t>(original,0x1e));
    policy.health=1.5;policy.attack=3;assert(Plan(original,policy,planned));
    assert(Read<std::uint32_t>(planned,4)==15002&&Read<std::uint16_t>(planned,0xe)==1234);
    Write(original,4,std::uint32_t{INT32_MAX});Write(original,0x34,std::uint32_t{INT32_MAX});
    Write(original,0xe,std::uint16_t{50000});assert(Plan(original,policy,planned));
    assert(Read<std::uint32_t>(planned,4)==INT32_MAX&&Read<std::uint16_t>(planned,0xe)==50000);
    std::uint32_t damage=0,cap=0;
    assert(Hit(policy,7,0,2000,13000,damage,cap)&&damage==6000&&cap==39000);
    assert(Hit(policy,7,0,999999,13000,damage,cap)&&damage==39000);
    for(auto type:{1u,2u,3u,4u})assert(!Hit(policy,7,type,2000,19000,damage,cap));
    assert(!Hit(policy,8,0,2000,13000,damage,cap));
    Write(original,4,std::uint32_t{100});assert(!Plan(original,policy,planned));
    policy.attack=std::numeric_limits<double>::quiet_NaN();assert(!Plan(original,policy,planned));
    assert(Accounts("",policy.accounts)&&policy.accounts.empty()&&!policy.protects(7));
    std::cout<<"newcomer policy, overflow, identity and exact native write footprint passed\n";
}
