#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace sdgo::dependency {
struct Version {
    std::array<uint32_t,4> numbers{};std::vector<std::string> suffix;bool valid=false;
};
inline bool Digits(const std::string& value) {
    return !value.empty()&&std::all_of(value.begin(),value.end(),[](unsigned char c){return c>='0'&&c<='9';});
}
inline std::vector<std::string> Split(const std::string& value,char separator) {
    std::vector<std::string> result;size_t begin=0;
    for(;;){const auto end=value.find(separator,begin);result.push_back(value.substr(begin,end==std::string::npos?end:end-begin));if(end==std::string::npos)break;begin=end+1;}return result;
}
inline Version Parse(std::string value) {
    Version result;const auto plus=value.find('+');if(plus!=std::string::npos)value.resize(plus);
    const auto dash=value.find('-');std::string main=value.substr(0,dash);
    auto fields=Split(main,'.');if(fields.empty()||fields.size()>4)return result;
    for(size_t i=0;i<fields.size();++i) {
        if(!Digits(fields[i]))return result;uint64_t number=0;
        for(char c:fields[i]){number=number*10+static_cast<unsigned>(c-'0');if(number>2147483647)return result;}result.numbers[i]=static_cast<uint32_t>(number);
    }
    if(dash!=std::string::npos){result.suffix=Split(value.substr(dash+1),'.');for(const auto& field:result.suffix)if(field.empty())return result;}
    result.valid=true;return result;
}
inline int Compare(const Version& a,const Version& b) {
    if(a.numbers!=b.numbers)return a.numbers<b.numbers?-1:1;
    if(a.suffix.empty()!=b.suffix.empty())return a.suffix.empty()?1:-1;
    for(size_t i=0;i<std::min(a.suffix.size(),b.suffix.size());++i) {
        auto x=a.suffix[i],y=b.suffix[i];if(x==y)continue;
        const bool xn=Digits(x),yn=Digits(y);if(xn!=yn)return xn?-1:1;
        if(xn){while(x.size()>1&&x[0]=='0')x.erase(0,1);while(y.size()>1&&y[0]=='0')y.erase(0,1);if(x.size()!=y.size())return x.size()<y.size()?-1:1;}
        if(x!=y)return x<y?-1:1;
    }
    return a.suffix.size()==b.suffix.size()?0:a.suffix.size()<b.suffix.size()?-1:1;
}
inline bool Compatible(const std::string& installed,const std::string& minimum) {
    if(minimum.empty())return true;const auto a=Parse(installed),b=Parse(minimum);return a.valid&&b.valid&&Compare(a,b)>=0;
}
}
