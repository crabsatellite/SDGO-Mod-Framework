#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "friend_service_lookup.h"
#include "native_actions.h"

namespace friends {

namespace friend_validation_bridge {

using Callback =
    void(__stdcall*)(const char*, const char*);

inline Callback callback = nullptr;
inline void* resume = nullptr;
inline const DWORD defaultMxcsr = 0x1F80;

inline void __stdcall Dispatch(
    const char* own,
    const char* target) noexcept
{
    if (callback)
        callback(own, target);
}

// Native site 0x0095C1D5 begins with:
//   cmp eax, esi
//   je  0x0095C08B
// The trampoline replays those instructions. Observe equality without
// altering the native registers, flags, stack, x87 or SIMD state.
__declspec(naked) inline void ObserveSelfComparison()
{
    __asm {
        pushfd
        cmp eax, esi
        jne restore_native
        pushad
        mov eax, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        mov [esp + 200h], eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [defaultMxcsr]
        cld
        lea eax, [ebp - 21h]
        push eax
        lea eax, [ebp - 32h]
        push eax
        call Dispatch
        fxrstor [esp]
        mov esp, [esp + 200h]
        popad
        popfd
        jmp native_path
    restore_native:
        popfd
    native_path:
        jmp dword ptr [resume]
    }
}

} // namespace friend_validation_bridge

class FriendActions
{
public:
    enum Operation : uint32_t
    {
        kNone = 0,
        kEnsureInitialized = 1,
        kRefreshList = 2,
        kRequestListPage = 3,
        kAddByName = 4,
        kAcceptPending = 5,
        kRejectPending = 6,
        kOpenConversation = 7,
        kSendMessage = 8,
        kRemoveFriend = 9,
    };

    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kWaitingForMessenger = 2,
        kWaitingForReply = 3,
        kReady = 4,
        kAlreadySatisfied = 5,
        kRequestSent = 6,
        kReplyObserved = 7,

        kBusy = 0x100,
        kInvalidCommand = 0x101,
        kInvalidTarget = 0x102,
        kInvalidBody = 0x103,
        kProfileUnavailable = 0x104,
        kMessengerUnavailable = 0x105,
        kFriendNotFound = 0x106,
        kPendingRequestMissing = 0x107,
        kNativeRequestNotSent = 0x108,
        kNativeFault = 0x109,
        kReplyRejected = 0x10A,
        kCacheMalformed = 0x10B,
        kUnsupported = 0x10C,
    };

    enum LocalValidation : uint32_t
    {
        kLocalValidationNone = 0,
        kLocalValidationAcceptedDuplicate = 1,
        kLocalValidationPendingDuplicate = 2,
        kLocalValidationSelfTarget = 3,
    };

    struct Command
    {
        uint32_t sequence;
        uint32_t operation;
        uint32_t target_id;
        uint32_t page;
        char target_name[17];
        char body[79];
    };

    struct Friend
    {
        uint32_t pilot_id;
        uint32_t presence;
        uint8_t relationship;
        uint8_t conversation_open;
        uint8_t reserved[2];
        char nickname[17];
    };

    struct PendingRequest
    {
        uint32_t sequence;
        uint32_t sender_id;
        uint32_t recipient_id;
        char sender[17];
        char recipient[17];
        uint8_t available;
        uint8_t reserved[3];
    };

