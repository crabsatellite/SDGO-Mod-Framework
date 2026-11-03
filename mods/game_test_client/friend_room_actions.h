#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "friend_actions.h"
#include "friend_invite_receiver.h"
#include "friend_invite_sender.h"
#include "room_actions.h"
#include "sdgo/game_test.h"

namespace friends {

template <
    typename NativeT,
    typename RoomT,
    typename SenderT,
    typename ReceiverT>
class FriendRoomActionsT
{
public:
    enum Operation : uint32_t
    {
        kCreateRoom = 12,
        kSendInvite = 13,
        kArmAccept = 14,
        kArmReject = 15,
        kDecide = 16,
        kLeave = 17,
    };

    enum GuardFailure : uint32_t
    {
        kGuardNone = 0,
        kGuardUnbound = 1,
        kGuardCreateStepUnavailable = 2,
        kGuardChannelMissing = 3,
        kGuardRoomMissing = 4,
        kGuardRoomModelMissing = 5,
        kGuardRoomIdMismatch = 6,
        kGuardChannelMismatch = 7,
        kGuardHostMismatch = 8,
        kGuardPlayerCountInvalid = 9,
        kGuardReceiverIdentityMismatch = 10,
        kGuardReceiverFailed = 11,
        kGuardDecisionNotReady = 12,
        kGuardLeaveRejected = 13,
        kGuardLeaveTransitionPending = 14,
        kGuardNativeActionRejected = 15,
    };

    using CreateStepFn = FriendActions::Result (*)(
        void* opaque,
        const SdgoGameTestContextV1* context);
    using SenderObservation = typename SenderT::Observation;
    using ReceiverObservation = typename ReceiverT::Observation;

    struct CmdProjection
    {
        uint32_t result;
        uint32_t command_sequence;
        uint32_t operation;
        uint32_t target_id;
        uint32_t native_app_state;
        uint32_t profile_pilot_id;
        uint32_t request_transport_sequence;
        uint32_t reply_sequence;
        uint32_t reply_result;
        uint16_t request_opcode;
        uint16_t reply_opcode;
        uint16_t reply_status;
        uint8_t request_sent;
        uint8_t reply_consumed;
        char target_name[17];
    };

    struct RoomProjection
    {
        uint32_t guard_failure;
        uint32_t native_app_state;
        uint32_t expected_room_id;
        uint32_t actual_room_id;
        uint32_t player_count;
        uint32_t create_reply_sequence;
        uint32_t create_result;
        uint32_t create_reply_room_id;
        uint32_t fresh_sequence;
        uint32_t leave_reply_sequence;
        uint32_t leave_result;
        uint16_t expected_channel_id;
        uint16_t actual_channel_id;
        uint16_t create_reply_opcode;
        uint16_t leave_reply_opcode;
        uint16_t leave_error;
        uint8_t room_variant;
        uint8_t room_mode;
        uint8_t model_present;
        uint8_t local_is_host;
        uint8_t wait_menu_observed;
        uint8_t fresh_model_present;
        uint8_t fresh_factory_succeeded;
        uint8_t leave_requested;
    };

    bool Bind(
        NativeT* native,
        RoomT* roomAdapter,
        SenderT* sender,
        ReceiverT* receiver,
        CreateStepFn createStep,
        void* createOpaque = nullptr)
    {
        native_ = native;
        rooms_ = roomAdapter;
        sender_ = sender;
        receiver_ = receiver;
        createStep_ = createStep;
        createOpaque_ = createOpaque;
        ResetCommand();
        // Mod binding precedes the host CRT/game initialization. Runtime
        // singletons must only be read from later native game-thread ticks.
        return native_ && rooms_ && sender_ && receiver_;
    }

