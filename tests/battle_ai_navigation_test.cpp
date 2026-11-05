#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../mods/battle_ai/native_navigation.h"

namespace {

using battle_ai::Vec3;
using battle_ai::native::Aabb;
using battle_ai::native::MotionSafety;
using battle_ai::native::NavigationApi;
using battle_ai::native::Ray3;

constexpr std::size_t kDocumentQuadTree = 0x82C4;
constexpr std::size_t kOwnerScene = 0x34;
constexpr std::size_t kSceneBackPointer = 0x0C;
constexpr std::size_t kObjectId = 0x04;
constexpr std::size_t kObjectTeam = 0x20;
constexpr std::size_t kSceneAabb = 0xBC;
constexpr std::size_t kObbCount = 0xD4;
constexpr std::size_t kObbArray = 0xD8;
constexpr std::size_t kSphereCount = 0xDC;
constexpr std::size_t kSphereArray = 0xE0;
constexpr std::size_t kSphereEnabled = 0x115;
constexpr int kCapacity = 1024;

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

template <typename T>
void Put(std::uint8_t* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

template <typename T>
void* Address(T method) {
    static_assert(sizeof(T) == sizeof(void*),
                  "fixture requires the Win32 single-inheritance member ABI");
    void* result = nullptr;
    std::memcpy(&result, &method, sizeof(result));
    return result;
}

struct SceneStorage {
    std::array<std::uint8_t, 0x180> bytes{};
    std::array<std::uint8_t, 0x108> obbs{};
    std::array<std::uint8_t, 0x40> spheres{};
    std::array<std::uint8_t, 0x10> object{};
};

struct Fixture;
Fixture* active = nullptr;

struct Callbacks {
    __declspec(noinline) void Query(void** output, int* count, int capacity,
                                    const Aabb*);
    __declspec(noinline) bool IsA(int kind);
    __declspec(noinline) bool ObjectIsA(int kind);
    __declspec(noinline) bool TerrainRay(const Ray3*, float* distance);
};

struct Fixture {
    std::array<std::uint8_t, 0x8300> document{};
    std::array<std::uint8_t, 0x40> owner{};
    std::array<void*, 6> sceneVtable{};
    std::array<void*, 2> objectVtable{};
    Callbacks quadtree{};
    std::array<SceneStorage, 4> scenes{};
    std::array<void*, kCapacity> results{};
    int reportedCount = 0;
    int queryCalls = 0;
    int segmentCalls = 0;
    int terrainCalls = 0;
    int obbCalls = 0;
    int sphereCalls = 0;
    std::uint8_t* terrainScene = nullptr;
    std::uint8_t* solidScene = nullptr;
    std::uint8_t* lightScene = nullptr;
    std::uint8_t* effectScene = nullptr;
    bool terrainHit = false;
    float terrainDistance = 0.0f;

    Fixture() {
        active = this;
        sceneVtable[5] = Address(&Callbacks::IsA);
        objectVtable[1] = Address(&Callbacks::ObjectIsA);
        Put(document.data(), kDocumentQuadTree,
            static_cast<void*>(&quadtree));
        Put(owner.data(), kOwnerScene,
            static_cast<void*>(scenes[0].bytes.data()));
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            ConfigureScene(i, static_cast<std::uint32_t>(100 + i));
        }
    }

    void ConfigureScene(std::size_t index, std::uint32_t objectId) {
        auto& scene = scenes[index];
        scene.bytes.fill(0);
        scene.obbs.fill(0);
        scene.spheres.fill(0);
        scene.object.fill(0);
        Put(scene.bytes.data(), 0, sceneVtable.data());
        Put(scene.bytes.data(), kSceneBackPointer, scene.object.data());
        Put(scene.object.data(), 0, objectVtable.data());
        Put(scene.object.data(), kObjectId, objectId);
        Put(scene.bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {11.0f, 1.0f, 1.0f}});
        Put(scene.bytes.data(), kObbCount, 0);
        Put(scene.bytes.data(), kObbArray,
            static_cast<std::uint8_t*>(nullptr));
        Put(scene.bytes.data(), kSphereCount, 0);
        Put(scene.bytes.data(), kSphereArray,
            static_cast<std::uint8_t*>(nullptr));
        scene.bytes[kSphereEnabled] = 0;
    }

    void Results(std::initializer_list<std::size_t> indices) {
        results.fill(nullptr);
        reportedCount = 0;
        for (const std::size_t index : indices) {
            results[reportedCount++] = scenes[index].bytes.data();
        }
    }

