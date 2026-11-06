#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "native_capabilities.h"
#include "native_navigation.h"

namespace battle_ai::native {

constexpr std::uint16_t kForceMoveRequestOpcode = 0x085A;
constexpr std::uint16_t kForceMoveNoticeOpcode = 0x0584;
constexpr std::uint16_t kForceMoveObjectMessageType = 0x001E;
constexpr int kForcedMoveObjectKind = 0x21;
constexpr int kCollisionSceneKind = 7;
constexpr uint32_t kMapTransportRequestTimeoutMs = 3000;

#pragma pack(push, 1)
// REQ 0x085A and NTF 0x0584 have the same wire layout. The request constructor
// at 0x00670020 writes the pilot ID at +0x06 and returns with RET 4. The
// notification handler at 0x004C3030 resolves that pilot ID before constructing
// ForceMoveObjectMessage.
struct ForceMoveWirePacket {
    std::uint16_t opcode = 0;
    // The client constructor writes zero. AgentServer replaces this with the
    // authenticated sender ID for REQ 0x085A; BattleServer replaces it with
    // each destination ID while fanning out NTF 0x0584.
    std::uint32_t routeId = 0;
    std::uint32_t pilotId = 0;
    float speed = 0.0f;
    float durationUpdates = 0.0f;
    Vec3 direction{};
};
#pragma pack(pop)

using ForceMoveRequest085A = ForceMoveWirePacket;
using ForceMoveNotice0584 = ForceMoveWirePacket;

// ObjMsg 0x1E is constructed at 0x004C3030 and consumed by 0x006DD5E0.
// Native code leaves the two alignment bytes at +0x02 unspecified; authored
// messages zero them. The three common header words and all optional
// target/delay/effect fields are zero in the map-volume path.
struct ForceMoveObjectMessage {
    std::uint16_t type = kForceMoveObjectMessageType;
    std::uint16_t alignment = 0;
    std::uint32_t header04 = 0;
    std::uint32_t header08 = 0;
    std::uint32_t header0C = 0;
    float speed = 0.0f;
    float durationUpdates = 0.0f;
    Vec3 direction{};
    std::uint32_t targetObjectId = 0;
    std::uint32_t delayUpdates = 0;
    std::uint32_t effectGridId = 0;
};

static_assert(sizeof(ForceMoveWirePacket) == 0x1E,
              "GOnline force-move wire ABI changed");
static_assert(offsetof(ForceMoveWirePacket, routeId) == 0x02,
              "GOnline force-move route field moved");
static_assert(offsetof(ForceMoveWirePacket, pilotId) == 0x06,
              "GOnline force-move pilot field moved");
static_assert(offsetof(ForceMoveWirePacket, speed) == 0x0A,
              "GOnline force-move speed field moved");
static_assert(offsetof(ForceMoveWirePacket, durationUpdates) == 0x0E,
              "GOnline force-move duration field moved");
static_assert(offsetof(ForceMoveWirePacket, direction) == 0x12,
              "GOnline force-move direction field moved");

static_assert(sizeof(ForceMoveObjectMessage) == 0x30,
              "GOnline ObjMsg 0x1E ABI changed");
static_assert(offsetof(ForceMoveObjectMessage, speed) == 0x10,
              "GOnline ObjMsg 0x1E speed field moved");
static_assert(offsetof(ForceMoveObjectMessage, durationUpdates) == 0x14,
              "GOnline ObjMsg 0x1E duration field moved");
static_assert(offsetof(ForceMoveObjectMessage, direction) == 0x18,
              "GOnline ObjMsg 0x1E direction field moved");
static_assert(offsetof(ForceMoveObjectMessage, targetObjectId) == 0x24,
              "GOnline ObjMsg 0x1E target field moved");
static_assert(offsetof(ForceMoveObjectMessage, delayUpdates) == 0x28,
              "GOnline ObjMsg 0x1E delay field moved");
static_assert(offsetof(ForceMoveObjectMessage, effectGridId) == 0x2C,
              "GOnline ObjMsg 0x1E effect field moved");

// Manifest-verified client ABI aliases. ApplyForceMoveObjectMessageFn is
// intentionally not used by TryMapTransport: the supported bridge follows the
// ordinary 0x085A request -> 0x0584 notification -> ObjMsg 0x1E path.
using ForceMovePacketConstructorFn =
    ForceMoveWirePacket*(__thiscall*)(ForceMoveWirePacket* packet,
                                     std::uint32_t pilotId);
using SendForceMovePacketFn =
    int(__thiscall*)(void* sender, void* connection, const ForceMoveWirePacket* packet,
                     int size, std::uint8_t flag);
using ApplyForceMoveObjectMessageFn =
    void(__thiscall*)(void* movementController, const ForceMoveObjectMessage* message);
using ShapeOverlapFn = bool(__cdecl*)(const void* ownerShape, const void* padShape);

// Native 6D0C21 forwards this field to 5238A0 without dereferencing it. Zero
// and small channel/connection values are not readable-memory pointers.
inline bool ReadForceMoveConnection(const uint8_t* sender,uint32_t& connection) noexcept {
    return sender&&detail::Read(sender,0x14,connection);
}

struct MapTransportBounds {
    float minimumSpeed = 0.0f;
    float maximumSpeed = 0.0f;
    float minimumDurationUpdates = 0.0f;
    float maximumDurationUpdates = 0.0f;
    float minimumDirectionLength = 0.0f;
    float maximumDirectionLength = 0.0f;
};

// Complete authored TRN inventory for this client: native speed16..100 and
// duration10..300 source updates. Keep the same finite envelope in discovery
// and the authenticated overlap bridge; never clamp an authored trajectory.
inline constexpr MapTransportBounds kAuthoredMapTransportBounds{1,100,1,300,0.5f,1.5f};

using MapTransportUnitGateFn =
    bool (*)(void* context, const std::uint8_t* unit);
using MapTransportAuthorityFn =
    bool (*)(void* context, const std::uint8_t* unit,
             const std::uint8_t* forcedMoveObject,
             const ForceMoveRequest085A* request);
using MapTransportSubmitFn =
    bool (*)(void* context, const ForceMoveRequest085A* request);

struct MapTransportApi {
    QueryFn query = nullptr;
    ShapeOverlapFn overlap = nullptr;
    void* context = nullptr;

