// OFFLINE ONLY.
//
// Win32 ABI/state-machine regression for FriendInviteReceiver. The fixture
// calls modeled original functions through the installed detours. It does not
// start GOnline, contact a server, inject a packet, or claim native gameplay.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../include/sdgo/sdgo_api.h"

#define private public
#include "../mods/game_test_client/native_actions.h"
#include "../mods/game_test_client/friend_invite_receiver.h"
#undef private

namespace
{
static_assert(
    sizeof(void*) == 4,
    "friend-invite receiver ABI test must be compiled as Win32");

constexpr uint16_t kChannelId = 0x0137;
constexpr uint32_t kRoomId = 0x1234ABCD;
constexpr uint32_t kInviterId = 0x10203040;
constexpr char kInviterName[] = "Inviter";
constexpr wchar_t kInviterWide[] = L"Inviter";

using PacketDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);
using PromptCtorDetour =
    void*(__fastcall*)(
        void*,
        void*,
        const void*);
using PromptCloneDetour =
    void*(__fastcall*)(void*, void*);
using PromptInitializeDetour =
    void(__fastcall*)(void*, void*, int);
using PromptDtorDetour =
    void(__fastcall*)(void*, void*);
using UiFindDetour =
    void*(__fastcall*)(
        void*,
        void*,
        const void*,
        uint32_t);

int g_failures = 0;
PacketDetour g_packetDetour = nullptr;
PromptCtorDetour g_promptCtorDetour = nullptr;
PromptCloneDetour g_promptCloneDetour = nullptr;
PromptInitializeDetour g_promptInitializeDetour = nullptr;
PromptDtorDetour g_promptDtorDetour = nullptr;
UiFindDetour g_uiFindDetour = nullptr;

NativeActions* g_native = nullptr;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x1000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_managedPrompt[0x94] = {};
uint8_t g_layoutRoot[0x240] = {};
uint8_t g_allowControl[0x240] = {};
uint8_t g_refuseControl[0x240] = {};
uint8_t g_room[0x100] = {};
uint8_t g_waitOwner[0x20] = {};
uint8_t g_manager[0x80] = {};

void* g_promptVtable[32] = {};
void* g_waitOwnerVtable[16] = {};
void* g_managerVtables[3][16] = {};

unsigned g_callbackCalls = 0;
unsigned g_dtorCalls = 0;
bool g_roomMaster = false;

