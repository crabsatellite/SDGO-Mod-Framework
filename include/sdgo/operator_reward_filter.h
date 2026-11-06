#pragma once

#include <stddef.h>
#include <stdint.h>

#define SDGO_OPERATOR_REWARD_FILTER_VERSION 1u
#define SDGO_OPERATOR_REWARD_FILTER_EXPORT \
    "SdgoBattleRewardsRegisterOperatorRewardFilterV1"

enum SdgoOperatorRewardPhaseV1 {
    SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT = 1,
    SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS = 2,
};

enum SdgoOperatorRewardFieldV1 {
    SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP = 1u << 0,
    SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP = 1u << 1,
    SDGO_OPERATOR_REWARD_FIELD_GP = 1u << 2,
};

enum SdgoOperatorRewardGpPolicyV1 {
    SDGO_OPERATOR_REWARD_GP_NATIVE = 0,
    SDGO_OPERATOR_REWARD_GP_REPLACE = 1,
};

enum SdgoOperatorRewardRoleV1 {
    SDGO_OPERATOR_REWARD_ROLE_HUMAN = 0,
    SDGO_OPERATOR_REWARD_ROLE_AI = 1,
};

enum SdgoOperatorRewardOutcomeV1 {
    SDGO_OPERATOR_REWARD_OUTCOME_DRAW = 0,
    SDGO_OPERATOR_REWARD_OUTCOME_WIN_OR_CLEAR = 1,
    SDGO_OPERATOR_REWARD_OUTCOME_LOSE_OR_FAIL = 2,
};

enum SdgoOperatorRewardFilterResultV1 {
    SDGO_OPERATOR_REWARD_FILTER_REJECT = -1,
    SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE = 0,
    SDGO_OPERATOR_REWARD_FILTER_APPLY = 1,
};

#pragma pack(push, 4)

/*
 * The frame and native pointers are valid only for the duration of callback.
 * operator_definition is the nullable opaque pointer cached at
 * battle_pilot+0x50. Its internal ID layout is deliberately not part of this
 * ABI.
 *
 * native_* are the values found in the original settlement frame at this
 * phase. current_* include an already prepared contribution-GP replacement,
 * when applicable. Only fields in writable_mask may be returned in write_mask.
 */
typedef struct SdgoOperatorRewardContextV1 {
    uint32_t size;
    uint32_t version;
    uint32_t site;
    uint32_t phase;
    uint32_t writable_mask;
    void* frame;
    void* room;
    void* room_pilot;
    void* battle_pilot;
    void* operator_definition;
    uint32_t account;
    uint32_t participant_role;
    uint32_t outcome;
    uint32_t won;
    uint32_t native_eligible;
    uint32_t gp_policy;
    uint32_t native_pilot_exp;
    uint32_t native_pilot_exp_event;
    uint32_t native_unit_exp;
    uint32_t native_unit_exp_event;
    uint32_t native_gp;
    uint32_t native_gp_event;
    uint32_t current_pilot_exp;
    uint32_t current_pilot_exp_event;
    uint32_t current_unit_exp;
    uint32_t current_unit_exp_event;
    uint32_t current_gp;
    uint32_t current_gp_event;
} SdgoOperatorRewardContextV1;

typedef struct SdgoOperatorRewardResultV1 {
    uint32_t size;
    uint32_t version;
    uint32_t write_mask;
    uint32_t target_pilot_exp;
    uint32_t target_unit_exp;
    uint32_t target_gp;
} SdgoOperatorRewardResultV1;

typedef int(__stdcall* SdgoOperatorRewardFilterCallbackV1)(
    const SdgoOperatorRewardContextV1* context,
    SdgoOperatorRewardResultV1* result,
    void* user);

typedef struct SdgoOperatorRewardFilterSinkV1 {
    uint32_t size;
    uint32_t version;
    SdgoOperatorRewardFilterCallbackV1 callback;
    void* user;
} SdgoOperatorRewardFilterSinkV1;

#pragma pack(pop)

typedef int(__cdecl* SdgoBattleRewardsRegisterOperatorRewardFilterV1Fn)(
    const SdgoOperatorRewardFilterSinkV1* sink);

#ifdef __cplusplus
static_assert(
    offsetof(SdgoOperatorRewardContextV1, operator_definition) ==
        20 + 4 * sizeof(void*),
    "operator reward context pointer layout changed");
static_assert(
    offsetof(SdgoOperatorRewardContextV1, account) ==
        20 + 5 * sizeof(void*),
    "operator reward context scalar layout changed");
static_assert(
    sizeof(SdgoOperatorRewardContextV1) == 92 + 5 * sizeof(void*),
    "operator reward context size changed");
static_assert(
    sizeof(SdgoOperatorRewardResultV1) == 24,
    "operator reward result size changed");
static_assert(
    sizeof(SdgoOperatorRewardFilterSinkV1) ==
        8 + sizeof(SdgoOperatorRewardFilterCallbackV1) + sizeof(void*),
    "operator reward filter sink size changed");
#endif
