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
        uint32_t casterState=0,casterPending=0,casterEndEvent=0,casterApplied=0,casterResult=0,casterReady=0,casterZeroCompletions=0;
    };
    struct Totals {
        uint64_t shots=0,attempts=0,zeroCompletions=0;
        uint64_t mobileDispatches=0,defensiveDispatches=0,backwardDispatches=0;
        uint32_t defenceFrames=0,defenceDisplacements=0,mobileGunFrames=0,defenceShots=0;
        uint32_t blockedDefenceFrames=0,specialDefenceFrames=0,neutralDefenceFrames=0,backwardShotFrames=0;
    };
    std::array<Unit,12> units_{};
    std::array<Totals,12> totals_{};
    uint32_t sequence_=0,captures_=0,recovered_=0,retreatShots_=0,pursuits_=0,frames_=0;
    SdgoBattleAiGetUnitSnapshotV1Fn get_=nullptr;
public:
    bool Observe(const SdgoGameTestContextV1* context,const SdgoPvpAiClientSnapshotV1& battle) {
        if(!context->api->SettingInt(sdgo_test::mod,"BattleAiBehavior",0)) return true;
        if(!get_) get_=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        if(!get_) {context->Assert(0,"the candidate battle AI read-only observer is loaded");return false;}
        if(sequence_!=battle.received_battle_start_count) {
            sequence_=battle.received_battle_start_count;units_={};totals_={};captures_=recovered_=retreatShots_=pursuits_=frames_=0;
        }
        ++frames_;
        for(uint32_t i=0;i<battle.bot_count&&i<12;++i) {
            SdgoBattleAiUnitSnapshotV1 now{};now.size=sizeof(now);now.version=1;
            if(!get_(battle.bots[i].object_id,&now)) continue;
            auto& state=units_[i];const auto before=state.last;
            auto& total=totals_[i];
            const bool sameLife=before.object_id==now.object_id&&before.generation==now.generation;
            const uint64_t shots=sameLife&&now.ranged_shots>=before.ranged_shots?now.ranged_shots-before.ranged_shots:now.ranged_shots;
            total.shots+=shots;
            total.attempts+=sameLife&&now.ranged_attempts>=before.ranged_attempts?
                now.ranged_attempts-before.ranged_attempts:now.ranged_attempts;
            total.mobileDispatches+=sameLife&&now.mobile_primary_dispatches>=before.mobile_primary_dispatches?
                now.mobile_primary_dispatches-before.mobile_primary_dispatches:now.mobile_primary_dispatches;
            total.defensiveDispatches+=sameLife&&now.mobile_defensive_dispatches>=before.mobile_defensive_dispatches?
                now.mobile_defensive_dispatches-before.mobile_defensive_dispatches:now.mobile_defensive_dispatches;
            total.backwardDispatches+=sameLife&&now.backward_primary_dispatches>=before.backward_primary_dispatches?
                now.backward_primary_dispatches-before.backward_primary_dispatches:now.backward_primary_dispatches;
            total.zeroCompletions+=sameLife&&now.special_zero_completions>=before.special_zero_completions?
                now.special_zero_completions-before.special_zero_completions:now.special_zero_completions;
            if(now.defence_moving) {
                ++total.defenceFrames;
                if(now.control_blocked) ++total.blockedDefenceFrames;
                if(now.special_busy||now.selected_slot==3) ++total.specialDefenceFrames;
                if(!now.control_blocked&&!now.special_busy&&now.selected_slot!=3&&now.attack_state==0x20)
                    ++total.neutralDefenceFrames;
                if(now.moving_shot) ++total.mobileGunFrames;
                total.defenceShots+=static_cast<uint32_t>(shots);
                const float dx=now.x-before.x,dz=now.z-before.z;
                if(sameLife&&dx*dx+dz*dz>0.0001f) ++total.defenceDisplacements;
            }
            if(sameLife&&shots&&now.hp>0&&now.moving_shot&&now.moving&&before.moving&&
               !now.control_blocked&&!before.control_blocked&&!now.special_busy&&!before.special_busy&&
               now.selected_slot>=0&&now.selected_slot<3&&now.selected_slot==before.selected_slot&&
               now.target_visible&&now.target_id==before.target_id) {
                const float dx=now.x-before.x,dz=now.z-before.z;
                if(dx*dx+dz*dz>0.0001f&&dx*(before.target_x-before.x)+dz*(before.target_z-before.z)<0)
                    ++total.backwardShotFrames;
            }
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
                    state.casterResult!=caster.special_result_victim||state.casterReady!=caster.special_result_damage||
                    state.casterZeroCompletions!=caster.special_zero_completions)) {
                    char text[288];sprintf_s(text,"native captured victim=%u caster=%u state=%X/%X end_event=%u result_applied=%u result_victim=%u result_damage=%u owner_flag=%u finished=%llu zero_completions=%u",
                        now.object_id,state.caster,caster.attack_state,caster.attack_pending,caster.special_end_event,
                        caster.special_result_applied,caster.special_result_victim,caster.special_result_damage,caster.special_owner_flag,
                        static_cast<unsigned long long>(caster.special_finished),caster.special_zero_completions);context->Observe(text);
                    state.casterState=caster.attack_state;state.casterPending=caster.attack_pending;state.casterEndEvent=caster.special_end_event;
                    state.casterApplied=caster.special_result_applied;state.casterResult=caster.special_result_victim;
                    state.casterReady=caster.special_result_damage;
                    state.casterZeroCompletions=caster.special_zero_completions;
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
        uint32_t stuck=0,unrecovered=0,zeroCompletions=0;
        uint64_t mobileDispatches=0,defensiveDispatches=0,backwardDispatches=0;
        for(size_t i=0;i<units_.size();++i) {
            const auto& unit=units_[i];const auto& total=totals_[i];
            if(unit.captured&&unit.last.hp>0) ++stuck;
            if(unit.awaitingRecovery&&unit.last.hp>0) ++unrecovered;
            zeroCompletions+=static_cast<uint32_t>(total.zeroCompletions);
            mobileDispatches+=total.mobileDispatches;defensiveDispatches+=total.defensiveDispatches;
            backwardDispatches+=total.backwardDispatches;
            if(unit.last.object_id) {
                char detail[448];sprintf_s(detail,"native battle AI unit=%u unit_data=%u shots=%llu attempts=%llu defence_frames=%u defence_displacements=%u mobile_gun_frames=%u defence_shots=%u blocked_defence_frames=%u special_defence_frames=%u neutral_defence_frames=%u physical_backward_shot_frames=%u last_slot=%d last_attack=%X/%X last_movement=%X/%X",
                    unit.last.object_id,unit.last.unit_id,static_cast<unsigned long long>(total.shots),static_cast<unsigned long long>(total.attempts),total.defenceFrames,
                    total.defenceDisplacements,total.mobileGunFrames,total.defenceShots,total.blockedDefenceFrames,
                    total.specialDefenceFrames,total.neutralDefenceFrames,total.backwardShotFrames,unit.last.selected_slot,
                    unit.last.attack_state,unit.last.attack_pending,unit.last.movement_state,unit.last.movement_pending);context->Observe(detail);
            }
        }
        char text[288];sprintf_s(text,"native battle AI behavior frames=%u captures=%u recovered=%u live_captured=%u live_unrecovered=%u defensive_shot_frames=%u far_pursuit_frames=%u zero_completions=%u",
            frames_,captures_,recovered_,stuck,unrecovered,retreatShots_,pursuits_,zeroCompletions);context->Observe(text);
        context->Assert(captures_>0&&recovered_>0&&stuck==0&&unrecovered==0,
            "a genuine special victim survived native release, resumed movement or shooting, and no live AI remained captured at settlement");
        char dispatches[256];sprintf_s(dispatches,"native post-ammo primary dispatches mobile=%llu defensive=%llu backward=%llu",
            static_cast<unsigned long long>(mobileDispatches),static_cast<unsigned long long>(defensiveDispatches),
            static_cast<unsigned long long>(backwardDispatches));context->Observe(dispatches);
        context->Assert(defensiveDispatches>0||backwardDispatches>0,
            "a moving-shot weapon reached its original post-ammo firing dispatch with native control available while on a defensive route or physically backing away from its opponent");
        context->Assert(pursuits_>0,"native observations show an AI closing on a visible enemy beyond its ordinary weapon reach");
    }
};
