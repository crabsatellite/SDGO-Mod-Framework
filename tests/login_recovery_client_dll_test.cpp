#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "sdgo/login_recovery.h"
#include "sdgo/sdgo_api.h"
#include "../mods/login_recovery_client/policy.h"

namespace {
using sdgo::login_recovery_client::Outcome;
using sdgo::login_recovery_client::State;

using LoginCtorHook =
    void*(__fastcall*)(void*, void*, const char*, const char*);
using DispatchHook =
    int(__fastcall*)(void*, void*, int, int, int, int, void*);
using PacketHook =
    int(__cdecl*)(std::uint32_t, int, std::uint32_t, void*, std::uint32_t);
using MethodHook = void(__fastcall*)(void*, void*);
using GateInitHook = bool(__fastcall*)(void*, void*, void*);
using SiteHook = void(__cdecl*)();

struct alignas(4) NativeWStringValue
{
    std::uint8_t bytes[0x1c];
};
static_assert(
    sizeof(NativeWStringValue) == 0x1c &&
        alignof(NativeWStringValue) == 4 &&
        std::is_trivially_copyable_v<
            NativeWStringValue>,
    "native pending-login key is passed by value");

std::array<std::uint8_t, 0x2800> app;
std::array<std::uint8_t, 0x1800> network;
std::array<std::uint8_t, 0x2100> gate;
std::array<std::uint8_t, 0x400> profile;
std::array<std::uint8_t, 32> manager;
std::array<std::uint8_t, 0x78> loginService;
std::array<std::uint8_t, 0xa0> loginService2;
std::uint8_t* profilePointer = profile.data();
std::array<std::uint8_t, 0x18> queuedRecord;
std::array<std::uint8_t*, 2> queueMap;
std::vector<std::uint8_t> queuedPayload;
std::vector<std::uint8_t> clonedMessage;
std::vector<std::string> logs;

LoginCtorHook loginCtorHook;
DispatchHook dispatchHook;
DispatchHook baseDispatchHook;
PacketHook packetHook;
PacketHook disconnectHook;
MethodHook gateWatchdogHook;
MethodHook loadingWatchdogHook;
GateInitHook gateInitHook;
GateInitHook baseGateInitHook;
SiteHook tickHook;
SiteHook reconnectBoundaryHook;
SiteHook reconnectFailureHook;
SiteHook sendBoundaryHook;

int originalCtorCalls;
int originalDispatchCalls;
int originalBaseDispatchCalls;
int originalPacketCalls;
int originalDisconnectCalls;
int originalGateWatchdogCalls;
int originalLoadingWatchdogCalls;
int originalGateInitCalls;
int originalBaseGateInitCalls;
int originalSendCalls;
int messageCtorCalls;
int messageDtorCalls;
int messageSendCalls;
int suspendCalls;
int queuePopCalls;
int freeCalls;
int errorMessageCalls;
int tickContinuations;
int boundaryContinuations;
int fatalContinuations;
int epilogueContinuations;
int sendSiteContinuations;
int sendEpilogueContinuations;
int nativeWStringCtorCalls;
int nativeWStringDtorCalls;
int serviceCtorCalls;
int serviceDtorCalls;
int serviceCloneCalls;
int serviceRegisterCalls;
int serviceActivateCalls;
int loginUi2CtorCalls;
int loginUi2DtorCalls;
int pendingRemoveCalls;
int pendingNameDtorCalls;
std::size_t lastServiceCloneSize;
bool sendSucceeds;
bool invokeReconnectFailure;
bool invokeDisconnectDuringMessageSend;
bool makeReadyAfterReconnect;
bool recreateLoginUiAfterReconnect;
bool preserveLoginUiAfterReconnect;
bool queueGateInitAfterReconnect;
bool baseLoginServiceRegistered;
bool derivedLoginServiceRegistered;
bool nativeUiAbiMismatch;
bool loginUi2DtorSawFullSource;
bool gateInitPending;
bool replayCredentialsMatched;
bool exposePendingRemoveSymbol;
bool pendingGuardPresent;
bool pendingRemoveSawExpectedKey;
bool pendingRemoveSawGuardPresent;
bool retirePendingGuardOnReply;
std::uint8_t derivedUiSymbolMask;
std::uint16_t lastDisplayedError;
DWORD siteFlagsBefore;
DWORD siteFlagsAfter;
void* lastServiceActivateContext;
SdgoApi activeApi;
std::uint8_t loginVtableToken;
std::uint8_t login2VtableToken;
void* const gateContext = reinterpret_cast<void*>(0x24681357);

constexpr char kUser[] = "fixtureUser";
constexpr char kHash[] = "0123456789abcdef0123456789abcdef";
constexpr wchar_t kBaseLoginServiceName[] = L"LoginInterFace";
constexpr wchar_t kDerivedLoginServiceName[] = L"LoginInterFace2";
constexpr wchar_t kPendingLoginKey[] = L"REQ_LOCAL_LOGIN_USER";
constexpr std::uint32_t kPendingStringMarker = 0x524c5251;
constexpr std::uint8_t kDerivedCtorSymbol = 0x01;
constexpr std::uint8_t kDerivedDtorSymbol = 0x02;
constexpr std::uint8_t kDerivedVtableSymbol = 0x04;
constexpr std::uint8_t kAllDerivedUiSymbols =
    kDerivedCtorSymbol |
    kDerivedDtorSymbol |
    kDerivedVtableSymbol;

template<class T>
void Put(void* object, std::size_t offset, T value)
{
    *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset) =
        value;
}

template<class T>
T Get(const void* object, std::size_t offset)
{
    return *reinterpret_cast<const T*>(
        static_cast<const std::uint8_t*>(object) + offset);
}

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}

void __cdecl Log(SdgoMod*, int, const char* format, ...)
{
    char text[1024]{};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
    va_end(arguments);
    logs.emplace_back(text);
}

std::uint8_t* __cdecl App()
{
    return app.data();
}

std::uint8_t* __cdecl Network()
{
    return network.data();
}

std::uint8_t* __cdecl Manager()
{
    return manager.data();
}

void MarkLoginServiceReady(
    void* service,
    void* vtable,
    std::uintptr_t controlBase)
{
    auto* bytes = static_cast<std::uint8_t*>(service);
    Put<void*>(bytes, 0, vtable);
    bytes[4] = 1;
    bytes[0x2d] = 0;
    Put<void*>(
        bytes,
        0x44,
        reinterpret_cast<void*>(controlBase + 1));
    Put<void*>(
        bytes,
        0x48,
        reinterpret_cast<void*>(controlBase + 2));
    Put<void*>(
        bytes,
        0x60,
        reinterpret_cast<void*>(controlBase + 3));
    Put<void*>(
        bytes,
        0x64,
        reinterpret_cast<void*>(controlBase + 4));
}

void SetBaseLoginServiceReady()
{
    loginService.fill(0);
    MarkLoginServiceReady(
        loginService.data(),
        &loginVtableToken,
        0x11110000);
    baseLoginServiceRegistered = true;
}

void SetDerivedLoginServiceReady()
{
    loginService2.fill(0);
    MarkLoginServiceReady(
        loginService2.data(),
        &login2VtableToken,
        0x22220000);
    loginService2[0x9f] = 0xa2;
    derivedLoginServiceRegistered = true;
}

void ClearLoginServices()
{
    loginService.fill(0);
    loginService2.fill(0);
    baseLoginServiceRegistered = false;
    derivedLoginServiceRegistered = false;
}

void* __cdecl ServiceManager()
{
    return manager.data();
}

void* __fastcall ServiceFindByName(
    void* self,
    void*,
    const void* descriptor)
{
    if (self != manager.data() ||
        !descriptor ||
        Get<std::uint32_t>(descriptor, 0) != 1) {
        nativeUiAbiMismatch = true;
        return nullptr;
    }
    const auto* name =
        Get<const wchar_t*>(descriptor, sizeof(std::uint32_t));
    if (name &&
        std::wcscmp(name, kDerivedLoginServiceName) == 0) {
        return derivedLoginServiceRegistered
            ? loginService2.data()
            : nullptr;
    }
    if (name &&
        std::wcscmp(name, kBaseLoginServiceName) == 0) {
        return baseLoginServiceRegistered
            ? loginService.data()
            : nullptr;
    }
    nativeUiAbiMismatch = true;
    return nullptr;
}

