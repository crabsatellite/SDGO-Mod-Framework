// Offline state/roster guards; native acceptance still requires two real clients.
#include "../mods/game_test_client/mission_coop_session_actions.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
int failures=0;
void Check(bool ok,const char* message){if(!ok){++failures;std::printf("FAIL %s\n",message);}}

struct FakeRoomObservation {
    uint32_t room_id=0,host_pilot_id=0,player_count=0,occupied_slot_count=0,ai_pilot_count=0;
    uint16_t room_mode=0;
    uint8_t local_is_host=0;
    bool settled=false;
    std::array<uint32_t,12> slot_pilot_ids{};
    std::array<uint8_t,12> slot_ready{};
};
struct FakeRooms {
    FakeRoomObservation first{},current{};int reads=0;
    FakeRoomObservation NativeRoomState(){return reads++?current:first;}
    static bool SettledRoomReady(const FakeRoomObservation& value){return value.settled;}
};
struct FakeMission {
    enum:uint32_t{kWaiting=1,kRoomReady=2,kBattleStartConsumed=3,kInvalidMode=100,kUnbound=101};
    uint32_t ensureResult=kRoomReady,startResult=kBattleStartConsumed;int resets=0,ensures=0,starts=0;
    void Reset(){++resets;}
    static constexpr auto kMissionChannelAccepted=MissionFlowActions::kMissionChannelAccepted;
    uint32_t EnsureRoom(uint16_t,const char*,bool,uint32_t,bool=false){++ensures;return ensureResult;}
    uint32_t Start(){++starts;return startResult;}
};
struct FakeReadyObservation {
    uint32_t result=0,sequence=0,room_id=0,local_slot=1;
    bool server_reply_observed=false,original_dispatch_completed=false,post_dispatch_model_ready=false;
};
struct FakeReady {
    enum:uint32_t{kReadyPending=1,kReadyAccepted=2,kReadyRejected=3};
    FakeReadyObservation observed{};bool start=true;int resets=0,readyCalls=0;uint32_t requestedRoom=0;
    void ResetReadyObservation(){++resets;}
    bool ReadyExactRoom(uint32_t room){++readyCalls;requestedRoom=room;return start;}
    FakeReadyObservation ReadyState()const{return observed;}
};
using Adapter=MissionCoopSessionActionsT<FakeMission,FakeRooms,FakeReady>;
using Command=friends::FriendActions::Command;
FakeRoomObservation Room(uint32_t room,uint32_t host,bool localHost,bool guestReady=false){
    FakeRoomObservation value{};value.settled=true;value.room_id=room;value.host_pilot_id=host;
    value.player_count=2;value.occupied_slot_count=2;value.room_mode=5;value.local_is_host=localHost;
    value.slot_pilot_ids[0]=host;value.slot_pilot_ids[1]=102;value.slot_ready[1]=guestReady;return value;
}
Command Cmd(uint32_t sequence,uint32_t operation,uint32_t host,const char* body=""){
    Command command{};command.sequence=sequence;command.operation=operation;command.target_id=host;
    if(*body)strcpy_s(command.body,body);return command;
}
}

int main(){
    {
        FakeMission mission;FakeRooms rooms;FakeReady ready;Adapter adapter;
        Check(!adapter.Bind(&mission,&rooms,&ready,5,0),"an unsealed mission ID cannot bind");
        Check(!adapter.Bind(&mission,&rooms,&ready,8,88072),"a mission ID cannot bind to the wrong room mode");
        Check(adapter.Bind(&mission,&rooms,&ready,5,88072),"the exact imported Mode5 mission binds");
    }
    {
        FakeMission mission;FakeRooms rooms;FakeReady ready;Adapter adapter;
        adapter.Bind(&mission,&rooms,&ready,5,88072);
        rooms.first={};rooms.current=Room(77,101,true);rooms.current.player_count=1;
        rooms.current.occupied_slot_count=1;rooms.current.slot_pilot_ids[1]=0;
        const auto submitted=adapter.Submit(Cmd(1,Adapter::CreateRoom,101,"sealed imported mission"),1,0);
        adapter.Tick(nullptr);const auto state=adapter.State();
        Check(submitted==friends::FriendActions::kWaitingForLobby&&mission.resets==1&&mission.ensures==1,
              "room creation uses the original mission adapter");
        Check(!adapter.Active()&&state.result==friends::FriendActions::kReady&&state.room==77&&state.roster_verified,
              "room creation rereads and seals the real one-host room");
    }
    {
        FakeMission mission;FakeRooms rooms;FakeReady ready;Adapter adapter;
        adapter.Bind(&mission,&rooms,&ready,5,88072);rooms.first=rooms.current=Room(77,101,false,true);
        ready.observed={FakeReady::kReadyAccepted,9,77,1,true,true,true};
        adapter.Submit(Cmd(2,Adapter::ReadyGuest,101),2,77);adapter.Tick(nullptr);const auto state=adapter.State();
        Check(ready.resets==1&&ready.readyCalls==1&&ready.requestedRoom==77,
              "guest Ready targets the exact joined room once");
        Check(!adapter.Active()&&state.result==friends::FriendActions::kReady&&state.guest_ready&&state.roster_verified,
              "guest Ready completes only after the original reply and model state");
    }
    {
        FakeMission mission;FakeRooms rooms;FakeReady ready;Adapter adapter;
        adapter.Bind(&mission,&rooms,&ready,5,88072);rooms.first=rooms.current=Room(77,101,true,false);
        adapter.Submit(Cmd(3,Adapter::StartMission,101),2,77);adapter.Tick(nullptr);
        Check(adapter.Active()&&mission.starts==0,"host Start waits for the genuine guest-ready slot");
        rooms.first.slot_ready[1]=rooms.current.slot_ready[1]=1;adapter.Tick(nullptr);
        Check(!adapter.Active()&&mission.starts==1&&adapter.State().result==friends::FriendActions::kReady,
              "host Start completes through the original consumed mission start");
        Adapter dirty;FakeMission dirtyMission;FakeRooms dirtyRooms;FakeReady dirtyReady;
        dirty.Bind(&dirtyMission,&dirtyRooms,&dirtyReady,5,88072);dirtyRooms.first=dirtyRooms.current=Room(88,101,true,true);
        dirtyRooms.first.slot_pilot_ids[1]=dirtyRooms.current.slot_pilot_ids[1]=101;
        dirty.Submit(Cmd(4,Adapter::StartMission,101),2,88);dirty.Tick(nullptr);
        Check(dirty.Active()&&dirtyMission.starts==0,"duplicate pilot slots never authorize a mission start");
    }
    std::printf("Mission co-op session offline: %d failures\n",failures);return failures?1:0;
}
