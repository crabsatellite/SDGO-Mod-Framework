#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../mods/battle_ai/native_map_transport.h"
#include "../mods/battle_ai/native_transport_catalog.h"

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

void TestPublicMapDiscovery() {
    using battle_ai::native::TransportVolume;
    using battle_ai::native::TransportCatalog;
    Fixture fixture;
    Check(battle_ai::native::TransportEntranceWaitMs(false)==200&&
          battle_ai::native::TransportEntranceWaitMs(true)==battle_ai::native::kMapTransportRequestTimeoutMs,
          "only an actual outstanding native request receives the bridge's full notification deadline");
    Check(battle_ai::native::TransportMadeProgress({0,0,0},{101,0,0},{2,0,0})&&
          !battle_ai::native::TransportMadeProgress({0,0,0},{100,0,0},{1,0,0})&&
          !battle_ai::native::TransportMadeProgress({0,0,0},{0,0,200},{1,0,0})&&
          !battle_ai::native::TransportMadeProgress({0,0,0},{NAN,0,0},{1,0,0}),
          "native arrival acceptance requires finite progress beyond the launch threshold along the authored direction");
    std::array<uint8_t,0x20> sender{};uint32_t connection=99;
    Check(battle_ai::native::ReadForceMoveConnection(sender.data(),connection)&&connection==0,
          "a native zero connection value is preserved rather than rejected as a null memory pointer");
    Put(sender.data(),0x14,uint32_t(17));
    Check(battle_ai::native::ReadForceMoveConnection(sender.data(),connection)&&connection==17,
          "an opaque native connection handle needs no readable memory allocation at its value");
    Check(!battle_ai::native::ReadForceMoveConnection(nullptr,connection),
          "connection decoding still rejects an unavailable sender object");
    const auto original=fixture.scenes[1].object;
    TransportVolume volume{};
    Check(TransportVolume::Read(fixture.scenes[1].bytes.data(),volume)&&volume.Live()&&
          volume.object==fixture.scenes[1].object.data()&&volume.speed==40&&volume.updates==35,
          "map discovery reads a live registered native force volume without changing its data");
    Check(fixture.scenes[1].object==original&&fixture.submitCalls==0,
          "discovering a public entrance neither sends a packet nor moves a unit");
    Vec3 exit{};
    Check(battle_ai::native::PredictTransportExit(volume,{0,400,0},exit)&&
          std::abs(exit.x-1292.4f)<0.01f&&exit.y==400&&exit.z==0,
          "horizontal route forecasting includes native per-tick damping and the terminal update");
    auto vertical=volume;vertical.direction={0,1,0};
    Check(battle_ai::native::PredictTransportExit(vertical,{10,400,20},exit)&&exit.x==10&&exit.z==20&&
          std::abs(exit.y-1692.4f)<0.01f,
          "native JumpFSM force movement contributes real vertical displacement to route forecasting");
    Check(battle_ai::native::TransportReachedRouteProgress(
              {0,300,0},{0,1180,0},{0,1160,0},vertical,42)&&
          !battle_ai::native::TransportReachedRouteProgress(
              {0,300,0},{0,600,0},{0,1160,0},vertical,42)&&
          !battle_ai::native::TransportReachedRouteProgress(
              {0,300,0},{500,300,0},{0,1160,0},vertical,42),
          "route quality separately requires native force travel to reach the planned exit height");
    Check(battle_ai::native::TransportAtRouteExit({20,1170,10},{0,1160,0},42)&&
          !battle_ai::native::TransportAtRouteExit({100,1170,10},{0,1160,0},42)&&
          !battle_ai::native::TransportAtRouteExit({20,1300,10},{0,1160,0},42),
          "landing completion requires both horizontal support proximity and the measured support height");
    vertical.speed=100;vertical.updates=300;
    Check(battle_ai::native::PredictTransportExit(vertical,{0,0,0},exit)&&std::abs(exit.y-27081)<0.01f,
          "the complete authored speed and duration envelope includes long and fast launchers");
    vertical.updates=301;
    Check(!battle_ai::native::PredictTransportExit(vertical,{0,0,0},exit),
          "transport forecasts still reject motion outside the verified authored envelope");
    fixture.scenes[1].object[0x1C]=1;
    Check(!volume.Live(),"removed map volumes cannot retain a planned transport link");
    fixture.scenes[1].object[0x1C]=0;
    Put(fixture.scenes[1].bytes.data(),0x88,Vec3{NAN,0,0});
    Check(!volume.Live(),"invalid live entrance transforms invalidate the old planning snapshot");
    Put(fixture.scenes[1].bytes.data(),0x88,Vec3{});
    const Aabb map{{-1000,-1000,-1000},{1000,1000,1000}};
    TransportCatalog catalog;
    fixture.reportedCount=128;
    auto api=fixture.Api();catalog.Pump(api.query,fixture.document.data(),map,0);
    Check(catalog.Volumes().empty()&&!catalog.Complete()&&catalog.Queries()==1,
          "a truncated public map query is subdivided instead of accepting a partial inventory");
    fixture.Results({0,1});catalog.Pump(api.query,fixture.document.data(),map,1);
    Check(catalog.Queries()==1,"discovery cannot repeat a native scene query in the same engine interval");
    catalog.Pump(api.query,fixture.document.data(),map,16);
    const auto revision=catalog.Revision();
    for(uint64_t tick=32;tick<=64;tick+=16)catalog.Pump(api.query,fixture.document.data(),map,tick);
    Check(catalog.Complete()&&catalog.Volumes().size()==1&&catalog.Revision()==revision,
          "overlapping spatial queries deduplicate a native entrance while completing the map inventory");
    catalog.Clear();Check(catalog.Volumes().empty()&&!catalog.Complete()&&catalog.Revision()>revision,
          "map lifecycle reset drops all previous entrance pointers and advances discovery identity");
    fixture.reportedCount=-1;catalog.Pump(api.query,fixture.document.data(),map,80);
    Check(!catalog.Complete()&&catalog.Saturated()==1,
          "a failed native query cannot turn an unexamined region into completed map discovery");
}

