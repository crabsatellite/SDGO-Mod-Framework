#pragma once
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"

// Third-form transform chains (entry form E -> A <-> B) for imported units.
//
// The original client keeps exactly one (current, alternate) record/model pair
// per battle unit and only ever swaps the two. An imported entry variant starts
// as E with A as its alternate; after E -> A the native code would swap back to
// E on the next request because B is unknown to the pair. This runtime rebases
// the alternate pair to B right before the native swap whenever the move
// controller's cached target is the current record's transform target but not
// the alternate record, rebuilds the attack controller's weapon tables and
// restores the deck-owned pair before the unit is destroyed.
namespace sdgo::unit_form_chain {
using GridData=void*(__cdecl*)();
using GridFind=bool(__thiscall*)(void*,const std::uint8_t**,std::uint32_t,int,char);
bool Attach(const SdgoApi* api,SdgoMod* mod,const sdgo::unit_catalog::Catalog& catalog,GridData data,GridFind find);
}

#pragma pack(push,4)
// Diagnostics only (GameTest observation): monotonic counters, no pointers.
typedef struct SdgoUnitFormChainStateV1 {
    uint32_t size,version,installed,reserved;
    uint64_t swaps_seen,rebased,refused,models_created,models_released,units_restored;
    uint32_t last_unit_id,last_current,last_target,last_refusal; // last refusal: 0 none, 1 no record, 2 no model, 3 unmanaged
} SdgoUnitFormChainStateV1;
#pragma pack(pop)
typedef int (__cdecl* SdgoReadUnitFormChainStateV1Fn)(SdgoUnitFormChainStateV1*);
#ifdef __cplusplus
static_assert(sizeof(SdgoUnitFormChainStateV1)==80,"unit form chain state ABI changed");
#endif