    FriendActions::Result Submit(
        const FriendActions::Command& command,
        uint16_t channelId,
        uint32_t roomId)
    {
        RefreshObservations();
        if (!native_ || !rooms_ || !sender_ || !receiver_)
            return Reject(
                command,
                FriendActions::kNativeFault,
                kGuardUnbound);
        if (active_)
            return FriendActions::kBusy;

        ResetCommand();
        command_.command_sequence = command.sequence;
        command_.operation = command.operation;
        command_.target_id = command.target_id;
        CopyText(
            command_.target_name,
            sizeof(command_.target_name),
            command.target_name);
        room_.expected_channel_id = channelId;
        room_.expected_room_id = roomId;

        switch (command.operation) {
        case kCreateRoom:
            if (!createStep_)
                return Fail(
                    FriendActions::kUnsupported,
                    kGuardCreateStepUnavailable);
            command_.result = FriendActions::kWaitingForLobby;
            active_ = true;
            return FriendActions::kWaitingForLobby;

        case kSendInvite:
            if (!ValidIdentity(command) || !channelId)
                return Fail(
                    FriendActions::kInvalidTarget,
                    channelId ? kGuardRoomMissing : kGuardChannelMissing);
            if (!roomId)
                return Fail(
                    FriendActions::kInvalidTarget,
                    kGuardRoomMissing);
            if (!CurrentRoomMatches(channelId, roomId, true))
                return Fail(
                    FriendActions::kInvalidTarget,
                    static_cast<GuardFailure>(
                        room_.guard_failure));
            return BeginSend(command);

        case kArmAccept:
        case kArmReject:
            if (!ValidIdentity(command) || !channelId || !roomId)
                return Fail(
                    FriendActions::kInvalidTarget,
                    !channelId ? kGuardChannelMissing :
                    (!roomId ? kGuardRoomMissing :
                     kGuardReceiverIdentityMismatch));
            return ArmReceiver(command, channelId, roomId);

        case kDecide:
            if (!channelId || !roomId)
                return Fail(
                    FriendActions::kInvalidTarget,
                    !channelId ? kGuardChannelMissing :
                    kGuardRoomMissing);
            if (!ReceiverMatches(command, channelId, roomId))
                return Fail(
                    FriendActions::kInvalidTarget,
                    kGuardReceiverIdentityMismatch);
            command_.result = FriendActions::kWaitingForReply;
            active_ = true;
            return FriendActions::kWaitingForReply;

        case kLeave:
            if (!CurrentRoomMatches(channelId, roomId, false))
                return Fail(
                    FriendActions::kInvalidTarget,
                    static_cast<GuardFailure>(
                        room_.guard_failure));
            leaveBaseline_ = room_.leave_reply_sequence;
            if (!rooms_->Leave())
                return Fail(
                    FriendActions::kNativeRequestNotSent,
                    kGuardNativeActionRejected);
            room_.leave_requested = 1;
            command_.result = FriendActions::kWaitingForReply;
            active_ = true;
            return FriendActions::kWaitingForReply;

        default:
            return Fail(
                FriendActions::kInvalidCommand,
                kGuardNativeActionRejected);
        }
    }

    void Tick(const SdgoGameTestContextV1* context)
    {
        // The receiver owns native invitation hooks whose state can advance
        // between IPC commands. Refresh it even after an arm receipt has
        // completed so controls_ready and prompt lifetime stay observable.
        RefreshObservations();
        if (!active_)
            return;

        switch (command_.operation) {
        case kCreateRoom:
            TickCreate(context);
            break;
        case kSendInvite:
            TickSend();
            break;
        case kDecide:
            TickDecision();
            break;
        case kLeave:
            TickLeave();
            break;
        default:
            Fail(
                FriendActions::kInvalidCommand,
                kGuardNativeActionRejected);
            break;
        }
    }

    CmdProjection State() const
    {
        return command_;
    }

    RoomProjection RoomState() const
    {
        return room_;
    }

    SenderObservation InviteSenderState() const
    {
        return senderState_;
    }

    ReceiverObservation InviteReceiverState() const
    {
        return receiverState_;
    }

    bool Active() const
    {
        return active_;
    }

    void ResetCommand()
    {
        std::memset(&command_, 0, sizeof(command_));
        command_.native_app_state = UINT32_MAX;
        active_ = false;
        decisionInvoked_ = false;
        leaveBaseline_ = 0;
        room_.guard_failure = kGuardNone;
        room_.leave_requested = 0;
    }

