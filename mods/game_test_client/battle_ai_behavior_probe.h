#pragma once
#include "sdgo/battle_ai_observer.h"
#include <array>
#include <cmath>

// Opt-in observations only. No SP, position, input, hurt-state or packet writes.
class BattleAiBehaviorProbe {
    struct Unit {
        SdgoBattleAiUnitSnapshotV1 last{};
        uint32_t caster=0;
        bool captured=false,awaitingRecovery=false;
        float releaseX=0,releaseZ=0;
        uint64_t releaseShots=0;
        uint32_t casterState=0,casterPending=0,casterEndEvent=0,casterApplied=0,casterResult=0,casterReady=0;
    };
    std::array<Unit,12> units_{};
    uint32_t sequence_=0,captures_=0,recovered_=0,retreatShots_=0,pursuits_=0,frames_=0;
    SdgoBattleAiGetUnitSnapshotV1Fn get_=nullptr;
public:
    bool Observe(const SdgoGameTestContextV1* context,const SdgoPvpAiClientSnapshotV1& battle) {
        if(!context->api->SettingInt(sdgo_test::mod,"BattleAiBehavior",0)) return true;
        if(!get_) get_=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        if(!get_) {context->Assert(0,"the candidate battle AI read-only observer is loaded");return false;}
        if(sequence_!=battle.received_battle_start_count) {
            sequence_=battle.received_battle_start_count;units_={};captures_=recovered_=retreatShots_=pursuits_=frames_=0;
        }
        ++frames_;
        for(uint32_t i=0;i<battle.bot_count&&i<12;++i) {
            SdgoBattleAiUnitSnapshotV1 now{};now.size=sizeof(now);now.version=1;
            if(!get_(battle.bots[i].object_id,&now)) continue;
            auto& state=units_[i];const auto before=state.last;
            if(before.object_id!=now.object_id||before.generation!=now.generation) state={};
            if(now.hurt_state==4&&!state.captured) {
                state.captured=true;state.caster=now.captured_by;++captures_;
                char text[192];sprintf_s(text,"native special victim=%u caster=%u hp=%d ai=%u movement=%X/%X attack=%X/%X",
                    now.object_id,now.captured_by,now.hp,now.ai_enabled,now.movement_state,now.movement_pending,now.attack_state,now.attack_pending);context->Observe(text);
            }
            if(state.captured&&now.hurt_state!=4) {
                state.captured=false;state.awaitingRecovery=now.hp>0;
                state.releaseX=now.x;state.releaseZ=now.z;state.releaseShots=now.ranged_shots;
                char text[128];sprintf_s(text,"native special victim=%u released to hurt_state=%u hp=%d",now.object_id,now.hurt_state,now.hp);context->Observe(text);
            }
            if(state.captured&&state.caster) {
                SdgoBattleAiUnitSnapshotV1 caster{};caster.size=sizeof(caster);caster.version=1;
                if(get_(state.caster,&caster)&&
                   (state.casterState!=caster.attack_state||state.casterPending!=caster.attack_pending||
                    state.casterEndEvent!=caster.special_end_event||state.casterApplied!=caster.special_result_applied||
                    state.casterResult!=caster.special_result_victim||state.casterReady!=caster.special_result_ready)) {
                    char text[256];sprintf_s(text,"native captured victim=%u caster=%u state=%X/%X end_event=%u result_applied=%u result_victim=%u ready=%u owner_flag=%u finished=%llu",
                        now.object_id,state.caster,caster.attack_state,caster.attack_pending,caster.special_end_event,
                        caster.special_result_applied,caster.special_result_victim,caster.special_result_ready,caster.special_owner_flag,
                        static_cast<unsigned long long>(caster.special_finished));context->Observe(text);
                    state.casterState=caster.attack_state;state.casterPending=caster.attack_pending;state.casterEndEvent=caster.special_end_event;
                    state.casterApplied=caster.special_result_applied;state.casterResult=caster.special_result_victim;
                    state.casterReady=caster.special_result_ready;
                }
            }
            if(state.awaitingRecovery&&now.hp>0&&!now.control_blocked) {
                const float dx=now.x-state.releaseX,dz=now.z-state.releaseZ;
                if(dx*dx+dz*dz>900||now.ranged_shots>state.releaseShots) {
                    ++recovered_;state.awaitingRecovery=false;
                    char text[128];sprintf_s(text,"native special victim=%u resumed movement or a real ranged shot",now.object_id);context->Observe(text);
                }
            }
            if(before.object_id==now.object_id&&before.generation==now.generation&&now.hp>0) {
                const float dx=now.x-before.x,dz=now.z-before.z;
                if(now.ranged_shots>before.ranged_shots&&now.moving_shot&&(now.moving||before.moving)&&
                   dx*dx+dz*dz>0.0001f&&(now.defence_moving||before.defence_moving)) ++retreatShots_;
                if(now.visible_far_targets&&now.target_visible&&now.target_id==before.target_id&&
                   now.target_distance>now.max_ordinary_range&&dx*dx+dz*dz>1&&
                   dx*(before.target_x-before.x)+dz*(before.target_z-before.z)>0&&
                   now.target_distance+1<before.target_distance) ++pursuits_;
            }
            state.last=now;
        }
        return true;
    }
    void Finish(const SdgoGameTestContextV1* context) const {
        if(!context->api->SettingInt(sdgo_test::mod,"BattleAiBehavior",0)) return;
        uint32_t stuck=0;
        for(const auto& unit:units_) if(unit.captured&&unit.last.hp>0) ++stuck;
        char text[224];sprintf_s(text,"native battle AI behavior frames=%u captures=%u recovered=%u live_captured=%u defensive_shot_frames=%u far_pursuit_frames=%u",
            frames_,captures_,recovered_,stuck,retreatShots_,pursuits_);context->Observe(text);
        context->Assert(captures_>0&&recovered_>0&&stuck==0,
            "a genuine special victim survived native release, resumed movement or shooting, and no live AI remained captured at settlement");
        context->Assert(retreatShots_>0,"a mobile ranged weapon produced actual native shots during defensive movement");
        context->Assert(pursuits_>0,"native observations show an AI closing on a visible enemy beyond its ordinary weapon reach");
    }
};
