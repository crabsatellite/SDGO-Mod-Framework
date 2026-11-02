#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "friend_actions.h"

namespace friends {

class FriendExtraActions
{
public:
    enum Operation : uint32_t
    {
        kCloseSelectedConversation = 10,
        kOpenProfile = 11,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t command_sequence;
        uint32_t operation;
        uint32_t target_id;
        int32_t selected_conversation;
        uint32_t conversation_count_before;
        uint32_t conversation_count_after;
        uint32_t profile_enqueue_count;
        uint32_t profile_reply_target_id;
        uint16_t profile_reply_opcode;
        uint8_t selected_target_matched;
        uint8_t profile_constructed;
        uint8_t profile_enqueued;
        uint8_t profile_clone_created;
        uint8_t profile_clone_identity_matched;
        uint8_t profile_initialized;
        uint8_t profile_request_started;
        uint8_t profile_reply_consumed;
        uint8_t profile_reply_target_matched;
        uint8_t profile_destroyed;
        uint8_t reserved;
        char target_name[17];
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod)
    {
        api_ = api;
        mod_ = mod;
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
        closeConversation_ =
            Symbol<CloseConversationFn>(
                "friends.extra.close_selected_conversation");
        profilePopupCtor_ =
            Symbol<ProfilePopupCtorFn>(
                "friends.extra.profile_popup_ctor");
        profilePopupDtor_ =
            Symbol<ProfilePopupDtorFn>(
                "friends.extra.profile_popup_dtor");
        profilePopupVtable_ =
            Symbol<void*>(
                "friends.extra.profile_popup_vtable");
        profilePopupClone_ =
            Symbol<ProfilePopupCloneFn>(
                "friends.extra.profile_popup_clone");
        profilePopupInitialize_ =
            Symbol<ProfilePopupInitializeFn>(
                "friends.extra.profile_popup_initialize");
        profilePopupDispatch_ =
            Symbol<ProfilePopupDispatchFn>(
                "friends.extra.profile_popup_dispatch");
        profilePopupScalarDtor_ =
            Symbol<ProfilePopupScalarDtorFn>(
                "friends.extra.profile_popup_scalar_dtor");
        enqueueUiObject_ =
            Symbol<EnqueueUiObjectFn>("flow.enqueue_ui_object");
        Reset();
        bound_ =
            api_ && api_->HookCode && mod_ && playerDataPointer_ &&
            serviceManagedPointer_ && serviceFindByName_ &&
            nativeWStringCtor_ && nativeWStringDtor_ &&
            messengerVtable_ && closeConversation_ &&
            profilePopupCtor_ && profilePopupDtor_ &&
            profilePopupVtable_ && profilePopupClone_ &&
            profilePopupInitialize_ && profilePopupDispatch_ &&
            profilePopupScalarDtor_ &&
            enqueueUiObject_;
        if (!bound_ || (active_ && active_ != this))
            return false;
        active_ = this;
        if (!api_->HookCode(
                mod_,
                "friends.extra.profile_popup_clone",
                reinterpret_cast<void*>(&ObserveProfilePopupClone),
                reinterpret_cast<void**>(&profilePopupClone_)) ||
            !api_->HookCode(
                mod_,
                "friends.extra.profile_popup_initialize",
                reinterpret_cast<void*>(
                    &ObserveProfilePopupInitialize),
                reinterpret_cast<void**>(
                    &profilePopupInitialize_)) ||
            !api_->HookCode(
                mod_,
                "friends.extra.profile_popup_dispatch",
                reinterpret_cast<void*>(
                    &ObserveProfilePopupDispatch),
                reinterpret_cast<void**>(
                    &profilePopupDispatch_)) ||
            !api_->HookCode(
                mod_,
                "friends.extra.profile_popup_scalar_dtor",
                reinterpret_cast<void*>(
                    &ObserveProfilePopupScalarDtor),
                reinterpret_cast<void**>(
                    &profilePopupScalarDtor_))) {
            active_ = nullptr;
            bound_ = false;
        }
        return bound_;
    }

    void Reset()
    {
        std::memset(&observation_, 0, sizeof(observation_));
        observation_.selected_conversation = -1;
        std::memset(
            expectedProfileName_,
            0,
            sizeof(expectedProfileName_));
        expectedProfileNameLength_ = 0;
        expectedProfileTargetId_ = 0;
        managedProfilePopup_ = nullptr;
        profileCloneSource_ = nullptr;
        profileCloneThreadId_ = 0;
        profileCloneScope_ = false;
        profilePending_ = false;
    }

