#pragma once

#include <stdint.h>

#define SDGO_PVE_AI_SCOREBOARD_STATUS_VERSION 3
#define SDGO_PVE_AI_SCOREBOARD_STATUS_EXPORT "SdgoPveAiScoreboardGetStatusV1"

struct SdgoPveAiScoreboardStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t active;
    uint32_t human_rows;
    uint32_t ai_rows;
    uint32_t total_rows;
    uint32_t sync_count;
    uint32_t render_count;
    uint32_t rejected_count;
    uint32_t panel_visible;
    uint32_t panel_state;
    uint32_t visible_render_count;
    uint32_t visible_text_render_count;
    uint32_t named_ai_rows;
    uint32_t hp_resolve_count;
};

static_assert(sizeof(SdgoPveAiScoreboardStatusV1) == 60,
              "unexpected PvE AI scoreboard status ABI");

typedef int(__cdecl* SdgoPveAiScoreboardGetStatusV1Fn)(
    SdgoPveAiScoreboardStatusV1* status);
