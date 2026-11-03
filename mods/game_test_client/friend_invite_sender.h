#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "friend_service_lookup.h"
#include "native_actions.h"

namespace friends {

// Native GameTest-only driver for the original MessengerPopup InviteBtn paths.
// It never builds or sends a replacement packet. The shipped callback at
// vtable+0x38 validates the event and invokes the active popup's shipped
// invitation business method at vtable+0x70. In app state 18 that is
// C3dWaitRoomMessengerPopup::InviteFriend, which constructs and dispatches the
// original Service3DWaitRoomInviteFriend event. The normal 3D room manager then
// owns the copied friend record and its native state tick sends 0x077E.
class FriendInviteSender
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kRequestSent = 1,
        kRequestQueued = 2,
        kRequestPending = 3,

        kInvalidTarget = 0x100,
        kInviteContextUnavailable = 0x101,
        kMessengerUnavailable = 0x102,
        kSelectedConversationUnavailable = 0x103,
        kSelectedTargetMismatch = 0x104,
        kFriendNotAccepted = 0x105,
        kInviteControlUnavailable = 0x106,
        kInviteControlMismatch = 0x107,
        kInviteControlNotReady = 0x108,
        kNativeRequestNotSent = 0x109,
        kNativeFault = 0x10A,
        kThreeDQueueUnavailable = 0x10B,
        kNativeRequestNotQueued = 0x10C,
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
        uint8_t invite_control_hit_testable;
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

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        playerDataPointer_ =
            Symbol<uint8_t**>("friends.player_data_pointer");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>("flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        gameApp_ =
            Symbol<GameAppFn>("game.app");
        threeDWaitRoomVtable_ =
            Symbol<void*>("flow.three_d_wait_room_vtable");
        normalThreeDManagerVtable_ =
            Symbol<void*>("flow.normal_3d_manager_vtable");
        messengerVtable_ =
            Symbol<void*>("friends.messenger_vtable");
        threeDMessengerVtable_ =
            Symbol<void*>(
                "friends.three_d_messenger_vtable");
        messengerServiceVtable_ =
            Symbol<void*>("friends.messenger_service_vtable");
        threeDMessengerServiceVtable_ =
            Symbol<void*>(
                "friends.three_d_messenger_service_vtable");
        threeDOpenMessenger_ =
            Symbol<OpenMessengerFn>(
                "friends.open_three_d_messenger");
        uiLoadManagerPointer_ =
            Symbol<void**>(
                "friends.ui_load_manager_pointer");
        uiLoadInProgress_ =
            Symbol<UiLoadInProgressFn>(
                "friends.ui_load_in_progress");
        inviteUiCallback_ =
            Symbol<InviteUiCallbackFn>(
                "friends.invite_ui_callback");
        inviteBusiness_ =
            Symbol<InviteBusinessFn>(
                "friends.invite_sender_business");
        threeDInviteBusiness_ =
            Symbol<InviteBusinessFn>(
                "friends.three_d_invite_sender_business");
        normalThreeDInviteStateVtable_ =
            Symbol<void*>(
                "friends.normal_3d_invite_state_vtable");
        Reset();
        bound_ =
            api_ && api_->Symbol && mod_ && native_ &&
            playerDataPointer_ && serviceManagedPointer_ &&
            serviceFindByName_ && nativeWStringCtor_ &&
            nativeWStringDtor_ && nativeUiFindControl_ &&
            gameApp_ && threeDWaitRoomVtable_ &&
            normalThreeDManagerVtable_ &&
            messengerVtable_ && threeDMessengerVtable_ &&
            messengerServiceVtable_ &&
            threeDMessengerServiceVtable_ &&
            threeDOpenMessenger_ &&
            uiLoadManagerPointer_ &&
            uiLoadInProgress_ &&
            inviteUiCallback_ &&
            inviteBusiness_ && threeDInviteBusiness_ &&
            normalThreeDInviteStateVtable_;
        return bound_;
    }

    void Reset()
    {
        const bool preserveOpenRequest =
            resumeOpenRequestOnReset_;
        resumeOpenRequestOnReset_ = false;
        std::memset(&observation_, 0, sizeof(observation_));
        observation_.selected_conversation = -1;
        pending_ = false;
        awaitingMessenger_ = false;
        pendingTargetId_ = 0;
        std::memset(
            pendingTargetName_,
            0,
            sizeof(pendingTargetName_));
        if (!preserveOpenRequest)
            threeDOpenRequested_ = false;
        if (!preserveOpenRequest)
            threeDOpenResult_ = false;
        if (!preserveOpenRequest) {
            threeDOpenAttempts_ = 0;
            waitingForUiLoader_ = false;
            waitedForUiLoader_ = false;
        }
        pendingQueueState_ = nullptr;
        pendingQueueHead_ = nullptr;
        pendingQueueCount_ = 0;
        std::memset(
            pendingFriendRecord_,
            0,
            sizeof(pendingFriendRecord_));
    }