    void Obb(std::size_t index, std::uint8_t code) {
        auto& scene = scenes[index];
        scene.obbs[0] = code;
        Put(scene.bytes.data(), kObbCount, 1);
        Put(scene.bytes.data(), kObbArray, scene.obbs.data());
    }

    void Sphere(std::size_t index, std::uint8_t code) {
        auto& scene = scenes[index];
        scene.spheres[0] = code;
        Put(scene.bytes.data(), kSphereCount, 1);
        Put(scene.bytes.data(), kSphereArray, scene.spheres.data());
        scene.bytes[kSphereEnabled] = 1;
    }

    NavigationApi Api();
};

void Callbacks::Query(void** output, int* count, int capacity, const Aabb*) {
    ++active->queryCalls;
    const int requested = active->reportedCount;
    const int copied = requested > 0
        ? (std::min)(requested, capacity)
        : 0;
    for (int i = 0; i < copied; ++i) output[i] = active->results[i];
    *count = requested;
}

bool Callbacks::IsA(int kind) {
    if(kind==1) {
        const auto self=reinterpret_cast<std::uint8_t*>(this);
        return self==active->terrainScene||self==active->solidScene||self==active->lightScene||self==active->effectScene;
    }
    return (kind == 3 && active->terrainScene == reinterpret_cast<std::uint8_t*>(this)) ||
        (kind == 2 && active->solidScene == reinterpret_cast<std::uint8_t*>(this)) ||
        (kind == 4 && active->lightScene == reinterpret_cast<std::uint8_t*>(this));
}

bool Callbacks::ObjectIsA(int kind) {
    return kind == 0x22 && reinterpret_cast<std::uint8_t*>(this)[8] != 0;
}

bool Callbacks::TerrainRay(const Ray3*, float* distance) {
    ++active->terrainCalls;
    *distance = active->terrainDistance;
    return active->terrainHit;
}

__declspec(noinline) bool __cdecl SegmentAabb(const Vec3* center,
                                               const Vec3* halfDelta,
                                               const Aabb* bounds) {
    ++active->segmentCalls;
    const Vec3 from = *center - *halfDelta;
    const Vec3 delta = *halfDelta * 2.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    const auto slab = [&](float origin, float direction, float lower, float upper) {
        if (std::abs(direction) < 1.0e-6f) return origin >= lower && origin <= upper;
        float first = (lower - origin) / direction;
        float second = (upper - origin) / direction;
        if (first > second) (std::swap)(first, second);
        minimum = (std::max)(minimum, first);
        maximum = (std::min)(maximum, second);
        return minimum <= maximum;
    };
    return slab(from.x, delta.x, bounds->min.x, bounds->max.x) &&
           slab(from.y, delta.y, bounds->min.y, bounds->max.y) &&
           slab(from.z, delta.z, bounds->min.z, bounds->max.z);
}

float DistanceFor(std::uint8_t code) {
    switch (code) {
        case 1: return 4.0f;
        case 2: return 11.0f;
        case 3: return std::numeric_limits<float>::quiet_NaN();
        default: return 0.0f;
    }
}

__declspec(noinline) bool __cdecl RayObb(const Ray3*, const void* shape,
                                         float* distance) {
    ++active->obbCalls;
    const auto code = *static_cast<const std::uint8_t*>(shape);
    *distance = DistanceFor(code);
    return code != 0;
}

__declspec(noinline) bool __cdecl RaySphere(const Ray3*, const void* shape,
                                            float* distance) {
    ++active->sphereCalls;
    const auto code = *static_cast<const std::uint8_t*>(shape);
    *distance = DistanceFor(code);
    return code != 0;
}

NavigationApi Fixture::Api() {
    NavigationApi api{};
    api.query = reinterpret_cast<battle_ai::native::QueryFn>(
        Address(&Callbacks::Query));
    api.segmentAabb = &SegmentAabb;
    api.terrainRay = reinterpret_cast<battle_ai::native::TerrainRayFn>(
        Address(&Callbacks::TerrainRay));
    api.rayObb = &RayObb;
    api.raySphere = &RaySphere;
    return api;
}

bool Clear(Fixture& fixture, std::uint32_t targetId = 999,
           Vec3 from = {0.0f, 0.0f, 0.0f},
           Vec3 to = {10.0f, 0.0f, 0.0f},
           bool ignoreFriendly = false) {
    active = &fixture;
    return battle_ai::native::FreshLineClear(
        fixture.Api(), fixture.document.data(), fixture.owner.data(),
        targetId, from, to, ignoreFriendly);
}