void* __fastcall NativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    ++nativeWStringCtorCalls;
    if (!value ||
        (std::wcscmp(value, kBaseLoginServiceName) != 0 &&
         std::wcscmp(value, kDerivedLoginServiceName) != 0 &&
         std::wcscmp(value, kPendingLoginKey) != 0)) {
        nativeUiAbiMismatch = true;
        return nullptr;
    }
    std::memset(self, 0, 0x1c);
    Put<const wchar_t*>(self, 0, value);
    if (std::wcscmp(value, kPendingLoginKey) == 0) {
        Put<std::uint32_t>(
            self,
            0x18,
            kPendingStringMarker);
    }
    return self;
}

void __fastcall NativeWStringDtor(void* self, void*)
{
    ++nativeWStringDtorCalls;
    if (self) {
        if (Get<std::uint32_t>(self, 0x18) ==
            kPendingStringMarker)
            ++pendingNameDtorCalls;
        SecureZeroMemory(self, 0x1c);
    }
}

int __fastcall PendingRemove(
    void* self,
    void*,
    NativeWStringValue name)
{
    ++pendingRemoveCalls;
    const auto* value =
        Get<const wchar_t*>(&name, 0);
    const bool expected =
        self == manager.data() &&
        value &&
        std::wcscmp(value, kPendingLoginKey) == 0 &&
        Get<std::uint32_t>(&name, 0x18) ==
            kPendingStringMarker;
    pendingRemoveSawExpectedKey =
        pendingRemoveSawExpectedKey || expected;
    pendingRemoveSawGuardPresent =
        pendingRemoveSawGuardPresent ||
        pendingGuardPresent;
    if (!expected)
        nativeUiAbiMismatch = true;
    else
        pendingGuardPresent = false;
    NativeWStringDtor(&name, nullptr);
    return expected ? 1 : 0;
}

void* __fastcall ServiceBaseCtor(void* self, void*)
{
    ++serviceCtorCalls;
    std::memset(self, 0, 0x78);
    return self;
}

void __fastcall ServiceBaseDtor(void*, void*)
{
    ++serviceDtorCalls;
}

void* __fastcall LoginUi2Ctor(void* self, void*)
{
    ++loginUi2CtorCalls;
    std::memset(self, 0, 0xa0);
    Put<void*>(self, 0, &login2VtableToken);
    static_cast<std::uint8_t*>(self)[0x9f] = 0xa2;
    return self;
}

void __fastcall LoginUi2Dtor(void* self, void*)
{
    ++loginUi2DtorCalls;
    loginUi2DtorSawFullSource =
        self &&
        Get<void*>(self, 0) == &login2VtableToken &&
        static_cast<std::uint8_t*>(self)[0x9f] == 0xa2;
    if (self)
        SecureZeroMemory(self, 0xa0);
}

void* __cdecl ServiceClone(void* source, bool)
{
    ++serviceCloneCalls;
    if (!source) {
        nativeUiAbiMismatch = true;
        return nullptr;
    }
    const auto vtable = Get<void*>(source, 0);
    if (vtable == &login2VtableToken) {
        lastServiceCloneSize = loginService2.size();
        std::memcpy(
            loginService2.data(),
            source,
            loginService2.size());
        return loginService2.data();
    }
    if (vtable == &loginVtableToken) {
        lastServiceCloneSize = loginService.size();
        std::memcpy(
            loginService.data(),
            source,
            loginService.size());
        return loginService.data();
    }
    nativeUiAbiMismatch = true;
    return nullptr;
}

void* __fastcall ServiceRegister(
    void* self,
    void*,
    void* service,
    const void* nameObject,
    bool rejectDuplicate)
{
    ++serviceRegisterCalls;
    const auto* name = nameObject
        ? Get<const wchar_t*>(nameObject, 0)
        : nullptr;
    const bool derived =
        service == loginService2.data() &&
        name &&
        std::wcscmp(name, kDerivedLoginServiceName) == 0;
    const bool base =
        service == loginService.data() &&
        name &&
        std::wcscmp(name, kBaseLoginServiceName) == 0;
    if (self != manager.data() ||
        (!derived && !base) ||
        rejectDuplicate) {
        nativeUiAbiMismatch = true;
        return nullptr;
    }
    if (derived)
        derivedLoginServiceRegistered = true;
    else
        baseLoginServiceRegistered = true;
    return service;
}

void __fastcall ServiceActivate(
    void* self,
    void*,
    void* context,
    bool)
{
    ++serviceActivateCalls;
    lastServiceActivateContext = context;
    if (self != manager.data() ||
        (context != gateContext && context != nullptr)) {
        nativeUiAbiMismatch = true;
        return;
    }
    if (derivedLoginServiceRegistered) {
        MarkLoginServiceReady(
            loginService2.data(),
            &login2VtableToken,
            0x22220000);
    }
    if (baseLoginServiceRegistered) {
        MarkLoginServiceReady(
            loginService.data(),
            &loginVtableToken,
            0x11110000);
    }
}

void __fastcall MessageCtor(void* message, void*)
{
    ++messageCtorCalls;
    std::memset(message, 0, 0x30);
    Put<std::uint32_t>(message, 0, 0xfeed1234);
}

void __fastcall MessageDtor(void* message, void*)
{
    ++messageDtorCalls;
    SecureZeroMemory(message, 0x30);
}

void __fastcall QueuePop(void* queue, void*)
{
    ++queuePopCalls;
    std::uint32_t count = Get<std::uint32_t>(queue, 0x10);
    if (!count)
        return;
    const std::uint32_t capacity = Get<std::uint32_t>(queue, 8);
    const std::uint32_t head = Get<std::uint32_t>(queue, 0xc);
    Put<std::uint32_t>(
        queue,
        0xc,
        count == 1 || !capacity ? 0 : (head + 1) % capacity);
    Put<std::uint32_t>(queue, 0x10, count - 1);
}

void __fastcall Suspend(void*, void*)
{
    ++suspendCalls;
}

void __cdecl NativeFree(void* memory)
{
    ++freeCalls;
    Check(
        memory == queuedPayload.data(),
        "queue purge frees only the copied old packet payload");
}

void* __fastcall NativeLoginCtor(
    void* packet,
    void*,
    const char* username,
    const char* hash)
{
    ++originalCtorCalls;
    pendingGuardPresent = true;
    replayCredentialsMatched =
        replayCredentialsMatched ||
        (originalCtorCalls > 1 &&
         username &&
         hash &&
         std::strcmp(username, kUser) == 0 &&
         std::strcmp(hash, kHash) == 0);
    std::memset(packet, 0, 0x4d);
    Put<std::uint16_t>(packet, 0, 0x2329);
    return packet;
}

int __fastcall NativeDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int opcode,
    void*)
{
    ++originalDispatchCalls;
    if (retirePendingGuardOnReply &&
        (opcode == 0x232a || opcode == 0x232b))
        pendingGuardPresent = false;
    return 77;
}

int __fastcall NativeBaseDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int opcode,
    void*)
{
    ++originalBaseDispatchCalls;
    if (retirePendingGuardOnReply &&
        (opcode == 0x232a || opcode == 0x232b))
        pendingGuardPresent = false;
    return 78;
}

int __cdecl NativePacket(
    std::uint32_t,
    int,
    std::uint32_t,
    void*,
    std::uint32_t)
{
    ++originalPacketCalls;
    return 42;
}

int __cdecl NativeDisconnect(
    std::uint32_t,
    int,
    std::uint32_t,
    void*,
    std::uint32_t)
{
    ++originalDisconnectCalls;
    return 43;
}

void __fastcall NativeGateWatchdog(void*, void*)
{
    ++originalGateWatchdogCalls;
}

void __fastcall NativeLoadingWatchdog(void*, void*)
{
    ++originalLoadingWatchdogCalls;
}

bool __fastcall NativeGateInit(void* self, void*, void* context)
{
    ++originalGateInitCalls;
    Check(
        context == gateContext || context == nullptr,
        "semantic GateInit hook preserves the native initialization context, including the stock null path");
    Put<void*>(app.data(), 0x4a4, self);
    if (recreateLoginUiAfterReconnect) {
        if (derivedUiSymbolMask == kAllDerivedUiSymbols)
            SetDerivedLoginServiceReady();
        else
            SetBaseLoginServiceReady();
    }
    return true;
}

