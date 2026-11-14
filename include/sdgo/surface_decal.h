#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sdgo::surface_decal {

struct Vec2 { double x = 0, y = 0; };
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Triangle { std::array<std::uint32_t, 3> vertex{}; };

// Positions belong to ONE rigid model node in its local coordinate system.
// Never pass a combined, animated world-space mesh here. The caller retains
// the node identity and applies its current native transform when rendering.
struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
};

struct Ray { Vec3 origin, direction; };
struct Hit {
    std::uint32_t triangle = 0;
    std::array<double, 3> barycentric{{1, 0, 0}};
    double distance = 0;
};

// Identity is supplied by the runtime's validated model/mesh contract. A
// triangle number alone is not stable across model replacements or LODs.
struct Anchor {
    std::uint64_t geometry = 0;
    std::uint32_t node = 0, triangle = 0;
    std::array<double, 3> barycentric{{1, 0, 0}};
    Vec3 tangent{1, 0, 0};
};
struct Placement {
    Anchor anchor;
    double width = 1, height = 1, rotation = 0, depth = 0.1;
};

struct Vertex {
    Vec3 position;
    Vec2 uv;
    std::uint32_t triangle = 0;
    std::array<double, 3> barycentric{};
};
struct Patch { std::vector<Vertex> vertices; }; // triangle list
struct Limits {
    std::size_t vertices = 262144, triangles = 262144, outputTriangles = 8192;
    double maxCoordinate = 1000000;
    double minSize = 0.0001, maxSize = 100000;
    double minNormalDot = 0.25;
};
enum class Result {
    ok, miss, invalidMesh, invalidRay, invalidPlacement,
    wrongGeometry, empty, budgetExceeded
};

// All functions leave output unchanged on failure. Pick uses a normalized
// direction internally, so Hit::distance is in model-node units.
Result Pick(const Mesh&, const Ray&, Hit&, const Limits& = {});
Result MakePlacement(const Mesh&, const Hit&, std::uint64_t geometry,
                     std::uint32_t node, Vec3 preferredTangent,
                     double width, double height, double depth,
                     Placement&, const Limits& = {});
Result Project(const Mesh&, std::uint64_t geometry, std::uint32_t node,
               const Placement&, Patch&, const Limits& = {});
bool Valid(const Placement&, const Limits& = {});

// Existing native item slots are retained. An absent placement means the
// original native fixed-slot renderer owns that sticker.
struct Slot {
    std::uint32_t sticker = 0;
    bool positioned = false;
    Placement placement;
};
struct Appearance {
    std::uint64_t unitSerial = 0, revision = 0;
    std::uint32_t unitType = 0;
    std::array<Slot, 3> slots{};
};
bool Valid(const Appearance&, const Limits& = {});
bool SameContent(const Appearance&, const Appearance&);

enum class EditState { closed, editing, awaitingReply, uncertain };
enum class Reply { saved, rejected, conflict };
class Editor {
public:
    bool Open(const Appearance&);
    bool Place(std::size_t slot, const Placement&);
    bool ResetPosition(std::size_t slot);
    bool Cancel();
    bool BeginSave(std::uint64_t request, Appearance& proposal);
    bool Acknowledge(std::uint64_t request, Reply, const Appearance& authority);
    void Timeout();
    // After a timeout only an authoritative refresh may unblock editing.
    bool Reconcile(const Appearance& authority);
    EditState State() const { return state_; }
    const Appearance& Preview() const { return draft_; }
    const Appearance& Saved() const { return saved_; }
    bool Dirty() const { return !SameContent(saved_, draft_); }
private:
    Appearance saved_{}, draft_{};
    EditState state_ = EditState::closed;
    std::uint64_t request_ = 0, lastRequest_ = 0;
};

} // namespace sdgo::surface_decal