MotionSafety Safety(Fixture& fixture, Vec3 destination,
                    Vec3 current = {0.0f, 0.0f, 0.0f}) {
    active = &fixture;
    return battle_ai::native::CheckMotionSafety(
        fixture.Api(), fixture.document.data(), fixture.owner.data(),
        current, destination);
}

void ConfigureDeathZone(Fixture& fixture, std::size_t index, Aabb bounds) {
    fixture.scenes[index].object[8] = 1;
    Put(fixture.scenes[index].bytes.data(), kSceneAabb, bounds);
}

void TestDeathZoneMotionSafety() {
    {
        Fixture fixture;
        Put(fixture.scenes[0].bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
        ConfigureDeathZone(fixture, 1,
            Aabb{{4.0f, -2.0f, -2.0f}, {6.0f, 2.0f, 2.0f}});
        fixture.Results({0, 1});
        Check(Safety(fixture, {5.0f, 0.0f, 0.0f}) == MotionSafety::Blocked,
              "a destination overlapping a DeathZone is rejected");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[0].bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
        ConfigureDeathZone(fixture, 1,
            Aabb{{4.0f, -2.0f, -2.0f}, {4.1f, 2.0f, 2.0f}});
        fixture.Results({1});
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Blocked,
              "a thin DeathZone crossed between clear endpoints is rejected");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[0].bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
        ConfigureDeathZone(fixture, 1,
            Aabb{{20.0f, -2.0f, -2.0f}, {21.0f, 2.0f, 2.0f}});
        fixture.Results({1});
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "movement with no DeathZone intersection remains available");
        fixture.scenes[1].object[8] = 0;
        Put(fixture.scenes[1].bytes.data(), kSceneAabb,
            Aabb{{4.0f, -2.0f, -2.0f}, {6.0f, 2.0f, 2.0f}});
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "an ordinary collision block is not treated as a DeathZone");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[0].bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
        Put(fixture.scenes[1].bytes.data(), kSceneBackPointer,
            static_cast<std::uint8_t*>(nullptr));
        fixture.terrainScene = fixture.scenes[1].bytes.data();
        fixture.Results({1});
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "a native terrain scene without an object backpointer does not stall motion");
        fixture.terrainScene = nullptr;
        fixture.solidScene = fixture.scenes[1].bytes.data();
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "a known static scene without a gameplay object is classified separately from death zones");
        fixture.solidScene = nullptr;
        fixture.lightScene = fixture.scenes[1].bytes.data();
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "an actual native light volume is not a movement or death-zone blocker");
        fixture.lightScene = nullptr;
        fixture.effectScene = fixture.scenes[1].bytes.data();
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "a native effect scene needs no subtype-specific exception in gameplay death-zone classification");
        fixture.effectScene = nullptr;
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Unavailable,
              "an objectless node without the native scene base still fails closed");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[0].bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
        ConfigureDeathZone(fixture, 1,
            Aabb{{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}});
        fixture.Results({1});
        Check(Safety(fixture, {4.0f, 0.0f, 0.0f}) == MotionSafety::Safe,
              "an owner already overlapping a DeathZone may leave it completely");
        Check(Safety(fixture, {1.0f, 0.0f, 0.0f}) == MotionSafety::Blocked,
              "an owner already overlapping a DeathZone may not remain inside it");
        ConfigureDeathZone(fixture, 2,
            Aabb{{2.5f, -2.0f, -2.0f}, {3.0f, 2.0f, 2.0f}});
        fixture.Results({1, 2});
        Check(Safety(fixture, {4.0f, 0.0f, 0.0f}) == MotionSafety::Blocked,
              "leaving an overlapping zone may not cross another DeathZone");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[0].bytes.data(), kSceneAabb,
            Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
        ConfigureDeathZone(fixture, 1,
            Aabb{{4.0f, -2.0f, -2.0f}, {6.0f, 2.0f, 2.0f}});
        fixture.Results({1});
        Put(fixture.owner.data(), 0x38, int{100});
        const auto alive = Safety(fixture, {10.0f, 0.0f, 0.0f});
        Put(fixture.owner.data(), 0x38, int{-100});
        const auto nonpositive = Safety(fixture, {10.0f, 0.0f, 0.0f});
        Check(alive == MotionSafety::Blocked && nonpositive == alive,
              "DeathZone movement safety does not depend on owner HP");
    }
    {
        Fixture fixture;
        fixture.reportedCount = kCapacity;
        fixture.results.fill(fixture.scenes[1].bytes.data());
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Unavailable,
              "a full native scene result reports unavailable motion safety");
        fixture.reportedCount = 1;
        fixture.results[0] = nullptr;
        Check(Safety(fixture, {10.0f, 0.0f, 0.0f}) == MotionSafety::Unavailable,
              "a malformed native scene result reports unavailable motion safety");
    }
}