void TestRequestLease() {
    using namespace battle_ai::native;
    const auto Snapshot=[](
        uint32_t generation,uint32_t request,uint32_t notice) {
        TransportRequestStatus value{};
        value.available=true;
        value.generation=generation;
        value.requestCount=request;
        value.noticeCount=notice;
        return value;
    };
    TransportRequestStatus status{};
    status.available=true;status.generation=7;
    status.requestCount=10;status.noticeCount=20;
    TransportRequestLease lease{};
    BeginTransportRequestLease(lease,status);
    status.requestCount=11;status.outstanding=true;
    status.outstandingGeneration=7;
    Check(ObserveTransportRequestLease(lease,status,false,100)==
              TransportRequestUpdate::Idle&&!lease.committed,
          "another pad's outstanding request cannot claim a planned route without selected-pad overlap");

    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    Check(ObserveTransportRequestLease(
              lease,Snapshot(7,10,20),true,100)==
              TransportRequestUpdate::Idle&&!lease.committed,
          "selected-pad overlap alone does not manufacture a submitted request");
    status={};status.available=true;status.generation=7;
    status.requestCount=11;status.noticeCount=20;
    status.outstanding=true;status.outstandingGeneration=7;
    Check(ObserveTransportRequestLease(lease,status,false,150)==
              TransportRequestUpdate::Committed&&lease.committed&&
              lease.requestCount==11&&lease.committedAt==150,
          "a new exact-generation request remains associated with the selected pad across the next engine frames");
    status.outstanding=false;status.noticeCount=21;
    Check(ObserveTransportRequestLease(lease,status,false,180)==
              TransportRequestUpdate::Acknowledged&&lease.acknowledged,
          "the correlated lease survives request clearing until the matching native notice can start movement");

    ++status.timeoutCount;
    Check(ObserveTransportRequestLease(lease,status,false,200)==
              TransportRequestUpdate::Failed,
          "a timeout counter increase terminates the exact submitted route lease");

    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    status={};status.available=true;status.generation=8;
    Check(ObserveTransportRequestLease(lease,status,true,100)==
              TransportRequestUpdate::GenerationChanged,
          "a bot generation reset cannot inherit an earlier pad selection");

    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    ObserveTransportRequestLease(
        lease,Snapshot(7,10,20),true,100);
    Check(TransportRequestAdmissionOpen(lease,100)&&
          TransportRequestAdmissionOpen(
              lease,100+kTransportRequestAssociationWindowMs)&&
          !TransportRequestAdmissionOpen(
              lease,100+kTransportRequestAssociationWindowMs+1),
          "selected entrance contact opens one bounded admission window");
    ObserveTransportRequestLease(
        lease,Snapshot(7,10,20),true,
        100+kTransportRequestAssociationWindowMs);
    Check(!TransportRequestAdmissionOpen(
              lease,100+kTransportRequestAssociationWindowMs+1),
          "continued overlap cannot extend a stale selected entrance admission");
    status={};status.available=true;status.generation=7;
    status.requestCount=11;status.noticeCount=21;
    Check(ObserveTransportRequestLease(
              lease,status,false,
              100+kTransportRequestAssociationWindowMs+1)==
              TransportRequestUpdate::Idle&&!lease.committed,
          "a later incidental request cannot reuse stale selected-pad contact");

    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    ObserveTransportRequestLease(
        lease,Snapshot(7,10,20),true,0);
    status=Snapshot(7,11,20);
    status.outstanding=true;
    status.outstandingGeneration=7;
    Check(ObserveTransportRequestLease(
              lease,status,false,50)==
              TransportRequestUpdate::Committed,
          "selected-pad contact at process tick zero remains a valid bounded association");

    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    status=Snapshot(7,11,21);
    status.exactWitness=true;
    status.exactPayload=true;status.requestTick=0xfffffff0u;
    status.snapshotTick=0x20u;
    Check(ObserveTransportRequestLease(lease,status,false,500)==
              TransportRequestUpdate::Acknowledged&&lease.committed&&
              lease.acknowledged&&lease.requestCount==11,
          "an exact V2 payload associates an ACK-cleared request without overlap and handles DWORD tick wrap");
    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    status.exactPayload=false;
    Check(ObserveTransportRequestLease(lease,status,false,500)==
              TransportRequestUpdate::Idle&&!lease.committed,
          "a mismatched V2 payload cannot claim the selected route after ACK clearing");
    Check(ObserveTransportRequestLease(lease,status,true,501)==
              TransportRequestUpdate::Idle&&!lease.committed,
          "selected-pad overlap cannot override a mismatched exact V2 payload");
    status.exactPayload=true;status.snapshotTick=
        status.requestTick+kMapTransportRequestTimeoutMs+1;
    Check(ObserveTransportRequestLease(lease,status,false,500)==
              TransportRequestUpdate::Idle&&!lease.committed,
          "a stale exact payload cannot be reused by a later planned route");

    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    status=Snapshot(7,11,21);
    status.exactWitness=true;
    status.exactPayload=true;
    status.exactFailure=true;
    status.requestTick=100;
    status.snapshotTick=120;
    Check(ObserveTransportRequestLease(
              lease,status,false,500)==
              TransportRequestUpdate::Failed,
          "a V2 timeout or ownership-loss clear rejects the exact current request");
    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    Check(ObserveTransportRequestLease(
              lease,status,false,500)==
              TransportRequestUpdate::Failed&&!lease.committed,
          "a fresh exact V2 failure rejects its selected route before an outstanding or notice witness can appear");
    BeginTransportRequestLease(lease,Snapshot(7,10,20));
    status=Snapshot(7,11,20);
    status.exactWitness=true;status.exactPayload=true;
    status.outstanding=true;status.outstandingGeneration=7;
    status.requestTick=100;status.snapshotTick=120;
    Check(ObserveTransportRequestLease(
              lease,status,false,500)==
              TransportRequestUpdate::Committed&&lease.committed,
          "a fresh exact V2 outstanding request commits its own request tick");
    status.exactPayload=false;status.exactFailure=true;
    Check(ObserveTransportRequestLease(
              lease,status,true,516)==
              TransportRequestUpdate::Committed&&lease.committed,
          "a later mismatched V2 failure cannot terminate an already committed exact route");

    Check(TransportAboveRouteExit({0,800,0},{0,400,0},50)&&
          !TransportAboveRouteExit({100,800,0},{0,400,0},50)&&
          TransportAtRouteExit({25,425,0},{0,400,0},50),
          "passive post-Force landing requires support directly below the native trajectory");
    const Aabb map31PadBounds{
        {-77.45846f,-112.43821f,-67.61916f},
        {77.45846f,112.43821f,67.61916f}};
    TransportVolume map31West{};map31West.bounds=map31PadBounds;
    map31West.speed=40;map31West.updates=30;
    map31West.direction={-0.72070497f,0.69322199f,-0.005237f};
    Vec3 map31WestPredicted{};
    TransportVolume map31East=map31West;
    map31East.direction={0.64037299f,0.76775301f,0.02186f};
    Vec3 map31EastPredicted{};
    Check(PredictTransportExit(
              map31West,{1484.01184f,1.7f,1038.77234f},
              map31WestPredicted)&&
          PredictTransportExit(
              map31East,{-1598.77954f,1.7f,-812.63953f},
              map31EastPredicted)&&
          std::hypot(
              double(map31WestPredicted.x-1484.01184f),
              double(map31WestPredicted.z-1038.77234f))>800&&
          std::hypot(
              double(map31EastPredicted.x+1598.77954f),
              double(map31EastPredicted.z+812.63953f))>712&&
          TransportGroundExitUseful(
              map31West,{1484.01184f,1.7f,1038.77234f},
              map31WestPredicted,
              {map31WestPredicted.x,3.3f,map31WestPredicted.z},90)&&
          TransportGroundExitUseful(
              map31East,{-1598.77954f,1.7f,-812.63953f},
              map31EastPredicted,
              {map31EastPredicted.x,3.3f,map31EastPredicted.z},90),
          "Map31 diagonal launchers keep their 712-800 unit horizontal navigation benefit at ground height");

    TransportVolume vertical=map31West;vertical.direction={0,1,0};
    Vec3 verticalPredicted{};
    Check(PredictTransportExit(vertical,{0,1.7f,0},verticalPredicted)&&
          !TransportGroundExitUseful(
              vertical,{0,1.7f,0},verticalPredicted,
              {0,3.3f,0},90)&&
          TransportGroundExitUseful(
              vertical,{0,320,0},verticalPredicted,
              {0,1168.3f,0},90),
          "a pure-Y launcher needs a measured higher layer because it has no horizontal route gain");
    TransportVolume horizontal=map31West;horizontal.direction={1,0,0};
    Vec3 horizontalPredicted{};
    Check(PredictTransportExit(horizontal,{0,200,0},horizontalPredicted)&&
          TransportGroundExitUseful(
              horizontal,{0,200,0},horizontalPredicted,
              {horizontalPredicted.x,0,horizontalPredicted.z},50),
          "horizontal launchers remain useful without an elevation gain");
    Check(TransportPayloadMatches(
              horizontal,7001,true,true,7001,9001,9001,
              horizontal.speed,horizontal.updates,
              horizontal.direction)&&
          !TransportPayloadMatches(
              horizontal,7001,true,true,7002,9001,9001,
              horizontal.speed,horizontal.updates,
              horizontal.direction)&&
          !TransportPayloadMatches(
              horizontal,7001,true,false,0,9001,9001,
              horizontal.speed+1,horizontal.updates,
              horizontal.direction),
          "V2 association requires the exact pilot and payload, plus the exact pad object whenever its ID is available");

    const auto terminal=TransportForceTerminalAirState(
        vertical,{10,100,20});
    Check(std::abs(terminal.velocity.x)<0.0001f&&
          std::abs(terminal.velocity.y-
                   vertical.speed*0.8999999761581421f)<0.0001f&&
          std::abs(terminal.velocity.z)<0.0001f,
          "post-Force planning starts from native terminal Move damping and retained Jump Y velocity");
    Check(TransportOrdinaryGravityProfileAllowed(
              10001,0,0,0,0,0,0,0,0,2,1)&&
          !TransportOrdinaryGravityProfileAllowed(
              0x4290,0,0,0,0,0,0,0,0,2,1)&&
          !TransportOrdinaryGravityProfileAllowed(
              10001,1,0,0,0,0,0,0,0,2,1)&&
          !TransportOrdinaryGravityProfileAllowed(
              10001,0,0,0,0,0,0,0x1000,0,2,1)&&
          !TransportOrdinaryGravityProfileAllowed(
              10001,0,0,0,0,0,0,0,1,2,1),
          "ordinary landing continuation admits only the exact native default gravity profile");
    Check(TransportOrdinaryGoalModeAllowed(1)&&
          !TransportOrdinaryGoalModeAllowed(2),
          "ordinary post-Force steering is restricted to native goal mode 1 and never assumes the mode-2 0.8 branch");

    TransportOrdinaryAirState air{{0,100,0},{2,10,-1}};
    Check(AdvanceTransportOrdinaryAir(air,{1,0,0},1,2)&&
          std::abs(air.velocity.x-2.20000005f)<0.0001f&&
          std::abs(air.velocity.y-9.40000057f)<0.0001f&&
          std::abs(air.velocity.z+0.60000002f)<0.0001f&&
          std::abs(air.position.x-2.20000005f)<0.0001f&&
          std::abs(air.position.y-109.400001f)<0.0001f,
          "ordinary post-Force motion matches native 0.6 run retention and 0.98Y-0.2D10 gravity");
    const auto rejectedModeState=air;
    Check(!AdvanceTransportOrdinaryAir(
              air,{1,0,0},1,2,2)&&
          air.position.x==rejectedModeState.position.x&&
          air.position.y==rejectedModeState.position.y&&
          air.velocity.x==rejectedModeState.velocity.x,
          "the forecast fails closed before mutating state for a non-3A movement mode");
    const auto reachable=PredictTransportOrdinaryAirArrival(
        {{0,100,0},{0,12,0}},{25,100,0},2,2);
    const auto unreachable=PredictTransportOrdinaryAirArrival(
        {{0,100,0},{0,12,0}},{2500,100,0},2,2);
    Check(reachable.reachable&&reachable.arrivalUpdate>0&&
          reachable.updates>=reachable.arrivalUpdate&&
          reachable.peakY>100&&!unreachable.reachable,
          "measured elevated support is admitted only inside the native recurrence flight window");
    const auto braking=PredictTransportOrdinaryAirArrival(
        {{0,100,0},{2,12,0}},{25,100,0},2,2,true);
    Check(braking.reachable&&!braking.path.empty()&&
          std::abs(braking.path.front().x)<0.001f&&
          std::abs(braking.path.back().x-25)<=20.001f&&
          std::abs(braking.path.back().y-100)<0.001f,
          "arrival prediction retains native zero-command XZ damping and clips the last collision sweep at the measured landing plane");
    Vec3 crossing{};
    Check(TransportLandingCrossing(
              {0,110,0},{10,90,20},100,crossing)&&
          crossing.x==5&&crossing.y==100&&crossing.z==10&&
          !TransportLandingCrossing(
              {0,90,0},{10,80,20},100,crossing),
          "landing collision clipping preserves the real swept segment and rejects a segment already below support");
}

} // namespace

int main() {
    TestLayouts();
    TestFailClosedGates();
    TestRegisteredOverlapAndDeathZoneIsolation();
    TestPayloadAuthorityAndSubmission();
    TestPublicMapDiscovery();
    TestRequestLease();
    if (failures != 0) {
        std::printf("%d battle AI map transport tests failed\n", failures);
        return 1;
    }
    std::printf("all battle AI map transport tests passed\n");
    return 0;
}
