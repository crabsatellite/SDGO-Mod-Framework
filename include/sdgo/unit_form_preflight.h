#pragma once
#include "sdgo/native_unit_transform_context.h"
#include "sdgo/unit_battle_definition.h"

namespace sdgo::unit_forms {
class UnitDefinitionLookup {
public:
    virtual ~UnitDefinitionLookup()=default;
    virtual const void* find(std::uint32_t identity)const=0;
};
enum class PreflightDecision { unmanaged, rejected, native_cached, native_uncached };
struct PreflightResult {
    PreflightDecision decision=PreflightDecision::rejected;
    const char* reason="";
    std::uint32_t normalizations=0;
};
// Read-only checks of a context captured at the original authorized/locked
// paired-bank boundary. The caller must separately check the observer's current
// lifecycle token before/after this call under that same native room lock.
// native_uncached means only that its definitions/edge match; it supplies no
// bank retirement, skill/weapon state migration or permission to mutate actors.
PreflightResult CheckUnitForm(const GraphBindings&,const std::map<std::uint32_t,BattleDefinition>&,
                             const NativeTransformContext&,const HumanUnitCaptureSnapshot&,
                             const UnitDefinitionLookup&);
} // namespace sdgo::unit_forms
