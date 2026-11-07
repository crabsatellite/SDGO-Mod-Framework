#pragma once
#include "sdgo/battle_ai_observer.h"

// Performance samples have no idle-duration acceptance gate. They retain the
// native shots, targets, control and displacement needed to detect a benchmark
// that became cheaper merely because the bots stopped doing work.
class BattleAiPerformanceProbe {
    uint64_t next_=0;
    SdgoBattleAiGetUnitSnapshotV1Fn get_=nullptr;
public:
    void Observe(const SdgoGameTestContextV1* context,const SdgoPvpAiClientSnapshotV1& battle) {
        if(!context->api->SettingInt(sdgo_test::mod,"BattleAiPerformance",0))return;
        const auto now=GetTickCount64();if(now<next_)return;next_=now+1000;
        if(!get_)get_=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(GetProcAddress(
            GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        if(!get_){context->Observe("AI performance observer unavailable");return;}
        for(uint32_t i=0;i<battle.bot_count&&i<12;++i) {
            SdgoBattleAiUnitSnapshotV1 value{};value.size=sizeof(value);value.version=1;
            if(!get_(battle.bots[i].object_id,&value))continue;
            char line[512];sprintf_s(line,
                "AI performance sample slot=%u pilot=%u object=%u model=%u generation=%llu hp=%d shots=%llu attempts=%llu target=%u visible=%u controlled=%u moving=%u x=%.3f y=%.3f z=%.3f attack=%u movement=%u",
                i,battle.bots[i].pilot_id,value.object_id,value.unit_id,static_cast<unsigned long long>(value.generation),
                value.hp,static_cast<unsigned long long>(value.ranged_shots),static_cast<unsigned long long>(value.ranged_attempts),
                value.target_id,value.target_visible,value.control_blocked,value.moving,value.x,value.y,value.z,value.attack_state,value.movement_state);
            context->Observe(line);
        }
    }
};