    struct Observation
    {
        uint32_t result;
        uint32_t command_sequence;
        uint32_t operation;
        uint32_t target_id;
        uint32_t native_app_state;
        uint32_t profile_pilot_id;
        uint32_t messenger_init_state;
        uint32_t current_page;
        uint32_t friend_count;
        uint32_t conversation_count;
        uint32_t list_page_replies;
        uint32_t list_total_count;
        uint32_t list_entries_seen;
        uint32_t request_transport_sequence;
        uint32_t reply_sequence;
        uint32_t reply_result;
        uint32_t local_validation_code;
        uint16_t request_opcode;
        uint16_t reply_opcode;
        uint16_t reply_status;
        uint8_t request_sent;
        uint8_t reply_consumed;
        uint8_t message_observed;
        uint8_t local_validation_rejected;
        char target_name[17];
        char last_sender[17];
        char last_message[79];
        PendingRequest pending;
        Friend friends[16];
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod, NativeActions* native)
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
        messengerVtable_ =
            Symbol<void*>("friends.messenger_vtable");
        messengerServiceVtable_ =
            Symbol<void*>("friends.messenger_service_vtable");
        addDialogVtable_ =
            Symbol<void*>("friends.add_dialog_vtable");
        pendingDialogVtable_ =
            Symbol<void*>("friends.pending_dialog_vtable");
        initialListRequest_ =
            Symbol<InitialListRequestFn>("friends.initial_list_request");
        messengerDispatch_ =
            Symbol<MessengerDispatchFn>("friends.messenger_dispatch");
        openMessenger_ =
            Symbol<SimpleBusinessFn>("friends.open_messenger");
        openAddDialog_ =
            Symbol<SimpleBusinessFn>("friends.open_add_dialog");
        addFriend_ =
            Symbol<BoolBusinessFn>("friends.add_friend_business");
        acceptedDuplicateValidator_ =
            Symbol<NativeWStringValidatorFn>(
                "friends.accepted_duplicate_validator");
        pendingDuplicateValidator_ =
            Symbol<AnsiValidatorFn>(
                "friends.pending_duplicate_validator");
        acceptPending_ =
            Symbol<SimpleBusinessFn>("friends.accept_pending_business");
        rejectPending_ =
            Symbol<SimpleBusinessFn>("friends.reject_pending_business");
        removeFriend_ =
            Symbol<SimpleBusinessFn>("friends.remove_friend_business");
        deleteDialogCtor_ =
            Symbol<DeleteDialogCtorFn>("friends.delete_dialog_ctor");
        deleteDialogDtor_ =
            Symbol<SimpleBusinessFn>("friends.delete_dialog_dtor");
        openConversation_ =
            Symbol<OpenConversationFn>("friends.open_conversation_business");
        sendMessage_ =
            Symbol<SimpleBusinessFn>("friends.send_message_business");
        uiEditSetText_ =
            Symbol<UiEditSetTextFn>("friends.ui_edit_set_text");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        treeItemByIndex_ =
            Symbol<TreeItemByIndexFn>(
                "friends.tree_item_by_index");
        treeItemData_ =
            Symbol<TreeItemDataFn>("friends.tree_item_data");
        treeSelect_ =
            Symbol<TreeSelectFn>("friends.tree_select");
        openSelectedFriend_ =
            Symbol<SimpleBusinessFn>(
                "friends.open_selected_friend");
        Reset();
        const bool hookSiteAvailable =
            api_ &&
            api_->size >=
                offsetof(SdgoApi, HookSite) +
                    sizeof(api_->HookSite) &&
            api_->HookSite;
        bound_ =
            api_ && api_->HookCode && hookSiteAvailable &&
            mod_ && native_ && playerDataPointer_ &&
            serviceManagedPointer_ && serviceFindByName_ &&
            nativeWStringCtor_ && nativeWStringDtor_ &&
            messengerVtable_ && messengerServiceVtable_ &&
            addDialogVtable_ &&
            pendingDialogVtable_ &&
            initialListRequest_ &&
            messengerDispatch_ && openMessenger_ &&
            openAddDialog_ && addFriend_ &&
            acceptedDuplicateValidator_ &&
            pendingDuplicateValidator_ &&
            acceptPending_ && rejectPending_ && removeFriend_ &&
            deleteDialogCtor_ && deleteDialogDtor_ &&
            openConversation_ && sendMessage_ && uiEditSetText_ &&
            nativeUiFindControl_ && treeItemByIndex_ &&
            treeItemData_ && treeSelect_ && openSelectedFriend_;
        if (!bound_ || (active_ && active_ != this))
            return false;
        active_ = this;
        friend_validation_bridge::callback =
            &ObserveSelfComparisonMatch;
        if (!api_->HookCode(
                mod_,
                "friends.messenger_dispatch",
                reinterpret_cast<void*>(&ObserveMessengerDispatch),
                reinterpret_cast<void**>(&messengerDispatch_)) ||
            !api_->HookCode(
                mod_,
                "friends.accepted_duplicate_validator",
                reinterpret_cast<void*>(&ObserveAcceptedDuplicate),
                reinterpret_cast<void**>(
                    &acceptedDuplicateValidator_)) ||
            !api_->HookCode(
                mod_,
                "friends.pending_duplicate_validator",
                reinterpret_cast<void*>(&ObservePendingDuplicate),
                reinterpret_cast<void**>(
                    &pendingDuplicateValidator_)) ||
            !api_->HookSite(
                mod_,
                "friends.add_self_compare",
                reinterpret_cast<void*>(
                    &friend_validation_bridge::
                        ObserveSelfComparison),
                &friend_validation_bridge::resume)) {
            friend_validation_bridge::callback = nullptr;
            active_ = nullptr;
            bound_ = false;
        }
        return bound_;
    }

    void Reset()
    {
        memset(&observation_, 0, sizeof(observation_));
        memset(&command_, 0, sizeof(command_));
        memset(pendingPacket_, 0, sizeof(pendingPacket_));
        replyBaseline_ = 0;
        matchedReplySequence_ = 0;
        commandPending_ = false;
        commandStarted_ = false;
        messengerOpenRequested_ = false;
        addDialogRequested_ = false;
        localValidationScope_ = false;
        localValidationDialog_ = nullptr;
        localValidationThreadId_ = 0;
        localValidationObservedCode_ = kLocalValidationNone;
        localValidationTargetLength_ = 0;
        localValidationWideTargetLength_ = 0;
        memset(
            localValidationTarget_,
            0,
            sizeof(localValidationTarget_));
        memset(
            localValidationWideTarget_,
            0,
            sizeof(localValidationWideTarget_));
    }

    Observation State() const { return observation_; }

    Result EnsureInitialized()
    {
        Command command = {};
        command.operation = kEnsureInitialized;
        return Submit(command);
    }

    Result Submit(const Command& command)
    {
        if (!bound_)
            return Publish(kUnbound, command);
        if (commandPending_)
            return Publish(kBusy, command);
        if (command.operation == kNone ||
            command.operation > kRemoveFriend)
            return Publish(kInvalidCommand, command);
        command_ = command;
        observation_.request_transport_sequence = 0;
        observation_.reply_result = 0;
        observation_.request_opcode = 0;
        observation_.reply_opcode = 0;
        observation_.reply_status = 0;
        observation_.local_validation_code =
            kLocalValidationNone;
        observation_.request_sent = 0;
        observation_.reply_consumed = 0;
        observation_.local_validation_rejected = 0;
        commandStarted_ = false;
        messengerOpenRequested_ = false;
        addDialogRequested_ = false;
        localValidationObservedCode_ = kLocalValidationNone;
        replyBaseline_ = matchedReplySequence_;
        commandPending_ = true;
        return Publish(kWaitingForMessenger, command);
    }

    void Tick()
    {
        RefreshPassiveState();
        if (!commandPending_)
            return;
        const Result result = Execute(command_);
        if (result != kWaitingForLobby &&
            result != kWaitingForMessenger &&
            result != kWaitingForReply &&
            result != kRequestSent) {
            commandPending_ = false;
            commandStarted_ = false;
        }
    }

