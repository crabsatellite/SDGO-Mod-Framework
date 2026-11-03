#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

namespace friends {

// GameTest-only observer/driver for the shipped incoming friend-room invite
// path. It accepts only a real 0x059A packet already dispatched by the client,
// follows the original prompt clone/initializer lifetime, and invokes the
// original AllowBtn or RefuseBtn callback with the real native control.
//
// It never constructs an invite packet, writes invitation/profile state, or
// fabricates a room. Accept completion requires the actual acceptance send,
// its native invitation identity, and the final guest-room model. Builds that
// take the 0x05AB/0x0734 continuation retain that separately observed lineage.
class FriendInviteReceiver
{
public:
    enum Decision : uint32_t
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
        kPromptWaitingVisible = 11,
        kFailed = 0x100,
    };

    enum Failure : uint32_t
    {
        kNoFailure = 0,
        kNotBound = 1,
        kInvalidExpectation = 2,
        kBusy = 3,
        kIncomingPacketMismatch = 4,
        kPromptNotCreated = 5,
        kPromptIdentityMismatch = 6,
        kPromptCloneMismatch = 7,
        kPromptUiMismatch = 8,
        kPromptNoLongerLive = 9,
        kDecisionTransportMismatch = 10,
        kInvitationContextMismatch = 11,
        kJoinTransportMismatch = 12,
        kNativeFault = 13,
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

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;

        gameApp_ = Symbol<GameAppFn>("game.app");
        profilePointer_ =
            Symbol<uint8_t**>("flow.profile_pointer");
        isRoomMaster_ =
            Symbol<IsRoomMasterFn>("flow.is_room_master");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        genericDispatch_ =
            Symbol<PacketDispatchFn>(
                "friends.invite_receiver_dispatch");
        promptCtor_ =
            Symbol<PromptCtorFn>(
                "friends.invite_receiver_prompt_ctor");
        promptClone_ =
            Symbol<PromptCloneFn>(
                "friends.invite_receiver_prompt_clone");
        promptInitialize_ =
            Symbol<PromptInitializeFn>(
                "friends.invite_receiver_prompt_initialize");
        promptDtor_ =
            Symbol<PromptDtorFn>(
                "friends.invite_receiver_prompt_dtor");
        promptVtable_ =
            Symbol<void*>(
                "friends.invite_receiver_prompt_vtable");
        promptCallback_ =
            Symbol<PromptCallbackFn>(
                "friends.invite_receiver_ui_callback");
        threeDWaitRoomVtable_ =
            Symbol<void*>(
                "flow.three_d_wait_room_vtable");
        managerVtables_[0] =
            Symbol<void*>(
                "flow.normal_3d_manager_vtable");
        managerVtables_[1] =
            Symbol<void*>(
                "flow.ai_3d_manager_vtable");
        managerVtables_[2] =
            Symbol<void*>(
                "flow.battleship_3d_manager_vtable");
        promptCloneEntry_ =
            reinterpret_cast<void*>(promptClone_);
        promptInitializeEntry_ =
            reinterpret_cast<void*>(promptInitialize_);

        ResetObservation();
        if (!api_ || !api_->HookCode || !mod_ || !native_ ||
            !gameApp_ || !profilePointer_ || !isRoomMaster_ ||
            !nativeUiFindControl_ || !genericDispatch_ ||
            !promptCtor_ || !promptClone_ || !promptInitialize_ ||
            !promptDtor_ || !promptVtable_ || !promptCallback_ ||
            !threeDWaitRoomVtable_ || !managerVtables_[0] ||
            !managerVtables_[1] || !managerVtables_[2] ||
            (active_ && active_ != this))
            return false;

        __try {
            void** vtable =
                static_cast<void**>(promptVtable_);
            if (vtable[kPromptCallbackSlot] !=
                    reinterpret_cast<void*>(promptCallback_) ||
                vtable[kPromptInitializeSlot] !=
                    reinterpret_cast<void*>(promptInitialize_) ||
                vtable[kPromptCloneSlot] !=
                    reinterpret_cast<void*>(promptClone_) ||
                !vtable[kPromptLayoutSlot])
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        active_ = this;
        if (!api_->HookCode(
                mod_,
                "friends.invite_receiver_dispatch",
                reinterpret_cast<void*>(&ObserveDispatch),
                reinterpret_cast<void**>(&genericDispatch_)) ||
            !api_->HookCode(
                mod_,
                "friends.invite_receiver_prompt_ctor",
                reinterpret_cast<void*>(&ObservePromptCtor),
                reinterpret_cast<void**>(&promptCtor_)) ||
            !api_->HookCode(
                mod_,
                "friends.invite_receiver_prompt_clone",
                reinterpret_cast<void*>(&ObservePromptClone),
                reinterpret_cast<void**>(&promptClone_)) ||
            !api_->HookCode(
                mod_,
                "friends.invite_receiver_prompt_initialize",
                reinterpret_cast<void*>(&ObservePromptInitialize),
                reinterpret_cast<void**>(&promptInitialize_)) ||
            !api_->HookCode(
                mod_,
                "friends.invite_receiver_prompt_dtor",
                reinterpret_cast<void*>(&ObservePromptDtor),
                reinterpret_cast<void**>(&promptDtor_)) ||
            !api_->HookCode(
                mod_,
                "flow.native_ui_find_control",
                reinterpret_cast<void*>(&ObserveUiFindControl),
                reinterpret_cast<void**>(&nativeUiFindControl_))) {
            active_ = nullptr;
            return false;
        }
        bound_ = true;
        return true;
    }