void TestExcludedBodies() {
    Fixture fixture;
    fixture.Results({0, 1, 2});
    fixture.Obb(0, 1);
    fixture.Obb(1, 1);
    fixture.Obb(2, 1);
    Put(fixture.scenes[1].object.data(), kObjectId, std::uint32_t{42});
    Put(fixture.scenes[2].bytes.data(), kSceneBackPointer,
        fixture.owner.data());
    Check(Clear(fixture, 42),
          "owner scenes and selected-target collision bodies are excluded");
    Check(fixture.obbCalls == 0,
          "excluded bodies never reach primitive collision callbacks");
}

void TestPrimitiveBlockers() {
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.terrainScene = fixture.scenes[1].bytes.data();
        fixture.terrainHit = true;
        fixture.terrainDistance = 4.0f;
        Check(!Clear(fixture), "terrain hit within the segment blocks");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(!Clear(fixture), "OBB hit within the segment blocks");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.Sphere(1, 1);
        Check(!Clear(fixture), "sphere hit within the segment blocks");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.Obb(1, 2);
        fixture.Sphere(1, 2);
        Check(Clear(fixture),
              "primitive hits beyond the target endpoint do not block");
    }
}

void TestFriendlyFiltering() {
    {
        Fixture fixture;
        Put(fixture.owner.data(), kObjectTeam, int{1});
        Put(fixture.scenes[1].object.data(), kObjectTeam, int{1});
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(!Clear(fixture),
              "strict melee visibility keeps a friendly body as a blocker");
    }
    {
        Fixture fixture;
        Put(fixture.owner.data(), kObjectTeam, int{1});
        Put(fixture.scenes[1].object.data(), kObjectTeam, int{1});
        fixture.Results({1});
        fixture.terrainScene = fixture.scenes[1].bytes.data();
        fixture.terrainHit = true;
        fixture.terrainDistance = 4.0f;
        Check(!Clear(fixture, 999, {0.0f, 0.0f, 0.0f},
                     {10.0f, 0.0f, 0.0f}, true),
              "terrain remains a ranged blocker regardless of scene ownership");
    }
    {
        Fixture fixture;
        Put(fixture.owner.data(), kObjectTeam, int{1});
        Put(fixture.scenes[1].object.data(), kObjectTeam, int{1});
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(Clear(fixture, 999, {0.0f, 0.0f, 0.0f},
                    {10.0f, 0.0f, 0.0f}, true),
              "ranged visibility ignores a same-team collision body");
        Check(fixture.obbCalls == 0,
              "ignored ranged-friendly bodies never reach primitive tests");
    }
    for (const int specialTeam : {0, 5}) {
        Fixture fixture;
        Put(fixture.owner.data(), kObjectTeam, specialTeam);
        Put(fixture.scenes[1].object.data(), kObjectTeam, specialTeam);
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(!Clear(fixture, 999, {0.0f, 0.0f, 0.0f},
                     {10.0f, 0.0f, 0.0f}, true),
              specialTeam == 0
                  ? "neutral team zero remains a ranged collision blocker"
                  : "special team five remains a ranged collision blocker");
    }
    {
        Fixture fixture;
        Put(fixture.owner.data(), kObjectTeam, int{1});
        Put(fixture.scenes[1].object.data(), kObjectTeam, int{2});
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(!Clear(fixture, 999, {0.0f, 0.0f, 0.0f},
                     {10.0f, 0.0f, 0.0f}, true),
              "an enemy collision body remains a ranged blocker");
    }
    {
        Fixture fixture;
        Put(fixture.owner.data(), kObjectTeam, int{1});
        Put(fixture.scenes[1].object.data(), kObjectTeam, int{0});
        fixture.Results({1});
        fixture.Obb(1, 1);
        Check(!Clear(fixture, 999, {0.0f, 0.0f, 0.0f},
                     {10.0f, 0.0f, 0.0f}, true),
              "a physical wall remains a ranged blocker");
    }
}

