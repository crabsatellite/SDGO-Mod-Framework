// Exercise the production Agent fix DLL at its exact HookSite ABI. No server,
// client, account, socket or database is opened.
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <xmmintrin.h>

#include "sdgo/sdgo_api.h"

#if !defined(_M_IX86)
#error agent_room_view_error_fix_runtime_test requires x86
#endif

namespace
{
    unsigned g_checks = 0;
    void* g_detour = nullptr;
    unsigned g_hookCalls = 0;
    const void* g_expectedPacket = nullptr;
    std::array<std::uint8_t, 8> g_expectedBytes{};
    void* g_manager = nullptr;
    std::uint32_t g_client = 0;
    const void* g_packet = nullptr;
    std::uint32_t g_length = 0;
    std::uint32_t g_flags = 0;
    DWORD g_lastError = 0;
    std::uint32_t g_beforeRegisters[8]{};
    std::uint32_t g_afterRegisters[8]{};
    DWORD g_beforeFlags = 0;
    DWORD g_afterFlags = 0;
    __declspec(align(16)) std::uint8_t g_beforeFx[512]{};
    __declspec(align(16)) std::uint8_t g_afterFx[512]{};

    void Check(bool condition, const char* text)
    {
        ++g_checks;
        if (!condition) throw std::runtime_error(text);
    }

    void Log(SdgoMod*, int, const char*, ...)
    {
    }

    int HookSite(
        SdgoMod*, const char* name, void* detour, void** original);

    int __fastcall CaptureSend(
        void* manager, void*, std::uint32_t client, const void* packet,
        std::uint32_t length, std::uint32_t flags)
    {
        g_manager = manager;
        g_client = client;
        g_packet = packet;
        g_length = length;
        g_flags = flags;
        g_lastError = GetLastError();
        if (packet == g_expectedPacket && packet)
            std::memcpy(g_expectedBytes.data(), packet, g_expectedBytes.size());
        return 0x2468ACE0;
    }

    extern "C" __declspec(naked) void OriginalTrampoline()
    {
        __asm { mov dword ptr [g_afterRegisters + 0], eax }
        __asm { mov dword ptr [g_afterRegisters + 4], ebx }
        __asm { mov dword ptr [g_afterRegisters + 8], ecx }
        __asm { mov dword ptr [g_afterRegisters + 12], edx }
        __asm { mov dword ptr [g_afterRegisters + 16], esi }
        __asm { mov dword ptr [g_afterRegisters + 20], edi }
        __asm { mov dword ptr [g_afterRegisters + 24], ebp }
        __asm { mov dword ptr [g_afterRegisters + 28], esp }
        __asm { pushfd }
        __asm { pop dword ptr [g_afterFlags] }
        __asm { fxsave [g_afterFx] }
        __asm { call CaptureSend }
        __asm { ret }
    }

    int HookSite(
        SdgoMod*, const char* name, void* detour, void** original)
    {
        Check(
            name &&
                std::strcmp(
                    name,
                    "agent_room_view_error_fix.send_client_call") == 0,
            "actual DLL owns only the verified 0x0880 SendToClient call");
        ++g_hookCalls;
        g_detour = detour;
        *original = reinterpret_cast<void*>(&OriginalTrampoline);
        return 1;
    }

    extern "C" __declspec(naked) int Invoke(
        void*, std::uint32_t, const void*, std::uint32_t, std::uint32_t)
    {
        __asm { mov eax, esp }
        __asm { push dword ptr [eax + 20] }
        __asm { push dword ptr [eax + 16] }
        __asm { push dword ptr [eax + 12] }
        __asm { push dword ptr [eax + 8] }
        __asm { mov ecx, dword ptr [eax + 4] }
        __asm { stc }
        __asm { mov dword ptr [g_beforeRegisters + 0], eax }
        __asm { mov dword ptr [g_beforeRegisters + 4], ebx }
        __asm { mov dword ptr [g_beforeRegisters + 8], ecx }
        __asm { mov dword ptr [g_beforeRegisters + 12], edx }
        __asm { mov dword ptr [g_beforeRegisters + 16], esi }
        __asm { mov dword ptr [g_beforeRegisters + 20], edi }
        __asm { mov dword ptr [g_beforeRegisters + 24], ebp }
        __asm { mov dword ptr [g_beforeRegisters + 28], esp }
        __asm { pushfd }
        __asm { pop dword ptr [g_beforeFlags] }
        __asm { fxsave [g_beforeFx] }
        __asm { jmp dword ptr [g_detour] }
    }