    Result Send(
        uint32_t targetId,
        const char* targetName)
    {
        if (pending_)
            return Publish(kRequestPending);
        Reset();
        if (!bound_ || !native_)
            return Publish(kUnbound);

        const size_t targetNameLength =
            BoundedLength(targetName, kMaximumNameLength);
        if (!targetId || targetNameLength == 0 ||
            targetNameLength > kMaximumNameLength)
            return Publish(kInvalidTarget);

        __try {
            observation_.native_app_state = native_->AppState();
            const bool threeDRoom =
                observation_.native_app_state ==
                kThreeDInvitationContextState;
            if (observation_.native_app_state !=
                    kLegacyInvitationContextState &&
                !threeDRoom)
                return Publish(kInviteContextUnavailable);
            observation_.three_d_room_path =
                threeDRoom ? 1 : 0;

            void* messenger =
                ResolveMessenger(threeDRoom);
            if (!messenger) {
                if (threeDRoom &&
                    observation_.
                        messenger_service_found &&
                    observation_.
                        messenger_service_class_matched &&
                    (waitingForUiLoader_ ||
                     (threeDOpenRequested_ &&
                      threeDOpenResult_))) {
                    return ArmMessengerWait(
                        targetId,
                        targetName,
                        targetNameLength);
                }
                return Publish(kMessengerUnavailable);
            }
            observation_.messenger_identity_matched = 1;

            uint8_t* selectedRecord = ResolveSelectedRecord();
            if (!selectedRecord)
                return Publish(kSelectedConversationUnavailable);

            const size_t selectedNameLength =
                BoundedLength(
                    reinterpret_cast<const char*>(
                        selectedRecord +
                        kFriendNameOffset),
                    kMaximumNameLength);
            if (selectedNameLength == 0 ||
                selectedNameLength > kMaximumNameLength)
                return Publish(kSelectedConversationUnavailable);

            observation_.recipient_id =
                *reinterpret_cast<const uint32_t*>(
                    selectedRecord);
            observation_.relationship =
                selectedRecord[kFriendRelationshipOffset];
            std::memcpy(
                observation_.recipient_name,
                selectedRecord + kFriendNameOffset,
                selectedNameLength);
            observation_.recipient_name[
                selectedNameLength] = '\0';

            if (observation_.recipient_id != targetId ||
                selectedNameLength != targetNameLength ||
                std::memcmp(
                    observation_.recipient_name,
                    targetName,
                    targetNameLength) != 0)
                return Publish(kSelectedTargetMismatch);
            observation_.selected_target_matched = 1;

            if (observation_.relationship !=
                    kAcceptedRelationship ||
                !AcceptedFriendListContains(
                    selectedRecord,
                    targetId,
                    targetName,
                    targetNameLength))
                return Publish(kFriendNotAccepted);
            observation_.accepted_friend_matched = 1;

            auto* messengerBytes =
                static_cast<uint8_t*>(messenger);
            void* chatRoot =
                *reinterpret_cast<void**>(
                    messengerBytes +
                    kMessengerChatRootOffset);
            if (!chatRoot)
                return Publish(kInviteControlUnavailable);

            void* inviteButton =
                FindControl(
                    chatRoot,
                    L"InviteBtn",
                    kButtonControlKind);
            if (!inviteButton)
                return Publish(kInviteControlUnavailable);
            observation_.invite_control_found = 1;

            const bool nameMatched =
                ControlHasName(
                    inviteButton,
                    L"InviteBtn");
            const bool parentMatched =
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(
                        inviteButton) +
                    kControlParentOffset) ==
                chatRoot;
            observation_.invite_control_name_matched =
                nameMatched ? 1 : 0;
            observation_.invite_control_parent_matched =
                parentMatched ? 1 : 0;
            if (!nameMatched || !parentMatched)
                return Publish(kInviteControlMismatch);

            auto** vtable =
                *reinterpret_cast<void***>(messenger);
            if (!vtable)
                return Publish(kMessengerUnavailable);
            observation_.callback_slot_matched =
                vtable[kUiCallbackVtableSlot] ==
                        reinterpret_cast<void*>(
                            inviteUiCallback_)
                    ? 1
                    : 0;
            observation_.business_slot_matched =
                vtable[kInviteBusinessVtableSlot] ==
                        reinterpret_cast<void*>(
                            threeDRoom
                                ? threeDInviteBusiness_
                                : inviteBusiness_)
                    ? 1
                    : 0;
            if (!observation_.callback_slot_matched ||
                !observation_.business_slot_matched)
                return Publish(kMessengerUnavailable);

            auto* chatRootBytes =
                static_cast<uint8_t*>(chatRoot);
            auto* inviteBytes =
                static_cast<uint8_t*>(
                    inviteButton);
            observation_.invite_control_visible =
                inviteBytes[
                    kControlVisibleOffset] != 0
                    ? 1
                    : 0;
            observation_.invite_control_enabled =
                inviteBytes[
                    kControlEnabledOffset] != 0
                    ? 1
                    : 0;
            observation_.invite_control_hit_testable =
                inviteBytes[
                    kControlHitTestableOffset] != 0
                    ? 1
                    : 0;
            if (!chatRootBytes[kControlVisibleOffset] ||
                !observation_.invite_control_visible ||
                !observation_.invite_control_enabled ||
                !observation_.
                    invite_control_hit_testable)
                return Publish(kInviteControlNotReady);

            const NativeActions::TransportObservation before =
                native_->TransportState();
            observation_.transport_sequence_before =
                before.send_sequence;

            ThreeDQueueContext queueBefore = {};
            if (threeDRoom) {
                if (!ResolveThreeDQueue(&queueBefore))
                    return Publish(kThreeDQueueUnavailable);
                observation_.three_d_queue_matched = 1;
                pendingQueueState_ = queueBefore.state;
                pendingQueueHead_ = queueBefore.head;
                pendingQueueCount_ = queueBefore.count;
                std::memcpy(
                    pendingFriendRecord_,
                    selectedRecord,
                    sizeof(pendingFriendRecord_));
            }

            // This is the exact shipped InviteBtn event path. The callback
            // checks name and parent again, then calls vtable+0x70. The
            // shipped 3D business method first enqueues a
            // Service3DWaitRoomInviteFriend event through popup+0xF0. Its
            // later native dispatch copies the selected record into state
            // 0x2B, whose worker sends 0x077E.
            inviteUiCallback_(
                messenger,
                kButtonClickEvent,
                inviteBytes + kControlNameOffset,
                inviteButton);

            if (threeDRoom) {
                ObserveThreeDQueue();
            }

            const NativeActions::TransportObservation after =
                native_->FindTransportSince(
                    before.send_sequence,
                    kInvitationRequestOpcode);
            RecordTransport(after);
            if (TransportMatches(before, after)) {
                observation_.request_sent = 1;
                observation_.recipient_correlated =
                    NativeRecipientLineageMatched()
                        ? 1
                        : 0;
                return Publish(kRequestSent);
            }

            if (threeDRoom) {
                pending_ = true;
                return Publish(
                    observation_.request_queued
                        ? kRequestQueued
                        : kRequestPending);
            }

            if (!observation_.request_sent)
                return Publish(kNativeRequestNotSent);

            return Publish(kNativeRequestNotSent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            pending_ = false;
            return Publish(kNativeFault);
        }
    }

