#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../mods/game_test_client/battle_combat_actions.h"

namespace {
int failures = 0;
int checks = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (condition)
        return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}

BattleCombatActions::ExpectedBattle Expected(
    uint32_t generation = 7,
    uint16_t mode = 1,
    const wchar_t* title = L"combat-test")
{
    return {generation, 9229, mode, title};
}

SdgoPvpAiClientSnapshotV1 Snapshot(
    uint32_t generation = 7,
    uint16_t mode = 1,
    const wchar_t* title = L"combat-test")
{
    SdgoPvpAiClientSnapshotV1 snapshot = {};
    snapshot.size = sizeof(snapshot);
    snapshot.version =
        SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
    snapshot.native_app_state = 22;
    snapshot.profile_pilot_id = 9229;
    snapshot.room_mode = mode;
    snapshot.room_title_length =
        static_cast<uint16_t>(std::wcslen(title));
    std::wmemcpy(
        snapshot.room_title,
        title,
        snapshot.room_title_length + 1);
    snapshot.lua_error_observer_available = 1;
    snapshot.received_battle_start_count = generation;
    return snapshot;
}

void Bot(
    SdgoPvpAiClientSnapshotV1& snapshot,
    uint32_t index,
    uint32_t pilot,
    uint32_t object,
    uint32_t npc,
    float x,
    float y,
    float z,
    uint32_t target,
    int32_t targetHp)
{
    auto& bot = snapshot.bots[index];
    bot.pilot_id = pilot;
    bot.object_id = object;
    bot.npc_id = npc;
    bot.target_object_id = target;
    bot.target_hp = targetHp;
    bot.team = index % 2 ? 2 : 1;
    bot.ai_enabled = 1;
    bot.valid_registered_object = 1;
    bot.local_control = 1;
    bot.master_npc = 1;
    bot.position_valid = 1;
    bot.position_x = x;
    bot.position_y = y;
    bot.position_z = z;
    if (snapshot.bot_count <= index)
        snapshot.bot_count = index + 1;
}

void TestAsynchronousFrames()
{
    BattleCombatActions actions;
    const auto expected = Expected();
    auto snapshot = Snapshot();
    Bot(
        snapshot,
        0,
        0x80000001,
        101,
        201,
        0.0f,
        3.0f,
        0.0f,
        900,
        100);

    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kWaitingForEvidence,
        "first genuine frame establishes target and continuity baselines");
    auto state = actions.State();
    Check(
        state.target_acquired &&
            state.target_acquisition_events == 1 &&
            !state.movement_observed &&
            !state.native_combat_evidence,
        "first frame records acquisition without inventing movement or damage");

    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kWaitingForEvidence,
        "duplicate asynchronous frame remains evidence-neutral");
    state = actions.State();
    Check(
        state.target_acquisition_events == 1 &&
            state.hp_decrease_events == 0,
        "duplicate frame does not repeat target acquisition or HP evidence");

    snapshot.bots[0].position_x = 0.75f;
    actions.Observe(snapshot, expected);
    Check(
        !actions.State().movement_observed,
        "sub-threshold drift does not satisfy movement evidence");

    snapshot.bots[0].position_x = 1.25f;
    actions.Observe(snapshot, expected);
    Check(
        actions.State().movement_observed &&
            actions.State().moved_pilot_id ==
                0x80000001,
        "the same validated pilot/object/NPC identity moved across frames");

    snapshot.bots[0].target_hp = 90;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kAccepted,
        "target, movement and same-object HP decrease complete evidence");
    state = actions.State();
    Check(
        state.complete_evidence &&
            state.hp_decrease_events == 1 &&
            state.total_hp_decrease == 10 &&
            state.damaged_target_object_id == 900 &&
            state.damaged_target_hp_before == 100 &&
            state.damaged_target_hp_after == 90,
        "native combat witness preserves the same target and HP endpoints");
}

