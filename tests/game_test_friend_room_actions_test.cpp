#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../mods/game_test_client/friend_room_actions.h"

template class friends::FriendRoomActionsT<
    NativeActions,
    RoomActions,
    friends::FriendInviteSender,
    friends::FriendInviteReceiver>;

namespace {

using friends::FriendActions;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::printf("FAIL: %s\n", message);
    ++failures;
}

struct FakeNative
{
    struct Onboarding
    {
        uint32_t profile_pilot_id;
        uint16_t channel_id;
    };

    uint32_t app = 6;
    mutable uint32_t runtime_reads = 0;
    Onboarding onboarding = {101, 31};

    uint32_t AppState() const
    {
        ++runtime_reads;
        return app;
    }

    Onboarding OnboardingState() const
    {
        ++runtime_reads;
        return onboarding;
    }
};

struct FakeRooms
{
    enum LeaveResult : uint32_t
    {
        kLeaveNone = 0,
        kLeaveRejected = 1,
        kLeaveAccepted = 2,
    };

    struct Observation
    {
        uint32_t reply_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t room_id;
        uint16_t reply_opcode;
        uint8_t room_variant;
        uint8_t reserved;
    };

    struct LeaveObservation
    {
        uint32_t reply_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint16_t reply_opcode;
        uint16_t error;
        uint8_t room_object_type;
        uint8_t reserved[3];
    };

    struct NativeRoomObservation
    {
        uint32_t native_app_state;
        uint32_t room_id;
        uint32_t player_count;
        uint8_t room_variant;
        uint8_t room_mode;
        uint8_t model_present;
        uint8_t local_is_host;
        uint8_t wait_menu_observed;
        uint8_t reserved[3];
    };

    struct FreshRoomObservation
    {
        uint32_t sequence;
        uint32_t native_app_state;
        uint32_t room_id;
        uint8_t room_variant;
        uint8_t room_mode;
        uint8_t model_present;
        uint8_t original_factory_succeeded;
    };

    Observation create = {};
    LeaveObservation leave = {};
    NativeRoomObservation nativeRoom = {};
    FreshRoomObservation fresh = {};
    bool leaveResult = true;
    bool leaveReady = true;
    unsigned leaveCalls = 0;

    bool LeaveReady() const { return leaveReady; }

    bool Leave()
    {
        ++leaveCalls;
        return leaveResult;
    }

    Observation CreateState() const
    {
        return create;
    }

    LeaveObservation LeaveState() const
    {
        return leave;
    }

    NativeRoomObservation NativeRoomState() const
    {
        return nativeRoom;
    }

    FreshRoomObservation FreshRoomState() const
    {
        return fresh;
    }
};

struct FakeSender
{
    enum Result : uint32_t
    {
        kUnbound = 0,
        kRequestSent = 1,
        kRequestQueued = 2,
        kRequestPending = 3,
        kInvalidTarget = 0x100,
        kSelectedTargetMismatch = 0x104,
        kFriendNotAccepted = 0x105,
        kNativeFault = 0x10A,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        int32_t selected_conversation;
        uint32_t conversation_count;
        uint32_t recipient_id;
        uint32_t transport_sequence_before;
        uint32_t transport_sequence_after;
        uint32_t request_size;
        uint32_t transport_result;
        uint16_t request_opcode;
        uint8_t relationship;
        uint8_t messenger_identity_matched;
        uint8_t selected_target_matched;
        uint8_t accepted_friend_matched;
        uint8_t invite_control_found;
        uint8_t invite_control_name_matched;
        uint8_t invite_control_parent_matched;
        uint8_t invite_control_visible;
        uint8_t invite_control_enabled;
        uint8_t callback_slot_matched;
        uint8_t business_slot_matched;
        uint8_t request_sent;
        uint8_t recipient_correlated;
        uint8_t three_d_room_path;
        uint8_t three_d_queue_matched;
        uint8_t request_queued;
        uint8_t queued_record_matched;
        uint8_t messenger_service_found;
        uint8_t messenger_service_class_matched;
        uint8_t messenger_popup_found;
        uint8_t messenger_popup_class_matched;
        uint8_t messenger_owner_popup_matched;
        uint8_t three_d_event_queue_matched;
        uint8_t messenger_open_requested;
        uint8_t messenger_open_result;
        uint8_t messenger_manager_list_valid;
        uint8_t player_data_found;
        uint8_t owner_popup_class_matched;
        uint8_t ui_load_manager_present;
        uint8_t ui_load_busy;
        uint8_t messenger_waited_for_ui_loader;
        uint8_t messenger_waiting_for_ui_loader;
        uint8_t messenger_open_attempts;
        uint32_t messenger_service_address;
        uint32_t messenger_service_vtable;
        uint32_t expected_messenger_service_vtable;
        uint32_t messenger_manager_address;
        uint32_t messenger_popup_address;
        uint32_t messenger_popup_vtable;
        uint32_t expected_messenger_popup_vtable;
        uint32_t player_ui_phase;
        uint32_t structural_popup_address;
        uint32_t structural_popup_matches;
        uint32_t ordinary_popup_address;
        uint32_t ordinary_popup_matches;
        uint32_t messenger_service_control_root;
        uint32_t messenger_owner_popup;
        uint32_t messenger_owner_popup_vtable;
        uint32_t three_d_service_event_queue;
        uint32_t three_d_popup_event_queue;
        char recipient_name[17];
    };

