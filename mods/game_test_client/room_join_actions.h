#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Native guest-side room entry and Ready actions for the second GameTest
// client. The adapter never receives a room id from its caller. It resolves an
// exact title from the live server-advertised RoomList vector, validates that
// vector's corresponding live GameRoom row, and invokes the shipped row
// callback. Completion requires the shipped reply handler plus the real room
// model installed by the client.
class RoomJoinActions
{
public:
    enum JoinResult : uint32_t
    {
        kJoinNone = 0,
        kJoinPending = 1,
        kJoinRejected = 2,
        kJoinAccepted = 3,
    };

    enum ReadyResult : uint32_t
    {
        kReadyNone = 0,
        kReadyPending = 1,
        kReadyRejected = 2,
        kReadyAccepted = 3,
    };

    struct JoinObservation
    {
        uint32_t sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t requested_room_id;
        uint32_t acknowledged_room_id;
        uint32_t model_room_id;
        uint16_t reply_opcode;
        uint8_t room_variant;
        uint8_t original_handler_completed;
        uint8_t local_is_guest;
        uint8_t reserved[3];
    };

    struct ReadyObservation
    {
        uint32_t sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t room_id;
        uint16_t reply_opcode;
        uint8_t local_slot;
        uint8_t ready_flag;
        uint8_t reply_detail;
        uint8_t server_reply_observed;
        uint8_t original_dispatch_completed;
        uint8_t post_dispatch_model_ready;
        uint8_t native_business_ready;
        uint8_t reserved;
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
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>(
                "flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>(
                "flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");
        roomListVtable_ =
            Symbol<void*>("flow.native_room_list_vtable");
        roomJoinCallback_ =
            Symbol<RoomJoinCallbackFn>(
                "flow.room_join_callback");
        threeDWaitRoomVtable_ =
            Symbol<void*>("flow.three_d_wait_room_vtable");
        readyCommandVtable_ =
            Symbol<void*>(
                "flow.room_join_ready_command_vtable");
        readyBusiness_ =
            Symbol<ReadyBusinessFn>(
                "flow.room_join_ready_business");
        waitRoomDispatch_ =
            Symbol<WaitRoomDispatchFn>(
                "flow.waitroom_packet_dispatch");
        managerVtables_[0] =
            Symbol<void*>("flow.normal_3d_manager_vtable");
        managerVtables_[1] =
            Symbol<void*>("flow.ai_3d_manager_vtable");
        managerVtables_[2] =
            Symbol<void*>(
                "flow.battleship_3d_manager_vtable");

        if (!api_ || !api_->HookCode || !api_->HookSlot ||
            !mod_ || !native_ ||
            !gameApp_ || !profilePointer_ || !isRoomMaster_ ||
            !serviceManagedPointer_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !nativeUiFindControl_ ||
            !roomListVtable_ || !roomJoinCallback_ ||
            !threeDWaitRoomVtable_ || !readyCommandVtable_ ||
            !readyBusiness_ || !waitRoomDispatch_ ||
            !managerVtables_[0] ||
            !managerVtables_[1] || !managerVtables_[2])
            return false;
        if (active_ && active_ != this)
            return false;

        __try {
            void** roomVtable =
                static_cast<void**>(roomListVtable_);
            void** readyVtable =
                static_cast<void**>(readyCommandVtable_);
            if (roomVtable[
                    kUiCallbackSlot / sizeof(void*)] !=
                    reinterpret_cast<void*>(roomJoinCallback_) ||
                readyVtable[
                    kReadyBusinessSlot / sizeof(void*)] !=
                    reinterpret_cast<void*>(readyBusiness_))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        active_ = this;
        if (!api_->HookSlot(
                mod_,
                "flow.room_join_list_packet_slot",
                reinterpret_cast<void*>(
                    &ObserveRoomListPacket),
                reinterpret_cast<void**>(
                    &roomListPacket_))) {
            active_ = nullptr;
            return false;
        }
        passiveRoomListPacket_ =
            roomListPacket_;
        if (!api_->HookCode(
                mod_,
                "flow.waitroom_packet_dispatch",
                reinterpret_cast<void*>(
                    &ObserveWaitRoomPacket),
                reinterpret_cast<void**>(
                    &waitRoomDispatch_))) {
            active_ = nullptr;
            return false;
        }
        passiveWaitRoomDispatch_ =
            waitRoomDispatch_;

        bound_ = true;
        return true;
    }

    // The caller supplies only the exact native title containing its nonce.
    // The room id is always taken from the genuine server list entry selected
    // by that title. Titles are the legacy list's exact 1..20 byte ASCII field.
    using RoomResolver=const uint8_t*(*)(void*);
    void SetRoomResolver(RoomResolver resolver,void* opaque) {
        roomResolver_=resolver;roomOpaque_=opaque;
    }