    void ResetCapture()
    {
        g_manager = nullptr;
        g_client = 0;
        g_packet = nullptr;
        g_length = 0;
        g_flags = 0;
        g_lastError = 0;
        g_expectedPacket = nullptr;
        g_expectedBytes.fill(0);
        std::memset(g_beforeRegisters, 0, sizeof(g_beforeRegisters));
        std::memset(g_afterRegisters, 0, sizeof(g_afterRegisters));
        std::memset(g_beforeFx, 0, sizeof(g_beforeFx));
        std::memset(g_afterFx, 0, sizeof(g_afterFx));
    }

    void VerifyMachineState()
    {
        Check(
            std::memcmp(
                g_beforeRegisters, g_afterRegisters,
                sizeof(g_beforeRegisters)) == 0,
            "HookSite preserves every live general register and stack pointer");
        Check(
            g_beforeFlags == g_afterFlags &&
                (g_afterFlags & 1u) != 0,
            "HookSite preserves the exact native flags including carry");
        Check(
            std::memcmp(g_beforeFx, g_afterFx, sizeof(g_beforeFx)) == 0,
            "HookSite preserves x87 and SIMD control and register state");
    }

    void InvokeAndVerify(
        void* manager, std::uint32_t client, const void* packet,
        std::uint32_t inputLength, std::uint32_t expectedLength,
        std::uint32_t flags)
    {
        ResetCapture();
        g_expectedPacket = packet;
        SetLastError(0x13572468);
        const auto result =
            Invoke(manager, client, packet, inputLength, flags);
        Check(
            result == static_cast<int>(0x2468ACE0),
            "trampoline replays the original SendToClient call exactly once");
        Check(
            g_manager == manager && g_client == client &&
                g_packet == packet && g_length == expectedLength &&
                g_flags == flags,
            "destination, packet, flags and corrected length reach the original call");
        Check(
            g_lastError == 0x13572468,
            "HookSite helper preserves the caller's last-error value");
        VerifyMachineState();
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    try {
        HMODULE dll = LoadLibraryW(argv[1]);
        const auto entry =
            dll ? reinterpret_cast<SdgoModLoadFn>(
                      GetProcAddress(dll, "SdgoModLoad"))
                : nullptr;
        Check(entry != nullptr, "production DLL exports SdgoModLoad");

        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.host_key = "AgentServer-548A86BC-000D7000";
        api.HookSite = &HookSite;
        api.Log = &Log;
        Check(
            entry(&api, nullptr) != 0 && g_hookCalls == 1 &&
                g_detour != nullptr,
            "production DLL registers one exact Agent HookSite");

        std::array<std::uint8_t, 8> error13D2{
            0x80, 0x08, 0x65, 0x00, 0x00, 0x00, 0xD2, 0x13};
        const auto originalError = error13D2;
        void* const manager = reinterpret_cast<void*>(0x10203040);
        InvokeAndVerify(
            manager, 0x55667788, error13D2.data(), 4, 8, 1);
        Check(
            error13D2 == originalError &&
                g_expectedBytes == originalError &&
                *reinterpret_cast<const std::uint16_t*>(
                    error13D2.data() + 6) == 0x13D2,
            "complete native error 0x13D2 is forwarded without packet mutation");

        std::array<std::uint8_t, 8> other{
            0x81, 0x08, 0x66, 0x00, 0x00, 0x00, 0xD2, 0x13};
        InvokeAndVerify(
            manager, 0x89ABCDEF, other.data(), 4, 4, 0xA5A5A5A5);
        Check(
            g_expectedBytes == other,
            "another opcode keeps its original four-byte forwarding length");

        InvokeAndVerify(
            manager, 0x01020304, error13D2.data(), 8, 8, 7);
        InvokeAndVerify(
            manager, 0x01020305, error13D2.data(), 6, 6, 9);
        InvokeAndVerify(
            manager, 0x01020306, nullptr, 4, 4, 11);
        Check(
            entry(&api, nullptr) != 0 && g_hookCalls == 2,
            "already-correct policy remains deterministic when loaded again");

        SdgoApi wrong = api;
        wrong.host_key = "BattleServer-548A891D-0046C000";
        Check(
            entry(&wrong, nullptr) == 0,
            "production DLL refuses a non-Agent native host");

        FreeLibrary(dll);
        std::printf(
            "%u production Agent HookSite ABI checks passed; no server or client opened\n",
            g_checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
