// Recover a pending login through the original logout/reconnect business event.
// Credentials exist only in process memory for the bounded active attempt.
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "sdgo/sdgo_api.h"
#include "sdgo/login_recovery.h"
#include "policy.h"
#include "native_login_ui.h"
#include "native_channel_ui.h"

namespace {
using namespace sdgo::login_recovery_client;
const SdgoApi* g_api;
SdgoMod* g_mod;
using Getter = uint8_t*(__cdecl*)();
using Method = void(__thiscall*)(void*);
using Constructor = void*(__thiscall*)(void*, const char*, const char*);
using Dispatch = int(__thiscall*)(void*, int, int, int, int, void*);
using Callback = int(__cdecl*)(uint32_t, int, uint32_t, void*, uint32_t);
using SendMessage = void(__thiscall*)(void*, void*, char);
using SendPacket = int(__thiscall*)(void*, void*, const void*, int, char);
using ErrorMessage = void(__cdecl*)(uint16_t, int, char);
using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
struct alignas(4) NativeWStringValue { uint8_t bytes[0x1c]; };
static_assert(sizeof(NativeWStringValue) == 0x1c && std::is_trivially_copyable_v<NativeWStringValue>);
using NativeStringCtor = void*(__thiscall*)(void*, const wchar_t*);
using PendingRemove = int(__thiscall*)(void*, NativeWStringValue);
NativeStringCtor g_pendingNameCtor;
Method g_pendingNameDtor;
PendingRemove g_pendingRemove;
Getter g_app, g_network, g_manager;
Method g_messageCtor, g_messageDtor, g_queuePop, g_suspend;
void(__cdecl* g_free)(void*);
SendMessage g_messageSend;
SendPacket g_send;
Constructor g_ctor;
Dispatch g_dispatch;
Dispatch g_baseDispatch;
Callback g_callback, g_disconnect;
Method g_gateWatchdog, g_loadingWatchdog;
using GateInit = bool(__thiscall*)(void*, void*);
GateInit g_gateInit;
GateInit g_baseGateInit;
sdgo::login_recovery::NativeLoginUi g_loginUi;
sdgo::login_recovery::NativeChannelUi g_channelUi;
bool g_wasChannelScene;
void* g_gateContext;
ErrorMessage g_errorMessage;
ConnectFn g_connect;
uint8_t** g_profile;
void* g_tickOriginal;
void* g_boundaryOriginal;
void* g_fatalOriginal;
void* g_epilogue;
void* g_sendSiteOriginal;
void* g_sendEpilogue;
const DWORD g_mxcsr = 0x1f80;
Policy g_policy;
SRWLOCK g_callbackLock = SRWLOCK_INIT;
SRWLOCK g_snapshotLock = SRWLOCK_INIT;
SdgoLoginRecoverySnapshotV1 g_snapshot{};
bool g_closing;
thread_local unsigned g_rebuildScope;
thread_local bool g_internalSubmit;
uint8_t* g_gate;
char g_username[15]{};
char g_hash[33]{};
bool g_credentials;
bool g_authenticated;
bool g_replayPending;
bool g_returnPending;
bool g_cancelRequested;
bool g_fatalSkipped;
bool g_boundarySeen;
bool g_queueValid;
bool g_suppressDuplicate;
bool g_deferInitial;
bool g_pendingAuthCleared = true;
uint64_t g_returnDeadline;
Action g_action;
uint32_t g_connectionGeneration, g_rebuilds, g_retrySends, g_authReplies;
uint32_t g_lastReject, g_purged;
volatile LONG g_dropped;
volatile LONG g_monitorLogin;
volatile LONG g_disconnectedEpoch;
uint32_t g_lastGateState = UINT32_MAX, g_lastFlowCount;
HWND g_window;
bool g_escapeConsumed;

template<class T> T Read(const void* object, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset);
}
bool LoginState(uint32_t value) { return value == 4 || value == 25; }
bool Active() {
    const auto state = g_policy.state();
    return state == State::AwaitingOutcome || state == State::PausedForInteraction || state == State::Backoff ||
           state == State::AwaitingRecoveryCompletion || state == State::ReturnToLoginQueued;
}
void ClearCredentials() {
    SecureZeroMemory(g_username, sizeof(g_username));
    SecureZeroMemory(g_hash, sizeof(g_hash));
    g_credentials = false;
}
uint32_t AppState() {
    auto* app = g_app();
    return app ? Read<uint32_t>(app, 0x4a0) : UINT32_MAX;
}
uint32_t GateState() {
    // SERVICE_CHANNEL (25) destroys the gate owner. It has a different
    // allocation and layout, so a cached gate is valid only in SERVICE_GATE.
    return g_gate && AppState() == 4 ? Read<uint32_t>(g_gate, 0x2058) : UINT32_MAX;
}
bool Ready() { return AppState() == 4 && GateState() == 3; }
bool InteractiveLogin() {
    // Queuing Mode 4 does not immediately destroy the previous owner. Its
    // still-visible controls must not acknowledge the new return action.
    auto* app = g_app();
    return app && AppState() == 4 && g_gate &&
           Read<void*>(app, 0x4a4) == g_gate && g_loginUi.Present();
}
void Publish() {
    SdgoLoginRecoverySnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot); snapshot.version = 1;
    snapshot.generation = g_policy.generation();
    snapshot.state = static_cast<uint32_t>(g_policy.state());
    snapshot.outcome = static_cast<uint32_t>(g_policy.outcome());
    snapshot.automatic_retries = g_policy.retries_scheduled();
    snapshot.action_id = g_action.id;
    snapshot.connection_generation = g_connectionGeneration;
    snapshot.rebuilds = g_rebuilds; snapshot.retry_sends = g_retrySends;
    snapshot.auth_replies = g_authReplies; snapshot.last_reject = g_lastReject;
    snapshot.app_state = AppState(); snapshot.gate_state = GateState();
    snapshot.purged_packets = g_purged;
    snapshot.dropped_callbacks = static_cast<uint32_t>(InterlockedCompareExchange(&g_dropped, 0, 0));
    snapshot.credentials_present = g_credentials;
    snapshot.return_ready = !Active() && !g_returnPending && g_pendingAuthCleared && InteractiveLogin();
    snapshot.recovery_active = Active();
    snapshot.interaction_wait = g_policy.state() == State::PausedForInteraction;
    AcquireSRWLockExclusive(&g_snapshotLock); g_snapshot = snapshot;
    ReleaseSRWLockExclusive(&g_snapshotLock);
}