    void Write(FILE* file) const
    {
        if (!file)
            return;
        const std::string recipient = Utf8(senderState_.recipient_name);
        const std::string inviter = Utf8(receiverState_.inviter_name);
        std::fprintf(
            file,
            "[room]\n"
            "guard_failure=%u\n"
            "app=%u\n"
            "expected_channel_id=%u\n"
            "actual_channel_id=%u\n"
            "expected_room_id=%u\n"
            "actual_room_id=%u\n"
            "player_count=%u\n"
            "room_variant=%u\n"
            "room_mode=%u\n"
            "model_present=%u\n"
            "local_is_host=%u\n"
            "wait_menu_observed=%u\n"
            "create_reply_sequence=%u\n"
            "create_result=%u\n"
            "create_reply_opcode=%u\n"
            "create_reply_room_id=%u\n"
            "fresh_sequence=%u\n"
            "fresh_model_present=%u\n"
            "fresh_factory_succeeded=%u\n"
            "leave_requested=%u\n"
            "leave_reply_sequence=%u\n"
            "leave_result=%u\n"
            "leave_reply_opcode=%u\n"
            "leave_error=%u\n",
            room_.guard_failure,
            room_.native_app_state,
            room_.expected_channel_id,
            room_.actual_channel_id,
            room_.expected_room_id,
            room_.actual_room_id,
            room_.player_count,
            room_.room_variant,
            room_.room_mode,
            room_.model_present,
            room_.local_is_host,
            room_.wait_menu_observed,
            room_.create_reply_sequence,
            room_.create_result,
            room_.create_reply_opcode,
            room_.create_reply_room_id,
            room_.fresh_sequence,
            room_.fresh_model_present,
            room_.fresh_factory_succeeded,
            room_.leave_requested,
            room_.leave_reply_sequence,
            room_.leave_result,
            room_.leave_reply_opcode,
            room_.leave_error);
        std::fprintf(
            file,
            "[invite_sender]\n"
            "result=%u\n"
            "app=%u\n"
            "messenger_service_found=%u\n"
            "messenger_service_class_matched=%u\n"
            "messenger_service_address=%u\n"
            "messenger_service_vtable=%u\n"
            "expected_messenger_service_vtable=%u\n"
            "messenger_manager_address=%u\n"
            "messenger_manager_list_valid=%u\n"
            "messenger_popup_found=%u\n"
            "messenger_popup_class_matched=%u\n"
            "messenger_popup_address=%u\n"
            "messenger_popup_vtable=%u\n"
            "expected_messenger_popup_vtable=%u\n"
            "player_data_found=%u\n"
            "player_ui_phase=%u\n"
            "structural_popup_address=%u\n"
            "structural_popup_matches=%u\n"
            "ordinary_popup_address=%u\n"
            "ordinary_popup_matches=%u\n"
            "messenger_service_control_root=%u\n"
            "messenger_owner_popup=%u\n"
            "messenger_owner_popup_vtable=%u\n"
            "owner_popup_class_matched=%u\n"
            "messenger_owner_popup_matched=%u\n"
            "three_d_service_event_queue=%u\n"
            "three_d_popup_event_queue=%u\n"
            "three_d_event_queue_matched=%u\n"
            "messenger_open_requested=%u\n"
            "messenger_open_result=%u\n"
            "ui_load_manager_present=%u\n"
            "ui_load_busy=%u\n"
            "messenger_waited_for_ui_loader=%u\n"
            "messenger_waiting_for_ui_loader=%u\n"
            "messenger_open_attempts=%u\n"
            "selected_conversation=%d\n"
            "conversation_count=%u\n"
            "recipient_id=%u\n"
            "recipient_name=%s\n"
            "relationship=%u\n"
            "selected_target_matched=%u\n"
            "accepted_friend_matched=%u\n"
            "invite_control_found=%u\n"
            "invite_control_name_matched=%u\n"
            "invite_control_parent_matched=%u\n"
            "invite_control_visible=%u\n"
            "invite_control_enabled=%u\n"
            "three_d_room_path=%u\n"
            "three_d_queue_matched=%u\n"
            "request_queued=%u\n"
            "queued_record_matched=%u\n"
            "request_sent=%u\n"
            "request_opcode=%u\n"
            "request_size=%u\n"
            "transport_result=%u\n"
            "transport_sequence_before=%u\n"
            "transport_sequence_after=%u\n"
            "recipient_correlated=%u\n",
            senderState_.result,
            senderState_.native_app_state,
            senderState_.messenger_service_found,
            senderState_.
                messenger_service_class_matched,
            senderState_.messenger_service_address,
            senderState_.messenger_service_vtable,
            senderState_.
                expected_messenger_service_vtable,
            senderState_.messenger_manager_address,
            senderState_.
                messenger_manager_list_valid,
            senderState_.messenger_popup_found,
            senderState_.
                messenger_popup_class_matched,
            senderState_.messenger_popup_address,
            senderState_.messenger_popup_vtable,
            senderState_.
                expected_messenger_popup_vtable,
            senderState_.player_data_found,
            senderState_.player_ui_phase,
            senderState_.structural_popup_address,
            senderState_.structural_popup_matches,
            senderState_.ordinary_popup_address,
            senderState_.ordinary_popup_matches,
            senderState_.
                messenger_service_control_root,
            senderState_.messenger_owner_popup,
            senderState_.
                messenger_owner_popup_vtable,
            senderState_.
                owner_popup_class_matched,
            senderState_.
                messenger_owner_popup_matched,
            senderState_.
                three_d_service_event_queue,
            senderState_.three_d_popup_event_queue,
            senderState_.
                three_d_event_queue_matched,
            senderState_.messenger_open_requested,
            senderState_.messenger_open_result,
            senderState_.ui_load_manager_present,
            senderState_.ui_load_busy,
            senderState_.
                messenger_waited_for_ui_loader,
            senderState_.
                messenger_waiting_for_ui_loader,
            senderState_.messenger_open_attempts,
            senderState_.selected_conversation,
            senderState_.conversation_count,
            senderState_.recipient_id,
            recipient.c_str(),
            senderState_.relationship,
            senderState_.selected_target_matched,
            senderState_.accepted_friend_matched,
            senderState_.invite_control_found,
            senderState_.invite_control_name_matched,
            senderState_.invite_control_parent_matched,
            senderState_.invite_control_visible,
            senderState_.invite_control_enabled,
            senderState_.three_d_room_path,
            senderState_.three_d_queue_matched,
            senderState_.request_queued,
            senderState_.queued_record_matched,
            senderState_.request_sent,
            senderState_.request_opcode,
            senderState_.request_size,
            senderState_.transport_result,
            senderState_.transport_sequence_before,
            senderState_.transport_sequence_after,
            senderState_.recipient_correlated);
        std::fprintf(
            file,
            "[invite_receiver]\n"
            "phase=%u\n"
            "failure=%u\n"
            "app=%u\n"
            "channel_id=%u\n"
            "room_id=%u\n"
            "inviter_id=%u\n"
            "inviter_name=%s\n"
            "decision=%u\n"
            "incoming_opcode=%u\n"
            "incoming_packet_matched=%u\n"
            "prompt_constructed=%u\n"
            "prompt_clone_created=%u\n"
            "prompt_clone_identity_matched=%u\n"
            "prompt_initialized=%u\n"
            "prompt_destroyed=%u\n"
            "allow_control_matched=%u\n"
            "refuse_control_matched=%u\n"
            "allow_control_enabled=%u\n"
            "refuse_control_enabled=%u\n"
            "allow_control_hit=%u\n"
            "refuse_control_hit=%u\n"
            "controls_ready=%u\n"
            "decision_invoked=%u\n"
            "decision_sent=%u\n"
            "decision_opcode=%u\n"
            "decision_packet_size=%u\n"
            "decision_transport_result=%u\n"
            "decision_transport_sequence_before=%u\n"
            "decision_transport_sequence_after=%u\n"
            "invitation_context_matched=%u\n"
            "join_continuation_observed=%u\n"
            "join_request_sent=%u\n"
            "join_opcode=%u\n"
            "join_packet_size=%u\n"
            "join_transport_result=%u\n"
            "join_transport_sequence_before=%u\n"
            "join_transport_sequence_after=%u\n"
            "model_room_id=%u\n"
            "room_model_matched=%u\n"
            "local_guest=%u\n"
            "wait_room_owner_matched=%u\n"
            "manager_vtable_matched=%u\n",
            receiverState_.phase,
            receiverState_.failure,
            receiverState_.native_app_state,
            receiverState_.channel_id,
            receiverState_.room_id,
            receiverState_.inviter_id,
            inviter.c_str(),
            receiverState_.decision,
            receiverState_.incoming_opcode,
            receiverState_.incoming_packet_matched,
            receiverState_.prompt_constructed,
            receiverState_.prompt_clone_created,
            receiverState_.prompt_clone_identity_matched,
            receiverState_.prompt_initialized,
            receiverState_.prompt_destroyed,
            receiverState_.allow_control_matched,
            receiverState_.refuse_control_matched,
            receiverState_.allow_control_enabled,
            receiverState_.refuse_control_enabled,
            receiverState_.allow_control_hit,
            receiverState_.refuse_control_hit,
            receiverState_.controls_ready,
            receiverState_.decision_invoked,
            receiverState_.decision_sent,
            receiverState_.decision_opcode,
            receiverState_.decision_packet_size,
            receiverState_.decision_transport_result,
            receiverState_.decision_transport_sequence_before,
            receiverState_.decision_transport_sequence_after,
            receiverState_.invitation_context_matched,
            receiverState_.join_continuation_observed,
            receiverState_.join_request_sent,
            receiverState_.join_opcode,
            receiverState_.join_packet_size,
            receiverState_.join_transport_result,
            receiverState_.join_transport_sequence_before,
            receiverState_.join_transport_sequence_after,
            receiverState_.model_room_id,
            receiverState_.room_model_matched,
            receiverState_.local_guest,
            receiverState_.wait_room_owner_matched,
            receiverState_.manager_vtable_matched);
    }

private:
    static constexpr uint16_t kLeaveReplyOpcode = 0x057B;

