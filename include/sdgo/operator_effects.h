#pragma once

#include <stddef.h>
#include <stdint.h>

#include "operator_reward_filter.h"

#define SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION 1u
#define SDGO_OPERATOR_EFFECTS_EXTENSION_EXPORT \
    "SdgoOperatorEffectsRegisterExtensionHandlerV1"
#define SDGO_OPERATOR_EFFECTS_MAX_EXTENSION_JSON 16384u

enum SdgoOperatorEffectExtensionStatusV1 {
    SDGO_OPERATOR_EFFECT_EXTENSION_REJECT = -1,
    SDGO_OPERATOR_EFFECT_EXTENSION_NO_CHANGE = 0,
    SDGO_OPERATOR_EFFECT_EXTENSION_APPLY = 1,
};

#pragma pack(push, 4)

/*
 * ExtensionsHex is decoded as one UTF-8 JSON object. Every top-level key is
 * a required namespace. Data alone never executes: a loaded trusted Mod must
 * register the exact namespace before the profile can apply.
 *
 * reward is valid only for the callback. payload_json is the complete JSON
 * value associated with extension_namespace and is not NUL-terminated.
 * current_* include built-in operator effects and earlier trusted extensions.
 */
typedef struct SdgoOperatorEffectExtensionContextV1 {
    uint32_t size;
    uint32_t version;
    const SdgoOperatorRewardContextV1* reward;
    uint32_t operator_number;
    const char* extension_namespace;
    uint32_t extension_namespace_size;
    const char* payload_json;
    uint32_t payload_json_size;
    uint32_t writable_mask;
    uint32_t current_pilot_exp;
    uint32_t current_unit_exp;
    uint32_t current_gp;
} SdgoOperatorEffectExtensionContextV1;

typedef struct SdgoOperatorEffectExtensionResultV1 {
    uint32_t size;
    uint32_t version;
    uint32_t write_mask;
    uint32_t target_pilot_exp;
    uint32_t target_unit_exp;
    uint32_t target_gp;
} SdgoOperatorEffectExtensionResultV1;

typedef int(__stdcall* SdgoOperatorEffectExtensionCallbackV1)(
    const SdgoOperatorEffectExtensionContextV1* context,
    SdgoOperatorEffectExtensionResultV1* result,
    void* user);

typedef struct SdgoOperatorEffectExtensionSinkV1 {
    uint32_t size;
    uint32_t version;
    const char* extension_namespace;
    uint32_t extension_namespace_size;
    SdgoOperatorEffectExtensionCallbackV1 callback;
    void* user;
} SdgoOperatorEffectExtensionSinkV1;

#pragma pack(pop)

typedef int(__cdecl* SdgoOperatorEffectsRegisterExtensionHandlerV1Fn)(
    const SdgoOperatorEffectExtensionSinkV1* sink);

#ifdef __cplusplus
static_assert(
    sizeof(SdgoOperatorEffectExtensionContextV1) ==
        36 + sizeof(void*) + 2 * sizeof(const char*),
    "operator effect extension context layout changed");
static_assert(
    sizeof(SdgoOperatorEffectExtensionResultV1) == 24,
    "operator effect extension result layout changed");
static_assert(
    sizeof(SdgoOperatorEffectExtensionSinkV1) ==
        12 + 2 * sizeof(void*) +
        sizeof(SdgoOperatorEffectExtensionCallbackV1),
    "operator effect extension sink layout changed");
#endif