    bool Arm(const ExpectedInvite& expected)
    {
        if (!bound_ || !native_)
            return Fail(kNotBound);
        if (managedPrompt_ ||
            (observation_.phase != kIdle &&
             observation_.phase != kRoomJoined &&
             observation_.phase != kRejected &&
             observation_.phase != kFailed)) {
            observation_.failure = kBusy;
            return false;
        }

        const size_t nameLength =
            BoundedLength(
                expected.inviter_name,
                kMaximumInviterNameLength);
        if (!expected.room_id || !expected.inviter_id ||
            nameLength == 0 ||
            nameLength > kMaximumInviterNameLength ||
            (expected.decision != kAccept &&
             expected.decision != kReject) ||
            !IsAsciiName(expected.inviter_name, nameLength))
            return Fail(kInvalidExpectation);

        ResetObservation();
        expectedChannelId_ = expected.channel_id;
        expectedRoomId_ = expected.room_id;
        expectedInviterId_ = expected.inviter_id;
        expectedDecision_ =
            static_cast<Decision>(expected.decision);
        expectedNameLength_ = nameLength;
        std::memcpy(
            expectedName_,
            expected.inviter_name,
            nameLength);
        expectedName_[nameLength] = '\0';
        observation_.phase = kArmed;
        observation_.channel_id = expectedChannelId_;
        observation_.room_id = expectedRoomId_;
        observation_.inviter_id = expectedInviterId_;
        observation_.decision =
            static_cast<uint8_t>(expectedDecision_);
        std::memcpy(
            observation_.inviter_name,
            expectedName_,
            sizeof(observation_.inviter_name));
        armed_ = true;
        return true;
    }

    bool InvokeDecision()
    {
        if (!armed_ || !bound_ || !native_ ||
            observation_.phase != kPromptInitialized ||
            !managedPrompt_)
            return Fail(kPromptNoLongerLive);

        void* prompt = managedPrompt_;
        void* control =
            expectedDecision_ == kAccept
                ? allowControl_
                : refuseControl_;
        if (!ValidateLivePromptAndControls(prompt, control))
            return Fail(kPromptUiMismatch);

        const NativeActions::TransportObservation before =
            native_->TransportState();
        observation_.decision_invoked = 1;
        __try {
            promptCallback_(
                prompt,
                kButtonActivateEvent,
                static_cast<uint8_t*>(control) +
                    kControlNameOffset,
                control);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Fail(kNativeFault);
        }
        const NativeActions::TransportObservation after =
            native_->TransportState();
        const uint16_t expectedOpcode =
            expectedDecision_ == kAccept
                ? kAcceptRequestOpcode
                : kRejectRequestOpcode;
        observation_.decision_transport_sequence_before =
            before.send_sequence;
        observation_.decision_transport_sequence_after =
            after.send_sequence;
        observation_.decision_opcode = after.opcode;
        observation_.decision_packet_size =
            after.packet_size;
        observation_.decision_transport_result =
            after.result;
        if (after.send_sequence == before.send_sequence ||
            after.opcode != expectedOpcode ||
            after.packet_size != kDecisionRequestSize ||
            after.result == 0)
            return Fail(kDecisionTransportMismatch);

        observation_.decision_sent = 1;
        if (expectedDecision_ == kAccept) {
            if (!InvitationContextMatches())
                return Fail(kInvitationContextMismatch);
            observation_.invitation_context_matched = 1;
            observation_.phase = kDecisionSent;
        } else {
            observation_.phase =
                observation_.prompt_destroyed
                    ? kRejected
                    : kDecisionSent;
        }
        return true;
    }