    bool JoinExactTitle(const char* exactTitle)
    {
        if (!bound_ || !native_ ||
            !ValidExactTitle(exactTitle) ||
            InterlockedCompareExchange(
                &joinPending_,
                0,
                0) != 0)
            return false;
        if (native_->AppState() != kRoomListAppState)
            return false;

        __try {
            RoomSelection selection = {};
            if (!ResolveRoomSelection(
                    exactTitle,
                    selection))
                return false;

            ResetJoinObservation();
            std::memcpy(
                expectedTitle_,
                exactTitle,
                std::strlen(exactTitle) + 1);
            InterlockedExchange(
                &expectedRoomId_,
                static_cast<LONG>(
                    selection.room_id));
            InterlockedExchangePointer(
                &pendingRoomList_,
                selection.service);
            InterlockedExchange(
                &joinResult_,
                kJoinPending);
            InterlockedExchange(
                &joinPending_,
                1);

            const NativeActions::TransportObservation before =
                native_->TransportState();
            roomJoinCallback_(
                selection.service,
                kRoomRowActivateEvent,
                selection.control_name,
                selection.control);
            const NativeActions::TransportObservation after =
                native_->TransportState();

            if (after.send_sequence == before.send_sequence ||
                after.opcode != kJoinRequestOpcode ||
                after.packet_size != kJoinRequestSize ||
                after.result == 0) {
                RejectPendingJoin(0, 0, 0);
                return false;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            RejectPendingJoin(0, 0, 0);
            return false;
        }
    }

    // Invokes the original guest 3D Ready command. It refuses to toggle an
    // already-ready pilot back off. The command's context byte is only an
    // optimistic local business flag. Success is published only after the full
    // CWaitRoom dispatcher returns from a genuine local 0x0999 and its native
    // slot model reads back ready.
    bool Ready()
    {
        if (!bound_ || !native_ ||
            InterlockedCompareExchange(
                &readyPending_,
                0,
                0) != 0)
            return false;

        __try {
            ReadyContext context = {};
            if (!ResolveReadyContext(context) ||
                context.ready)
                return false;

            ResetReadyObservation();
            InterlockedExchange(
                &readyRoomId_,
                static_cast<LONG>(context.room_id));
            InterlockedExchange(
                &readyLocalSlot_,
                context.local_slot);
            InterlockedExchangePointer(
                &pendingReadyCommand_,
                context.command);
            InterlockedExchangePointer(
                &pendingReadyWaitRoom_,
                context.wait_room);
            InterlockedExchange(
                &readyResult_,
                kReadyPending);
            InterlockedExchange(
                &readyPending_,
                1);

            const NativeActions::TransportObservation before =
                native_->TransportState();
            readyBusiness_(context.command);
            const NativeActions::TransportObservation after =
                native_->TransportState();
            const uint8_t businessReady =
                Read<uint8_t>(
                    context.manager +
                    kManagerCommandContext);

            if (after.send_sequence == before.send_sequence ||
                after.opcode != kReadyRequestOpcode ||
                after.packet_size != kReadyRequestSize ||
                after.result == 0 ||
                businessReady != 1) {
                RejectPendingReady(0, context.local_slot, 0);
                return false;
            }
            InterlockedExchange(
                &readyNativeBusinessReady_,
                businessReady);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            RejectPendingReady(0, 0xFF, 0);
            return false;
        }
    }

    JoinObservation JoinState() const
    {
        RefreshJoinAcceptance();

        JoinObservation observed = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(
                    &joinWriteVersion_,
                    0,
                    0);
            if (before & 1)
                continue;
            observed.sequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &joinSequence_,
                        0,
                        0));
            observed.result =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &joinResult_,
                        0,
                        0));
            observed.native_app_state =
                native_ ? native_->AppState()
                        : UINT32_MAX;
            observed.requested_room_id =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &expectedRoomId_,
                        0,
                        0));
            observed.acknowledged_room_id =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &acknowledgedRoomId_,
                        0,
                        0));
            observed.model_room_id =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &modelRoomId_,
                        0,
                        0));
            const uint32_t reply =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &joinReply_,
                        0,
                        0));
            observed.reply_opcode =
                static_cast<uint16_t>(
                    reply & 0xFFFFu);
            observed.room_variant =
                static_cast<uint8_t>(
                    (reply >> 16) & 0xFFu);
            observed.original_handler_completed =
                InterlockedCompareExchange(
                    &joinOriginalCompleted_,
                    0,
                    0) != 0;
            observed.local_is_guest =
                InterlockedCompareExchange(
                    &joinLocalGuest_,
                    0,
                    0) != 0;
            const LONG after =
                InterlockedCompareExchange(
                    &joinWriteVersion_,
                    0,
                    0);
            if (before == after && !(after & 1))
                return observed;
        }
        std::memset(
            &observed,
            0,
            sizeof(observed));
        observed.native_app_state = UINT32_MAX;
        return observed;
    }

    ReadyObservation ReadyState() const
    {
        ReadyObservation observed = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(
                    &readyWriteVersion_,
                    0,
                    0);
            if (before & 1)
                continue;
            observed.sequence =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &readySequence_,
                        0,
                        0));
            observed.result =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &readyResult_,
                        0,
                        0));
            observed.native_app_state =
                native_ ? native_->AppState()
                        : UINT32_MAX;
            observed.room_id =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &readyRoomId_,
                        0,
                        0));
            const uint32_t reply =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &readyReplyDetails_,
                        0,
                        0));
            observed.reply_opcode =
                static_cast<uint16_t>(
                    reply & 0xFFFFu);
            observed.local_slot =
                static_cast<uint8_t>(
                    (reply >> 16) & 0xFFu);
            observed.ready_flag =
                static_cast<uint8_t>(
                    (reply >> 24) & 0xFFu);
            observed.reply_detail =
                static_cast<uint8_t>(
                    InterlockedCompareExchange(
                        &readyReplyDetail_,
                        0,
                        0));
            observed.server_reply_observed =
                InterlockedCompareExchange(
                    &readyServerReplyObserved_,
                    0,
                    0) != 0;
            observed.original_dispatch_completed =
                InterlockedCompareExchange(
                    &readyOriginalCompleted_,
                    0,
                    0) != 0;
            observed.post_dispatch_model_ready =
                InterlockedCompareExchange(
                    &readyModelReady_,
                    0,
                    0) != 0;
            observed.native_business_ready =
                InterlockedCompareExchange(
                    &readyNativeBusinessReady_,
                    0,
                    0) != 0;
            const LONG after =
                InterlockedCompareExchange(
                    &readyWriteVersion_,
                    0,
                    0);
            if (before == after && !(after & 1))
                return observed;
        }
        std::memset(
            &observed,
            0,
            sizeof(observed));
        observed.native_app_state = UINT32_MAX;
        return observed;
    }

    // Clears only adapter-owned receipts and the remembered exact title.
    // It does not touch the client's room, list, state machine, or transport.
    void ResetObservations()
    {
        ResetJoinObservation();
        ResetReadyObservation();
        expectedTitle_[0] = '\0';
        InterlockedExchange(
            &expectedRoomId_,
            0);
        InterlockedExchangePointer(
            &pendingRoomList_,
            nullptr);
        InterlockedExchangePointer(
            &pendingReadyCommand_,
            nullptr);
        InterlockedExchangePointer(
            &pendingReadyWaitRoom_,
            nullptr);
    }