    static size_t BoundedLength(const char* text, size_t limit)
    {
        if (!text)
            return 0;
        size_t length = 0;
        while (length <= limit && text[length])
            ++length;
        return length;
    }

    static void CopyText(char* destination, size_t capacity, const char* source)
    {
        if (!destination || !capacity)
            return;
        std::memset(destination, 0, capacity);
        const size_t length = (std::min)(
            BoundedLength(source, capacity - 1),
            capacity - 1);
        if (length)
            std::memcpy(destination, source, length);
    }

    static bool SameText(const char* left, const char* right)
    {
        const size_t leftLength = BoundedLength(left, 16);
        const size_t rightLength = BoundedLength(right, 16);
        return leftLength == rightLength &&
            leftLength <= 16 &&
            std::memcmp(left, right, leftLength) == 0;
    }

    static std::string Utf8(const char* value)
    {
        if (!value)
            return "";
        wchar_t wide[64] = {};
        char encoded[256] = {};
        if (!MultiByteToWideChar(
                CP_ACP,
                0,
                value,
                -1,
                wide,
                static_cast<int>(_countof(wide))) ||
            !WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                wide,
                -1,
                encoded,
                static_cast<int>(_countof(encoded)),
                nullptr,
                nullptr))
            return "<invalid-native-text>";
        std::string result(encoded);
        for (char& character : result) {
            if (character == '\r' || character == '\n')
                character = '?';
        }
        return result;
    }