    Observation State() const { return observation_; }

    FriendActions::Result Submit(const FriendActions::Command& command)
    {
        if (profilePending_)
            return FriendActions::kBusy;
        Reset();
        observation_.command_sequence = command.sequence;
        observation_.operation = command.operation;
        observation_.target_id = command.target_id;
        std::memcpy(
            observation_.target_name,
            command.target_name,
            sizeof(observation_.target_name));
        observation_.target_name[16] = '\0';
        if (!bound_)
            return Publish(FriendActions::kUnbound);
        __try {
            if (command.operation == kCloseSelectedConversation)
                return CloseSelectedConversation(command);
            if (command.operation == kOpenProfile)
                return OpenProfile(command);
            return Publish(FriendActions::kInvalidCommand);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(FriendActions::kNativeFault);
        }
    }

    void Tick() {}

private:
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kProfilePopupSize = 0x210;
    static constexpr size_t kProfileNameOffset = 0x50;
    static constexpr size_t kMaximumProfileNameLength = 64;
    static constexpr size_t kProfileDataOffset = 0x70;
    static constexpr size_t kProfileDataSize = 0x10D;
    static constexpr size_t kProfileReplyTargetOffset = 0x180;
    static constexpr uint16_t kProfileReplyOpcode = 0x084B;
    static constexpr uint32_t kRelationshipAccepted = 7;
    static constexpr uint32_t kMaximumNativeEntries = 100;

    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using CloseConversationFn = void(__thiscall*)(void*, int);
    using ProfilePopupCtorFn =
        void(__thiscall*)(void*, const void*, int);
    using ProfilePopupDtorFn = void(__thiscall*)(void*);
    using ProfilePopupCloneFn = void*(__thiscall*)(void*);
    using ProfilePopupInitializeFn =
        void(__thiscall*)(void*, int);
    using ProfilePopupDispatchFn =
        int(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);
    using ProfilePopupScalarDtorFn =
        void*(__thiscall*)(void*, uint32_t);
    using EnqueueUiObjectFn = void*(__cdecl*)(void*, int);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !mod_ || !name)
            return nullptr;
        return reinterpret_cast<T>(api_->Symbol(mod_, name));
    }

    static const wchar_t* NativeStringData(const void* storage)
    {
        if (!storage)
            return nullptr;
        const auto* bytes =
            static_cast<const uint8_t*>(storage);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                bytes + 0x14);
        return capacity < 8
            ? reinterpret_cast<const wchar_t*>(bytes)
            : *reinterpret_cast<wchar_t* const*>(bytes);
    }

    static bool CaptureNativeString(
        const void* storage,
        wchar_t* destination,
        size_t capacity,
        size_t* capturedLength)
    {
        if (!storage || !destination || capacity == 0 ||
            !capturedLength)
            return false;
        const auto* bytes =
            static_cast<const uint8_t*>(storage);
        const uint32_t length =
            *reinterpret_cast<const uint32_t*>(
                bytes + 0x10);
        const uint32_t nativeCapacity =
            *reinterpret_cast<const uint32_t*>(
                bytes + 0x14);
        if (length > nativeCapacity ||
            length >= capacity)
            return false;
        const wchar_t* value = NativeStringData(storage);
        if (!value)
            return false;
        std::memcpy(
            destination,
            value,
            length * sizeof(wchar_t));
        destination[length] = L'\0';
        *capturedLength = length;
        return true;
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
            *reinterpret_cast<const uint32_t*>(
                bytes + 0x10);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                bytes + 0x14);
        if (length != expectedLength ||
            length > capacity)
            return false;
        const wchar_t* value = NativeStringData(storage);
        return value &&
            std::memcmp(
                value,
                expected,
                expectedLength * sizeof(wchar_t)) == 0;
    }

    void* ResolveMessenger() const
    {
        if (!serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_ ||
            !messengerVtable_)
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
                L"MessengerPopup");
            constructed = true;
            service = serviceFindByName_(manager, descriptor);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    descriptor + sizeof(uint32_t));
        }
        if (!service ||
            *reinterpret_cast<void**>(service) != messengerVtable_)
            return nullptr;
        return service;
    }

    static size_t BoundedLength(
        const char* value,
        size_t maximum)
    {
        if (!value)
            return maximum + 1;
        for (size_t index = 0; index <= maximum; ++index) {
            if (value[index] == '\0')
                return index;
        }
        return maximum + 1;
    }

    static bool Matches(
        const uint8_t* record,
        const FriendActions::Command& command)
    {
        if (!record)
            return false;
        const uint32_t id =
            *reinterpret_cast<const uint32_t*>(record);
        const char* name =
            reinterpret_cast<const char*>(record + 0x0A);
        bool specified = false;
        if (command.target_id) {
            specified = true;
            if (id != command.target_id)
                return false;
        }
        const size_t nameLength =
            BoundedLength(command.target_name, 16);
        if (nameLength > 16)
            return false;
        if (nameLength != 0) {
            specified = true;
            if (std::strncmp(
                    name,
                    command.target_name,
                    16) != 0)
                return false;
        }
        return specified;
    }

    static uint8_t* NodeAt(
        uint8_t* head,
        uint32_t count,
        uint32_t index)
    {
        if (!head || count > kMaximumNativeEntries ||
            index >= count)
            return nullptr;
        uint8_t* node = *reinterpret_cast<uint8_t**>(head);
        for (uint32_t current = 0;
             current < index && node && node != head;
             ++current) {
            node = *reinterpret_cast<uint8_t**>(node);
        }
        return node && node != head ? node : nullptr;
    }

    uint8_t* FindFriendRecord(
        const FriendActions::Command& command) const
    {
        uint8_t* data =
            playerDataPointer_ ? *playerDataPointer_ : nullptr;
        if (!data)
            return nullptr;
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(data + 0x7A4);
        const uint32_t count =
            *reinterpret_cast<uint32_t*>(data + 0x7A8);
        if (!head || count > kMaximumNativeEntries)
            return nullptr;
        uint8_t* node = *reinterpret_cast<uint8_t**>(head);
        for (uint32_t index = 0;
             index < count && node && node != head;
             ++index) {
            uint8_t* record =
                *reinterpret_cast<uint8_t**>(node + 8);
            if (record && Matches(record, command))
                return record;
            node = *reinterpret_cast<uint8_t**>(node);
        }
        return nullptr;
    }

    FriendActions::Result CloseSelectedConversation(
        const FriendActions::Command& command)
    {
        void* messenger = ResolveMessenger();
        if (!messenger)
            return Publish(FriendActions::kMessengerUnavailable);
        uint8_t* data =
            playerDataPointer_ ? *playerDataPointer_ : nullptr;
        if (!data)
            return Publish(FriendActions::kProfileUnavailable);
        const int32_t selected =
            *reinterpret_cast<int32_t*>(data + 0x790);
        const uint32_t count =
            *reinterpret_cast<uint32_t*>(data + 0x7B4);
        observation_.selected_conversation = selected;
        observation_.conversation_count_before = count;
        observation_.conversation_count_after = count;
        if (selected < 0 ||
            static_cast<uint32_t>(selected) >= count)
            return Publish(FriendActions::kFriendNotFound);
        uint8_t* head =
            *reinterpret_cast<uint8_t**>(data + 0x7B0);
        uint8_t* node =
            NodeAt(head, count, static_cast<uint32_t>(selected));
        uint8_t* conversation =
            node ? *reinterpret_cast<uint8_t**>(node + 8) : nullptr;
        uint8_t* record =
            conversation
                ? *reinterpret_cast<uint8_t**>(conversation)
                : nullptr;
        const bool hasTarget =
            command.target_id != 0 ||
            BoundedLength(command.target_name, 16) != 0;
        if (!record || (hasTarget && !Matches(record, command)))
            return Publish(FriendActions::kFriendNotFound);
        observation_.selected_target_matched = 1;
        closeConversation_(messenger, selected);
        data = playerDataPointer_ ? *playerDataPointer_ : nullptr;
        if (!data)
            return Publish(FriendActions::kNativeFault);
        observation_.conversation_count_after =
            *reinterpret_cast<uint32_t*>(data + 0x7B4);
        return Publish(
            observation_.conversation_count_after + 1 == count
                ? FriendActions::kReady
                : FriendActions::kNativeRequestNotSent);
    }

    FriendActions::Result OpenProfile(
        const FriendActions::Command& command)
    {
        const size_t targetNameLength =
            BoundedLength(command.target_name, 16);
        if (targetNameLength > 16)
            return Publish(FriendActions::kInvalidTarget);
        if (!command.target_id &&
            targetNameLength == 0)
            return Publish(FriendActions::kInvalidTarget);
        uint8_t* record = FindFriendRecord(command);
        if (!record || record[0x1B] != kRelationshipAccepted)
            return Publish(FriendActions::kFriendNotFound);
        expectedProfileTargetId_ =
            *reinterpret_cast<uint32_t*>(record);
        alignas(4) uint8_t popup[kProfilePopupSize] = {};
        bool constructed = false;
        bool targetCaptured = false;
        void* queued = nullptr;
        __try {
            profilePopupCtor_(popup, record + 0x38, 0);
            constructed = true;
            observation_.profile_constructed = 1;
            targetCaptured =
                CaptureNativeString(
                    popup + kProfileNameOffset,
                    expectedProfileName_,
                    sizeof(expectedProfileName_) /
                        sizeof(expectedProfileName_[0]),
                    &expectedProfileNameLength_);
            if (targetCaptured) {
                profileCloneSource_ = popup;
                profileCloneThreadId_ = GetCurrentThreadId();
                profileCloneScope_ = true;
                queued = enqueueUiObject_(popup, 1);
                profileCloneScope_ = false;
                profileCloneThreadId_ = 0;
                profileCloneSource_ = nullptr;
                if (queued) {
                    observation_.profile_enqueued = 1;
                    ++observation_.profile_enqueue_count;
                }
            }
        } __finally {
            profileCloneScope_ = false;
            profileCloneThreadId_ = 0;
            profileCloneSource_ = nullptr;
            if (constructed)
                profilePopupDtor_(popup);
        }
        if (!targetCaptured || !queued ||
            !observation_.profile_clone_created ||
            !observation_.profile_clone_identity_matched ||
            !observation_.profile_initialized ||
            !observation_.profile_request_started) {
            managedProfilePopup_ = nullptr;
            profilePending_ = false;
            return Publish(
                FriendActions::kNativeRequestNotSent);
        }
        if (observation_.profile_reply_consumed) {
            profilePending_ = false;
            return static_cast<FriendActions::Result>(
                observation_.result);
        }
        profilePending_ = true;
        return Publish(FriendActions::kWaitingForReply);
    }

    static void* __fastcall ObserveProfilePopupClone(
        void* self,
        void*)
    {
        FriendExtraActions* actions = active_;
        if (!actions || !actions->profilePopupClone_)
            return nullptr;
        void* clone =
            actions->profilePopupClone_(self);
        const DWORD error = GetLastError();
        if (actions->profileCloneScope_ &&
            actions->profileCloneThreadId_ ==
                GetCurrentThreadId() &&
            actions->profileCloneSource_ == self) {
            actions->observation_.profile_clone_created =
                clone && clone != self ? 1 : 0;
            if (clone && clone != self) {
                __try {
                    const uint8_t* bytes =
                        static_cast<const uint8_t*>(clone);
                    const bool identityMatched =
                        *reinterpret_cast<void* const*>(bytes) ==
                            actions->profilePopupVtable_ &&
                        NativeStringMatches(
                            bytes + kProfileNameOffset,
                            actions->expectedProfileName_,
                            actions->
                                expectedProfileNameLength_);
                    actions->observation_
                        .profile_clone_identity_matched =
                        identityMatched ? 1 : 0;
                    if (identityMatched)
                        actions->managedProfilePopup_ = clone;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    actions->observation_
                        .profile_clone_identity_matched = 0;
                }
            }
        }
        SetLastError(error);
        return clone;
    }

    static void __fastcall ObserveProfilePopupInitialize(
        void* self,
        void*,
        int compare)
    {
        FriendExtraActions* actions = active_;
        if (!actions || !actions->profilePopupInitialize_)
            return;
        actions->profilePopupInitialize_(self, compare);
        const DWORD error = GetLastError();
        if (actions->managedProfilePopup_ == self) {
            actions->observation_.profile_initialized = 1;
            actions->observation_.profile_request_started = 1;
        }
        SetLastError(error);
    }

    static int __fastcall ObserveProfilePopupDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        FriendExtraActions* actions = active_;
        if (!actions || !actions->profilePopupDispatch_)
            return 0;
        const uint16_t normalized =
            static_cast<uint16_t>(opcode);
        uint8_t replyProfile[kProfileDataSize] = {};
        uint32_t replyTargetId = 0;
        bool captured = false;
        if (actions->managedProfilePopup_ == self &&
            normalized == kProfileReplyOpcode &&
            packet) {
            __try {
                const uint8_t* bytes =
                    static_cast<const uint8_t*>(packet) + 6;
                std::memcpy(
                    replyProfile,
                    bytes,
                    sizeof(replyProfile));
                std::memcpy(
                    &replyTargetId,
                    bytes,
                    sizeof(replyTargetId));
                captured = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                captured = false;
            }
        }
        const int result =
            actions->profilePopupDispatch_(
                self,
                a,
                b,
                c,
                opcode,
                packet);
        const DWORD error = GetLastError();
        if (actions->managedProfilePopup_ == self &&
            normalized == kProfileReplyOpcode) {
            actions->observation_.profile_reply_opcode =
                normalized;
            actions->observation_.profile_reply_target_id =
                replyTargetId;
            actions->observation_.profile_reply_consumed = 1;
            bool targetMatched = false;
            if (captured) {
                __try {
                    const uint8_t* bytes =
                        static_cast<const uint8_t*>(self);
                    targetMatched =
                        replyTargetId ==
                            actions->expectedProfileTargetId_ &&
                        *reinterpret_cast<const uint32_t*>(
                            bytes +
                            kProfileReplyTargetOffset) ==
                            replyTargetId &&
                        std::memcmp(
                            bytes + kProfileDataOffset,
                            replyProfile,
                            sizeof(replyProfile)) == 0;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    targetMatched = false;
                }
            }
            actions->observation_
                .profile_reply_target_matched =
                targetMatched ? 1 : 0;
            actions->profilePending_ = false;
            actions->Publish(
                targetMatched
                    ? FriendActions::kReplyObserved
                    : FriendActions::kReplyRejected);
        }
        SetLastError(error);
        return result;
    }

    static void* __fastcall ObserveProfilePopupScalarDtor(
        void* self,
        void*,
        uint32_t flags)
    {
        FriendExtraActions* actions = active_;
        if (!actions || !actions->profilePopupScalarDtor_)
            return self;
        const bool matched =
            actions->managedProfilePopup_ == self;
        if (matched) {
            actions->observation_.profile_destroyed = 1;
            actions->managedProfilePopup_ = nullptr;
            if (actions->profilePending_) {
                actions->profilePending_ = false;
                actions->Publish(
                    FriendActions::kReplyRejected);
            }
        }
        return actions->profilePopupScalarDtor_(
            self,
            flags);
    }

    FriendActions::Result Publish(FriendActions::Result result)
    {
        observation_.result = result;
        return result;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    uint8_t** playerDataPointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    void* messengerVtable_ = nullptr;
    CloseConversationFn closeConversation_ = nullptr;
    ProfilePopupCtorFn profilePopupCtor_ = nullptr;
    ProfilePopupDtorFn profilePopupDtor_ = nullptr;
    void* profilePopupVtable_ = nullptr;
    ProfilePopupCloneFn profilePopupClone_ = nullptr;
    ProfilePopupInitializeFn profilePopupInitialize_ = nullptr;
    ProfilePopupDispatchFn profilePopupDispatch_ = nullptr;
    ProfilePopupScalarDtorFn profilePopupScalarDtor_ = nullptr;
    EnqueueUiObjectFn enqueueUiObject_ = nullptr;
    wchar_t expectedProfileName_[
        kMaximumProfileNameLength + 1] = {};
    size_t expectedProfileNameLength_ = 0;
    uint32_t expectedProfileTargetId_ = 0;
    void* managedProfilePopup_ = nullptr;
    void* profileCloneSource_ = nullptr;
    DWORD profileCloneThreadId_ = 0;
    bool profileCloneScope_ = false;
    bool profilePending_ = false;
    bool bound_ = false;
    Observation observation_ = {};
    inline static FriendExtraActions* active_ = nullptr;
};

} // namespace friends
