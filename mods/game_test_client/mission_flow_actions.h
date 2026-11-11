#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"
#include "sdgo/room_titles.h"

// Drives the shipped multiplayer mission path for room modes 5 and 8.
//
// The adapter enters through upmenu/SingleBtn, limits the original channel
// chooser to an actually advertised Mode-2 channel, uses the live mission
// lobby and create-dialog callbacks, and waits for the original reply
// consumers and 3D state objects. It never constructs a channel, mission
// descriptor, room/reply/profile object, application state, or result.
//
// Every polling call invokes at most one original UI/business action. Mode 10
// has a distinct single-mission popup/automatic-start path and is deliberately
// rejected here. Modes 11/12 are native AI modes and are not mission aliases.
class MissionFlowActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kEnteringMissionChannel = 2,
        kWaitingForMissionChannel = 3,
        kMissionChannelAccepted = 4,
        kWaitingForMissionLobby = 5,
        kSelectingMultiplayer = 6,
        kOpeningCreateDialog = 7,
        kWaitingForCreateDialog = 8,
        kCyclingMission = 9,
        kWaitingForMissionRefresh = 10,
        kSettingRoomTitle = 11,
        kCreateSubmitted = 12,
        kWaitingForCreateReply = 13,
        kWaitingForNativeRoom = 14,
        kRoomReady = 15,
        kStartQueued = 16,
        kStartRequestSent = 17,
        kBattleStartConsumed = 18,
        kCreateDialogReady = 19,

        kInvalidMode = 0x100,
        kInvalidTitle = 0x101,
        kTopMenuMismatch = 0x102,
        kTopMenuControlMismatch = 0x103,
        kNoAdvertisedMissionChannel = 0x104,
        kChannelRequestRejected = 0x105,
        kChannelSelectionMismatch = 0x106,
        kMissionLobbyMismatch = 0x107,
        kMissionLobbyControlMismatch = 0x108,
        kCreateDialogMismatch = 0x109,
        kMissionDescriptorMismatch = 0x10A,
        kRequestedMissionUnavailable = 0x10B,
        kTitleActionRejected = 0x10C,
        kCreateTransportRejected = 0x10D,
        kCreateReplyMismatch = 0x10E,
        kNativeRoomMismatch = 0x10F,
        kLocalPilotNotHost = 0x110,
        kStartStateMismatch = 0x111,
        kStartAlreadyQueued = 0x112,
        kNativeFault = 0x113,
        kStartServerRejected = 0x114,
    };

    enum ChannelScanStatus : uint32_t
    {
        kChannelScanNotRequested = 0,
        kChannelScanWaitingForReply = 1,
        kChannelScanReady = 2,
        kChannelScanReplyTimedOut = 0x100,
        kChannelScanProfileUnavailable = 0x101,
        kChannelScanVectorUnavailable = 0x102,
        kChannelScanVectorMalformed = 0x103,
        kChannelScanGroupOutOfRange = 0x104,
        kChannelScanChannelCountInvalid = 0x105,
    };

    uint32_t NativeStartConsumedSequence() const
    {
        return static_cast<uint32_t>(InterlockedCompareExchange(&nativeStartConsumedSequence_,0,0));
    }

    struct Observation
    {
        uint32_t result;
        uint32_t run_generation;
        uint32_t native_app_state;
        uint32_t selected_channel_group;
        uint32_t target_channel_group;
        uint32_t requested_mission_id;
        uint32_t selected_mission_id;
        uint32_t selected_mission_key;
        uint32_t requested_room_id;
        uint32_t native_room_id;
        uint32_t create_transport_sequence;
        uint32_t create_reply_sequence;
        uint32_t start_transport_baseline;
        uint32_t start_reply_sequence;
        uint32_t channel_scan_status;
        uint32_t channel_wait_polls;
        uint32_t channel_choice_callbacks;
        uint32_t channel_page_mode;
        uint32_t channel_request_state;
        uint32_t advertised_group_count;
        uint32_t advertised_raw_count;
        uint32_t advertised_mode2_count;
        uint32_t advertised_eligible_count;
        uint32_t rejected_zero_id_count;
        uint32_t rejected_non_mode2_count;
        uint32_t rejected_disabled_count;
        uint32_t rejected_zero_capacity_count;
        uint32_t rejected_full_count;
        uint32_t create_control_kind;
        uint32_t create_control_flags;
        uint32_t native_main_vtable;
        uint32_t native_command_vtable;
        uint32_t native_main_state;
        uint32_t native_context_flags;
        uint32_t last_start_reply_opcode;
        uint32_t last_start_reply_status;
        uint16_t selected_channel;
        uint16_t target_channel;
        uint16_t create_opcode;
        uint16_t start_opcode;
        uint8_t requested_mode;
        uint8_t native_room_mode;
        uint8_t channel_choice_armed;
        uint8_t channel_request_sent;
        uint8_t mission_lobby_observed;
        uint8_t create_dialog_observed;
        uint8_t create_submitted;
        uint8_t create_reply_consumed;
        uint8_t native_room_ready;
        uint8_t local_is_host;
        uint8_t start_queued;
        uint8_t start_request_sent;
        uint8_t battle_start_consumed;
        uint8_t natural_result_observed;
        uint8_t natural_exit_observed;
        uint8_t channel_list_ready;
        uint8_t channel_scan_valid;
        uint8_t reserved[2];
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        if (bound_)
            return false;

        api_ = api;
        mod_ = mod;
        native_ = native;
        gameApp_ = Symbol<GameAppFn>("game.app");
        profilePointer_ =
            Symbol<uint8_t**>("flow.profile_pointer");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>(
                "flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>(
                "flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>(
                "flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        nativeEditSetText_ =
            Symbol<NativeEditSetTextFn>(
                "flow.native_edit_set_text");

        lobbyUiVtable_ =
            Symbol<void*>("flow.lobby_ui_vtable");
        lobbyUiCallback_ =
            Symbol<UiCallbackFn>(
                "flow.lobby_ui_callback");
        lobbyChannelListChoice_ =
            Symbol<LobbyChannelListChoiceFn>(
                "flow.lobby_channel_list_choice");
        lobbyJoinChannel_ =
            Symbol<LobbyJoinChannelFn>(
                "flow.lobby_join_channel_business");

        missionLobbyVtable_ =
            Symbol<void*>(
                "flow.mission_lobby_vtable");
        missionLobbyCallback_ =
            Symbol<UiCallbackFn>(
                "flow.mission_lobby_callback");
        missionLobbyPacketDispatch_ =
            Symbol<MissionReplyDispatchFn>(
                "flow.mission_lobby_packet_dispatch");
        missionLobbyReplyDispatch_ =
            Symbol<MissionReplyDispatchFn>(
                "flow.mission_lobby_reply_dispatch");

        missionCreateVtable_ =
            Symbol<void*>(
                "flow.mission_create_vtable");
        missionCreateCallback_ =
            Symbol<UiCallbackFn>(
                "flow.mission_create_callback");
        missionCreateUpdate_ =
            Symbol<MissionCreateUpdateFn>(
                "flow.mission_create_update");

        threeDWaitRoomVtable_ =
            Symbol<void*>(
                "flow.three_d_wait_room_vtable");
        threeDStartCommandVtable_ =
            Symbol<void*>(
                "flow.three_d_start_command_vtable");
        threeDRoomHasState_ =
            Symbol<ThreeDRoomHasStateFn>(
                "flow.three_d_room_has_state");
        threeDRoomAddState_ =
            Symbol<ThreeDRoomAddStateFn>(
                "flow.three_d_room_add_state");
        missionThreeDManagerVtable_ =
            Symbol<void*>(
                "flow.mission_3d_manager_vtable");
        missionThreeDMainVtable_ =
            Symbol<void*>(
                "flow.mission_3d_main_vtable");
        missionStartBusiness_=Symbol<MissionStartBusinessFn>("flow.mission_3d_start_business");
        missionStartReplyDispatch_ =
            Symbol<MissionReplyDispatchFn>(
                "flow.mission_3d_start_reply_dispatch");
        roomInfo_=Symbol<RoomInfoFn>("flow.mission_room_info");
        isRoomMaster_ =
            Symbol<IsRoomMasterFn>(
                "flow.mission_is_room_master");

        if (!api_ || !api_->HookCode || !mod_ || !native_ ||
            !gameApp_ || !profilePointer_ ||
            !serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_ ||
            !nativeUiFindControl_ || !nativeEditSetText_ ||
            !lobbyUiVtable_ || !lobbyUiCallback_ ||
            !lobbyChannelListChoice_ || !lobbyJoinChannel_ ||
            !missionLobbyVtable_ || !missionLobbyCallback_ ||
            !missionLobbyPacketDispatch_ ||
            !missionLobbyReplyDispatch_ ||
            !missionCreateVtable_ || !missionCreateCallback_ ||
            !missionCreateUpdate_ ||
            !threeDWaitRoomVtable_ ||
            !threeDStartCommandVtable_ ||
            !threeDRoomHasState_ || !threeDRoomAddState_ ||
            !missionThreeDManagerVtable_ ||
            !missionThreeDMainVtable_ ||
            !missionStartReplyDispatch_ || !missionStartBusiness_ || !isRoomMaster_ || !roomInfo_)
            return false;

        __try {
            if (!SlotMatches(
                    lobbyUiVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        lobbyUiCallback_)) ||
                !SlotMatches(
                    missionLobbyVtable_,
                    kReplyVtableSlot,
                    reinterpret_cast<void*>(
                        missionLobbyPacketDispatch_)) ||
                !SlotMatches(
                    missionLobbyVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        missionLobbyCallback_)) ||
                !SlotMatches(
                    missionCreateVtable_,
                    kUpdateVtableSlot,
                    reinterpret_cast<void*>(
                        missionCreateUpdate_)) ||
                !SlotMatches(
                    missionCreateVtable_,
                    kUiCallbackVtableSlot,
                    reinterpret_cast<void*>(
                        missionCreateCallback_)) ||
                !SlotMatches(
                    threeDStartCommandVtable_,
                    kStartReplyVtableSlot,
                    reinterpret_cast<void*>(
                        missionStartReplyDispatch_)))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (active_ && active_ != this)
            return false;
        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.lobby_channel_list_choice",
                reinterpret_cast<void*>(
                    &ChooseAdvertisedMissionChannel),
                reinterpret_cast<void**>(
                    &lobbyChannelListChoice_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.mission_lobby_reply_dispatch",
                reinterpret_cast<void*>(
                    &ObserveMissionCreateReply),
                reinterpret_cast<void**>(
                    &missionLobbyReplyDispatch_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.mission_3d_start_reply_dispatch",
                reinterpret_cast<void*>(
                    &ObserveMissionStartReply),
                reinterpret_cast<void**>(
                    &missionStartReplyDispatch_))) {
            active_ = nullptr;
            return false;
        }

        bound_ = true;
        Reset();
        return true;
    }

    // Clears only adapter-owned observations and arms no action. Native
    // services, profile, room, channel and application state are untouched.
    void Reset()
    {
        InterlockedIncrement(&runGeneration_);
        InterlockedExchange(&lastResult_, kUnbound);
        InterlockedExchange(&channelChoiceArmed_, 0);
        InterlockedExchange(&channelChoiceFailed_, 0);
        InterlockedExchange(&channelRequestSent_, 0);
        InterlockedExchange(&targetChannelGroup_, 0);
        InterlockedExchange(&targetChannel_, 0);
        InterlockedExchange(
            &channelScanStatus_,
            kChannelScanNotRequested);
        InterlockedExchange(&channelWaitPolls_, 0);
        InterlockedExchange(&channelChoiceCallbacks_, 0);
        InterlockedExchange(&channelPageMode_, 0);
        InterlockedExchange(&channelRequestState_, 0);
        InterlockedExchange(&channelListReady_, 0);
        InterlockedExchange(&advertisedGroupCount_, 0);
        InterlockedExchange(&advertisedRawCount_, 0);
        InterlockedExchange(&advertisedMode2Count_, 0);
        InterlockedExchange(&advertisedEligibleCount_, 0);
        InterlockedExchange(&rejectedZeroIdCount_, 0);
        InterlockedExchange(&rejectedNonMode2Count_, 0);
        InterlockedExchange(&rejectedDisabledCount_, 0);
        InterlockedExchange(&rejectedZeroCapacityCount_, 0);
        InterlockedExchange(&rejectedFullCount_, 0);
        InterlockedExchange(&createTransportSequence_, 0);
        InterlockedExchange(&createSubmitGeneration_, 0);
        InterlockedExchange(&createOpcode_, 0);
        InterlockedExchange(&createSubmitted_, 0);
        InterlockedExchange(&createReplySequence_, 0);
        InterlockedExchange(&createReplyConsumed_, 0);
        InterlockedExchange(&createReplyFailed_, 0);
        InterlockedExchange(&requestedRoomId_, 0);
        InterlockedExchange(&startTransportBaseline_, 0);
        InterlockedExchange(&startQueued_, 0);
        InterlockedExchange(&startRequestSent_, 0);
        InterlockedExchange(&startReplySequence_, 0);
        InterlockedExchange(&battleStartConsumed_, 0);
        InterlockedExchange(&lastStartReplyOpcode_,0);InterlockedExchange(&lastStartReplyStatus_,0);
        InterlockedExchangePointer(
            &topOwner_,
            nullptr);
        InterlockedExchangePointer(
            &missionLobbyOwner_,
            nullptr);
        InterlockedExchangePointer(
            &startCommandOwner_,
            nullptr);
        requestedMode_ = 0;
        requestedMissionId_ = 0;
        title_[0] = '\0';
        topEntryInvoked_ = false;
        makeRoomInvoked_ = false;
        createDialogObserved_ = false;
        prepareOnly_ = false;
        createControlKind_=0;createControlFlags_=0;
        nativeMainVtable_=0;nativeCommandVtable_=0;nativeMainState_=UINT32_MAX;
        nativeContextFlags_=0;
        titleSet_ = false;
        createAttempted_ = false;
        initialMissionKey_ = 0;
        missionCycleCount_ = 0;
        selectedMissionId_ = 0;
        selectedMissionKey_ = 0;
        nativeRoomId_ = 0;
        nativeRoomMode_ = 0;
        nativeRoomReady_ = false;
        localIsHost_ = false;
    }

    // Poll from the original client update thread. Modes 5/8 share this
    // multiplayer mission flow. A different mode/title requires Reset().
    Result EnsureRoom(
        uint16_t mode,
        const char* roomTitle,
        bool submit = true,
        uint32_t missionId = 0)
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (mode != 5 && mode != 8)
            return Publish(kInvalidMode);
        if (missionId != 0 &&
            (missionId <= kCreateMissionThreshold ? 5 : 8) != mode)
            return Publish(kInvalidMode);
        if (!ValidTitle(roomTitle))
            return Publish(kInvalidTitle);
        if (!ArmScenario(mode, roomTitle, missionId))
            return Publish(
                requestedMode_ == mode &&
                        requestedMissionId_ == missionId
                    ? kInvalidTitle
                    : kInvalidMode);
        prepareOnly_ = !submit;

        if (InterlockedCompareExchange(
                &createReplyConsumed_, 0, 0) != 0)
            return Publish(PollNativeRoom());
        if (InterlockedCompareExchange(
                &createReplyFailed_, 0, 0) != 0)
            return Publish(kCreateReplyMismatch);
        if (InterlockedCompareExchange(
                &createSubmitted_, 0, 0) != 0)
            return Publish(kWaitingForCreateReply);

        const NativeActions::OnboardingObservation selected =
            native_->OnboardingState();
        if (selected.native_app_state != kLobbyAppState)
            return Publish(kWaitingForLobby);

        __try {
            void* lobby = ResolveUniqueByVtable(
                missionLobbyVtable_);
            if (lobby) {
                if (!SelectedMissionChannelMatches(
                        selected))
                    return Publish(
                        kChannelSelectionMismatch);
                InterlockedExchangePointer(
                    &missionLobbyOwner_,
                    lobby);
                const Result lobbyResult =
                    DriveMissionLobby(lobby);
                return Publish(lobbyResult);
            }

            const LONG channelFailure =
                InterlockedCompareExchange(
                    &channelChoiceFailed_, 0, 0);
            if (channelFailure != 0)
                return Publish(
                    static_cast<Result>(
                        channelFailure));
            if (InterlockedCompareExchange(
                    &channelRequestSent_, 0, 0) != 0) {
                const uint32_t targetGroup =
                    static_cast<uint32_t>(
                        InterlockedCompareExchange(
                            &targetChannelGroup_,
                            0,
                            0));
                const uint16_t targetChannel =
                    static_cast<uint16_t>(
                        InterlockedCompareExchange(
                            &targetChannel_,
                            0,
                            0));
                if (selected.channel_group ==
                        targetGroup &&
                    selected.channel_id ==
                        targetChannel)
                    return Publish(
                        kMissionChannelAccepted);
                return Publish(
                    kWaitingForMissionChannel);
            }

            return Publish(EnterMissionChannel());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    // Invoke the actual mission Start business. It first queues team admission
    // (state 0x0E), then the native battle-start command (0x0C). Bypassing the
    // first state sends 08A7 before a battle room exists and yields 0FB7.
    Result Start()
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (requestedMode_ != 5 &&
            requestedMode_ != 8)
            return Publish(kInvalidMode);

        if (InterlockedCompareExchange(
                &battleStartConsumed_, 0, 0) != 0)
            return Publish(kBattleStartConsumed);
        if(InterlockedCompareExchange(&lastStartReplyStatus_,0,0)!=0)
            return Publish(kStartServerRejected);
        if(InterlockedCompareExchange(&startRequestSent_,0,0)!=0)
            return Publish(kStartRequestSent);

        if (InterlockedCompareExchange(
                &startQueued_, 0, 0) != 0) {
            const NativeActions::TransportObservation sent =
                native_->TransportState();
            const uint32_t baseline =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &startTransportBaseline_,
                        0,
                        0));
            if (sent.send_sequence > baseline &&
                sent.opcode == kStartRequestOpcode &&
                sent.packet_size ==
                    kStartRequestSize &&
                sent.result != 0) {
                InterlockedExchange(
                    &startRequestSent_, 1);
                return Publish(
                    kStartRequestSent);
            }
            return Publish(kStartQueued);
        }

        __try {
            NativeRoomView room = {};
            const RoomResult roomResult =
                ReadNativeRoom(&room);
            if (roomResult == kRoomNotReady)
                return Publish(kWaitingForNativeRoom);
            if (roomResult == kRoomNotHost)
                return Publish(kLocalPilotNotHost);
            if (roomResult != kRoomMatched)
                return Publish(kStartStateMismatch);
            if (threeDRoomHasState_(
                    room.manager,
                    kStartState,
                    0))
                return Publish(kStartAlreadyQueued);

            const NativeActions::TransportObservation before =
                native_->TransportState();
            InterlockedExchangePointer(
                &startCommandOwner_,
                room.start_command);
            InterlockedExchange(
                &startTransportBaseline_,
                static_cast<LONG>(
                    before.send_sequence));
            InterlockedExchange(&startQueued_, 1);
            missionStartBusiness_(room.main_state);
            if(!threeDRoomHasState_(room.manager,0x0E,0) &&
               !threeDRoomHasState_(room.manager,kStartState,0)) {
                InterlockedExchange(&startQueued_,0);
                return Publish(kWaitingForNativeRoom);
            }
            const NativeActions::TransportObservation after =
                native_->TransportState();
            if (after.send_sequence >
                    before.send_sequence &&
                after.opcode ==
                    kStartRequestOpcode &&
                after.packet_size ==
                    kStartRequestSize &&
                after.result != 0) {
                InterlockedExchange(
                    &startRequestSent_, 1);
                return Publish(
                    kStartRequestSent);
            }
            return Publish(kStartQueued);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    // Game-thread-only snapshot. This method deliberately reads the same
    // native observations and adapter state that EnsureRoom/Start mutate;
    // callers that publish snapshots to another thread must copy this value
    // there instead of invoking native accessors from that thread.
    Observation PollSnapshot()
    {
        Observation observed = {};
        const NativeActions::OnboardingObservation selected =
            native_
                ? native_->OnboardingState()
                : NativeActions::OnboardingObservation{};
        observed.result =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &lastResult_, 0, 0));
        observed.run_generation =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &runGeneration_, 0, 0));
        observed.native_app_state =
            selected.native_app_state;
        observed.selected_channel_group =
            selected.channel_group;
        observed.target_channel_group =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &targetChannelGroup_, 0, 0));
        observed.requested_mission_id =
            requestedMissionId_;
        observed.selected_mission_id =
            selectedMissionId_;
        observed.selected_mission_key =
            selectedMissionKey_;
        observed.requested_room_id =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &requestedRoomId_, 0, 0));
        observed.native_room_id =
            nativeRoomId_;
        observed.create_transport_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &createTransportSequence_,
                    0,
                    0));
        observed.create_reply_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &createReplySequence_,
                    0,
                    0));
        observed.start_transport_baseline =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &startTransportBaseline_,
                    0,
                    0));
        observed.start_reply_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &startReplySequence_,
                    0,
                    0));
        observed.channel_scan_status =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &channelScanStatus_,
                    0,
                    0));
        observed.channel_wait_polls =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &channelWaitPolls_,
                    0,
                    0));
        observed.channel_choice_callbacks =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &channelChoiceCallbacks_,
                    0,
                    0));
        observed.channel_page_mode =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &channelPageMode_,
                    0,
                    0));
        observed.channel_request_state =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &channelRequestState_,
                    0,
                    0));
        observed.advertised_group_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &advertisedGroupCount_,
                    0,
                    0));
        observed.advertised_raw_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &advertisedRawCount_,
                    0,
                    0));
        observed.advertised_mode2_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &advertisedMode2Count_,
                    0,
                    0));
        observed.advertised_eligible_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &advertisedEligibleCount_,
                    0,
                    0));
        observed.rejected_zero_id_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &rejectedZeroIdCount_,
                    0,
                    0));
        observed.rejected_non_mode2_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &rejectedNonMode2Count_,
                    0,
                    0));
        observed.rejected_disabled_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &rejectedDisabledCount_,
                    0,
                    0));
        observed.rejected_zero_capacity_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &rejectedZeroCapacityCount_,
                    0,
                    0));
        observed.rejected_full_count =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &rejectedFullCount_,
                    0,
                    0));
        observed.selected_channel =
            selected.channel_id;
        observed.target_channel =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &targetChannel_, 0, 0));
        observed.create_opcode =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &createOpcode_, 0, 0));
        observed.start_opcode =
            InterlockedCompareExchange(
                &startRequestSent_, 0, 0)
                ? kStartRequestOpcode
                : 0;
        observed.requested_mode =
            requestedMode_;
        observed.native_room_mode =
            nativeRoomMode_;
        observed.channel_choice_armed =
            InterlockedCompareExchange(
                &channelChoiceArmed_, 0, 0) != 0;
        observed.channel_request_sent =
            InterlockedCompareExchange(
                &channelRequestSent_, 0, 0) != 0;
        observed.mission_lobby_observed =
            InterlockedCompareExchangePointer(
                &missionLobbyOwner_,
                nullptr,
                nullptr) != nullptr;
        observed.create_dialog_observed =
            createDialogObserved_ ? 1 : 0;
        observed.create_control_kind=createControlKind_;
        observed.create_control_flags=createControlFlags_;
        observed.native_main_vtable=nativeMainVtable_;
        observed.native_command_vtable=nativeCommandVtable_;
        observed.native_main_state=nativeMainState_;
        observed.native_context_flags=nativeContextFlags_;
        observed.last_start_reply_opcode=static_cast<uint32_t>(InterlockedCompareExchange(&lastStartReplyOpcode_,0,0));
        observed.last_start_reply_status=static_cast<uint32_t>(InterlockedCompareExchange(&lastStartReplyStatus_,0,0));
        observed.create_submitted =
            InterlockedCompareExchange(
                &createSubmitted_, 0, 0) != 0;
        observed.create_reply_consumed =
            InterlockedCompareExchange(
                &createReplyConsumed_, 0, 0) != 0;
        observed.native_room_ready =
            nativeRoomReady_ ? 1 : 0;
        observed.local_is_host =
            localIsHost_ ? 1 : 0;
        observed.start_queued =
            InterlockedCompareExchange(
                &startQueued_, 0, 0) != 0;
        observed.start_request_sent =
            InterlockedCompareExchange(
                &startRequestSent_, 0, 0) != 0;
        observed.battle_start_consumed =
            InterlockedCompareExchange(
                &battleStartConsumed_, 0, 0) != 0;
        observed.channel_list_ready =
            InterlockedCompareExchange(
                &channelListReady_, 0, 0) != 0;
        observed.channel_scan_valid =
            observed.channel_scan_status ==
                kChannelScanReady;
        // Result and active-battle exit are intentionally OPEN until their
        // original consumers/transitions are separately verified.
        observed.natural_result_observed = 0;
        observed.natural_exit_observed = 0;
        return observed;
    }

    Observation State() { return PollSnapshot(); }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kWaitingForLobby: return "waiting-for-lobby";
        case kEnteringMissionChannel: return "entering-mission-channel";
        case kWaitingForMissionChannel: return "waiting-for-mission-channel";
        case kMissionChannelAccepted: return "mission-channel-accepted";
        case kWaitingForMissionLobby: return "waiting-for-mission-lobby";
        case kSelectingMultiplayer: return "selecting-multiplayer";
        case kOpeningCreateDialog: return "opening-create-dialog";
        case kWaitingForCreateDialog: return "waiting-for-create-dialog";
        case kCyclingMission: return "cycling-mission";
        case kWaitingForMissionRefresh: return "waiting-for-mission-refresh";
        case kSettingRoomTitle: return "setting-room-title";
        case kCreateSubmitted: return "create-submitted";
        case kWaitingForCreateReply: return "waiting-for-create-reply";
        case kWaitingForNativeRoom: return "waiting-for-native-room";
        case kRoomReady: return "room-ready";
        case kStartQueued: return "start-queued";
        case kStartRequestSent: return "start-request-sent";
        case kBattleStartConsumed: return "battle-start-consumed";
        case kCreateDialogReady: return "create-dialog-ready";
        case kInvalidMode: return "invalid-mode";
        case kInvalidTitle: return "invalid-title";
        case kTopMenuMismatch: return "top-menu-mismatch";
        case kTopMenuControlMismatch: return "top-menu-control-mismatch";
        case kNoAdvertisedMissionChannel: return "no-advertised-mission-channel";
        case kChannelRequestRejected: return "channel-request-rejected";
        case kChannelSelectionMismatch: return "channel-selection-mismatch";
        case kMissionLobbyMismatch: return "mission-lobby-mismatch";
        case kMissionLobbyControlMismatch: return "mission-lobby-control-mismatch";
        case kCreateDialogMismatch: return "create-dialog-mismatch";
        case kMissionDescriptorMismatch: return "mission-descriptor-mismatch";
        case kRequestedMissionUnavailable: return "requested-mission-unavailable";
        case kTitleActionRejected: return "title-action-rejected";
        case kCreateTransportRejected: return "create-transport-rejected";
        case kCreateReplyMismatch: return "create-reply-mismatch";
        case kNativeRoomMismatch: return "native-room-mismatch";
        case kLocalPilotNotHost: return "local-pilot-not-host";
        case kStartStateMismatch: return "start-state-mismatch";
        case kStartAlreadyQueued: return "start-already-queued";
        case kNativeFault: return "native-fault";
        case kStartServerRejected: return "start-server-rejected";
        default: return "unknown";
        }
    }

