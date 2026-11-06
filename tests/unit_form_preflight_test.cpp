#include <windows.h>
#include "sdgo/unit_form_preflight.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace sdgo::unit_forms;
namespace {
void Put(BattleDefinition& data,std::size_t at,std::uint32_t value){std::memcpy(data.data()+at,&value,4);}
class Authority final:public GraphAuthority {
public:
    std::string revision(const std::string&)const override{return std::string(64,'a');}
    bool entry(const std::string&,std::uint32_t id)const override{return id==15001;}
    bool unit(const std::string&,const GraphNode&)const override{return true;}
};
struct Fixture:UnitDefinitionLookup {
    std::shared_ptr<const GraphBindings> graphs;
    std::map<std::uint32_t,BattleDefinition> expected,actual;
    NativeTransformContext context{};HumanUnitCaptureSnapshot snapshot;
    std::uint32_t bad=0;
    Fixture(){
        GraphOwner owner{"Example",std::string(64,'a'),{15001},{}};
        for(unsigned id:{15001,15002,15003}){
            const unsigned target=id==15002?15003:15002;
            owner.nodes.push_back({id,target,NodeOrigin::owned,std::string(64,'b'),std::string(64,'c')});
            BattleDefinition data{};Put(data,0,id);Put(data,0xc,target);Put(data,0x45,1);expected[id]=actual[id]=data;
        }
        Authority authority;std::string error;graphs=GraphBindings::Build({owner},authority,error);assert(graphs);
        context.room=reinterpret_cast<void*>(0x10000);context.source=reinterpret_cast<void*>(0x20000);
        context.player=reinterpret_cast<void*>(0x30000);context.slot=reinterpret_cast<void*>(0x40000);
        context.owner=7;context.root=15001;context.deck=0;context.mode=1;context.hp=99;
        context.active=0;context.current=15001;context.target=15002;context.serial=0x1234567800000001ull;
        context.forms[0]=15001;context.forms[1]=15002;context.targets[0]=15002;context.targets[1]=15003;
        context.definitions[0]=find(15001);context.definitions[1]=find(15002);
        snapshot.room=reinterpret_cast<std::uintptr_t>(context.room);snapshot.player=reinterpret_cast<std::uintptr_t>(context.player);
        snapshot.roomEpoch=1;snapshot.generation=2;snapshot.unit.owner=7;snapshot.unit.root=15001;snapshot.unit.deck=0;
        snapshot.unit.serial=context.serial;snapshot.unit.definition=find(15001);snapshot.unit.composition.mode=1;
    }
    const void* find(std::uint32_t id)const override{
        if(id==bad)return reinterpret_cast<void*>(1);const auto p=actual.find(id);return p==actual.end()?nullptr:p->second.data();
    }
    PreflightResult check(){return CheckUnitForm(*graphs,expected,context,snapshot,*this);}
};
}
int main(){
    Fixture f;const auto before=f.actual;
    assert(f.check().decision==PreflightDecision::native_cached&&f.actual==before);
    f.context.active=1;f.context.current=15002;f.context.target=15003;
    assert(f.check().decision==PreflightDecision::native_uncached&&f.actual==before);
    // The old cached root is not a valid reverse edge from the source graph.
    f.context.target=15001;assert(f.check().decision==PreflightDecision::rejected);f.context.target=15003;
    f.snapshot.unit.serial^=0x100000000ull;assert(f.check().decision==PreflightDecision::rejected);f.snapshot.unit.serial=f.context.serial;
    f.expected.erase(15003);assert(f.check().decision==PreflightDecision::rejected);f.expected[15003]=f.actual[15003];
    for(std::size_t at:{0u,0xcu,0x35u,0x49u,0x59u,0x6au,0x6eu}){
        f.actual[15003][at]^=1;assert(f.check().decision==PreflightDecision::rejected);f.actual[15003][at]^=1;
    }
    BattleDefinition replacement=f.actual[15001];f.snapshot.unit.definition=replacement.data();
    assert(f.check().decision==PreflightDecision::rejected);f.snapshot.unit.definition=f.find(15001);
    f.context.definitions[1]=replacement.data();assert(f.check().decision==PreflightDecision::rejected);f.context.definitions[1]=f.find(15002);
    f.bad=15003;assert(f.check().decision==PreflightDecision::rejected);f.bad=0;
    Put(f.expected[15001],0x45,0);const auto normalized=f.check();assert(normalized.decision==PreflightDecision::native_uncached&&normalized.normalizations>0);
    f.expected[15001]=f.actual[15001];f.context.owner=0x80000007u;assert(f.check().decision==PreflightDecision::unmanaged);
    f.context.owner=7;f.context.root=16000;assert(f.check().decision==PreflightDecision::unmanaged);
}
