#include <windows.h>
#include "sdgo/unit_form_preflight.h"

namespace sdgo::unit_forms {
namespace {
bool Read(const void* definition,std::uint32_t id,BattleDefinition* data) {
    __try {
        if(!definition)return false;
        std::memcpy(data->data(),definition,data->size());return DefinitionWord(*data,0)==id;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
PreflightResult CheckUnitForm(const GraphBindings& graphs,const std::map<std::uint32_t,BattleDefinition>& expected,
                             const NativeTransformContext& context,const HumanUnitCaptureSnapshot& captured,
                             const UnitDefinitionLookup& lookup) {
    if(!graphs.hasEntry(context.root)||(context.owner&0x80000000u))return {PreflightDecision::unmanaged,"",0};
    if(!MatchesCapture(context,captured))return {PreflightDecision::rejected,"captured unit identity differs",0};
    if(context.active>1||context.current!=context.forms[context.active]||!context.hp)
        return {PreflightDecision::rejected,"current native bank differs",0};
    if(!graphs.resolveTarget(context.root,context.current,context.target))
        return {PreflightDecision::rejected,"requested directed edge is not allowed",0};
    const std::uint32_t ids[]={context.root,context.forms[0],context.forms[1],context.target};
    const void* pointers[4]{};std::uint32_t normalized=0;
    for(unsigned i=0;i<4;++i){
        const auto found=expected.find(ids[i]);const auto* node=graphs.inspectNode(context.root,ids[i]);
        if(found==expected.end()||!node)return {PreflightDecision::rejected,"native definition witness is absent",normalized};
        pointers[i]=lookup.find(ids[i]);BattleDefinition actual{};
        if(!Read(pointers[i],ids[i],&actual)||DefinitionWord(actual,0xc)!=node->target)
            return {PreflightDecision::rejected,"current native definition identity/edge differs",normalized};
        const auto match=MatchBattleDefinition(found->second,actual);
        if(match==DefinitionMatch::different)return {PreflightDecision::rejected,"current native definition bytes differ",normalized};
        if(match==DefinitionMatch::native_default_normalized)++normalized;
    }
    if(pointers[0]!=captured.unit.definition||pointers[1]!=context.definitions[0]||pointers[2]!=context.definitions[1])
        return {PreflightDecision::rejected,"native registry/captured bank pointers differ",normalized};
    const auto decision=context.target==context.forms[context.active^1]?PreflightDecision::native_cached:PreflightDecision::native_uncached;
    return {decision,"",normalized};
}
} // namespace sdgo::unit_forms