private:
    enum RoomResult
    {
        kRoomNotReady,
        kRoomMatched,
        kRoomNotHost,
        kRoomInvalid,
    };

    struct NativeRoomView
    {
        uint8_t* profile;
        void* room;
        uint8_t* owner;
        uint8_t* manager;
        uint8_t* main_state;
        void* start_command;
        uint32_t room_id;
        uint8_t room_mode;
    };

    enum ChannelScanResult
    {
        kChannelScanFound,
        kChannelScanNoMatch,
        kChannelScanInvalid,
    };

    struct ChannelScan
    {
        uint32_t status;
        uint32_t group_count;
        uint32_t raw_count;
        uint32_t mode2_count;
        uint32_t eligible_count;
        uint32_t rejected_zero_id;
        uint32_t rejected_non_mode2;
        uint32_t rejected_disabled;
        uint32_t rejected_zero_capacity;
        uint32_t rejected_full;
    };

    static constexpr uint32_t kLobbyAppState = 6;
    static constexpr uint32_t kWaitRoomAppState = 18;
    static constexpr uint32_t kButtonEvent = 0x101;
    static constexpr uint32_t kRadioEvent = 0x301;
    static constexpr uint32_t kButtonKind = 4;
    static constexpr uint32_t kRadioKind = 0x0C;
    static constexpr uint32_t kEditKind = 8;
    static constexpr uint32_t kMissionChannelMode = 2;
    static constexpr LONG kMaxChannelListWaitPolls = 600;
    static constexpr uint32_t kCreateMissionThreshold = 0x313990;
    static constexpr uint16_t kJoinChannelOpcode = 0x04FA;
    static constexpr uint32_t kJoinChannelSize = 0x0E;
    static constexpr uint16_t kCreateOpcodeValue = 0x0640;
    static constexpr uint32_t kCreatePacketSize = 0x53;
    static constexpr uint16_t kCreateReplyOpcode = 0x0A33;
    static constexpr uint16_t kRoomDetailsOpcode = 0x0621;
    static constexpr uint32_t kRoomDetailsSize = 10;
    static constexpr uint16_t kStartRequestOpcode = 0x08A7;
    static constexpr uint32_t kStartRequestSize = 8;
    static constexpr uint16_t kStartReplyOpcode = 0x06C3;

    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kNativeStringSize = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kControlVisible = 0x162;
    static constexpr size_t kControlEnabled = 0x167;
    static constexpr size_t kUiCallbackVtableSlot =
        0x38 / sizeof(void*);
    static constexpr size_t kReplyVtableSlot =
        0x28 / sizeof(void*);
    static constexpr size_t kUpdateVtableSlot =
        0x30 / sizeof(void*);
    static constexpr size_t kStartReplyVtableSlot =
        0x18 / sizeof(void*);

    static constexpr size_t kServiceListHead = 0x44;
    static constexpr size_t kServiceNodeNext = 0;
    static constexpr size_t kServiceNodeObject = 8;
    static constexpr uint32_t kMaxServiceNodes = 512;

    static constexpr size_t kTopRoot = 0x44;
    static constexpr size_t kTopSingleButton = 0x54;
    static constexpr size_t kTopRequestState = 0x68;
    static constexpr size_t kTopTargetGroup = 0x6C;
    static constexpr size_t kTopPageMode = 0x70;
    static constexpr uint32_t kMissionPageMode = 2;
    static constexpr uint32_t kChannelListPending = 2;
    static constexpr uint32_t kJoinPending = 4;

    static constexpr size_t kMissionLobbyRoot = 0x60;
    static constexpr size_t kMissionLobbyCategory = 0x64;
    static constexpr uint8_t kMultiplayerCategory = 3;

    static constexpr size_t kCreateRoot = 0x4C;
    static constexpr size_t kCreateCurrentKey = 0x50;
    static constexpr size_t kCreatePendingKey = 0x54;
    static constexpr size_t kCreateMissionId = 0x58;
    static constexpr uint32_t kMaxMissionCycles = 256;

    static constexpr size_t kGroupVectorBegin = 0x5B0;
    static constexpr size_t kGroupVectorEnd = 0x5B4;
    static constexpr size_t kGroupStride = 0x764;
    static constexpr size_t kGroupChannelCount = 0x22;
    static constexpr size_t kGroupChannels = 0x24;
    static constexpr size_t kChannelStride = 0x1D;
    static constexpr size_t kChannelId = 0x00;
    static constexpr size_t kChannelCapacity = 0x0A;
    static constexpr size_t kChannelPopulation = 0x0E;
    // Shipped 0x007F1F50 filters this +0x16 field. It corresponds to
    // tbl_ChannelInfo.Mode; +0x06 is not the channel mode.
    static constexpr size_t kChannelMode = 0x16;
    static constexpr size_t kChannelEnabled = 0x1C;
    static constexpr uint32_t kMaxGroups = 16;
    static constexpr uint32_t kMaxChannels = 64;

    static constexpr size_t kProfileRoom = 0xEC;
    static constexpr size_t kGameAppState = 0x4A0;
    static constexpr size_t kGameAppStateObject = 0x4A4;
    static constexpr size_t kWaitRoomManager = 0x04;
    static constexpr size_t kManagerStateArray = 0x0C;
    static constexpr size_t kStartCommandOffset = 0x30;
    static constexpr int kStartState = 0x0C;
    static constexpr int kQueueBack = 1;

    using GameAppFn = uint8_t*(__cdecl*)();
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using NativeEditSetTextFn =
        void(__thiscall*)(void*, const void*, uint32_t);
    using UiCallbackFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
    using LobbyChannelListChoiceFn =
        void(__thiscall*)(void*);
    using LobbyJoinChannelFn =
        void(__thiscall*)(void*, uint16_t);
    using MissionReplyDispatchFn =
        int(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);
    using MissionCreateUpdateFn =
        void(__thiscall*)(void*);
    using ThreeDRoomHasStateFn =
        bool(__thiscall*)(void*, int, int);
    using ThreeDRoomAddStateFn =
        void(__thiscall*)(void*, int, void*, int, int);
    using IsRoomMasterFn =
        bool(__stdcall*)(void*);

    Result Publish(Result result)
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        return result;
    }

    bool ArmScenario(
        uint16_t mode,
        const char* roomTitle,
        uint32_t missionId)
    {
        if (requestedMode_ == 0) {
            requestedMode_ =
                static_cast<uint8_t>(mode);
            requestedMissionId_ = missionId;
            strcpy_s(
                title_,
                sizeof(title_),
                roomTitle);
            return true;
        }
        return requestedMode_ == mode &&
               requestedMissionId_ == missionId &&
               strcmp(title_, roomTitle) == 0;
    }

    static bool ValidTitle(const char* value)
    {
        if (!value)
            return false;
        const size_t length = strlen(value);
        if (length == 0 ||
            length > SDGO_ROOM_TITLE_MAX_UTF8_BYTES)
            return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char ch =
                static_cast<unsigned char>(value[i]);
            if (ch < 0x20 || ch > 0x7E)
                return false;
        }
        return true;
    }

    Result FailChannelChoice(Result result)
    {
        InterlockedExchange(
            &channelChoiceArmed_,
            0);
        InterlockedExchange(
            &channelChoiceFailed_,
            static_cast<LONG>(result));
        return result;
    }

    Result EnterMissionChannel()
    {
        if (topEntryInvoked_) {
            if (InterlockedCompareExchange(
                    &channelChoiceFailed_, 0, 0) != 0)
                return static_cast<Result>(
                    InterlockedCompareExchange(
                        &channelChoiceFailed_, 0, 0));
            if (InterlockedCompareExchange(
                    &channelRequestSent_, 0, 0) != 0)
                return kWaitingForMissionChannel;
            if (InterlockedCompareExchange(
                    &channelChoiceArmed_, 0, 0) == 0)
                return kChannelRequestRejected;

            auto* owner = static_cast<uint8_t*>(
                InterlockedCompareExchangePointer(
                    &topOwner_,
                    nullptr,
                    nullptr));
            if (!owner ||
                *reinterpret_cast<void**>(owner) !=
                    lobbyUiVtable_)
                return FailChannelChoice(
                    kChannelRequestRejected);
            const uint32_t pageMode =
                *reinterpret_cast<uint32_t*>(
                    owner + kTopPageMode);
            const uint32_t requestState =
                *reinterpret_cast<uint32_t*>(
                    owner + kTopRequestState);
            InterlockedExchange(
                &channelPageMode_,
                static_cast<LONG>(pageMode));
            InterlockedExchange(
                &channelRequestState_,
                static_cast<LONG>(requestState));
            if (pageMode != kMissionPageMode ||
                requestState !=
                    kChannelListPending)
                return FailChannelChoice(
                    kChannelRequestRejected);

            const LONG polls =
                InterlockedIncrement(
                    &channelWaitPolls_);
            if (polls >
                kMaxChannelListWaitPolls) {
                InterlockedExchange(
                    &channelScanStatus_,
                    kChannelScanReplyTimedOut);
                return FailChannelChoice(
                    kChannelRequestRejected);
            }
            return kEnteringMissionChannel;
        }

        void* top = ResolveNamedService(
            L"upmenu",
            lobbyUiVtable_);
        if (!top)
            return kTopMenuMismatch;
        auto* bytes =
            static_cast<uint8_t*>(top);
        if (*reinterpret_cast<uint32_t*>(
                bytes + kTopRequestState) != 0)
            return kWaitingForMissionChannel;

        void* root =
            *reinterpret_cast<void**>(
                bytes + kTopRoot);
        void* cached =
            *reinterpret_cast<void**>(
                bytes + kTopSingleButton);
        void* button = FindControl(
            root,
            L"SingleBtn",
            kButtonKind);
        if (!ValidateControl(
                root,
                button,
                L"SingleBtn",
                true) ||
            button != cached)
            return kTopMenuControlMismatch;

        InterlockedExchange(
            &channelChoiceArmed_, 1);
        InterlockedExchange(
            &channelScanStatus_,
            kChannelScanWaitingForReply);
        InterlockedExchange(&channelWaitPolls_, 0);
        InterlockedExchange(&channelListReady_, 0);
        topEntryInvoked_ = true;
        InterlockedExchangePointer(
            &topOwner_,
            top);
        lobbyUiCallback_(
            top,
            kButtonEvent,
            static_cast<uint8_t*>(button) +
                kControlName,
            button);
        if (*reinterpret_cast<uint32_t*>(
                bytes + kTopPageMode) !=
                kMissionPageMode ||
            *reinterpret_cast<uint32_t*>(
                bytes + kTopRequestState) !=
                kChannelListPending) {
            InterlockedExchange(
                &channelChoiceArmed_, 0);
            InterlockedExchange(
                &channelChoiceFailed_,
                kChannelRequestRejected);
            return kChannelRequestRejected;
        }
        InterlockedExchange(
            &channelPageMode_,
            static_cast<LONG>(
                *reinterpret_cast<uint32_t*>(
                    bytes + kTopPageMode)));
        InterlockedExchange(
            &channelRequestState_,
            static_cast<LONG>(
                *reinterpret_cast<uint32_t*>(
                    bytes + kTopRequestState)));
        return kEnteringMissionChannel;
    }

    Result DriveMissionLobby(void* lobby)
    {
        if (!lobby ||
            *reinterpret_cast<void**>(lobby) !=
                missionLobbyVtable_)
            return kMissionLobbyMismatch;
        auto* bytes =
            static_cast<uint8_t*>(lobby);
        void* root =
            *reinterpret_cast<void**>(
                bytes + kMissionLobbyRoot);
        if (!root)
            return kWaitingForMissionLobby;

        if (bytes[kMissionLobbyCategory] !=
            kMultiplayerCategory) {
            void* radio = FindControl(
                root,
                L"MultiRadio",
                kRadioKind);
            if (!ValidateControl(
                    root,
                    radio,
                    L"MultiRadio",
                    true))
                return kMissionLobbyControlMismatch;
            missionLobbyCallback_(
                lobby,
                kRadioEvent,
                static_cast<uint8_t*>(radio) +
                    kControlName,
                radio);
            if (bytes[kMissionLobbyCategory] !=
                kMultiplayerCategory)
                return kMissionLobbyMismatch;
            return kSelectingMultiplayer;
        }

        void* dialog = ResolveUniqueByVtable(
            missionCreateVtable_);
        if (dialog && !makeRoomInvoked_)
            return kCreateDialogMismatch;
        if (!dialog) {
            if (makeRoomInvoked_)
                return kWaitingForCreateDialog;
            void* button = FindControl(
                root,
                L"MakeRoomBtn",
                kButtonKind);
            if (!ValidateControl(
                    root,
                    button,
                    L"MakeRoomBtn",
                    true))
                return kMissionLobbyControlMismatch;
            makeRoomInvoked_ = true;
            missionLobbyCallback_(
                lobby,
                kButtonEvent,
                static_cast<uint8_t*>(button) +
                    kControlName,
                button);
            return kOpeningCreateDialog;
        }
        return DriveCreateDialog(dialog);
    }

    Result InspectCreateControl(void* root,void* control,const wchar_t* name,uint32_t kind)
    {
        createControlKind_=kind;createControlFlags_=0;
        if(!control)return kWaitingForCreateDialog;
        const auto* bytes=static_cast<const uint8_t*>(control);
        createControlFlags_=1u |
            (*reinterpret_cast<void* const*>(bytes+kControlParent)==root ? 2u : 0u) |
            (ValidateControl(root,control,name,false) ? 4u : 0u) |
            (bytes[kControlVisible] ? 8u : 0u) | (bytes[kControlEnabled] ? 16u : 0u);
        if((createControlFlags_&7u)!=7u)return kCreateDialogMismatch;
        return (createControlFlags_&31u)==31u ? kRoomReady : kWaitingForCreateDialog;
    }

    Result DriveCreateDialog(void* dialog)
    {
        if (!dialog ||
            *reinterpret_cast<void**>(dialog) !=
                missionCreateVtable_)
            return kCreateDialogMismatch;
        createDialogObserved_ = true;
        auto* bytes =
            static_cast<uint8_t*>(dialog);
        void* root =
            *reinterpret_cast<void**>(
                bytes + kCreateRoot);
        if (!root)
            return kWaitingForCreateDialog;

        const uint32_t currentKey =
            *reinterpret_cast<uint32_t*>(
                bytes + kCreateCurrentKey);
        const uint32_t pendingKey =
            *reinterpret_cast<uint32_t*>(
                bytes + kCreatePendingKey);
        const uint32_t missionId =
            *reinterpret_cast<uint32_t*>(
                bytes + kCreateMissionId);
        selectedMissionKey_ = currentKey;
        selectedMissionId_ = missionId;
        if (!currentKey || !pendingKey ||
            !missionId)
            return kMissionDescriptorMismatch;
        if (currentKey != pendingKey)
            return kWaitingForMissionRefresh;

        const uint8_t actualMode =
            missionId <= kCreateMissionThreshold
                ? 5
                : 8;
        if (actualMode != requestedMode_ ||
            (requestedMissionId_ != 0 &&
             missionId != requestedMissionId_)) {
            if (initialMissionKey_ == 0)
                initialMissionKey_ = currentKey;
            else if (currentKey == initialMissionKey_ &&
                     missionCycleCount_ != 0)
                return kRequestedMissionUnavailable;
            if (missionCycleCount_ >=
                kMaxMissionCycles)
                return kRequestedMissionUnavailable;

            void* right = FindControl(
                root,
                L"MapRightBtn",
                kButtonKind);
            const auto rightReady=InspectCreateControl(root,right,L"MapRightBtn",1);
            if(rightReady!=kRoomReady)return rightReady;
            ++missionCycleCount_;
            missionCreateCallback_(
                dialog,
                kButtonEvent,
                static_cast<uint8_t*>(right) +
                    kControlName,
                right);
            const uint32_t afterPending =
                *reinterpret_cast<uint32_t*>(
                    bytes + kCreatePendingKey);
            if (!afterPending ||
                afterPending == currentKey)
                return kRequestedMissionUnavailable;
            return kCyclingMission;
        }

        if (prepareOnly_) {
            void* edit=FindControl(root,L"GameRoomNameEdit",kEditKind);
            const auto ready=InspectCreateControl(root,edit,L"GameRoomNameEdit",2);
            return ready==kRoomReady ? kCreateDialogReady : ready;
        }
        if (!titleSet_) {
            void* edit = FindControl(
                root,
                L"GameRoomNameEdit",
                kEditKind);
            const auto editReady=InspectCreateControl(root,edit,L"GameRoomNameEdit",2);
            if(editReady!=kRoomReady)return editReady;
            wchar_t wideTitle[
                SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
            const size_t length = strlen(title_);
            for (size_t i = 0; i < length; ++i)
                wideTitle[i] =
                    static_cast<unsigned char>(
                        title_[i]);

            alignas(4) uint8_t nativeTitle[
                kNativeWStringSize] = {};
            bool constructed = false;
            __try {
                constructed =
                    nativeWStringCtor_(
                        nativeTitle,
                        wideTitle) != nullptr;
                if (constructed)
                    nativeEditSetText_(
                        edit,
                        nativeTitle,
                        0);
            } __finally {
                if (constructed)
                    nativeWStringDtor_(
                        nativeTitle);
            }
            if (!constructed)
                return kTitleActionRejected;
            titleSet_ = true;
            return kSettingRoomTitle;
        }

        if (createAttempted_)
            return InterlockedCompareExchange(
                       &createSubmitted_, 0, 0)
                       ? kWaitingForCreateReply
                       : kCreateTransportRejected;

        void* ok = FindControl(
            root,
            L"SettingOkBtn",
            kButtonKind);
        const auto okReady=InspectCreateControl(root,ok,L"SettingOkBtn",3);
        if(okReady!=kRoomReady)return okReady;

        const NativeActions::TransportObservation before =
            native_->TransportState();
        createAttempted_ = true;
        missionCreateCallback_(
            dialog,
            kButtonEvent,
            static_cast<uint8_t*>(ok) +
                kControlName,
            ok);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        InterlockedExchange(
            &createTransportSequence_,
            static_cast<LONG>(
                after.send_sequence));
        InterlockedExchange(
            &createOpcode_,
            static_cast<LONG>(
                after.opcode));
        if (after.send_sequence ==
                before.send_sequence ||
            after.opcode != kCreateOpcodeValue ||
            after.packet_size !=
                kCreatePacketSize ||
            after.result == 0)
            return kCreateTransportRejected;
        InterlockedExchange(
            &createSubmitGeneration_,
            InterlockedCompareExchange(
                &runGeneration_, 0, 0));
        InterlockedExchange(
            &createSubmitted_, 1);
        return kCreateSubmitted;
    }

    Result PollNativeRoom()
    {
        nativeRoomId_ = 0;
        nativeRoomMode_ = 0;
        nativeRoomReady_ = false;
        localIsHost_ = false;
        InterlockedExchangePointer(
            &startCommandOwner_,
            nullptr);
        __try {
            NativeRoomView room = {};
            const RoomResult result =
                ReadNativeRoom(&room);
            if (result == kRoomNotReady)
                return kWaitingForNativeRoom;
            if (result == kRoomNotHost)
                return kLocalPilotNotHost;
            if (result != kRoomMatched)
                return kNativeRoomMismatch;

            nativeRoomId_ = room.room_id;
            nativeRoomMode_ = room.room_mode;
            nativeRoomReady_ = true;
            localIsHost_ = true;
            InterlockedExchangePointer(
                &startCommandOwner_,
                room.start_command);
            return kRoomReady;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return kNativeFault;
        }
    }

    RoomResult ReadNativeRoom(
        NativeRoomView* output)
    {
        if (!output || !gameApp_ ||
            !profilePointer_ ||
            !isRoomMaster_)
            return kRoomInvalid;
        memset(output, 0, sizeof(*output));

        uint8_t* profile =
            *profilePointer_;
        uint8_t* app = gameApp_();
        if (!profile || !app)
            return kRoomNotReady;
        if (*reinterpret_cast<uint32_t*>(
                app + kGameAppState) !=
            kWaitRoomAppState)
            return kRoomNotReady;

        // This native 3D path owns its packed room in the verified model.
        // profile+EC can become non-null during initialization without being
        // that packed view; never prefer it over the current model getter.
        void* room = nullptr;
        uint8_t* owner =
            *reinterpret_cast<uint8_t**>(
                app + kGameAppStateObject);
        if (!owner)
            return kRoomNotReady;
        if (*reinterpret_cast<void**>(owner) !=
            threeDWaitRoomVtable_)
            return kRoomInvalid;
        uint8_t* manager =
            *reinterpret_cast<uint8_t**>(
                owner + kWaitRoomManager);
        if (!manager ||
            *reinterpret_cast<void**>(manager) !=
                missionThreeDManagerVtable_)
            return kRoomInvalid;
        {
            auto* model=*reinterpret_cast<uint8_t**>(manager+0x38);
            auto* vtable=model ? *reinterpret_cast<uint8_t**>(model) : nullptr;
            if(!vtable || !roomInfo_ || *reinterpret_cast<RoomInfoFn*>(vtable+0x1C)!=roomInfo_)
                return kRoomInvalid;
            room=roomInfo_(model);
            if(!room)return kRoomNotReady;
        }
        uint8_t** states =
            *reinterpret_cast<uint8_t***>(
                manager + kManagerStateArray);
        if (!states)
            return kRoomInvalid;
        uint8_t* mainState = states[0];
        void* startCommand = states[kStartState];
        nativeMainVtable_=mainState ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(mainState))) : 0;
        nativeCommandVtable_=startCommand ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(startCommand))) : 0;
        nativeMainState_=mainState ? *reinterpret_cast<uint32_t*>(mainState+8) : UINT32_MAX;
        if (!mainState || !startCommand ||
            *reinterpret_cast<void**>(mainState) !=
                missionThreeDMainVtable_ ||
            *reinterpret_cast<void**>(startCommand) !=
                threeDStartCommandVtable_)
            return kRoomInvalid;
        if(nativeMainState_!=2)return kRoomNotReady;
        const uint8_t* command =
            static_cast<const uint8_t*>(
                startCommand);
        nativeContextFlags_=(*reinterpret_cast<uint8_t**>(mainState+0x0C)==manager+0x24 ? 1u : 0u) |
            (*reinterpret_cast<uint8_t**>(mainState+0x10)==manager ? 2u : 0u) |
            (*reinterpret_cast<uint8_t* const*>(command+0x0C)==manager+0x24 ? 4u : 0u) |
            (*reinterpret_cast<uint8_t* const*>(command+0x10)==manager ? 8u : 0u);

        const uint32_t roomId =
            *reinterpret_cast<uint32_t*>(room);
        const uint8_t roomMode =
            *(static_cast<uint8_t*>(room) + 5);
        nativeRoomId_=roomId;nativeRoomMode_=roomMode;
        if(nativeContextFlags_!=15u)return kRoomInvalid;
        const uint32_t requestedId =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &requestedRoomId_, 0, 0));
        if(!roomId || !roomMode)return kRoomNotReady;
        if (!requestedId || roomId != requestedId ||
            roomMode != requestedMode_)
            return kRoomInvalid;
        if (!isRoomMaster_(room))
            return kRoomNotHost;

        output->profile = profile;
        output->room = room;
        output->owner = owner;
        output->manager = manager;
        output->main_state = mainState;
        output->start_command = startCommand;
        output->room_id = roomId;
        output->room_mode = roomMode;
        return kRoomMatched;
    }

    void* ResolveNamedService(
        const wchar_t* name,
        void* expectedVtable) const
    {
        if (!name || !expectedVtable ||
            !serviceManagedPointer_ ||
            !serviceFindByName_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;
        void* manager =
            *serviceManagedPointer_;
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
                    descriptor +
                        sizeof(uint32_t),
                    name) != nullptr;
            if (constructed)
                service = serviceFindByName_(
                    manager,
                    descriptor);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    descriptor +
                    sizeof(uint32_t));
        }
        if (!service ||
            *reinterpret_cast<void**>(service) !=
                expectedVtable)
            return nullptr;
        return service;
    }

    void* ResolveUniqueByVtable(
        void* expectedVtable) const
    {
        if (!expectedVtable ||
            !serviceManagedPointer_)
            return nullptr;
        uint8_t* manager =
            static_cast<uint8_t*>(
                *serviceManagedPointer_);
        if (!manager)
            return nullptr;
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(
                manager + kServiceListHead);
        if (!head)
            return nullptr;

        uint8_t* node =
            *reinterpret_cast<uint8_t**>(
                head + kServiceNodeNext);
        void* matched = nullptr;
        uint32_t visited = 0;
        while (node && node != head &&
               visited++ < kMaxServiceNodes) {
            void* service =
                *reinterpret_cast<void**>(
                    node + kServiceNodeObject);
            if (service &&
                *reinterpret_cast<void**>(
                    service) ==
                    expectedVtable) {
                if (matched && matched != service)
                    return nullptr;
                matched = service;
            }
            node =
                *reinterpret_cast<uint8_t**>(
                    node + kServiceNodeNext);
        }
        if (!node || visited >=
                         kMaxServiceNodes)
            return nullptr;
        return matched;
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
            if (constructed)
                control = nativeUiFindControl_(
                    root,
                    nativeName,
                    kind);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    nativeName);
        }
        return control;
    }

    static bool ValidateControl(
        void* root,
        const void* control,
        const wchar_t* expected,
        bool requireInteractive)
    {
        if (!root || !control || !expected)
            return false;
        const uint8_t* bytes =
            static_cast<const uint8_t*>(
                control);
        if (*reinterpret_cast<void* const*>(
                bytes + kControlParent) != root)
            return false;
        if (requireInteractive &&
            (!bytes[kControlVisible] ||
             !bytes[kControlEnabled]))
            return false;

        const uint8_t* nativeName =
            bytes + kControlName;
        const uint32_t size =
            *reinterpret_cast<const uint32_t*>(
                nativeName +
                kNativeStringSize);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                nativeName +
                kNativeStringCapacity);
        const size_t expectedSize =
            wcslen(expected);
        if (size != expectedSize ||
            size > 63)
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
               wmemcmp(
                   value,
                   expected,
                   size) == 0;
    }

    static bool SlotMatches(
        void* vtable,
        size_t slot,
        void* expected)
    {
        return vtable && expected &&
               static_cast<void**>(
                   vtable)[slot] ==
                   expected;
    }

    void PublishChannelScan(
        const ChannelScan& scan)
    {
        InterlockedExchange(
            &channelScanStatus_,
            static_cast<LONG>(scan.status));
        InterlockedExchange(
            &advertisedGroupCount_,
            static_cast<LONG>(scan.group_count));
        InterlockedExchange(
            &advertisedRawCount_,
            static_cast<LONG>(scan.raw_count));
        InterlockedExchange(
            &advertisedMode2Count_,
            static_cast<LONG>(scan.mode2_count));
        InterlockedExchange(
            &advertisedEligibleCount_,
            static_cast<LONG>(scan.eligible_count));
        InterlockedExchange(
            &rejectedZeroIdCount_,
            static_cast<LONG>(
                scan.rejected_zero_id));
        InterlockedExchange(
            &rejectedNonMode2Count_,
            static_cast<LONG>(
                scan.rejected_non_mode2));
        InterlockedExchange(
            &rejectedDisabledCount_,
            static_cast<LONG>(
                scan.rejected_disabled));
        InterlockedExchange(
            &rejectedZeroCapacityCount_,
            static_cast<LONG>(
                scan.rejected_zero_capacity));
        InterlockedExchange(
            &rejectedFullCount_,
            static_cast<LONG>(
                scan.rejected_full));
    }

    ChannelScanResult FindMissionChannel(
        void* lobby,
        uint32_t* targetGroup,
        uint16_t* targetChannel,
        ChannelScan* scan) const
    {
        if (!scan)
            return kChannelScanInvalid;
        memset(scan, 0, sizeof(*scan));
        scan->status =
            kChannelScanProfileUnavailable;
        if (!lobby || !targetGroup ||
            !targetChannel ||
            !profilePointer_)
            return kChannelScanInvalid;
        uint8_t* profile =
            *profilePointer_;
        if (!profile)
            return kChannelScanInvalid;
        const uint32_t groupIndex =
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(lobby) +
                kTopTargetGroup);
        const uint8_t* begin =
            *reinterpret_cast<uint8_t**>(
                profile + kGroupVectorBegin);
        const uint8_t* end =
            *reinterpret_cast<uint8_t**>(
                profile + kGroupVectorEnd);
        if (!begin || !end) {
            scan->status =
                kChannelScanVectorUnavailable;
            return kChannelScanInvalid;
        }
        if (end < begin) {
            scan->status =
                kChannelScanVectorMalformed;
            return kChannelScanInvalid;
        }
        const uintptr_t bytes =
            reinterpret_cast<uintptr_t>(end) -
            reinterpret_cast<uintptr_t>(begin);
        if (bytes % kGroupStride != 0) {
            scan->status =
                kChannelScanVectorMalformed;
            return kChannelScanInvalid;
        }
        const uint32_t groupCount =
            static_cast<uint32_t>(
                bytes / kGroupStride);
        scan->group_count = groupCount;
        if (!groupCount ||
            groupCount > kMaxGroups ||
            groupIndex >= groupCount) {
            scan->status =
                kChannelScanGroupOutOfRange;
            return kChannelScanInvalid;
        }
        const uint8_t* group =
            begin + groupIndex * kGroupStride;
        const uint32_t channelCount =
            *reinterpret_cast<const uint16_t*>(
                group + kGroupChannelCount);
        scan->raw_count = channelCount;
        if (channelCount > kMaxChannels) {
            scan->status =
                kChannelScanChannelCountInvalid;
            return kChannelScanInvalid;
        }
        scan->status = kChannelScanReady;
        bool found = false;
        for (uint32_t i = 0;
             i < channelCount;
             ++i) {
            const uint8_t* row =
                group + kGroupChannels +
                i * kChannelStride;
            const uint16_t channel =
                *reinterpret_cast<
                    const uint16_t*>(
                    row + kChannelId);
            const uint32_t mode =
                *reinterpret_cast<
                    const uint32_t*>(
                    row + kChannelMode);
            const uint32_t capacity =
                *reinterpret_cast<
                    const uint32_t*>(
                    row + kChannelCapacity);
            const uint32_t population =
                *reinterpret_cast<
                    const uint32_t*>(
                    row + kChannelPopulation);
            if (mode ==
                kMissionChannelMode)
                ++scan->mode2_count;
            if (!channel) {
                ++scan->rejected_zero_id;
                continue;
            }
            if (mode !=
                kMissionChannelMode) {
                ++scan->rejected_non_mode2;
                continue;
            }
            if (!row[kChannelEnabled]) {
                ++scan->rejected_disabled;
                continue;
            }
            if (capacity == 0) {
                ++scan->rejected_zero_capacity;
                continue;
            }
            if (population >= capacity) {
                ++scan->rejected_full;
                continue;
            }
            ++scan->eligible_count;
            if (!found) {
                *targetGroup = groupIndex;
                *targetChannel = channel;
                found = true;
            }
        }
        return found
            ? kChannelScanFound
            : kChannelScanNoMatch;
    }

    bool IsAdvertisedMissionChannel(
        uint32_t groupIndex,
        uint16_t targetChannel) const
    {
        if (!targetChannel || !profilePointer_)
            return false;
        uint8_t* profile = *profilePointer_;
        if (!profile)
            return false;
        const uint8_t* begin =
            *reinterpret_cast<uint8_t**>(
                profile + kGroupVectorBegin);
        const uint8_t* end =
            *reinterpret_cast<uint8_t**>(
                profile + kGroupVectorEnd);
        if (!begin || !end || end < begin)
            return false;
        const uintptr_t bytes =
            reinterpret_cast<uintptr_t>(end) -
            reinterpret_cast<uintptr_t>(begin);
        if (bytes % kGroupStride != 0)
            return false;
        const uint32_t groupCount =
            static_cast<uint32_t>(
                bytes / kGroupStride);
        if (!groupCount ||
            groupCount > kMaxGroups ||
            groupIndex >= groupCount)
            return false;
        const uint8_t* group =
            begin + groupIndex * kGroupStride;
        const uint32_t channelCount =
            *reinterpret_cast<const uint16_t*>(
                group + kGroupChannelCount);
        if (!channelCount ||
            channelCount > kMaxChannels)
            return false;
        for (uint32_t i = 0;
             i < channelCount;
             ++i) {
            const uint8_t* row =
                group + kGroupChannels +
                i * kChannelStride;
            if (*reinterpret_cast<
                    const uint16_t*>(
                    row + kChannelId) !=
                targetChannel)
                continue;
            const uint32_t mode =
                *reinterpret_cast<
                    const uint32_t*>(
                    row + kChannelMode);
            const uint32_t capacity =
                *reinterpret_cast<
                    const uint32_t*>(
                    row + kChannelCapacity);
            return mode ==
                       kMissionChannelMode &&
                   row[kChannelEnabled] &&
                   capacity != 0;
        }
        return false;
    }

    bool SelectedMissionChannelMatches(
        const NativeActions::OnboardingObservation&
            selected)
    {
        if (InterlockedCompareExchange(
                &channelRequestSent_,
                0,
                0) == 0)
            return false;
        const uint32_t targetGroup =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &targetChannelGroup_,
                    0,
                    0));
        const uint16_t targetChannel =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &targetChannel_,
                    0,
                    0));
        return selected.channel_group ==
                   targetGroup &&
               selected.channel_id ==
                   targetChannel &&
               IsAdvertisedMissionChannel(
                   targetGroup,
                   targetChannel);
    }

    static void __fastcall
    ChooseAdvertisedMissionChannel(
        void* self,
        void*)
    {
        MissionFlowActions* actions =
            active_;
        if (!actions ||
            !actions->lobbyChannelListChoice_)
            return;
        if (InterlockedCompareExchange(
                &actions->channelChoiceArmed_,
                0,
                0) == 0) {
            if (self ==
                    InterlockedCompareExchangePointer(
                        &actions->topOwner_,
                        nullptr,
                        nullptr) &&
                InterlockedCompareExchange(
                    &actions->channelChoiceFailed_,
                    0,
                    0) != 0)
                return;
            actions->lobbyChannelListChoice_(
                self);
            return;
        }

        bool ownerMatched = false;
        __try {
            ownerMatched =
                self &&
                self ==
                    InterlockedCompareExchangePointer(
                        &actions->topOwner_,
                        nullptr,
                        nullptr) &&
                *reinterpret_cast<void**>(self) ==
                    actions->lobbyUiVtable_ &&
                *reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(self) +
                    kTopPageMode) ==
                    kMissionPageMode &&
                *reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(self) +
                    kTopRequestState) ==
                    kChannelListPending;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ownerMatched = false;
        }
        if (!ownerMatched) {
            actions->lobbyChannelListChoice_(
                self);
            return;
        }

        InterlockedIncrement(
            &actions->channelChoiceCallbacks_);
        InterlockedExchange(
            &actions->channelListReady_,
            1);
        InterlockedExchange(
            &actions->channelPageMode_,
            static_cast<LONG>(
                *reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(self) +
                    kTopPageMode)));
        InterlockedExchange(
            &actions->channelRequestState_,
            static_cast<LONG>(
                *reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(self) +
                    kTopRequestState)));
        uint32_t group = 0;
        uint16_t channel = 0;
        ChannelScan scan = {};
        scan.status =
            kChannelScanVectorMalformed;
        ChannelScanResult scanResult =
            kChannelScanInvalid;
        __try {
            scanResult =
                actions->FindMissionChannel(
                self,
                &group,
                &channel,
                &scan);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            scan.status =
                kChannelScanVectorMalformed;
            scanResult =
                kChannelScanInvalid;
        }
        actions->PublishChannelScan(scan);
        if (scanResult !=
            kChannelScanFound) {
            InterlockedExchange(
                &actions->channelChoiceArmed_,
                0);
            InterlockedExchange(
                &actions->channelChoiceFailed_,
                scanResult ==
                        kChannelScanNoMatch
                    ? kNoAdvertisedMissionChannel
                    : kChannelRequestRejected);
            return;
        }

        const NativeActions::TransportObservation before =
            actions->native_->TransportState();
        actions->lobbyJoinChannel_(
            self,
            channel);
        const NativeActions::TransportObservation after =
            actions->native_->TransportState();
        InterlockedExchange(
            &actions->channelChoiceArmed_,
            0);
        if (after.send_sequence ==
                before.send_sequence ||
            after.opcode !=
                kJoinChannelOpcode ||
            after.packet_size !=
                kJoinChannelSize ||
            after.result == 0 ||
            *reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(self) +
                kTopRequestState) !=
                kJoinPending) {
            InterlockedExchange(
                &actions->channelChoiceFailed_,
                kChannelRequestRejected);
            return;
        }
        InterlockedExchange(
            &actions->targetChannelGroup_,
            static_cast<LONG>(group));
        InterlockedExchange(
            &actions->targetChannel_,
            static_cast<LONG>(channel));
        InterlockedExchange(
            &actions->channelRequestSent_,
            1);
    }

    static int __fastcall
    ObserveMissionCreateReply(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        MissionFlowActions* actions =
            active_;
        if (!actions ||
            !actions->missionLobbyReplyDispatch_)
            return 0;

        const bool armed =
            InterlockedCompareExchange(
                &actions->createSubmitted_,
                0,
                0) != 0;
        const bool generationMatched =
            InterlockedCompareExchange(
                &actions->createSubmitGeneration_,
                0,
                0) ==
            InterlockedCompareExchange(
                &actions->runGeneration_,
                0,
                0);
        const bool ownerMatched =
            self ==
            InterlockedCompareExchangePointer(
                &actions->missionLobbyOwner_,
                nullptr,
                nullptr);
        const bool isCreateReply =
            armed && generationMatched &&
            ownerMatched &&
            (opcode & 0xFFFF) ==
                kCreateReplyOpcode &&
            packet;
        uint32_t roomId = 0;
        NativeActions::TransportObservation before = {};
        if (isCreateReply) {
            before =
                actions->native_->TransportState();
            __try {
                roomId =
                    *reinterpret_cast<
                        const uint32_t*>(
                        static_cast<
                            const uint8_t*>(
                            packet) +
                        6);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                roomId = 0;
            }
        }

        const int result =
            actions->missionLobbyReplyDispatch_(
                self,
                a,
                b,
                c,
                opcode,
                packet);
        if (!isCreateReply)
            return result;

        const NativeActions::TransportObservation after =
            actions->native_->TransportState();
        if (!roomId ||
            after.send_sequence ==
                before.send_sequence ||
            after.opcode !=
                kRoomDetailsOpcode ||
            after.packet_size !=
                kRoomDetailsSize ||
            after.result == 0) {
            InterlockedExchange(
                &actions->createReplyConsumed_,
                0);
            InterlockedExchange(
                &actions->createReplyFailed_,
                1);
            InterlockedExchange(
                &actions->requestedRoomId_,
                0);
            return result;
        }
        InterlockedExchange(
            &actions->requestedRoomId_,
            static_cast<LONG>(roomId));
        InterlockedExchange(
            &actions->createReplyConsumed_,
            1);
        InterlockedExchange(
            &actions->createReplyFailed_,
            0);
        InterlockedIncrement(
            &actions->createReplySequence_);
        return result;
    }

    static int __fastcall
    ObserveMissionStartReply(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        MissionFlowActions* actions =
            active_;
        if (!actions ||
            !actions->missionStartReplyDispatch_)
            return 0;
        // This original Start command is shared by native room types. Record
        // its genuine active-owner success independently of this adapter's
        // optional mission scenario and the PvP-only AI world snapshot.
        bool activeNativeStart=false;
        if ((opcode&0xFFFF)==kStartReplyOpcode && packet) {
            __try {
                auto* app=actions->gameApp_();
                auto* owner=app && *reinterpret_cast<uint32_t*>(app+kGameAppState)==kWaitRoomAppState ?
                    *reinterpret_cast<uint8_t**>(app+kGameAppStateObject) : nullptr;
                auto* manager=owner && *reinterpret_cast<void**>(owner)==actions->threeDWaitRoomVtable_ ?
                    *reinterpret_cast<uint8_t**>(owner+kWaitRoomManager) : nullptr;
                auto** states=manager ? *reinterpret_cast<uint8_t***>(manager+kManagerStateArray) : nullptr;
                auto* command=static_cast<uint8_t*>(self);
                activeNativeStart=command && states && states[kStartState]==command &&
                    *reinterpret_cast<void**>(command)==actions->threeDStartCommandVtable_ &&
                    *reinterpret_cast<uint32_t*>(command+8)==2 &&
                    *reinterpret_cast<uint8_t**>(command+0x0C)==manager+0x24 &&
                    *reinterpret_cast<uint8_t**>(command+0x10)==manager &&
                    *reinterpret_cast<const uint16_t*>(packet)==kStartReplyOpcode;
            } __except(EXCEPTION_EXECUTE_HANDLER) {activeNativeStart=false;}
        }
        const bool owned =
            InterlockedCompareExchange(
                &actions->startQueued_,
                0,
                0) != 0 &&
            self ==
                InterlockedCompareExchangePointer(
                    &actions->startCommandOwner_,
                    nullptr,
                    nullptr) && packet;
        if(owned) {
            InterlockedExchange(&actions->lastStartReplyOpcode_,opcode&0xFFFF);
            if((opcode&0xFFFF)==0x07B5) {
                __try { InterlockedExchange(&actions->lastStartReplyStatus_,
                    *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(packet)+6)); }
                __except(EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&actions->lastStartReplyStatus_,UINT16_MAX); }
            }
            const auto sent=actions->native_->TransportState();
            if(sent.send_sequence>static_cast<uint32_t>(InterlockedCompareExchange(&actions->startTransportBaseline_,0,0)) &&
                sent.opcode==kStartRequestOpcode && sent.packet_size==kStartRequestSize && sent.result)
                InterlockedExchange(&actions->startRequestSent_,1);
        }
        bool matched=owned && (opcode&0xFFFF)==kStartReplyOpcode &&
            InterlockedCompareExchange(&actions->startRequestSent_,0,0)!=0;
        if (matched) {
            __try {
                const uint8_t* command =
                    static_cast<const uint8_t*>(
                        self);
                uint8_t* manager =
                    *reinterpret_cast<
                        uint8_t* const*>(
                        command + 0x10);
                matched =
                    manager &&
                    *reinterpret_cast<
                        const uint32_t*>(
                        command + 8) == 2 &&
                    *reinterpret_cast<
                        uint8_t* const*>(
                        command + 0x0C) ==
                        manager + 0x24;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                matched = false;
            }
        }
        const int result =
            actions->missionStartReplyDispatch_(
                self,
                a,
                b,
                c,
                opcode,
                packet);
        if (activeNativeStart) {
            __try {
                if (*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self)+8)==3)
                    InterlockedIncrement(&actions->nativeStartConsumedSequence_);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        bool consumed = false;
        if (matched) {
            __try {
                consumed =
                    *reinterpret_cast<
                        const uint32_t*>(
                        static_cast<
                            const uint8_t*>(
                            self) +
                        8) == 3;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                consumed = false;
            }
        }
        if (consumed) {
            InterlockedExchange(
                &actions->battleStartConsumed_,
                1);
            InterlockedIncrement(
                &actions->startReplySequence_);
        }
        return result;
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_ && api_->Symbol
            ? reinterpret_cast<Fn>(
                  api_->Symbol(mod_, name))
            : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    GameAppFn gameApp_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    NativeEditSetTextFn nativeEditSetText_ = nullptr;

    void* lobbyUiVtable_ = nullptr;
    UiCallbackFn lobbyUiCallback_ = nullptr;
    LobbyChannelListChoiceFn lobbyChannelListChoice_ = nullptr;
    LobbyJoinChannelFn lobbyJoinChannel_ = nullptr;
    void* missionLobbyVtable_ = nullptr;
    UiCallbackFn missionLobbyCallback_ = nullptr;
    MissionReplyDispatchFn missionLobbyPacketDispatch_ = nullptr;
    MissionReplyDispatchFn missionLobbyReplyDispatch_ = nullptr;
    void* missionCreateVtable_ = nullptr;
    UiCallbackFn missionCreateCallback_ = nullptr;
    MissionCreateUpdateFn missionCreateUpdate_ = nullptr;
    void* threeDWaitRoomVtable_ = nullptr;
    void* threeDStartCommandVtable_ = nullptr;
    ThreeDRoomHasStateFn threeDRoomHasState_ = nullptr;
    ThreeDRoomAddStateFn threeDRoomAddState_ = nullptr;
    void* missionThreeDManagerVtable_ = nullptr;
    void* missionThreeDMainVtable_ = nullptr;
    using MissionStartBusinessFn=void(__thiscall*)(void*);
    MissionStartBusinessFn missionStartBusiness_=nullptr;
    MissionReplyDispatchFn missionStartReplyDispatch_ = nullptr;
    using RoomInfoFn=uint8_t*(__thiscall*)(uint8_t*);
    RoomInfoFn roomInfo_=nullptr;
    IsRoomMasterFn isRoomMaster_ = nullptr;

    bool bound_ = false;
    uint8_t requestedMode_ = 0;
    uint32_t requestedMissionId_ = 0;
    char title_[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
    bool topEntryInvoked_ = false;
    bool makeRoomInvoked_ = false;
    bool createDialogObserved_ = false;
    bool prepareOnly_ = false;
    uint32_t createControlKind_=0,createControlFlags_=0;
    uint32_t nativeMainVtable_=0,nativeCommandVtable_=0,nativeMainState_=UINT32_MAX;
    uint32_t nativeContextFlags_=0;
    bool titleSet_ = false;
    bool createAttempted_ = false;
    uint32_t initialMissionKey_ = 0;
    uint32_t missionCycleCount_ = 0;
    uint32_t selectedMissionId_ = 0;
    uint32_t selectedMissionKey_ = 0;
    uint32_t nativeRoomId_ = 0;
    uint8_t nativeRoomMode_ = 0;
    bool nativeRoomReady_ = false;
    bool localIsHost_ = false;

    mutable volatile LONG lastResult_ = kUnbound;
    volatile LONG runGeneration_ = 0;
    volatile LONG channelChoiceArmed_ = 0;
    volatile LONG channelChoiceFailed_ = 0;
    volatile LONG channelRequestSent_ = 0;
    volatile LONG targetChannelGroup_ = 0;
    volatile LONG targetChannel_ = 0;
    volatile LONG channelScanStatus_ =
        kChannelScanNotRequested;
    volatile LONG channelWaitPolls_ = 0;
    volatile LONG channelChoiceCallbacks_ = 0;
    volatile LONG channelPageMode_ = 0;
    volatile LONG channelRequestState_ = 0;
    volatile LONG channelListReady_ = 0;
    volatile LONG advertisedGroupCount_ = 0;
    volatile LONG advertisedRawCount_ = 0;
    volatile LONG advertisedMode2Count_ = 0;
    volatile LONG advertisedEligibleCount_ = 0;
    volatile LONG rejectedZeroIdCount_ = 0;
    volatile LONG rejectedNonMode2Count_ = 0;
    volatile LONG rejectedDisabledCount_ = 0;
    volatile LONG rejectedZeroCapacityCount_ = 0;
    volatile LONG rejectedFullCount_ = 0;
    volatile LONG createTransportSequence_ = 0;
    volatile LONG createSubmitGeneration_ = 0;
    volatile LONG createOpcode_ = 0;
    volatile LONG createSubmitted_ = 0;
    volatile LONG createReplySequence_ = 0;
    volatile LONG createReplyConsumed_ = 0;
    volatile LONG createReplyFailed_ = 0;
    volatile LONG requestedRoomId_ = 0;
    volatile LONG startTransportBaseline_ = 0;
    volatile LONG startQueued_ = 0;
    volatile LONG startRequestSent_ = 0;
    volatile LONG startReplySequence_ = 0;
    volatile LONG lastStartReplyOpcode_=0,lastStartReplyStatus_=0;
    mutable volatile LONG nativeStartConsumedSequence_=0;
    volatile LONG battleStartConsumed_ = 0;
    void* volatile topOwner_ = nullptr;
    void* volatile missionLobbyOwner_ = nullptr;
    void* volatile startCommandOwner_ = nullptr;

    inline static MissionFlowActions* active_ = nullptr;
};