private:
    struct RoomSelection
    {
        void* service;
        void* control;
        const void* control_name;
        uint32_t room_id;
    };

    struct ReadyContext
    {
        uint8_t* manager;
        void* command;
        uint8_t* wait_room;
        uint32_t room_id;
        uint8_t local_slot;
        bool ready;
    };

    static constexpr uint32_t kRoomListAppState = 6;
    static constexpr uint32_t kThreeDWaitRoomAppState = 18;
    static constexpr uint32_t kRoomRowActivateEvent = 0x303;
    static constexpr uint16_t kRoomListReplyOpcode = 0x0550;
    static constexpr uint16_t kJoinRequestOpcode = 0x0734;
    static constexpr uint32_t kJoinRequestSize = 0x32;
    static constexpr uint16_t kReadyRequestOpcode = 0x05A6;
    static constexpr uint16_t kReadyReplyOpcode = 0x0999;
    static constexpr uint32_t kReadyRequestSize = 8;
    static constexpr uint32_t kRoomEntryStride = 0x90;
    static constexpr uint32_t kMaximumRoomRows = 10;
    static constexpr uint32_t kMaximumServiceNodes = 512;
    static constexpr size_t kProfileRoom = 0xEC;
    static constexpr size_t kGameAppState = 0x4A0;
    static constexpr size_t kGameAppStateOwner = 0x4A4;
    static constexpr size_t kRoomListMakeButton = 0x84;
    static constexpr size_t kRoomListRowBank = 0x94;
    static constexpr size_t kRoomListVectorBegin = 0x124;
    static constexpr size_t kRoomListVectorEnd = 0x128;
    static constexpr size_t kRoomEntryTitle = 0x7A;
    static constexpr size_t kRoomEntryTitleSize = 20;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kControlVisible = 0x162;
    static constexpr size_t kControlEnabled = 0x167;
    static constexpr size_t kControlClickBusiness = 0x21C;
    static constexpr size_t kNativeStringLength = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kUiCallbackSlot = 0x38;
    static constexpr size_t kReadyBusinessSlot = 0x08;
    static constexpr size_t kManagerStateArray = 0x0C;
    static constexpr size_t kManagerLocalSlot = 0x20;
    static constexpr size_t kManagerCommandContext = 0x24;
    static constexpr size_t kManagerWaitRoom = 0x38;
    static constexpr size_t kWaitRoomSlotStride = 0x48E;
    static constexpr size_t kWaitRoomSlotReady = 0xA06;
    static constexpr size_t kReadyCommandIndex = 4;

    using GameAppFn = uint8_t*(__cdecl*)();
    using IsRoomMasterFn =
        bool(__stdcall*)(void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(
            void*,
            const void*,
            uint32_t);
    using RoomJoinCallbackFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);
    using PacketDispatchFn =
        int(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);
    using WaitRoomDispatchFn =
        void(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);
    using ReadyBusinessFn =
        int(__thiscall*)(void*);

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
        std::memcpy(
            &value,
            address,
            sizeof(value));
        return value;
    }

    static bool ValidExactTitle(
        const char* title)
    {
        if (!title)
            return false;
        const size_t length =
            std::strlen(title);
        if (length == 0 ||
            length > kRoomEntryTitleSize)
            return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char value =
                static_cast<unsigned char>(
                    title[i]);
            if (value < 0x20 || value > 0x7E)
                return false;
        }
        return true;
    }

    static bool EntryTitleEquals(
        const uint8_t* entry,
        const char* title)
    {
        const size_t length =
            std::strlen(title);
        const char* field =
            reinterpret_cast<const char*>(
                entry + kRoomEntryTitle);
        if (std::memcmp(
                field,
                title,
                length) != 0)
            return false;
        if (length == kRoomEntryTitleSize)
            return true;
        if (field[length] != '\0')
            return false;
        for (size_t i = length + 1;
             i < kRoomEntryTitleSize;
             ++i) {
            if (field[i] != '\0')
                return false;
        }
        return true;
    }

    bool ResolveRoomSelection(
        const char* title,
        RoomSelection& selection) const
    {
        std::memset(
            &selection,
            0,
            sizeof(selection));
        uint8_t* service =
            ResolveUniqueRoomList();
        if (!service)
            return false;

        uint8_t* makeButton =
            Read<uint8_t*>(
                service +
                kRoomListMakeButton);
        uint8_t* root =
            makeButton
                ? Read<uint8_t*>(
                      makeButton +
                      kControlParent)
                : nullptr;
        if (!makeButton || !root ||
            !root[kControlVisible] ||
            !makeButton[kControlVisible] ||
            !makeButton[kControlEnabled] ||
            !ControlHasName(
                makeButton,
                L"MakeRoomBtn") ||
            FindControl(
                root,
                L"MakeRoomBtn",
                4) != makeButton)
            return false;

        uint8_t* begin =
            Read<uint8_t*>(
                service +
                kRoomListVectorBegin);
        uint8_t* end =
            Read<uint8_t*>(
                service +
                kRoomListVectorEnd);
        if (!begin || !end ||
            reinterpret_cast<uintptr_t>(end) <
                reinterpret_cast<uintptr_t>(begin))
            return false;
        const uintptr_t bytes =
            static_cast<uintptr_t>(
                end - begin);
        if (bytes == 0 ||
            bytes % kRoomEntryStride != 0)
            return false;
        const uint32_t count =
            static_cast<uint32_t>(
                bytes / kRoomEntryStride);
        if (count == 0 ||
            count > kMaximumRoomRows)
            return false;

        int matchedIndex = -1;
        for (uint32_t index = 0;
             index < count;
             ++index) {
            const uint8_t* entry =
                begin +
                index *
                    kRoomEntryStride;
            if (!EntryTitleEquals(
                    entry,
                    title))
                continue;
            if (matchedIndex >= 0)
                return false;
            matchedIndex =
                static_cast<int>(index);
        }
        if (matchedIndex < 0)
            return false;

        uint8_t* entry =
            begin +
            static_cast<size_t>(
                matchedIndex) *
                kRoomEntryStride;
        const uint32_t roomId =
            Read<uint32_t>(entry);
        if (roomId == 0 ||
            Read<uint32_t>(entry + 0x21) != 0 ||
            entry[0x19] != 0 ||
            entry[0x8F] != 0)
            return false;

        uint8_t* control =
            Read<uint8_t*>(
                service +
                kRoomListRowBank +
                static_cast<size_t>(
                    matchedIndex) *
                    0x0C);
        if (!control ||
            Read<uint8_t*>(
                control +
                kControlParent) != root ||
            !control[kControlVisible] ||
            !control[kControlEnabled] ||
            !Read<void*>(
                control +
                kControlClickBusiness))
            return false;

        wchar_t rowName[16] = {};
        const wchar_t prefix[] =
            L"GameRoom";
        std::memcpy(
            rowName,
            prefix,
            sizeof(prefix));
        rowName[8] =
            static_cast<wchar_t>(
                L'0' +
                matchedIndex);
        rowName[9] = L'\0';
        if (!ControlHasName(
                control,
                rowName) ||
            FindControl(
                root,
                rowName,
                0x0C) != control)
            return false;

        selection.service = service;
        selection.control = control;
        selection.control_name =
            control +
            kControlName;
        selection.room_id = roomId;
        return true;
    }

    uint8_t* ResolveUniqueRoomList() const
    {
        void* managed =
            serviceManagedPointer_
                ? *serviceManagedPointer_
                : nullptr;
        if (!managed)
            return nullptr;
        uint8_t* manager =
            static_cast<uint8_t*>(
                managed);
        uint8_t* head =
            Read<uint8_t*>(
                manager + 0x44);
        if (!head)
            return nullptr;
        uint8_t* node =
            Read<uint8_t*>(head);
        uint8_t* found = nullptr;
        uint32_t count = 0;
        while (node && node != head &&
               count <
                   kMaximumServiceNodes) {
            uint8_t* service =
                Read<uint8_t*>(
                    node + 8);
            if (service &&
                Read<void*>(service) ==
                    roomListVtable_) {
                if (found)
                    return nullptr;
                found = service;
            }
            node =
                Read<uint8_t*>(node);
            ++count;
        }
        if (node != head)
            return nullptr;
        return found;
    }

    void* FindControl(
        void* root,
        const wchar_t* name,
        uint32_t kind) const
    {
        if (!root || !name)
            return nullptr;
        alignas(4) uint8_t nativeName[
            kNativeWStringSize] = {};
        bool constructed = false;
        void* result = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (constructed)
                result =
                    nativeUiFindControl_(
                        root,
                        nativeName,
                        kind);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    nativeName);
        }
        return result;
    }

    static bool ControlHasName(
        const void* control,
        const wchar_t* expected)
    {
        if (!control || !expected)
            return false;
        const uint8_t* storage =
            static_cast<const uint8_t*>(
                control) +
            kControlName;
        const uint32_t length =
            Read<uint32_t>(
                storage +
                kNativeStringLength);
        const uint32_t capacity =
            Read<uint32_t>(
                storage +
                kNativeStringCapacity);
        const size_t expectedLength =
            std::wcslen(expected);
        if (length != expectedLength ||
            capacity < length)
            return false;
        const wchar_t* value =
            capacity < 8
                ? reinterpret_cast<
                      const wchar_t*>(
                      storage)
                : Read<const wchar_t*>(
                      storage);
        return value &&
               std::wmemcmp(
                   value,
                   expected,
                   expectedLength) == 0;
    }

    bool ResolveReadyContext(
        ReadyContext& context) const
    {
        std::memset(
            &context,
            0,
            sizeof(context));
        if (InterlockedCompareExchange(
                &joinResult_,
                0,
                0) != kJoinAccepted)
            return false;

        uint8_t* profile =
            profilePointer_
                ? *profilePointer_
                : nullptr;
        uint8_t* room = ResolvedRoom(profile);
        if (!profile || !room ||
            isRoomMaster_(room))
            return false;
        const uint32_t roomId =
            Read<uint32_t>(room);
        if (roomId == 0 ||
            roomId !=
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &expectedRoomId_,
                        0,
                        0)))
            return false;

        uint8_t* app =
            gameApp_ ? gameApp_()
                     : nullptr;
        if (!app ||
            Read<uint32_t>(
                app +
                kGameAppState) !=
                kThreeDWaitRoomAppState)
            return false;
        uint8_t* owner =
            Read<uint8_t*>(
                app +
                kGameAppStateOwner);
        if (!owner ||
            Read<void*>(owner) !=
                threeDWaitRoomVtable_)
            return false;
        uint8_t* manager =
            Read<uint8_t*>(
                owner + 4);
        if (!manager ||
            !IsSupportedManagerVtable(
                Read<void*>(manager)))
            return false;

        uint8_t** states =
            Read<uint8_t**>(
                manager +
                kManagerStateArray);
        uint8_t* command =
            states
                ? states[
                      kReadyCommandIndex]
                : nullptr;
        if (!command ||
            Read<void*>(command) !=
                readyCommandVtable_ ||
            Read<uint32_t>(
                command + 4) !=
                kReadyCommandIndex ||
            Read<uint8_t*>(
                command +
                0x0C) !=
                manager +
                    kManagerCommandContext ||
            Read<uint8_t*>(
                command +
                0x10) !=
                manager)
            return false;

        const uint32_t localSlot =
            Read<uint32_t>(
                manager +
                kManagerLocalSlot);
        if (localSlot >= 12)
            return false;

        uint8_t* waitRoom =
            Read<uint8_t*>(
                manager +
                kManagerWaitRoom);
        if (!waitRoom)
            return false;

        bool ready = false;
        if (!ReadNativeReadyFlag(
                waitRoom,
                localSlot,
                ready))
            return false;

        context.manager = manager;
        context.command = command;
        context.wait_room = waitRoom;
        context.room_id = roomId;
        context.local_slot =
            static_cast<uint8_t>(
                localSlot);
        context.ready = ready;
        return true;
    }

    bool ReadNativeReadyFlag(
        uint8_t* waitRoom,
        uint32_t localSlot,
        bool& ready) const
    {
        ready = false;
        if (!waitRoom ||
            localSlot >= 12)
            return false;
        const uint8_t nativeReady =
            Read<uint8_t>(
                waitRoom +
                kWaitRoomSlotReady +
                static_cast<size_t>(
                    localSlot) *
                    kWaitRoomSlotStride);
        if (nativeReady > 1)
            return false;
        ready = nativeReady != 0;
        return true;
    }

    bool IsSupportedManagerVtable(
        void* vtable) const
    {
        return vtable ==
                   managerVtables_[0] ||
               vtable ==
                   managerVtables_[1] ||
               vtable ==
                   managerVtables_[2];
    }

    void RefreshJoinAcceptance() const
    {
        if (InterlockedCompareExchange(
                &joinPending_,
                0,
                0) == 0 ||
            InterlockedCompareExchange(
                &joinResult_,
                0,
                0) != kJoinPending)
            return;
        const uint32_t expected =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &expectedRoomId_,
                    0,
                    0));
        if (expected == 0 ||
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &acknowledgedRoomId_,
                    0,
                    0)) != expected ||
            InterlockedCompareExchange(
                &joinOriginalCompleted_,
                0,
                0) == 0)
            return;

        __try {
            uint8_t* profile =
                profilePointer_
                    ? *profilePointer_
                    : nullptr;
            uint8_t* room = ResolvedRoom(profile);
            if (!profile || !room ||
                Read<uint32_t>(room) !=
                    expected ||
                isRoomMaster_(room))
                return;
            uint8_t* app =
                gameApp_ ? gameApp_()
                         : nullptr;
            if (!app ||
                Read<uint32_t>(
                    app +
                    kGameAppState) !=
                    kThreeDWaitRoomAppState)
                return;
            uint8_t* owner =
                Read<uint8_t*>(
                    app +
                    kGameAppStateOwner);
            if (!owner ||
                Read<void*>(owner) !=
                    threeDWaitRoomVtable_ ||
                !Read<void*>(owner + 4))
                return;

            if (InterlockedCompareExchange(
                    &joinPending_,
                    0,
                    1) != 1)
                return;
            InterlockedExchangePointer(
                &pendingRoomList_,
                nullptr);
            InterlockedIncrement(
                &joinWriteVersion_);
            InterlockedExchange(
                &modelRoomId_,
                static_cast<LONG>(
                    expected));
            InterlockedExchange(
                &joinLocalGuest_,
                1);
            InterlockedExchange(
                &joinResult_,
                kJoinAccepted);
            InterlockedIncrement(
                &joinSequence_);
            InterlockedIncrement(
                &joinWriteVersion_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void ResetJoinObservation()
    {
        InterlockedExchange(
            &joinPending_,
            0);
        InterlockedExchange(
            &joinWriteVersion_,
            0);
        InterlockedExchange(
            &joinSequence_,
            0);
        InterlockedExchange(
            &joinResult_,
            kJoinNone);
        InterlockedExchange(
            &acknowledgedRoomId_,
            0);
        InterlockedExchange(
            &modelRoomId_,
            0);
        InterlockedExchange(
            &joinReply_,
            0);
        InterlockedExchange(
            &joinOriginalCompleted_,
            0);
        InterlockedExchange(
            &joinLocalGuest_,
            0);
    }

    void ResetReadyObservation()
    {
        InterlockedExchange(
            &readyPending_,
            0);
        InterlockedExchange(
            &readyWriteVersion_,
            0);
        InterlockedExchange(
            &readySequence_,
            0);
        InterlockedExchange(
            &readyResult_,
            kReadyNone);
        InterlockedExchange(
            &readyRoomId_,
            0);
        InterlockedExchange(
            &readyLocalSlot_,
            0xFF);
        InterlockedExchange(
            &readyReplyDetails_,
            0);
        InterlockedExchange(
            &readyReplyDetail_,
            0);
        InterlockedExchange(
            &readyServerReplyObserved_,
            0);
        InterlockedExchange(
            &readyOriginalCompleted_,
            0);
        InterlockedExchange(
            &readyModelReady_,
            0);
        InterlockedExchange(
            &readyNativeBusinessReady_,
            0);
        InterlockedExchangePointer(
            &pendingReadyCommand_,
            nullptr);
        InterlockedExchangePointer(
            &pendingReadyWaitRoom_,
            nullptr);
    }

    void RejectPendingJoin(
        uint16_t opcode,
        uint32_t acknowledgedRoomId,
        uint8_t variant,
        bool originalCompleted = false)
    {
        if (InterlockedCompareExchange(
                &joinPending_,
                0,
                1) != 1)
            return;
        InterlockedExchangePointer(
            &pendingRoomList_,
            nullptr);
        InterlockedIncrement(
            &joinWriteVersion_);
        InterlockedExchange(
            &acknowledgedRoomId_,
            static_cast<LONG>(
                acknowledgedRoomId));
        InterlockedExchange(
            &joinReply_,
            static_cast<LONG>(
                static_cast<uint32_t>(
                    opcode) |
                (static_cast<uint32_t>(
                     variant)
                 << 16)));
        InterlockedExchange(
            &joinOriginalCompleted_,
            originalCompleted ? 1 : 0);
        InterlockedExchange(
            &joinResult_,
            kJoinRejected);
        InterlockedIncrement(
            &joinSequence_);
        InterlockedIncrement(
            &joinWriteVersion_);
    }

    void RejectPendingReady(
        uint16_t opcode,
        uint8_t localSlot,
        uint8_t readyFlag)
    {
        if (InterlockedCompareExchange(
                &readyPending_,
                0,
                1) != 1)
            return;
        InterlockedExchangePointer(
            &pendingReadyCommand_,
            nullptr);
        InterlockedExchangePointer(
            &pendingReadyWaitRoom_,
            nullptr);
        InterlockedIncrement(
            &readyWriteVersion_);
        InterlockedExchange(
            &readyReplyDetails_,
            static_cast<LONG>(
                static_cast<uint32_t>(
                    opcode) |
                (static_cast<uint32_t>(
                     localSlot)
                 << 16) |
                (static_cast<uint32_t>(
                     readyFlag)
                 << 24)));
        InterlockedExchange(
            &readyResult_,
            kReadyRejected);
        InterlockedIncrement(
            &readySequence_);
        InterlockedIncrement(
            &readyWriteVersion_);
    }

    static int __fastcall ObserveRoomListPacket(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        RoomJoinActions* actions =
            active_;
        if (!actions ||
            !actions->roomListPacket_) {
            PacketDispatchFn passive =
                passiveRoomListPacket_;
            return passive
                ? passive(
                      self,
                      a,
                      b,
                      c,
                      opcode,
                      packet)
                : 0;
        }

        const uint16_t normalized =
            static_cast<uint16_t>(
                opcode & 0xFFFF);
        uint32_t roomId = 0;
        uint8_t variant = 0;
        bool owned = false;
        if (normalized ==
                kRoomListReplyOpcode &&
            packet &&
            InterlockedCompareExchange(
                &actions->joinPending_,
                0,
                0) != 0 &&
            InterlockedCompareExchangePointer(
                &actions->pendingRoomList_,
                nullptr,
                nullptr) == self) {
            __try {
                roomId =
                    Read<uint32_t>(
                        static_cast<uint8_t*>(
                            packet) +
                        6);
                variant =
                    *(static_cast<uint8_t*>(
                          packet) +
                      10);
                owned = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                owned = false;
            }
        }

        const int result =
            actions->roomListPacket_(
                self,
                a,
                b,
                c,
                opcode,
                packet);
        if (!owned)
            return result;

        const uint32_t expected =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &actions->expectedRoomId_,
                    0,
                    0));
        if (roomId == 0 ||
            roomId != expected) {
            actions->RejectPendingJoin(
                normalized,
                roomId,
                variant,
                true);
            return result;
        }

        InterlockedIncrement(
            &actions->joinWriteVersion_);
        InterlockedExchange(
            &actions->acknowledgedRoomId_,
            static_cast<LONG>(roomId));
        InterlockedExchange(
            &actions->joinReply_,
            static_cast<LONG>(
                static_cast<uint32_t>(
                    normalized) |
                (static_cast<uint32_t>(
                     variant)
                 << 16)));
        InterlockedExchange(
            &actions->
                 joinOriginalCompleted_,
            1);
        InterlockedIncrement(
            &actions->joinSequence_);
        InterlockedIncrement(
            &actions->joinWriteVersion_);
        actions->RefreshJoinAcceptance();
        return result;
    }

    static void __fastcall ObserveWaitRoomPacket(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        RoomJoinActions* actions =
            active_;
        if (!actions ||
            !actions->waitRoomDispatch_) {
            WaitRoomDispatchFn passive =
                passiveWaitRoomDispatch_;
            if (passive) {
                passive(
                    self,
                    a,
                    b,
                    c,
                    opcode,
                    packet);
            }
            return;
        }

        const uint16_t normalized =
            static_cast<uint16_t>(
                opcode & 0xFFFF);
        uint8_t slot = 0xFF;
        uint8_t flag = 0;
        uint8_t detail = 0;
        bool owned = false;
        if (normalized ==
                kReadyReplyOpcode &&
            packet &&
            InterlockedCompareExchange(
                &actions->readyPending_,
                0,
                0) != 0 &&
            InterlockedCompareExchangePointer(
                &actions->
                     pendingReadyWaitRoom_,
                nullptr,
                nullptr) == self) {
            __try {
                ReadyContext context = {};
                uint8_t* bytes =
                    static_cast<uint8_t*>(
                        packet);
                slot = bytes[6];
                flag = bytes[7];
                detail = bytes[8];
                owned =
                    actions->
                        ResolveReadyContext(
                            context) &&
                    context.wait_room == self &&
                    context.command ==
                        InterlockedCompareExchangePointer(
                            &actions->
                                 pendingReadyCommand_,
                            nullptr,
                            nullptr) &&
                    context.room_id ==
                        static_cast<uint32_t>(
                            InterlockedCompareExchange(
                                &actions->
                                     readyRoomId_,
                                0,
                                0)) &&
                    context.local_slot ==
                        slot &&
                    context.local_slot ==
                        static_cast<uint8_t>(
                            InterlockedCompareExchange(
                                &actions->
                                     readyLocalSlot_,
                                0,
                                0));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                owned = false;
            }
        }

        actions->waitRoomDispatch_(
            self,
            a,
            b,
            c,
            opcode,
            packet);
        if (!owned)
            return;

        bool modelReady = false;
        bool sameContext = false;
        __try {
            ReadyContext after = {};
            sameContext =
                actions->
                    ResolveReadyContext(
                        after) &&
                after.wait_room == self &&
                after.command ==
                    InterlockedCompareExchangePointer(
                        &actions->
                             pendingReadyCommand_,
                        nullptr,
                        nullptr) &&
                after.room_id ==
                    static_cast<uint32_t>(
                        InterlockedCompareExchange(
                            &actions->
                                 readyRoomId_,
                            0,
                            0)) &&
                after.local_slot == slot;
            modelReady =
                sameContext &&
                after.ready;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sameContext = false;
            modelReady = false;
        }

        if (InterlockedCompareExchange(
                &actions->readyPending_,
                0,
                1) != 1)
            return;
        InterlockedExchangePointer(
            &actions->pendingReadyCommand_,
            nullptr);
        InterlockedExchangePointer(
            &actions->pendingReadyWaitRoom_,
            nullptr);
        InterlockedIncrement(
            &actions->readyWriteVersion_);
        InterlockedExchange(
            &actions->readyReplyDetails_,
            static_cast<LONG>(
                static_cast<uint32_t>(
                    normalized) |
                (static_cast<uint32_t>(
                     slot)
                 << 16) |
                (static_cast<uint32_t>(
                     flag)
                 << 24)));
        InterlockedExchange(
            &actions->readyReplyDetail_,
            detail);
        InterlockedExchange(
            &actions->
                 readyServerReplyObserved_,
            1);
        InterlockedExchange(
            &actions->
                 readyOriginalCompleted_,
            1);
        InterlockedExchange(
            &actions->readyModelReady_,
            modelReady ? 1 : 0);
        InterlockedExchange(
            &actions->readyResult_,
            flag == 1 &&
                    sameContext &&
                    modelReady
                ? kReadyAccepted
                : kReadyRejected);
        InterlockedIncrement(
            &actions->readySequence_);
        InterlockedIncrement(
            &actions->readyWriteVersion_);
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    GameAppFn gameApp_ = nullptr;
    uint8_t* ResolvedRoom(uint8_t* profile) const {
        if(roomResolver_)return const_cast<uint8_t*>(roomResolver_(roomOpaque_));
        return profile ? Read<uint8_t*>(profile+kProfileRoom):nullptr;
    }
    RoomResolver roomResolver_=nullptr;
    void* roomOpaque_=nullptr;
    uint8_t** profilePointer_ = nullptr;
    IsRoomMasterFn isRoomMaster_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    void* roomListVtable_ = nullptr;
    RoomJoinCallbackFn roomJoinCallback_ = nullptr;
    PacketDispatchFn roomListPacket_ = nullptr;
    void* threeDWaitRoomVtable_ = nullptr;
    void* readyCommandVtable_ = nullptr;
    ReadyBusinessFn readyBusiness_ = nullptr;
    WaitRoomDispatchFn waitRoomDispatch_ = nullptr;
    void* managerVtables_[3] = {};
    bool bound_ = false;
    char expectedTitle_[21] = {};

    mutable volatile LONG joinPending_ = 0;
    mutable volatile LONG joinWriteVersion_ = 0;
    mutable volatile LONG joinSequence_ = 0;
    mutable volatile LONG joinResult_ = kJoinNone;
    mutable volatile LONG expectedRoomId_ = 0;
    mutable volatile LONG acknowledgedRoomId_ = 0;
    mutable volatile LONG modelRoomId_ = 0;
    mutable volatile LONG joinReply_ = 0;
    mutable volatile LONG joinOriginalCompleted_ = 0;
    mutable volatile LONG joinLocalGuest_ = 0;
    mutable PVOID volatile pendingRoomList_ = nullptr;

    mutable volatile LONG readyPending_ = 0;
    mutable volatile LONG readyWriteVersion_ = 0;
    mutable volatile LONG readySequence_ = 0;
    mutable volatile LONG readyResult_ = kReadyNone;
    mutable volatile LONG readyRoomId_ = 0;
    mutable volatile LONG readyLocalSlot_ = 0xFF;
    mutable volatile LONG readyReplyDetails_ = 0;
    mutable volatile LONG readyReplyDetail_ = 0;
    mutable volatile LONG readyServerReplyObserved_ = 0;
    mutable volatile LONG readyOriginalCompleted_ = 0;
    mutable volatile LONG readyModelReady_ = 0;
    mutable volatile LONG readyNativeBusinessReady_ = 0;
    mutable PVOID volatile pendingReadyCommand_ = nullptr;
    mutable PVOID volatile pendingReadyWaitRoom_ = nullptr;

    inline static RoomJoinActions* active_ = nullptr;
    inline static PacketDispatchFn
        passiveRoomListPacket_ = nullptr;
    inline static WaitRoomDispatchFn
        passiveWaitRoomDispatch_ = nullptr;
};
