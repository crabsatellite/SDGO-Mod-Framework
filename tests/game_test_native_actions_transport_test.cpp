// OFFLINE-ONLY transport routing tests for NativeActions.
//
// The fixture models the shipped x86 network object and HookCode contract.
// It never starts GOnline or contacts a server.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../include/sdgo/sdgo_api.h"

#define private public
#include "../mods/game_test_client/native_actions.h"
#undef private

namespace
{
int g_failures = 0;

uint8_t g_network[0x80] = {};
uint8_t g_stream = 0;
uint8_t g_app[0x500] = {};
uint8_t* g_profile = nullptr;

unsigned g_originalCalls = 0;
unsigned g_originalDepth = 0;
unsigned g_maxOriginalDepth = 0;
void* g_lastNetwork = nullptr;
void* g_lastStream = nullptr;
const void* g_lastPacket = nullptr;
int g_lastSize = 0;
char g_lastFlag = 0;
int g_originalResult = 0;
DWORD g_originalError = ERROR_SUCCESS;
bool g_originalSetsError = true;

using NetworkObserverFn = int(__fastcall*)(
    void*,
    void*,
    void*,
    const void*,
    int,
    char);
NetworkObserverFn g_networkObserver = nullptr;
void* g_loginObserver = nullptr;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeLoginPacketCtor(
    void* self,
    void*,
    const char*,
    const char*)
{
    return self;
}

char* __cdecl FakePasswordMd5Hex(
    const void*,
    char* output,
    int)
{
    std::memset(output, '0', 32);
    output[32] = '\0';
    return output;
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void* network,
    void*,
    void* stream,
    const void* packet,
    int size,
    char flag)
{
    ++g_originalCalls;
    ++g_originalDepth;
    if (g_originalDepth > g_maxOriginalDepth)
        g_maxOriginalDepth = g_originalDepth;
    g_lastNetwork = network;
    g_lastStream = stream;
    g_lastPacket = packet;
    g_lastSize = size;
    g_lastFlag = flag;
    if (g_originalSetsError)
        SetLastError(g_originalError);
    --g_originalDepth;
    return g_originalResult;
}

int __fastcall FakeLoginServiceDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 1;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t*)
{
    return self;
}