void Check(
    bool condition,
    const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

template <typename T>
void Put(
    void* base,
    size_t offset,
    T value)
{
    std::memcpy(
        static_cast<uint8_t*>(base) + offset,
        &value,
        sizeof(value));
}

template <typename T>
T Get(
    const void* base,
    size_t offset)
{
    T value = {};
    std::memcpy(
        &value,
        static_cast<const uint8_t*>(base) + offset,
        sizeof(value));
    return value;
}

const wchar_t* NativeStringData(
    const void* storage)
{
    const auto* bytes =
        static_cast<const uint8_t*>(storage);
    return Get<uint32_t>(bytes, 0x14) < 8
        ? reinterpret_cast<const wchar_t*>(bytes)
        : Get<const wchar_t*>(bytes, 0);
}

void SetNativeString(
    void* storage,
    const wchar_t* value)
{
    std::memset(storage, 0, 0x18);
    const uint32_t length =
        static_cast<uint32_t>(std::wcslen(value));
    if (length < 8) {
        std::memcpy(
            storage,
            value,
            (length + 1) * sizeof(wchar_t));
        Put<uint32_t>(storage, 0x14, 7);
    } else {
        Put<const wchar_t*>(storage, 0, value);
        Put<uint32_t>(storage, 0x14, length);
    }
    Put<uint32_t>(storage, 0x10, length);
}

void PublishTransport(
    uint16_t opcode,
    uint32_t size,
    LONG result = 1)
{
    InterlockedIncrement(
        &g_native->transportWriteVersion_);
    InterlockedExchange(
        &g_native->transportOpcode_,
        opcode);
    InterlockedExchange(
        &g_native->transportSize_,
        static_cast<LONG>(size));
    InterlockedExchange(
        &g_native->transportResult_,
        result);
    InterlockedIncrement(
        &g_native->transportSequence_);
    InterlockedIncrement(
        &g_native->transportWriteVersion_);
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

bool __stdcall FakeIsRoomMaster(void* room)
{
    return room == g_room && g_roomMaster;
}

void* __fastcall FakePromptCtor(
    void* self,
    void*,
    const void* packet)
{
    auto* prompt =
        static_cast<uint8_t*>(self);
    const auto* bytes =
        static_cast<const uint8_t*>(packet);
    std::memset(prompt, 0, 0x94);
    Put<void*>(prompt, 0, g_promptVtable);
    SetNativeString(
        prompt + 0x4C,
        kInviterWide);
    prompt[0x68] = 1;
    prompt[0x69] = 0;
    prompt[0x6B] = bytes[0x10];
    Put<uint16_t>(
        prompt,
        0x6C,
        0);
    Put<uint16_t>(
        prompt,
        0x6E,
        Get<uint16_t>(bytes, 0x06));
    Put<uint32_t>(
        prompt,
        0x70,
        Get<uint32_t>(bytes, 0x08));
    Put<uint16_t>(
        prompt,
        0x74,
        0);
    Put<uint32_t>(
        prompt,
        0x78,
        Get<uint32_t>(bytes, 0x0C));
    prompt[0x7C] = bytes[0x23];
    std::memcpy(
        prompt + 0x7D,
        bytes + 0x24,
        20);
    return self;
}

void* __fastcall FakePromptClone(
    void* self,
    void*)
{
    std::memcpy(
        g_managedPrompt,
        self,
        sizeof(g_managedPrompt));
    return g_managedPrompt;
}

void* __fastcall FakePromptCloneTrampoline(
    void* self,
    void* reserved)
{
    return FakePromptClone(self, reserved);
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    if (root != g_layoutRoot ||
        kind != 4 ||
        !nativeName)
        return nullptr;
    const wchar_t* name =
        NativeStringData(nativeName);
    if (!name)
        return nullptr;
    if (std::wcscmp(name, L"AllowBtn") == 0)
        return g_allowControl;
    if (std::wcscmp(name, L"RefuseBtn") == 0)
        return g_refuseControl;
    return nullptr;
}

void __fastcall FakePromptInitialize(
    void* self,
    void*,
    int)
{
    alignas(4) uint8_t allowName[0x18] = {};
    alignas(4) uint8_t refuseName[0x18] = {};
    SetNativeString(allowName, L"AllowBtn");
    SetNativeString(refuseName, L"RefuseBtn");
    void* allow =
        g_uiFindDetour(
            g_layoutRoot,
            nullptr,
            allowName,
            4);
    void* refuse =
        g_uiFindDetour(
            g_layoutRoot,
            nullptr,
            refuseName,
            4);
    Check(
        allow == g_allowControl &&
            refuse == g_refuseControl,
        "initializer resolved the modeled original AllowBtn and RefuseBtn");
    Put<uint32_t>(self, 0x44, 1);
}

void __fastcall FakePromptInitializeTrampoline(
    void* self,
    void* reserved,
    int compare)
{
    FakePromptInitialize(
        self,
        reserved,
        compare);
}

void __fastcall FakePromptDtor(
    void* self,
    void*)
{
    ++g_dtorCalls;
    (void)self;
}

uint32_t __stdcall FakePromptCallback(
    void* prompt,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_callbackCalls;
    Check(
        prompt == g_managedPrompt &&
            event == 0x101 &&
            nativeName ==
                static_cast<uint8_t*>(control) +
                    0x118,
        "decision used the exact live prompt/control callback arguments");

    const wchar_t* name =
        NativeStringData(nativeName);
    if (name &&
        std::wcscmp(name, L"AllowBtn") == 0) {
        g_profileStorage[0x860] = 1;
        std::memcpy(
            g_profileStorage + 0x864,
            g_managedPrompt + 0x6C,
            40);
        PublishTransport(0x04A8, 0x11);
    } else if (
        name &&
        std::wcscmp(name, L"RefuseBtn") == 0) {
        PublishTransport(0x07A9, 0x11);
    }
    g_promptDtorDetour(prompt, nullptr);
    return 0;
}

void __fastcall FakePromptLayout(
    void*,
    void*)
{
}

int __fastcall FakePacketDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    if (static_cast<uint16_t>(opcode) == 0x059A) {
        alignas(4) uint8_t source[0x94] = {};
        g_promptCtorDetour(
            source,
            nullptr,
            packet);
        void* managed =
            g_promptCloneDetour(
                source,
                nullptr);
        g_promptInitializeDetour(
            managed,
            nullptr,
            1);
        g_promptDtorDetour(
            source,
            nullptr);
    } else if (
        static_cast<uint16_t>(opcode) ==
            0x05AB &&
        g_profileStorage[0x860] == 1) {
        PublishTransport(0x0734, 0x32);
    }
    return 77;
}

void* ApiSymbol(
    SdgoMod*,
    const char* name)
{
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(
            &FakeGameApp);
    if (std::strcmp(
            name,
            "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(
            name,
            "flow.is_room_master") == 0)
        return reinterpret_cast<void*>(
            &FakeIsRoomMaster);
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeUiFindControl);
    if (std::strcmp(
            name,
            "friends.invite_receiver_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakePacketDispatch);
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakePromptCtor);
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_clone") == 0)
        return reinterpret_cast<void*>(
            &FakePromptClone);
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_initialize") == 0)
        return reinterpret_cast<void*>(
            &FakePromptInitialize);
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakePromptDtor);
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_vtable") == 0)
        return g_promptVtable;
    if (std::strcmp(
            name,
            "friends.invite_receiver_ui_callback") == 0)
        return reinterpret_cast<void*>(
            &FakePromptCallback);
    if (std::strcmp(
            name,
            "flow.three_d_wait_room_vtable") == 0)
        return g_waitOwnerVtable;
    if (std::strcmp(
            name,
            "flow.normal_3d_manager_vtable") == 0)
        return g_managerVtables[0];
    if (std::strcmp(
            name,
            "flow.ai_3d_manager_vtable") == 0)
        return g_managerVtables[1];
    if (std::strcmp(
            name,
            "flow.battleship_3d_manager_vtable") == 0)
        return g_managerVtables[2];
    return nullptr;
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    if (!name || !detour || !original)
        return 0;
    if (std::strcmp(
            name,
            "friends.invite_receiver_dispatch") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakePacketDispatch);
        g_packetDetour =
            reinterpret_cast<PacketDetour>(
                detour);
        return 1;
    }
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_ctor") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakePromptCtor);
        g_promptCtorDetour =
            reinterpret_cast<PromptCtorDetour>(
                detour);
        return 1;
    }
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_clone") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakePromptCloneTrampoline);
        g_promptCloneDetour =
            reinterpret_cast<PromptCloneDetour>(
                detour);
        return 1;
    }
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_initialize") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakePromptInitializeTrampoline);
        g_promptInitializeDetour =
            reinterpret_cast<PromptInitializeDetour>(
                detour);
        return 1;
    }
    if (std::strcmp(
            name,
            "friends.invite_receiver_prompt_dtor") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakePromptDtor);
        g_promptDtorDetour =
            reinterpret_cast<PromptDtorDetour>(
                detour);
        return 1;
    }
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0) {
        *original =
            reinterpret_cast<void*>(
                &FakeUiFindControl);
        g_uiFindDetour =
            reinterpret_cast<UiFindDetour>(
                detour);
        return 1;
    }
    return 0;
}

