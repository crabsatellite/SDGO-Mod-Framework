#pragma once
#include "sdgo/surface_decal_protocol.h"

// Native isolated-test contract. No network endpoint or alternative player UI.
// Both exports require the active native game-test runtime on the game thread.
namespace sdgo::surface_decal::test {
struct Snapshot {
    std::uint32_t size=sizeof(Snapshot),version=1;
    std::uint32_t editing=0,placing=0,state=0,pending=0,selected=0;
    std::uint32_t freshViews=0,triangles=0,draws=0;
    std::uint32_t boundModels=0,positionedModels=0;
    std::uint32_t battleModels=0,battlePositioned=0,battleDraws=0;
    wire::Bytes saved{},preview{};
};
using Observe=int(__cdecl*)(Snapshot*);
using Place=int(__cdecl*)(double,double);
struct TargetSnapshot {
    std::uint32_t size=sizeof(TargetSnapshot),version=1;
    std::uint32_t battleModels=0,triangles=0,draws=0;
    std::uint32_t nodes=0,pendingGeometry=0,matchingStickers=0;
    wire::Bytes appearance{};
};
using Target=int(__cdecl*)(std::uint64_t,TargetSnapshot*);
struct RoomSnapshot {
    std::uint32_t size=sizeof(RoomSnapshot),version=1;
    std::uint32_t models=0,triangles=0,draws=0;
    std::uint32_t nodes=0,pendingGeometry=0,matchingStickers=0;
    wire::Bytes appearance{};
};
using RoomTarget=int(__cdecl*)(std::uint64_t,RoomSnapshot*);
inline constexpr char roomExport[]="SdgoSurfaceDecalTestRoomV1";
inline constexpr char targetExport[]="SdgoSurfaceDecalTestTargetV1";
inline constexpr char observeExport[]="SdgoSurfaceDecalTestObserveV1";
inline constexpr char placeExport[]="SdgoSurfaceDecalTestPlaceV1";
}
