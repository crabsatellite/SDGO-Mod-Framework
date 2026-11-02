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
    uint32_t special_end_event, special_result_applied, special_result_victim, special_result_ready, special_owner_flag;
};
typedef int(__cdecl* SdgoBattleAiGetUnitSnapshotV1Fn)(uint32_t, SdgoBattleAiUnitSnapshotV1*);
