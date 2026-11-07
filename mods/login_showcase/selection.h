#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sdgo::login_showcase {
enum class Mode { Grades, All, Recent, Units };
constexpr uint32_t GradeBit(uint32_t rank,uint32_t subgrade) {
    return rank>=1 && rank<=4 && subgrade>=2 && subgrade<=5
        ? 1u<<((rank-1)*4+subgrade-2):0;
}
constexpr uint32_t kDefaultGrades=GradeBit(4,3)|GradeBit(4,4)|GradeBit(4,5);
struct Config {
    Mode mode=Mode::Grades;
    uint32_t grade_mask=kDefaultGrades,recent_count=10;
    std::vector<uint32_t> unit_ids;
};
struct Unit {
    uint32_t unit_id,catalog_id,rank,subgrade;
};
inline std::string Trim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    value=value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
    return value;
}
inline bool ParseMode(std::string text,Mode& mode) {
    text=Trim(std::move(text));
    for(auto& c:text)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    if(text=="grades")mode=Mode::Grades;
    else if(text=="all")mode=Mode::All;
    else if(text=="recent")mode=Mode::Recent;
    else if(text=="units")mode=Mode::Units;
    else return false;
    return true;
}
inline bool Tokens(const std::string& text,std::vector<std::string>& result) {
    result.clear();if(Trim(text).empty())return true;
    size_t start=0;
    do {
        const auto end=text.find(',',start);
        auto token=Trim(text.substr(start,end==std::string::npos?end:end-start));
        if(token.empty())return false;
        result.push_back(std::move(token));
        if(end==std::string::npos)return true;
        start=end+1;
    } while(start<=text.size());
    return false;
}
inline bool ParseGrades(const std::string& text,uint32_t& mask) {
    std::vector<std::string> parts;if(!Tokens(text,parts)||parts.empty())return false;
    uint32_t result=0;
    for(auto value:parts) {
        for(auto& c:value)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
        const auto at=std::string("CBAS").find(value[0]);
        if(at==std::string::npos||value.size()>2)return false;
        uint32_t sub=2;
        if(value.size()==2) {
            const auto suffix=std::string("SRU").find(value[1]);
            if(suffix==std::string::npos)return false;
            sub=static_cast<uint32_t>(suffix)+3;
        }
        result|=GradeBit(static_cast<uint32_t>(at)+1,sub);
    }
    mask=result;return true;
}
inline bool ParseUnits(const std::string& text,std::vector<uint32_t>& ids) {
    std::vector<std::string> parts;if(!Tokens(text,parts)||parts.size()>2048)return false;
    std::vector<uint32_t> result;std::unordered_set<uint32_t> seen;
    for(const auto& value:parts) {
        uint64_t id=0;
        for(char c:value) {
            if(c<'0'||c>'9')return false;
            id=id*10+static_cast<unsigned>(c-'0');
            if(id>0x7fffffffu)return false;
        }
        if(!id)return false;
        if(seen.insert(static_cast<uint32_t>(id)).second)result.push_back(static_cast<uint32_t>(id));
    }
    ids=std::move(result);return true;
}
inline std::vector<uint32_t> Candidates(const Config& config,const std::vector<Unit>& input) {
    std::unordered_map<uint32_t,Unit> unique;
    for(const auto& unit:input) {
        if(!unit.unit_id || unit.unit_id>0x7fffffffu || !unit.catalog_id || !GradeBit(unit.rank,unit.subgrade))continue;
        const auto old=unique.find(unit.unit_id);
        if(old==unique.end()||unit.catalog_id>old->second.catalog_id)unique.insert_or_assign(unit.unit_id,unit);
    }
    std::vector<Unit> units;
    for(const auto& pair:unique)units.push_back(pair.second);
    std::sort(units.begin(),units.end(),[](const Unit& a,const Unit& b){
        return a.catalog_id!=b.catalog_id?a.catalog_id>b.catalog_id:a.unit_id<b.unit_id;
    });
    std::vector<uint32_t> result;
    if(config.mode==Mode::Units) {
        std::unordered_set<uint32_t> seen;
        for(auto id:config.unit_ids)if(unique.count(id)&&seen.insert(id).second)result.push_back(id);
        return result;
    }
    for(const auto& unit:units) {
        if(config.mode==Mode::Grades && !(config.grade_mask&GradeBit(unit.rank,unit.subgrade)))continue;
        result.push_back(unit.unit_id);
        if(config.mode==Mode::Recent && result.size()>=config.recent_count)break;
    }
    if(config.mode==Mode::Recent && !config.recent_count)result.clear();
    return result;
}
// One shuffled traversal avoids repeated models within a scene, and avoids an
// immediate repeat across traversals when the pool has more than one unit.
class Rotation {
    std::vector<uint32_t> source_,bag_;
    size_t cursor_=0;
    uint32_t last_=0;
public:
    void SetPool(const std::vector<uint32_t>& pool) {
        if(pool==source_)return;
        source_=pool;bag_.clear();cursor_=0;
    }
    size_t size() const {return source_.size();}
    template<class Random> uint32_t Next(Random&& random) {
        if(source_.empty())return 0;
        if(cursor_==bag_.size()) {
            bag_=source_;cursor_=0;
            for(size_t i=bag_.size();i>1;--i)std::swap(bag_[i-1],bag_[random()%i]);
            if(bag_.size()>1 && bag_[0]==last_)std::swap(bag_[0],bag_[1]);
        }
        last_=bag_[cursor_++];return last_;
    }
};
}
