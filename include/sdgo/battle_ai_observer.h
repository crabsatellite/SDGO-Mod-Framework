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

// Independent passive ABI for directed native map-action navigation.
#define SDGO_BATTLE_AI_TRANSPORT_OBSERVER_EXPORT "SdgoBattleAiGetTransportSnapshotV1"
struct SdgoBattleAiTransportSnapshotV1 {
    uint32_t size,version,object_id,phase;
    uint64_t generation;
    uint32_t catalog_count,catalog_complete,catalog_queries,catalog_saturated;
    uint64_t selected,activations,completed,rejected,resumed;
    uint32_t link_id,reserved;
    float entry_x,entry_y,entry_z,exit_x,exit_y,exit_z;
    float activated_x,activated_y,activated_z,arrived_x,arrived_y,arrived_z;
    uint32_t usable_links,ground_rejected,corridor_rejected,search_nodes;
    float sphere_x,sphere_y,sphere_z,sphere_radius;
    float pad_min_x,pad_min_y,pad_min_z,pad_max_x,pad_max_y,pad_max_z;
    float owner_min_x,owner_min_y,owner_min_z,owner_max_x,owner_max_y,owner_max_z;
    uint32_t native_overlap,query_contains_pad,query_count,contact_available;
};
typedef int(__cdecl* SdgoBattleAiGetTransportSnapshotV1Fn)(uint32_t,SdgoBattleAiTransportSnapshotV1*);

// Independent read-only ABI for native map phase Hide/Unhide consumption.
#define SDGO_BATTLE_AI_PHASE_GEOMETRY_OBSERVER_EXPORT "SdgoBattleAiGetPhaseGeometrySnapshotV1"
struct SdgoBattleAiPhaseGeometrySnapshotV1 {
    uint32_t size,version;
    uint64_t global_revision,hide_count,unhide_count,last_event_sequence;
    uint32_t last_object_id,last_action;
    uint32_t actor_valid,route_active;
    uint64_t actor_generation,actor_seen_revision;
    uint64_t actor_invalidation_count,actor_route_rebuild_count;
    uint64_t actor_path_generation;
};
typedef int(__cdecl* SdgoBattleAiGetPhaseGeometrySnapshotV1Fn)(
    uint32_t,SdgoBattleAiPhaseGeometrySnapshotV1*);
