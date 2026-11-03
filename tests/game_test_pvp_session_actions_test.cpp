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
    unsigned joins=0,readies=0;
    bool JoinExactTitle(const char*){++joins;return true;}
    bool Ready(){++readies;return true;}
    uint32_t SelectionStage()const{return 8;}
    uint32_t SelectionControlFlags()const{return 31;}
    auto JoinState()const{return join;}
    auto ReadyState()const{return ready;}
};
using Driver=PvpSessionActionsT<Native,Room,Joiner>;
SdgoPvpAiClientSnapshotV1 snapshot{};
bool Snap(SdgoPvpAiClientSnapshotV1& s){s=snapshot;return true;}
bool Control(uint32_t seq){return controlled&&seq==7;}
int Start(const SdgoGameTestContextV1*){++starts;return SDGO_GAME_TEST_PASS;}
int Channel(uint32_t group,uint16_t channel){return !channelReady?SDGO_GAME_TEST_WAIT:group==1&&channel==9304?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;}
void Assert(int,const char*){}
SdgoGameTestContextV1 context{};
struct World {
    Native native;Room room;Joiner join;Driver driver;
    World(){
        native.profile.profile_pilot_id=32;
        auto& r=room.room;r.native_app_state=18;r.model_present=1;r.room_id=81;r.room_mode=3;r.player_count=3;r.host_pilot_id=31;
        Check(driver.Bind(&native,&room,&join,&Snap,&Start,"gt-peer !ai5",3,5,&Control,&Channel),"bound native adapters");
    }
    void Joined(){
        F::Command c{};c.sequence=1;c.operation=Driver::Join;c.target_id=31;strcpy_s(c.body,"gt-peer !ai5");
        driver.Submit(c,3,&context,1,9304);join.join.result=Joiner::kJoinAccepted;join.join.model_room_id=81;join.join.local_is_guest=1;join.join.original_handler_completed=1;driver.Tick(&context);
    }
};
void RemoteSnapshot(){
    snapshot={};snapshot.size=sizeof(snapshot);snapshot.version=SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;snapshot.native_app_state=22;
    snapshot.profile_pilot_id=32;snapshot.room_mode=3;snapshot.received_battle_start_count=7;snapshot.human_participant_count=3;snapshot.bot_count=5;
    wcscpy_s(snapshot.room_title,L"gt-peer !ai5");snapshot.room_title_length=11;
    for(unsigned i=0;i<5;++i){auto& b=snapshot.bots[i];b.pilot_id=0x80000000u+i+1;b.object_id=100+i;b.valid_registered_object=b.position_valid=b.remote_copy=1;b.position_x=static_cast<float>(i);}
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
        World w;w.Joined();RemoteSnapshot();w.driver.Tick(&context);snapshot.bots[0].position_x+=2;w.driver.Tick(&context);
        Check(w.driver.State().remote_bots==5&&w.driver.State().movement_events==1,"actual remote identities and post-control movement are accumulated");
        F::Command c{};c.sequence=2;c.operation=Driver::FinishBattle;c.target_id=31;w.driver.Submit(c,3,&context);
        w.room.end.sequence=4;w.room.end.reply_opcode=0x0871;w.room.end.original_handler_completed=0;w.driver.Tick(&context);
        Check(!w.driver.State().settled,"end packet alone cannot skip original native handler");
        const auto unchanged=w.room.room;w.room.end.original_handler_completed=1;w.driver.Tick(&context);
        Check(w.driver.State().result==F::kReady&&w.driver.State().settled&&w.driver.State().end_sequence==4,"native end and original human-owned room complete the peer battle");
        Check(!memcmp(&unchanged,&w.room.room,sizeof(unchanged)),"observer does not mutate native room metadata");
    }
    std::printf("Coordinated native PvP actions: %d failures\n",failures);return failures?1:0;
}
