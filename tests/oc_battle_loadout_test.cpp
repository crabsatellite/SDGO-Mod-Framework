#include "ai_test_support.h"
#include "sdgo/oc_battle_loadout.h"
#include "sdgo/oc_battle_attributes.h"
using namespace sdgo::oc_custom;
int main() {
    std::array<std::uint8_t, kUnitRecordBytes> record{};
    Carrier carrier{}; carrier.flags = carrier_room_rule | carrier_g1_profile;
    carrier.g1_skills = {30182, 30183, 0, 0};
    WriteCarrier(record.data(), carrier);
    const auto saved = record;
    std::array<std::uint8_t, 0x32> block{}; block.fill(0xA5);
    const auto original = block;
    SCENARIO("G2 and unknown room preserve the whole native block");
    CHECK(!SelectBattleLoadout(block.data(), record.data(), true, RoomSystem::parts));
    CHECK(block == original);
    CHECK(!SelectBattleLoadout(block.data(), record.data(), false, RoomSystem::g1));
    CHECK(block == original);
    SCENARIO("G1 uses its own skills before native effect construction");
    CHECK(SelectBattleLoadout(block.data(), record.data(), true, RoomSystem::g1));
    for (std::size_t i=0;i<4;++i) {
        std::uint16_t skill=0; std::memcpy(&skill,block.data()+0x26+i*3,2);
        CHECK(skill==carrier.g1_skills[i]); CHECK(block[0x28+i*3]==(skill?1:0));
    }
    CHECK(std::memcmp(block.data(),original.data(),0x26)==0); CHECK(record==saved);
    SCENARIO("OC clears all four IDs and activation bytes without clearing native stats");
    CHECK(SelectBattleLoadout(block.data(), record.data(), true, RoomSystem::oc));
    for(std::size_t i=0x26;i<block.size();++i) CHECK(block[i]==0);
    CHECK(std::memcmp(block.data(),original.data(),0x26)==0); CHECK(record==saved);
    SCENARIO("missing G1 profile cannot inherit G2; server OC works without room discovery");
    carrier.flags=carrier_room_rule;WriteCarrier(record.data(),carrier);block=original;
    CHECK(SelectBattleLoadout(block.data(),record.data(),true,RoomSystem::g1));
    for(std::size_t i=0x26;i<block.size();++i) CHECK(block[i]==0);
    carrier.flags=carrier_oc_mode;WriteCarrier(record.data(),carrier);block=original;
    CHECK(SelectBattleLoadout(block.data(),record.data(),false,RoomSystem::parts));
    for(std::size_t i=0x26;i<block.size();++i) CHECK(block[i]==0);
    SCENARIO("native attribute validation rejects a correct label with stale effects");
    BattleAttributes expected{}; expected.skills={30182,30183,0,0};expected.medium_damage=0.88f;
    expected.medium_attack_speed=1.05f;expected.medium_reload=0.03f;expected.medium_range=60;expected.medium_cooldown=5;
    auto actual=expected;CHECK(SameBattleAttributes(actual,expected));
    actual.medium_damage=1;CHECK(!SameBattleAttributes(actual,expected));
    actual=expected;actual.medium_reload=0.05f;CHECK(!SameBattleAttributes(actual,expected));
    expected.skills={};expected.medium_damage=1;expected.medium_attack_speed=1;
    expected.medium_reload=0;expected.medium_range=0;expected.medium_cooldown=0;
    actual=expected;actual.medium_range=200;CHECK(!SameBattleAttributes(actual,expected));
    actual=expected;actual.skills={30182,0,0,0};CHECK(!SameBattleAttributes(actual,expected));
    actual=expected;actual.maximum_hp=500;CHECK(!SameBattleAttributes(actual,expected));
    return ai_test::Finish();
}
