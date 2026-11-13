#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../mods/map_catalog/presentation.h"
#include "../mods/map_catalog/shield_contact_presentation.h"

namespace m = sdgo::map_catalog;

void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Put(m::Entry& entry, size_t offset, uint32_t value) {
    std::memcpy(entry.bytes.data() + offset, &value, sizeof(value));
}

int main(int argc, char** argv) {
    Check(m::ValidFlags(m::kGameModeType, 0x5401), "mission section policy is bounded to eight phases");
    Check(!m::ValidFlags(m::kNpcTemplateType, 0x5401), "section policy cannot attach to NPC records");
    Check(!m::ValidFlags(m::kGameModeType, 0x10001), "unknown section bits are rejected");
    Check(m::ShowsSectionClear(0, 2), "old missions retain native section banners");
    for (uint32_t phase = 1; phase <= 8; ++phase)
        Check(m::ShowsSectionClear(0x5401, phase) == (phase == 3 || phase == 5 || phase == 7),
              "only declared completed sections show a banner");
    Check(m::ShowsSectionClear(0x5401, 0), "unknown native phase preserves original presentation");
    {
        namespace hit = sdgo::map_catalog::shield_contact;
        sdgo::map_boundary::Entry boundary{};
        boundary.flags = sdgo::map_boundary::kAreaEnabled | sdgo::map_boundary::kDamageEnabled;
        boundary.left = -1000.0f; boundary.top = 1000.0f;
        boundary.right = 1000.0f; boundary.bottom = -1000.0f;
        boundary.damageEdgeMask = sdgo::map_boundary::kEdgeRight;
        hit::Plan plan{};
        Check(hit::MakePlan(boundary, {950.0f, 25.0f, 40.0f}, plan),
              "authoritative shield contact produces a presentation plan");
        Check(plan.wall.x == 1000.0f && plan.wall.y == 125.0f && plan.wall.z == 40.0f,
              "wall context remains clamped to the configured damage edge");
        Check(plan.directionX == -1.0f && plan.directionZ == 0.0f,
              "native hurt direction points inward from the touched wall");
        const hit::Plan accepted = plan;
        Check(!hit::MakePlan(boundary, {INFINITY, 25.0f, 40.0f}, plan) &&
                   plan.wall.x == accepted.wall.x,
              "invalid coordinates cannot replace an accepted presentation plan");
        Check(std::strstr(hit::kLuaFormat, "0506.zfx") != nullptr &&
                  std::strstr(hit::kLuaFormat, "0087.zfx") != nullptr &&
                  std::strstr(hit::kLuaFormat, "0106.zfx") == nullptr &&
                  std::strstr(hit::kLuaFormat, "0050.zsd") == nullptr &&
                  std::strstr(hit::kLuaFormat, "CameraShake") == nullptr,
              "contact presentation preserves terrain effects without invented body particles or sound");

        std::array<uint8_t, 0xF00> unit{};
        std::array<uint8_t, 0x240> movement{};
        const int32_t health = 700;
        hit::WriteField(unit.data(), hit::kUnitHealth, health);
        hit::WriteField(unit.data(), hit::kUnitMovementFsm, movement.data());
        hit::WriteField(movement.data(), hit::kMovementOwner, unit.data());
        hit::WriteField(movement.data(), hit::kFsmCurrentState, movement.data() + 0x20);
        hit::WriteField<uint8_t*>(movement.data(), hit::kFsmPendingState, nullptr);
        unsigned queued = 0;
        uint8_t* queuedFsm = nullptr;
        uint8_t* queuedState = nullptr;
        auto queue = [&](uint8_t* fsm, uint8_t* state) noexcept {
            ++queued; queuedFsm = fsm; queuedState = state; return true;
        };
        Check(hit::QueueNativeHurt(unit.data(), plan.directionX, plan.directionZ, queue),
              "idle native movement FSM accepts ordinary wall hurt");
        Check(queued == 1 && queuedFsm == movement.data() &&
                  queuedState == movement.data() + hit::kMovementHurtState,
              "wall hurt queues the shipped movement E8 state exactly once");
        Check(hit::ReadField<int32_t>(unit.data(), hit::kUnitHealth) == health,
              "presentation-only native hurt never writes authoritative HP");
        Check(hit::ReadField<uint8_t>(movement.data(), hit::kMovementHurtStyle) ==
                  hit::kOrdinaryHurtStyle &&
                  hit::ReadField<float>(movement.data(), hit::kMovementMagnitude) == 0.0f &&
                  hit::ReadField<float>(movement.data(), hit::kMovementDirectionX) == -1.0f &&
                  hit::ReadField<float>(movement.data(), hit::kMovementDirectionY) == 0.0f &&
                  hit::ReadField<float>(movement.data(), hit::kMovementDirectionZ) == 0.0f,
              "wall hurt uses native light ordinary-hit inputs without extra knockback");

        const float sentinelDirection = 9.0f;
        const uint8_t sentinelStyle = 7;
        const float sentinelMagnitude = 8.0f;
        hit::WriteField(movement.data(), hit::kMovementDirectionX, sentinelDirection);
        hit::WriteField(movement.data(), hit::kMovementHurtStyle, sentinelStyle);
        hit::WriteField(movement.data(), hit::kMovementMagnitude, sentinelMagnitude);
        hit::WriteField(movement.data(), hit::kFsmCurrentState,
                        movement.data() + hit::kMovementProtectedHurtState);
        Check(!hit::QueueNativeHurt(unit.data(), -1.0f, 0.0f, queue) && queued == 1 &&
                  hit::ReadField<float>(movement.data(), hit::kMovementDirectionX) == sentinelDirection &&
                  hit::ReadField<uint8_t>(movement.data(), hit::kMovementHurtStyle) == sentinelStyle &&
                  hit::ReadField<float>(movement.data(), hit::kMovementMagnitude) == sentinelMagnitude &&
                  hit::ReadField<int32_t>(unit.data(), hit::kUnitHealth) == health,
              "protected native hurt state is preserved without input or HP mutation");
        const uint8_t invulnerable = 1;
        hit::WriteField(unit.data(), hit::kUnitInvulnerable, invulnerable);
        Check(!hit::QueueNativeHurt(unit.data(), -1.0f, 0.0f, queue) && queued == 1,
              "native invulnerability preserves the current action and HP");
        hit::WriteField<uint8_t>(unit.data(), hit::kUnitInvulnerable, 0);
        hit::WriteField<uint32_t>(unit.data(), hit::kUnitHurtState, 2);
        Check(!hit::QueueNativeHurt(unit.data(), -1.0f, 0.0f, queue) && queued == 1 &&
                  hit::ReadField<int32_t>(unit.data(), hit::kUnitHealth) == health,
              "native SP/hurt protection rejects synthetic wall interruption");
        hit::WriteField<uint32_t>(unit.data(), hit::kUnitHurtState, 0);
        hit::WriteField(movement.data(), hit::kFsmCurrentState, movement.data() + 0x20);
        hit::WriteField(movement.data(), hit::kFsmPendingState,
                        movement.data() + hit::kMovementDownState);
        Check(!hit::QueueNativeHurt(unit.data(), -1.0f, 0.0f, queue) && queued == 1,
              "pending native knockdown cannot be overwritten by wall feedback");
    }
    m::Catalog catalog;
    m::Entry npc{m::kNpcTemplateType, 8001, 0, std::vector<uint8_t>(598)};
    Put(npc, 0, npc.id);
    Put(npc, 390, 7001); // inherited stock-mission reference, absent in this package
    Put(npc, 394, 7002);
    npc.bytes[398] = 's';
    Check(!m::DeclaresUnitCutin(catalog, npc), "inherited missing speech must not enable the native mode switch");
    catalog.records.push_back({m::kUnitType, 7001, 0, {}});
    Check(!m::DeclaresUnitCutin(catalog, npc), "an unrelated grid kind cannot satisfy speech ownership");
    catalog.records.push_back({m::kSpeechType, 7001, 0, std::vector<uint8_t>(506)});
    Check(!m::DeclaresUnitCutin(catalog, npc), "missing inherited ending speech also prevents accidental opt-in");
    Put(npc, 394, 0);
    Check(m::DeclaresUnitCutin(catalog, npc), "authored speech plus illustration retains native cut-in eligibility");
    npc.bytes[398] = 0;
    Check(!m::DeclaresUnitCutin(catalog, npc), "speech without a unit illustration remains ordinary dialogue");
    if (argc > 1) {
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0);
        Check(length > 0, "catalog path is UTF-8");
        std::wstring path(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, path.data(), length);
        std::string error;
        Check(catalog.Load(path, error), error.c_str());
        unsigned enabled = 0, inherited = 0;
        for (const auto& entry : catalog.records) {
            if (m::DeclaresUnitCutin(catalog, entry)) ++enabled;
            else if (entry.kind == m::kNpcTemplateType && entry.bytes.size() == 598 &&
                     m::U32(entry.bytes.data() + 390) && entry.bytes[398]) ++inherited;
        }
        std::printf("Catalog presentation: %u authored cut-ins; %u inherited references excluded\n", enabled, inherited);
    }
    std::puts("PASS imported NPC presentation ownership");
}
