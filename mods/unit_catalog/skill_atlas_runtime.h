#pragma once
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"

namespace sdgo::unit_skill_atlas {
using GridData=void*(__cdecl*)();
using GridFind=bool(__thiscall*)(void*,const std::uint8_t**,std::uint32_t,int,char);
struct Scope {std::wstring directory;const sdgo::unit_catalog::Catalog* catalog=nullptr;};
bool Attach(const SdgoApi* api,SdgoMod* mod,const sdgo::unit_catalog::Catalog& catalog,GridData data,GridFind find,
            const std::vector<Scope>& scopes);
}