    Observation observation = {};
    Result sendResult = kRequestQueued;
    unsigned sendCalls = 0;
    unsigned pollCalls = 0;

    Result Send(uint32_t targetId, const char* targetName)
    {
        ++sendCalls;
        observation = {};
        observation.result = sendResult;
        observation.native_app_state = 18;
        observation.selected_conversation = 0;
        observation.conversation_count = 1;
        observation.recipient_id = targetId;
        observation.transport_sequence_before = 40;
        observation.relationship = 1;
        observation.selected_target_matched = 1;
        observation.accepted_friend_matched = 1;
        observation.invite_control_found = 1;
        observation.invite_control_name_matched = 1;
        observation.invite_control_parent_matched = 1;
        observation.invite_control_visible = 1;
        observation.invite_control_enabled = 1;
        observation.three_d_room_path = 1;
        observation.three_d_queue_matched = 1;
        observation.request_queued = 1;
        observation.queued_record_matched = 1;
        observation.messenger_service_found = 1;
        observation.
            messenger_service_class_matched = 1;
        observation.messenger_popup_found = 1;
        observation.messenger_popup_class_matched = 1;
        observation.messenger_owner_popup_matched = 1;
        observation.three_d_event_queue_matched = 1;
        observation.messenger_open_requested = 1;
        observation.messenger_open_result = 1;
        observation.messenger_manager_list_valid = 1;
        observation.player_data_found = 1;
        observation.owner_popup_class_matched = 1;
        observation.ui_load_manager_present = 1;
        observation.ui_load_busy = 0;
        observation.
            messenger_waited_for_ui_loader = 1;
        observation.
            messenger_waiting_for_ui_loader = 0;
        observation.messenger_open_attempts = 1;
        observation.messenger_service_address =
            0x1000;
        observation.messenger_service_vtable =
            0x2000;
        observation.
            expected_messenger_service_vtable =
            0x2000;
        observation.messenger_manager_address =
            0x6000;
        observation.messenger_popup_address =
            0x3000;
        observation.messenger_popup_vtable =
            0x4000;
        observation.expected_messenger_popup_vtable =
            0x4000;
        observation.player_ui_phase = 2;
        observation.structural_popup_address =
            0x3000;
        observation.structural_popup_matches = 1;
        observation.ordinary_popup_address = 0;
        observation.ordinary_popup_matches = 0;
        observation.messenger_service_control_root =
            0x7000;
        observation.messenger_owner_popup =
            0x3000;
        observation.messenger_owner_popup_vtable =
            0x4000;
        observation.three_d_service_event_queue =
            0x5000;
        observation.three_d_popup_event_queue =
            0x5000;
        strcpy_s(
            observation.recipient_name,
            targetName);
        return sendResult;
    }

