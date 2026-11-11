#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sdgo::newcomer {
constexpr std::size_t kUnitBytes=0x658, kFormStride=0x0c, kForms=2;
using Unit=std::array<std::uint8_t,kUnitBytes>;
struct Policy {
    bool enabled=false;
    double health=2,attack=2;
    std::vector<std::uint32_t> accounts;
    bool protects(std::uint32_t account) const noexcept {
        return enabled && account && !(account&0x80000000u) &&
            std::binary_search(accounts.begin(),accounts.end(),account);
    }
};
inline bool Multiplier(const std::string& text,double& output) {
    if(text.empty() || text.size()>32)return false;
    for(char c:text)if(!(c>='0'&&c<='9')&&c!='.')return false;
    char* end=nullptr;const auto value=std::strtod(text.c_str(),&end);
    if(end!=text.c_str()+text.size() || !std::isfinite(value) || value<1 || value>10)return false;
    output=value;return true;
}
inline bool Accounts(const std::string& text,std::vector<std::uint32_t>& output) {
    std::vector<std::uint32_t> parsed;
    std::size_t at=0;
    while(at<text.size()){
        while(at<text.size() && text[at]==' ')++at;
        if(at==text.size())return false;
        std::uint64_t value=0;const auto begin=at;
        while(at<text.size() && text[at]>='0'&&text[at]<='9'){
            value=value*10+static_cast<unsigned>(text[at++]-'0');
            if(value>2147483647)return false;
        }
        if(at==begin || !value || parsed.size()>=256)return false;
        parsed.push_back(static_cast<std::uint32_t>(value));
        while(at<text.size() && text[at]==' ')++at;
        if(at<text.size() && (text[at++]!=',' || at==text.size()))return false;
    }
    std::sort(parsed.begin(),parsed.end());
    parsed.erase(std::unique(parsed.begin(),parsed.end()),parsed.end());
    output=std::move(parsed);return true;
}
template<class T> T Read(const Unit& unit,std::size_t offset) {
    T value{};std::memcpy(&value,unit.data()+offset,sizeof(value));return value;
}
template<class T> void Write(Unit& unit,std::size_t offset,T value) {
    std::memcpy(unit.data()+offset,&value,sizeof(value));
}
inline std::uint32_t Scale(std::uint32_t value,double multiplier,std::uint32_t cap) {
    return static_cast<std::uint32_t>((std::min)(std::floor(value*multiplier+0.5),static_cast<double>(cap)));
}
// Native-final values include the active room's parts/OC rules and the native
// life-budget fraction. This plan changes HP only. Ordinary hit scaling is
// separate, so every native attack/defense/special stat remains bit-identical.
inline bool Plan(const Unit& before,const Policy& policy,Unit& after) {
    if(!std::isfinite(policy.health)||!std::isfinite(policy.attack)||
       policy.health<1||policy.health>10||policy.attack<1||policy.attack>10)return false;
    const auto maximum=Read<std::uint32_t>(before,4),current=Read<std::uint32_t>(before,0x34);
    if(!Read<std::uint32_t>(before,0)||!maximum||maximum>INT32_MAX||current>maximum)return false;
    after=before;
    Write(after,4,Scale(maximum,policy.health,INT32_MAX));
    Write(after,0x34,Scale(current,policy.health,INT32_MAX));
    return true;
}
inline bool Hit(const Policy& policy,std::uint32_t account,std::uint32_t attackType,
                std::uint32_t proposed,std::uint32_t cap,
                std::uint32_t& damageAfter,std::uint32_t& capAfter) {
    // Native type 0 is the ordinary weapon path; 1/2/3 are special attacks.
    // First retain the original per-hit cap, then scale this accepted input.
    // Damage is calculated by the original client, so changing a server-side
    // attack stat alone would not change ordinary human attacks.
    if(!policy.protects(account)||attackType!=0||!cap||cap>100000||
       !std::isfinite(policy.attack)||policy.attack<1||policy.attack>10)return false;
    damageAfter=Scale((std::min)(proposed,cap),policy.attack,INT32_MAX);
    capAfter=Scale(cap,policy.attack,INT32_MAX);return true;
}
} // namespace sdgo::newcomer