    Observation Refresh()
    {
        observation_.native_app_state =
            native_ ? native_->AppState()
                    : UINT32_MAX;
        if(observation_.phase==kPromptWaitingVisible) {
            if(observation_.prompt_destroyed)Fail(kPromptNoLongerLive);
            else if(ValidateLivePromptAndControls(managedPrompt_,allowControl_)) {
                observation_.controls_ready=1;observation_.phase=kPromptInitialized;
            }
        }
        if (observation_.phase == kJoinRequestSent ||
            (observation_.phase==kDecisionSent && observation_.decision_sent && observation_.invitation_context_matched))
            RefreshRoomCompletion();
        if (expectedDecision_ == kReject &&
            observation_.decision_sent &&
            observation_.prompt_destroyed &&
            observation_.phase != kFailed)
            observation_.phase = kRejected;
        return observation_;
    }

    Observation State()
    {
        return Refresh();
    }

    void Disarm()
    {
        armed_ = false;
    }

private:
    static constexpr uint16_t kIncomingInviteOpcode = 0x059A;
    static constexpr uint16_t kJoinContinuationOpcode = 0x05AB;
    static constexpr uint16_t kAcceptRequestOpcode = 0x04A8;
    static constexpr uint16_t kRejectRequestOpcode = 0x07A9;
    static constexpr uint16_t kJoinRequestOpcode = 0x0734;
    static constexpr uint32_t kIncomingInvitePacketSize = 0x38;
    static constexpr uint32_t kDecisionRequestSize = 0x11;
    static constexpr uint32_t kJoinRequestSize = 0x32;
    static constexpr uint32_t kThreeDWaitRoomAppState = 18;
    static constexpr uint32_t kButtonActivateEvent = 0x101;
    static constexpr uint32_t kButtonControlKind = 4;
    static constexpr size_t kMaximumInviterNameLength = 16;
    static constexpr size_t kMaximumPasswordLength = 20;
    static constexpr size_t kPacketChannelOffset = 0x06;
    static constexpr size_t kPacketRoomOffset = 0x08;
    static constexpr size_t kPacketInviterOffset = 0x0C;
    static constexpr size_t kPacketNativeFlagOffset = 0x10;
    static constexpr size_t kPacketNameLengthOffset = 0x11;
    static constexpr size_t kPacketNameOffset = 0x12;
    static constexpr size_t kPacketPasswordLengthOffset = 0x23;
    static constexpr size_t kPromptNameOffset = 0x4C;
    static constexpr size_t kPromptFriendKindOffset = 0x68;
    static constexpr size_t kPromptPilotKindOffset = 0x69;
    static constexpr size_t kPromptNativeFlagOffset = 0x6B;
    static constexpr size_t kPromptContextOffset = 0x6C;
    static constexpr size_t kPromptChannelOffset = 0x6E;
    static constexpr size_t kPromptRoomOffset = 0x70;
    static constexpr size_t kPromptInviterOffset = 0x78;
    static constexpr size_t kPromptReadyOffset = 0x44;
    static constexpr size_t kPromptCallbackSlot = 0x38 / sizeof(void*);
    static constexpr size_t kPromptInitializeSlot = 0x3C / sizeof(void*);
    static constexpr size_t kPromptCloneSlot = 0x54 / sizeof(void*);
    static constexpr size_t kPromptLayoutSlot = 0x58 / sizeof(void*);
    static constexpr size_t kControlNameOffset = 0x118;
    static constexpr size_t kControlParentOffset = 0x15C;
    static constexpr size_t kControlVisibleOffset = 0x162;
    static constexpr size_t kControlEnabledOffset = 0x163;
    static constexpr size_t kControlHitOffset = 0x167;
    static constexpr size_t kNativeStringLengthOffset = 0x10;
    static constexpr size_t kNativeStringCapacityOffset = 0x14;
    static constexpr size_t kProfileInvitationState = 0x860;
    static constexpr size_t kProfileInvitationContext = 0x864;
    static constexpr size_t kProfileRoom = 0xEC;
    static constexpr size_t kGameAppState = 0x4A0;
    static constexpr size_t kGameAppStateOwner = 0x4A4;

    using GameAppFn = uint8_t*(__cdecl*)();
    using IsRoomMasterFn = bool(__stdcall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(
            void*,
            const void*,
            uint32_t);
    using PacketDispatchFn =
        int(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);
    using PromptCtorFn =
        void*(__thiscall*)(void*, const void*);
    using PromptCloneFn = void*(__thiscall*)(void*);
    using PromptInitializeFn =
        void(__thiscall*)(void*, int);
    using PromptDtorFn = void(__thiscall*)(void*);
    using PromptCallbackFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    template <typename T>
    static T Read(const void* address)
    {
        T value = {};
        std::memcpy(&value, address, sizeof(value));
        return value;
    }

    static size_t BoundedLength(
        const char* value,
        size_t maximum)
    {
        if (!value)
            return maximum + 1;
        for (size_t i = 0; i <= maximum; ++i) {
            if (value[i] == '\0')
                return i;
        }
        return maximum + 1;
    }

    static bool IsAsciiName(
        const char* value,
        size_t length)
    {
        if (!value)
            return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char c =
                static_cast<unsigned char>(value[i]);
            if (c < 0x21 || c > 0x7E)
                return false;
        }
        return true;
    }