void TestSharedTargetDeduplication()
{
    BattleCombatActions actions;
    const auto expected = Expected(8);
    auto snapshot = Snapshot(8);
    Bot(
        snapshot,
        0,
        0x80000011,
        111,
        211,
        0.0f,
        0.0f,
        0.0f,
        910,
        200);
    Bot(
        snapshot,
        1,
        0x80000012,
        112,
        212,
        10.0f,
        0.0f,
        0.0f,
        910,
        200);
    actions.Observe(snapshot, expected);
    auto state = actions.State();
    Check(
        state.unique_target_count == 1 &&
            state.target_acquisition_events == 1,
        "two AIs sharing one target create one per-frame target baseline");

    snapshot.bots[0].position_x = 2.0f;
    snapshot.bots[1].position_x = 12.0f;
    snapshot.bots[0].target_hp = 180;
    snapshot.bots[1].target_hp = 180;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kAccepted,
        "shared target can complete evidence after both reports agree");
    state = actions.State();
    Check(
        state.moved_identity_count == 2 &&
            state.hp_decrease_events == 1 &&
            state.total_hp_decrease == 20,
        "shared target HP decrease is counted once per frame");

    BattleCombatActions conflict;
    auto conflicting = Snapshot(8);
    Bot(
        conflicting,
        0,
        0x80000021,
        121,
        221,
        0.0f,
        0.0f,
        0.0f,
        920,
        100);
    Bot(
        conflicting,
        1,
        0x80000022,
        122,
        222,
        10.0f,
        0.0f,
        0.0f,
        920,
        80);
    conflict.Observe(conflicting, expected);
    state = conflict.State();
    Check(
        state.unique_target_count == 0 &&
            !state.target_acquired &&
            !state.native_combat_evidence,
        "conflicting HP reports for a shared target are discarded");

    conflicting.bots[0].target_hp = 70;
    conflicting.bots[1].target_hp = 70;
    conflict.Observe(conflicting, expected);
    Check(
        conflict.State().hp_decrease_events == 0,
        "first consistent frame after conflict only establishes a baseline");
    conflicting.bots[0].target_hp = 60;
    conflicting.bots[1].target_hp = 60;
    conflict.Observe(conflicting, expected);
    Check(
        conflict.State().hp_decrease_events == 1 &&
            conflict.State().total_hp_decrease == 10,
        "subsequent consistent shared-target decrease is counted once");
}

void TestContinuityBreaks()
{
    BattleCombatActions actions;
    const auto expected = Expected(9);
    auto snapshot = Snapshot(9);
    Bot(
        snapshot,
        0,
        0x80000031,
        131,
        231,
        0.0f,
        0.0f,
        0.0f,
        930,
        100);
    actions.Observe(snapshot, expected);

    snapshot.bots[0].object_id = 132;
    snapshot.bots[0].npc_id = 232;
    snapshot.bots[0].position_x = 100.0f;
    snapshot.bots[0].target_object_id = 931;
    snapshot.bots[0].target_hp = 20;
    actions.Observe(snapshot, expected);
    auto state = actions.State();
    Check(
        !state.movement_observed &&
            !state.native_combat_evidence,
        "bot identity and target object replacement establish new baselines");

    snapshot.bots[0].target_hp = 40;
    actions.Observe(snapshot, expected);
    Check(
        actions.State().hp_decrease_events == 0,
        "HP recovery or respawn increase is never damage evidence");

    snapshot.bots[0].target_object_id = 0;
    snapshot.bots[0].target_hp = -1;
    actions.Observe(snapshot, expected);
    snapshot.bots[0].target_object_id = 931;
    snapshot.bots[0].target_hp = 15;
    actions.Observe(snapshot, expected);
    Check(
        actions.State().hp_decrease_events == 0,
        "target absence breaks HP continuity before reacquisition");

    snapshot.bots[0].position_x = 102.0f;
    snapshot.bots[0].target_hp = 10;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kAccepted,
        "new continuous life can later provide genuine movement and HP decrease");
    state = actions.State();
    Check(
        state.hp_decrease_events == 1 &&
            state.total_hp_decrease == 5,
        "replacement, increase and absence created no false HP events");
}

void TestInvalidFramesCannotContribute()
{
    BattleCombatActions actions;
    const auto expected = Expected(10);
    auto snapshot = Snapshot(10);
    Bot(
        snapshot,
        0,
        0x80000041,
        141,
        241,
        0.0f,
        0.0f,
        0.0f,
        940,
        100);
    actions.Observe(snapshot, expected);

    snapshot.bots[0].position_x =
        std::numeric_limits<float>::quiet_NaN();
    snapshot.bots[0].target_hp = 50;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kWaitingForValidatedBot,
        "NaN position prevents the row from becoming a validated bot");
    auto state = actions.State();
    Check(
        !state.movement_observed &&
            !state.native_combat_evidence,
        "NaN frame contributes neither movement nor HP decrease");

    snapshot.bots[0].position_x = 100.0f;
    actions.Observe(snapshot, expected);
    Check(
        !actions.State().movement_observed &&
            !actions.State().native_combat_evidence,
        "first valid frame after NaN re-establishes continuity");

    snapshot.bots[0].valid_registered_object = 0;
    snapshot.bots[0].position_x = 200.0f;
    snapshot.bots[0].target_hp = 10;
    actions.Observe(snapshot, expected);
    state = actions.State();
    Check(
        state.validated_bot_count == 0 &&
            !state.movement_observed &&
            !state.native_combat_evidence,
        "unregistered identity cannot contribute acceptance evidence");

    snapshot.bots[0].valid_registered_object = 1;
    snapshot.native_app_state = 6;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kInactiveSnapshot,
        "non-GAME_MAIN snapshot is not accepted");
    Check(
        !actions.State().current_frame_valid,
        "inactive snapshot is explicitly marked invalid");

    snapshot.native_app_state = 22;
    snapshot.room_title[0] = L'X';
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kForeignBattle,
        "foreign room title is rejected");
    state = actions.State();
    Check(
        !state.target_acquired &&
            !state.movement_observed &&
            !state.native_combat_evidence,
        "foreign room clears accumulated room evidence");
}

