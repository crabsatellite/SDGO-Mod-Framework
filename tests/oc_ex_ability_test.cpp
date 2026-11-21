// Offline EX regression: real carrier encoding and Battle Mod application on
// bounded synthetic records. No game, player data or database is loaded.
#include "ai_test_support.h"
#include "../mods/oc_custom_battle/oc_custom_battle.cpp"

#include <array>

namespace oc = sdgo::oc_custom;

namespace {
int __stdcall NativeStatsForTest(std::uint8_t* unit, std::uint8_t*, std::uint8_t*, int, char,
                                std::uint8_t, std::uint8_t) {
    unit[13] = 0xFE; // stale native state must not leak across room systems
    return 1;
}
void __cdecl NativeCopyContinuationForTest() {}
// Own the whole frame transition so the compiler cannot inline a synthetic
// EBP into the C++ test runner's frame or address its locals through it.
__declspec(naked) void __cdecl RunCopiedSkillsSiteForTest(std::uint8_t*, bool) {
    __asm {
        push ebp
        mov ebp, [esp + 8]
        cmp byte ptr [esp + 12], 0
        je normal_path
        call LoadoutDeckEntry
        jmp done
    normal_path:
        call LoadoutNormalEntry
    done:
        pop ebp
        ret
    }
}
struct Fixture {
    std::array<std::uint8_t, 0x700> unit{};
    std::array<std::uint8_t, oc::kUnitRecordBytes> record{};
    std::array<std::uint8_t, 0x100> definition{};
    std::array<std::uint8_t, 5 * 16> caps{};
    oc::Carrier carrier{};

    Fixture() {
        definition[kDefinitionGrade] = 1;
        WriteAt(caps.data(), 16 + 4, std::int32_t{169});
        WriteAt(caps.data(), 16 + 8, std::int32_t{169});
        g_grade_caps = caps.data();
        g_log_apply = false;
        carrier.flags = oc::carrier_profile | oc::carrier_room_rule;
        carrier.oc_level = 15;
        carrier.extra_bonus = 1;
        carrier.delta[oc::stat_attack] = 10;
        carrier.delta[oc::stat_defense] = 7;
        SetStats(0, 100, 120, 130, 140);
    }

    ~Fixture() { g_grade_caps = nullptr; }

    void SetStats(int form, std::uint16_t base_attack, std::uint16_t base_defense,
                  std::uint16_t native_attack, std::uint16_t native_defense) {
        WriteAt(definition.data(), kDefinitionAttack, base_attack);
        WriteAt(definition.data(), kDefinitionDefense, base_defense);
        const auto stride = static_cast<std::size_t>(form) * kFormStride;
        WriteAt(unit.data(), kUnitAttack + stride, native_attack);
        WriteAt(unit.data(), kUnitDefense + stride, native_defense);
    }

    void Apply(int form = 0, bool oc_room = true, std::uint8_t mode = 0) {
        oc::WriteCarrier(record.data(), carrier);
        ApplyUnsafe(unit.data(), record.data(), definition.data(), form, oc_room, mode);
    }

    std::uint16_t Stat(std::size_t field, int form = 0) const {
        std::uint16_t value = 0;
        ReadAt(unit.data(), field + static_cast<std::size_t>(form) * kFormStride, &value);
        return value;
    }
};
}