    static bool ValidIdentity(const FriendActions::Command& command)
    {
        const size_t length = BoundedLength(command.target_name, 16);
        return command.target_id != 0 && length > 0 && length <= 16;
    }

    FriendActions::Result Reject(
        const FriendActions::Command& command,
        FriendActions::Result result,
        GuardFailure failure)
    {
        ResetCommand();
        command_.command_sequence = command.sequence;
        command_.operation = command.operation;
        command_.target_id = command.target_id;
        CopyText(
            command_.target_name,
            sizeof(command_.target_name),
            command.target_name);
        return Fail(result, failure);
    }

    FriendActions::Result Fail(
        FriendActions::Result result,
        GuardFailure failure)
    {
        command_.result = result;
        room_.guard_failure = failure;
        active_ = false;
        return result;
    }

    FriendActions::Result Complete(FriendActions::Result result)
    {
        command_.result = result;
        room_.guard_failure = kGuardNone;
        active_ = false;
        return result;
    }

    bool CurrentRoomMatches(
        uint16_t channelId,
        uint32_t roomId,
        bool requireHost)
    {
        if (!channelId) {
            room_.guard_failure = kGuardChannelMissing;
            return false;
        }
        if (!roomId) {
            room_.guard_failure = kGuardRoomMissing;
            return false;
        }
        if (!room_.model_present) {
            room_.guard_failure = kGuardRoomModelMissing;
            return false;
        }
        if (room_.actual_room_id != roomId) {
            room_.guard_failure = kGuardRoomIdMismatch;
            return false;
        }
        if (room_.actual_channel_id != channelId) {
            room_.guard_failure = kGuardChannelMismatch;
            return false;
        }
        if (requireHost && !room_.local_is_host) {
            room_.guard_failure = kGuardHostMismatch;
            return false;
        }
        return true;
    }