    static const wchar_t* NativeStringData(
        const void* storage)
    {
        if (!storage)
            return nullptr;
        const auto* bytes =
            static_cast<const uint8_t*>(storage);
        const uint32_t capacity =
            Read<uint32_t>(
                bytes +
                kNativeStringCapacityOffset);
        return capacity < 8
            ? reinterpret_cast<const wchar_t*>(
                  bytes)
            : Read<const wchar_t*>(bytes);
    }

    static bool NativeStringMatches(
        const void* storage,
        const wchar_t* expected,
        size_t expectedLength)
    {
        if (!storage || !expected)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(storage);
        const uint32_t length =
            Read<uint32_t>(
                bytes +
                kNativeStringLengthOffset);
        const uint32_t capacity =
            Read<uint32_t>(
                bytes +
                kNativeStringCapacityOffset);
        if (length != expectedLength ||
            length > capacity)
            return false;
        const wchar_t* data = NativeStringData(storage);
        return data &&
            std::memcmp(
                data,
                expected,
                expectedLength *
                    sizeof(wchar_t)) == 0;
    }

    bool PacketMatchesExpected(
        const void* packet) const
    {
        if (!packet)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(packet);
        if (Read<uint16_t>(bytes) !=
                kIncomingInviteOpcode ||
            Read<uint16_t>(
                bytes +
                kPacketChannelOffset) !=
                expectedChannelId_ ||
            Read<uint32_t>(
                bytes +
                kPacketRoomOffset) !=
                expectedRoomId_ ||
            Read<uint32_t>(
                bytes +
                kPacketInviterOffset) !=
                expectedInviterId_ ||
            bytes[kPacketNameLengthOffset] !=
                expectedNameLength_ ||
            bytes[kPacketPasswordLengthOffset] >
                kMaximumPasswordLength)
            return false;
        const size_t packetNameLength =
            BoundedLength(
                reinterpret_cast<const char*>(
                    bytes +
                    kPacketNameOffset),
                kMaximumInviterNameLength);
        return packetNameLength ==
                   expectedNameLength_ &&
            std::memcmp(
                bytes + kPacketNameOffset,
                expectedName_,
                expectedNameLength_) == 0;
    }

    bool PromptMatchesExpected(
        const void* prompt,
        bool captureName)
    {
        if (!prompt)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(prompt);
        if (Read<void*>(bytes) != promptVtable_ ||
            bytes[kPromptFriendKindOffset] != 1 ||
            bytes[kPromptPilotKindOffset] != 0 ||
            bytes[kPromptNativeFlagOffset] !=
                observedNativeFlag_ ||
            Read<uint16_t>(
                bytes +
                kPromptChannelOffset) !=
                expectedChannelId_ ||
            Read<uint32_t>(
                bytes +
                kPromptRoomOffset) !=
                expectedRoomId_ ||
            Read<uint32_t>(
                bytes +
                kPromptInviterOffset) !=
                expectedInviterId_)
            return false;

        wchar_t expectedWide[17] = {};
        for (size_t i = 0;
             i < expectedNameLength_;
             ++i) {
            expectedWide[i] =
                static_cast<unsigned char>(
                    expectedName_[i]);
        }
        if (!NativeStringMatches(
                bytes + kPromptNameOffset,
                expectedWide,
                expectedNameLength_))
            return false;
        if (captureName) {
            std::memcpy(
                observation_.prompt_name,
                expectedWide,
                (expectedNameLength_ + 1) *
                    sizeof(wchar_t));
        }
        return true;
    }

    bool ValidateControl(
        void* root,
        void* control,
        const wchar_t* name,
        size_t nameLength) const
    {
        if (!root || !control)
            return false;
        const auto* rootBytes =
            static_cast<const uint8_t*>(root);
        const auto* controlBytes =
            static_cast<const uint8_t*>(control);
        return rootBytes[kControlVisibleOffset] != 0 &&
            controlBytes[kControlVisibleOffset] != 0 &&
            controlBytes[kControlEnabledOffset] != 0 &&
            controlBytes[kControlHitOffset] != 0 &&
            Read<void*>(
                controlBytes +
                kControlParentOffset) == root &&
            NativeStringMatches(
                controlBytes +
                    kControlNameOffset,
                name,
                nameLength);
    }

    bool ValidateControlIdentity(void* root,void* control,const wchar_t* name,size_t length) const {
        if(!root || !control)return false;
        const auto* bytes=static_cast<const uint8_t*>(control);
        return Read<void*>(bytes+kControlParentOffset)==root && NativeStringMatches(bytes+kControlNameOffset,name,length);
    }

