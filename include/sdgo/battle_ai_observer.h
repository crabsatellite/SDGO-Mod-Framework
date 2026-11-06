#pragma once
#include <stdint.h>

// Optional main-thread, read-only native battle observations for GameTest.
#define SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT "SdgoBattleAiGetUnitSnapshotV1"
struct SdgoBattleAiUnitSnapshotV1 {
    uint32_t size, version, object_id, unit_id;
    uint64_t generation, ranged_shots, special_finished;
    int32_t hp, selected_slot;
    uint32_t hurt_state, captured_by, control_blocked, special_busy, ai_enabled;
    uint32_t target_id, target_visible, defence_moving, moving_shot, moving;
    float target_distance, weapon_range, max_ordinary_range;
    float target_x,target_y,target_z;
    uint32_t visible_far_targets;
    float x, y, z;
    uint32_t attack_state, attack_pending, movement_state, movement_pending;
    uint32_t special_end_event, special_result_applied, special_result_victim, special_result_damage, special_owner_flag;
    uint32_t special_zero_completions;
    uint64_t ranged_attempts;
    uint64_t ranged_nonzero_returns;
    uint64_t mobile_primary_dispatches;
    uint64_t mobile_defensive_dispatches;
    uint64_t backward_primary_dispatches;
};
typedef int(__cdecl* SdgoBattleAiGetUnitSnapshotV1Fn)(uint32_t, SdgoBattleAiUnitSnapshotV1*);

// Separate read-only ABI so the published 208-byte unit snapshot stays intact.
#define SDGO_BATTLE_AI_NAVIGATION_OBSERVER_EXPORT "SdgoBattleAiGetNavigationSnapshotV1"
struct SdgoBattleAiNavigationSnapshotV1 {
    uint32_t size,version,object_id,shared_target,shared_source,search_goal_kind;
    uint64_t generation,shared_contact_frames,shared_pursuit_frames,exploration_routes;
    float goal_x,goal_y,goal_z;
};
typedef int(__cdecl* SdgoBattleAiGetNavigationSnapshotV1Fn)(uint32_t,SdgoBattleAiNavigationSnapshotV1*);

// Passive receipt for a genuine mission Boss special-start notice already
// consumed by the original client handler. This does not drive AI, HP, input,
// packets or mission state.
#define SDGO_BATTLE_AI_MISSION_BOSS_SPECIAL_EXPORT \
    "SdgoBattleAiGetMissionBossSpecialV1"
struct SdgoBattleAiMissionBossSpecialV1 {
    uint32_t size,version;
    uint64_t sequence,battle_epoch,event_tick_ms;
    uint32_t mission_object,unit,pilot_id,object_id,npc_data_id,weapon_id;
    uint32_t native_app_state;
    int32_t hp_before,max_hp_before,hp_after,max_hp_after,original_result;
    uint32_t state_before_valid,state_after_valid;
    uint32_t special_busy_before,special_busy_after;
    uint32_t attack_state_before,attack_state_after;
    uint32_t original_returned;
};
typedef int(__cdecl* SdgoBattleAiGetMissionBossSpecialV1Fn)(
    SdgoBattleAiMissionBossSpecialV1*);

#ifdef __cplusplus
static_assert(
    sizeof(SdgoBattleAiMissionBossSpecialV1) == 112,
    "unexpected mission Boss special observer ABI");
#endif