void* __fastcall CaptureLogin(void* packet, void*, const char* username, const char* hash) {
    if (!g_internalSubmit && LoginState(AppState())) {
        // Preserve the identity already in flight. Cancellation retires the
        // old socket before a new manual identity can be submitted.
        if (Active() || g_returnPending || g_cancelRequested) {
            g_suppressDuplicate = true;
            return g_ctor(packet, username, hash);
        }
        ClearCredentials();
        const size_t length = username ? strnlen_s(username, sizeof(g_username)) : 0;
        bool valid = length > 0 && length < sizeof(g_username) && hash && strnlen_s(hash, 33) == 32;
        for (size_t i = 0; valid && i < 32; ++i)
            valid = (hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f') ||
                    (hash[i] >= 'A' && hash[i] <= 'F');
        if (valid) {
            memcpy(g_username, username, length); memcpy(g_hash, hash, 32);
            g_credentials = true; g_authenticated = false;
            g_replayPending = false; g_returnPending = false; g_action = {};
            g_lastReject = 0; g_lastGateState = UINT32_MAX; g_lastFlowCount = 0;
            g_policy.Begin(GetTickCount64());
            g_pendingAuthCleared = false;
            g_suppressDuplicate = false;
            InterlockedExchange(&g_monitorLogin, 1);
            g_deferInitial = !Ready();
            g_api->Log(g_mod, SDGO_LOG_INFO, "pending native login captured; bounded recovery generation=%llu", g_policy.generation());
        }
    }
    return g_ctor(packet, username, hash);
}
bool __fastcall InitializeGate(void* self, void*, void* context) {
    const bool initialized = g_gateInit(self, context);
    if (initialized) { g_gate = static_cast<uint8_t*>(self); g_gateContext = context; }
    return initialized;
}
bool __fastcall InitializeBaseGate(void* self, void*, void* context) {
    const bool initialized = g_baseGateInit(self, context);
    if (initialized) { g_gate = static_cast<uint8_t*>(self); g_gateContext = context; }
    return initialized;
}

int ObserveGateDispatch(Dispatch original, void* self, int ip, int port, int id, int opcode, void* packet) {
    g_gate = static_cast<uint8_t*>(self);
    const auto generation = g_policy.generation();
    const auto normalized = static_cast<uint16_t>(opcode);
    const uint16_t reject = normalized == 0x232a && packet ? Read<uint16_t>(packet, 6) : 0;
    // Let the authentic native handler consume every reply, including rejects.
    // Its pointer remains compatible with the optional GameTest code observer.
    const int result = original(self, ip, port, id, opcode, packet);
    // Both native authentication reply handlers retire this named request.
    if (normalized == 0x232a || normalized == 0x232b) g_pendingAuthCleared = true;
    if (Active()) {
        if (normalized == 0x232a) {
            ++g_authReplies;
            // This opcode/status is the native authenticated already-online
            // response; it is never inferred from the waiting-window numbers.
            if (g_policy.Reject(generation, reject, reject == 1002, GetTickCount64())) {
                g_lastReject = reject;
                if (reject != 1002) ClearCredentials();
            }
            g_api->Log(g_mod, SDGO_LOG_INFO, "native login rejected status=%u generation=%llu", reject, generation);
        } else if (normalized == 0x232b) {
            ++g_authReplies;
            if (g_policy.Progress(generation, GetTickCount64())) g_authenticated = true;
        } else if (g_policy.state() == State::AwaitingOutcome || g_authenticated)
            g_policy.Progress(generation, GetTickCount64());
    }
    return result;
}
int __fastcall GateDispatch(void* self, void*, int ip, int port, int id, int opcode, void* packet) {
    return ObserveGateDispatch(g_dispatch, self, ip, port, id, opcode, packet);
}
int __fastcall BaseGateDispatch(void* self, void*, int ip, int port, int id, int opcode, void* packet) {
    return ObserveGateDispatch(g_baseDispatch, self, ip, port, id, opcode, packet);
}

int __cdecl PacketCallback(uint32_t ip, int port, uint32_t id, void* packet, uint32_t size) {
    AcquireSRWLockShared(&g_callbackLock);
    int result = 1;
    __try {
        if (g_closing) InterlockedIncrement(&g_dropped);
        else result = g_callback(ip, port, id, packet, size);
    } __finally { ReleaseSRWLockShared(&g_callbackLock); }
    return result;
}
int __cdecl DisconnectCallback(uint32_t ip, int port, uint32_t id, void* packet, uint32_t size) {
    AcquireSRWLockShared(&g_callbackLock);
    int result = 1;
    __try {
        if (g_closing) InterlockedIncrement(&g_dropped);
        else if (InterlockedCompareExchange(&g_monitorLogin, 0, 0) && LoginState(AppState()) &&
                 g_network() && id == Read<uint32_t>(g_network(), 0x14))
            InterlockedExchange(&g_disconnectedEpoch, static_cast<LONG>(g_connectionGeneration + 1));
        else result = g_disconnect(ip, port, id, packet, size);
    } __finally { ReleaseSRWLockShared(&g_callbackLock); }
    return result;
}

bool SpecialFlow(void* self, size_t listOffset, size_t countOffset) {
    if (!Read<uint32_t>(self, countOffset)) return false;
    const auto* sentinel = Read<uint8_t*>(self, listOffset);
    const auto* last = Read<uint8_t*>(sentinel, 4);
    const uint32_t step = Read<uint32_t>(last, 8);
    return step == 32 || step == 34;
}
void __fastcall GateWatchdog(void* self, void*) {
    if (!Active() || SpecialFlow(self, 0x2064, 0x2068)) g_gateWatchdog(self);
}
void __fastcall LoadingWatchdog(void* self, void*) {
    if (!Active() || SpecialFlow(self, 0x70, 0x74)) g_loadingWatchdog(self);
}

// Native NetAsync uses WSAAsyncSelect on the owning game window. Bound the
// original synchronous connect only inside our reconnect call. Other sockets,
// startup, gameplay and any existing endpoint translator keep their call chain.
int WSAAPI Connect(SOCKET socket, const sockaddr* address, int length) {
    if (!g_rebuildScope) return g_connect(socket, address, length);
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking)) return SOCKET_ERROR;
    int result = g_connect(socket, address, length);
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (result == SOCKET_ERROR && (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS)) {
        fd_set writeSet, errorSet; FD_ZERO(&writeSet); FD_ZERO(&errorSet);
        FD_SET(socket, &writeSet); FD_SET(socket, &errorSet);
        timeval timeout{2, 0};
        const int selected = select(0, nullptr, &writeSet, &errorSet, &timeout);
        int optionLength = sizeof(error);
        if (selected > 0 && getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &optionLength) == 0)
            result = error ? SOCKET_ERROR : 0;
        else { result = SOCKET_ERROR; error = selected == 0 ? WSAETIMEDOUT : WSAGetLastError(); }
    }
    nonblocking = 0;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0 && result == 0) { result = SOCKET_ERROR; error = WSAGetLastError(); }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

