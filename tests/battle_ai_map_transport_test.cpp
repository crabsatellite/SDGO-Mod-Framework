#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../mods/battle_ai/native_map_transport.h"

namespace {

using battle_ai::Vec3;
using battle_ai::native::Aabb;
using battle_ai::native::ForceMoveRequest085A;
using battle_ai::native::MapTransportApi;
using battle_ai::native::MapTransportBounds;
using battle_ai::native::MapTransportResult;
using battle_ai::native::MapTransportWitness;

constexpr std::size_t kDocumentQuadTree = 0x82C4;
constexpr std::size_t kUnitScene = 0x34;
constexpr std::size_t kUnitHp = 0x38;
constexpr std::size_t kUnitPilotId = 0x2B68;
constexpr std::size_t kObjectScene = 0x08;
constexpr std::size_t kSceneObject = 0x0C;
constexpr std::size_t kSceneForward = 0x6C;
constexpr std::size_t kSceneBounds = 0xBC;
constexpr std::size_t kSceneObbArray = 0xD8;
constexpr std::size_t kSceneSphereArray = 0xE0;
constexpr std::size_t kPadDuration = 0x34;
constexpr std::size_t kPadSpeed = 0x38;

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

struct Fixture;
Fixture* active = nullptr;

struct Callbacks {
    __declspec(noinline) void Query(void** output, int* count, int capacity,
                                    const Aabb*);
    __declspec(noinline) bool SceneIsKind(int kind);
    __declspec(noinline) bool ObjectIsKind(int kind);
};

struct Scene {
    std::array<std::uint8_t, 0x180> bytes{};
    std::array<std::uint8_t, 0x40> object{};
    std::array<std::uint8_t, 0x20> shape{};
};

struct Fixture {
    std::array<std::uint8_t, 0x8300> document{};
    std::array<std::uint8_t, 0x2C00> unit{};
    std::array<void*, 6> sceneVtable{};
    std::array<void*, 2> objectVtable{};
    Callbacks quadtree{};
    std::array<Scene, 4> scenes{};
    std::array<void*, 128> results{};
    int reportedCount = 0;

    bool owned = true;
    bool action = true;
    bool control = true;
    bool forced = false;
    bool authority = true;
    bool submit = true;
    bool overlap = true;
    bool forceAfterAuthority = false;
    int queryCalls = 0;
    int authorityCalls = 0;
    int submitCalls = 0;
    ForceMoveRequest085A request{};
    const std::uint8_t* authorityUnit = nullptr;
    const std::uint8_t* authorityObject = nullptr;

    Fixture() {
        active = this;
        sceneVtable[5] = Address(&Callbacks::SceneIsKind);
        objectVtable[1] = Address(&Callbacks::ObjectIsKind);
        Put(document.data(), kDocumentQuadTree,
            reinterpret_cast<std::uint8_t*>(&quadtree));
        Put(unit.data(), kUnitScene, scenes[0].bytes.data());
        Put(unit.data(), kUnitHp, std::int32_t(1000));
        Put(unit.data(), kUnitPilotId, std::uint32_t(70001));

        for (std::size_t i = 0; i < scenes.size(); ++i) {
            auto& scene = scenes[i];
            Put(scene.bytes.data(), 0, sceneVtable.data());
            Put(scene.bytes.data(), kSceneObject, scene.object.data());
            Put(scene.bytes.data(), kSceneBounds,
                Aabb{{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}});
            Put(scene.bytes.data(), kSceneForward, Vec3{1.0f, 0.0f, 0.0f});
            Put(scene.bytes.data(), kSceneObbArray, scene.shape.data());
            Put(scene.bytes.data(), kSceneSphereArray, scene.shape.data());
            Put(scene.object.data(), 0, objectVtable.data());
            Put(scene.object.data(), kObjectScene, scene.bytes.data());
            scene.object[0x10] = 0;
        }
        Put(scenes[1].object.data(), kPadSpeed, 40.0f);
        Put(scenes[1].object.data(), kPadDuration, 35.0f);
        scenes[1].object[0x10] = 0x21;
        Results({0, 1});
    }

    void Results(std::initializer_list<std::size_t> indices) {
        results.fill(nullptr);
        reportedCount = 0;
        for (auto index : indices) results[reportedCount++] = scenes[index].bytes.data();
    }