    Result Poll()
    {
        ++pollCalls;
        if (pollCalls == 1) {
            observation.result = kRequestPending;
            return kRequestPending;
        }
        observation.result = kRequestSent;
        observation.transport_sequence_after = 41;
        observation.request_size = 0x1E;
        observation.transport_result = 1;
        observation.request_opcode = 0x077E;
        observation.request_sent = 1;
        observation.recipient_correlated = 1;
        return kRequestSent;
    }

    Observation State() const
    {
        return observation;
    }
};

struct FakeReceiver
{
    enum Decision : uint8_t
    {
        kNoDecision = 0,
        kAccept = 1,
        kReject = 2,
    };

    enum Phase : uint32_t
    {
        kIdle = 0,
        kArmed = 1,
        kDispatchObserved = 2,
        kPromptConstructed = 3,
        kPromptQueued = 4,
        kPromptInitialized = 5,
        kDecisionSent = 6,
        kJoinContinuationObserved = 7,
        kJoinRequestSent = 8,
        kRoomJoined = 9,
        kRejected = 10,
        kFailed = 0x100,
    };

    enum Failure : uint32_t
    {
        kNoFailure = 0,
        kNotBound = 1,
        kInvalidExpectation = 2,
    };

    struct ExpectedInvite
    {
        uint16_t channel_id;
        uint16_t reserved;
        uint32_t room_id;
        uint32_t inviter_id;
        uint8_t decision;
        uint8_t reserved2[3];
        const char* inviter_name;
    };

    struct Observation
    {
        uint32_t phase;
        uint32_t failure;
        uint32_t native_app_state;
        uint32_t inviter_id;
        uint32_t room_id;
        uint16_t channel_id;
        uint16_t incoming_opcode;
        uint16_t decision_opcode;
        uint16_t join_opcode;
        uint32_t decision_packet_size;
        uint32_t decision_transport_result;
        uint32_t decision_transport_sequence_before;
        uint32_t decision_transport_sequence_after;
        uint32_t join_packet_size;
        uint32_t join_transport_result;
        uint32_t join_transport_sequence_before;
        uint32_t join_transport_sequence_after;
        uintptr_t source_prompt;
        uintptr_t managed_prompt;
        uintptr_t layout_root;
        uintptr_t allow_control;
        uintptr_t refuse_control;
        uintptr_t profile_room;
        uint32_t model_room_id;
        uint8_t native_flag;
        uint8_t decision;
        uint8_t incoming_packet_matched;
        uint8_t prompt_constructed;
        uint8_t prompt_clone_created;
        uint8_t prompt_clone_identity_matched;
        uint8_t prompt_initialized;
        uint8_t prompt_destroyed;
        uint8_t allow_control_matched;
        uint8_t refuse_control_matched;
        uint8_t allow_control_enabled;
        uint8_t refuse_control_enabled;
        uint8_t allow_control_hit;
        uint8_t refuse_control_hit;
        uint8_t controls_ready;
        uint8_t decision_invoked;
        uint8_t decision_sent;
        uint8_t invitation_context_matched;
        uint8_t join_continuation_observed;
        uint8_t join_request_sent;
        uint8_t room_model_matched;
        uint8_t local_guest;
        uint8_t wait_room_owner_matched;
        uint8_t manager_vtable_matched;
        uint8_t reserved3[2];
        char inviter_name[17];
        wchar_t prompt_name[17];
    };

    Observation observation = {};
    bool armResult = true;
    bool invokeResult = true;
    unsigned armCalls = 0;
    unsigned invokeCalls = 0;
    unsigned refreshCalls = 0;

    bool Arm(const ExpectedInvite& expected)
    {
        ++armCalls;
        observation = {};
        if (!armResult) {
            observation.failure = kInvalidExpectation;
            observation.phase = kFailed;
            return false;
        }
        observation.phase = kArmed;
        observation.native_app_state = 6;
        observation.channel_id = expected.channel_id;
        observation.room_id = expected.room_id;
        observation.inviter_id = expected.inviter_id;
        observation.decision = expected.decision;
        strcpy_s(
            observation.inviter_name,
            expected.inviter_name);
        return true;
    }