    // These gates must be live native observations. ownedLocalAi also owns the
    // PvP-bot and generation/session binding; actionAllowed and controlAllowed
    // retain the engine's action/HP/control authority.
    MapTransportUnitGateFn ownedLocalAi = nullptr;
    MapTransportUnitGateFn actionAllowed = nullptr;
    MapTransportUnitGateFn controlAllowed = nullptr;
    // This observation must include active movement plus any locally pending
    // or outstanding 0x085A request owned by the submit callback.
    MapTransportUnitGateFn forcedMoving = nullptr;

    // AgentServer authenticates REQ 0x085A by replacing routeId. BattleServer
    // accepts a different pilot only for the current room host, requires the
    // NPC high bit, and resolves that pilot in the room's native slot table.
    // This callback must observe that current room/session binding.
    MapTransportAuthorityFn authority = nullptr;
    // Submit must atomically claim its pending-request state and enqueue REQ
    // 0x085A through the native sender. It must not call 0x006DD5E0 or
    // manufacture NTF 0x0584 locally.
    MapTransportSubmitFn submit = nullptr;
};

struct MapTransportWitness {
    std::uint64_t attempts = 0;
    std::uint64_t queries = 0;
    std::uint64_t candidates = 0;
    std::uint64_t forcedMoveObjects = 0;
    std::uint64_t overlaps = 0;
    std::uint64_t authorityAccepted = 0;
    std::uint64_t submitted = 0;
    std::uint64_t rejectedUnit = 0;
    std::uint64_t rejectedState = 0;
    std::uint64_t rejectedQuery = 0;
    std::uint64_t rejectedPayload = 0;
    std::uint64_t rejectedAuthority = 0;
    std::uint64_t rejectedSubmit = 0;
};

enum class MapTransportResult : std::uint8_t {
    InvalidContract,
    UnitRejected,
    StateBlocked,
    QueryUnavailable,
    NoOverlap,
    InvalidPayload,
    AuthorityRejected,
    SubmitRejected,
    Submitted
};

namespace map_transport_detail {

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
constexpr int kSceneCapacity = 128;

using SceneIsKindFn = bool(__thiscall*)(const void* scene, int kind);
using ObjectIsKindFn = bool(__thiscall*)(const void* object, int kind);

inline bool ValidBounds(const MapTransportBounds& bounds) noexcept {
    return std::isfinite(bounds.minimumSpeed) &&
           std::isfinite(bounds.maximumSpeed) &&
           std::isfinite(bounds.minimumDurationUpdates) &&
           std::isfinite(bounds.maximumDurationUpdates) &&
           std::isfinite(bounds.minimumDirectionLength) &&
           std::isfinite(bounds.maximumDirectionLength) &&
           bounds.minimumSpeed > 0.0f &&
           bounds.minimumSpeed <= bounds.maximumSpeed &&
           bounds.minimumDurationUpdates > 0.0f &&
           bounds.minimumDurationUpdates <= bounds.maximumDurationUpdates &&
           bounds.minimumDirectionLength > 0.0f &&
           bounds.minimumDirectionLength <= bounds.maximumDirectionLength;
}

inline bool IsKind(const std::uint8_t* object, std::size_t slot, int kind) noexcept {
    void** vtable = nullptr;
    if (!detail::Copy(object, &vtable, sizeof(vtable)) || !vtable ||
        slot > ((std::numeric_limits<std::size_t>::max)() / sizeof(void*)) ||
        !detail::Readable(vtable + slot, sizeof(void*))) {
        return false;
    }
    void* entry = nullptr;
    if (!detail::Copy(vtable + slot, &entry, sizeof(entry)) || !entry) return false;
    if (slot == 5) {
        return reinterpret_cast<SceneIsKindFn>(entry)(object, kind);
    }
    return reinterpret_cast<ObjectIsKindFn>(entry)(object, kind);
}

inline bool ValidPayload(const MapTransportBounds& bounds, float speed,
                         float durationUpdates, Vec3 direction) noexcept {
    if (!Finite(direction) || !std::isfinite(speed) ||
        !std::isfinite(durationUpdates) ||
        speed < bounds.minimumSpeed || speed > bounds.maximumSpeed ||
        durationUpdates < bounds.minimumDurationUpdates ||
        durationUpdates > bounds.maximumDurationUpdates) {
        return false;
    }
    const double length = Length(direction);
    return std::isfinite(length) &&
           length >= static_cast<double>(bounds.minimumDirectionLength) &&
           length <= static_cast<double>(bounds.maximumDirectionLength);
}

inline MapTransportResult TryMapTransportUnsafe(
    const MapTransportApi& api, const MapTransportBounds& bounds,
    std::uint8_t* document, std::uint8_t* unit,
    MapTransportWitness* witness) noexcept {
    if (witness) ++witness->attempts;
    if (!api.query || !api.overlap || !api.ownedLocalAi ||
        !api.actionAllowed || !api.controlAllowed || !api.forcedMoving ||
        !api.authority || !api.submit || !ValidBounds(bounds) ||
        !document || !unit) {
        return MapTransportResult::InvalidContract;
    }

    std::int32_t hp = 0;
    std::uint32_t pilotId = 0;
    if (!api.ownedLocalAi(api.context, unit) ||
        !detail::Read(unit, kUnitHp, hp) || hp <= 0 ||
        !detail::Read(unit, kUnitPilotId, pilotId) || pilotId == 0) {
        if (witness) ++witness->rejectedUnit;
        return MapTransportResult::UnitRejected;
    }
    if (!api.actionAllowed(api.context, unit) ||
        !api.controlAllowed(api.context, unit) ||
        api.forcedMoving(api.context, unit)) {
        if (witness) ++witness->rejectedState;
        return MapTransportResult::StateBlocked;
    }

    std::uint8_t* quadtree = nullptr;
    std::uint8_t* ownerScene = nullptr;
    Aabb ownerBounds{};
    const void* ownerShape = nullptr;
    if (!detail::ReadPointer(document, kDocumentQuadTree, quadtree) ||
        !detail::ReadPointer(unit, kUnitScene, ownerScene) ||
        !detail::Read(ownerScene, kSceneBounds, ownerBounds) ||
        !detail::ValidAabb(ownerBounds) ||
        !detail::Copy(ownerScene + kSceneSphereArray, &ownerShape,
                      sizeof(ownerShape)) ||
        !ownerShape) {
        if (witness) ++witness->rejectedQuery;
        return MapTransportResult::QueryUnavailable;
    }

    void* scenes[kSceneCapacity]{};
    int sceneCount = 0;
    if (witness) ++witness->queries;
    api.query(quadtree, scenes, &sceneCount, kSceneCapacity, &ownerBounds);
    if (sceneCount < 0 || sceneCount >= kSceneCapacity) {
        if (witness) ++witness->rejectedQuery;
        return MapTransportResult::QueryUnavailable;
    }

    for (int index = 0; index < sceneCount; ++index) {
        auto* scene = static_cast<std::uint8_t*>(scenes[index]);
        if (witness) ++witness->candidates;
        if (!scene || scene == ownerScene ||
            !IsKind(scene, 5, kCollisionSceneKind)) {
            continue;
        }

        std::uint8_t* object = nullptr;
        if (!detail::ReadPointer(scene, kSceneObject, object) ||
            !IsKind(object, 1, kForcedMoveObjectKind)) {
            continue;
        }
        if (witness) ++witness->forcedMoveObjects;

        std::uint8_t* registeredScene = nullptr;
        const void* padShape = nullptr;
        if (!detail::ReadPointer(object, kObjectScene, registeredScene) ||
            registeredScene != scene ||
            !detail::Copy(scene + kSceneObbArray, &padShape, sizeof(padShape)) ||
            !padShape) {
            if (witness) ++witness->rejectedQuery;
            return MapTransportResult::QueryUnavailable;
        }
        if (!api.overlap(ownerShape, padShape)) continue;
        if (witness) ++witness->overlaps;

        float speed = 0.0f;
        float durationUpdates = 0.0f;
        Vec3 direction{};
        if (!detail::Read(object, kPadSpeed, speed) ||
            !detail::Read(object, kPadDuration, durationUpdates) ||
            !detail::Read(scene, kSceneForward, direction) ||
            !ValidPayload(bounds, speed, durationUpdates, direction)) {
            if (witness) ++witness->rejectedPayload;
            return MapTransportResult::InvalidPayload;
        }

        ForceMoveRequest085A request{};
        request.opcode = kForceMoveRequestOpcode;
        request.pilotId = pilotId;
        request.speed = speed;
        request.durationUpdates = durationUpdates;
        request.direction = direction;
        if (!api.authority(api.context, unit, object, &request)) {
            if (witness) ++witness->rejectedAuthority;
            return MapTransportResult::AuthorityRejected;
        }
        if (witness) ++witness->authorityAccepted;

        // The query and authority callbacks may have taken long enough for the
        // unit identity or movement state to change. Narrow that window before
        // submit; the submit callback still owns the atomic pending latch.
        std::int32_t currentHp = 0;
        std::uint32_t currentPilotId = 0;
        if (!api.ownedLocalAi(api.context, unit) ||
            !detail::Read(unit, kUnitHp, currentHp) || currentHp <= 0 ||
            !detail::Read(unit, kUnitPilotId, currentPilotId) ||
            currentPilotId != pilotId) {
            if (witness) ++witness->rejectedUnit;
            return MapTransportResult::UnitRejected;
        }
        if (!api.actionAllowed(api.context, unit) ||
            !api.controlAllowed(api.context, unit) ||
            api.forcedMoving(api.context, unit)) {
            if (witness) ++witness->rejectedState;
            return MapTransportResult::StateBlocked;
        }
        if (!api.submit(api.context, &request)) {
            if (witness) ++witness->rejectedSubmit;
            return MapTransportResult::SubmitRejected;
        }
        if (witness) ++witness->submitted;
        return MapTransportResult::Submitted;
    }
    return MapTransportResult::NoOverlap;
}

} // namespace map_transport_detail

inline MapTransportResult TryMapTransport(
    const MapTransportApi& api, const MapTransportBounds& bounds,
    std::uint8_t* document, std::uint8_t* unit,
    MapTransportWitness* witness = nullptr) noexcept {
#if defined(_MSC_VER) && defined(_WIN32)
    __try {
        return map_transport_detail::TryMapTransportUnsafe(
            api, bounds, document, unit, witness);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (witness) ++witness->rejectedQuery;
        return MapTransportResult::QueryUnavailable;
    }
#else
    return map_transport_detail::TryMapTransportUnsafe(
        api, bounds, document, unit, witness);
#endif
}

} // namespace battle_ai::native
