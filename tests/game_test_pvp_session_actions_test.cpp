// Offline adapter/state guards only. No game, account or server is started.
#include "../mods/game_test_client/pvp_session_actions.h"
#include <cstdio>
using F=friends::FriendActions;
namespace {
int failures=0,starts=0;bool controlled=true,channelReady=true;
void Check(bool ok,const char* name){if(!ok){++failures;std::printf("FAIL %s\n",name);}}
struct Native {
    NativeActions::OnboardingObservation profile{};
    auto OnboardingState()const{return profile;}
};
struct Room {
    RoomActions::NativeRoomObservation room{};RoomActions::BattleEndObservation end{};
    auto NativeRoomState()const{return room;}
    auto BattleEndState()const{return end;}
    static bool SettledRoomReady(const RoomActions::NativeRoomObservation& r){return RoomActions::SettledRoomReady(r);}
};
struct Joiner {
    enum {kJoinRejected=2,kJoinAccepted=3,kReadyRejected=2,kReadyAccepted=3};
    RoomJoinActions::JoinObservation join{};RoomJoinActions::ReadyObservation ready{};
    unsigned joins=0,readies=0;bool canReady=true;
    bool JoinExactTitle(const char*){++joins;return true;}
    bool Ready(){++readies;return canReady;}
    uint32_t SelectionStage()const{return 8;}
    uint32_t SelectionControlFlags()const{return 31;}
    uint32_t ReadyStage()const{return 10;}
    uint32_t ReadyContextFlags()const{return 15;}
    auto JoinState()const{return join;}
    auto ReadyState()const{return ready;}
};
using Driver=PvpSessionActionsT<Native,Room,Joiner>;
SdgoPvpAiClientSnapshotV1 snapshot{};
bool Snap(SdgoPvpAiClientSnapshotV1& s){s=snapshot;return true;}
bool Control(uint32_t seq){return controlled&&(seq==7||seq==8);}
int Start(const SdgoGameTestContextV1*){++starts;return SDGO_GAME_TEST_PASS;}
int Channel(uint32_t group,uint16_t channel){return !channelReady?SDGO_GAME_TEST_WAIT:group==1&&channel==9304?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;}
void Assert(int,const char*){}
SdgoGameTestContextV1 context{};
struct World {
    Native native;Room room;Joiner join;Driver driver;
    World(){
        snapshot={};controlled=true;channelReady=true;
        native.profile.profile_pilot_id=32;
        auto& r=room.room;r.native_app_state=18;r.model_present=1;r.room_id=81;r.room_mode=3;r.player_count=3;r.host_pilot_id=31;
        r.occupied_slot_count=3;r.slot_pilot_ids[0]=31;r.slot_pilot_ids[1]=32;r.slot_pilot_ids[2]=33;
        Check(driver.Bind(&native,&room,&join,&Snap,&Start,"gt-peer !ai5",3,5,&Control,&Channel),"bound native adapters");
    }
    void Joined(){
        F::Command c{};c.sequence=1;c.operation=Driver::Join;c.target_id=31;strcpy_s(c.body,"gt-peer !ai5");
        driver.Submit(c,3,&context,1,9304);join.join.result=Joiner::kJoinAccepted;join.join.model_room_id=81;join.join.local_is_guest=1;join.join.original_handler_completed=1;driver.Tick(&context);
    }
    void Ready(uint32_t sequence=2){
        F::Command c{};c.operation=Driver::Ready;c.sequence=sequence;c.target_id=31;
        driver.Submit(c,3,&context);join.ready.result=Joiner::kReadyAccepted;join.ready.room_id=81;
        join.ready.server_reply_observed=join.ready.original_dispatch_completed=join.ready.post_dispatch_model_ready=1;
        driver.Tick(&context);
    }
};
void RemoteSnapshot(uint32_t sequence=7,bool local=false,uint32_t pilot=32){
    snapshot={};snapshot.size=sizeof(snapshot);snapshot.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;snapshot.native_app_state=22;
    snapshot.profile_pilot_id=pilot;snapshot.room_mode=3;snapshot.received_battle_start_count=sequence;snapshot.human_participant_count=3;snapshot.bot_count=5;
    wcscpy_s(snapshot.room_title,L"gt-peer !ai5");snapshot.room_title_length=11;
    for(unsigned i=0;i<5;++i){auto& b=snapshot.bots[i];b.pilot_id=0x80000000u+i+1;b.object_id=100+i;b.valid_registered_object=b.position_valid=1;b.local_control=local;b.remote_copy=!local;b.position_x=static_cast<float>(i);}
}
}
int main(){
    context.Assert=&Assert;
    {
        World w;channelReady=false;w.Joined();
        Check(w.join.joins==0,"guest does not search or join another channel before the original selector completes");
        channelReady=true;w.driver.Tick(&context);
        Check(w.join.joins==1&&w.driver.State().result==F::kReady,"the exact-room join follows acknowledged native channel preparation");
    }
    {
        World w;
        F::Command c{};c.sequence=1;c.operation=Driver::Join;c.target_id=31;strcpy_s(c.body,"gt-peer !ai5");
        w.driver.Submit(c,3,&context,1,9304);w.driver.Tick(&context);
        Check(w.join.joins==1&&w.driver.State().result!=F::kReady,"sent join waits for the actual delayed native acknowledgement");
        // The original reply replaces the room-list page with the 3D room.
        // Requiring the list again would strand an otherwise accepted join.
        channelReady=false;w.join.join.result=Joiner::kJoinAccepted;w.join.join.model_room_id=81;
        w.join.join.local_is_guest=1;w.join.join.original_handler_completed=1;w.driver.Tick(&context);
        Check(w.driver.State().result==F::kReady&&w.join.joins==1,"delayed original join completes after leaving the native list page without reopening it");
        channelReady=true;
    }
    {
        World w;w.Joined();Check(w.driver.State().result==F::kReady&&w.join.joins==1,"exact live room plus original join reply completes");
        F::Command c{};c.operation=Driver::Ready;c.sequence=2;c.target_id=31;w.driver.Submit(c,3,&context);
        w.join.ready.result=Joiner::kReadyAccepted;w.join.ready.room_id=81;w.driver.Tick(&context);
        Check(w.driver.State().result==F::kNativeFault,"Ready enum alone cannot replace native acknowledgement and model readback");
    }
    {
        World w;w.room.room.host_pilot_id=0x80000001u;w.Joined();Check(w.driver.State().result==F::kNativeFault,"AI host or foreign owner is not accepted as the requested human host");
    }
    {
        World w;F::Command c{};c.operation=Driver::Start;c.target_id=31;c.sequence=1;
        Check(w.driver.Submit(c,3,&context)==F::kNativeFault&&starts==0,"guest cannot invoke host start business");
    }
    {
        World w;w.Joined();RemoteSnapshot();controlled=false;w.driver.Tick(&context);snapshot.bots[0].position_x+=10;w.driver.Tick(&context);
        Check(!w.driver.State().frames&&!w.driver.State().battle_seen,"rendered pre-control frames are not battle movement evidence");controlled=true;
        snapshot.bots[0].local_control=1;w.driver.Tick(&context);Check(!w.driver.State().frames,"guest must observe synchronized copies rather than locally running AI");
    }
    {
        World w;w.room.room.player_count=2;w.Joined();
        Check(w.driver.State().humans==2,"early guest records the real admission-time population");
        RemoteSnapshot();w.driver.Tick(&context);
        Check(w.driver.State().humans==3&&w.driver.State().battle_seen,
              "battle population refreshes from the acknowledged full native roster after later guests join");
    }
    {
        World w;w.Joined();RemoteSnapshot();w.driver.Tick(&context);snapshot.bots[0].position_x+=2;w.driver.Tick(&context);
        Check(w.driver.State().remote_bots==5&&w.driver.State().movement_events==1,"actual remote identities and post-control movement are accumulated");
        F::Command c{};c.sequence=2;c.operation=Driver::FinishBattle;c.target_id=31;w.driver.Submit(c,3,&context);
        w.room.end.sequence=4;w.room.end.reply_opcode=0x0871;w.room.end.original_handler_completed=0;w.driver.Tick(&context);
        Check(!w.driver.State().settled,"end packet alone cannot skip original native handler");
        const auto unchanged=w.room.room;w.room.end.original_handler_completed=1;w.driver.Tick(&context);
        Check(w.driver.State().result==F::kReady&&w.driver.State().settled&&w.driver.State().end_sequence==4,"native end and original human-owned room complete the peer battle");
        Check(!memcmp(&unchanged,&w.room.room,sizeof(unchanged)),"observer does not mutate native room metadata");
    }
    {
        World w;w.Joined();w.Ready();
        RemoteSnapshot();w.driver.Tick(&context);snapshot.bots[0].position_x+=2;w.driver.Tick(&context);
        F::Command finish{};finish.sequence=3;finish.operation=Driver::FinishBattle;finish.target_id=31;
        w.driver.Submit(finish,3,&context);w.room.end.sequence=4;w.room.end.reply_opcode=0x0871;
        w.room.end.original_handler_completed=1;w.driver.Tick(&context);
        Check(w.driver.State().settled&&w.driver.State().round==1,"the first acknowledged round settles before reuse");

        w.join.canReady=false;
        w.Ready(4);
        Check(w.driver.State().result!=F::kReady&&!w.driver.State().ready_reply,
              "a blocked new Ready cannot consume the previous round's accepted acknowledgement");
        w.driver.Tick(&context);
        Check(w.driver.State().result!=F::kReady,
              "the guest keeps waiting while native Main load gates prevent the new request");
        w.join.canReady=true;w.join.ready={};w.driver.Tick(&context);
        Check(w.driver.State().result!=F::kReady,
              "queuing this round's Ready still requires this round's original acknowledgement");
        w.join.ready.result=Joiner::kReadyAccepted;w.join.ready.room_id=81;
        w.join.ready.server_reply_observed=w.join.ready.original_dispatch_completed=w.join.ready.post_dispatch_model_ready=1;
        w.driver.Tick(&context);
        Check(w.driver.State().round==2&&!w.driver.State().battle_seen&&!w.driver.State().settled&&
              !w.driver.State().frames&&!w.driver.State().movement_events,
              "second-round Ready clears only prior observer evidence");
        w.driver.Tick(&context);
        Check(!w.driver.State().battle_seen,"the prior native start count cannot satisfy the second round");
        RemoteSnapshot(8);w.driver.Tick(&context);
        Check(w.driver.State().battle_seen&&w.driver.State().start_sequence==8&&w.driver.State().frames==1&&
              !w.driver.State().movement_events,"the guest recognizes a fresh native battle generation with fresh samples");
        snapshot.bots[0].position_x+=2;w.driver.Tick(&context);
        finish.sequence=5;w.driver.Submit(finish,3,&context);w.driver.Tick(&context);
        Check(!w.driver.State().settled,"the prior native settlement sequence cannot satisfy the second round");
        w.room.end.sequence=5;w.driver.Tick(&context);
        Check(w.driver.State().settled&&w.driver.State().end_sequence==5&&w.driver.State().movement_events==1,
              "only the second native settlement and second-round movement complete the rematch");
    }
    {
        World w;w.Joined();w.Ready();RemoteSnapshot();w.driver.Tick(&context);
        snapshot.bots[0].position_x+=2;w.driver.Tick(&context);
        F::Command finish{};finish.sequence=3;finish.operation=Driver::FinishBattle;finish.target_id=31;
        w.driver.Submit(finish,3,&context);w.room.end.sequence=4;w.room.end.reply_opcode=0x0871;
        w.room.end.original_handler_completed=1;w.room.room.occupied_slot_count=4;w.room.room.ai_pilot_count=1;
        w.room.room.slot_pilot_ids[3]=0x80000001u;w.driver.Tick(&context);
        Check(!w.driver.State().settled&&w.driver.State().result!=F::kReady,
              "human count alone cannot settle while an AI room slot remains");
        w.room.room.occupied_slot_count=3;w.room.room.ai_pilot_count=0;w.room.room.slot_pilot_ids[3]=0;
        w.driver.Tick(&context);
        Check(w.driver.State().settled,"settlement completes after all twelve returned slots contain only humans");
    }
    {
        World w;w.Joined();w.room.room.occupied_slot_count=4;w.room.room.ai_pilot_count=1;
        w.room.room.slot_pilot_ids[3]=0x80000001u;
        F::Command ready{};ready.operation=Driver::Ready;ready.sequence=2;ready.target_id=31;
        Check(w.driver.Submit(ready,3,&context)==F::kNativeFault,
              "a guest cannot Ready a rematch over a ghost AI slot");
    }
    {
        World w;w.native.profile.profile_pilot_id=31;w.room.room.local_is_host=1;
        F::Command start{};start.operation=Driver::Start;start.sequence=1;start.target_id=31;
        const int before=starts;
        Check(w.driver.Submit(start,3,&context)==F::kWaitingForLobby,"the clean human host may request native Start");
        w.driver.Tick(&context);
        Check(starts==before+1&&w.driver.State().result!=F::kReady,
              "a callback PASS alone cannot reuse an old battle as a new Start receipt");
        RemoteSnapshot(7,true,31);w.driver.Tick(&context);
        Check(w.driver.State().result==F::kReady&&w.driver.State().battle_seen&&w.driver.State().start_sequence==7,
              "host Start completes only after a fresh controlled native battle generation");
    }
    std::printf("Coordinated native PvP actions: %d failures\n",failures);return failures?1:0;
}
