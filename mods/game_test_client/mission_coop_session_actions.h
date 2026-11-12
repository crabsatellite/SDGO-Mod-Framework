#pragma once

#include <cstdio>
#include <cstring>
#include "sdgo/game_test.h"

#include "friend_actions.h"
#include "room_join_actions.h"
#include "mission_flow_actions.h"
#include "room_actions.h"

// Command adapter for one real two-human mission room. Creation and Start stay
// in MissionFlowActions; invitation acceptance stays in FriendRoomActions; the
// guest Ready action stays in GuestReadyActions. This class only sequences and
// correlates those original client businesses and their consumed replies.
template<class MissionT=MissionFlowActions,class RoomT=RoomActions,class ReadyT=RoomJoinActions>
class MissionCoopSessionActionsT {
public:
    using Result=friends::FriendActions::Result;
    using Command=friends::FriendActions::Command;
    enum Operation:uint32_t {CreateRoom=43,ReadyGuest=44,StartMission=45,EnterLobby=46};
    struct Observation {
        uint32_t result=0,operation=0,sequence=0,mission_result=0;
        uint32_t expected_host=0,host=0,room=0,humans=0,occupied=0,ai=0;
        uint32_t ready_result=0,ready_sequence=0;
        uint16_t mode=0;
        uint8_t local_host=0,guest_ready=0,roster_verified=0,reserved=0;
    };

