#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace room_style_preview {

constexpr uint32_t kApp2dNormal = 7;
constexpr uint32_t kApp2dMission = 16;
constexpr uint32_t kApp3dWaitRoom = 18;
constexpr uint16_t kNormalSourceTag = 6;
constexpr uint16_t kMissionSourceTag = 7;
constexpr uint8_t kNormalFactory = 0;
constexpr uint8_t kMissionFactory = 3;
/*
 * FFFFFFFF is the shipped unset/random-map sentinel at this transition
 * boundary. APP7 consumes payload +8 directly. APP16 source tag 7 instead
 * copies the already-published native global into its +6434 field, then sends
 * an 08A5 room-info refresh containing only the room id. Preserve the sentinel
 * for both paths; the refresh supplies the concrete room/mission description.
 */
constexpr uint32_t kRandomMapSentinel = 0xffffffffu;
constexpr uint32_t kNormalRefreshOpcode = 0x0943;
constexpr uint32_t kMissionRefreshOpcode = 0x08a5;

enum class Kind : uint32_t {
    Normal = 1,
    Mission = 2
};

#pragma pack(push, 1)
struct NativeTransitionPayload24 {
    uint16_t source_app;
    uint16_t reserved_02;
    uint32_t room_id;
    uint32_t map_id;
    uint32_t context_43dc;
    uint8_t factory_type;
    uint8_t reserved_11[3];
    uint32_t auxiliary;
};
#pragma pack(pop)

static_assert(sizeof(NativeTransitionPayload24) == 24, "native transition payload size");
static_assert(offsetof(NativeTransitionPayload24, room_id) == 4, "room id offset");
static_assert(offsetof(NativeTransitionPayload24, map_id) == 8, "map id offset");
static_assert(offsetof(NativeTransitionPayload24, context_43dc) == 12, "context offset");
static_assert(offsetof(NativeTransitionPayload24, factory_type) == 16, "factory offset");
static_assert(offsetof(NativeTransitionPayload24, auxiliary) == 20, "auxiliary offset");

struct PlanInput {
    bool preference_2d;
    bool caller_allowed;
    bool native_identity_valid;
    bool already_used;
    Kind kind;
    uint32_t service;
    NativeTransitionPayload24 payload;
};

struct Plan {
    bool rewrite;
    uint32_t reject_reason;
    uint32_t target_service;
    uint16_t source_tag;
    uint32_t refresh_opcode;
};

inline Plan MakePlan(const PlanInput& in) {
    Plan out = {};
    if (!in.preference_2d) {
        out.reject_reason = 1;
        return out;
    }
    if (in.service != kApp3dWaitRoom) {
        out.reject_reason = 2;
        return out;
    }
    if (!in.caller_allowed) {
        out.reject_reason = 3;
        return out;
    }
    const uint8_t expectedFactory =
        in.kind == Kind::Normal ? kNormalFactory : kMissionFactory;
    if (in.payload.factory_type != expectedFactory) {
        out.reject_reason = 5;
        return out;
    }
    if (in.payload.room_id == 0) {
        out.reject_reason = 6;
        return out;
    }
    if (!in.native_identity_valid) {
        out.reject_reason = 8;
        return out;
    }
    if (in.already_used) {
        out.reject_reason = 9;
        return out;
    }
    out.rewrite = true;
    out.target_service =
        in.kind == Kind::Normal ? kApp2dNormal : kApp2dMission;
    out.source_tag =
        in.kind == Kind::Normal ? kNormalSourceTag : kMissionSourceTag;
    out.refresh_opcode =
        in.kind == Kind::Normal ? kNormalRefreshOpcode : kMissionRefreshOpcode;
    return out;
}

inline void ApplyPlan(NativeTransitionPayload24* payload, const Plan& plan) {
    if (payload && plan.rewrite)
        payload->source_app = plan.source_tag;
}

/*
 * The native 24-byte builders initialize +0/+4/+8/+0C/+10/+14, but their
 * allocator leaves alignment bytes +02..03 and +11..13 unspecified. Padding
 * is preserved, never used as identity. Context and auxiliary are explicit.
 */
inline bool NativeShapeMatches(
    const NativeTransitionPayload24& payload,
    uint32_t profileContext) {
    return payload.context_43dc == profileContext &&
        payload.auxiliary == 0;
}

inline uint32_t Crc32(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

} // namespace room_style_preview