// Runs after the old native network destructor and before the new allocation.
// No old producer may cross the fence; fresh connection packets are admitted
// only after the old native queue has been discarded, never dispatched.
void* __stdcall ReconnectBoundary(void*) {
    if (g_rebuildScope) {
        g_boundarySeen = true; g_queueValid = true;
        auto* app = g_app(); auto* queue = app + 0x2530;
        __try {
            const uint32_t capacity = Read<uint32_t>(queue, 8);
            const uint32_t count = Read<uint32_t>(queue, 0x10);
            const uint32_t head = Read<uint32_t>(queue, 0xc);
            auto** map = Read<uint8_t**>(queue, 4);
            if (count && (!capacity || count > capacity || head >= capacity || !map)) g_queueValid = false;
            for (uint32_t i = 0; g_queueValid && i < count; ++i) {
                const auto* record = map[Read<uint32_t>(queue, 0xc)];
                if (!record) { g_queueValid = false; break; }
                g_free(Read<void*>(record, 0xc)); g_queuePop(queue); ++g_purged;
            }
            MSG message{};
            // The verified TCP transport registers message 0x40B. Remove its
            // already-posted old socket notifications before socket reuse.
            while (PeekMessageW(&message, Read<HWND>(app, 0x3c), 0x40b, 0x40b, PM_REMOVE)) {
                // WM_QUIT ignores the message-range filter. Preserve the
                // user's shutdown request instead of consuming it as stale IO.
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_queueValid = false; }
        AcquireSRWLockExclusive(&g_callbackLock); ++g_connectionGeneration; g_closing = !g_queueValid;
        ReleaseSRWLockExclusive(&g_callbackLock);
    }
    return g_boundaryOriginal;
}
void* __stdcall FatalBoundary(void*) {
    if (g_rebuildScope) { g_fatalSkipped = true; return g_epilogue; }
    return g_fatalOriginal;
}
void* __stdcall SendBoundary(void* frame) {
    const auto* packet = Read<const uint8_t*>(frame, 0xc);
    if (Read<int>(frame, 0x10) == 0x4d && packet && Read<uint16_t>(packet, 0) == 0x2329) {
        if (!g_internalSubmit && g_suppressDuplicate) { g_suppressDuplicate = false; return g_sendEpilogue; }
        if (!g_internalSubmit && g_deferInitial) {
            // The offline native page accepts the user's action, but its
            // request must wait for a fresh server welcome and native gate.
            g_deferInitial = false;
            g_policy.TransportFailure(g_policy.generation(), GetTickCount64());
            return g_sendEpilogue;
        }
    }
    return g_sendSiteOriginal;
}

bool Rebuild() {
    AcquireSRWLockExclusive(&g_callbackLock); g_closing = true;
    ReleaseSRWLockExclusive(&g_callbackLock);
    g_gate = nullptr; g_gateContext = nullptr; g_authenticated = false;
    g_wasChannelScene = false;
    g_suppressDuplicate = false; g_deferInitial = false;
    g_boundarySeen = false; g_fatalSkipped = false; g_queueValid = false;
    alignas(4) uint8_t message[0x30]{};
    bool constructed = false, dispatched = false;
    ++g_rebuildScope;
    __try {
        g_suspend(g_app());
        g_messageCtor(message); constructed = true;
        *reinterpret_cast<uint32_t*>(message + 0xc) = 0x13;
        *reinterpret_cast<uint32_t*>(message + 0x2c) = 6;
        g_messageSend(g_manager(), message, 1); dispatched = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { dispatched = false; }
    if (constructed) {
        __try { g_messageDtor(message); }
        __except (EXCEPTION_EXECUTE_HANDLER) { dispatched = false; }
    }
    --g_rebuildScope; ++g_rebuilds;
    g_lastGateState = UINT32_MAX; g_lastFlowCount = 0;
    g_api->Log(g_mod, SDGO_LOG_INFO, "native reconnect generation=%u boundary=%u queue=%u connect_failed=%u discarded=%u",
        g_connectionGeneration, g_boundarySeen, g_queueValid, g_fatalSkipped, g_purged);
    return dispatched && g_boundarySeen && g_queueValid;
}

bool Replay() {
    if (!g_credentials || !Ready()) return false;
    bool sent = false; uint8_t packet[0x4d]{};
    g_internalSubmit = true;
    __try {
        g_ctor(packet, g_username, g_hash);
        auto* network = g_network();
        void* stream = network ? Read<void*>(network, 0x14) : nullptr;
        if (stream) {
            ++*reinterpret_cast<uint32_t*>(network + 0x58);
            *reinterpret_cast<uint32_t*>(network + 0x5c) += sizeof(packet);
            sent = g_send(network, stream, packet, sizeof(packet), 0) != 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { sent = false; }
    g_internalSubmit = false; SecureZeroMemory(packet, sizeof(packet));
    if (sent) ++g_retrySends;
    return sent;
}
bool RetirePendingLogin() {
    NativeWStringValue name{};
    bool constructed = false, transferred = false, completed = false;
    __try {
        void* manager = g_manager();
        if (manager) {
            constructed = g_pendingNameCtor(&name, L"REQ_LOCAL_LOGIN_USER") != nullptr;
            if (constructed) {
                // The native callee consumes/destroys its by-value string.
                // Removing an absent name is also a successful retirement.
                transferred = true;
                g_pendingRemove(manager, name);
                completed = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { completed = false; }
    if (constructed && !transferred) {
        __try { g_pendingNameDtor(&name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { completed = false; }
    }
    SecureZeroMemory(&name, sizeof(name));
    return completed;
}
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (g_escapeConsumed && wparam == VK_ESCAPE &&
        (message == WM_CHAR || message == WM_KEYUP || message == WM_KEYDOWN)) {
        if (message == WM_KEYUP) g_escapeConsumed = false;
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE && Active() && AppState() == 4 && GateState() != 4) {
        g_escapeConsumed = true; g_cancelRequested = true; return 0;
    }
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, WindowProcedure, id); g_window = nullptr; ClearCredentials(); }
    return DefSubclassProc(window, message, wparam, lparam);
}

void Tick() {
    auto* app = g_app(); if (!app) return;
    const uint32_t appState = AppState(); const auto now = GetTickCount64();
    if (appState != 4) { g_gate = nullptr; g_gateContext = nullptr; }
    // Native Back from the channel picker has already retired its session.
    // End the old identity instead of replaying it while the user edits login.
    if (g_wasChannelScene && appState == 4 && Active()) {
        g_policy.Cancel(g_policy.generation(), now); ClearCredentials();
        g_authenticated = false; g_replayPending = false; g_returnPending = false; g_action = {};
    }
    g_wasChannelScene = Active() && g_authenticated && appState == 25;
    const LONG disconnected = InterlockedExchange(&g_disconnectedEpoch, 0);
    if (Active() && disconnected == static_cast<LONG>(g_connectionGeneration + 1)) {
        if (g_policy.state() == State::PausedForInteraction)
            g_policy.ResumeAfterInteraction(g_policy.generation(), now, false);
        if (g_policy.pending_action_kind() == ActionKind::RetryLogin) {
            g_replayPending = false;
            g_policy.RecoveryCompleted(g_action.generation, g_action.id, true, now);
        }
        g_policy.TransportFailure(g_policy.generation(), now);
    }
    const HWND window = Read<HWND>(app, 0x3c);
    if (window && !g_window && SetWindowSubclass(window, WindowProcedure, 0x53444c52, 0)) g_window = window;
    if (g_cancelRequested) {
        g_cancelRequested = false;
        g_policy.Cancel(g_policy.generation(), now); ClearCredentials();
        g_replayPending = false; g_action = {}; g_returnPending = true;
        g_returnDeadline = now + 15000;
        if (!Rebuild()) g_returnPending = false;
    }
    const bool interaction =
        (appState == 4 && g_gate && (GateState() == 4 || SpecialFlow(g_gate, 0x2064, 0x2068))) ||
        (g_authenticated && appState == 25 && g_channelUi.WaitingForUser(app));
    if (interaction) g_policy.PauseForInteraction(g_policy.generation(), now);
    else if (g_policy.state() == State::PausedForInteraction) g_policy.ResumeAfterInteraction(g_policy.generation(), now, false);
    if (Active() && g_authenticated && (appState == 5 || appState == 6) && g_profile && *g_profile && Read<uint32_t>(*g_profile, 0x138)) {
        if (g_policy.Success(g_policy.generation(), now)) {
            ClearCredentials(); g_replayPending = false;
            g_api->Log(g_mod, SDGO_LOG_INFO, "native login completed into usable lobby/hangar generation=%llu", g_policy.generation());
        }
    }
    if (Active() && LoginState(appState)) {
        const uint32_t state = GateState();
        const uint32_t count = g_gate ? Read<uint32_t>(g_gate, 0x2068) : 0;
        if (state != g_lastGateState || count != g_lastFlowCount) {
            // Polling a field for the first time is not an authoritative
            // server event and must not cancel an already scheduled retry.
            if (g_policy.state() == State::AwaitingOutcome)
                g_policy.Progress(g_policy.generation(), now);
            g_lastGateState = state; g_lastFlowCount = count;
        }
    }
    // Expire actions before considering a send. A late ready event must never
    // transmit once its action or total budget has already expired.
    const Action action = g_policy.Tick(now);
    if (action) {
        g_action = action; g_replayPending = false;
        const bool retry = action.kind == ActionKind::RetryLogin;
        if (!retry) ClearCredentials();
        const bool rebuilt = Rebuild();
        if (!rebuilt) g_policy.RecoveryCompleted(action.generation, action.id, false, GetTickCount64());
        else if (retry) {
            // Native reconstruction is synchronous. Waiting for the new
            // server welcome belongs to the normal bounded outcome phase,
            // so a missing welcome consumes another retry rather than an
            // adapter-action failure that prematurely skips the retry budget.
            g_policy.RecoveryCompleted(action.generation, action.id, true, GetTickCount64());
            if (g_fatalSkipped) {
                g_policy.TransportFailure(action.generation, GetTickCount64());
            } else g_replayPending = true;
        } else { g_returnPending = true; g_returnDeadline = GetTickCount64() + 10000; }
    }
    if (g_policy.state() != State::AwaitingOutcome) g_replayPending = false;
    if (g_replayPending && Ready()) {
        g_replayPending = false;
        const bool sent = Replay();
        g_policy.Progress(g_policy.generation(), now);
        if (!sent) g_policy.TransportFailure(g_policy.generation(), now);
        g_api->Log(g_mod, SDGO_LOG_INFO, "native retry request number=%u sent=%u", g_action.retryNumber, sent);
    }
    if (g_returnPending) {
        if (AppState() == 4 && g_gate && Read<void*>(app, 0x4a4) == g_gate && !InteractiveLogin() &&
            (g_fatalSkipped || now + 8000 >= g_returnDeadline))
            g_loginUi.EnsureVisible(g_gateContext);
        if (InteractiveLogin() && RetirePendingLogin()) {
            g_pendingAuthCleared = true;
            g_returnPending = false;
            if (g_action) g_policy.RecoveryCompleted(g_action.generation, g_action.id, true, now);
            if (g_lastReject && g_lastReject != 1002) g_errorMessage(0, g_lastReject, 0);
            g_api->Log(g_mod, SDGO_LOG_INFO, "interactive native login page restored");
        } else if (now >= g_returnDeadline) {
            g_returnPending = false;
            if (g_action) g_policy.RecoveryCompleted(g_action.generation, g_action.id, false, now);
        }
    }
    if (!Active() && !g_returnPending) ClearCredentials();
    InterlockedExchange(&g_monitorLogin, Active() || g_returnPending);
    Publish();
}
void* __stdcall TickBoundary(void*) {
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ClearCredentials(); g_policy.Cancel(g_policy.generation(), GetTickCount64());
        g_api->Log(g_mod, SDGO_LOG_ERROR, "native recovery guard refused unexpected runtime state");
    }
    return g_tickOriginal;
}

// Preserve every live register, flags, stack and FP/SIMD value. A reserved stack
// word holds the chosen continuation, so even the branch selector is restored.
#define RECOVERY_SITE(name, helper) \
__declspec(naked) void name() { \
    __asm { lea esp, [esp-4] } \
    __asm { pushfd } \
    __asm { pushad } \
    __asm { mov eax, esp } \
    __asm { sub esp, 210h } \
    __asm { and esp, 0fffffff0h } \
    __asm { mov [esp+200h], eax } \
    __asm { fxsave [esp] } \
    __asm { fninit } \
    __asm { ldmxcsr dword ptr [g_mxcsr] } \
    __asm { cld } \
    __asm { push ebp } \
    __asm { call helper } \
    __asm { mov edx, [esp+200h] } \
    __asm { mov [edx+24h], eax } \
    __asm { fxrstor [esp] } \
    __asm { mov esp, [esp+200h] } \
    __asm { popad } \
    __asm { popfd } \
    __asm { ret } \
}
RECOVERY_SITE(TickSite, TickBoundary)
RECOVERY_SITE(ReconnectSite, ReconnectBoundary)
RECOVERY_SITE(FatalSite, FatalBoundary)
RECOVERY_SITE(SendSite, SendBoundary)
#undef RECOVERY_SITE

template<class T> bool Bind(T& value, const char* name) {
    value = reinterpret_cast<T>(g_api->Symbol(g_mod, name)); return value != nullptr;
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoLoginRecoveryGetSnapshotV1(SdgoLoginRecoverySnapshotV1* out) {
    if (!out || out->size != sizeof(*out) || out->version != 1) return 0;
    AcquireSRWLockShared(&g_snapshotLock); *out = g_snapshot;
    ReleaseSRWLockShared(&g_snapshotLock); return out->version == 1;
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION || api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->host_key || strcmp(api->host_key, "GOnline-54BE1568-00C97000") || !api->Symbol || !api->HookSite ||
        !api->HookCode || !api->HookSlot || !api->HookSystemExport || !api->Log) return 0;
    g_api = api; g_mod = mod;
    if (!Bind(g_app, "recovery.app") || !Bind(g_network, "recovery.network") || !Bind(g_manager, "recovery.manager") ||
        !Bind(g_messageCtor, "recovery.message_ctor") || !Bind(g_messageDtor, "recovery.message_dtor") ||
        !Bind(g_messageSend, "recovery.message_send") || !Bind(g_queuePop, "recovery.queue_pop") ||
        !Bind(g_suspend, "recovery.suspend") || !Bind(g_free, "recovery.free") || !Bind(g_profile, "recovery.profile") ||
        !Bind(g_send, "recovery.send") || !Bind(g_sendEpilogue, "recovery.send_epilogue") ||
        !Bind(g_epilogue, "recovery.reconnect_epilogue") || !Bind(g_errorMessage, "recovery.error_message")) return 0;
    if (!Bind(g_pendingNameCtor, "flow.native_wstring_ctor") ||
        !Bind(g_pendingNameDtor, "flow.native_wstring_dtor") ||
        !Bind(g_pendingRemove, "recovery.pending_remove")) return 0;
    if (!g_loginUi.Bind(api, mod) || !g_channelUi.Bind(api, mod)) return 0;
    if (!api->HookCode(mod, "recovery.login_ctor", reinterpret_cast<void*>(&CaptureLogin), reinterpret_cast<void**>(&g_ctor)) ||
        !api->HookCode(mod, "recovery.gate_init", reinterpret_cast<void*>(&InitializeGate), reinterpret_cast<void**>(&g_gateInit)) ||
        !api->HookCode(mod, "recovery.base_gate_init", reinterpret_cast<void*>(&InitializeBaseGate), reinterpret_cast<void**>(&g_baseGateInit)) ||
        !api->HookCode(mod, "recovery.gate_dispatch", reinterpret_cast<void*>(&GateDispatch), reinterpret_cast<void**>(&g_dispatch)) ||
        !api->HookSlot(mod, "recovery.base_gate_dispatch", reinterpret_cast<void*>(&BaseGateDispatch), reinterpret_cast<void**>(&g_baseDispatch)) ||
        !api->HookCode(mod, "recovery.packet_callback", reinterpret_cast<void*>(&PacketCallback), reinterpret_cast<void**>(&g_callback)) ||
        !api->HookCode(mod, "recovery.disconnect_callback", reinterpret_cast<void*>(&DisconnectCallback), reinterpret_cast<void**>(&g_disconnect)) ||
        !api->HookCode(mod, "recovery.gate_watchdog", reinterpret_cast<void*>(&GateWatchdog), reinterpret_cast<void**>(&g_gateWatchdog)) ||
        !api->HookCode(mod, "recovery.loading_watchdog", reinterpret_cast<void*>(&LoadingWatchdog), reinterpret_cast<void**>(&g_loadingWatchdog)) ||
        !api->HookSystemExport(mod, L"ws2_32.dll", "connect", reinterpret_cast<void*>(&Connect), reinterpret_cast<void**>(&g_connect)) ||
        !api->HookSite(mod, "recovery.tick", reinterpret_cast<void*>(&TickSite), &g_tickOriginal) ||
        !api->HookSite(mod, "recovery.reconnect_boundary", reinterpret_cast<void*>(&ReconnectSite), &g_boundaryOriginal) ||
        !api->HookSite(mod, "recovery.reconnect_failure", reinterpret_cast<void*>(&FatalSite), &g_fatalOriginal) ||
        !api->HookSite(mod, "recovery.send_boundary", reinterpret_cast<void*>(&SendSite), &g_sendSiteOriginal)) return 0;
    api->Log(mod, SDGO_LOG_INFO, "bounded native login recovery active; three automatic retries, native reconnect and Escape cancellation");
    return 1;
}