bool __fastcall NativeBaseGateInit(
    void* self,
    void*,
    void*)
{
    ++originalBaseGateInitCalls;
    Put<void*>(app.data(), 0x4a4, self);
    return true;
}

int __fastcall NativeSend(
    void*,
    void*,
    void* stream,
    const void* packet,
    int size,
    char)
{
    ++originalSendCalls;
    Check(stream == reinterpret_cast<void*>(0x12345678), "retry uses the native transport identity");
    Check(
        packet != nullptr &&
            size == 0x4d &&
            Get<std::uint16_t>(packet, 0) == 0x2329,
        "retry sends a freshly constructed native login packet");
    return sendSucceeds ? 1 : 0;
}

void __cdecl ErrorMessage(std::uint16_t, int error, char)
{
    ++errorMessageCalls;
    lastDisplayedError = static_cast<std::uint16_t>(error);
}

int WSAAPI NativeConnect(SOCKET, const sockaddr*, int)
{
    return 0;
}

__declspec(naked) void TickContinuation()
{
    __asm { pushfd }
    __asm { inc dword ptr [tickContinuations] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void BoundaryContinuation()
{
    __asm { pushfd }
    __asm { inc dword ptr [boundaryContinuations] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void FatalContinuation()
{
    __asm { pushfd }
    __asm { inc dword ptr [fatalContinuations] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void EpilogueContinuation()
{
    __asm { pushfd }
    __asm { inc dword ptr [epilogueContinuations] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void SendSiteContinuation()
{
    __asm { pushfd }
    __asm { inc dword ptr [sendSiteContinuations] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void SendEpilogueContinuation()
{
    __asm { pushfd }
    __asm { inc dword ptr [sendEpilogueContinuations] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void __cdecl InvokeSiteWithFrame(SiteHook, void*)
{
    __asm { push ebp }
    __asm { mov eax, [esp+8] }
    __asm { mov ebp, [esp+0ch] }
    __asm { mov ecx, 7fffffffh }
    __asm { add ecx, 1 }
    __asm { pushfd }
    __asm { pop edx }
    __asm { mov dword ptr [siteFlagsBefore], edx }
    __asm { call eax }
    __asm { pushfd }
    __asm { pop edx }
    __asm { mov dword ptr [siteFlagsAfter], edx }
    __asm { pop ebp }
    __asm { ret }
}

void __fastcall MessageSend(void*, void*, void* message, char immediate)
{
    ++messageSendCalls;
    Check(immediate == 1, "recovery dispatches the stock service event synchronously");
    clonedMessage.assign(
        static_cast<const std::uint8_t*>(message),
        static_cast<const std::uint8_t*>(message) + 0x30);
    Check(reconnectBoundaryHook != nullptr, "reconnect queue boundary is registered");
    reconnectBoundaryHook();
    if (invokeDisconnectDuringMessageSend)
        disconnectHook(0, 0, 0x12345678, nullptr, 0);
    if (invokeReconnectFailure)
        reconnectFailureHook();
    if (!preserveLoginUiAfterReconnect)
        ClearLoginServices();
    gateInitPending = queueGateInitAfterReconnect;
    Put<std::uint32_t>(
        app.data(),
        0x4a0,
        makeReadyAfterReconnect ? 4u : 25u);
}

void* __cdecl Symbol(SdgoMod*, const char* name)
{
    if (std::strcmp(name, "recovery.app") == 0)
        return reinterpret_cast<void*>(&App);
    if (std::strcmp(name, "recovery.network") == 0)
        return reinterpret_cast<void*>(&Network);
    if (std::strcmp(name, "recovery.manager") == 0)
        return reinterpret_cast<void*>(&Manager);
    if (std::strcmp(name, "recovery.message_ctor") == 0)
        return reinterpret_cast<void*>(&MessageCtor);
    if (std::strcmp(name, "recovery.message_dtor") == 0)
        return reinterpret_cast<void*>(&MessageDtor);
    if (std::strcmp(name, "recovery.message_send") == 0)
        return reinterpret_cast<void*>(&MessageSend);
    if (std::strcmp(name, "recovery.queue_pop") == 0)
        return reinterpret_cast<void*>(&QueuePop);
    if (std::strcmp(name, "recovery.suspend") == 0)
        return reinterpret_cast<void*>(&Suspend);
    if (std::strcmp(name, "recovery.free") == 0)
        return reinterpret_cast<void*>(&NativeFree);
    if (std::strcmp(name, "recovery.profile") == 0)
        return reinterpret_cast<void*>(&profilePointer);
    if (std::strcmp(name, "recovery.send") == 0)
        return reinterpret_cast<void*>(&NativeSend);
    if (std::strcmp(name, "recovery.send_epilogue") == 0)
        return reinterpret_cast<void*>(&SendEpilogueContinuation);
    if (std::strcmp(name, "recovery.reconnect_epilogue") == 0)
        return reinterpret_cast<void*>(&EpilogueContinuation);
    if (std::strcmp(name, "recovery.error_message") == 0)
        return reinterpret_cast<void*>(&ErrorMessage);
    if (std::strcmp(name, "recovery.pending_remove") == 0)
        return exposePendingRemoveSymbol
            ? reinterpret_cast<void*>(&PendingRemove)
            : nullptr;
    if (std::strcmp(
            name,
            "login_recovery.client.service_manager") == 0)
        return reinterpret_cast<void*>(&ServiceManager);
    if (std::strcmp(name, "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(&ServiceFindByName);
    if (std::strcmp(name, "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(&NativeWStringCtor);
    if (std::strcmp(name, "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(&NativeWStringDtor);
    if (std::strcmp(
            name,
            "login_recovery.client.service_base_ctor") == 0)
        return reinterpret_cast<void*>(&ServiceBaseCtor);
    if (std::strcmp(
            name,
            "login_recovery.client.service_base_dtor") == 0)
        return reinterpret_cast<void*>(&ServiceBaseDtor);
    if (std::strcmp(
            name,
            "login_recovery.client.service_clone") == 0)
        return reinterpret_cast<void*>(&ServiceClone);
    if (std::strcmp(
            name,
            "login_recovery.client.service_register") == 0)
        return reinterpret_cast<void*>(&ServiceRegister);
    if (std::strcmp(
            name,
            "login_recovery.client.service_activate") == 0)
        return reinterpret_cast<void*>(&ServiceActivate);
    if (std::strcmp(
            name,
            "login_recovery.client.login_ui_vtable") == 0)
        return reinterpret_cast<void*>(&loginVtableToken);
    if (std::strcmp(
            name,
            "login_recovery.client.login_ui2_ctor") == 0)
        return derivedUiSymbolMask & kDerivedCtorSymbol
            ? reinterpret_cast<void*>(&LoginUi2Ctor)
            : nullptr;
    if (std::strcmp(
            name,
            "login_recovery.client.login_ui2_dtor") == 0)
        return derivedUiSymbolMask & kDerivedDtorSymbol
            ? reinterpret_cast<void*>(&LoginUi2Dtor)
            : nullptr;
    if (std::strcmp(
            name,
            "login_recovery.client.login_ui2_vtable") == 0)
        return derivedUiSymbolMask & kDerivedVtableSymbol
            ? reinterpret_cast<void*>(&login2VtableToken)
            : nullptr;
    return nullptr;
}

int __cdecl HookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    if (std::strcmp(name, "recovery.login_ctor") == 0) {
        loginCtorHook = reinterpret_cast<LoginCtorHook>(detour);
        *original = reinterpret_cast<void*>(&NativeLoginCtor);
    } else if (std::strcmp(name, "recovery.gate_init") == 0) {
        gateInitHook = reinterpret_cast<GateInitHook>(detour);
        *original = reinterpret_cast<void*>(&NativeGateInit);
    } else if (std::strcmp(name, "recovery.base_gate_init") == 0) {
        baseGateInitHook = reinterpret_cast<GateInitHook>(detour);
        *original = reinterpret_cast<void*>(&NativeBaseGateInit);
    } else if (std::strcmp(name, "recovery.gate_dispatch") == 0) {
        dispatchHook = reinterpret_cast<DispatchHook>(detour);
        *original = reinterpret_cast<void*>(&NativeDispatch);
    } else if (std::strcmp(name, "recovery.packet_callback") == 0) {
        packetHook = reinterpret_cast<PacketHook>(detour);
        *original = reinterpret_cast<void*>(&NativePacket);
    } else if (std::strcmp(name, "recovery.disconnect_callback") == 0) {
        disconnectHook = reinterpret_cast<PacketHook>(detour);
        *original = reinterpret_cast<void*>(&NativeDisconnect);
    } else if (std::strcmp(name, "recovery.gate_watchdog") == 0) {
        gateWatchdogHook = reinterpret_cast<MethodHook>(detour);
        *original = reinterpret_cast<void*>(&NativeGateWatchdog);
    } else if (std::strcmp(name, "recovery.loading_watchdog") == 0) {
        loadingWatchdogHook = reinterpret_cast<MethodHook>(detour);
        *original = reinterpret_cast<void*>(&NativeLoadingWatchdog);
    } else {
        return 0;
    }
    return 1;
}

int __cdecl HookSlot(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    if (std::strcmp(name, "recovery.base_gate_dispatch") != 0)
        return 0;
    baseDispatchHook = reinterpret_cast<DispatchHook>(detour);
    *original = reinterpret_cast<void*>(&NativeBaseDispatch);
    return 1;
}

int __cdecl HookSite(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    if (std::strcmp(name, "recovery.tick") == 0) {
        tickHook = reinterpret_cast<SiteHook>(detour);
        *original = reinterpret_cast<void*>(&TickContinuation);
    } else if (std::strcmp(name, "recovery.reconnect_boundary") == 0) {
        reconnectBoundaryHook = reinterpret_cast<SiteHook>(detour);
        *original = reinterpret_cast<void*>(&BoundaryContinuation);
    } else if (std::strcmp(name, "recovery.reconnect_failure") == 0) {
        reconnectFailureHook = reinterpret_cast<SiteHook>(detour);
        *original = reinterpret_cast<void*>(&FatalContinuation);
    } else if (std::strcmp(name, "recovery.send_boundary") == 0) {
        sendBoundaryHook = reinterpret_cast<SiteHook>(detour);
        *original = reinterpret_cast<void*>(&SendSiteContinuation);
    } else {
        return 0;
    }
    return 1;
}

int __cdecl HookSystemExport(
    SdgoMod*,
    const wchar_t* dll,
    const char* function,
    void*,
    void** original)
{
    if (_wcsicmp(dll, L"ws2_32.dll") != 0 ||
        std::strcmp(function, "connect") != 0)
        return 0;
    *original = reinterpret_cast<void*>(&NativeConnect);
    return 1;
}

void ResetFixture()
{
    app.fill(0);
    network.fill(0);
    gate.fill(0);
    profile.fill(0);
    manager.fill(0);
    loginService.fill(0);
    loginService2.fill(0);
    queuedRecord.fill(0);
    queueMap.fill(nullptr);
    queuedPayload.assign(19, 0xa5);
    clonedMessage.clear();
    logs.clear();
    profilePointer = profile.data();
    loginCtorHook = nullptr;
    dispatchHook = nullptr;
    baseDispatchHook = nullptr;
    packetHook = nullptr;
    disconnectHook = nullptr;
    gateWatchdogHook = nullptr;
    loadingWatchdogHook = nullptr;
    gateInitHook = nullptr;
    baseGateInitHook = nullptr;
    tickHook = nullptr;
    reconnectBoundaryHook = nullptr;
    reconnectFailureHook = nullptr;
    sendBoundaryHook = nullptr;
    originalCtorCalls = 0;
    originalDispatchCalls = 0;
    originalBaseDispatchCalls = 0;
    originalPacketCalls = 0;
    originalDisconnectCalls = 0;
    originalGateWatchdogCalls = 0;
    originalLoadingWatchdogCalls = 0;
    originalGateInitCalls = 0;
    originalBaseGateInitCalls = 0;
    originalSendCalls = 0;
    messageCtorCalls = 0;
    messageDtorCalls = 0;
    messageSendCalls = 0;
    suspendCalls = 0;
    queuePopCalls = 0;
    freeCalls = 0;
    errorMessageCalls = 0;
    tickContinuations = 0;
    boundaryContinuations = 0;
    fatalContinuations = 0;
    epilogueContinuations = 0;
    sendSiteContinuations = 0;
    sendEpilogueContinuations = 0;
    nativeWStringCtorCalls = 0;
    nativeWStringDtorCalls = 0;
    serviceCtorCalls = 0;
    serviceDtorCalls = 0;
    serviceCloneCalls = 0;
    serviceRegisterCalls = 0;
    serviceActivateCalls = 0;
    loginUi2CtorCalls = 0;
    loginUi2DtorCalls = 0;
    pendingRemoveCalls = 0;
    pendingNameDtorCalls = 0;
    lastServiceCloneSize = 0;
    sendSucceeds = true;
    invokeReconnectFailure = false;
    invokeDisconnectDuringMessageSend = false;
    makeReadyAfterReconnect = true;
    recreateLoginUiAfterReconnect = true;
    preserveLoginUiAfterReconnect = false;
    queueGateInitAfterReconnect = true;
    baseLoginServiceRegistered = false;
    derivedLoginServiceRegistered = false;
    nativeUiAbiMismatch = false;
    loginUi2DtorSawFullSource = false;
    gateInitPending = false;
    replayCredentialsMatched = false;
    exposePendingRemoveSymbol = true;
    pendingGuardPresent = false;
    pendingRemoveSawExpectedKey = false;
    pendingRemoveSawGuardPresent = false;
    retirePendingGuardOnReply = true;
    derivedUiSymbolMask = kAllDerivedUiSymbols;
    lastDisplayedError = 0;
    siteFlagsBefore = 0;
    siteFlagsAfter = 0;
    lastServiceActivateContext = nullptr;
    activeApi = {};
    Put<std::uint32_t>(app.data(), 0x4a0, 4);
    Put<HWND>(app.data(), 0x3c, nullptr);
    Put<void*>(network.data(), 0x14, reinterpret_cast<void*>(0x12345678));
    Put<std::uint32_t>(gate.data(), 0x2058, 3);
    Put<std::uint32_t>(profile.data(), 0x138, 0x10203040);
    SetDerivedLoginServiceReady();
    auto* queue = app.data() + 0x2530;
    Put<std::uint8_t**>(queue, 4, queueMap.data());
    Put<std::uint32_t>(queue, 8, static_cast<std::uint32_t>(queueMap.size()));
}

struct Loaded {
    HMODULE module = nullptr;
    SdgoModLoadFn entry = nullptr;
    SdgoLoginRecoveryGetSnapshotV1Fn snapshot = nullptr;

    Loaded() = default;
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    Loaded(Loaded&& other) noexcept
        : module(std::exchange(other.module, nullptr)),
          entry(std::exchange(other.entry, nullptr)),
          snapshot(std::exchange(other.snapshot, nullptr))
    {
    }
    Loaded& operator=(Loaded&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (module)
            FreeLibrary(module);
        module = std::exchange(other.module, nullptr);
        entry = std::exchange(other.entry, nullptr);
        snapshot = std::exchange(other.snapshot, nullptr);
        return *this;
    }
    ~Loaded()
    {
        if (module)
            FreeLibrary(module);
    }
};

Loaded Load(const wchar_t* path)
{
    ResetFixture();
    Loaded loaded;
    loaded.module = LoadLibraryW(path);
    Check(loaded.module != nullptr, "actual login-recovery client DLL loads");
    loaded.entry = reinterpret_cast<SdgoModLoadFn>(
        GetProcAddress(loaded.module, "SdgoModLoad"));
    loaded.snapshot = reinterpret_cast<SdgoLoginRecoveryGetSnapshotV1Fn>(
        GetProcAddress(
            loaded.module,
            SDGO_LOGIN_RECOVERY_SNAPSHOT_EXPORT));
    Check(
        loaded.entry != nullptr && loaded.snapshot != nullptr,
        "actual DLL exports entry and read-only recovery snapshot");
    return loaded;
}

SdgoApi Api(const char* host)
{
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.host_key = host;
    api.Log = &Log;
    api.Symbol = &Symbol;
    api.HookCode = &HookCode;
    api.HookSlot = &HookSlot;
    api.HookSystemExport = &HookSystemExport;
    api.HookSite = &HookSite;
    return api;
}

void Activate(Loaded& loaded)
{
    activeApi = Api("GOnline-54BE1568-00C97000");
    auto* fakeMod = reinterpret_cast<SdgoMod*>(1);
    Check(
        loaded.entry(&activeApi, fakeMod) != 0,
        "actual DLL registers all verified native recovery hooks");
    Check(
        loginCtorHook &&
            dispatchHook &&
            baseDispatchHook &&
            packetHook &&
            disconnectHook &&
            gateWatchdogHook &&
            loadingWatchdogHook &&
            gateInitHook &&
            baseGateInitHook &&
            tickHook &&
            reconnectBoundaryHook &&
            reconnectFailureHook &&
            sendBoundaryHook,
        "derived and base Gate hooks plus credential, callback, watchdog, reconnect and send boundaries are all active");
    Check(
        gateInitHook(gate.data(), nullptr, gateContext) &&
            originalGateInitCalls == 1 &&
            originalBaseGateInitCalls == 0,
        "derived GateInit capture preserves the original native boolean result without invoking the base observer");
}

SdgoLoginRecoverySnapshotV1 Snapshot(Loaded& loaded)
{
    SdgoLoginRecoverySnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = 1;
    Check(loaded.snapshot(&snapshot) != 0, "credential-free runtime snapshot is readable");
    return snapshot;
}

SdgoLoginRecoverySnapshotV1 SnapshotQuiet(Loaded& loaded)
{
    SdgoLoginRecoverySnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = 1;
    if (!loaded.snapshot(&snapshot))
        throw std::runtime_error("credential-free runtime snapshot became unreadable");
    return snapshot;
}

std::size_t CountWritableModuleBytes(
    HMODULE module,
    const char* bytes,
    std::size_t length)
{
    if (!module || !bytes || !length)
        return 0;
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        throw std::runtime_error("loaded DLL has no valid DOS header");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        throw std::runtime_error("loaded DLL has no valid NT header");
    const auto* section = IMAGE_FIRST_SECTION(nt);
    std::size_t matches = 0;
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        if (!(section[index].Characteristics & IMAGE_SCN_MEM_WRITE))
            continue;
        const std::size_t size = section[index].Misc.VirtualSize;
        if (size < length)
            continue;
        const auto* start = base + section[index].VirtualAddress;
        for (std::size_t offset = 0; offset + length <= size; ++offset) {
            if (std::memcmp(start + offset, bytes, length) == 0)
                ++matches;
        }
    }
    return matches;
}

void CheckCredentialCopies(
    Loaded& loaded,
    bool expected,
    const char* message)
{
    const bool userFound =
        CountWritableModuleBytes(
            loaded.module,
            kUser,
            std::strlen(kUser)) != 0;
    const bool hashFound =
        CountWritableModuleBytes(
            loaded.module,
            kHash,
            std::strlen(kHash)) != 0;
    Check(
        expected ? userFound && hashFound : !userFound && !hashFound,
        message);
}

void Tick()
{
    if (gateInitPending) {
        gateInitPending = false;
        if (!gateInitHook(gate.data(), nullptr, gateContext))
            throw std::runtime_error(
                "derived native GateInit rejected the next-frame context");
    }
    const int before = tickContinuations;
    tickHook();
    Check(
        tickContinuations == before + 1,
        "tick HookSite preserves control flow through its original continuation");
}

void TickQuiet()
{
    if (gateInitPending) {
        gateInitPending = false;
        if (!gateInitHook(gate.data(), nullptr, gateContext))
            throw std::runtime_error(
                "derived native GateInit rejected the next-frame context");
    }
    const int before = tickContinuations;
    tickHook();
    if (tickContinuations != before + 1)
        throw std::runtime_error(
            "tick HookSite did not preserve its continuation");
}

void Capture()
{
    std::array<std::uint8_t, 0x4d> packet{};
    Check(
        loginCtorHook(
            packet.data(),
            nullptr,
            kUser,
            kHash) == packet.data(),
        "semantic login constructor capture preserves the original constructor result");
}

void Dispatch(std::uint16_t opcode, std::uint16_t status)
{
    std::array<std::uint8_t, 16> packet{};
    Put<std::uint16_t>(packet.data(), 6, status);
    Check(
        dispatchHook(
            gate.data(),
            nullptr,
            0,
            0,
            0,
            opcode,
            packet.data()) == 77,
        "reply observer preserves the original login-service result");
}

bool WaitForRebuild(Loaded& loaded, std::uint32_t baseline, DWORD timeout)
{
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        TickQuiet();
        if (SnapshotQuiet(loaded).rebuilds > baseline)
            return true;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    return false;
}

void MakeGateReady()
{
    Put<std::uint32_t>(app.data(), 0x4a0, 4);
    Put<std::uint32_t>(gate.data(), 0x2058, 3);
    Dispatch(0, 0);
}

void InstallOneQueuedPacket()
{
    auto* queue = app.data() + 0x2530;
    Put<void*>(queuedRecord.data(), 0xc, queuedPayload.data());
    queueMap[0] = queuedRecord.data();
    Put<std::uint32_t>(queue, 0xc, 0);
    Put<std::uint32_t>(queue, 0x10, 1);
}

void AssertNoSecretsInLogs()
{
    for (const std::string& line : logs) {
        Check(
            line.find(kUser) == std::string::npos &&
                line.find(kHash) == std::string::npos,
            "runtime logging contains neither username nor password hash");
    }
}

void TestScope(const wchar_t* path)
{
    {
        Loaded loaded = Load(path);
        Check(!loaded.entry(nullptr, nullptr), "missing API is rejected safely");
        SdgoApi wrong = Api("WorldServer-548A86E3-0010C000");
        Check(
            !loaded.entry(&wrong, nullptr) &&
                !loginCtorHook &&
                !tickHook,
            "unknown host fails closed before installing any hook");
    }
    {
        Loaded loaded = Load(path);
        exposePendingRemoveSymbol = false;
        activeApi = Api("GOnline-54BE1568-00C97000");
        auto* fakeMod = reinterpret_cast<SdgoMod*>(1);
        Check(
            loaded.entry(&activeApi, fakeMod) == 0 &&
                !loginCtorHook &&
                !gateInitHook &&
                !tickHook,
            "missing native pending-login removal binding fails closed before hooks");
    }
}

void TestNativeLoginUiBindings(const wchar_t* path)
{
    {
        Loaded loaded = Load(path);
        derivedUiSymbolMask = 0;
        ClearLoginServices();
        SetBaseLoginServiceReady();
        Activate(loaded);
        Tick();
        const auto ready = Snapshot(loaded);
        Check(
            ready.return_ready &&
                ready.app_state == 4 &&
                ready.gate_state == 3 &&
                baseLoginServiceRegistered &&
                !derivedLoginServiceRegistered &&
                Get<void*>(loginService.data(), 0) ==
                    &loginVtableToken &&
                Get<void*>(loginService.data(), 0x44) &&
                Get<void*>(loginService.data(), 0x48) &&
                Get<void*>(loginService.data(), 0x60) &&
                Get<void*>(loginService.data(), 0x64),
            "base-only ABI still resolves the existing LoginInterFace controls");
        Check(
            serviceCtorCalls == 0 &&
                serviceDtorCalls == 0 &&
                loginUi2CtorCalls == 0 &&
                loginUi2DtorCalls == 0 &&
                nativeWStringCtorCalls ==
                    nativeWStringDtorCalls &&
                !nativeUiAbiMismatch,
            "all three absent derived symbols retain the old base ABI without fallback reconstruction");
    }

    {
        Loaded loaded = Load(path);
        derivedUiSymbolMask = 0;
        ClearLoginServices();
        SetBaseLoginServiceReady();
        Activate(loaded);
        recreateLoginUiAfterReconnect = false;
        invokeReconnectFailure = true;
        Capture();
        Dispatch(0x232a, 1001);
        Tick();
        Tick();
        const auto restored = Snapshot(loaded);
        Check(
            restored.state == static_cast<std::uint32_t>(
                State::ReturnedToLogin) &&
                restored.return_ready &&
                serviceCtorCalls == 1 &&
                serviceDtorCalls == 1 &&
                loginUi2CtorCalls == 0 &&
                loginUi2DtorCalls == 0 &&
                serviceCloneCalls == 1 &&
                lastServiceCloneSize == loginService.size() &&
                serviceRegisterCalls == 1 &&
                serviceActivateCalls == 1 &&
                baseLoginServiceRegistered &&
                !derivedLoginServiceRegistered &&
                Get<void*>(loginService.data(), 0) ==
                    &loginVtableToken &&
                nativeWStringCtorCalls ==
                    nativeWStringDtorCalls &&
                !nativeUiAbiMismatch,
            "all three absent derived symbols preserve the old base fallback constructor and destructor ABI");
    }

    constexpr std::array<std::uint8_t, 6> kPartialMasks{
        kDerivedCtorSymbol,
        kDerivedDtorSymbol,
        kDerivedVtableSymbol,
        static_cast<std::uint8_t>(
            kDerivedCtorSymbol |
            kDerivedDtorSymbol),
        static_cast<std::uint8_t>(
            kDerivedCtorSymbol |
            kDerivedVtableSymbol),
        static_cast<std::uint8_t>(
            kDerivedDtorSymbol |
            kDerivedVtableSymbol),
    };
    for (const std::uint8_t mask : kPartialMasks) {
        Loaded loaded = Load(path);
        derivedUiSymbolMask = mask;
        activeApi = Api("GOnline-54BE1568-00C97000");
        auto* fakeMod = reinterpret_cast<SdgoMod*>(1);
        Check(
            loaded.entry(&activeApi, fakeMod) == 0 &&
                !loginCtorHook &&
                !gateInitHook &&
                !tickHook,
            "partial derived LoginInterFace2 symbol groups fail closed before hooks");
    }
}

void TestRetiredGateIsNotDereferenced(const wchar_t* path)
{
    struct VirtualPage
    {
        void* value = nullptr;
        ~VirtualPage()
        {
            if (value)
                VirtualFree(value, 0, MEM_RELEASE);
        }
    } retired;

    Loaded loaded = Load(path);
    Activate(loaded);
    retired.value = VirtualAlloc(
        nullptr,
        0x3000,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);
    Check(
        retired.value != nullptr,
        "fixture reserves a disposable native gate owner");
    Put<std::uint32_t>(retired.value, 0x2058, 3);
    Check(
        gateInitHook(retired.value, nullptr, gateContext),
        "derived GateInit accepts the disposable gate owner");
    Put<std::uint32_t>(app.data(), 0x4a0, 25);
    DWORD oldProtection = 0;
    Check(
        VirtualProtect(
            retired.value,
            0x3000,
            PAGE_NOACCESS,
            &oldProtection) != 0,
        "retired gate storage becomes inaccessible before the state-25 tick");
    Tick();
    const auto snapshot = Snapshot(loaded);
    bool guardRejectedState = false;
    for (const std::string& line : logs) {
        guardRejectedState =
            guardRejectedState ||
            line.find(
                "native recovery guard refused unexpected runtime state") !=
                std::string::npos;
    }
    Check(
        snapshot.app_state == 25 &&
            snapshot.gate_state == UINT32_MAX &&
            !guardRejectedState,
        "leaving app state 4 clears the retired gate without dereferencing its inaccessible storage");
}

void TestOldLoginUiCannotAcknowledgeFreshReturn(
    const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    Check(
        Get<void*>(app.data(), 0x4a4) == gate.data(),
        "initial GateInit publishes the native app gate owner");
    preserveLoginUiAfterReconnect = true;
    queueGateInitAfterReconnect = false;
    invokeReconnectFailure = true;
    retirePendingGuardOnReply = false;
    Capture();
    Check(
        pendingGuardPresent,
        "initial native login request seeds the pending authentication guard");
    Dispatch(0x232a, 1001);
    Tick();
    Tick();
    const auto stale = Snapshot(loaded);
    Check(
        stale.state == static_cast<std::uint32_t>(
            State::AwaitingRecoveryCompletion) &&
            !stale.return_ready &&
            stale.gate_state == UINT32_MAX &&
            !gateInitPending &&
            Get<void*>(app.data(), 0x4a4) ==
                gate.data() &&
            derivedLoginServiceRegistered &&
            Get<void*>(loginService2.data(), 0) ==
                &login2VtableToken &&
            loginService2[4] != 0 &&
            loginService2[0x2d] == 0 &&
            Get<void*>(loginService2.data(), 0x44) &&
            pendingGuardPresent &&
            pendingRemoveCalls == 0 &&
            pendingNameDtorCalls == 0 &&
            errorMessageCalls == 0,
        "old still-visible LoginInterFace2 neither acknowledges the return nor retires its native request guard");

    ClearLoginServices();
    alignas(4) std::array<std::uint8_t, 0x2100> freshGate{};
    Put<std::uint32_t>(freshGate.data(), 0x2058, 3);
    Check(
        gateInitHook(
            freshGate.data(),
            nullptr,
            gateContext) &&
            Get<void*>(app.data(), 0x4a4) ==
                freshGate.data() &&
            derivedLoginServiceRegistered,
        "fresh GateInit publishes the replacement owner and its real registered login UI");
    Tick();
    const auto restored = Snapshot(loaded);
    Check(
        restored.state == static_cast<std::uint32_t>(
            State::ReturnedToLogin) &&
            restored.outcome == static_cast<std::uint32_t>(
                Outcome::PasswordRejected) &&
            restored.return_ready &&
            errorMessageCalls == 1 &&
            lastDisplayedError == 1001 &&
            serviceCtorCalls == 0 &&
            loginUi2CtorCalls == 0 &&
            !pendingGuardPresent &&
            pendingRemoveCalls == 1 &&
            pendingNameDtorCalls == 1 &&
            pendingRemoveSawExpectedKey &&
            pendingRemoveSawGuardPresent,
        "fresh UI completion consumes and destroys the by-value pending-login key exactly once");
}

void TestMismatchedGateOwnerCannotConstructUi(
    const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    recreateLoginUiAfterReconnect = false;
    queueGateInitAfterReconnect = false;
    invokeReconnectFailure = true;
    Capture();
    Dispatch(0x232a, 1001);
    Tick();

    alignas(4) std::array<std::uint8_t, 0x2100>
        cachedGate{};
    alignas(4) std::array<std::uint8_t, 0x2100>
        currentOwner{};
    Put<std::uint32_t>(cachedGate.data(), 0x2058, 3);
    Put<std::uint32_t>(currentOwner.data(), 0x2058, 3);
    Check(
        gateInitHook(
            cachedGate.data(),
            nullptr,
            gateContext),
        "fixture captures a fresh gate before ownership changes");
    Put<void*>(
        app.data(),
        0x4a4,
        currentOwner.data());
    Tick();
    const auto mismatched = Snapshot(loaded);
    Check(
        mismatched.state == static_cast<std::uint32_t>(
            State::AwaitingRecoveryCompletion) &&
            !mismatched.return_ready &&
            Get<void*>(app.data(), 0x4a4) !=
                cachedGate.data() &&
            serviceCtorCalls == 0 &&
            loginUi2CtorCalls == 0 &&
            serviceCloneCalls == 0 &&
            serviceRegisterCalls == 0 &&
            serviceActivateCalls == 0 &&
            pendingRemoveCalls == 0,
        "owner mismatch blocks eligible fallback before every UI constructor, registration and activation");

    Check(
        gateInitHook(
            currentOwner.data(),
            nullptr,
            gateContext) &&
            Get<void*>(app.data(), 0x4a4) ==
                currentOwner.data(),
        "matching GateInit publishes the current owner");
    Tick();
    const auto restored = Snapshot(loaded);
    Check(
        restored.state == static_cast<std::uint32_t>(
            State::ReturnedToLogin) &&
            restored.return_ready &&
            loginUi2CtorCalls == 1 &&
            loginUi2DtorCalls == 1 &&
            serviceCloneCalls == 1 &&
            serviceRegisterCalls == 1 &&
            serviceActivateCalls == 1 &&
            pendingRemoveCalls == 1 &&
            pendingNameDtorCalls == 1 &&
            !nativeUiAbiMismatch,
        "matching current ownership permits one fallback lifecycle and completed return");
}

void TestNullGateContextFallbackIsReady(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    recreateLoginUiAfterReconnect = false;
    queueGateInitAfterReconnect = false;
    invokeReconnectFailure = true;
    Capture();
    Dispatch(0x232a, 1001);
    Tick();
    const auto pending = Snapshot(loaded);
    Check(
        pending.state == static_cast<std::uint32_t>(
            State::AwaitingRecoveryCompletion) &&
            !pending.return_ready &&
            !gateInitPending &&
            !derivedLoginServiceRegistered &&
            !pendingGuardPresent &&
            pendingRemoveCalls == 0,
        "null-context regression reaches the real post-rebuild wait before GateInit");

    alignas(4) std::array<std::uint8_t, 0x2100> freshGate{};
    Put<std::uint32_t>(freshGate.data(), 0x2058, 3);
    Check(
        gateInitHook(
            freshGate.data(),
            nullptr,
            nullptr) &&
            Get<void*>(app.data(), 0x4a4) ==
                freshGate.data(),
        "stock GateInit accepts a null initialization context and publishes the fresh owner");
    Tick();
    const auto restored = Snapshot(loaded);
    Check(
        restored.state == static_cast<std::uint32_t>(
            State::ReturnedToLogin) &&
            restored.outcome == static_cast<std::uint32_t>(
                Outcome::PasswordRejected) &&
            restored.return_ready &&
            restored.gate_state == 3 &&
            derivedLoginServiceRegistered &&
            serviceActivateCalls == 1 &&
            lastServiceActivateContext == nullptr &&
            loginUi2CtorCalls == 1 &&
            loginUi2DtorCalls == 1 &&
            loginUi2DtorSawFullSource &&
            lastServiceCloneSize == loginService2.size() &&
            pendingRemoveCalls == 1 &&
            pendingNameDtorCalls == 1 &&
            pendingRemoveSawExpectedKey &&
            !pendingRemoveSawGuardPresent &&
            nativeWStringCtorCalls ==
                nativeWStringDtorCalls &&
            !nativeUiAbiMismatch &&
            errorMessageCalls == 1 &&
            lastDisplayedError == 1001,
        "null-context native activation reconstructs a ready derived login UI and completes the return");
}

void TestSuccessAndTerminalReject(const wchar_t* path)
{
    {
        Loaded loaded = Load(path);
        Activate(loaded);
        CheckCredentialCopies(
            loaded,
            false,
            "fixture credentials are absent from writable DLL memory before capture");
        Capture();
        CheckCredentialCopies(
            loaded,
            true,
            "semantic capture places the fixture credentials only in the DLL's bounded writable storage");
        Tick();
        auto captured = Snapshot(loaded);
        Check(
            captured.credentials_present &&
                captured.recovery_active &&
                captured.generation == 1,
            "valid semantic credentials begin exactly one bounded recovery generation");
        Dispatch(0x232b, 0);
        Put<std::uint32_t>(app.data(), 0x4a0, 5);
        Tick();
        const auto completed = Snapshot(loaded);
        Check(
            completed.state == static_cast<std::uint32_t>(State::Succeeded) &&
                completed.outcome == static_cast<std::uint32_t>(Outcome::Success) &&
                !completed.credentials_present &&
                !completed.recovery_active,
            "authentication is successful only after native lobby or hangar entry, then credentials are erased");
        CheckCredentialCopies(
            loaded,
            false,
            "successful native login physically clears both fixture credentials from writable DLL memory");
        AssertNoSecretsInLogs();
    }

    {
        Loaded loaded = Load(path);
        Activate(loaded);
        Capture();
        CheckCredentialCopies(
            loaded,
            true,
            "terminal-reject fixture begins with both credentials captured");
        const auto appBefore = app;
        const auto gateBefore = gate;
        Dispatch(0x232a, 1001);
        Check(
            app == appBefore && gate == gateBefore,
            "password rejection observation does not synthesize a reply or write native application state");
        CheckCredentialCopies(
            loaded,
            false,
            "true wrong-password rejection physically clears both credentials before recovery UI work");
        Tick();
        Tick();
        const auto rejected = Snapshot(loaded);
        Check(
            rejected.last_reject == 1001 &&
                !rejected.credentials_present &&
                rejected.automatic_retries == 0 &&
                rejected.retry_sends == 0 &&
                rejected.outcome == static_cast<std::uint32_t>(
                    Outcome::PasswordRejected) &&
                rejected.state == static_cast<std::uint32_t>(
                    State::ReturnedToLogin) &&
                errorMessageCalls == 1 &&
                lastDisplayedError == 1001,
            "wrong password is terminal, never retries, restores native login UI and reports the authentic error");
        AssertNoSecretsInLogs();
    }
}

void TestDuplicateSubmitBoundary(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    Capture();
    Tick();
    const auto before = Snapshot(loaded);
    Capture();
    const auto duplicate = Snapshot(loaded);
    Check(
        duplicate.generation == before.generation &&
            duplicate.automatic_retries == 0 &&
            originalCtorCalls == 2,
        "a second native submit during recovery preserves the original identity and generation");

    std::array<std::uint8_t, 0x4d> packet{};
    std::array<std::uint8_t, 0x20> frame{};
    Put<std::uint16_t>(packet.data(), 0, 0x2329);
    Put<const void*>(frame.data(), 0xc, packet.data());
    Put<int>(frame.data(), 0x10, static_cast<int>(packet.size()));
    InvokeSiteWithFrame(sendBoundaryHook, frame.data());
    constexpr DWORD kArithmeticFlags =
        0x00000001u |
        0x00000004u |
        0x00000010u |
        0x00000040u |
        0x00000080u |
        0x00000800u;
    Check(
        sendEpilogueContinuations == 1 &&
            sendSiteContinuations == 0,
        "send boundary skips exactly one concurrent duplicate native 2329 packet");
    Check(
        (siteFlagsBefore & kArithmeticFlags) ==
            (siteFlagsAfter & kArithmeticFlags),
        "send HookSite preserves the caller's arithmetic flags while selecting its epilogue");

    InvokeSiteWithFrame(sendBoundaryHook, frame.data());
    Check(
        sendEpilogueContinuations == 1 &&
            sendSiteContinuations == 1,
        "duplicate suppression is consumed once and the next native send follows the original continuation");
    AssertNoSecretsInLogs();
}

void TestTerminalReturnReconstructsLoginUi(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    recreateLoginUiAfterReconnect = false;
    invokeReconnectFailure = true;
    Capture();
    Dispatch(0x232a, 1001);
    Tick();
    const auto pending = Snapshot(loaded);
    Check(
        pending.state == static_cast<std::uint32_t>(
            State::AwaitingRecoveryCompletion) &&
            !pending.return_ready &&
            serviceCtorCalls == 0 &&
            serviceRegisterCalls == 0 &&
            serviceActivateCalls == 0 &&
            gateInitPending,
        "first Mode 4 frame remains pending and does not invent a login UI before the new GateInit");
    Tick();
    const auto restored = Snapshot(loaded);
    Check(
        restored.state == static_cast<std::uint32_t>(
            State::ReturnedToLogin) &&
            restored.outcome == static_cast<std::uint32_t>(
                Outcome::PasswordRejected) &&
            restored.return_ready &&
            !restored.credentials_present,
        "terminal failure returns only after a real interactive native login service is present");
    Check(
        serviceCtorCalls == 0 &&
            serviceDtorCalls == 0 &&
            loginUi2CtorCalls == 1 &&
            loginUi2DtorCalls == 1 &&
            loginUi2DtorSawFullSource &&
            serviceCloneCalls == 1 &&
            lastServiceCloneSize == loginService2.size() &&
            serviceRegisterCalls == 1 &&
            serviceActivateCalls == 1 &&
            nativeWStringCtorCalls >= 3 &&
            nativeWStringCtorCalls == nativeWStringDtorCalls &&
            !nativeUiAbiMismatch,
        "fallback login UI uses the full A0 derived constructor, destructor and one stock clone-register-activate lifecycle");
    Check(
        derivedLoginServiceRegistered &&
            !baseLoginServiceRegistered &&
            Get<void*>(loginService2.data(), 0) ==
                &login2VtableToken &&
            loginService2[4] != 0 &&
            loginService2[0x2d] == 0 &&
            loginService2[0x9f] == 0xa2 &&
            Get<void*>(loginService2.data(), 0x44) &&
            Get<void*>(loginService2.data(), 0x48) &&
            Get<void*>(loginService2.data(), 0x60) &&
            Get<void*>(loginService2.data(), 0x64),
        "restored LoginInterFace2 exposes its root, button, username and password controls");
    Check(
        epilogueContinuations == 1 &&
            fatalContinuations == 0 &&
            errorMessageCalls == 1 &&
            lastDisplayedError == 1001,
        "recovery-only failure skips shutdown and reports the authentic terminal error after UI restoration");
    CheckCredentialCopies(
        loaded,
        false,
        "fallback UI restoration retains no fixture credential bytes in writable DLL memory");
    AssertNoSecretsInLogs();
}

void TestReconnectAndQueueBoundary(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    InstallOneQueuedPacket();
    Capture();
    Dispatch(0x232a, 1002);
    Check(
        WaitForRebuild(loaded, 0, 2500),
        "authenticated old-session rejection reaches the first bounded native reconnect");
    auto rebuilt = Snapshot(loaded);
    Check(
        messageCtorCalls == 1 &&
            messageSendCalls == 1 &&
            messageDtorCalls == 1 &&
            suspendCalls == 1,
        "reconnect uses one caller-owned native service message with matching destruction");
    Check(
        clonedMessage.size() == 0x30 &&
            Get<std::uint32_t>(clonedMessage.data(), 0xc) == 0x13 &&
            Get<std::uint32_t>(clonedMessage.data(), 0x2c) == 6,
        "stock logout/reconnect service command 13 result 6 survives source destruction");
    Check(
        boundaryContinuations == 1 &&
            rebuilt.purged_packets == 1 &&
            queuePopCalls == 1 &&
            freeCalls == 1 &&
            Get<std::uint32_t>(app.data() + 0x2530, 0x10) == 0,
        "old callback producer stops before its queued payload is freed and popped exactly once");

    const int packetsBefore = originalPacketCalls;
    std::array<std::uint8_t, 8> packet{};
    Check(
        packetHook(0, 0, 1, packet.data(), packet.size()) == 42 &&
            originalPacketCalls == packetsBefore + 1,
        "fresh post-boundary network callback reaches the original game path");
    MakeGateReady();
    Tick();
    rebuilt = Snapshot(loaded);
    Check(
        rebuilt.retry_sends == 1 &&
            originalSendCalls == 1 &&
            replayCredentialsMatched,
        "ready native login UI rebuild sends one freshly constructed semantic retry");
    AssertNoSecretsInLogs();
}

void TestDisconnectDuringPendingAction(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    makeReadyAfterReconnect = false;
    invokeDisconnectDuringMessageSend = true;
    Capture();
    Dispatch(0x232a, 1002);
    Check(
        WaitForRebuild(loaded, 0, 2500),
        "old-session recovery reaches a rebuilt but not-yet-ready native login transport");
    const auto pending = Snapshot(loaded);
    Check(
        pending.state == static_cast<std::uint32_t>(
            State::AwaitingOutcome) &&
            pending.action_id != 0 &&
            pending.automatic_retries == 1 &&
            pending.retry_sends == 0 &&
            originalDisconnectCalls == 0,
        "disconnect callback during the native action is recorded without cross-thread policy mutation");
    Tick();
    const auto failed = Snapshot(loaded);
    Check(
        originalDisconnectCalls == 0 &&
            failed.state == static_cast<std::uint32_t>(State::Backoff) &&
            failed.automatic_retries == 2 &&
            failed.retry_sends == 0 &&
            failed.credentials_present,
        "next tick consumes the matching connection epoch and schedules exactly one bounded retry");
    AssertNoSecretsInLogs();
}

void TestInvalidQueueFailsClosed(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    auto* queue = app.data() + 0x2530;
    Put<std::uint32_t>(queue, 8, 1);
    Put<std::uint32_t>(queue, 0xc, 0);
    Put<std::uint32_t>(queue, 0x10, 2);
    Capture();
    Dispatch(0x232a, 1002);
    Check(
        WaitForRebuild(loaded, 0, 2500),
        "malformed old queue is encountered only inside a requested reconnect");
    const int packetsBefore = originalPacketCalls;
    std::array<std::uint8_t, 8> packet{};
    Check(
        packetHook(0, 0, 1, packet.data(), packet.size()) == 1 &&
            originalPacketCalls == packetsBefore,
        "closing fence drops callbacks after an invalid queue instead of dispatching stale bytes");
    Tick();
    const auto failed = Snapshot(loaded);
    Check(
        failed.dropped_callbacks == 1 &&
            failed.purged_packets == 0 &&
            queuePopCalls == 0 &&
            freeCalls == 0,
        "invalid queue refuses every payload free and records the dropped callback");
}

void TestFatalConnectBranch(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    invokeReconnectFailure = true;
    Capture();
    Dispatch(0x232a, 1002);
    Check(
        WaitForRebuild(loaded, 0, 2500),
        "recovery reaches the native reconnect failure branch");
    const auto failed = Snapshot(loaded);
    Check(
        failed.automatic_retries == 2 &&
            failed.rebuilds == 1 &&
            failed.retry_sends == 0 &&
            failed.state == static_cast<std::uint32_t>(State::Backoff) &&
            epilogueContinuations == 1 &&
            fatalContinuations == 0,
        "recovery-only connect failure selects the verified native epilogue and consumes one bounded transport retry");
    AssertNoSecretsInLogs();
}

void TestBoundedRetries(const wchar_t* path)
{
    Loaded loaded = Load(path);
    Activate(loaded);
    Capture();
    Tick();
    constexpr std::array<DWORD, 3> kTimeouts{
        2500,
        3500,
        5500};
    for (std::uint32_t retry = 1; retry <= 3; ++retry) {
        Dispatch(0x232a, 1002);
        const auto before = Snapshot(loaded);
        Check(
            WaitForRebuild(
                loaded,
                before.rebuilds,
                kTimeouts[retry - 1]),
            "each admitted retry performs one bounded native reconnect");
        Tick();
        const auto after = Snapshot(loaded);
        Check(
            after.automatic_retries == retry &&
                after.rebuilds == retry &&
                after.retry_sends == retry &&
                originalSendCalls == static_cast<int>(retry) &&
                after.state == static_cast<std::uint32_t>(
                    State::AwaitingOutcome),
            "each successful rebuild sends exactly one semantic retry and returns to bounded outcome waiting");
    }
    Dispatch(0x232a, 1002);
    const auto beforeReturn = Snapshot(loaded);
    Check(
        WaitForRebuild(loaded, beforeReturn.rebuilds, 1000),
        "fourth old-session rejection enters one return-to-login action");
    Tick();
    const auto exhausted = Snapshot(loaded);
    Check(
        exhausted.automatic_retries == 3 &&
            exhausted.rebuilds == 4 &&
            exhausted.retry_sends == 3 &&
            !exhausted.credentials_present &&
            exhausted.outcome == static_cast<std::uint32_t>(
                Outcome::RetryLimitReached) &&
            exhausted.state == static_cast<std::uint32_t>(
                State::ReturnedToLogin),
        "actual DLL admits no more than three automatic retries, then returns to native login without another send");
    CheckCredentialCopies(
        loaded,
        false,
        "retry-limit return physically clears both fixture credentials from writable DLL memory");
    AssertNoSecretsInLogs();
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return 2;
    try {
        TestScope(argv[1]);
        TestNativeLoginUiBindings(argv[1]);
        TestRetiredGateIsNotDereferenced(argv[1]);
        TestOldLoginUiCannotAcknowledgeFreshReturn(argv[1]);
        TestMismatchedGateOwnerCannotConstructUi(argv[1]);
        TestNullGateContextFallbackIsReady(argv[1]);
        TestSuccessAndTerminalReject(argv[1]);
        TestDuplicateSubmitBoundary(argv[1]);
        TestTerminalReturnReconstructsLoginUi(argv[1]);
        TestReconnectAndQueueBoundary(argv[1]);
        TestDisconnectDuringPendingAction(argv[1]);
        TestInvalidQueueFailsClosed(argv[1]);
        TestFatalConnectBranch(argv[1]);
        TestBoundedRetries(argv[1]);
        std::puts("ACTUAL LOGIN RECOVERY CLIENT DLL ABI AND FAILURE BOUNDARIES PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