    bool InvokeDecision()
    {
        ++invokeCalls;
        if (!invokeResult)
            return false;
        observation.decision_invoked = 1;
        observation.decision_sent = 1;
        observation.decision_packet_size = 0x11;
        observation.decision_transport_result = 1;
        observation.decision_transport_sequence_before = 50;
        observation.decision_transport_sequence_after = 51;
        observation.decision_opcode =
            observation.decision == kAccept
                ? 0x04A8
                : 0x07A9;
        observation.phase = kDecisionSent;
        return true;
    }

    Observation Refresh()
    {
        ++refreshCalls;
        return observation;
    }
};

using Bridge = friends::FriendRoomActionsT<
    FakeNative,
    FakeRooms,
    FakeSender,
    FakeReceiver>;

struct CreateScript
{
    FakeNative* native;
    FakeRooms* rooms;
    unsigned calls;
    bool installRoom;
};

FriendActions::Result CreateStep(
    void* opaque,
    const SdgoGameTestContextV1*)
{
    auto* script = static_cast<CreateScript*>(opaque);
    ++script->calls;
    if (script->calls == 1)
        return FriendActions::kWaitingForReply;
    if (script->installRoom) {
        script->native->app = 18;
        script->rooms->nativeRoom.native_app_state = 18;
        script->rooms->nativeRoom.room_id = 9001;
        script->rooms->nativeRoom.player_count = 1;
        script->rooms->nativeRoom.room_variant = 0;
        script->rooms->nativeRoom.room_mode = 1;
        script->rooms->nativeRoom.model_present = 1;
        script->rooms->nativeRoom.local_is_host = 1;
        script->rooms->create.reply_sequence = 7;
        script->rooms->create.result = 2;
        script->rooms->create.room_id = 9001;
        script->rooms->create.reply_opcode = 0x0918;
        script->rooms->fresh.sequence = 3;
        script->rooms->fresh.room_id = 9001;
        script->rooms->fresh.model_present = 1;
        script->rooms->fresh.original_factory_succeeded = 1;
    }
    return FriendActions::kReplyObserved;
}

FriendActions::Command Command(
    uint32_t sequence,
    uint32_t operation,
    uint32_t targetId = 0,
    const char* targetName = "")
{
    FriendActions::Command command = {};
    command.sequence = sequence;
    command.operation = operation;
    command.target_id = targetId;
    strcpy_s(command.target_name, targetName);
    return command;
}

void TestCreateRequiresRealRoom()
{
    FakeNative native;
    FakeRooms rooms;
    FakeSender sender;
    FakeReceiver receiver;
    CreateScript script = {&native, &rooms, 0, true};
    Bridge bridge;
    Check(
        bridge.Bind(
            &native,
            &rooms,
            &sender,
            &receiver,
            &CreateStep,
            &script),
        "bridge binds all native room collaborators");
    Check(
        bridge.Submit(
            Command(1, Bridge::kCreateRoom),
            31,
            0) == FriendActions::kWaitingForLobby,
        "create accepts an unknown expected server room id");
    bridge.Tick(nullptr);
    Check(
        bridge.Active() &&
            bridge.State().result ==
                FriendActions::kWaitingForReply,
        "create remains active while the injected native step waits");
    bridge.Tick(nullptr);
    const auto created = bridge.State();
    const auto room = bridge.RoomState();
    Check(
        !bridge.Active() &&
            created.result == FriendActions::kReplyObserved,
        "create completes only after the injected native step passes");
    Check(
        room.actual_room_id == 9001 &&
            room.actual_channel_id == 31 &&
            room.model_present &&
            room.local_is_host &&
            room.player_count == 1,
        "create publishes the actual channel, room, model, host and player count");

    script.calls = 0;
    script.installRoom = false;
    rooms.nativeRoom = {};
    native.app = 6;
    Check(
        bridge.Submit(
            Command(2, Bridge::kCreateRoom),
            31,
            0) == FriendActions::kWaitingForLobby,
        "a second create command starts independently");
    bridge.Tick(nullptr);
    bridge.Tick(nullptr);
    Check(
        bridge.State().result ==
                FriendActions::kReplyRejected &&
            bridge.RoomState().guard_failure ==
                Bridge::kGuardRoomModelMissing,
        "a callback PASS cannot fabricate a missing native room model");
}