    Result Poll()
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (!pending_)
            return static_cast<Result>(
                observation_.result);
        if (awaitingMessenger_) {
            const uint32_t targetId =
                pendingTargetId_;
            char targetName[
                kMaximumNameLength + 1] = {};
            std::memcpy(
                targetName,
                pendingTargetName_,
                sizeof(targetName));
            pending_ = false;
            awaitingMessenger_ = false;
            resumeOpenRequestOnReset_ = true;
            return Send(targetId, targetName);
        }

        __try {
            ObserveThreeDQueue();
            const NativeActions::TransportObservation after =
                native_->FindTransportSince(
                    observation_.
                        transport_sequence_before,
                    kInvitationRequestOpcode);
            if (!after.send_sequence)
                return Publish(kRequestPending);
            RecordTransport(after);

            pending_ = false;
            if (after.packet_size !=
                    kInvitationRequestSize ||
                after.result == 0)
                return Publish(kNativeRequestNotSent);

            observation_.request_sent = 1;
            observation_.recipient_correlated =
                NativeRecipientLineageMatched()
                    ? 1
                    : 0;
            return Publish(kRequestSent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            pending_ = false;
            return Publish(kNativeFault);
        }
    }

    Observation State() const
    {
        return observation_;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound:
            return "unbound";
        case kRequestSent:
            return "request-sent";
        case kRequestQueued:
            return "request-queued";
        case kRequestPending:
            return "request-pending";
        case kInvalidTarget:
            return "invalid-target";
        case kInviteContextUnavailable:
            return "invite-context-unavailable";
        case kMessengerUnavailable:
            return "messenger-unavailable";
        case kSelectedConversationUnavailable:
            return "selected-conversation-unavailable";
        case kSelectedTargetMismatch:
            return "selected-target-mismatch";
        case kFriendNotAccepted:
            return "friend-not-accepted";
        case kInviteControlUnavailable:
            return "invite-control-unavailable";
        case kInviteControlMismatch:
            return "invite-control-mismatch";
        case kInviteControlNotReady:
            return "invite-control-not-ready";
        case kNativeRequestNotSent:
            return "native-request-not-sent";
        case kNativeFault:
            return "native-fault";
        case kThreeDQueueUnavailable:
            return "three-d-queue-unavailable";
        case kNativeRequestNotQueued:
            return "native-request-not-queued";
        default:
            return "unknown";
        }
    }

