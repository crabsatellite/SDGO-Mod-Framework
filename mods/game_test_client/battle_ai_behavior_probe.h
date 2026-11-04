#pragma once
#include "sdgo/battle_ai_observer.h"
#include "battle_ai_activity_clock.h"
#include "../battle_ai/native_capabilities.h"
#include "../match_ai/forced_pad_observer.h"
#include "../match_ai/gate_status_observer.h"
#include <array>
#include <cmath>

// Opt-in observations only. No SP, position, input, hurt-state or packet writes.
class BattleAiBehaviorProbe {
    struct Unit {
        SdgoBattleAiUnitSnapshotV1 last{};
        SdgoBattleAiNavigationSnapshotV1 navigation{};
        uint32_t caster=0;
        bool captured=false,awaitingRecovery=false;
        float releaseX=0,releaseZ=0;
        uint64_t releaseShots=0;
        uint32_t casterState=0,casterPending=0,casterEndEvent=0,casterApplied=0,casterResult=0,casterReady=0,casterZeroCompletions=0;
        BattleAiActivityClock activity{};
        uint64_t idleReportedAt=0,missingSince=0,lastSnapshotAt=0;
    };
    struct Totals {
        uint64_t shots=0,opponentShots=0,attempts=0,zeroCompletions=0;
        uint64_t mobileDispatches=0,defensiveDispatches=0,backwardDispatches=0;
        uint32_t defenceFrames=0,defenceDisplacements=0,mobileGunFrames=0,defenceShots=0;
        uint32_t blockedDefenceFrames=0,specialDefenceFrames=0,neutralDefenceFrames=0,backwardShotFrames=0;
        uint32_t targetFrames=0,movementFrames=0,attackFrames=0,meleeSwingFrames=0;
        uint64_t specialCompletions=0;
        uint32_t forcedFrames=0,forcedMovementFrames=0;
        uint64_t maxIdleMs=0,maxSampleGapMs=0,maxMissingMs=0;
        uint64_t sharedFrames=0,sharedPursuitFrames=0,explorationRoutes=0;
    };
    std::array<Unit,12> units_{};
    std::array<Totals,12> totals_{};
    uint32_t sequence_=0,captures_=0,recovered_=0,retreatShots_=0,pursuits_=0,frames_=0;
    SdgoBattleAiGetUnitSnapshotV1Fn get_=nullptr;
    SdgoBattleAiGetNavigationSnapshotV1Fn getNavigation_=nullptr;
    SdgoPvpAiClientGetForcedPadStatusV1Fn getPads_=nullptr;
    SdgoPvpAiClientGetGateStatusV1Fn getGates_=nullptr;
    SdgoPvpAiClientForcedPadStatusV1 padStatus_{};
    SdgoPvpAiClientGateStatusV1 gateStatus_{};
    uint32_t expectedBots_=0;
    void** manager_=nullptr;
    using FindObjectFn=uint8_t*(__thiscall*)(void*,uint32_t);
    FindObjectFn find_=nullptr;
    bool Manager(const SdgoGameTestContextV1* context,uint8_t*& manager) {
        if(!manager_)manager_=reinterpret_cast<void**>(context->api->Symbol(sdgo_test::mod,"game.unit_manager"));
        return manager_&&battle_ai::native::detail::ReadPointer(reinterpret_cast<uint8_t*>(manager_),0,manager);
    }
    bool LocalHp(const SdgoGameTestContextV1* context,uint32_t pilot,int32_t& hp,uint32_t& object) {
        using namespace battle_ai::native::detail;
        uint8_t* manager=nullptr;uint8_t* unit=nullptr;uint32_t actualPilot=0;
        return Manager(context,manager)&&
            ReadPointer(manager,0x88,unit)&&Read(unit,0x2B68,actualPilot)&&actualPilot==pilot&&Read(unit,0x38,hp)&&Read(unit,4,object);
    }
    bool RawAlive(const SdgoGameTestContextV1* context,uint32_t id) {
        uint8_t* manager=nullptr;
        if(!find_)find_=reinterpret_cast<FindObjectFn>(context->api->Symbol(sdgo_test::mod,"um.find_object"));
        if(!find_||!Manager(context,manager))return true;
        __try {
            const auto unit=find_(manager,id);int32_t hp=0;
            if(!unit)return false;
            if(!battle_ai::native::detail::Read(unit,0x38,hp))return true;
            return hp>0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return true;}
    }
public:
    bool Observe(const SdgoGameTestContextV1* context,const SdgoPvpAiClientSnapshotV1& battle) {
        if(!context->api->SettingInt(sdgo_test::mod,"BattleAiBehavior",0) &&
           !context->api->SettingInt(sdgo_test::mod,"BattleAiActivity",0)) return true;
        if(!get_) get_=reinterpret_cast<SdgoBattleAiGetUnitSnapshotV1Fn>(GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_UNIT_OBSERVER_EXPORT));
        if(!get_) {context->Assert(0,"the candidate battle AI read-only observer is loaded");return false;}
        if(!getNavigation_)getNavigation_=reinterpret_cast<SdgoBattleAiGetNavigationSnapshotV1Fn>(
            GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),SDGO_BATTLE_AI_NAVIGATION_OBSERVER_EXPORT));
        if(context->api->SettingInt(sdgo_test::mod,"BattleAiActivity",0)&&!getNavigation_) {
            context->Assert(0,"the activity candidate includes the matching passive team and map-navigation observer");return false;
        }
        if(sequence_!=battle.received_battle_start_count) {
            sequence_=battle.received_battle_start_count;units_={};totals_={};captures_=recovered_=retreatShots_=pursuits_=frames_=0;expectedBots_=0;
            padStatus_={};gateStatus_={};
        }
        if(battle.bot_count>expectedBots_) expectedBots_=battle.bot_count;
        ++frames_;
        if(context->api->SettingInt(sdgo_test::mod,"BattleAiActivity",0)) {
            if(!getPads_)getPads_=reinterpret_cast<SdgoPvpAiClientGetForcedPadStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"),SDGO_PVP_AI_CLIENT_FORCED_PAD_STATUS_EXPORT));
            if(!getGates_)getGates_=reinterpret_cast<SdgoPvpAiClientGetGateStatusV1Fn>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"),SDGO_PVP_AI_GATE_STATUS_EXPORT));
            SdgoPvpAiClientForcedPadStatusV1 pads{};pads.size=sizeof(pads);pads.version=1;
            SdgoPvpAiClientGateStatusV1 gates{};gates.size=sizeof(gates);gates.version=1;
            if(!getPads_||!getGates_||!getPads_(&pads)||!getGates_(&gates)||
               pads.battle_start_sequence!=sequence_||gates.battle_start_sequence!=sequence_||!pads.bridge_available) {
                context->Assert(0,"passive native map-device and gate witnesses match the active AI battle");return false;
            }
            if(gates.authority_count>gateStatus_.authority_count) {
                char detail[208];sprintf_s(detail,"native AI gate authority pilot=%u object=%u gate=%u generation=%u count=%u",
                    gates.last_attacker_pilot,gates.last_attacker_object,gates.last_gate_object,gates.last_generation,gates.authority_count);context->Observe(detail);
            }
            padStatus_=pads;gateStatus_=gates;
        }
        const uint64_t clock=GetTickCount64();
        std::array<SdgoBattleAiUnitSnapshotV1,12> current{};
        std::array<bool,12> available{};
        std::array<bool,12> alive{};
        for(uint32_t i=0;i<battle.bot_count&&i<12;++i) {
            current[i].size=sizeof(current[i]);current[i].version=1;
            available[i]=get_(battle.bots[i].object_id,&current[i])!=0;
            if(available[i])alive[i]=current[i].hp>0;
            else alive[i]=RawAlive(context,battle.bots[i].object_id);
        }
        int32_t humanHp=0;uint32_t humanObject=0;const bool humanKnown=LocalHp(context,battle.profile_pilot_id,humanHp,humanObject);
        for(uint32_t i=0;i<battle.bot_count&&i<12;++i) {
            if(!available[i]) {
                auto& state=units_[i];auto& total=totals_[i];
                if(alive[i]) {
                    if(!state.missingSince)state.missingSince=state.lastSnapshotAt?state.lastSnapshotAt:clock;
                    const auto missing=clock-state.missingSince;
                    if(missing>total.maxMissingMs)total.maxMissingMs=missing;
                } else {state.missingSince=0;state.activity.Observe(clock,false,false);}
                continue;
            }
            const auto& now=current[i];
            auto& state=units_[i];const auto before=state.last;
            const auto previousNavigation=state.navigation;
            auto& total=totals_[i];
            const bool sameLife=before.object_id==now.object_id&&before.generation==now.generation;
            if(!sameLife) {
                state={};
                char detail[256];sprintf_s(detail,"native AI life pilot=%u object=%u npc=%u unit_data=%u generation=%llu team=%u hp=%d",
                    battle.bots[i].pilot_id,now.object_id,battle.bots[i].npc_id,now.unit_id,
                    static_cast<unsigned long long>(now.generation),battle.bots[i].team,now.hp);context->Observe(detail);
            }
            state.missingSince=0;state.lastSnapshotAt=clock;
            if(getNavigation_) {
                SdgoBattleAiNavigationSnapshotV1 navigation{};navigation.size=sizeof(navigation);navigation.version=1;
                if(!getNavigation_(now.object_id,&navigation)||navigation.object_id!=now.object_id||navigation.generation!=now.generation) {
                    context->Assert(0,"passive navigation evidence belongs to the same actual bot life");return false;
                }
                const auto delta=[&](uint64_t currentValue,uint64_t previousValue) {
                    return sameLife&&currentValue>=previousValue?currentValue-previousValue:currentValue;
                };
                total.sharedFrames+=delta(navigation.shared_contact_frames,previousNavigation.shared_contact_frames);
                total.sharedPursuitFrames+=delta(navigation.shared_pursuit_frames,previousNavigation.shared_pursuit_frames);
                total.explorationRoutes+=delta(navigation.exploration_routes,previousNavigation.exploration_routes);
                if(navigation.shared_source&&navigation.shared_target&&navigation.shared_pursuit_frames&&
                   (!sameLife||!previousNavigation.shared_pursuit_frames||navigation.shared_target!=previousNavigation.shared_target||
                    navigation.shared_source!=previousNavigation.shared_source)) {
                    char detail[224];sprintf_s(detail,"native AI team support unit=%u ally=%u target=%u own_visible=%u physical_pursuit_frames=%llu",
                        now.object_id,navigation.shared_source,navigation.shared_target,now.target_visible,
                        static_cast<unsigned long long>(navigation.shared_pursuit_frames));context->Observe(detail);
                }
                state.navigation=navigation;
            }
            // Opponent presence is independent of acquisition, visibility and
            // weapon range. An unreadable human cannot waive the idle check.
            bool enemyAlive=battle.bots[i].team!=battle.local_team&&(!humanKnown||humanHp>0);
            for(uint32_t j=0;j<battle.bot_count&&j<12;++j)
                if(battle.bots[j].team!=battle.bots[i].team&&alive[j])enemyAlive=true;
            const float activityDx=now.x-before.x,activityDz=now.z-before.z;
            const bool moving=sameLife&&!now.control_blocked&&activityDx*activityDx+activityDz*activityDz>0.01f;
            const bool attacking=now.attack_state==0x34||now.attack_state==0x5C||now.special_busy;
            if(now.movement_state==0x124||now.movement_pending==0x124) {
                ++total.forcedFrames;if(moving)++total.forcedMovementFrames;
            }
            const auto opponent=[&](uint32_t id) {
                if(!id)return false;
                if(humanKnown&&battle.bots[i].team!=battle.local_team&&id==humanObject)return true;
                for(uint32_t j=0;j<battle.bot_count&&j<12;++j)
                    if(battle.bots[j].team!=battle.bots[i].team&&id==battle.bots[j].object_id)return true;
                return false;
            };
            const bool fightingOpponent=opponent(now.target_id);
            if(now.hp>0&&fightingOpponent) {
                ++total.targetFrames;
                if(moving) ++total.movementFrames;
                if(attacking) ++total.attackFrames;
                if(now.attack_state==0x5C)++total.meleeSwingFrames;
            }
            // special_finished is a native receipt sequence, not a count.
            if((fightingOpponent||opponent(before.target_id))&&now.special_finished&&
               (!sameLife||now.special_finished!=before.special_finished))++total.specialCompletions;
            const bool eligible=now.hp>0&&enemyAlive&&!now.control_blocked;
            const bool fired=sameLife&&now.ranged_shots>before.ranged_shots;
            const auto elapsed=state.activity.Observe(clock,eligible,moving||attacking||fired);
            if(state.activity.maxIdleMs>total.maxIdleMs)total.maxIdleMs=state.activity.maxIdleMs;
            if(state.activity.maxSampleGapMs>total.maxSampleGapMs)total.maxSampleGapMs=state.activity.maxSampleGapMs;
            if(elapsed>=500 && clock-state.idleReportedAt>=500) {
                    char detail[384];sprintf_s(detail,"native AI idle unit=%u elapsed=%llu sample_gap=%llu slot=%d target=%u distance=%.1f movement=%X/%X attack=%X/%X defence=%u moving=%u pos=%.1f,%.1f,%.1f shots=%llu attempts=%llu",
                        now.object_id,static_cast<unsigned long long>(elapsed),static_cast<unsigned long long>(state.activity.maxSampleGapMs),
                        now.selected_slot,now.target_id,now.target_distance,now.movement_state,now.movement_pending,
                        now.attack_state,now.attack_pending,now.defence_moving,now.moving,now.x,now.y,now.z,
                        static_cast<unsigned long long>(now.ranged_shots),static_cast<unsigned long long>(now.ranged_attempts));
                    context->Observe(detail);state.idleReportedAt=clock;
            }
            const uint64_t shots=sameLife&&now.ranged_shots>=before.ranged_shots?now.ranged_shots-before.ranged_shots:now.ranged_shots;
            total.shots+=shots;
            if(fightingOpponent||opponent(before.target_id))total.opponentShots+=shots;
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
        if(context->api->SettingInt(sdgo_test::mod,"BattleAiActivity",0)) {
            uint32_t observed=0,active=0;
            for(size_t i=0;i<units_.size();++i) {
                if(!units_[i].last.object_id) continue;
                ++observed;
                const auto& total=totals_[i];
                if(total.targetFrames&&(total.meleeSwingFrames||total.opponentShots||total.specialCompletions)&&total.maxIdleMs<=1000&&total.maxSampleGapMs<=1000&&total.maxMissingMs<=1000) ++active;
                char detail[608];sprintf_s(detail,"native AI activity unit=%u unit_data=%u target_frames=%u movement_frames=%u attack_frames=%u melee_swings=%u special_finish_events=%llu shots=%llu opponent_shots=%llu attempts=%llu backward_dispatches=%llu max_free_idle_ms=%llu max_sample_gap_ms=%llu max_missing_ms=%llu shared_frames=%llu shared_pursuit_frames=%llu exploration_routes=%llu",
                    units_[i].last.object_id,units_[i].last.unit_id,total.targetFrames,total.movementFrames,total.attackFrames,
                    total.meleeSwingFrames,static_cast<unsigned long long>(total.specialCompletions),
                    static_cast<unsigned long long>(total.shots),static_cast<unsigned long long>(total.opponentShots),static_cast<unsigned long long>(total.attempts),static_cast<unsigned long long>(total.backwardDispatches),
                    static_cast<unsigned long long>(total.maxIdleMs),static_cast<unsigned long long>(total.maxSampleGapMs),static_cast<unsigned long long>(total.maxMissingMs),
                    static_cast<unsigned long long>(total.sharedFrames),static_cast<unsigned long long>(total.sharedPursuitFrames),
                    static_cast<unsigned long long>(total.explorationRoutes));context->Observe(detail);
                char map[208];sprintf_s(map,"native AI map activity unit=%u forced_frames=%u physical_forced_frames=%u gate_authorities=%u",
                    units_[i].last.object_id,total.forcedFrames,total.forcedMovementFrames,gateStatus_.authority_count);context->Observe(map);
                for(uint32_t j=0;j<padStatus_.bot_count&&j<12;++j) {
                    const auto& pad=padStatus_.bots[j];if(pad.object_id!=units_[i].last.object_id)continue;
                    char device[272];sprintf_s(device,"native AI pad unit=%u requests=%u notices=%u displacements=%u timeouts=%u send_failures=%u outstanding=%u",
                        pad.object_id,pad.request_count,pad.notice_count,pad.displacement_count,pad.timeout_count,pad.send_failure_count,pad.outstanding);context->Observe(device);
                }
            }
            context->Assert(expectedBots_>0&&observed==expectedBots_&&active==expectedBots_,
                "every AI acquired an opponent and attacked, with no more than one second of controllable inactivity while enemies were alive and no over-one-second observation gap");
        }
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