private:
    static constexpr uint32_t kLobbyAppState = 6;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kFriendLimit = 16;

    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using InitialListRequestFn = void(__cdecl*)();
    using SimpleBusinessFn = void(__thiscall*)(void*);
    using BoolBusinessFn = bool(__thiscall*)(void*);
    using NativeWStringValidatorFn =
        bool(__thiscall*)(void*, const void*);
    using AnsiValidatorFn =
        bool(__thiscall*)(void*, const char*);
    using DeleteDialogCtorFn =
        void*(__thiscall*)(void*, const void*);
    using OpenConversationFn =
        uint32_t(__thiscall*)(void*, const void*);
    using UiEditSetTextFn =
        void(__thiscall*)(void*, const void*, int);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using TreeItemByIndexFn =
        void*(__thiscall*)(void*, uint32_t);
    using TreeItemDataFn =
        void*(__thiscall*)(void*);
    using TreeSelectFn =
        void(__thiscall*)(void*, void*);
    using MessengerDispatchFn =
        int(__thiscall*)(void*, int, int, int, int, void*);

    static size_t BoundedLength(const char* value, size_t maximum)
    {
        if (!value)
            return maximum + 1;
        for (size_t index = 0; index <= maximum; ++index) {
            if (value[index] == '\0')
                return index;
        }
        return maximum + 1;
    }

    void* ResolveService(
        const wchar_t* name,
        const void* expectedVtable) const
    {
        if (!name || !expectedVtable || !serviceManagedPointer_ ||
            !serviceFindByName_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;
        void* manager = *serviceManagedPointer_;
        if (!manager)
            return nullptr;
        alignas(4) uint8_t descriptor[kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            nativeWStringCtor_(
                descriptor + sizeof(uint32_t),
                name);
            constructed = true;
            service = serviceFindByName_(manager, descriptor);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    descriptor + sizeof(uint32_t));
        }
        if (!service ||
            *reinterpret_cast<void**>(service) != expectedVtable)
            return nullptr;
        return service;
    }

    void* ResolveMessengerService() const
    {
        return ResolveService(
            L"Messenger",
            messengerServiceVtable_);
    }

    void* ResolveMessenger(void* service = nullptr) const
    {
        if (!service)
            service = ResolveMessengerService();
        if (!service)
            return nullptr;
        void* popup =
            ResolveService(L"MessengerPopup", messengerVtable_);
        if (!popup)
            return nullptr;
        __try {
            return *reinterpret_cast<void**>(
                       static_cast<uint8_t*>(service) + 0x48) ==
                    popup
                ? popup
                : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void* ResolveOrOpenMessenger(bool allowOpen)
    {
        void* service = ResolveMessengerService();
        if (!service)
            return nullptr;
        void* popup = ResolveMessenger(service);
        if (popup || !allowOpen || messengerOpenRequested_)
            return popup;
        openMessenger_(service);
        messengerOpenRequested_ = true;
        return ResolveMessenger(service);
    }

    void* ResolveAddDialog() const
    {
        void* manager =
            serviceManagedPointer_
                ? *serviceManagedPointer_
                : nullptr;
        const friend_service_lookup::UniqueService found =
            friend_service_lookup::FindUniqueByVtable(
                manager,
                addDialogVtable_);
        return found.list_valid && found.matches == 1
            ? found.service
            : nullptr;
    }

    static bool FixedNameMatches(
        const char* fixed,
        const char* expected)
    {
        const size_t length = BoundedLength(expected, 16);
        if (!fixed || length == 0 || length > 16)
            return false;
        if (memcmp(fixed, expected, length) != 0)
            return false;
        return length == 16 || fixed[length] == '\0';
    }

    bool PendingCommandMatches(const Command& command) const
    {
        return observation_.pending.available &&
            command.target_id != 0 &&
            command.target_id ==
                observation_.pending.sender_id &&
            FixedNameMatches(
                observation_.pending.sender,
                command.target_name);
    }

    bool PendingDialogMatches(
        void* dialog,
        const Command& command) const
    {
        if (!dialog || !PendingCommandMatches(command))
            return false;
        __try {
            const auto* bytes =
                static_cast<const uint8_t*>(dialog);
            const auto* packet = bytes + 0x60;
            return *reinterpret_cast<void* const*>(bytes + 0x4C) &&
                memcmp(
                    packet,
                    pendingPacket_,
                    sizeof(pendingPacket_)) == 0 &&
                *reinterpret_cast<const uint32_t*>(packet + 2) ==
                    command.target_id &&
                *reinterpret_cast<const uint32_t*>(packet + 6) ==
                    observation_.pending.recipient_id &&
                FixedNameMatches(
                    reinterpret_cast<const char*>(packet + 0x0E),
                    command.target_name) &&
                FixedNameMatches(
                    reinterpret_cast<const char*>(packet + 0x1F),
                    observation_.pending.recipient);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* ResolvePendingDialog(const Command& command) const
    {
        void* manager =
            serviceManagedPointer_
                ? *serviceManagedPointer_
                : nullptr;
        const friend_service_lookup::UniqueService found =
            friend_service_lookup::FindUniqueByVtable(
                manager,
                pendingDialogVtable_);
        if (!found.list_valid || found.matches != 1 ||
            !PendingDialogMatches(found.service, command))
            return nullptr;
        return found.service;
    }

    bool SetEditText(void* edit, const char* text) const
    {
        const size_t length = BoundedLength(text, 78);
        if (!edit || length == 0 || length > 78)
            return false;
        wchar_t wide[79] = {};
        const int wideLength = MultiByteToWideChar(
            CP_ACP,
            MB_ERR_INVALID_CHARS,
            text,
            static_cast<int>(length),
            wide,
            78);
        if (wideLength <= 0 || wideLength > 78)
            return false;
        wide[wideLength] = L'\0';
        alignas(4) uint8_t nativeText[kNativeWStringSize] = {};
        bool constructed = false;
        __try {
            nativeWStringCtor_(nativeText, wide);
            constructed = true;
            uiEditSetText_(edit, nativeText, 0);
        } __finally {
            if (constructed)
                nativeWStringDtor_(nativeText);
        }
        return true;
    }

    void* FindControl(
        void* root,
        const wchar_t* name,
        uint32_t type) const
    {
        if (!root || !name || !nativeUiFindControl_)
            return nullptr;
        alignas(4) uint8_t nativeName[kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            nativeWStringCtor_(nativeName, name);
            constructed = true;
            control =
                nativeUiFindControl_(root, nativeName, type);
        } __finally {
            if (constructed)
                nativeWStringDtor_(nativeName);
        }
        return control;
    }

    Result Execute(const Command& command)
    {
        if (!native_)
            return Publish(kUnbound, command);
        const NativeActions::OnboardingObservation nativeState =
            native_->OnboardingState();
        if (nativeState.native_app_state != kLobbyAppState)
            return Publish(kWaitingForLobby, command);

        void* messenger = ResolveOrOpenMessenger(
            command.operation == kEnsureInitialized);
        if (!messenger)
            return Publish(command.operation==kEnsureInitialized ? kWaitingForMessenger : kMessengerUnavailable, command);

        __try {
            switch (command.operation) {
            case kEnsureInitialized:
                return EnsureMessenger(messenger, command);
            case kRefreshList:
                {
                    Command page = command;
                    page.page = 0;
                    return RequestListPage(page);
                }
            case kRequestListPage:
                return RequestListPage(command);
            case kAddByName:
                return AddFriend(messenger, command);
            case kAcceptPending:
                return RespondToPending(command, true);
            case kRejectPending:
                return RespondToPending(command, false);
            case kOpenConversation:
                return OpenConversation(messenger, command);
            case kSendMessage:
                return SendMessage(messenger, command);
            case kRemoveFriend:
                return Remove(command);
            default:
                return Publish(kInvalidCommand, command);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault, command);
        }
    }

    Result EnsureMessenger(void*, const Command& command)
    {
        // UI layout initialization belongs to the native managed lifecycle.
        // Its vtable+0x3C method immediately dereferences controls returned by
        // vtable+0x58, and cannot be used to probe an unopened layout safely.
        RefreshPassiveState();
        if (observation_.messenger_init_state == 0)
            initialListRequest_();
        RefreshPassiveState();
        return Publish(
            observation_.messenger_init_state == 2
                ? kReady
                : kWaitingForReply,
            command);
    }

    Result RequestListPage(const Command& command)
    {
        if (command.page > UINT16_MAX)
            return Publish(kInvalidTarget, command);
        if (commandStarted_)
            return PollReply(command);
        alignas(4) uint8_t packet[8] = {};
        *reinterpret_cast<uint16_t*>(packet) = 0x0697;
        *reinterpret_cast<uint16_t*>(packet + 6) =
            static_cast<uint16_t>(command.page);
        const NativeActions::TransportObservation before =
            native_->TransportState();
        if (!native_->SendRequest(packet, sizeof(packet)))
            return Publish(kNativeRequestNotSent, command);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        observation_.request_transport_sequence =
            after.send_sequence;
        observation_.request_opcode = after.opcode;
        observation_.request_sent =
            after.send_sequence != before.send_sequence &&
            after.opcode == 0x0697;
        commandStarted_ = observation_.request_sent != 0;
        replyBaseline_ = observation_.reply_sequence;
        return Publish(
            observation_.request_sent
                ? kRequestSent
                : kNativeRequestNotSent,
            command);
    }

    Result AddFriend(void* messenger, const Command& command)
    {
        if (BoundedLength(command.target_name, 16) == 0 ||
            BoundedLength(command.target_name, 16) > 16)
            return Publish(kInvalidTarget, command);
        if (commandStarted_)
            return PollReply(command);
        void* dialog = ResolveAddDialog();
        if (!dialog) {
            if (!addDialogRequested_) {
                openAddDialog_(messenger);
                addDialogRequested_ = true;
            }
            dialog = ResolveAddDialog();
        }
        if (!dialog)
            return Publish(kWaitingForMessenger, command);
        void* root =
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(dialog) + 0x4C);
        void* edit =
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(dialog) + 0x50);
        if (!root || !edit)
            return Publish(kWaitingForMessenger, command);
        if (!SetEditText(edit, command.target_name))
            return Publish(kNativeRequestNotSent, command);
        const NativeActions::TransportObservation before =
            native_->TransportState();
        BeginLocalValidationScope(dialog, command);
        __try {
            (void)addFriend_(dialog);
        } __finally {
            EndLocalValidationScope();
        }
        const NativeActions::TransportObservation after =
            native_->TransportState();
        CommitLocalValidationReceipt(before, after);
        return ArmRequest(command, before, after, 0x0829);
    }

    Result RespondToPending(const Command& command, bool accept)
    {
        if (commandStarted_)
            return PollReply(command);
        if (!observation_.pending.available)
            return Publish(kPendingRequestMissing, command);
        if (!PendingCommandMatches(command))
            return Publish(kInvalidTarget, command);
        void* dialog = ResolvePendingDialog(command);
        if (!dialog)
            return Publish(kWaitingForMessenger, command);
        const NativeActions::TransportObservation before =
            native_->TransportState();
        if (accept)
            acceptPending_(dialog);
        else
            rejectPending_(dialog);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        return ArmRequest(
            command,
            before,
            after,
            accept ? 0x0645 : 0x0737);
    }

    Result OpenConversation(void* messenger, const Command& command)
    {
        void* record = FindFriendRecord(command);
        if (!record)
            return Publish(kFriendNotFound, command);
        void* treeRoot =
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(messenger) + 0x50);
        void* tree = FindControl(treeRoot, L"FriendTree", 0x12);
        if (!tree)
            return Publish(kMessengerUnavailable, command);
        void* item = nullptr;
        const uint32_t count =
            observation_.friend_count < 100
                ? observation_.friend_count
                : 100;
        for (uint32_t index = 0; index < count; ++index) {
            void* candidate = treeItemByIndex_(tree, index);
            if (candidate && treeItemData_(candidate) == record) {
                item = candidate;
                break;
            }
        }
        if (!item)
            return Publish(kFriendNotFound, command);
        treeSelect_(tree, item);
        openSelectedFriend_(messenger);
        RefreshPassiveState();
        uint8_t* data =
            playerDataPointer_ ? *playerDataPointer_ : nullptr;
        return Publish(
            data &&
                    *reinterpret_cast<int32_t*>(data + 0x790) >= 0
                ? kReady
                : kNativeRequestNotSent,
            command);
    }

    Result SendMessage(void* messenger, const Command& command)
    {
        if (BoundedLength(command.body, 78) == 0 ||
            BoundedLength(command.body, 78) > 78)
            return Publish(kInvalidBody, command);
        void* selectedRecord = FindSelectedConversationRecord();
        if (!selectedRecord ||
            !RecordMatchesCommand(selectedRecord, command))
            return Publish(kFriendNotFound, command);
        void* edit =
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(messenger) + 0x84);
        if (!SetEditText(edit, command.body))
            return Publish(kNativeRequestNotSent, command);
        const NativeActions::TransportObservation before =
            native_->TransportState();
        sendMessage_(messenger);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        const Result result =
            ArmRequest(command, before, after, 0x0560);
        if (result == kRequestSent) {
            commandPending_ = false;
            commandStarted_ = false;
        }
        return result;
    }

    Result Remove(const Command& command)
    {
        void* record = FindFriendRecord(command);
        if (!record)
            return Publish(kFriendNotFound, command);
        if (commandStarted_)
            return PollReply(command);
        alignas(4) uint8_t dialog[0x70] = {};
        bool constructed = false;
        const NativeActions::TransportObservation before =
            native_->TransportState();
        __try {
            deleteDialogCtor_(dialog, record);
            constructed = true;
            removeFriend_(dialog);
        } __finally {
            if (constructed)
                deleteDialogDtor_(dialog);
        }
        const NativeActions::TransportObservation after =
            native_->TransportState();
        return ArmRequest(command, before, after, 0x06C2);
    }

    Result ArmRequest(
        const Command& command,
        const NativeActions::TransportObservation& before,
        const NativeActions::TransportObservation& after,
        uint16_t expectedOpcode)
    {
        observation_.request_transport_sequence =
            after.send_sequence;
        observation_.request_opcode = after.opcode;
        observation_.request_sent =
            after.send_sequence != before.send_sequence &&
            after.opcode == expectedOpcode &&
            after.result != 0;
        if (!observation_.request_sent)
            return Publish(kNativeRequestNotSent, command);
        commandStarted_ = true;
        replyBaseline_ = matchedReplySequence_;
        return Publish(kRequestSent, command);
    }

    Result PollReply(const Command& command)
    {
        if (matchedReplySequence_ <= replyBaseline_)
            return Publish(kWaitingForReply, command);
        observation_.reply_consumed = 1;
        return Publish(
            observation_.reply_status == 0
                ? kReplyObserved
                : kReplyRejected,
            command);
    }

    void* FindFriendRecord(const Command& command) const
    {
        uint8_t* data =
            playerDataPointer_ ? *playerDataPointer_ : nullptr;
        if (!data)
            return nullptr;
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(data + 0x7A4);
        const uint32_t count =
            *reinterpret_cast<uint32_t*>(data + 0x7A8);
        if (!head || count > 100)
            return nullptr;
        uint8_t* node =
            *reinterpret_cast<uint8_t**>(head);
        for (uint32_t index = 0;
             index < count && node && node != head;
             ++index) {
            uint8_t* record =
                *reinterpret_cast<uint8_t**>(node + 8);
            if (record) {
                const uint32_t id =
                    *reinterpret_cast<uint32_t*>(record);
                const char* name =
                    reinterpret_cast<const char*>(record + 0x0A);
                if ((command.target_id && id == command.target_id) ||
                    (command.target_name[0] &&
                     strncmp(name, command.target_name, 16) == 0))
                    return record;
            }
            node = *reinterpret_cast<uint8_t**>(node);
        }
        return nullptr;
    }

    static bool RecordMatchesCommand(
        const void* recordValue,
        const Command& command)
    {
        if (!recordValue ||
            (!command.target_id && !command.target_name[0]))
            return false;
        const uint8_t* record =
            static_cast<const uint8_t*>(recordValue);
        if (command.target_id &&
            *reinterpret_cast<const uint32_t*>(record) !=
                command.target_id)
            return false;
        if (command.target_name[0] &&
            strncmp(
                reinterpret_cast<const char*>(record + 0x0A),
                command.target_name,
                16) != 0)
            return false;
        return true;
    }

    void* FindSelectedConversationRecord() const
    {
        uint8_t* data =
            playerDataPointer_ ? *playerDataPointer_ : nullptr;
        if (!data)
            return nullptr;
        const int32_t selected =
            *reinterpret_cast<const int32_t*>(data + 0x790);
        const uint32_t count =
            *reinterpret_cast<const uint32_t*>(data + 0x7B4);
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(data + 0x7B0);
        if (selected < 0 ||
            static_cast<uint32_t>(selected) >= count ||
            count > 100 || !head)
            return nullptr;
        uint8_t* node =
            *reinterpret_cast<uint8_t**>(head);
        for (int32_t index = 0;
             index < selected && node && node != head;
             ++index)
            node = *reinterpret_cast<uint8_t**>(node);
        if (!node || node == head)
            return nullptr;
        uint8_t* payload =
            *reinterpret_cast<uint8_t**>(node + 8);
        return payload
            ? *reinterpret_cast<void**>(payload)
            : nullptr;
    }

    void RefreshPassiveState()
    {
        if (!native_ || !playerDataPointer_)
            return;
        const NativeActions::OnboardingObservation nativeState =
            native_->OnboardingState();
        observation_.native_app_state = nativeState.native_app_state;
        observation_.profile_pilot_id = nativeState.profile_pilot_id;
        memset(observation_.friends, 0, sizeof(observation_.friends));
        observation_.friend_count = 0;
        observation_.conversation_count = 0;
        __try {
            uint8_t* data = *playerDataPointer_;
            if (!data)
                return;
            observation_.messenger_init_state =
                *reinterpret_cast<uint32_t*>(data + 0x788);
            observation_.current_page =
                *reinterpret_cast<uint32_t*>(data + 0x78C);
            const uint32_t count =
                *reinterpret_cast<uint32_t*>(data + 0x7A8);
            observation_.friend_count = count;
            observation_.conversation_count =
                *reinterpret_cast<uint32_t*>(data + 0x7B4);
            uint8_t* head =
                *reinterpret_cast<uint8_t**>(data + 0x7A4);
            if (!head || count > 100) {
                if (count)
                    observation_.result = kCacheMalformed;
                return;
            }
            uint8_t* node =
                *reinterpret_cast<uint8_t**>(head);
            const uint32_t shown =
                count < kFriendLimit
                    ? count
                    : static_cast<uint32_t>(kFriendLimit);
            for (uint32_t index = 0;
                 index < shown && node && node != head;
                 ++index) {
                uint8_t* record =
                    *reinterpret_cast<uint8_t**>(node + 8);
                if (!record) {
                    observation_.result = kCacheMalformed;
                    return;
                }
                Friend& out = observation_.friends[index];
                out.pilot_id =
                    *reinterpret_cast<uint32_t*>(record);
                memcpy(out.nickname, record + 0x0A, 16);
                out.nickname[16] = '\0';
                out.relationship = record[0x1B];
                out.presence =
                    *reinterpret_cast<uint32_t*>(record + 0x1C);
                node = *reinterpret_cast<uint8_t**>(node);
            }
            uint8_t* conversationHead =
                *reinterpret_cast<uint8_t**>(data + 0x7B0);
            uint8_t* conversationNode =
                conversationHead
                    ? *reinterpret_cast<uint8_t**>(
                          conversationHead)
                    : nullptr;
            for (uint32_t conversation = 0;
                 conversation < observation_.conversation_count &&
                 conversationNode &&
                 conversationNode != conversationHead;
                 ++conversation) {
                uint8_t* payload =
                    *reinterpret_cast<uint8_t**>(
                        conversationNode + 8);
                uint8_t* record =
                    payload
                        ? *reinterpret_cast<uint8_t**>(payload)
                        : nullptr;
                if (record) {
                    const uint32_t id =
                        *reinterpret_cast<uint32_t*>(record);
                    for (uint32_t index = 0;
                         index < shown;
                         ++index) {
                        if (observation_.friends[index].pilot_id ==
                            id)
                            observation_.friends[index]
                                .conversation_open = 1;
                    }
                }
                conversationNode =
                    *reinterpret_cast<uint8_t**>(
                        conversationNode);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            observation_.result = kCacheMalformed;
        }
    }

    void BeginLocalValidationScope(
        void* dialog,
        const Command& command)
    {
        localValidationScope_ = false;
        localValidationDialog_ = nullptr;
        localValidationThreadId_ = 0;
        localValidationObservedCode_ = kLocalValidationNone;
        localValidationTargetLength_ =
            BoundedLength(command.target_name, 16);
        localValidationWideTargetLength_ = 0;
        memset(
            localValidationTarget_,
            0,
            sizeof(localValidationTarget_));
        memset(
            localValidationWideTarget_,
            0,
            sizeof(localValidationWideTarget_));
        if (!dialog ||
            localValidationTargetLength_ == 0 ||
            localValidationTargetLength_ > 16)
            return;
        memcpy(
            localValidationTarget_,
            command.target_name,
            localValidationTargetLength_);
        const int wideLength = MultiByteToWideChar(
            CP_ACP,
            MB_ERR_INVALID_CHARS,
            command.target_name,
            static_cast<int>(localValidationTargetLength_),
            localValidationWideTarget_,
            16);
        if (wideLength <= 0 || wideLength > 16)
            return;
        localValidationWideTarget_[wideLength] = L'\0';
        localValidationWideTargetLength_ =
            static_cast<size_t>(wideLength);
        localValidationDialog_ = dialog;
        localValidationThreadId_ = GetCurrentThreadId();
        localValidationScope_ = true;
    }

    void EndLocalValidationScope()
    {
        localValidationScope_ = false;
        localValidationDialog_ = nullptr;
        localValidationThreadId_ = 0;
    }

    void CommitLocalValidationReceipt(
        const NativeActions::TransportObservation& before,
        const NativeActions::TransportObservation& after)
    {
        if (localValidationObservedCode_ !=
                kLocalValidationNone &&
            after.send_sequence == before.send_sequence) {
            observation_.local_validation_rejected = 1;
            observation_.local_validation_code =
                localValidationObservedCode_;
        }
        localValidationObservedCode_ = kLocalValidationNone;
    }

    bool ActiveLocalValidationCall(void* self) const
    {
        return localValidationScope_ &&
            localValidationDialog_ == self &&
            localValidationThreadId_ == GetCurrentThreadId();
    }

    bool ActiveAnsiTargetMatches(const char* target) const
    {
        if (!localValidationScope_ || !target ||
            localValidationTargetLength_ == 0 ||
            localValidationTargetLength_ > 16)
            return false;
        __try {
            for (size_t index = 0;
                 index < localValidationTargetLength_;
                 ++index) {
                if (target[index] !=
                    localValidationTarget_[index])
                    return false;
            }
            return target[localValidationTargetLength_] == '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ActiveNativeWStringTargetMatches(
        const void* value) const
    {
        if (!localValidationScope_ || !value ||
            localValidationWideTargetLength_ == 0 ||
            localValidationWideTargetLength_ > 16)
            return false;
        __try {
            const uint8_t* bytes =
                static_cast<const uint8_t*>(value);
            const uint32_t length =
                *reinterpret_cast<const uint32_t*>(
                    bytes + 0x10);
            const uint32_t capacity =
                *reinterpret_cast<const uint32_t*>(
                    bytes + 0x14);
            if (length != localValidationWideTargetLength_)
                return false;
            const wchar_t* text =
                capacity < 8
                    ? reinterpret_cast<const wchar_t*>(bytes)
                    : *reinterpret_cast<
                        const wchar_t* const*>(bytes);
            if (!text)
                return false;
            for (size_t index = 0; index < length; ++index) {
                if (text[index] !=
                    localValidationWideTarget_[index])
                    return false;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void RecordLocalValidation(LocalValidation code)
    {
        if (localValidationObservedCode_ ==
            kLocalValidationNone)
            localValidationObservedCode_ = code;
    }

    static bool __fastcall ObserveAcceptedDuplicate(
        void* self,
        void*,
        const void* target)
    {
        FriendActions* actions = active_;
        if (!actions || !actions->acceptedDuplicateValidator_)
            return true;
        const bool result =
            actions->acceptedDuplicateValidator_(self, target);
        const DWORD error = GetLastError();
        if (!result &&
            actions->ActiveLocalValidationCall(self) &&
            actions->ActiveNativeWStringTargetMatches(target)) {
            actions->RecordLocalValidation(
                kLocalValidationAcceptedDuplicate);
        }
        SetLastError(error);
        return result;
    }

    static bool __fastcall ObservePendingDuplicate(
        void* self,
        void*,
        const char* target)
    {
        FriendActions* actions = active_;
        if (!actions || !actions->pendingDuplicateValidator_)
            return true;
        const bool result =
            actions->pendingDuplicateValidator_(self, target);
        const DWORD error = GetLastError();
        if (!result &&
            actions->ActiveLocalValidationCall(self) &&
            actions->ActiveAnsiTargetMatches(target)) {
            actions->RecordLocalValidation(
                kLocalValidationPendingDuplicate);
        }
        SetLastError(error);
        return result;
    }

    static void __stdcall ObserveSelfComparisonMatch(
        const char* own,
        const char* target) noexcept
    {
        const DWORD error = GetLastError();
        FriendActions* actions = active_;
        if (actions &&
            actions->localValidationScope_ &&
            actions->localValidationThreadId_ ==
                GetCurrentThreadId() &&
            actions->ActiveAnsiTargetMatches(target)) {
            bool same = true;
            __try {
                for (size_t index = 0; index <= 16; ++index) {
                    if (own[index] != target[index]) {
                        same = false;
                        break;
                    }
                    if (own[index] == '\0')
                        break;
                    if (index == 16)
                        same = false;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                same = false;
            }
            if (same) {
                actions->RecordLocalValidation(
                    kLocalValidationSelfTarget);
            }
        }
        SetLastError(error);
    }

    static int __fastcall ObserveMessengerDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        FriendActions* actions = active_;
        if (!actions || !actions->messengerDispatch_)
            return 0;
        const uint16_t normalized =
            static_cast<uint16_t>(opcode);
        uint16_t status = 0;
        uint32_t total = 0;
        uint32_t entries = 0;
        if (packet) {
            __try {
                const uint8_t* bytes =
                    static_cast<const uint8_t*>(packet);
                if (normalized == 0x0829) {
                    memcpy(
                        actions->pendingPacket_,
                        bytes,
                        sizeof(actions->pendingPacket_));
                    PendingRequest& pending =
                        actions->observation_.pending;
                    ++pending.sequence;
                    pending.sender_id =
                        *reinterpret_cast<const uint32_t*>(
                            bytes + 2);
                    pending.recipient_id =
                        *reinterpret_cast<const uint32_t*>(
                            bytes + 6);
                    memcpy(pending.sender, bytes + 0x0E, 16);
                    memcpy(pending.recipient, bytes + 0x1F, 16);
                    pending.sender[16] = '\0';
                    pending.recipient[16] = '\0';
                    pending.available = 1;
                } else if (normalized == 0x04B3) {
                    entries = bytes[6];
                    total =
                        *reinterpret_cast<const uint32_t*>(
                            bytes + 7);
                } else if (normalized == 0x0737) {
                    status =
                        *reinterpret_cast<const uint16_t*>(
                            bytes + 0x30);
                } else if (normalized == 0x0953) {
                    memcpy(
                        actions->observation_.last_sender,
                        bytes + 0x0B,
                        16);
                    actions->observation_.last_sender[16] =
                        '\0';
                    size_t length = 0;
                    while (length < 78 &&
                           bytes[0x1D + length] != 0)
                        ++length;
                    memcpy(
                        actions->observation_.last_message,
                        bytes + 0x1D,
                        length);
                    actions->observation_
                        .last_message[length] = '\0';
                } else if (normalized == 0x0A45) {
                    status =
                        *reinterpret_cast<const uint16_t*>(
                            bytes + 6);
                } else if (
                    normalized == 0x05A5 ||
                    normalized == 0x05D0 ||
                    normalized == 0x0946) {
                    status =
                        *reinterpret_cast<const uint16_t*>(
                            bytes + 6);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                status = UINT16_MAX;
            }
        }
        const int result = actions->messengerDispatch_(
            self,
            a,
            b,
            c,
            opcode,
            packet);
        actions->observation_.reply_sequence++;
        actions->observation_.reply_opcode = normalized;
        actions->observation_.reply_status = status;
        actions->observation_.reply_result =
            static_cast<uint32_t>(result);
        if (normalized == 0x0953)
            actions->observation_.message_observed = 1;
        if (normalized == 0x0A45)
            actions->observation_.reply_consumed = 1;
        if (normalized == 0x04B3) {
            actions->observation_.list_page_replies++;
            actions->observation_.list_total_count = total;
            actions->observation_.list_entries_seen += entries;
        }
        if (normalized == 0x0645 ||
            normalized == 0x0737 ||
            normalized == 0x04DE)
            actions->observation_.pending.available = 0;
        if (actions->commandPending_ &&
            actions->ReplyMatchesActiveCommand(normalized))
            ++actions->matchedReplySequence_;
        actions->RefreshPassiveState();
        return result;
    }

    bool ReplyMatchesActiveCommand(uint16_t opcode) const
    {
        switch (command_.operation) {
        case kRefreshList:
        case kRequestListPage:
            return opcode == 0x04B3;
        case kAddByName:
        case kAcceptPending:
            return opcode == 0x0645 || opcode == 0x0737;
        case kRejectPending:
            return opcode == 0x0737;
        case kRemoveFriend:
            return opcode == 0x04DE;
        default:
            return false;
        }
    }

    Result Publish(Result result, const Command& command)
    {
        observation_.result = result;
        observation_.command_sequence = command.sequence;
        observation_.operation = command.operation;
        observation_.target_id = command.target_id;
        memcpy(
            observation_.target_name,
            command.target_name,
            sizeof(observation_.target_name));
        if (native_) {
            const NativeActions::OnboardingObservation state =
                native_->OnboardingState();
            observation_.native_app_state = state.native_app_state;
            observation_.profile_pilot_id = state.profile_pilot_id;
        }
        return result;
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_
            ? reinterpret_cast<Fn>(api_->Symbol(mod_, name))
            : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    uint8_t** playerDataPointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    void* messengerVtable_ = nullptr;
    void* messengerServiceVtable_ = nullptr;
    void* addDialogVtable_ = nullptr;
    void* pendingDialogVtable_ = nullptr;
    InitialListRequestFn initialListRequest_ = nullptr;
    MessengerDispatchFn messengerDispatch_ = nullptr;
    SimpleBusinessFn openMessenger_ = nullptr;
    SimpleBusinessFn openAddDialog_ = nullptr;
    BoolBusinessFn addFriend_ = nullptr;
    NativeWStringValidatorFn acceptedDuplicateValidator_ =
        nullptr;
    AnsiValidatorFn pendingDuplicateValidator_ = nullptr;
    SimpleBusinessFn acceptPending_ = nullptr;
    SimpleBusinessFn rejectPending_ = nullptr;
    SimpleBusinessFn removeFriend_ = nullptr;
    DeleteDialogCtorFn deleteDialogCtor_ = nullptr;
    SimpleBusinessFn deleteDialogDtor_ = nullptr;
    OpenConversationFn openConversation_ = nullptr;
    SimpleBusinessFn sendMessage_ = nullptr;
    UiEditSetTextFn uiEditSetText_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    TreeItemByIndexFn treeItemByIndex_ = nullptr;
    TreeItemDataFn treeItemData_ = nullptr;
    TreeSelectFn treeSelect_ = nullptr;
    SimpleBusinessFn openSelectedFriend_ = nullptr;
    Observation observation_ = {};
    Command command_ = {};
    uint8_t pendingPacket_[0x30] = {};
    uint32_t replyBaseline_ = 0;
    uint32_t matchedReplySequence_ = 0;
    char localValidationTarget_[17] = {};
    wchar_t localValidationWideTarget_[17] = {};
    size_t localValidationTargetLength_ = 0;
    size_t localValidationWideTargetLength_ = 0;
    void* localValidationDialog_ = nullptr;
    DWORD localValidationThreadId_ = 0;
    LocalValidation localValidationObservedCode_ =
        kLocalValidationNone;
    bool bound_ = false;
    bool commandPending_ = false;
    bool commandStarted_ = false;
    bool messengerOpenRequested_ = false;
    bool addDialogRequested_ = false;
    bool localValidationScope_ = false;
    inline static FriendActions* active_ = nullptr;
};

} // namespace friends