private:
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kMaximumNativeEntries = 100;
    static constexpr size_t kMaximumNameLength = 16;
    static constexpr size_t kFriendNameOffset = 0x0A;
    static constexpr size_t kFriendRelationshipOffset = 0x1B;
    static constexpr size_t kPlayerUiPhaseOffset = 0x788;
    static constexpr size_t kSelectedConversationOffset = 0x790;
    static constexpr size_t kConversationListOffset = 0x7B0;
    static constexpr size_t kConversationCountOffset = 0x7B4;
    static constexpr size_t kMessengerChatRootOffset = 0x54;
    static constexpr size_t kMessengerOwnerPopupOffset = 0x48;
    static constexpr size_t kMessengerControlRootOffset = 0x50;
    static constexpr size_t kThreeDServiceEventQueueOffset = 0x60;
    static constexpr size_t kThreeDPopupEventQueueOffset = 0xF0;
    static constexpr size_t kControlNameOffset = 0x118;
    static constexpr size_t kControlParentOffset = 0x15C;
    static constexpr size_t kControlVisibleOffset = 0x162;
    static constexpr size_t kControlEnabledOffset = 0x163;
    static constexpr size_t kControlHitTestableOffset = 0x167;
    static constexpr size_t kNativeStringSizeOffset = 0x10;
    static constexpr size_t kNativeStringCapacityOffset = 0x14;
    static constexpr size_t kUiCallbackVtableSlot =
        0x38 / sizeof(void*);
    static constexpr size_t kInviteBusinessVtableSlot =
        0x70 / sizeof(void*);
    static constexpr uint32_t kButtonControlKind = 4;
    static constexpr uint32_t kButtonClickEvent = 0x101;
    static constexpr uint32_t kLegacyInvitationContextState = 7;
    static constexpr uint32_t kThreeDInvitationContextState = 18;
    static constexpr size_t kGameAppStateObjectOffset = 0x4A4;
    static constexpr size_t kWaitRoomManagerOffset = 0x04;
    static constexpr size_t kThreeDStateArrayOffset = 0x0C;
    static constexpr size_t kThreeDStateIdOffset = 0x04;
    static constexpr size_t kThreeDStateContextOffset = 0x0C;
    static constexpr size_t kThreeDStateManagerOffset = 0x10;
    static constexpr size_t kThreeDStateListHeadOffset = 0x18;
    static constexpr size_t kThreeDStateListCountOffset = 0x1C;
    static constexpr uint32_t kNormalThreeDInviteState = 0x2B;
    static constexpr size_t kThreeDFriendRecordSize = 0x38;
    static constexpr uint8_t kAcceptedRelationship = 7;
    static constexpr uint16_t kInvitationRequestOpcode = 0x077E;
    static constexpr uint32_t kInvitationRequestSize = 0x1E;
    static constexpr uint8_t kMaximumMessengerOpenAttempts = 2;

    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using GameAppFn = uint8_t*(__cdecl*)();
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(
            void*,
            const void*,
            uint32_t);
    using InviteUiCallbackFn =
        void(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
    using InviteBusinessFn =
        void(__thiscall*)(void*);
    using OpenMessengerFn =
        bool(__thiscall*)(void*);
    using UiLoadInProgressFn =
        bool(__thiscall*)(void*);

    template <typename T>
    T Symbol(const char* name) const
    {
        return api_ && api_->Symbol && mod_ && name
            ? reinterpret_cast<T>(
                  api_->Symbol(mod_, name))
            : nullptr;
    }

    static size_t BoundedLength(
        const char* value,
        size_t maximum)
    {
        if (!value)
            return maximum + 1;
        for (size_t index = 0;
             index <= maximum;
             ++index) {
            if (value[index] == '\0')
                return index;
        }
        return maximum + 1;
    }

    static uint32_t PointerValue(
        const void* value)
    {
        return static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(
                value));
    }

    static void* ReadObjectVtable(
        const void* object) noexcept
    {
        if (!object)
            return nullptr;
        __try {
            return *reinterpret_cast<
                void* const*>(object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    Result ArmMessengerWait(
        uint32_t targetId,
        const char* targetName,
        size_t targetNameLength)
    {
        pendingTargetId_ = targetId;
        std::memset(
            pendingTargetName_,
            0,
            sizeof(pendingTargetName_));
        std::memcpy(
            pendingTargetName_,
            targetName,
            targetNameLength);
        awaitingMessenger_ = true;
        pending_ = true;
        return Publish(kRequestPending);
    }

    void* ResolveNamedService(
        const wchar_t* name) const
    {
        if (!name ||
            !serviceManagedPointer_ ||
            !serviceFindByName_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;
        void* manager = *serviceManagedPointer_;
        if (!manager)
            return nullptr;

        alignas(4) uint8_t descriptor[
            kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(
            descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    descriptor + sizeof(uint32_t),
                    name) != nullptr;
            if (constructed) {
                service =
                    serviceFindByName_(
                        manager,
                        descriptor);
            }
        } __finally {
            if (constructed) {
                nativeWStringDtor_(
                    descriptor +
                    sizeof(uint32_t));
            }
        }
        return service;
    }

    bool ObserveUiLoadBusy()
    {
        void* manager =
            uiLoadManagerPointer_
                ? *uiLoadManagerPointer_
                : nullptr;
        observation_.ui_load_manager_present =
            manager ? 1 : 0;
        const bool busy =
            manager &&
            uiLoadInProgress_ &&
            uiLoadInProgress_(manager);
        observation_.ui_load_busy =
            busy ? 1 : 0;
        observation_.
            messenger_waited_for_ui_loader =
            waitedForUiLoader_ ? 1 : 0;
        observation_.
            messenger_waiting_for_ui_loader =
            waitingForUiLoader_ ? 1 : 0;
        observation_.messenger_open_attempts =
            threeDOpenAttempts_;
        return busy;
    }

    void* ResolveMessenger(
        bool threeDRoom)
    {
        const void* expectedServiceVtable =
            threeDRoom
                ? threeDMessengerServiceVtable_
                : messengerServiceVtable_;
        const void* expectedPopupVtable =
            threeDRoom
                ? threeDMessengerVtable_
                : messengerVtable_;
        observation_.
            expected_messenger_service_vtable =
            PointerValue(expectedServiceVtable);
        observation_.
            expected_messenger_popup_vtable =
            PointerValue(expectedPopupVtable);

        void* service =
            ResolveNamedService(
                L"Messenger");
        observation_.messenger_service_found =
            service ? 1 : 0;
        observation_.messenger_service_address =
            PointerValue(service);
        void* serviceVtable =
            service
                ? *reinterpret_cast<void**>(
                      service)
                : nullptr;
        observation_.messenger_service_vtable =
            PointerValue(serviceVtable);
        observation_.
            messenger_service_class_matched =
            serviceVtable == expectedServiceVtable
                ? 1
                : 0;

        void* popup =
            ResolveNamedService(
                L"MessengerPopup");
        observation_.messenger_popup_found =
            popup ? 1 : 0;
        observation_.messenger_popup_address =
            PointerValue(popup);
        void* popupVtable =
            popup
                ? *reinterpret_cast<void**>(
                      popup)
                : nullptr;
        observation_.messenger_popup_vtable =
            PointerValue(popupVtable);
        observation_.messenger_popup_class_matched =
            popupVtable == expectedPopupVtable
                ? 1
                : 0;
        observation_.messenger_open_requested =
            threeDOpenRequested_ ? 1 : 0;
        observation_.messenger_open_result =
            threeDOpenResult_ ? 1 : 0;
        const bool uiLoadBusy =
            ObserveUiLoadBusy();

        uint8_t* player =
            playerDataPointer_
                ? *playerDataPointer_
                : nullptr;
        observation_.player_data_found =
            player ? 1 : 0;
        if (player) {
            observation_.player_ui_phase =
                *reinterpret_cast<uint32_t*>(
                    player + kPlayerUiPhaseOffset);
            observation_.selected_conversation =
                *reinterpret_cast<int32_t*>(
                    player +
                    kSelectedConversationOffset);
            observation_.conversation_count =
                *reinterpret_cast<uint32_t*>(
                    player +
                    kConversationCountOffset);
        }

        void* manager =
            serviceManagedPointer_
                ? *serviceManagedPointer_
                : nullptr;
        observation_.messenger_manager_address =
            PointerValue(manager);
        if (manager) {
            const friend_service_lookup::UniqueService
                structuralPopup =
                    friend_service_lookup::
                        FindUniqueByVtable(
                            manager,
                            expectedPopupVtable);
            observation_.
                messenger_manager_list_valid =
                structuralPopup.list_valid ? 1 : 0;
            observation_.structural_popup_address =
                PointerValue(
                    structuralPopup.service);
            observation_.structural_popup_matches =
                structuralPopup.matches;

            const friend_service_lookup::UniqueService
                ordinaryPopup =
                    expectedPopupVtable ==
                            messengerVtable_
                        ? structuralPopup
                        : friend_service_lookup::
                              FindUniqueByVtable(
                                  manager,
                                  messengerVtable_);
            if (!ordinaryPopup.list_valid) {
                observation_.
                    messenger_manager_list_valid = 0;
            }
            observation_.ordinary_popup_address =
                PointerValue(ordinaryPopup.service);
            observation_.ordinary_popup_matches =
                ordinaryPopup.matches;
        }

        if (!service ||
            serviceVtable != expectedServiceVtable)
            return nullptr;

        void* ownerPopup =
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(service) +
                kMessengerOwnerPopupOffset);
        void* serviceRoot =
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(service) +
                kMessengerControlRootOffset);
        observation_.messenger_service_control_root =
            PointerValue(serviceRoot);
        observation_.messenger_owner_popup =
            PointerValue(ownerPopup);
        void* ownerPopupVtable =
            ReadObjectVtable(ownerPopup);
        observation_.messenger_owner_popup_vtable =
            PointerValue(ownerPopupVtable);
        observation_.owner_popup_class_matched =
            ownerPopupVtable == expectedPopupVtable
                ? 1
                : 0;
        void* serviceQueue = nullptr;
        if (threeDRoom) {
            serviceQueue =
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(service) +
                    kThreeDServiceEventQueueOffset);
            observation_.three_d_service_event_queue =
                PointerValue(serviceQueue);
        }

        if (popup &&
            popupVtable == expectedPopupVtable) {
            observation_.
                messenger_owner_popup_matched =
                ownerPopup == popup ? 1 : 0;
            if (threeDRoom) {
                void* popupQueue =
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(popup) +
                        kThreeDPopupEventQueueOffset);
                observation_.
                    three_d_service_event_queue =
                    PointerValue(serviceQueue);
                observation_.
                    three_d_popup_event_queue =
                    PointerValue(popupQueue);
                observation_.
                    three_d_event_queue_matched =
                    serviceQueue &&
                            serviceQueue == popupQueue
                        ? 1
                        : 0;
            }
            if (observation_.
                    messenger_owner_popup_matched &&
                (!threeDRoom ||
                 observation_.
                     three_d_event_queue_matched))
                return popup;
        }

        const bool mayAttemptOpen =
            !threeDOpenRequested_ ||
            (waitingForUiLoader_ &&
             !threeDOpenResult_ &&
             threeDOpenAttempts_ <
                 kMaximumMessengerOpenAttempts);
        if (threeDRoom &&
            threeDOpenMessenger_ &&
            mayAttemptOpen &&
            uiLoadBusy) {
            waitedForUiLoader_ = true;
            waitingForUiLoader_ = true;
            observation_.
                messenger_waited_for_ui_loader = 1;
            observation_.
                messenger_waiting_for_ui_loader = 1;
            return nullptr;
        }
        if (threeDRoom &&
            threeDOpenMessenger_ &&
            mayAttemptOpen &&
            !uiLoadBusy) {
            waitingForUiLoader_ = false;
            observation_.
                messenger_waiting_for_ui_loader = 0;
            threeDOpenRequested_ = true;
            ++threeDOpenAttempts_;
            observation_.messenger_open_requested = 1;
            observation_.messenger_open_attempts =
                threeDOpenAttempts_;
            threeDOpenResult_ =
                threeDOpenMessenger_(service);
            observation_.messenger_open_result =
                threeDOpenResult_ ? 1 : 0;
            if (!threeDOpenResult_) {
                const bool becameBusy =
                    ObserveUiLoadBusy();
                if (becameBusy &&
                    threeDOpenAttempts_ <
                        kMaximumMessengerOpenAttempts) {
                    waitedForUiLoader_ = true;
                    waitingForUiLoader_ = true;
                    observation_.
                        messenger_waited_for_ui_loader =
                        1;
                    observation_.
                        messenger_waiting_for_ui_loader =
                        1;
                } else {
                    waitingForUiLoader_ = false;
                    observation_.
                        messenger_waiting_for_ui_loader =
                        0;
                }
            }
        }
        return nullptr;
    }

    struct ThreeDQueueContext
    {
        uint8_t* manager;
        uint8_t* state;
        uint8_t* head;
        uint32_t count;
    };

    bool ResolveThreeDQueue(
        ThreeDQueueContext* output) const
    {
        if (!output || !gameApp_ ||
            !threeDWaitRoomVtable_ ||
            !normalThreeDManagerVtable_ ||
            !normalThreeDInviteStateVtable_)
            return false;
        std::memset(output, 0, sizeof(*output));

        uint8_t* app = gameApp_();
        uint8_t* owner =
            app
                ? *reinterpret_cast<uint8_t**>(
                      app +
                      kGameAppStateObjectOffset)
                : nullptr;
        if (!owner ||
            *reinterpret_cast<void**>(owner) !=
                threeDWaitRoomVtable_)
            return false;

        uint8_t* manager =
            *reinterpret_cast<uint8_t**>(
                owner +
                kWaitRoomManagerOffset);
        if (!manager ||
            *reinterpret_cast<void**>(manager) !=
                normalThreeDManagerVtable_)
            return false;

        auto** states =
            *reinterpret_cast<uint8_t***>(
                manager +
                kThreeDStateArrayOffset);
        uint8_t* state =
            states
                ? states[
                      kNormalThreeDInviteState]
                : nullptr;
        if (!state ||
            *reinterpret_cast<void**>(state) !=
                normalThreeDInviteStateVtable_ ||
            *reinterpret_cast<const uint32_t*>(
                state +
                kThreeDStateIdOffset) !=
                kNormalThreeDInviteState ||
            *reinterpret_cast<uint8_t**>(
                state +
                kThreeDStateContextOffset) !=
                manager + 0x24 ||
            *reinterpret_cast<uint8_t**>(
                state +
                kThreeDStateManagerOffset) !=
                manager)
            return false;

        uint8_t* head =
            *reinterpret_cast<uint8_t**>(
                state +
                kThreeDStateListHeadOffset);
        const uint32_t count =
            *reinterpret_cast<const uint32_t*>(
                state +
                kThreeDStateListCountOffset);
        if (!head || count >
                kMaximumNativeEntries ||
            (count == 0 &&
             (*reinterpret_cast<uint8_t**>(head) !=
                  head ||
              *reinterpret_cast<uint8_t**>(
                  head + 4) != head)))
            return false;

        output->manager = manager;
        output->state = state;
        output->head = head;
        output->count = count;
        return true;
    }

    static bool QueueTailMatches(
        const ThreeDQueueContext& queue,
        const uint8_t* selectedRecord)
    {
        if (!queue.head || !queue.count ||
            !selectedRecord)
            return false;
        uint8_t* tail =
            *reinterpret_cast<uint8_t**>(
                queue.head + 4);
        if (!tail || tail == queue.head ||
            *reinterpret_cast<uint8_t**>(tail) !=
                queue.head)
            return false;
        uint8_t* copiedRecord =
            *reinterpret_cast<uint8_t**>(
                tail + 8);
        return copiedRecord &&
            copiedRecord != selectedRecord &&
            std::memcmp(
                copiedRecord,
                selectedRecord,
                kThreeDFriendRecordSize) == 0;
    }

    void ObserveThreeDQueue()
    {
        if (observation_.request_queued ||
            !pendingQueueState_ ||
            !pendingQueueHead_)
            return;
        ThreeDQueueContext current = {};
        if (!ResolveThreeDQueue(&current) ||
            current.state != pendingQueueState_ ||
            current.head != pendingQueueHead_ ||
            current.count <= pendingQueueCount_ ||
            !QueueTailMatches(
                current,
                pendingFriendRecord_))
            return;
        observation_.request_queued = 1;
        observation_.queued_record_matched = 1;
    }

    bool NativeRecipientLineageMatched() const
    {
        return observation_.messenger_identity_matched &&
            observation_.selected_target_matched &&
            observation_.accepted_friend_matched &&
            observation_.callback_slot_matched &&
            observation_.business_slot_matched;
    }

    uint8_t* ResolveSelectedRecord()
    {
        uint8_t* data =
            playerDataPointer_
                ? *playerDataPointer_
                : nullptr;
        if (!data)
            return nullptr;
        const int32_t selected =
            *reinterpret_cast<const int32_t*>(
                data +
                kSelectedConversationOffset);
        const uint32_t count =
            *reinterpret_cast<const uint32_t*>(
                data +
                kConversationCountOffset);
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(
                data +
                kConversationListOffset);
        observation_.selected_conversation =
            selected;
        observation_.conversation_count =
            count;
        if (selected < 0 ||
            static_cast<uint32_t>(
                selected) >= count ||
            count > kMaximumNativeEntries ||
            !head)
            return nullptr;
        uint8_t* node =
            *reinterpret_cast<uint8_t**>(head);
        uint8_t* previous = head;
        for (int32_t index = 0;
             index <= selected;
             ++index) {
            if (!node || node == head ||
                *reinterpret_cast<uint8_t**>(
                    node + 4) != previous)
                return nullptr;
            if (index == selected)
                break;
            previous = node;
            node =
                *reinterpret_cast<uint8_t**>(
                    node);
        }
        uint8_t* payload =
            *reinterpret_cast<uint8_t**>(
                node + 8);
        return payload
            ? *reinterpret_cast<uint8_t**>(
                  payload)
            : nullptr;
    }

    bool AcceptedFriendListContains(
        const uint8_t* selectedRecord,
        uint32_t targetId,
        const char* targetName,
        size_t targetNameLength) const
    {
        uint8_t* data =
            playerDataPointer_
                ? *playerDataPointer_
                : nullptr;
        if (!data || !selectedRecord)
            return false;
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(
                data + 0x7A4);
        const uint32_t count =
            *reinterpret_cast<const uint32_t*>(
                data + 0x7A8);
        if (!head || count >
                kMaximumNativeEntries)
            return false;
        uint8_t* node =
            *reinterpret_cast<uint8_t**>(head);
        uint8_t* previous = head;
        for (uint32_t index = 0;
             index < count;
             ++index) {
            if (!node || node == head ||
                *reinterpret_cast<uint8_t**>(
                    node + 4) != previous)
                return false;
            uint8_t* record =
                *reinterpret_cast<uint8_t**>(
                    node + 8);
            if (record == selectedRecord) {
                const char* name =
                    reinterpret_cast<const char*>(
                        record +
                        kFriendNameOffset);
                const size_t nameLength =
                    BoundedLength(
                        name,
                        kMaximumNameLength);
                return nameLength ==
                        targetNameLength &&
                    *reinterpret_cast<
                        const uint32_t*>(
                            record) == targetId &&
                    record[
                        kFriendRelationshipOffset] ==
                        kAcceptedRelationship &&
                    std::memcmp(
                        name,
                        targetName,
                        targetNameLength) == 0;
            }
            previous = node;
            node =
                *reinterpret_cast<uint8_t**>(
                    node);
        }
        return false;
    }

    void* FindControl(
        void* root,
        const wchar_t* name,
        uint32_t kind) const
    {
        if (!root || !name ||
            !nativeUiFindControl_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;
        alignas(4) uint8_t nativeName[
            kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (constructed) {
                control =
                    nativeUiFindControl_(
                        root,
                        nativeName,
                        kind);
            }
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    nativeName);
        }
        return control;
    }

    static bool ControlHasName(
        const void* control,
        const wchar_t* expected)
    {
        if (!control || !expected)
            return false;
        const auto* nativeName =
            static_cast<const uint8_t*>(
                control) +
            kControlNameOffset;
        const uint32_t size =
            *reinterpret_cast<const uint32_t*>(
                nativeName +
                kNativeStringSizeOffset);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                nativeName +
                kNativeStringCapacityOffset);
        const size_t expectedSize =
            std::wcslen(expected);
        if (size != expectedSize || size > 63)
            return false;
        const wchar_t* value =
            capacity < 8
                ? reinterpret_cast<
                      const wchar_t*>(
                      nativeName)
                : *reinterpret_cast<
                      wchar_t* const*>(
                      nativeName);
        return value &&
            std::wmemcmp(
                value,
                expected,
                size) == 0;
    }

    static bool TransportMatches(
        const NativeActions::TransportObservation& before,
        const NativeActions::TransportObservation& after)
    {
        return after.send_sequence !=
                before.send_sequence &&
            after.opcode ==
                kInvitationRequestOpcode &&
            after.packet_size ==
                kInvitationRequestSize &&
            after.result != 0;
    }

    void RecordTransport(
        const NativeActions::TransportObservation& transport)
    {
        observation_.transport_sequence_after =
            transport.send_sequence;
        observation_.request_opcode =
            transport.opcode;
        observation_.request_size =
            transport.packet_size;
        observation_.transport_result =
            transport.result;
    }

    Result Publish(Result result)
    {
        observation_.result =
            static_cast<uint32_t>(result);
        return result;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    uint8_t** playerDataPointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    GameAppFn gameApp_ = nullptr;
    void* threeDWaitRoomVtable_ = nullptr;
    void* normalThreeDManagerVtable_ = nullptr;
    void* messengerVtable_ = nullptr;
    void* threeDMessengerVtable_ = nullptr;
    void* messengerServiceVtable_ = nullptr;
    void* threeDMessengerServiceVtable_ = nullptr;
    OpenMessengerFn threeDOpenMessenger_ = nullptr;
    void** uiLoadManagerPointer_ = nullptr;
    UiLoadInProgressFn uiLoadInProgress_ = nullptr;
    InviteUiCallbackFn inviteUiCallback_ = nullptr;
    InviteBusinessFn inviteBusiness_ = nullptr;
    InviteBusinessFn threeDInviteBusiness_ = nullptr;
    void* normalThreeDInviteStateVtable_ = nullptr;
    bool bound_ = false;
    bool pending_ = false;
    bool awaitingMessenger_ = false;
    bool threeDOpenRequested_ = false;
    bool threeDOpenResult_ = false;
    uint8_t threeDOpenAttempts_ = 0;
    bool waitingForUiLoader_ = false;
    bool waitedForUiLoader_ = false;
    bool resumeOpenRequestOnReset_ = false;
    uint32_t pendingTargetId_ = 0;
    char pendingTargetName_[
        kMaximumNameLength + 1] = {};
    uint8_t* pendingQueueState_ = nullptr;
    uint8_t* pendingQueueHead_ = nullptr;
    uint32_t pendingQueueCount_ = 0;
    uint8_t pendingFriendRecord_[
        kThreeDFriendRecordSize] = {};
    Observation observation_ = {};
};

} // namespace friends