void ResetFixture()
{
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_managedPrompt,
        0,
        sizeof(g_managedPrompt));
    std::memset(
        g_layoutRoot,
        0,
        sizeof(g_layoutRoot));
    std::memset(
        g_allowControl,
        0,
        sizeof(g_allowControl));
    std::memset(
        g_refuseControl,
        0,
        sizeof(g_refuseControl));
    std::memset(g_room, 0, sizeof(g_room));
    std::memset(
        g_waitOwner,
        0,
        sizeof(g_waitOwner));
    std::memset(
        g_manager,
        0,
        sizeof(g_manager));

    Put<uint32_t>(g_app, 0x4A0, 6);
    g_layoutRoot[0x162] = 1;
    SetNativeString(
        g_allowControl + 0x118,
        L"AllowBtn");
    SetNativeString(
        g_refuseControl + 0x118,
        L"RefuseBtn");
    Put<void*>(
        g_allowControl,
        0x15C,
        g_layoutRoot);
    Put<void*>(
        g_refuseControl,
        0x15C,
        g_layoutRoot);
    g_allowControl[0x162] = 1;
    g_allowControl[0x163] = 1;
    g_allowControl[0x167] = 1;
    g_refuseControl[0x162] = 1;
    g_refuseControl[0x163] = 1;
    g_refuseControl[0x167] = 1;
    g_roomMaster = false;
}