void InstallHostedRoom(
    FakeNative* native,
    FakeRooms* rooms,
    uint32_t roomId)
{
    native->app = 18;
    native->onboarding.channel_id = 31;
    rooms->nativeRoom.native_app_state = 18;
    rooms->nativeRoom.room_id = roomId;
    rooms->nativeRoom.player_count = 1;
    rooms->nativeRoom.model_present = 1;
    rooms->nativeRoom.local_is_host = 1;
}

void TestInviteSendWaitsForTransport()
{
    FakeNative native;
    FakeRooms rooms;
    FakeSender sender;
    FakeReceiver receiver;
    CreateScript script = {&native, &rooms, 0, true};
    Bridge bridge;
    bridge.Bind(
        &native,
        &rooms,
        &sender,
        &receiver,
        &CreateStep,
        &script);
    InstallHostedRoom(&native, &rooms, 9001);

    Check(
        bridge.Submit(
            Command(
                1,
                Bridge::kSendInvite,
                202,
                "FriendTwo"),
            31,
            9001) == FriendActions::kWaitingForReply,
        "queued 3D invite remains pending");
    Check(
        sender.sendCalls == 1,
        "invite sender is invoked exactly once");
    bridge.Tick(nullptr);
    Check(
        bridge.Active() &&
            bridge.State().result ==
                FriendActions::kWaitingForReply,
        "queued invite does not complete before transport changes");
    bridge.Tick(nullptr);
    const auto state = bridge.State();
    Check(
        !bridge.Active() &&
            state.result == FriendActions::kReplyObserved &&
            state.request_sent &&
            state.request_opcode == 0x077E &&
            state.request_transport_sequence == 41,
        "invite completes on the actual 077E transport receipt");
    Check(
        sender.sendCalls == 1 && sender.pollCalls == 2,
        "pending invite polls without resending");
}

void ReadyPrompt(FakeReceiver* receiver)
{
    receiver->observation.phase =
        FakeReceiver::kPromptInitialized;
    receiver->observation.prompt_constructed = 1;
    receiver->observation.prompt_clone_created = 1;
    receiver->observation.prompt_clone_identity_matched = 1;
    receiver->observation.prompt_initialized = 1;
    receiver->observation.allow_control_matched = 1;
    receiver->observation.refuse_control_matched = 1;
    receiver->observation.allow_control_enabled = 1;
    receiver->observation.refuse_control_enabled = 1;
    receiver->observation.allow_control_hit = 1;
    receiver->observation.refuse_control_hit = 1;
    receiver->observation.controls_ready = 1;
}

void TestArmAndRejectDecision()
{
    FakeNative native;
    FakeRooms rooms;
    FakeSender sender;
    FakeReceiver receiver;
    CreateScript script = {&native, &rooms, 0, true};
    Bridge bridge;
    bridge.Bind(
        &native,
        &rooms,
        &sender,
        &receiver,
        &CreateStep,
        &script);

    Check(
        bridge.Submit(
            Command(
                1,
                Bridge::kArmReject,
                303,
                "Inviter"),
            31,
            7007) == FriendActions::kReady,
        "reject arm copies the exact invitation identity");
    const unsigned refreshAfterArm = receiver.refreshCalls;
    ReadyPrompt(&receiver);
    bridge.Tick(nullptr);
    Check(
        receiver.refreshCalls > refreshAfterArm &&
            bridge.InviteReceiverState().controls_ready,
        "receiver refresh stays passive after the arm command is done");

    receiver.observation.controls_ready = 0;
    Check(
        bridge.Submit(
            Command(
                2,
                Bridge::kDecide,
                303,
                "Inviter"),
            31,
            7007) == FriendActions::kWaitingForReply,
        "decision command waits on the armed invitation");
    bridge.Tick(nullptr);
    Check(
        receiver.invokeCalls == 0 &&
            bridge.RoomState().guard_failure ==
                Bridge::kGuardDecisionNotReady,
        "decision never calls the original callback before controls are ready");

    ReadyPrompt(&receiver);
    bridge.Tick(nullptr);
    Check(
        receiver.invokeCalls == 1 &&
            bridge.Active() &&
            receiver.observation.phase ==
                FakeReceiver::kDecisionSent,
        "ready reject invokes the original callback once");
    receiver.observation.prompt_destroyed = 1;
    receiver.observation.phase = FakeReceiver::kRejected;
    bridge.Tick(nullptr);
    Check(
        !bridge.Active() &&
            bridge.State().result ==
                FriendActions::kReplyObserved &&
            bridge.State().request_opcode == 0x07A9,
        "reject completes only after the real prompt lifetime ends");
}