void* __fastcall FakeNicknameDialogCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(
    void* object,
    int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(
    void*,
    void*)
{
}

void __fastcall FakeNicknameDialogConfirm(
    void*,
    void*)
{
}

void* ApiSymbol(
    SdgoMod*,
    const char* name)
{
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(&FakeGameApp);
    if (std::strcmp(
            name,
            "flow.login_packet_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginPacketCtor);
    if (std::strcmp(
            name,
            "flow.password_md5_hex") == 0)
        return reinterpret_cast<void*>(
            &FakePasswordMd5Hex);
    if (std::strcmp(name, "flow.network") == 0)
        return reinterpret_cast<void*>(&FakeNetwork);
    if (std::strcmp(name, "flow.network_send") == 0)
        return reinterpret_cast<void*>(
            &FakeNetworkSend);
    if (std::strcmp(
            name,
            "flow.login_service_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginServiceDispatch);
    if (std::strcmp(
            name,
            "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(
            name,
            "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringCtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogCtor);
    if (std::strcmp(
            name,
            "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(
            &FakeEnqueueUiObject);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogConfirm);
    return nullptr;
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    if (!detour || !original)
        return 0;
    if (std::strcmp(
            name,
            "flow.login_service_dispatch") == 0) {
        g_loginObserver = detour;
        *original = reinterpret_cast<void*>(
            &FakeLoginServiceDispatch);
        return 1;
    }
    if (std::strcmp(
            name,
            "flow.network_send") == 0) {
        g_networkObserver =
            reinterpret_cast<NetworkObserverFn>(detour);
        *original = reinterpret_cast<void*>(
            &FakeNetworkSend);
        return 1;
    }
    return 0;
}

void ResetCalls()
{
    g_originalCalls = 0;
    g_originalDepth = 0;
    g_maxOriginalDepth = 0;
    g_lastNetwork = nullptr;
    g_lastStream = nullptr;
    g_lastPacket = nullptr;
    g_lastSize = 0;
    g_lastFlag = 0;
}

void ResetNetwork(uint32_t calls, uint32_t bytes)
{
    std::memset(g_network, 0, sizeof(g_network));
    *reinterpret_cast<void**>(g_network + 0x14) =
        &g_stream;
    *reinterpret_cast<uint32_t*>(g_network + 0x58) =
        calls;
    *reinterpret_cast<uint32_t*>(g_network + 0x5C) =
        bytes;
}

void TestFallbackBeforeObserverBinding()
{
    NativeActions actions;
    NativeActions::active_ = nullptr;
    actions.network_ = &FakeNetwork;
    actions.networkSend_ =
        reinterpret_cast<NativeActions::NetworkSendFn>(
            &FakeNetworkSend);
    actions.networkObserverInstalled_ = false;

    ResetCalls();
    ResetNetwork(4, 100);
    const uint8_t packet[] = {
        0x97, 0x06, 0x11, 0x22, 0x33, 0x44,
    };
    g_originalResult = 7;
    g_originalSetsError = false;
    SetLastError(ERROR_RETRY);

    const bool sent =
        actions.SendRequest(packet, sizeof(packet));
    Check(
        sent &&
            g_originalCalls == 1 &&
            g_maxOriginalDepth == 1,
        "unbound SendRequest calls the original transport exactly once");
    Check(
        g_lastNetwork == g_network &&
            g_lastStream == &g_stream &&
            g_lastPacket == packet &&
            g_lastSize ==
                static_cast<int>(sizeof(packet)) &&
            g_lastFlag == 0,
        "fallback preserves all native transport arguments");
    Check(
        *reinterpret_cast<uint32_t*>(
            g_network + 0x58) == 5 &&
            *reinterpret_cast<uint32_t*>(
                g_network + 0x5C) == 106,
        "fallback preserves native request counters");
    Check(
        actions.TransportState().send_sequence == 0,
        "fallback does not fabricate observer telemetry");
    Check(
        GetLastError() == ERROR_RETRY,
        "fallback preserves incoming LastError when original leaves it unchanged");
}

void BindObservedActions(
    NativeActions& actions,
    SdgoApi& api)
{
    NativeActions::active_ = nullptr;
    api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;
    NativeActions::Config config = {};
    config.username = "testuser";
    config.password = "testpass";
    Check(
        actions.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            config),
        "NativeActions binds the network observer");
}

void TestOwnedRequestUsesObserverOnce()
{
    NativeActions actions;
    SdgoApi api = {};
    BindObservedActions(actions, api);
    ResetCalls();
    ResetNetwork(9, 200);
    const uint8_t packet[] = {
        0x97, 0x06, 0xAA, 0xBB, 0xCC, 0xDD,
    };
    g_originalResult = -3;
    g_originalError = ERROR_NETWORK_UNREACHABLE;
    g_originalSetsError = true;
    SetLastError(ERROR_SUCCESS);

    const bool sent =
        actions.SendRequest(packet, sizeof(packet));
    const NativeActions::TransportObservation observed =
        actions.TransportState();
    Check(
        sent &&
            g_originalCalls == 1 &&
            g_maxOriginalDepth == 1,
        "bound SendRequest routes through observer without recursion");
    Check(
        g_lastNetwork == g_network &&
            g_lastStream == &g_stream &&
            g_lastPacket == packet &&
            g_lastSize ==
                static_cast<int>(sizeof(packet)) &&
            g_lastFlag == 0,
        "observed owned request preserves original arguments");
    Check(
        *reinterpret_cast<uint32_t*>(
            g_network + 0x58) == 10 &&
            *reinterpret_cast<uint32_t*>(
                g_network + 0x5C) == 206,
        "observed owned request increments native counters once");
    Check(
        observed.send_sequence == 1 &&
            observed.opcode == 0x0697 &&
            observed.packet_size == sizeof(packet) &&
            observed.result ==
                static_cast<uint32_t>(-3),
        "owned request publishes telemetry from the real original call");
    Check(
        GetLastError() == ERROR_NETWORK_UNREACHABLE,
        "observer preserves the original transport LastError");
}

void TestOtherBusinessCallStillUsesOriginalOnce()
{
    NativeActions actions;
    SdgoApi api = {};
    BindObservedActions(actions, api);
    ResetCalls();
    ResetNetwork(12, 300);
    const uint32_t baseline =
        actions.TransportState().send_sequence;
    const uint8_t requestPacket[] = {
        0x29, 0x08, 0x10, 0x20, 0x30, 0x40,
    };
    const uint8_t closePacket[] = {
        0xFB, 0x07, 0x01, 0x02,
    };

    Check(
        g_networkObserver != nullptr,
        "HookCode captured the network observer detour");
    g_originalResult = 11;
    g_originalError = ERROR_SUCCESS;
    g_originalSetsError = true;
    const int requestResult = g_networkObserver
        ? g_networkObserver(
              g_network,
              nullptr,
              &g_stream,
              requestPacket,
              static_cast<int>(
                  sizeof(requestPacket)),
              0)
        : 0;

    g_originalResult = 0;
    g_originalSetsError = false;
    SetLastError(ERROR_IO_PENDING);
    const int closeResult = g_networkObserver
        ? g_networkObserver(
              g_network,
              nullptr,
              &g_stream,
              closePacket,
              static_cast<int>(
                  sizeof(closePacket)),
              1)
        : 1;
    const NativeActions::TransportObservation latest =
        actions.TransportState();
    const NativeActions::TransportObservation request =
        actions.FindTransportSince(
            baseline,
            0x0829);
    Check(
        requestResult == 11 &&
            closeResult == 0 &&
            g_originalCalls == 2 &&
            g_maxOriginalDepth == 1,
        "two UI business sends each call original transport once");
    Check(
        g_lastNetwork == g_network &&
            g_lastStream == &g_stream &&
            g_lastPacket == closePacket &&
            g_lastSize ==
                static_cast<int>(
                    sizeof(closePacket)) &&
            g_lastFlag == 1,
        "last UI send preserves original arguments and flag");
    Check(
        latest.send_sequence == baseline + 2 &&
            latest.opcode == 0x07FB &&
            latest.packet_size ==
                sizeof(closePacket) &&
            latest.result == 0,
        "last transport state remains the later close send");
    Check(
        request.send_sequence == baseline + 1 &&
            request.opcode == 0x0829 &&
            request.packet_size ==
                sizeof(requestPacket) &&
            request.result == 11,
        "history finds the actual earlier request after a later send");
    Check(
        actions.FindTransportSince(
            request.send_sequence,
            0x0829).send_sequence == 0 &&
            actions.FindTransportSince(
                baseline,
                0x0999).send_sequence == 0,
        "history rejects stale and missing opcode matches");
    Check(
        GetLastError() == ERROR_IO_PENDING,
        "observer preserves incoming LastError when original leaves it unchanged");
}

void TestBoundedHistoryWrap()
{
    NativeActions actions;
    SdgoApi api = {};
    BindObservedActions(actions, api);
    ResetCalls();
    ResetNetwork(0, 0);
    g_originalSetsError = false;

    for (uint32_t index = 0; index < 70; ++index) {
        uint16_t opcode =
            static_cast<uint16_t>(0x1000 + index);
        if (index == 0)
            opcode = 0x0829;
        else if (index == 10)
            opcode = 0x0888;
        const uint8_t packet[] = {
            static_cast<uint8_t>(opcode & 0xFF),
            static_cast<uint8_t>(opcode >> 8),
            static_cast<uint8_t>(index),
            0,
        };
        g_originalResult =
            static_cast<int>(index + 1);
        g_networkObserver(
            g_network,
            nullptr,
            &g_stream,
            packet,
            static_cast<int>(sizeof(packet)),
            0);
    }

    const auto latest = actions.TransportState();
    const auto overwritten =
        actions.FindTransportSince(0, 0x0829);
    const auto retained =
        actions.FindTransportSince(0, 0x0888);
    Check(
        g_originalCalls == 70 &&
            latest.send_sequence == 70,
        "bounded history still calls and publishes all originals");
    Check(
        overwritten.send_sequence == 0,
        "history drops an opcode overwritten beyond sixty-four events");
    Check(
        retained.send_sequence == 11 &&
            retained.packet_size == 4 &&
            retained.result == 11,
        "history retains exact matching data inside the sixty-four event window");
}

void TestHistoryOwnershipIsPerInstance()
{
    NativeActions first;
    SdgoApi firstApi = {};
    BindObservedActions(first, firstApi);
    ResetCalls();
    ResetNetwork(0, 0);
    const uint8_t firstPacket[] = {
        0x29, 0x08, 1, 0,
    };
    g_originalResult = 3;
    g_networkObserver(
        g_network,
        nullptr,
        &g_stream,
        firstPacket,
        static_cast<int>(sizeof(firstPacket)),
        0);

    NativeActions second;
    SdgoApi secondApi = {};
    BindObservedActions(second, secondApi);
    const uint8_t secondPacket[] = {
        0xFB, 0x07, 2, 0,
    };
    g_originalResult = 4;
    g_networkObserver(
        g_network,
        nullptr,
        &g_stream,
        secondPacket,
        static_cast<int>(sizeof(secondPacket)),
        0);

    Check(
        first.FindTransportSince(
            0,
            0x0829).send_sequence == 1 &&
            first.FindTransportSince(
                0,
                0x07FB).send_sequence == 0,
        "first instance retains only its own request history");
    Check(
        second.FindTransportSince(
            0,
            0x07FB).send_sequence == 1 &&
            second.FindTransportSince(
                0,
                0x0829).send_sequence == 0,
        "second instance cannot consume the first instance history");
}

void TestRejectedOwnedRequestHasNoFakeTelemetry()
{
    NativeActions actions;
    SdgoApi api = {};
    BindObservedActions(actions, api);
    ResetCalls();
    ResetNetwork(1, 10);
    const auto before = actions.TransportState();

    Check(
        !actions.SendRequest(nullptr, 6) &&
            g_originalCalls == 0,
        "invalid owned request does not call original transport");
    const auto after = actions.TransportState();
    Check(
        after.send_sequence == before.send_sequence,
        "request rejection does not increment fake telemetry");
}

} // namespace

int main()
{
    TestFallbackBeforeObserverBinding();
    TestOwnedRequestUsesObserverOnce();
    TestOtherBusinessCallStillUsesOriginalOnce();
    TestBoundedHistoryWrap();
    TestHistoryOwnershipIsPerInstance();
    TestRejectedOwnedRequestHasNoFakeTelemetry();
    NativeActions::active_ = nullptr;
    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "%d native transport checks failed\n",
            g_failures);
        return EXIT_FAILURE;
    }
    std::printf(
        "all native transport checks passed\n");
    return EXIT_SUCCESS;
}