    MapTransportApi Api();
};

void Callbacks::Query(void** output, int* count, int capacity, const Aabb*) {
    ++active->queryCalls;
    const int copied = (std::min)(capacity, active->reportedCount);
    for (int i = 0; i < copied; ++i) output[i] = active->results[i];
    *count = active->reportedCount;
}

bool Callbacks::SceneIsKind(int kind) {
    return kind == 7;
}

bool Callbacks::ObjectIsKind(int kind) {
    const auto* object = reinterpret_cast<const std::uint8_t*>(this);
    return kind == object[0x10];
}

bool Owned(void*, const std::uint8_t*) { return active->owned; }
bool Action(void*, const std::uint8_t*) { return active->action; }
bool Control(void*, const std::uint8_t*) { return active->control; }
bool Forced(void*, const std::uint8_t*) { return active->forced; }

bool __cdecl Overlap(const void*, const void*) {
    return active->overlap;
}

bool Authority(void*, const std::uint8_t* unit, const std::uint8_t* object,
               const ForceMoveRequest085A* request) {
    ++active->authorityCalls;
    active->authorityUnit = unit;
    active->authorityObject = object;
    active->request = *request;
    if (active->forceAfterAuthority) active->forced = true;
    return active->authority;
}

bool Submit(void*, const ForceMoveRequest085A* request) {
    ++active->submitCalls;
    active->request = *request;
    return active->submit;
}

MapTransportApi Fixture::Api() {
    MapTransportApi api{};
    api.query = reinterpret_cast<battle_ai::native::QueryFn>(
        Address(&Callbacks::Query));
    api.overlap = &Overlap;
    api.ownedLocalAi = &Owned;
    api.actionAllowed = &Action;
    api.controlAllowed = &Control;
    api.forcedMoving = &Forced;
    api.authority = &Authority;
    api.submit = &Submit;
    return api;
}

MapTransportBounds Bounds() {
    return {1.0f, 80.0f, 1.0f, 120.0f, 0.5f, 1.5f};
}

MapTransportResult Run(Fixture& fixture, MapTransportWitness* witness = nullptr,
                       MapTransportApi* overrideApi = nullptr,
                       MapTransportBounds bounds = Bounds()) {
    active = &fixture;
    const auto api = overrideApi ? *overrideApi : fixture.Api();
    return battle_ai::native::TryMapTransport(
        api, bounds, fixture.document.data(), fixture.unit.data(), witness);
}

void TestLayouts() {
    using battle_ai::native::ForceMoveObjectMessage;
    using battle_ai::native::ForceMoveWirePacket;

    ForceMoveWirePacket request{};
    request.opcode = battle_ai::native::kForceMoveRequestOpcode;
    request.pilotId = 0x11223344;
    request.speed = 40.0f;
    request.durationUpdates = 35.0f;
    request.direction = {1.0f, 0.0f, -1.0f};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&request);
    Check(sizeof(request) == 0x1E &&
              *reinterpret_cast<const std::uint16_t*>(bytes) == 0x085A &&
              *reinterpret_cast<const std::uint32_t*>(bytes + 2) == 0 &&
              *reinterpret_cast<const std::uint32_t*>(bytes + 6) == 0x11223344 &&
              *reinterpret_cast<const float*>(bytes + 0x0A) == 40.0f &&
              *reinterpret_cast<const float*>(bytes + 0x0E) == 35.0f,
          "REQ 0x085A has the verified packed 30-byte pilot and motion layout");

    ForceMoveObjectMessage message{};
    message.speed = request.speed;
    message.durationUpdates = request.durationUpdates;
    message.direction = request.direction;
    Check(sizeof(message) == 0x30 && message.type == 0x1E &&
              message.alignment == 0 && message.header04 == 0 &&
              message.header08 == 0 && message.header0C == 0 &&
              message.targetObjectId == 0 && message.delayUpdates == 0 &&
              message.effectGridId == 0,
          "ObjMsg 0x1E has the verified 48-byte zero-optional-field layout");
}

void TestFailClosedGates() {
    {
        Fixture fixture;
        auto api = fixture.Api();
        api.authority = nullptr;
        Check(Run(fixture, nullptr, &api) == MapTransportResult::InvalidContract &&
                  fixture.queryCalls == 0 && fixture.submitCalls == 0,
              "missing live room authority callback disables the bridge before scanning");
    }
    {
        Fixture fixture;
        fixture.owned = false;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::UnitRejected &&
                  witness.rejectedUnit == 1 && fixture.queryCalls == 0,
              "a unit outside the locally-owned PvP AI contract is rejected");
    }
    {
        Fixture fixture;
        Put(fixture.unit.data(), kUnitHp, std::int32_t(0));
        Check(Run(fixture) == MapTransportResult::UnitRejected &&
                  fixture.queryCalls == 0,
              "a dead owned AI cannot trigger forced movement");
    }
    {
        Fixture fixture;
        Put(fixture.unit.data(), kUnitPilotId, std::uint32_t(0));
        Check(Run(fixture) == MapTransportResult::UnitRejected &&
                  fixture.queryCalls == 0,
              "a zero native pilot ID cannot be placed in REQ 0x085A");
    }
    {
        Fixture fixture;
        fixture.action = false;
        Check(Run(fixture) == MapTransportResult::StateBlocked &&
                  fixture.queryCalls == 0,
              "the native action gate retains ownership");
    }
    {
        Fixture fixture;
        fixture.control = false;
        Check(Run(fixture) == MapTransportResult::StateBlocked &&
                  fixture.queryCalls == 0,
              "the native control gate retains ownership");
    }
    {
        Fixture fixture;
        fixture.forced = true;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::StateBlocked &&
                  witness.rejectedState == 1 && fixture.queryCalls == 0 &&
                  fixture.submitCalls == 0,
              "an already forced-moving AI cannot submit a duplicate trigger");
    }
}