void TestAcceptRequiresRoomJoin()
{
    FakeNative native;
    FakeRooms rooms;
    FakeSender sender;
    FakeReceiver receiver;
    CreateScript script = {&native, &rooms, 0, true};
    Bridge bridge;
    bridge.Bind(
        &native,
        &rooms,
        &sender,
        &receiver,
        &CreateStep,
        &script);

    bridge.Submit(
        Command(
            1,
            Bridge::kArmAccept,
            404,
            "HostPilot"),
        31,
        8118);
    ReadyPrompt(&receiver);
    bridge.Submit(
        Command(
            2,
            Bridge::kDecide,
            404,
            "HostPilot"),
        31,
        8118);
    bridge.Tick(nullptr);
    Check(
        receiver.invokeCalls == 1 && bridge.Active(),
        "accept invokes the original callback when its prompt is ready");

    receiver.observation.phase =
        FakeReceiver::kJoinRequestSent;
    receiver.observation.join_request_sent = 1;
    receiver.observation.join_opcode = 0x0734;
    receiver.observation.join_packet_size = 0x32;
    receiver.observation.join_transport_result = 1;
    receiver.observation.join_transport_sequence_before = 61;
    receiver.observation.join_transport_sequence_after = 62;
    bridge.Tick(nullptr);
    Check(
        bridge.Active() &&
            bridge.State().result ==
                FriendActions::kWaitingForReply,
        "accept does not complete at the join request");

    receiver.observation.phase = FakeReceiver::kRoomJoined;
    receiver.observation.model_room_id = 8118;
    receiver.observation.room_model_matched = 1;
    receiver.observation.local_guest = 1;
    receiver.observation.wait_room_owner_matched = 1;
    receiver.observation.manager_vtable_matched = 1;
    bridge.Tick(nullptr);
    Check(
        !bridge.Active() &&
            bridge.State().result ==
                FriendActions::kReplyObserved &&
            bridge.State().reply_opcode == 0x0734,
        "accept completes only at the receiver's real room-joined phase");
}

void TestLeaveRequiresFreshReplyAndClearedModel()
{
    FakeNative native;
    FakeRooms rooms;
    FakeSender sender;
    FakeReceiver receiver;
    CreateScript script = {&native, &rooms, 0, true};
    Bridge bridge;
    bridge.Bind(
        &native,
        &rooms,
        &sender,
        &receiver,
        &CreateStep,
        &script);
    InstallHostedRoom(&native, &rooms, 9001);
    rooms.leave.reply_sequence = 10;
    rooms.leave.result = FakeRooms::kLeaveAccepted;
    rooms.leave.reply_opcode = 0x057B;
    rooms.leaveReady = false;

    Check(
        bridge.Submit(
            Command(1, Bridge::kLeave),
            31,
            9001) == FriendActions::kWaitingForReply &&
            rooms.leaveCalls == 0,
        "leave waits for the native room Main stage after model publication");
    bridge.Tick(nullptr);
    Check(bridge.Active() && rooms.leaveCalls == 0,
          "loading room does not invoke or force its leave business");
    rooms.leaveReady = true;
    bridge.Tick(nullptr);
    Check(
        bridge.Active() && rooms.leaveCalls == 1,
        "ready room invokes leave once and ignores a stale accepted receipt");

    rooms.leave.reply_sequence = 11;
    bridge.Tick(nullptr);
    Check(
        bridge.Active() &&
            bridge.RoomState().guard_failure ==
                Bridge::kGuardLeaveTransitionPending,
        "accepted leave reply still waits for lobby state and model cleanup");

    native.app = 6;
    rooms.nativeRoom = {};
    rooms.nativeRoom.native_app_state = 6;
    bridge.Tick(nullptr);
    Check(
        !bridge.Active() &&
            bridge.State().result ==
                FriendActions::kReplyObserved &&
            bridge.State().reply_sequence == 11 &&
            bridge.State().reply_opcode == 0x057B,
        "leave completes on a fresh accepted reply, app state 6 and no profile room model");
}

