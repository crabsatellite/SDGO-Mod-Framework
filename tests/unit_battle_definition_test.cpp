#include "sdgo/unit_battle_definition.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace sdgo::unit_forms;
void Put(BattleDefinition& data,std::uint32_t value){std::memcpy(data.data()+0x45,&value,4);}
int main(){
    BattleDefinition expected;
    for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::uint8_t>(i*17+3);
    for(std::uint32_t initial:{0u,4u,2147483647u,0xffffffffu}){
        Put(expected,initial);auto actual=expected;Put(actual,1);
        const auto oldExpected=expected,oldActual=actual;
        assert(MatchBattleDefinition(expected,expected)==DefinitionMatch::exact);
        assert(MatchBattleDefinition(expected,actual)==DefinitionMatch::native_default_normalized);
        assert(expected==oldExpected&&actual==oldActual);
        for(std::size_t i=0;i<expected.size();++i)if(i<0x45||i>=0x49){
            auto drift=actual;drift[i]^=1;assert(MatchBattleDefinition(expected,drift)==DefinitionMatch::different);
        }
        Put(actual,2);assert(MatchBattleDefinition(expected,actual)==DefinitionMatch::different);
    }
    for(std::uint32_t initial:{1u,2u,3u}){
        Put(expected,initial);auto actual=expected;
        assert(MatchBattleDefinition(expected,actual)==DefinitionMatch::exact);
        Put(actual,initial==1?2:1);assert(MatchBattleDefinition(expected,actual)==DefinitionMatch::different);
    }
    auto target=expected;std::uint32_t identity=15001;std::memcpy(expected.data(),&identity,4);identity=15003;std::memcpy(target.data(),&identity,4);
    Put(target,2);assert(EquivalentPrologueData(expected,target));
    for(std::size_t i=4;i<expected.size();++i){
        auto changed=target;changed[i]^=1;
        assert(EquivalentPrologueData(expected,changed)==((i>=0x45&&i<0x49)||(i>=0x59&&i<0x5d)));
    }
}
