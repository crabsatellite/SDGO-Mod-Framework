#pragma once
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"
namespace sdgo::unit_stages {
bool Attach(const SdgoApi*,SdgoMod*,const unit_catalog::Catalog&,const wchar_t* directory=nullptr);
void Forget(void* unit);
}