void TestFreshDifferentTargets() {
    Fixture fixture;
    fixture.Results({1, 2});
    Put(fixture.scenes[1].object.data(), kObjectId, std::uint32_t{41});
    Put(fixture.scenes[2].object.data(), kObjectId, std::uint32_t{42});
    fixture.Obb(1, 1);
    fixture.Obb(2, 0);
    const bool target41 = Clear(fixture, 41);
    const bool target42 = Clear(fixture, 42);
    Check(target41 && !target42,
          "consecutive different-target queries produce independent results");
    Check(fixture.queryCalls == 2,
          "every line query invokes the quadtree without a two-second cache");
}

void TestCapacityAndMalformedData() {
    {
        Fixture fixture;
        fixture.reportedCount = kCapacity;
        fixture.results.fill(fixture.scenes[1].bytes.data());
        Check(!Clear(fixture),
              "a full 1024-result query is conservatively blocked");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        Put(fixture.scenes[1].bytes.data(), kObbCount, 1);
        Put(fixture.scenes[1].bytes.data(), kObbArray,
            static_cast<std::uint8_t*>(nullptr));
        Check(!Clear(fixture), "null OBB array with a positive count fails closed");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        Put(fixture.scenes[1].bytes.data(), kObbCount, 1);
        Put(fixture.scenes[1].bytes.data(), kObbArray,
            reinterpret_cast<std::uint8_t*>(1));
        Check(!Clear(fixture),
              "unreadable OBB storage is caught by the Win32 safety boundary");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        Put(fixture.scenes[1].bytes.data(), kObbCount, 4097);
        Put(fixture.scenes[1].bytes.data(), kObbArray,
            fixture.scenes[1].obbs.data());
        Check(!Clear(fixture), "excessive shape count fails closed");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        fixture.scenes[1].bytes[kSphereEnabled] = 1;
        Put(fixture.scenes[1].bytes.data(), kSphereCount, 1);
        Put(fixture.scenes[1].bytes.data(), kSphereArray,
            static_cast<std::uint8_t*>(nullptr));
        Check(!Clear(fixture),
              "null sphere array with a positive count fails closed");
    }
    {
        Fixture fixture;
        fixture.reportedCount = 1;
        fixture.results[0] = nullptr;
        Check(!Clear(fixture), "null query result fails closed");
    }
    {
        Fixture fixture;
        fixture.Results({1});
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Put(fixture.scenes[1].bytes.data(), kSceneAabb,
            Aabb{{nan, -1.0f, -1.0f}, {11.0f, 1.0f, 1.0f}});
        Check(!Clear(fixture), "NaN scene bounds fail closed");
    }
    {
        Fixture fixture;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(!Clear(fixture, 999, {nan, 0.0f, 0.0f},
                     {10.0f, 0.0f, 0.0f}),
              "NaN endpoints fail closed before the native query");
        Check(fixture.queryCalls == 0,
              "invalid endpoints do not reach native collision code");
    }
    {
        Fixture fixture;
        const float huge = (std::numeric_limits<float>::max)();
        Check(!Clear(fixture, 999, {huge, 0.0f, 0.0f},
                     {huge, 1.0f, 0.0f}),
              "overflowing segment midpoint fails closed");
    }
}

void TestInputsRemainUnchanged() {
    Fixture fixture;
    fixture.Results({1, 2, 3});
    fixture.terrainScene = fixture.scenes[1].bytes.data();
    fixture.Obb(2, 0);
    fixture.Sphere(3, 0);

    const auto document = fixture.document;
    const auto owner = fixture.owner;
    const auto scenes = fixture.scenes;
    Check(Clear(fixture), "mixed nonblocking scene query completes");
    bool sceneBytesEqual = true;
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        sceneBytesEqual =
            sceneBytesEqual &&
            scenes[i].bytes == fixture.scenes[i].bytes &&
            scenes[i].obbs == fixture.scenes[i].obbs &&
            scenes[i].spheres == fixture.scenes[i].spheres &&
            scenes[i].object == fixture.scenes[i].object;
    }
    Check(document == fixture.document && owner == fixture.owner &&
              sceneBytesEqual,
          "document, owner, scenes, objects, and shapes remain byte-identical");
}

}  // namespace

int main() {
    static_assert(sizeof(void*) == 4,
                  "GOnline navigation fixture must run as Win32");
    TestExcludedBodies();
    TestPrimitiveBlockers();
    TestFriendlyFiltering();
    TestFreshDifferentTargets();
    TestCapacityAndMalformedData();
    TestInputsRemainUnchanged();
    TestDeathZoneMotionSafety();
    std::printf("NAVIGATION WRAPPER CHECKS %s\n",
                failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