void TestRegisteredOverlapAndDeathZoneIsolation() {
    {
        Fixture fixture;
        fixture.scenes[1].object[0x10] = 0x22;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::NoOverlap &&
                  witness.forcedMoveObjects == 0 && fixture.authorityCalls == 0 &&
                  fixture.submitCalls == 0,
              "a registered type 0x22 DeathZone remains outside map transport");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[1].object.data(), kObjectScene,
            fixture.scenes[2].bytes.data());
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::QueryUnavailable &&
                  witness.forcedMoveObjects == 1 &&
                  witness.rejectedQuery == 1 && fixture.submitCalls == 0,
              "a type 0x21 object not registered to the queried scene fails closed");
    }
    {
        Fixture fixture;
        fixture.overlap = false;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::NoOverlap &&
                  witness.forcedMoveObjects == 1 && witness.overlaps == 0 &&
                  fixture.authorityCalls == 0 && fixture.submitCalls == 0,
              "a nearby type 0x21 volume cannot trigger without exact shape overlap");
    }
    {
        Fixture fixture;
        fixture.reportedCount = 128;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::QueryUnavailable &&
                  witness.rejectedQuery == 1 && fixture.submitCalls == 0,
              "a full possibly-truncated 128-scene query fails closed");
    }
}

void TestPayloadAuthorityAndSubmission() {
    {
        Fixture fixture;
        Put(fixture.scenes[1].object.data(), kPadSpeed,
            std::numeric_limits<float>::quiet_NaN());
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::InvalidPayload &&
                  witness.overlaps == 1 && witness.rejectedPayload == 1 &&
                  fixture.authorityCalls == 0 && fixture.submitCalls == 0,
              "non-finite pad motion fields are rejected before authority or send");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[1].object.data(), kPadDuration, 121.0f);
        Check(Run(fixture) == MapTransportResult::InvalidPayload &&
                  fixture.submitCalls == 0,
              "pad duration must remain inside the caller's explicit update bounds");
    }
    {
        Fixture fixture;
        Put(fixture.scenes[1].bytes.data(), kSceneForward,
            Vec3{2.0f, 0.0f, 0.0f});
        Check(Run(fixture) == MapTransportResult::InvalidPayload &&
                  fixture.submitCalls == 0,
              "pad direction length must remain inside explicit finite bounds");
    }
    {
        Fixture fixture;
        fixture.authority = false;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::AuthorityRejected &&
                  witness.overlaps == 1 && witness.rejectedAuthority == 1 &&
                  fixture.authorityCalls == 1 && fixture.submitCalls == 0,
              "room/pilot authority rejection prevents the native request");
    }
    {
        Fixture fixture;
        fixture.forceAfterAuthority = true;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::StateBlocked &&
                  witness.authorityAccepted == 1 &&
                  witness.rejectedState == 1 && fixture.submitCalls == 0,
              "movement claimed during authority is rechecked before submit");
    }
    {
        Fixture fixture;
        fixture.submit = false;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::SubmitRejected &&
                  witness.authorityAccepted == 1 &&
                  witness.rejectedSubmit == 1 && fixture.submitCalls == 1,
              "native sender refusal is reported without manufacturing movement");
    }
    {
        Fixture fixture;
        MapTransportWitness witness{};
        Check(Run(fixture, &witness) == MapTransportResult::Submitted &&
                  fixture.queryCalls == 1 && fixture.authorityCalls == 1 &&
                  fixture.submitCalls == 1 && witness.attempts == 1 &&
                  witness.queries == 1 && witness.forcedMoveObjects == 1 &&
                  witness.overlaps == 1 && witness.authorityAccepted == 1 &&
                  witness.submitted == 1 &&
                  fixture.authorityUnit == fixture.unit.data() &&
                  fixture.authorityObject == fixture.scenes[1].object.data() &&
                  fixture.request.opcode == 0x085A &&
                  fixture.request.routeId == 0 &&
                  fixture.request.pilotId == 70001 &&
                  fixture.request.speed == 40.0f &&
                  fixture.request.durationUpdates == 35.0f &&
                  fixture.request.direction.x == 1.0f &&
                  fixture.request.direction.y == 0.0f &&
                  fixture.request.direction.z == 0.0f,
              "one owned overlapping pad submits one ordinary bounded REQ 0x085A");
    }
}

} // namespace

int main() {
    TestLayouts();
    TestFailClosedGates();
    TestRegisteredOverlapAndDeathZoneIsolation();
    TestPayloadAuthorityAndSubmission();
    if (failures != 0) {
        std::printf("%d battle AI map transport tests failed\n", failures);
        return 1;
    }
    std::printf("all battle AI map transport tests passed\n");
    return 0;
}
