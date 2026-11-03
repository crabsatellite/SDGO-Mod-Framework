#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "room_style_preview_core.h"
#include "room_style_preview_status.h"

namespace {

using namespace room_style_preview;

int failures = 0;

void Check(bool condition, const char* label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

NativeTransitionPayload24 Payload(uint8_t factory) {
    NativeTransitionPayload24 payload = {};
    payload.source_app = 6;
    payload.reserved_02 = 0x55aa;
    payload.room_id = 0x12345678;
    payload.map_id = 0x3344;
    payload.context_43dc = 0x11223344;
    payload.factory_type = factory;
    payload.reserved_11[0] = 0xa1;
    payload.reserved_11[1] = 0xb2;
    payload.reserved_11[2] = 0xc3;
    payload.auxiliary = 0x55667788;
    return payload;
}

PlanInput Valid(Kind kind) {
    PlanInput input = {};
    input.preference_2d = true;
    input.caller_allowed = true;
    input.native_identity_valid = true;
    input.kind = kind;
    input.service = kApp3dWaitRoom;
    input.payload = Payload(kind == Kind::Normal ? kNormalFactory : kMissionFactory);
    return input;
}

void TestNormal() {
    auto input = Valid(Kind::Normal);
    const auto before = input.payload;
    const Plan plan = MakePlan(input);
    Check(plan.rewrite, "normal plan rewrites");
    Check(plan.target_service == kApp2dNormal, "normal target APP7");
    Check(plan.source_tag == kNormalSourceTag, "normal source tag6");
    Check(plan.refresh_opcode == kNormalRefreshOpcode, "normal refresh 0943");
    ApplyPlan(&input.payload, plan);
    Check(input.payload.source_app == 6, "normal source remains tag6");
    Check(std::memcmp(
              reinterpret_cast<const uint8_t*>(&input.payload) + 2,
              reinterpret_cast<const uint8_t*>(&before) + 2,
              sizeof(input.payload) - 2) == 0,
          "normal preserves bytes +02..+17");
}

void TestNormalRandomMap() {
    auto input = Valid(Kind::Normal);
    input.payload.map_id = kRandomMapSentinel;
    const Plan plan = MakePlan(input);
    Check(plan.rewrite, "normal random-map sentinel rewrites");
    Check(plan.target_service == kApp2dNormal, "random-map target APP7");
    const auto before = input.payload;
    ApplyPlan(&input.payload, plan);
    Check(input.payload.map_id == kRandomMapSentinel,
          "random-map sentinel stays unchanged");
    Check(std::memcmp(
              reinterpret_cast<const uint8_t*>(&input.payload) + 2,
              reinterpret_cast<const uint8_t*>(&before) + 2,
              sizeof(input.payload) - 2) == 0,
          "random-map path preserves bytes +02..+17");
}

void TestMission() {
    auto input = Valid(Kind::Mission);
    const auto before = input.payload;
    const Plan plan = MakePlan(input);
    Check(plan.rewrite, "mission plan rewrites");
    Check(plan.target_service == kApp2dMission, "mission target APP16");
    Check(plan.source_tag == kMissionSourceTag, "mission source tag7");
    Check(plan.refresh_opcode == kMissionRefreshOpcode, "mission refresh 08A5");
    ApplyPlan(&input.payload, plan);
    Check(input.payload.source_app == 7, "mission writes tag7");
    Check(std::memcmp(
              reinterpret_cast<const uint8_t*>(&input.payload) + 2,
              reinterpret_cast<const uint8_t*>(&before) + 2,
              sizeof(input.payload) - 2) == 0,
          "mission preserves bytes +02..+17");
}

void TestMissionUnsetMap() {
    auto input = Valid(Kind::Mission);
    input.payload.map_id = kRandomMapSentinel;
    const auto before = input.payload;
    const Plan plan = MakePlan(input);
    Check(plan.rewrite, "Mission unset-map sentinel rewrites");
    Check(plan.target_service == kApp2dMission,
          "Mission unset-map target APP16");
    ApplyPlan(&input.payload, plan);
    Check(input.payload.source_app == kMissionSourceTag,
          "Mission unset-map writes source tag7");
    Check(input.payload.map_id == kRandomMapSentinel,
          "Mission unset-map sentinel stays unchanged");
    Check(std::memcmp(
              reinterpret_cast<const uint8_t*>(&input.payload) + 2,
              reinterpret_cast<const uint8_t*>(&before) + 2,
              sizeof(input.payload) - 2) == 0,
          "Mission unset-map path preserves bytes +02..+17");
}

void TestFailClosed() {
    auto input = Valid(Kind::Normal);
    input.preference_2d = false;
    Check(!MakePlan(input).rewrite, "3d preference passes through");
    input = Valid(Kind::Normal);
    input.service = 14;
    Check(!MakePlan(input).rewrite, "non APP18 passes through");
    input = Valid(Kind::Normal);
    input.caller_allowed = false;
    Check(!MakePlan(input).rewrite, "unknown caller passes through");
    input = Valid(Kind::Normal);
    input.payload.factory_type = 1;
    Check(!MakePlan(input).rewrite, "Team factory does not become normal");
    input = Valid(Kind::Mission);
    input.payload.factory_type = 0;
    Check(!MakePlan(input).rewrite, "normal factory does not become Mission");
    input = Valid(Kind::Normal);
    input.native_identity_valid = false;
    Check(!MakePlan(input).rewrite, "invalid native identity passes through");
    input = Valid(Kind::Normal);
    input.already_used = true;
    Check(!MakePlan(input).rewrite, "one-shot passes through after use");
    input = Valid(Kind::Normal);
    input.payload.room_id = 0;
    Check(!MakePlan(input).rewrite, "zero room passes through");
}

void TestNativePaddingIsOpaque() {
    auto payload = Payload(kNormalFactory);
    const uint32_t profileContext = payload.context_43dc;
    payload.reserved_02 = 0xd3a7;
    payload.reserved_11[0] = 0x19;
    payload.reserved_11[1] = 0xe4;
    payload.reserved_11[2] = 0x72;
    payload.auxiliary = 0;
    Check(NativeShapeMatches(payload, profileContext),
          "uninitialized native padding is not identity");
    ++payload.context_43dc;
    Check(!NativeShapeMatches(payload, profileContext),
          "context mismatch rejects native shape");
    payload.context_43dc = profileContext;
    payload.auxiliary = 1;
    Check(!NativeShapeMatches(payload, profileContext),
          "nonzero auxiliary rejects native shape");
}

} // namespace

int main() {
    static_assert(sizeof(RoomStylePreviewSnapshotV1) == 104, "snapshot ABI");
    TestNormal();
    TestNormalRandomMap();
    TestMission();
    TestMissionUnsetMap();
    TestFailClosed();
    TestNativePaddingIsOpaque();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "room_style_preview_offline_test: PASS\n";
    return 0;
}