void BuildInvitePacket(
    uint8_t (&packet)[0x38])
{
    std::memset(packet, 0, sizeof(packet));
    Put<uint16_t>(packet, 0, 0x059A);
    Put<uint16_t>(
        packet,
        0x06,
        kChannelId);
    Put<uint32_t>(
        packet,
        0x08,
        kRoomId);
    Put<uint32_t>(
        packet,
        0x0C,
        kInviterId);
    packet[0x10] = 0x5A;
    packet[0x11] =
        static_cast<uint8_t>(
            std::strlen(kInviterName));
    std::memcpy(
        packet + 0x12,
        kInviterName,
        sizeof(kInviterName));
    packet[0x23] = 0;
}

friends::FriendInviteReceiver::ExpectedInvite Expected(
    friends::FriendInviteReceiver::Decision decision)
{
    friends::FriendInviteReceiver::ExpectedInvite expected = {};
    expected.channel_id = kChannelId;
    expected.room_id = kRoomId;
    expected.inviter_id = kInviterId;
    expected.decision =
        static_cast<uint8_t>(decision);
    expected.inviter_name = kInviterName;
    return expected;
}

void ConfigureVtables()
{
    std::memset(
        g_promptVtable,
        0,
        sizeof(g_promptVtable));
    g_promptVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakePromptCallback);
    g_promptVtable[0x3C / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakePromptInitialize);
    g_promptVtable[0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakePromptClone);
    g_promptVtable[0x58 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakePromptLayout);
}

void PublishJoinedRoom()
{
    Put<uint32_t>(g_room, 0, kRoomId);
    Put<uint8_t*>(
        g_profileStorage,
        0xEC,
        g_room);
    Put<uint32_t>(g_app, 0x4A0, 18);
    Put<void*>(
        g_waitOwner,
        0,
        g_waitOwnerVtable);
    Put<uint8_t*>(
        g_waitOwner,
        4,
        g_manager);
    Put<void*>(
        g_manager,
        0,
        g_managerVtables[0]);
    Put<uint8_t*>(
        g_app,
        0x4A4,
        g_waitOwner);
}

} // namespace