int main() {
    SCENARIO("EX selection requests reject invalid choices and preserve revision");
    for (std::uint8_t ability = 1; ability <= 8; ++ability) {
        std::array<std::uint8_t, oc::kExSetRequestBytes> wire{};
        oc::ExSetRequest request{0x123456789ULL, 987, ability}, decoded;
        oc::WriteExSetRequest(wire.data(), request);
        CHECK(oc::ReadExSetRequest(wire.data(), wire.size(), &decoded));
        CHECK(decoded.unit_uid == request.unit_uid && decoded.revision == 987 && decoded.ability == ability);
        CHECK(!oc::ReadExSetRequest(wire.data(), wire.size() - 1, &decoded));
        wire[12] = 9;
        CHECK(!oc::ReadExSetRequest(wire.data(), wire.size(), &decoded));
        wire[12] = 0;
        CHECK(!oc::ReadExSetRequest(wire.data(), wire.size(), &decoded));
    }
    SCENARIO("every EX reaches the native client byte; other systems and mode 4 clear it");
    for (std::uint8_t ability = 0; ability <= 8; ++ability) {
        for (bool room : {false, true}) {
            for (std::uint8_t level : {std::uint8_t{14}, std::uint8_t{15}}) {
                for (std::uint8_t mode : {std::uint8_t{0}, std::uint8_t{4}}) {
                    Fixture f;
                    f.unit[13] = 0xFF;
                    f.carrier.extra_bonus = ability;
                    f.carrier.oc_level = level;
                    f.Apply(0, room, mode);
                    CHECK(f.unit[13] == (room && level == 15 && mode != 4 ? ability : 0));
                }
            }
        }
    }
    SCENARIO("EX replies carry the authoritative choice, revision and consumable balance");
    {
        std::array<std::uint8_t, oc::kExReplyBytes> wire{};
        oc::ExReply reply;
        reply.ret_val = 8; // a stale revision refreshes state without charging again
        reply.item = oc::kExSettingItem;
        reply.remaining = 23;
        reply.profile.unit_uid = 0x100000003ULL;
        reply.profile.revision = 9;
        reply.profile.oc_level = 15;
        reply.profile.extra_bonus = 7;
        oc::WriteExReply(wire.data(), reply);
        oc::ExReply decoded;
        CHECK(oc::ReadExReply(wire.data(), wire.size(), &decoded));
        CHECK(decoded.ret_val == 8 && decoded.remaining == 23 && decoded.item == oc::kExSettingItem);
        CHECK(decoded.profile.unit_uid == reply.profile.unit_uid && decoded.profile.revision == 9);
        CHECK(decoded.profile.extra_bonus == 7 && decoded.profile.oc_level == 15);
        CHECK(!oc::ReadExReply(wire.data(), wire.size() - 1, &decoded));
        wire[10] = 0;
        CHECK(!oc::ReadExReply(wire.data(), wire.size(), &decoded));
    }
    SCENARIO("the real room dispatcher clears EX even when G1/G2 skip OC stat application");
    for (const char* title : {"!oc", "!g1", "!g2", "ordinary room"}) {
        Fixture f;
        f.carrier.extra_bonus = 8;
        oc::WriteCarrier(f.record.data(), f.carrier);
        std::array<std::uint8_t, 0x4000> room{};
        std::array<std::uint8_t, 16> frame{};
        room[kRoomTitleLength] = static_cast<std::uint8_t>(std::strlen(title));
        std::memcpy(room.data() + kRoomTitle, title, std::strlen(title));
        WriteAt(frame.data(), 8, room.data());
        g_human_build = 0x1000;
        t_caller_return = g_human_build + 1;
        t_caller_frame = frame.data();
        g_original = &NativeStatsForTest;
        CHECK(BuildStatsHook(f.unit.data(), f.record.data(), f.definition.data(), 0, 0, 0, 0) == 1);
        CHECK(f.unit[13] == (std::strcmp(title, "!oc") == 0 ? 8 : 0));
    }
    g_original = nullptr;
    g_human_build = 0;
    SCENARIO("both native copy sites select parts before the server cache reads them");
    g_loadout_normal_original = reinterpret_cast<void*>(&NativeCopyContinuationForTest);
    g_loadout_deck_original = reinterpret_cast<void*>(&NativeCopyContinuationForTest);
    for (bool deck : {false, true}) {
        for (const char* title : {"!oc", "!g1", "!g2"}) {
            Fixture f;
            f.carrier.flags |= oc::carrier_g1_profile;
            f.carrier.g1_skills = {30182, 30183, 0, 0};
            oc::WriteCarrier(f.record.data(), f.carrier);
            const auto saved = f.record;
            const std::array<std::uint16_t, 4> g2 = {30468, 30470, 0, 0};
            WriteUnitSkills(f.unit.data(), g2);
            std::array<std::uint8_t, 0x4000> room{};
            room[kRoomTitleLength] = static_cast<std::uint8_t>(std::strlen(title));
            std::memcpy(room.data() + kRoomTitle, title, std::strlen(title));
            std::array<std::uint8_t, 64> stack{};
            auto* frame = stack.data() + 32;
            WriteAt(frame, 8, room.data());
            WriteAt(frame - 16, 0, f.record.data());
            WriteAt(frame - 12, 0, f.unit.data());
            SetLastError(1234);
            RunCopiedSkillsSiteForTest(frame, deck);
            CHECK(GetLastError() == 1234);
            std::array<std::uint16_t, 4> selected{};
            std::memcpy(selected.data(), f.unit.data() + kUnitCustomSkills, 8);
            const auto expected = std::strcmp(title, "!oc") == 0 ? std::array<std::uint16_t, 4>{}
                : std::strcmp(title, "!g1") == 0 ? f.carrier.g1_skills : g2;
            CHECK(selected == expected);
            CHECK(f.record == saved);
        }
    }
    g_loadout_normal_original = g_loadout_deck_original = nullptr;
    SCENARIO("EX uses the original float32 coefficient and truncation for every u16 base");
    bool all_match = true;
    for (std::uint32_t base = 0; base <= 65535; ++base) {
        // Exact rational value of float32 0x3CF5C28F, independently evaluated
        // without floating-point operations or the production helper.
        const auto expected = static_cast<std::int32_t>(static_cast<std::uint64_t>(base) * 16106127 / 536870912);
        all_match = all_match && oc::ExBaseStatGain(1, oc::stat_attack, static_cast<std::uint16_t>(base)) == expected;
        all_match = all_match && oc::ExBaseStatGain(2, oc::stat_defense, static_cast<std::uint16_t>(base)) == expected;
    }
    CHECK(all_match);
    CHECK(oc::ExBaseStatGain(1, oc::stat_attack, 100) == 2);
    CHECK(oc::ExBaseStatGain(1, oc::stat_attack, 34) == 1);
    CHECK(oc::ExBaseStatGain(1, oc::stat_defense, 100) == 0);
    CHECK(oc::ExBaseStatGain(2, oc::stat_attack, 100) == 0);
    CHECK(oc::ExBaseStatGain(8, oc::stat_attack, 100) == 0);

    SCENARIO("v1 and v2 keep the old carrier layout and preserve all EX IDs");
    for (bool g1 : {false, true}) {
        for (std::uint8_t extra = 0; extra <= 8; ++extra) {
            std::array<std::uint8_t, oc::kUnitRecordBytes> record;
            record.fill(0xA5);
            oc::Carrier original;
            original.flags = oc::carrier_profile | oc::carrier_room_rule;
            if (g1) original.flags |= oc::carrier_g1_profile;
            original.extra_bonus = extra;
            original.oc_level = 15;
            original.normal_points = 1;
            original.over_points = 12;
            original.revision = 0x00563412;
            original.delta = {7, 900, 12, 6, 4, 450};
            original.g1_skills = {30001, 30033, 31023, 0};
            oc::WriteCarrier(record.data(), original);
            const auto* wire = record.data() + oc::kCarrierOffset;
            CHECK(wire[2] == (g1 ? 2 : 1));
            CHECK((wire[3] & 0x0F) == original.flags);
            CHECK((wire[3] >> 4) == extra);
            CHECK(wire[24] == 0);
            CHECK(wire[23] == oc::detail::Checksum(wire));
            CHECK(record[oc::kCarrierOffset - 1] == 0xA5);
            CHECK(record[oc::kCarrierOffset + oc::kCarrierBytes] == 0xA5);
            oc::Carrier decoded;
            CHECK(oc::ReadCarrier(record.data(), &decoded));
            CHECK(decoded.flags == original.flags);
            CHECK(decoded.extra_bonus == extra);
            CHECK(decoded.delta == original.delta);
            CHECK(decoded.revision == original.revision);
            CHECK(!g1 || decoded.g1_skills == original.g1_skills);
            record[oc::kCarrierOffset + 3] ^= 0x10;
            CHECK(!oc::ReadCarrier(record.data(), &decoded));
        }
    }

    SCENARIO("invalid EX values are not emitted or accepted even with a matching checksum");
    {
        std::array<std::uint8_t, oc::kUnitRecordBytes> record{};
        oc::Carrier original;
        original.extra_bonus = 255;
        oc::WriteCarrier(record.data(), original);
        oc::Carrier decoded;
        CHECK(oc::ReadCarrier(record.data(), &decoded));
        CHECK(decoded.extra_bonus == 0);
        record[oc::kCarrierOffset + 3] = 0x90;
        record[oc::kCarrierOffset + 23] = oc::detail::Checksum(record.data() + oc::kCarrierOffset);
        CHECK(!oc::ReadCarrier(record.data(), &decoded));
    }

    SCENARIO("Battle applies EX to the form's base, not the already-enhanced native stat");
    {
        Fixture f;
        f.Apply();
        CHECK(f.Stat(kUnitAttack) == 142); // native 130 + allocated 10 + trunc(base 100 * float3%)
        CHECK(f.Stat(kUnitDefense) == 147);
        f.carrier.extra_bonus = 2;
        f.SetStats(1, 120, 100, 130, 140);
        f.Apply(1);
        CHECK(f.Stat(kUnitAttack, 1) == 140);
        CHECK(f.Stat(kUnitDefense, 1) == 149);
        CHECK(f.Stat(kUnitAttack) == 142); // another form was not changed
    }

    SCENARIO("EX gain is capped after adding allocated points");
    {
        Fixture f;
        f.SetStats(0, 160, 160, 160, 160);
        f.carrier.delta[oc::stat_attack] = 8;
        f.Apply();
        CHECK(f.Stat(kUnitAttack) == 169);
        CHECK(f.Stat(kUnitDefense) == 167);
    }

    SCENARIO("G1/G2 and missing profiles never receive OC or EX deltas");
    for (bool oc_room : {false, true}) {
        for (bool has_profile : {false, true}) {
            if (oc_room && has_profile) continue;
            Fixture f;
            if (!has_profile) f.carrier.flags &= static_cast<std::uint8_t>(~oc::carrier_profile);
            f.Apply(0, oc_room);
            CHECK(f.Stat(kUnitAttack) == 130);
            CHECK(f.Stat(kUnitDefense) == 140);
        }
    }
    SCENARIO("an EX level without a selected ability keeps ordinary OC values");
    {
        Fixture f;
        f.carrier.extra_bonus = 0;
        f.Apply();
        CHECK(f.Stat(kUnitAttack) == 140);
        CHECK(f.Stat(kUnitDefense) == 147);
    }
    SCENARIO("EX requires level 15 and native mode allowing EX; ordinary OC stays intact");
    for (std::uint8_t level : {std::uint8_t{14}, std::uint8_t{15}}) {
        for (std::uint8_t mode : {std::uint8_t{0}, std::uint8_t{4}}) {
            Fixture f;
            f.carrier.oc_level = level;
            f.Apply(0, true, mode);
            CHECK(f.Stat(kUnitAttack) == (level == 15 && mode != 4 ? 142 : 140));
            CHECK(f.Stat(kUnitDefense) == 147);
        }
    }
    SCENARIO("server-wide OC activates EX without a room title");
    {
        Fixture f;
        f.carrier.flags = oc::carrier_profile | oc::carrier_oc_mode;
        f.Apply(0, false);
        CHECK(f.Stat(kUnitAttack) == 142);
    }
    return ai_test::Finish();
}
