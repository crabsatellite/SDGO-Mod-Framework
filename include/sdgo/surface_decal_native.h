#pragma once
#include "sdgo/sdgo_api.h"
#include "sdgo/surface_decal.h"

namespace sdgo::surface_decal::native {

struct Node {
    std::uint32_t index = 0;
    std::uint64_t geometry = 0;
    Mesh mesh;
    std::vector<std::uint32_t> triangleGroups;
    std::array<float,16> world{};
};
enum class ReadResult { ok, unbound, wrongModel, malformed, unavailable, budgetExceeded };
class GeometryReader {
public:
    bool Bind(const SdgoApi*, SdgoMod*);
    // Call only during a native model's live, game-thread render/update scope.
    // Pointers and shared resource arrays are never retained or mutated.
    ReadResult Read(void* model, std::vector<Node>& output) const;
private:
    const void* modelVtable_ = nullptr;
    const void* modelObsVtable_ = nullptr;
    const void* rigidMeshVtable_ = nullptr;
    using World = const float*(__thiscall*)(void*);
    World world_ = nullptr;
};
} // namespace sdgo::surface_decal::native
