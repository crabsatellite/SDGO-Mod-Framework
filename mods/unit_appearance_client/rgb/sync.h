#pragma once
#include "sdgo/rgb_paint_protocol.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/rgb_paint_observer.h"
namespace sdgo::rgb_paint::sync {
using Saved=void(*)(std::uint64_t session,std::uint64_t unit,bool accepted,const Record* record);
bool Bind(const SdgoApi*,SdgoMod*);
void Pump();
void AbortLoad();
std::uint64_t Session();
bool ReadNative(const void*,Record&);
bool Current(std::uint64_t unit,Record&);
void Want(std::uint64_t unit,bool priority=false);
bool SaveRecord(const Record&,Saved);
void Preview(std::uint64_t unit,const Palette*);
void BindModel(void*,const Record&);
struct Observation{std::uint64_t revision=0;unsigned reads=0,draws=0,regions=0,matched=0,battleDraws=0,battleMatched=0;Palette colors{};};
bool Observe(std::uint64_t unit,Observation&);
bool ObserveScene(SdgoRgbPaintSceneObservationV1&);
}
