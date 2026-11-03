#pragma once
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "friend_actions.h"
#include "room_actions.h"
#include "room_join_actions.h"
#include "battle_combat_actions.h"
#include "sdgo/game_test.h"
#include "sdgo/pvp_ai_client_observer.h"

// Coordinated developer actions use the same native room/business adapters as
// ordinary GameTest. No participant, Ready flag, result or gameplay state is set.
template<class NativeT=NativeActions,class RoomT=RoomActions,class JoinT=RoomJoinActions>
class PvpSessionActionsT {
public:
    using Result=friends::FriendActions::Result;
    using Command=friends::FriendActions::Command;
    using SnapshotFn=bool(*)(SdgoPvpAiClientSnapshotV1&);
    using StartFn=int(*)(const SdgoGameTestContextV1*);
    using ControlFn=bool(*)(uint32_t);
    using ChannelFn=int(*)(uint32_t,uint16_t);
    enum Operation:uint32_t {Join=34,Ready=35,Start=36,FinishBattle=37};
    struct Observation {
        uint32_t result=0,operation=0,sequence=0,pilot=0,expected_host=0,host=0,room=0,humans=0;
        uint32_t start_sequence=0,end_sequence=0,frames=0,movement_events=0,bots=0,local_bots=0,remote_bots=0;
        uint32_t join_result=0,ready_result=0,hp_events=0,lua_errors=0,join_stage=0;
        uint16_t end_opcode=0;
        uint8_t local_host=0,battle_seen=0,settled=0,join_original=0,ready_reply=0,ready_original=0,ready_model=0;
    };
    bool Bind(NativeT* native,RoomT* room,JoinT* join,SnapshotFn snapshot,StartFn start,
              const char* title,uint32_t mode,uint32_t bots,ControlFn control,ChannelFn channel) {
        native_=native;room_=room;join_=join;snapshot_=snapshot;start_=start;mode_=mode;expectedBots_=bots;
        control_=control;
        channel_=channel;
        if(!native||!room||!join||!snapshot||!start||!control||!channel||bots<1||bots>7||
           (mode!=1&&mode!=3&&mode!=6&&mode!=7&&mode!=9)||!title||!Title(title))return false;
        return true;
    }
    Result Submit(const Command& command,uint32_t humans,const SdgoGameTestContextV1* context,uint32_t group=UINT32_MAX,uint16_t channel=0) {
        if(active_||!context||humans<2||humans>8||!command.target_id||(command.target_id&0x80000000u))return Fail();
        op_=command.operation;state_.operation=op_;state_.sequence=command.sequence;
        state_.expected_host=command.target_id;expectedHumans_=humans;state_.result=friends::FriendActions::kWaitingForLobby;
        ticks_=0;at_=GetTickCount64();active_=true;
        const auto current=room_->NativeRoomState();
        state_.pilot=native_->OnboardingState().profile_pilot_id;
        if(op_==Join) {
            if(state_.pilot==command.target_id||group>3||!channel||!Title(command.body))return Fail();
            targetGroup_=group;targetChannel_=channel;
            joinStarted_=false;
        } else if(op_==Ready) {
            if(state_.pilot==command.target_id||!joined_)return Fail();
            readyStarted_=false;
        } else if(op_==Start) {
            if(state_.battle_seen||state_.pilot!=command.target_id||!current.local_is_host||current.player_count!=humans)return Fail();
            expectedRoom_=current.room_id;
        } else if(op_==FinishBattle) {
            if(!expectedRoom_||!state_.battle_seen)return Fail();
        } else return Fail();
        return static_cast<Result>(state_.result);
    }
    void Tick(const SdgoGameTestContextV1* context) {
        if(!native_||!room_||!join_)return;
        Track();
        if(!active_)return;
        if(op_==Join) {
            const int channel=channel_(targetGroup_,targetChannel_);
            if(channel==SDGO_GAME_TEST_FAIL){Fail();return;}
            if(channel!=SDGO_GAME_TEST_PASS)return;
            if(!joinStarted_)joinStarted_=join_->JoinExactTitle(title_);
            state_.join_stage=join_->SelectionStage();
            const auto joined=join_->JoinState();state_.join_result=joined.result;state_.join_original=joined.original_handler_completed;
            if(joined.result==JoinT::kJoinRejected){Fail();return;}
            if(joined.result!=JoinT::kJoinAccepted)return;
            const auto current=room_->NativeRoomState();
            if(!joined.local_is_guest||!joined.original_handler_completed||current.local_is_host||
               current.room_id!=joined.model_room_id||current.host_pilot_id!=state_.expected_host){Fail();return;}
            expectedRoom_=current.room_id;state_.room=current.room_id;state_.host=current.host_pilot_id;
            state_.humans=current.player_count;state_.local_host=current.local_is_host;joined_=true;Complete();
        } else if(op_==Ready) {
            if(!readyStarted_)readyStarted_=join_->Ready();
            const auto ready=join_->ReadyState();state_.ready_result=ready.result;
            state_.ready_reply=ready.server_reply_observed;state_.ready_original=ready.original_dispatch_completed;state_.ready_model=ready.post_dispatch_model_ready;
            if(ready.result==JoinT::kReadyRejected){Fail();return;}
            if(ready.result!=JoinT::kReadyAccepted)return;
            if(ready.room_id!=expectedRoom_||!ready.server_reply_observed||!ready.original_dispatch_completed||!ready.post_dispatch_model_ready){Fail();return;}
            Complete();
        } else if(op_==Start) {
            auto step=*context;step.tick=ticks_++;step.elapsed_ms=GetTickCount64()-at_;
            const int result=start_(&step);
            if(result==SDGO_GAME_TEST_FAIL){Fail();return;}
            if(result==SDGO_GAME_TEST_PASS)Complete();
        } else if(op_==FinishBattle) {
            const auto ended=room_->BattleEndState();const auto current=room_->NativeRoomState();
            if(!ended.sequence||ended.reply_opcode!=0x0871||!ended.original_handler_completed||!RoomT::SettledRoomReady(current))return;
            if(current.room_id!=expectedRoom_||current.room_mode!=mode_||current.player_count!=expectedHumans_||!current.host_pilot_id||
               (current.host_pilot_id&0x80000000u)||!state_.movement_events||!state_.frames||state_.lua_errors||
               (state_.pilot==state_.expected_host&&!combat_.State().complete_evidence)){Fail();return;}
            state_.room=current.room_id;state_.host=current.host_pilot_id;state_.humans=current.player_count;
            state_.local_host=current.local_is_host;state_.end_sequence=ended.sequence;state_.end_opcode=ended.reply_opcode;state_.settled=1;
            context->Assert(1,"the coordinated client observed the real battle and original settlement in the same human-owned room");
            Complete();
        }
    }
    Observation State()const{return state_;}
    void Append(FILE* file)const {
        const auto s=State();
        fprintf(file,"[pvp]\noperation=%u\nsequence=%u\nresult=%u\npilot=%u\nexpected_host=%u\nhost=%u\nroom=%u\nhumans=%u\nlocal_host=%u\nstart_sequence=%u\nend_sequence=%u\nend_opcode=%u\nbattle_seen=%u\nsettled=%u\nframes=%u\nmovement_events=%u\nbots=%u\nlocal_bots=%u\nremote_bots=%u\njoin_result=%u\njoin_original=%u\nready_result=%u\nready_reply=%u\nready_original=%u\nready_model=%u\n",
            s.operation,s.sequence,s.result,s.pilot,s.expected_host,s.host,s.room,s.humans,s.local_host,s.start_sequence,s.end_sequence,s.end_opcode,s.battle_seen,s.settled,s.frames,s.movement_events,s.bots,s.local_bots,s.remote_bots,s.join_result,s.join_original,s.ready_result,s.ready_reply,s.ready_original,s.ready_model);
        fprintf(file,"hp_events=%u\nlua_errors=%u\njoin_stage=%u\n",s.hp_events,s.lua_errors,s.join_stage);
        const auto selected=native_->OnboardingState();
        fprintf(file,"channel_id=%u\nchannel_group=%u\n",selected.channel_id,selected.channel_group);
    }
private:
    bool Title(const char* title) {
        if(!title||!*title||strlen(title)>=sizeof(title_))return false;
        wchar_t wide[32]{};
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,title,-1,wide,32))return false;
        strcpy_s(title_,title);wcscpy_s(wideTitle_,wide);return true;
    }
    Result Fail(){active_=false;state_.result=friends::FriendActions::kNativeFault;return friends::FriendActions::kNativeFault;}
    void Complete(){active_=false;state_.result=friends::FriendActions::kReady;}
    void Track() {
        SdgoPvpAiClientSnapshotV1 s{};
        if(!snapshot_||!snapshot_(s)||s.size<sizeof(s)||s.version!=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION||
           s.native_app_state!=22||!s.received_battle_start_count||!control_(s.received_battle_start_count))return;
        if(s.profile_pilot_id!=state_.pilot||s.room_mode!=mode_||wcscmp(s.room_title,wideTitle_)||
           s.human_participant_count!=expectedHumans_||s.bot_count!=expectedBots_)return;
        uint32_t local=0,remote=0;
        for(uint32_t i=0;i<s.bot_count;++i) {
            const auto& bot=s.bots[i];
            if(!bot.valid_registered_object||!bot.position_valid)return;
            local+=bot.local_control?1:0;remote+=bot.remote_copy&&!bot.local_control?1:0;
        }
        if((state_.pilot==state_.expected_host&&local!=expectedBots_)||
           (state_.pilot!=state_.expected_host&&remote!=expectedBots_))return;
        for(uint32_t i=0;i<s.bot_count;++i) {
            const auto& bot=s.bots[i];auto& before=samples_[i];
            if(before.id==bot.object_id && before.pilot==bot.pilot_id &&
               (fabsf(before.x-bot.position_x)>.1f||fabsf(before.z-bot.position_z)>.1f))++state_.movement_events;
            before={bot.pilot_id,bot.object_id,bot.position_x,bot.position_z};
        }
        ++state_.frames;state_.battle_seen=1;state_.start_sequence=s.received_battle_start_count;
        state_.bots=s.bot_count;state_.local_bots=local;state_.remote_bots=remote;
        state_.lua_errors=s.lua_error_count;
        if(state_.pilot==state_.expected_host) {
            const BattleCombatActions::ExpectedBattle expected{s.received_battle_start_count,s.profile_pilot_id,static_cast<uint16_t>(mode_),wideTitle_};
            combat_.Observe(s,expected);state_.hp_events=combat_.State().hp_decrease_events;
        }
    }
    struct Sample {uint32_t pilot=0,id=0;float x=0,z=0;};
    NativeT* native_=nullptr;RoomT* room_=nullptr;JoinT* join_=nullptr;SnapshotFn snapshot_=nullptr;StartFn start_=nullptr;ControlFn control_=nullptr;
    ChannelFn channel_=nullptr;uint32_t targetGroup_=UINT32_MAX;uint16_t targetChannel_=0;
    Observation state_{};uint32_t expectedBots_=0,expectedHumans_=0,mode_=0,expectedRoom_=0,op_=0;
    std::array<Sample,12> samples_{};
    BattleCombatActions combat_;
    char title_[128]{};wchar_t wideTitle_[32]{};
    bool active_=false,joined_=false,joinStarted_=false,readyStarted_=false;uint64_t at_=0,ticks_=0;
};
using PvpSessionActions=PvpSessionActionsT<>;