    bool ValidateLivePromptAndControls(
        void* prompt,
        void* selectedControl) const
    {
        __try {
            if (!prompt || prompt != managedPrompt_ ||
                observation_.prompt_destroyed ||
                !PromptVtableMatches(prompt) ||
                !PromptIdentityMatches(prompt) ||
                Read<uint32_t>(
                    static_cast<uint8_t*>(prompt) +
                    kPromptReadyOffset) == 0 ||
                !ValidateControl(
                    layoutRoot_,
                    allowControl_,
                    L"AllowBtn",
                    8) ||
                !ValidateControl(
                    layoutRoot_,
                    refuseControl_,
                    L"RefuseBtn",
                    9))
                return false;
            return selectedControl == allowControl_ ||
                selectedControl == refuseControl_;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool PromptVtableMatches(const void* prompt) const
    {
        if (!prompt)
            return false;
        void* vtable = Read<void*>(prompt);
        if (vtable != promptVtable_)
            return false;
        auto** slots =
            static_cast<void**>(vtable);
        return slots[kPromptCallbackSlot] ==
                   reinterpret_cast<void*>(
                       promptCallback_) &&
            slots[kPromptInitializeSlot] ==
                promptInitializeEntry_ &&
            slots[kPromptCloneSlot] ==
                promptCloneEntry_;
    }

    bool PromptIdentityMatches(
        const void* prompt) const
    {
        return const_cast<FriendInviteReceiver*>(this)
            ->PromptMatchesExpected(prompt, false);
    }

    bool InvitationContextMatches() const
    {
        __try {
            uint8_t* profile =
                profilePointer_
                    ? *profilePointer_
                    : nullptr;
            if (!profile ||
                profile[kProfileInvitationState] != 1)
                return false;
            const uint8_t* context =
                profile +
                kProfileInvitationContext;
            return Read<uint16_t>(context) == 0 &&
                Read<uint16_t>(context + 2) ==
                    expectedChannelId_ &&
                Read<uint32_t>(context + 4) ==
                    expectedRoomId_ &&
                Read<uint16_t>(context + 8) == 0 &&
                Read<uint32_t>(context + 0x0C) ==
                    expectedInviterId_ &&
                context[0x10] <=
                    kMaximumPasswordLength;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void RefreshRoomCompletion()
    {
        __try {
            uint8_t* profile =
                profilePointer_
                    ? *profilePointer_
                    : nullptr;
            uint8_t* room =
                profile
                    ? Read<uint8_t*>(
                          profile +
                          kProfileRoom)
                    : nullptr;
            uint8_t* app =
                gameApp_ ? gameApp_()
                         : nullptr;
            observation_.profile_room =
                reinterpret_cast<uintptr_t>(room);
            observation_.model_room_id =
                room ? Read<uint32_t>(room)
                     : 0;
            observation_.native_app_state =
                app ? Read<uint32_t>(
                          app +
                          kGameAppState)
                    : UINT32_MAX;
            observation_.room_model_matched =
                room &&
                        observation_.model_room_id ==
                            expectedRoomId_
                    ? 1
                    : 0;
            observation_.local_guest =
                room && !isRoomMaster_(room)
                    ? 1
                    : 0;

            uint8_t* owner =
                app
                    ? Read<uint8_t*>(
                          app +
                          kGameAppStateOwner)
                    : nullptr;
            observation_.wait_room_owner_matched =
                app &&
                        observation_.native_app_state ==
                            kThreeDWaitRoomAppState &&
                        owner &&
                        Read<void*>(owner) ==
                            threeDWaitRoomVtable_
                    ? 1
                    : 0;
            uint8_t* manager =
                owner ? Read<uint8_t*>(owner + 4)
                      : nullptr;
            void* managerVtable =
                manager ? Read<void*>(manager)
                        : nullptr;
            observation_.manager_vtable_matched =
                manager &&
                        (managerVtable ==
                             managerVtables_[0] ||
                         managerVtable ==
                             managerVtables_[1] ||
                         managerVtable ==
                             managerVtables_[2])
                    ? 1
                    : 0;
            if (observation_.room_model_matched &&
                observation_.local_guest &&
                observation_.wait_room_owner_matched &&
                observation_.manager_vtable_matched)
                observation_.phase = kRoomJoined;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    bool BeginIncoming(const void* packet)
    {
        if (!armed_ ||
            observation_.phase != kArmed ||
            !packet)
            return false;
        __try {
            if (!PacketMatchesExpected(packet))
                return false;
            std::memcpy(
                incomingPacket_,
                packet,
                kIncomingInvitePacketSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fail(kIncomingPacketMismatch);
            return false;
        }
        incomingPacketPointer_ = packet;
        observedNativeFlag_ =
            incomingPacket_[
                kPacketNativeFlagOffset];
        observation_.native_flag =
            observedNativeFlag_;
        dispatchThreadId_ = GetCurrentThreadId();
        dispatchScope_ = true;
        observation_.incoming_packet_matched = 1;
        observation_.incoming_opcode =
            kIncomingInviteOpcode;
        observation_.phase = kDispatchObserved;
        return true;
    }

    void FinishIncoming()
    {
        dispatchScope_ = false;
        dispatchThreadId_ = 0;
        incomingPacketPointer_ = nullptr;
        if (observation_.phase == kDispatchObserved)
            Fail(kPromptNotCreated);
        else if (
            observation_.phase ==
            kPromptConstructed)
            Fail(kPromptCloneMismatch);
        // A verified clone may be initialized later by the native UI loader.
        // Its original initializer and lifetime hooks remain authoritative.
    }

    bool CanObserveJoinContinuation() const
    {
        return armed_ &&
            expectedDecision_ == kAccept &&
            observation_.phase == kDecisionSent &&
            observation_.decision_sent &&
            observation_.invitation_context_matched;
    }

    bool Fail(Failure failure)
    {
        observation_.failure = failure;
        observation_.phase = kFailed;
        return false;
    }

    void ResetObservation()
    {
        std::memset(
            &observation_,
            0,
            sizeof(observation_));
        observation_.native_app_state =
            UINT32_MAX;
        armed_ = false;
        dispatchScope_ = false;
        initializeScope_ = false;
        dispatchThreadId_ = 0;
        initializeThreadId_ = 0;
        incomingPacketPointer_ = nullptr;
        sourcePrompt_ = nullptr;
        managedPrompt_ = nullptr;
        layoutRoot_ = nullptr;
        allowControl_ = nullptr;
        refuseControl_ = nullptr;
        expectedChannelId_ = 0;
        expectedRoomId_ = 0;
        expectedInviterId_ = 0;
        observedNativeFlag_ = 0;
        expectedDecision_ = kNoDecision;
        expectedNameLength_ = 0;
        std::memset(
            expectedName_,
            0,
            sizeof(expectedName_));
        std::memset(
            incomingPacket_,
            0,
            sizeof(incomingPacket_));
    }

    static int __fastcall ObserveDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        FriendInviteReceiver* receiver = active_;
        if (!receiver || !receiver->genericDispatch_)
            return 0;
        const uint16_t normalized =
            static_cast<uint16_t>(opcode);
        bool ownedIncoming = false;
        bool ownedContinuation = false;
        NativeActions::TransportObservation before = {};
        if (normalized == kIncomingInviteOpcode)
            ownedIncoming =
                receiver->BeginIncoming(packet);
        else if (normalized ==
                     kJoinContinuationOpcode &&
                 receiver->CanObserveJoinContinuation()) {
            ownedContinuation =
                receiver->InvitationContextMatches();
            if (ownedContinuation) {
                receiver->observation_
                    .join_continuation_observed = 1;
                receiver->observation_.phase =
                    kJoinContinuationObserved;
                before =
                    receiver->native_->TransportState();
            }
        }

        const int result =
            receiver->genericDispatch_(
                self,
                a,
                b,
                c,
                opcode,
                packet);
        const DWORD error = GetLastError();

        if (ownedIncoming)
            receiver->FinishIncoming();
        if (ownedContinuation) {
            const NativeActions::TransportObservation after =
                receiver->native_->TransportState();
            receiver->observation_
                .join_transport_sequence_before =
                before.send_sequence;
            receiver->observation_
                .join_transport_sequence_after =
                after.send_sequence;
            receiver->observation_.join_opcode =
                after.opcode;
            receiver->observation_.join_packet_size =
                after.packet_size;
            receiver->observation_
                .join_transport_result =
                after.result;
            if (after.send_sequence !=
                    before.send_sequence &&
                after.opcode ==
                    kJoinRequestOpcode &&
                after.packet_size ==
                    kJoinRequestSize &&
                after.result != 0) {
                receiver->observation_
                    .join_request_sent = 1;
                receiver->observation_.phase =
                    kJoinRequestSent;
            } else {
                receiver->Fail(
                    kJoinTransportMismatch);
            }
        }
        SetLastError(error);
        return result;
    }

    static void* __fastcall ObservePromptCtor(
        void* self,
        void*,
        const void* packet)
    {
        FriendInviteReceiver* receiver = active_;
        if (!receiver || !receiver->promptCtor_)
            return self;
        void* constructed =
            receiver->promptCtor_(self, packet);
        const DWORD error = GetLastError();
        if (receiver->dispatchScope_ &&
            receiver->dispatchThreadId_ ==
                GetCurrentThreadId() &&
            packet ==
                receiver->incomingPacketPointer_) {
            receiver->observation_.prompt_constructed =
                constructed == self ? 1 : 0;
            bool matched = false;
            __try {
                matched =
                    constructed == self &&
                    receiver->PromptMatchesExpected(
                        self,
                        true);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                matched = false;
            }
            if (matched) {
                receiver->sourcePrompt_ = self;
                receiver->observation_.source_prompt =
                    reinterpret_cast<uintptr_t>(self);
                receiver->observation_.phase =
                    kPromptConstructed;
            } else {
                receiver->Fail(
                    kPromptIdentityMismatch);
            }
        }
        SetLastError(error);
        return constructed;
    }

    static void* __fastcall ObservePromptClone(
        void* self,
        void*)
    {
        FriendInviteReceiver* receiver = active_;
        if (!receiver || !receiver->promptClone_)
            return nullptr;
        void* clone =
            receiver->promptClone_(self);
        const DWORD error = GetLastError();
        if (receiver->dispatchScope_ &&
            receiver->dispatchThreadId_ ==
                GetCurrentThreadId() &&
            self == receiver->sourcePrompt_) {
            receiver->observation_
                .prompt_clone_created =
                clone && clone != self ? 1 : 0;
            bool matched = false;
            __try {
                matched =
                    clone && clone != self &&
                    receiver->PromptMatchesExpected(
                        clone,
                        false);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                matched = false;
            }
            receiver->observation_
                .prompt_clone_identity_matched =
                matched ? 1 : 0;
            if (matched) {
                receiver->managedPrompt_ = clone;
                receiver->observation_.managed_prompt =
                    reinterpret_cast<uintptr_t>(clone);
                receiver->observation_.phase =
                    kPromptQueued;
            } else {
                receiver->Fail(
                    kPromptCloneMismatch);
            }
        }
        SetLastError(error);
        return clone;
    }

    static void __fastcall ObservePromptInitialize(
        void* self,
        void*,
        int compare)
    {
        FriendInviteReceiver* receiver = active_;
        if (!receiver || !receiver->promptInitialize_)
            return;
        const bool owned =
            self == receiver->managedPrompt_ &&
            !receiver->observation_.prompt_destroyed;
        if (owned) {
            receiver->layoutRoot_ = nullptr;
            receiver->allowControl_ = nullptr;
            receiver->refuseControl_ = nullptr;
            receiver->initializeThreadId_ =
                GetCurrentThreadId();
            receiver->initializeScope_ = true;
        }
        receiver->promptInitialize_(self, compare);
        const DWORD error = GetLastError();
        if (owned) {
            receiver->initializeScope_ = false;
            receiver->initializeThreadId_ = 0;
            bool matched = false;
            bool allowMatched = false;
            bool refuseMatched = false;
            bool allowEnabled = false;
            bool refuseEnabled = false;
            bool allowHit = false;
            bool refuseHit = false;
            __try {
                allowMatched =
                    receiver->ValidateControlIdentity(
                        receiver->layoutRoot_,
                        receiver->allowControl_,
                        L"AllowBtn",
                        8);
                refuseMatched =
                    receiver->ValidateControlIdentity(
                        receiver->layoutRoot_,
                        receiver->refuseControl_,
                        L"RefuseBtn",
                        9);
                if (receiver->allowControl_) {
                    const auto* allowBytes =
                        static_cast<const uint8_t*>(
                            receiver->allowControl_);
                    allowEnabled =
                        allowBytes[kControlEnabledOffset] != 0;
                    allowHit =
                        allowBytes[kControlHitOffset] != 0;
                }
                if (receiver->refuseControl_) {
                    const auto* refuseBytes =
                        static_cast<const uint8_t*>(
                            receiver->refuseControl_);
                    refuseEnabled =
                        refuseBytes[kControlEnabledOffset] != 0;
                    refuseHit =
                        refuseBytes[kControlHitOffset] != 0;
                }
                matched =
                    receiver->PromptVtableMatches(self) &&
                    receiver->PromptIdentityMatches(self) &&
                    Read<uint32_t>(
                        static_cast<uint8_t*>(self) +
                        kPromptReadyOffset) != 0 &&
                    allowMatched &&
                    refuseMatched && allowEnabled && refuseEnabled && allowHit && refuseHit;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                matched = false;
                allowMatched = false;
                refuseMatched = false;
                allowEnabled = false;
                refuseEnabled = false;
                allowHit = false;
                refuseHit = false;
            }
            receiver->observation_.prompt_initialized =
                matched ? 1 : 0;
            receiver->observation_
                .allow_control_matched =
                allowMatched ? 1 : 0;
            receiver->observation_
                .refuse_control_matched =
                refuseMatched ? 1 : 0;
            receiver->observation_
                .allow_control_enabled =
                allowEnabled ? 1 : 0;
            receiver->observation_
                .refuse_control_enabled =
                refuseEnabled ? 1 : 0;
            receiver->observation_.allow_control_hit =
                allowHit ? 1 : 0;
            receiver->observation_.refuse_control_hit =
                refuseHit ? 1 : 0;
            receiver->observation_.controls_ready = matched && receiver->ValidateLivePromptAndControls(self,receiver->allowControl_)?1:0;
            if (matched) {
                receiver->observation_.phase =
                    receiver->observation_.controls_ready?kPromptInitialized:kPromptWaitingVisible;
            } else {
                receiver->Fail(
                    kPromptUiMismatch);
            }
        }
        SetLastError(error);
    }

    static void __fastcall ObservePromptDtor(
        void* self,
        void*)
    {
        FriendInviteReceiver* receiver = active_;
        if (!receiver || !receiver->promptDtor_)
            return;
        if (self == receiver->managedPrompt_) {
            receiver->observation_.prompt_destroyed = 1;
            receiver->managedPrompt_ = nullptr;
            receiver->layoutRoot_ = nullptr;
            receiver->allowControl_ = nullptr;
            receiver->refuseControl_ = nullptr;
            if (receiver->expectedDecision_ ==
                    kReject &&
                receiver->observation_.decision_sent &&
                receiver->observation_.phase !=
                    kFailed)
                receiver->observation_.phase =
                    kRejected;
        }
        receiver->promptDtor_(self);
    }

    static void* __fastcall ObserveUiFindControl(
        void* root,
        void*,
        const void* nativeName,
        uint32_t kind)
    {
        FriendInviteReceiver* receiver = active_;
        if (!receiver ||
            !receiver->nativeUiFindControl_)
            return nullptr;
        void* control =
            receiver->nativeUiFindControl_(
                root,
                nativeName,
                kind);
        const DWORD error = GetLastError();
        if (receiver->initializeScope_ &&
            receiver->initializeThreadId_ ==
                GetCurrentThreadId() &&
            kind == kButtonControlKind &&
            root && control) {
            bool allow = false;
            bool refuse = false;
            __try {
                allow = NativeStringMatches(
                    nativeName,
                    L"AllowBtn",
                    8);
                refuse = NativeStringMatches(
                    nativeName,
                    L"RefuseBtn",
                    9);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                allow = false;
                refuse = false;
            }
            if (allow || refuse) {
                if (!receiver->layoutRoot_)
                    receiver->layoutRoot_ = root;
                if (receiver->layoutRoot_ == root) {
                    if (allow)
                        receiver->allowControl_ =
                            control;
                    if (refuse)
                        receiver->refuseControl_ =
                            control;
                    receiver->observation_.layout_root =
                        reinterpret_cast<uintptr_t>(
                            root);
                    receiver->observation_.allow_control =
                        reinterpret_cast<uintptr_t>(
                            receiver->allowControl_);
                    receiver->observation_.refuse_control =
                        reinterpret_cast<uintptr_t>(
                            receiver->refuseControl_);
                }
            }
        }
        SetLastError(error);
        return control;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    GameAppFn gameApp_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    IsRoomMasterFn isRoomMaster_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    PacketDispatchFn genericDispatch_ = nullptr;
    PromptCtorFn promptCtor_ = nullptr;
    PromptCloneFn promptClone_ = nullptr;
    PromptInitializeFn promptInitialize_ = nullptr;
    PromptDtorFn promptDtor_ = nullptr;
    void* promptVtable_ = nullptr;
    PromptCallbackFn promptCallback_ = nullptr;
    void* promptCloneEntry_ = nullptr;
    void* promptInitializeEntry_ = nullptr;
    void* threeDWaitRoomVtable_ = nullptr;
    void* managerVtables_[3] = {};
    bool bound_ = false;
    bool armed_ = false;
    bool dispatchScope_ = false;
    bool initializeScope_ = false;
    DWORD dispatchThreadId_ = 0;
    DWORD initializeThreadId_ = 0;
    const void* incomingPacketPointer_ = nullptr;
    void* sourcePrompt_ = nullptr;
    void* managedPrompt_ = nullptr;
    void* layoutRoot_ = nullptr;
    void* allowControl_ = nullptr;
    void* refuseControl_ = nullptr;
    uint16_t expectedChannelId_ = 0;
    uint32_t expectedRoomId_ = 0;
    uint32_t expectedInviterId_ = 0;
    uint8_t observedNativeFlag_ = 0;
    Decision expectedDecision_ = kNoDecision;
    size_t expectedNameLength_ = 0;
    char expectedName_[17] = {};
    uint8_t incomingPacket_[kIncomingInvitePacketSize] = {};
    Observation observation_ = {};

    inline static FriendInviteReceiver* active_ = nullptr;
};

} // namespace friends
