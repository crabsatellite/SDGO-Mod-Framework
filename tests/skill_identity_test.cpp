#include "sdgo/skill_identity.h"
#include <cstdio>
#include <map>

using namespace sdgo::skill_identity;
namespace {
int failures=0;
void Check(bool valid,const char* name){std::printf("%s %s\n",valid?"PASS":"FAIL",name);if(!valid)++failures;}
struct Allocation final:Authority {
    bool allow=true, baselineDestination=false;
    std::string revision(const std::string&)const override{return std::string(64,'a');}
    bool originalSkill(std::uint32_t id)const override{return id==70207||(baselineDestination&&id==9800123);}
    bool templateSkill(std::uint32_t id,const std::string& hash)const override{return id==70207&&hash==std::string(64,'b');}
    bool allocatedSkill(const std::string& owner,std::uint32_t id,const std::string& client,const std::string& server)const override{
        return allow&&owner=="example"&&id==9800123&&client==std::string(64,'c')&&server==std::string(64,'d');
    }
};
struct Registry final:Lookup {
    std::map<std::uint32_t,const void*> values;
    const void* find(std::uint32_t id)const override{const auto found=values.find(id);return found==values.end()?nullptr:found->second;}
};
}
int main(){
    Allocation authority;std::string error;
    Candidate candidate{"example",std::string(64,'a'),{{9800123,70207,std::string(64,'c'),std::string(64,'d'),std::string(64,'b')}}};
    auto bindings=Bindings::Build({candidate},authority,error);Check(bool(bindings),"independent allocation/template authority admits the selector mapping");
    std::uint32_t source=70207,target=9800123,copy=9800123;Registry lookup;lookup.values={{70207,&source},{9800123,&target}};
    auto result=bindings->resolve(&target,target,lookup);
    Check(result.resolution==Resolution::Aliased&&result.selector==70207&&target==9800123,"selector changes without changing the destination record ID");
    Check(bindings->resolve(&source,source,lookup).resolution==Resolution::Original,"stock behavior remains on the original path");
    Check(bindings->resolve(&copy,copy,lookup).resolution==Resolution::Rejected,"an actor copy or forged matching ID cannot substitute for the borrowed registry pointer");
    lookup.values.erase(70207);Check(bindings->resolve(&target,target,lookup).resolution==Resolution::Rejected,"missing source definition refuses the alias");
    authority.allow=false;Check(!Bindings::Build({candidate},authority,error),"candidate owner claims do not replace allocation authority");authority.allow=true;
    authority.baselineDestination=true;Check(!Bindings::Build({candidate},authority,error),"an original destination cannot be adopted");authority.baselineDestination=false;
    auto changed=candidate;changed.skills[0].source=70205;Check(!Bindings::Build({changed},authority,error),"an unaudited or default selector is not treated as this semantic profile");
    changed=candidate;changed.skills[0].clientSha256=std::string(64,'0');Check(!Bindings::Build({changed},authority,error),"changed final client record hash refuses");
    changed=candidate;changed.skills.push_back(changed.skills[0]);Check(!Bindings::Build({changed},authority,error),"duplicate destinations refuse the whole mapping");
    return failures?1:0;
}