    bool ReceiverMatches(
        const FriendActions::Command& command,
        uint16_t channelId,
        uint32_t roomId) const
    {
        if (receiverState_.channel_id != channelId ||
            receiverState_.room_id != roomId ||
            receiverState_.inviter_id == 0)
            return false;
        if (command.target_id &&
            command.target_id != receiverState_.inviter_id)
            return false;
        if (command.target_name[0] &&
            !SameText(
                command.target_name,
                receiverState_.inviter_name))
            return false;
        return true;
    }

    FriendActions::Result BeginSend(
        const FriendActions::Command& command)
    {
        const typename SenderT::Result result =
            sender_->Send(command.target_id, command.target_name);
        CopySender(sender_->State());
        return PublishSendResult(result);
    }

    FriendActions::Result PublishSendResult(
        typename SenderT::Result result)
    {
        if (result == SenderT::kRequestSent) {
            command_.request_sent = senderState_.request_sent;
            command_.request_opcode = senderState_.request_opcode;
            command_.request_transport_sequence =
                senderState_.transport_sequence_after;
            command_.reply_sequence =
                senderState_.transport_sequence_after;
            command_.reply_opcode = senderState_.request_opcode;
            command_.reply_result = senderState_.transport_result;
            command_.reply_consumed =
                senderState_.recipient_correlated;
            return Complete(FriendActions::kReplyObserved);
        }
        if (result == SenderT::kRequestQueued ||
            result == SenderT::kRequestPending) {
            command_.result = FriendActions::kWaitingForReply;
            active_ = true;
            return FriendActions::kWaitingForReply;
        }
        if (result == SenderT::kInvalidTarget ||
            result == SenderT::kSelectedTargetMismatch ||
            result == SenderT::kFriendNotAccepted)
            return Fail(
                FriendActions::kInvalidTarget,
                kGuardNativeActionRejected);
        return Fail(
            result == SenderT::kNativeFault
                ? FriendActions::kNativeFault
                : FriendActions::kNativeRequestNotSent,
            kGuardNativeActionRejected);
    }

    FriendActions::Result ArmReceiver(
        const FriendActions::Command& command,
        uint16_t channelId,
        uint32_t roomId)
    {
        typename ReceiverT::ExpectedInvite expected = {};
        expected.channel_id = channelId;
        expected.room_id = roomId;
        expected.inviter_id = command.target_id;
        expected.decision = static_cast<uint8_t>(
            command.operation == kArmAccept
                ? ReceiverT::kAccept
                : ReceiverT::kReject);
        expected.inviter_name = command.target_name;
        if (!receiver_->Arm(expected)) {
            CopyReceiver(receiver_->Refresh());
            return Fail(
                receiverState_.failure == ReceiverT::kInvalidExpectation
                    ? FriendActions::kInvalidTarget
                    : FriendActions::kNativeFault,
                kGuardReceiverFailed);
        }
        CopyReceiver(receiver_->Refresh());
        return Complete(FriendActions::kReady);
    }