void TestIniOutput()
{
    FakeNative native;
    FakeRooms rooms;
    FakeSender sender;
    FakeReceiver receiver;
    CreateScript script = {&native, &rooms, 0, true};
    Bridge bridge;
    bridge.Bind(
        &native,
        &rooms,
        &sender,
        &receiver,
        &CreateStep,
        &script);
    bridge.Submit(
        Command(
            1,
            Bridge::kArmAccept,
            505,
            "AsciiPilot"),
        31,
        8123);

    FILE* file = nullptr;
    Check(
        tmpfile_s(&file) == 0 && file,
        "temporary INI output opens");
    if (!file)
        return;
    bridge.Write(file);
    std::rewind(file);
    std::string text;
    char buffer[512] = {};
    while (std::fgets(buffer, sizeof(buffer), file))
        text += buffer;
    std::fclose(file);
    Check(
        text.find("[room]\n") != std::string::npos &&
            text.find("[invite_sender]\n") !=
                std::string::npos &&
            text.find("[invite_receiver]\n") !=
                std::string::npos,
        "bridge appends the three typed ConfigParser sections");
    Check(
        text.find("inviter_name=AsciiPilot\n") !=
                std::string::npos &&
            text.find(
                "messenger_service_found=0\n") !=
                std::string::npos &&
            text.find(
                "messenger_popup_found=0\n") !=
                std::string::npos &&
            text.find(
                "messenger_open_requested=0\n") !=
                std::string::npos &&
            text.find(
                "messenger_open_result=0\n") !=
                std::string::npos &&
            text.find(
                "messenger_manager_list_valid=0\n") !=
                std::string::npos &&
            text.find(
                "player_ui_phase=0\n") !=
                std::string::npos &&
            text.find(
                "structural_popup_matches=0\n") !=
                std::string::npos &&
            text.find(
                "ui_load_manager_present=0\n") !=
                std::string::npos &&
            text.find(
                "ui_load_busy=0\n") !=
                std::string::npos &&
            text.find(
                "messenger_waited_for_ui_loader=0\n") !=
                std::string::npos &&
            text.find(
                "messenger_open_attempts=0\n") !=
                std::string::npos &&
            text.find("allow_control_enabled=0\n") !=
                std::string::npos &&
            text.find("allow_control_hit=0\n") !=
                std::string::npos &&
            text.find('{') == std::string::npos,
        "INI output publishes resolver diagnostics, enabled/hit flags, UTF-8 names and no JSON");
}

} // namespace

int main()
{
    {
        FakeNative native;FakeRooms roomAdapter;FakeSender sender;FakeReceiver receiver;Bridge bridge;
        Check(bridge.Bind(&native,&roomAdapter,&sender,&receiver,nullptr),"bridge binds static adapters before host initialization");
        Check(native.runtime_reads==0,"binding never creates or reads runtime game singletons before host initialization");
    }
    TestCreateRequiresRealRoom();
    TestInviteSendWaitsForTransport();
    TestArmAndRejectDecision();
    TestAcceptRequiresRoomJoin();
    TestLeaveRequiresFreshReplyAndClearedModel();
    TestIniOutput();
    if (failures) {
        std::printf("%d friend-room bridge checks failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all friend-room bridge checks passed\n");
    return EXIT_SUCCESS;
}