    bool Bind(MissionT* mission,RoomT* rooms,ReadyT* ready,uint16_t mode,uint32_t missionId) {
        mission_=mission;rooms_=rooms;ready_=ready;mode_=mode;missionId_=missionId;
        return mission_&&rooms_&&ready_&&(mode_==5||mode_==8)&&missionId_&&
            (missionId_<=0x313990u?5u:8u)==mode_;
    }
    Result Submit(const Command& command,uint32_t humans,uint32_t roomId) {
        if(active_||!mission_||!rooms_||!ready_||!command.target_id||
           (command.target_id&0x80000000u))return Fail();
        state_={};state_.operation=command.operation;state_.sequence=command.sequence;
        state_.expected_host=command.target_id;state_.humans=humans;state_.mode=mode_;
        expectedRoom_=roomId;readyStarted_=false;rosterAccepted_=false;
        if(command.operation==CreateRoom||command.operation==EnterLobby) {
            if(humans!=1||roomId||!ValidTitle(command.body))return Fail();
            strcpy_s(title_,command.body);mission_->Reset();
        } else if(command.operation==ReadyGuest) {
            if(humans!=2||!roomId)return Fail();
            ready_->ResetReadyObservation();
        } else if(command.operation==StartMission) {
            if(humans!=2||!roomId)return Fail();
        } else return Fail();
        active_=true;state_.result=friends::FriendActions::kWaitingForLobby;
        return static_cast<Result>(state_.result);
    }
    void Tick(const SdgoGameTestContextV1*) {
        if(!active_)return;
        auto room=rooms_->NativeRoomState();ObserveRoom(room);
        if(state_.operation==EnterLobby) {
            const auto result=mission_->EnsureRoom(mode_,title_,false,missionId_,true);
            state_.mission_result=result;
            if(result>=MissionT::kInvalidMode||result==MissionT::kUnbound){Fail();return;}
            if(result==MissionT::kMissionChannelAccepted)Complete();
            return;
        }
        if(state_.operation==CreateRoom) {
            const auto result=mission_->EnsureRoom(mode_,title_,true,missionId_);
            state_.mission_result=result;
            if(result>=MissionT::kInvalidMode||result==MissionT::kUnbound){Fail();return;}
            if(result!=MissionT::kRoomReady)return;
            room=rooms_->NativeRoomState();ObserveRoom(room);
            if(!RoomT::SettledRoomReady(room))return;
            if(!CleanRoster(room,1)||!room.local_is_host){Fail();return;}
            expectedRoom_=room.room_id;state_.roster_verified=1;Complete();return;
        }
        if(state_.operation==ReadyGuest) {
            if(!CleanRoster(room,2)||room.local_is_host)return;
            if(!readyStarted_) {
                readyStarted_=ready_->ReadyExactRoom(expectedRoom_);
                if(!readyStarted_)return;
            }
            const auto observed=ready_->ReadyState();
            state_.ready_result=observed.result;state_.ready_sequence=observed.sequence;
            state_.guest_ready=observed.post_dispatch_model_ready;
            if(observed.result==ReadyT::kReadyRejected){Fail();return;}
            // Mission member machines can consume a different native Ready
            // transport than the normal-room tap. Accept its real replicated
            // room-model postcondition after the original Ready business was
            // queued, without inventing a normal-room send or ACK receipt.
            if(observed.result==ReadyT::kReadyPending&&AnyGuestReady(room)) {
                state_.guest_ready=1;state_.ready_result=4;
                state_.roster_verified=1;Complete();return;
            }
            if(observed.result!=ReadyT::kReadyAccepted)return;
            if(observed.room_id!=expectedRoom_||!observed.server_reply_observed||
               !observed.original_dispatch_completed||!observed.post_dispatch_model_ready||
               observed.local_slot>=12||!room.slot_ready[observed.local_slot]){Fail();return;}
            state_.roster_verified=1;Complete();return;
        }
        if(state_.operation==StartMission) {
            if(!rosterAccepted_) {
                if(!CleanRoster(room,2)||!room.local_is_host||!AnyGuestReady(room))return;
                rosterAccepted_=true;state_.roster_verified=1;
            }
            const auto result=mission_->Start();state_.mission_result=result;
            if(result>=MissionT::kInvalidMode||result==MissionT::kUnbound){Fail();return;}
            if(result==MissionT::kBattleStartConsumed)Complete();
        }
    }
    Observation State()const{return state_;}
    bool Active()const{return active_;}
    void Append(FILE* file)const {
        if(!file)return;const auto s=State();
        std::fprintf(file,"[mission_ready_diagnostics]\nstage=%u\ncontext_flags=%u\n",
            ready_?ready_->ReadyStage():0,ready_?ready_->ReadyContextFlags():0);
        std::fprintf(file,"[mission_coop]\noperation=%u\nsequence=%u\nresult=%u\nmission_result=%u\nexpected_host=%u\nhost=%u\nroom=%u\nhumans=%u\noccupied=%u\nai=%u\nmode=%u\nlocal_host=%u\nguest_ready=%u\nroster_verified=%u\nready_result=%u\nready_sequence=%u\n",
            s.operation,s.sequence,s.result,s.mission_result,s.expected_host,s.host,s.room,s.humans,
            s.occupied,s.ai,s.mode,s.local_host,s.guest_ready,s.roster_verified,s.ready_result,s.ready_sequence);
    }
private:
    static bool ValidTitle(const char* title) {
        if(!title||!*title||std::strlen(title)>SDGO_ROOM_TITLE_MAX_UTF8_BYTES)return false;
        wchar_t wide[SDGO_ROOM_TITLE_MAX_UTF8_BYTES+1]{};
        return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,title,-1,wide,
            static_cast<int>(sizeof(wide)/sizeof(wide[0])))!=0;
    }
    template<class RoomObservation>void ObserveRoom(const RoomObservation& room) {
        if(!RoomT::SettledRoomReady(room))return;
        state_.room=room.room_id;state_.host=room.host_pilot_id;state_.occupied=room.occupied_slot_count;
        state_.ai=room.ai_pilot_count;state_.local_host=room.local_is_host;
    }
    template<class RoomObservation>bool CleanRoster(const RoomObservation& room,uint32_t humans)const {
        if(!RoomT::SettledRoomReady(room)||room.room_mode!=mode_||room.player_count!=humans||
           room.host_pilot_id!=state_.expected_host||(room.host_pilot_id&0x80000000u)||
           room.occupied_slot_count!=humans||room.ai_pilot_count||
           (expectedRoom_&&room.room_id!=expectedRoom_))return false;
        uint32_t occupied=0;bool hostPresent=false;
        for(size_t i=0;i<12;++i) {
            const uint32_t pilot=room.slot_pilot_ids[i];if(!pilot)continue;
            if(pilot&0x80000000u)return false;
            for(size_t j=0;j<i;++j)if(room.slot_pilot_ids[j]==pilot)return false;
            hostPresent=hostPresent||pilot==room.host_pilot_id;++occupied;
        }
        return occupied==humans&&hostPresent;
    }
    template<class RoomObservation>static bool AnyGuestReady(const RoomObservation& room) {
        for(size_t i=0;i<12;++i)if(room.slot_pilot_ids[i]&&room.slot_pilot_ids[i]!=room.host_pilot_id&&room.slot_ready[i])return true;
        return false;
    }
    Result Fail(){active_=false;state_.result=friends::FriendActions::kNativeFault;return friends::FriendActions::kNativeFault;}
    void Complete(){active_=false;state_.result=friends::FriendActions::kReady;}
    MissionT* mission_=nullptr;RoomT* rooms_=nullptr;ReadyT* ready_=nullptr;
    Observation state_{};uint16_t mode_=0;uint32_t missionId_=0,expectedRoom_=0;
    char title_[SDGO_ROOM_TITLE_MAX_UTF8_BYTES+1]{};
    bool active_=false,readyStarted_=false,rosterAccepted_=false;
};
using MissionCoopSessionActions=MissionCoopSessionActionsT<>;