    void TickCreate(const SdgoGameTestContextV1* context)
    {
        const FriendActions::Result result =
            createStep_(createOpaque_, context);
        RefreshObservations();
        if (result >= FriendActions::kBusy) {
            Fail(result, kGuardNativeActionRejected);
            return;
        }
        if (result != FriendActions::kReady &&
            result != FriendActions::kAlreadySatisfied &&
            result != FriendActions::kReplyObserved) {
            command_.result = result;
            return;
        }

        if (!room_.model_present) {
            Fail(
                FriendActions::kReplyRejected,
                kGuardRoomModelMissing);
            return;
        }
        if (!room_.actual_room_id ||
            (room_.expected_room_id &&
             room_.actual_room_id != room_.expected_room_id)) {
            Fail(
                FriendActions::kReplyRejected,
                kGuardRoomIdMismatch);
            return;
        }
        if (room_.expected_channel_id &&
            room_.actual_channel_id != room_.expected_channel_id) {
            Fail(
                FriendActions::kReplyRejected,
                kGuardChannelMismatch);
            return;
        }
        if (!room_.local_is_host) {
            Fail(
                FriendActions::kReplyRejected,
                kGuardHostMismatch);
            return;
        }
        if (room_.player_count < 1 ||
            room_.player_count > 12) {
            Fail(
                FriendActions::kReplyRejected,
                kGuardPlayerCountInvalid);
            return;
        }
        command_.reply_sequence = room_.create_reply_sequence;
        command_.reply_opcode = room_.create_reply_opcode;
        command_.reply_result = room_.create_result;
        command_.reply_consumed = 1;
        Complete(FriendActions::kReplyObserved);
    }

    void TickSend()
    {
        const typename SenderT::Result result = sender_->Poll();
        CopySender(sender_->State());
        PublishSendResult(result);
    }

    void TickDecision()
    {
        CopyReceiver(receiver_->Refresh());
        if (receiverState_.failure != ReceiverT::kNoFailure ||
            receiverState_.phase == ReceiverT::kFailed) {
            Fail(
                FriendActions::kNativeFault,
                kGuardReceiverFailed);
            return;
        }

        if (!decisionInvoked_) {
            if (receiverState_.phase !=
                    ReceiverT::kPromptInitialized ||
                !receiverState_.controls_ready) {
                command_.result = FriendActions::kWaitingForReply;
                room_.guard_failure = kGuardDecisionNotReady;
                return;
            }
            if (!receiver_->InvokeDecision()) {
                CopyReceiver(receiver_->Refresh());
                Fail(
                    FriendActions::kNativeFault,
                    kGuardReceiverFailed);
                return;
            }
            decisionInvoked_ = true;
            CopyReceiver(receiver_->Refresh());
        }

        command_.request_sent = receiverState_.decision_sent;
        command_.request_opcode =
            receiverState_.decision_opcode;
        command_.request_transport_sequence =
            receiverState_.decision_transport_sequence_after;
        if (receiverState_.decision ==
                ReceiverT::kReject &&
            receiverState_.phase == ReceiverT::kRejected) {
            command_.reply_sequence =
                receiverState_.
                    decision_transport_sequence_after;
            command_.reply_opcode =
                receiverState_.decision_opcode;
            command_.reply_result = 1;
            command_.reply_consumed = 1;
            Complete(FriendActions::kReplyObserved);
            return;
        }
        if (receiverState_.decision ==
                ReceiverT::kAccept &&
            receiverState_.phase == ReceiverT::kRoomJoined) {
            command_.reply_sequence =
                receiverState_.join_transport_sequence_after;
            command_.reply_opcode =
                receiverState_.join_opcode;
            command_.reply_result = 1;
            command_.reply_consumed = 1;
            Complete(FriendActions::kReplyObserved);
            return;
        }
        room_.guard_failure = kGuardNone;
        command_.result = FriendActions::kWaitingForReply;
    }