int main()
{
    ConfigureVtables();

    SdgoApi api = {};
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;
    auto* mod =
        reinterpret_cast<SdgoMod*>(
            static_cast<uintptr_t>(1));

    NativeActions native;
    native.gameApp_ = &FakeGameApp;
    g_native = &native;

    friends::FriendInviteReceiver receiver;
    Check(
        receiver.Bind(&api, mod, &native),
        "receiver bound all exact native observers");
    Check(
        g_packetDetour &&
            g_promptCtorDetour &&
            g_promptCloneDetour &&
            g_promptInitializeDetour &&
            g_promptDtorDetour &&
            g_uiFindDetour,
        "receiver installed every required passive detour");

    uint8_t invite[0x38] = {};
    BuildInvitePacket(invite);

    ResetFixture();
    g_allowControl[0x163] = 0;
    Check(
        receiver.Arm(
            Expected(
                friends::FriendInviteReceiver::
                    kReject)),
        "disabled-control attempt armed with exact invitation identity");
    g_packetDetour(
        nullptr,
        nullptr,
        0,
        0,
        0,
        0x059A,
        invite);
    auto disabled = receiver.State();
    Check(
        disabled.phase ==
                friends::FriendInviteReceiver::
                    kFailed &&
            disabled.failure ==
                friends::FriendInviteReceiver::
                    kPromptUiMismatch &&
            !disabled.allow_control_enabled &&
            disabled.allow_control_hit &&
            !disabled.controls_ready,
        "visible and hit-testable control is rejected when native enabled +163 is false");
    g_promptDtorDetour(g_managedPrompt, nullptr);

    ResetFixture();
    g_refuseControl[0x167] = 0;
    Check(
        receiver.Arm(
            Expected(
                friends::FriendInviteReceiver::
                    kReject)),
        "non-hit control attempt rearmed after failed prompt cleanup");
    g_packetDetour(
        nullptr,
        nullptr,
        0,
        0,
        0,
        0x059A,
        invite);
    auto nonHit = receiver.State();
    Check(
        nonHit.phase ==
                friends::FriendInviteReceiver::
                    kFailed &&
            nonHit.failure ==
                friends::FriendInviteReceiver::
                    kPromptUiMismatch &&
            nonHit.refuse_control_enabled &&
            !nonHit.refuse_control_hit &&
            !nonHit.controls_ready,
        "visible and enabled control is rejected when native hit-selection +167 is false");
    g_promptDtorDetour(g_managedPrompt, nullptr);

    ResetFixture();
    Check(
        receiver.Arm(
            Expected(
                friends::FriendInviteReceiver::
                    kReject)),
        "reject attempt armed with exact inviter/channel/room identity");
    Check(
        g_packetDetour(
            nullptr,
            nullptr,
            0,
            0,
            0,
            0x059A,
            invite) == 77,
        "incoming 0x059A passed through the original dispatcher");
    auto rejected = receiver.State();
    Check(
        rejected.phase ==
                friends::FriendInviteReceiver::
                    kPromptInitialized &&
            rejected.prompt_constructed &&
            rejected.prompt_clone_created &&
            rejected.prompt_clone_identity_matched &&
            rejected.prompt_initialized &&
            rejected.allow_control_enabled &&
            rejected.refuse_control_enabled &&
            rejected.allow_control_hit &&
            rejected.refuse_control_hit &&
            rejected.controls_ready &&
            rejected.native_flag == 0x5A,
        "real prompt clone and initialized native controls were correlated");
    Check(
        receiver.InvokeDecision(),
        "original RefuseBtn callback emitted a decision");
    rejected = receiver.State();
    Check(
        rejected.phase ==
                friends::FriendInviteReceiver::
                    kRejected &&
            rejected.decision_opcode == 0x07A9 &&
            rejected.decision_packet_size == 0x11 &&
            rejected.decision_transport_result != 0 &&
            rejected.prompt_destroyed,
        "reject required exact 0x07A9/0x11 transport and prompt lifetime end");

    ResetFixture();
    Check(
        receiver.Arm(
            Expected(
                friends::FriendInviteReceiver::
                    kAccept)),
        "accept attempt rearmed after the completed reject");
    g_packetDetour(
        nullptr,
        nullptr,
        0,
        0,
        0,
        0x059A,
        invite);
    Check(
        receiver.InvokeDecision(),
        "original AllowBtn callback emitted an accept decision");
    auto accepted = receiver.State();
    Check(
        accepted.phase ==
                friends::FriendInviteReceiver::
                    kDecisionSent &&
            accepted.decision_opcode == 0x04A8 &&
            accepted.decision_packet_size == 0x11 &&
            accepted.invitation_context_matched,
        "accept required exact 0x04A8/0x11 transport and native profile context");

    g_packetDetour(
        nullptr,
        nullptr,
        0,
        0,
        0,
        0x05AB,
        nullptr);
    accepted = receiver.State();
    Check(
        accepted.phase ==
                friends::FriendInviteReceiver::
                    kJoinRequestSent &&
            accepted.join_continuation_observed &&
            accepted.join_request_sent &&
            accepted.join_opcode == 0x0734 &&
            accepted.join_packet_size == 0x32,
        "original 0x05AB continuation emitted exact 0x0734/0x32 join transport");
    Check(
        accepted.phase !=
            friends::FriendInviteReceiver::
                kRoomJoined,
        "a join request alone was not reported as room completion");

    PublishJoinedRoom();
    accepted = receiver.Refresh();
    Check(
        accepted.phase ==
                friends::FriendInviteReceiver::
                    kRoomJoined &&
            accepted.model_room_id == kRoomId &&
            accepted.room_model_matched &&
            accepted.local_guest &&
            accepted.wait_room_owner_matched &&
            accepted.manager_vtable_matched,
        "completion required the exact native guest room, wait owner, and manager");

    Check(
        g_callbackCalls == 2,
        "exactly one original decision callback ran per attempt");
    Check(
        g_dtorCalls == 8,
        "source and managed prompt destructors followed all native lifetimes");

    friends::FriendInviteReceiver::active_ = nullptr;
    return g_failures == 0 ? 0 : 1;
}
