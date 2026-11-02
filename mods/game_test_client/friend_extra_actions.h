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
        uint8_t selected_target_matched;
        uint8_t profile_constructed;
        uint8_t profile_enqueued;
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
        enqueueUiObject_ =
            Symbol<EnqueueUiObjectFn>("flow.enqueue_ui_object");
        Reset();
        bound_ =
            api_ && mod_ && playerDataPointer_ &&
            serviceManagedPointer_ && serviceFindByName_ &&
            nativeWStringCtor_ && nativeWStringDtor_ &&
            messengerVtable_ && closeConversation_ &&
            profilePopupCtor_ && profilePopupDtor_ &&
            enqueueUiObject_;
        return bound_;
    }

    void Reset()
    {
        std::memset(&observation_, 0, sizeof(observation_));
        observation_.selected_conversation = -1;
    }

    Observation State() const { return observation_; }

    FriendActions::Result Submit(const FriendActions::Command& command)
    {
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
    static constexpr size_t kProfilePopupSize = 0x214;
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
    using EnqueueUiObjectFn = void*(__cdecl*)(void*, int);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !mod_ || !name)
            return nullptr;
        return reinterpret_cast<T>(api_->Symbol(mod_, name));
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
                L"Messenger");
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
        return
            (command.target_id && id == command.target_id) ||
            (command.target_name[0] &&
             std::strncmp(name, command.target_name, 16) == 0);
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
        if (!command.target_id &&
            BoundedLength(command.target_name, 16) == 0)
            return Publish(FriendActions::kInvalidTarget);
        uint8_t* record = FindFriendRecord(command);
        if (!record || record[0x1B] != kRelationshipAccepted)
            return Publish(FriendActions::kFriendNotFound);
        alignas(4) uint8_t popup[kProfilePopupSize] = {};
        bool constructed = false;
        void* queued = nullptr;
        __try {
            profilePopupCtor_(popup, record + 0x38, 0);
            constructed = true;
            observation_.profile_constructed = 1;
            queued = enqueueUiObject_(popup, 1);
            if (queued) {
                observation_.profile_enqueued = 1;
                ++observation_.profile_enqueue_count;
            }
        } __finally {
            if (constructed)
                profilePopupDtor_(popup);
        }
        return Publish(
            queued
                ? FriendActions::kReady
                : FriendActions::kNativeRequestNotSent);
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
    EnqueueUiObjectFn enqueueUiObject_ = nullptr;
    bool bound_ = false;
    Observation observation_ = {};
};

} // namespace friends