    void TickLeave()
    {
        RefreshObservations();
        if (room_.leave_reply_sequence <= leaveBaseline_) {
            command_.result = FriendActions::kWaitingForReply;
            return;
        }
        command_.reply_sequence = room_.leave_reply_sequence;
        command_.reply_opcode = room_.leave_reply_opcode;
        command_.reply_status = room_.leave_error;
        command_.reply_result = room_.leave_result;
        if (room_.leave_result != RoomT::kLeaveAccepted ||
            room_.leave_reply_opcode != kLeaveReplyOpcode ||
            room_.leave_error != 0) {
            Fail(
                FriendActions::kReplyRejected,
                kGuardLeaveRejected);
            return;
        }
        if (room_.native_app_state != 6 ||
            room_.model_present) {
            command_.result = FriendActions::kWaitingForReply;
            room_.guard_failure =
                kGuardLeaveTransitionPending;
            return;
        }
        command_.reply_consumed = 1;
        Complete(FriendActions::kReplyObserved);
    }

    void RefreshObservations()
    {
        if (native_) {
            const auto onboarding = native_->OnboardingState();
            command_.native_app_state = native_->AppState();
            command_.profile_pilot_id =
                onboarding.profile_pilot_id;
            room_.native_app_state =
                command_.native_app_state;
            room_.actual_channel_id =
                onboarding.channel_id;
        }
        if (rooms_) {
            const auto nativeRoom = rooms_->NativeRoomState();
            const auto create = rooms_->CreateState();
            const auto fresh = rooms_->FreshRoomState();
            const auto leave = rooms_->LeaveState();
            room_.native_app_state =
                nativeRoom.native_app_state;
            room_.actual_room_id = nativeRoom.room_id;
            room_.player_count = nativeRoom.player_count;
            room_.room_variant = nativeRoom.room_variant;
            room_.room_mode = nativeRoom.room_mode;
            room_.model_present = nativeRoom.model_present;
            room_.local_is_host = nativeRoom.local_is_host;
            room_.wait_menu_observed =
                nativeRoom.wait_menu_observed;
            room_.create_reply_sequence =
                create.reply_sequence;
            room_.create_result = create.result;
            room_.create_reply_room_id = create.room_id;
            room_.create_reply_opcode = create.reply_opcode;
            room_.fresh_sequence = fresh.sequence;
            room_.fresh_model_present = fresh.model_present;
            room_.fresh_factory_succeeded =
                fresh.original_factory_succeeded;
            room_.leave_reply_sequence =
                leave.reply_sequence;
            room_.leave_result = leave.result;
            room_.leave_reply_opcode = leave.reply_opcode;
            room_.leave_error = leave.error;
        }
        if (sender_)
            CopySender(sender_->State());
        if (receiver_)
            CopyReceiver(receiver_->Refresh());
    }

    void CopySender(const SenderObservation& source)
    {
        senderState_ = source;
    }

    void CopyReceiver(const ReceiverObservation& source)
    {
        receiverState_ = source;
    }

    NativeT* native_ = nullptr;
    RoomT* rooms_ = nullptr;
    SenderT* sender_ = nullptr;
    ReceiverT* receiver_ = nullptr;
    CreateStepFn createStep_ = nullptr;
    void* createOpaque_ = nullptr;
    CmdProjection command_ = {};
    RoomProjection room_ = {};
    SenderObservation senderState_ = {};
    ReceiverObservation receiverState_ = {};
    uint32_t leaveBaseline_ = 0;
    bool active_ = false;
    bool decisionInvoked_ = false;
};

using FriendRoomActions = FriendRoomActionsT<
    NativeActions,
    RoomActions,
    FriendInviteSender,
    FriendInviteReceiver>;

} // namespace friends