void TestGenerationAndRoomReset()
{
    BattleCombatActions actions;
    const auto firstExpected = Expected(11);
    auto first = Snapshot(11);
    Bot(
        first,
        0,
        0x80000051,
        151,
        251,
        0.0f,
        0.0f,
        0.0f,
        950,
        100);
    actions.Observe(first, firstExpected);
    first.bots[0].position_x = 2.0f;
    actions.Observe(first, firstExpected);
    Check(
        actions.State().target_acquired &&
            actions.State().movement_observed,
        "first generation accumulated partial evidence");

    const auto secondExpected =
        Expected(12, 3, L"combat-next");
    auto second = Snapshot(12, 3, L"combat-next");
    Bot(
        second,
        0,
        0x80000051,
        151,
        251,
        50.0f,
        0.0f,
        0.0f,
        950,
        1);
    actions.Observe(second, secondExpected);
    auto state = actions.State();
    Check(
        state.target_acquired &&
            !state.movement_observed &&
            !state.native_combat_evidence &&
            state.hp_decrease_events == 0,
        "new expected room generation resets movement and HP continuity");

    Check(
        actions.Observe(first, secondExpected) ==
            BattleCombatActions::kForeignBattle,
        "stale snapshot from the old battle is rejected");
    state = actions.State();
    Check(
        !state.target_acquired &&
            !state.complete_evidence,
        "generation mismatch clears evidence instead of accepting stale state");
}

void TestLuaErrorsLatch()
{
    BattleCombatActions actions;
    const auto expected = Expected(13);
    auto snapshot = Snapshot(13);
    Bot(
        snapshot,
        0,
        0x80000061,
        161,
        261,
        0.0f,
        0.0f,
        0.0f,
        960,
        100);
    actions.Observe(snapshot, expected);
    snapshot.bots[0].position_x = 2.0f;
    snapshot.bots[0].target_hp = 90;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kAccepted,
        "control battle reaches complete native evidence");

    snapshot.lua_error_count = 1;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kLuaError,
        "nonzero accumulated Lua error rejects the battle");
    auto state = actions.State();
    Check(
        state.lua_error_latched &&
            !state.complete_evidence &&
            state.last_lua_error_count == 1,
        "Lua error is latched and revokes complete evidence");

    snapshot.lua_error_count = 0;
    Check(
        actions.Observe(snapshot, expected) ==
            BattleCombatActions::kLuaError,
        "counter regression cannot erase a latched same-generation Lua error");

    auto next = Snapshot(14);
    Bot(
        next,
        0,
        0x80000062,
        162,
        262,
        0.0f,
        0.0f,
        0.0f,
        970,
        100);
    actions.Observe(next, Expected(14));
    Check(
        !actions.State().lua_error_latched &&
            actions.State().target_acquired,
        "new battle generation resets the prior battle's Lua latch");

    BattleCombatActions unavailable;
    auto noObserver = Snapshot(15);
    Bot(
        noObserver,
        0,
        0x80000063,
        163,
        263,
        0.0f,
        0.0f,
        0.0f,
        980,
        100);
    noObserver.lua_error_observer_available = 0;
    Check(
        unavailable.Observe(noObserver, Expected(15)) ==
            BattleCombatActions::kLuaObserverUnavailable,
        "unavailable accumulated Lua observation cannot accept a frame");
    Check(
        !unavailable.State().target_acquired,
        "unobserved Lua frame contributes no combat evidence");
}

void TestStructuralAndIdentityConflicts()
{
    BattleCombatActions actions;
    auto snapshot = Snapshot(16);
    snapshot.size = sizeof(snapshot) - 1;
    Check(
        actions.Observe(snapshot, Expected(16)) ==
            BattleCombatActions::kInvalidSnapshot,
        "short ABI snapshot fails closed");

    snapshot = Snapshot(16);
    Bot(
        snapshot,
        0,
        0x80000071,
        171,
        271,
        0.0f,
        0.0f,
        0.0f,
        990,
        100);
    Bot(
        snapshot,
        1,
        0x80000071,
        172,
        272,
        10.0f,
        0.0f,
        0.0f,
        991,
        100);
    Check(
        actions.Observe(snapshot, Expected(16)) ==
            BattleCombatActions::kWaitingForValidatedBot,
        "same pilot with competing object identity invalidates both rows");
    Check(
        actions.State().validated_bot_count == 0 &&
            !actions.State().target_acquired,
        "identity conflict cannot create target evidence");
}
} // namespace

int main()
{
    TestAsynchronousFrames();
    TestSharedTargetDeduplication();
    TestContinuityBreaks();
    TestInvalidFramesCannotContribute();
    TestGenerationAndRoomReset();
    TestLuaErrorsLatch();
    TestStructuralAndIdentityConflicts();

    std::printf(
        "SUMMARY: %s (%d checks, %d failure%s)\n",
        failures ? "FAIL" : "PASS",
        checks,
        failures,
        failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
