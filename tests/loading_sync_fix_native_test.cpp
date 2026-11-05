// Execute the production x86 Mod DLL against instruction bytes from the
// supported BattleServer.exe. This is a controlled native-code replay: it does
// not invoke the server entry point, initialize networking, or connect to SQL.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "native_title_image.h"

#if !defined(_M_IX86)
#error loading_sync_fix_native_test requires the x86 ABI
#endif

namespace
{
    constexpr std::size_t kOccupancySiteCount = 11;
    constexpr std::size_t kHookSiteCount = 12;
    constexpr std::size_t kRoomSize = 0x32880;
    constexpr std::size_t kPilotId = 0x95C;
    constexpr std::size_t kSlotStride = 0x48E;
    constexpr std::size_t kPhase1 = 0x964;
    constexpr std::size_t kPhase2 = 0x966;
    constexpr std::size_t kPhase3 = 0x966;
    constexpr std::size_t kLoadingPhase = 0x2EEB0;
    constexpr std::size_t kPhaseStart = 0x2EEB4;
    constexpr std::size_t kPhaseDeadline = 0x2EEB8;
    constexpr std::size_t kPhaseRetryDeadline = 0x2EECC;
    constexpr std::uint32_t kAiBit = 0x80000000u;
    constexpr std::uint32_t kZeroFlag = 0x40u;
    // Lower architectural flags through OF: arithmetic results plus the
    // control flags that the site must leave untouched.
    constexpr std::uint32_t kComparableFlags = 0x0FFFu;

    enum class RegisterAbi
    {
        eax_room_edx_offset,
        edx_room_ecx_offset,
        ecx_room_eax_offset,
    };

    struct SiteDefinition
    {
        const char* name;
        std::uint32_t rva;
        RegisterAbi abi;
        std::array<std::uint8_t, 10> expected;
    };

    constexpr std::array<std::uint8_t, 10> kEaxEdxD6 =
        {0x83,0xBC,0x10,0x5C,0x09,0x00,0x00,0x00,0x75,0x02};
    constexpr std::array<std::uint8_t, 10> kEaxEdxD9 =
        {0x83,0xBC,0x10,0x5C,0x09,0x00,0x00,0x00,0x75,0x02};
    constexpr std::array<std::uint8_t, 10> kEdxEcx =
        {0x83,0xBC,0x0A,0x5C,0x09,0x00,0x00,0x00,0x75,0x02};
    constexpr std::array<std::uint8_t, 10> kEcxEax =
        {0x83,0xBC,0x01,0x5C,0x09,0x00,0x00,0x00,0x75,0x02};

    constexpr SiteDefinition kDefinitions[kOccupancySiteCount] = {
        {"loading_sync.phase1_count", 0x00080F53,
         RegisterAbi::eax_room_edx_offset, kEaxEdxD6},
        {"loading_sync.phase1_remove", 0x000810B5,
         RegisterAbi::eax_room_edx_offset, kEaxEdxD6},
        {"loading_sync.phase1_retry", 0x00081283,
         RegisterAbi::edx_room_ecx_offset, kEdxEcx},
        {"loading_sync.phase2_count", 0x000813D7,
         RegisterAbi::eax_room_edx_offset, kEaxEdxD9},
        {"loading_sync.phase2_remove", 0x000814C0,
         RegisterAbi::edx_room_ecx_offset, kEdxEcx},
        {"loading_sync.phase3_count", 0x000816A7,
         RegisterAbi::eax_room_edx_offset, kEaxEdxD9},
        {"loading_sync.phase3_remove", 0x000817FA,
         RegisterAbi::ecx_room_eax_offset, kEcxEax},
        {"loading_sync.load_complete_count", 0x000BEA91,
         RegisterAbi::edx_room_ecx_offset, kEdxEcx},
        {"loading_sync.host_removed_candidate", 0x0007B1C2,
         RegisterAbi::edx_room_ecx_offset, kEdxEcx},
        {"loading_sync.host_observer_candidate", 0x0007B2F7,
         RegisterAbi::edx_room_ecx_offset, kEdxEcx},
        {"loading_sync.host_invalid_candidate", 0x0007B433,
         RegisterAbi::ecx_room_eax_offset, kEcxEax},
    };

    constexpr std::uint32_t kPhase2InitializeRva = 0x000BA936;
    constexpr std::uint32_t kPhase2InitializeContinueRva = 0x000BA943;
    constexpr std::uint32_t kPhase2AlreadyAdvancedRva = 0x000BA9B2;
    constexpr std::uint32_t kPhase2BroadcastCompleteRva = 0x000BA9F4;
    constexpr std::size_t kPhase2InitializeInstructionBytes = 13;
    constexpr std::array<std::uint8_t, 16> kPhase2InitializeExpected = {
        0x8B,0x45,0xEC,0xC7,0x80,0xB0,0xEE,0x02,
        0x00,0x02,0x00,0x00,0x00,0x6A,0x00,0xE8};
    constexpr std::array<std::uint8_t, 16> kPhase2ContinueExpected = {
        0x6A,0x00,0xE8,0x10,0x8D,0x27,0x00,0x83,
        0xC4,0x04,0x8B,0x4D,0xEC,0x89,0x81,0xB4};
    constexpr std::array<std::uint8_t, 16> kPhase2AlreadyExpected = {
        0x8B,0x55,0xE8,0x89,0x95,0x7C,0xEB,0xFF,
        0xFF,0xB8,0xC4,0x09,0x00,0x00,0x66,0x89};

    struct SiteRuntime
    {
        std::uint8_t* code = nullptr;
        void* detour = nullptr;
    };

    struct Phase2InitializeRuntime
    {
        std::uint8_t* code = nullptr;
        std::uint8_t* alreadyAdvanced = nullptr;
        void* detour = nullptr;
    };

    struct Registers
    {
        std::uint32_t eax = 0;
        std::uint32_t ebx = 0;
        std::uint32_t ecx = 0;
        std::uint32_t edx = 0;
        std::uint32_t esi = 0;
        std::uint32_t edi = 0;
        std::uint32_t ebp = 0;
        std::uint32_t esp = 0;
        std::uint32_t flags = 0;
    };

    std::array<SiteRuntime, kOccupancySiteCount> g_sites = {};
    Phase2InitializeRuntime g_phase2Initialize = {};
    std::array<std::uint8_t, kRoomSize> g_room = {};
    std::vector<std::string> g_logs;
    unsigned g_checks = 0;
    unsigned g_hookCalls = 0;
    std::string g_missingSymbol;

    void* g_entry = nullptr;
    std::uint8_t* g_roomPointer = nullptr;
    std::uint32_t g_slotOffset = 0;
    std::uint32_t g_expectedEsp = 0;
    Registers g_seen = {};

    std::uint32_t g_replayClockValue = 0;
    unsigned g_replayClockCalls = 0;
    unsigned g_replayBroadcastCalls = 0;
    void* g_replayBroadcastRoom = nullptr;
    std::uint32_t g_replayBroadcastSize = 0;
    std::uint16_t g_replayBroadcastOpcode = 0;
    std::uint32_t g_replayBroadcastPilot = 0;
    std::uint32_t g_replayBroadcastToken = 0;
    unsigned g_phase2ReplayCompleted = 0;
    std::uint32_t g_phase2ExpectedEsp = 0;
    void* g_phase2ExpectedFrame = nullptr;
    Registers g_phase2TargetSeen = {};
    volatile LONG g_watchPhase2Target = 0;
    volatile LONG g_phase2TargetCaptured = 0;

    void Check(bool condition, const char* message)
    {
        ++g_checks;
        if (!condition) throw std::runtime_error(message);
    }

    template <typename T>
    void Put(std::size_t offset, T value)
    {
        Check(offset <= g_room.size() && sizeof(value) <= g_room.size() - offset,
              "room fixture write stays in bounds");
        std::memcpy(g_room.data() + offset, &value, sizeof(value));
    }

    template <typename T>
    T Get(std::size_t offset)
    {
        Check(offset <= g_room.size() && sizeof(T) <= g_room.size() - offset,
              "room fixture read stays in bounds");
        T value = {};
        std::memcpy(&value, g_room.data() + offset, sizeof(value));
        return value;
    }

    void SetPilot(unsigned slot, std::uint32_t pilot)
    {
        Check(slot < 12, "room slot index is valid");
        Put<std::uint32_t>(kPilotId + slot * kSlotStride, pilot);
    }

    void SetStage(unsigned slot, std::size_t offset, std::uint8_t stage)
    {
        Check(slot < 12, "loading stage slot index is valid");
        Put<std::uint8_t>(offset + slot * kSlotStride, stage);
    }

    std::uint32_t __cdecl ReplayClock(std::uint32_t)
    {
        ++g_replayClockCalls;
        return g_replayClockValue;
    }

    __declspec(naked) void ReplayBroadcast()
    {
        __asm {
            mov dword ptr [g_replayBroadcastRoom], ecx
            mov eax, dword ptr [esp + 4]
            movzx edx, word ptr [eax]
            mov word ptr [g_replayBroadcastOpcode], dx
            mov edx, dword ptr [eax + 2]
            mov dword ptr [g_replayBroadcastPilot], edx
            mov edx, dword ptr [eax + 6]
            mov dword ptr [g_replayBroadcastToken], edx
            mov eax, dword ptr [esp + 8]
            mov dword ptr [g_replayBroadcastSize], eax
            inc dword ptr [g_replayBroadcastCalls]
            xor eax, eax
            ret 0Ch
        }
    }

    __declspec(naked) void CapturePhase2ReplayCompletion()
    {
        __asm {
            mov dword ptr [g_phase2ReplayCompleted], 1
            ret
        }
    }

    __declspec(naked) void InvokePhase2Replay()
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov ebp, esp
            sub esp, 01500h
            mov eax, dword ptr [g_roomPointer]
            mov dword ptr [ebp - 014h], eax
            mov dword ptr [ebp - 018h], 055667788h
            mov dword ptr [ebp - 01440h], 011223344h
            mov dword ptr [ebp - 010h], 07Bh
            mov dword ptr [g_phase2ExpectedFrame], ebp
            mov eax, 010203040h
            mov ebx, 020304050h
            mov ecx, 030405060h
            mov edx, 040506070h
            mov esi, 050607080h
            mov edi, 060708090h
            push 0202h
            popfd
            mov dword ptr [g_phase2ExpectedEsp], esp
            call dword ptr [g_entry]
            mov esp, ebp
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    LONG CALLBACK CapturePhase2Target(EXCEPTION_POINTERS* exception)
    {
        if (InterlockedCompareExchange(
                &g_watchPhase2Target, 0, 0) == 0 ||
            !exception || !exception->ExceptionRecord ||
            !exception->ContextRecord ||
            exception->ExceptionRecord->ExceptionCode !=
                STATUS_GUARD_PAGE_VIOLATION ||
            exception->ExceptionRecord->ExceptionAddress !=
                g_phase2Initialize.alreadyAdvanced) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const CONTEXT& context = *exception->ContextRecord;
        g_phase2TargetSeen.eax = context.Eax;
        g_phase2TargetSeen.ebx = context.Ebx;
        g_phase2TargetSeen.ecx = context.Ecx;
        g_phase2TargetSeen.edx = context.Edx;
        g_phase2TargetSeen.esi = context.Esi;
        g_phase2TargetSeen.edi = context.Edi;
        g_phase2TargetSeen.ebp = context.Ebp;
        g_phase2TargetSeen.esp = context.Esp;
        g_phase2TargetSeen.flags = context.EFlags;
        InterlockedExchange(&g_phase2TargetCaptured, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ResetPhase2Replay()
    {
        g_replayClockCalls = 0;
        g_replayBroadcastCalls = 0;
        g_replayBroadcastRoom = nullptr;
        g_replayBroadcastSize = 0;
        g_replayBroadcastOpcode = 0;
        g_replayBroadcastPilot = 0;
        g_replayBroadcastToken = 0;
        g_phase2ReplayCompleted = 0;
        g_phase2TargetSeen = {};
        InterlockedExchange(&g_phase2TargetCaptured, 0);
    }

    void ExecutePhase2Replay(void* entry, bool captureAdvancedTarget)
    {
        Check(entry != nullptr, "phase-2 initialization replay entry is available");
        ResetPhase2Replay();
        g_entry = entry;
        g_roomPointer = g_room.data();
        void* handler = nullptr;
        DWORD oldProtection = 0;
        if (captureAdvancedTarget) {
            handler = AddVectoredExceptionHandler(1, &CapturePhase2Target);
            Check(handler != nullptr,
                  "phase-2 advanced-target observer is installed");
            Check(VirtualProtect(
                      g_phase2Initialize.code, 4096,
                      PAGE_EXECUTE_READWRITE | PAGE_GUARD,
                      &oldProtection) != FALSE,
                  "phase-2 advanced target is guarded for context capture");
            InterlockedExchange(&g_watchPhase2Target, 1);
        }
        InvokePhase2Replay();
        if (captureAdvancedTarget) {
            InterlockedExchange(&g_watchPhase2Target, 0);
            DWORD ignoredProtection = 0;
            Check(VirtualProtect(
                      g_phase2Initialize.code, 4096,
                      oldProtection, &ignoredProtection) != FALSE,
                  "phase-2 advanced target protection is restored");
            Check(RemoveVectoredExceptionHandler(handler) != 0,
                  "phase-2 advanced-target observer is removed");
        }
        Check(g_phase2ReplayCompleted == 1,
              "phase-2 replay reaches the native post-broadcast path");
    }

    __declspec(naked) void CaptureContinuation()
    {
        __asm {
            mov dword ptr [g_seen.eax], eax
            mov dword ptr [g_seen.ebx], ebx
            mov dword ptr [g_seen.ecx], ecx
            mov dword ptr [g_seen.edx], edx
            mov dword ptr [g_seen.esi], esi
            mov dword ptr [g_seen.edi], edi
            mov dword ptr [g_seen.ebp], ebp
            mov dword ptr [g_seen.esp], esp
            pushfd
            pop dword ptr [g_seen.flags]
            ret
        }
    }

    __declspec(naked) void InvokeEaxEdx()
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov eax, dword ptr [g_roomPointer]
            mov edx, dword ptr [g_slotOffset]
            mov ecx, 033445566h
            mov ebx, 044556677h
            mov esi, 055667788h
            mov edi, 066778899h
            mov ebp, 0778899AAh
            push 0202h
            popfd
            mov dword ptr [g_expectedEsp], esp
            call dword ptr [g_entry]
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    __declspec(naked) void InvokeEdxEcx()
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov eax, 011223344h
            mov edx, dword ptr [g_roomPointer]
            mov ecx, dword ptr [g_slotOffset]
            mov ebx, 044556677h
            mov esi, 055667788h
            mov edi, 066778899h
            mov ebp, 0778899AAh
            push 0202h
            popfd
            mov dword ptr [g_expectedEsp], esp
            call dword ptr [g_entry]
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    __declspec(naked) void InvokeEcxEax()
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov eax, dword ptr [g_slotOffset]
            mov edx, 022334455h
            mov ecx, dword ptr [g_roomPointer]
            mov ebx, 044556677h
            mov esi, 055667788h
            mov edi, 066778899h
            mov ebp, 0778899AAh
            push 0202h
            popfd
            mov dword ptr [g_expectedEsp], esp
            call dword ptr [g_entry]
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    Registers ExpectedRegisters(RegisterAbi abi)
    {
        Registers expected = {};
        expected.eax = 0x11223344u;
        expected.ebx = 0x44556677u;
        expected.ecx = 0x33445566u;
        expected.edx = 0x22334455u;
        expected.esi = 0x55667788u;
        expected.edi = 0x66778899u;
        expected.ebp = 0x778899AAu;
        expected.esp = g_expectedEsp - sizeof(void*);
        const std::uint32_t room =
            static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(g_roomPointer));
        if (abi == RegisterAbi::eax_room_edx_offset) {
            expected.eax = room;
            expected.edx = g_slotOffset;
        } else if (abi == RegisterAbi::edx_room_ecx_offset) {
            expected.edx = room;
            expected.ecx = g_slotOffset;
        } else {
            expected.ecx = room;
            expected.eax = g_slotOffset;
        }
        return expected;
    }

    Registers Execute(std::size_t site, void* entry, unsigned slot,
                      std::uint32_t pilot)
    {
        Check(site < g_sites.size() && entry != nullptr,
              "native site entry is available");
        SetPilot(slot, pilot);
        g_seen = {};
        g_entry = entry;
        g_roomPointer = g_room.data();
        g_slotOffset = static_cast<std::uint32_t>(slot * kSlotStride);
        const RegisterAbi abi = kDefinitions[site].abi;
        if (abi == RegisterAbi::eax_room_edx_offset) InvokeEaxEdx();
        else if (abi == RegisterAbi::edx_room_ecx_offset) InvokeEdxEcx();
        else InvokeEcxEax();
        const Registers expected = ExpectedRegisters(abi);
        Check(g_seen.eax == expected.eax &&
                  g_seen.ebx == expected.ebx &&
                  g_seen.ecx == expected.ecx &&
                  g_seen.edx == expected.edx &&
                  g_seen.esi == expected.esi &&
                  g_seen.edi == expected.edi &&
                  g_seen.ebp == expected.ebp,
              "site replay preserves every general-purpose register");
        Check(g_seen.esp == expected.esp,
              "site replay reaches continuation with the original stack");
        return g_seen;
    }

    Registers ExecuteStock(std::size_t site, unsigned slot,
                           std::uint32_t pilot)
    {
        return Execute(site, g_sites[site].code, slot, pilot);
    }

    Registers ExecuteFixed(std::size_t site, unsigned slot,
                           std::uint32_t pilot)
    {
        return Execute(site, g_sites[site].detour, slot, pilot);
    }

    bool Occupied(std::size_t site, bool fixed, unsigned slot)
    {
        const std::uint32_t pilot =
            Get<std::uint32_t>(kPilotId + slot * kSlotStride);
        const Registers result =
            fixed ? ExecuteFixed(site, slot, pilot)
                  : ExecuteStock(site, slot, pilot);
        return (result.flags & kZeroFlag) == 0;
    }

    int FindWaitingCandidate(std::size_t site, bool fixed,
                             std::size_t stageOffset,
                             std::uint8_t completedStage)
    {
        for (unsigned slot = 0; slot < 12; ++slot) {
            if (!Occupied(site, fixed, slot)) continue;
            if (Get<std::uint8_t>(stageOffset + slot * kSlotStride) !=
                completedStage) {
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

    int FindFirstOccupied(std::size_t site, bool fixed)
    {
        for (unsigned slot = 0; slot < 12; ++slot) {
            if (Occupied(site, fixed, slot)) return static_cast<int>(slot);
        }
        return -1;
    }

    unsigned CountCompleted(std::size_t site, bool fixed,
                            std::size_t stageOffset,
                            std::uint8_t completedStage)
    {
        unsigned completed = 0;
        for (unsigned slot = 0; slot < 12; ++slot) {
            if (Occupied(site, fixed, slot) &&
                Get<std::uint8_t>(stageOffset + slot * kSlotStride) ==
                    completedStage) {
                ++completed;
            }
        }
        return completed;
    }

    struct LoadingReplay
    {
        unsigned humanCount = 0;
        unsigned removeCalls = 0;
        std::uint32_t removedPilot = 0;
        unsigned send06F3 = 0;
    };

    void NativeRemoveReplay(LoadingReplay& replay, int slot)
    {
        Check(slot >= 0 && slot < 12, "native removal slot is valid");
        replay.removedPilot =
            Get<std::uint32_t>(kPilotId +
                               static_cast<unsigned>(slot) * kSlotStride);
        ++replay.removeCalls;
        // The shipped RoomRemove path decrements the room's human count
        // unconditionally. The guards must therefore keep Mod AI away from it.
        Check(replay.humanCount > 0, "native human count can be decremented");
        --replay.humanCount;
        SetPilot(static_cast<unsigned>(slot), 0);
    }

    void Send06F3IfComplete(LoadingReplay& replay, std::size_t site,
                            bool fixed, std::size_t stageOffset,
                            std::uint8_t completedStage)
    {
        if (CountCompleted(site, fixed, stageOffset, completedStage) ==
            replay.humanCount) {
            ++replay.send06F3;
        }
    }

    std::size_t FindSite(const char* name)
    {
        for (std::size_t index = 0; index < kOccupancySiteCount; ++index) {
            if (std::strcmp(name, kDefinitions[index].name) == 0) return index;
        }
        return kOccupancySiteCount;
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        if (!name) return nullptr;
        if (g_missingSymbol == name) return nullptr;
        if (std::strcmp(name, "loading_sync.phase2_initialize") == 0) {
            return g_phase2Initialize.code;
        }
        if (std::strcmp(
                name, "loading_sync.phase2_initialize_continue") == 0) {
            return g_phase2Initialize.code +
                   kPhase2InitializeInstructionBytes;
        }
        if (std::strcmp(
                name,
                "loading_sync.phase2_initialize_already_advanced") == 0) {
            return g_phase2Initialize.alreadyAdvanced;
        }
        for (std::size_t index = 0; index < kOccupancySiteCount; ++index) {
            const std::string continuation =
                std::string(kDefinitions[index].name) + "_continue";
            if (std::strcmp(name, kDefinitions[index].name) == 0) {
                return g_sites[index].code;
            }
            if (continuation == name) return g_sites[index].code + 8;
        }
        return nullptr;
    }

    int HookSite(SdgoMod*, const char* name, void* detour, void** original)
    {
        Check(name && detour && original,
              "production Mod requests a declared native site");
        if (std::strcmp(name, "loading_sync.phase2_initialize") == 0) {
            Check(g_phase2Initialize.detour == nullptr,
                  "phase-2 initialization site is hooked once");
            g_phase2Initialize.detour = detour;
            *original = g_phase2Initialize.code;
            ++g_hookCalls;
            return 1;
        }
        const std::size_t index = FindSite(name);
        Check(index < kOccupancySiteCount,
              "production Mod requests a declared occupancy site");
        Check(g_sites[index].detour == nullptr,
              "each native site is hooked once");
        g_sites[index].detour = detour;
        *original = g_sites[index].code;
        ++g_hookCalls;
        return 1;
    }

    void Log(SdgoMod*, int, const char* format, ...)
    {
        char text[1024] = {};
        va_list arguments;
        va_start(arguments, format);
        vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
        va_end(arguments);
        g_logs.emplace_back(text);
    }

    SdgoApi Api()
    {
        SdgoApi api = {};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.host_key = "BattleServer-548A891D-0046C000";
        api.Symbol = &Symbol;
        api.HookSite = &HookSite;
        api.Log = &Log;
        return api;
    }

    void ResetActivation()
    {
        g_hookCalls = 0;
        g_missingSymbol.clear();
        g_logs.clear();
        g_phase2Initialize.detour = nullptr;
        for (auto& site : g_sites) site.detour = nullptr;
    }

    void BuildSiteReplays(const room_title_native::KnownBattleServerImage& image)
    {
        for (std::size_t index = 0; index < kOccupancySiteCount; ++index) {
            const SiteDefinition& definition = kDefinitions[index];
            const auto* native =
                image.Function<const std::uint8_t*>(definition.rva);
            Check(std::memcmp(native, definition.expected.data(),
                              definition.expected.size()) == 0,
                  "shipped BattleServer CMP and following JNE bytes match");
            auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
                nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                PAGE_EXECUTE_READWRITE));
            Check(code != nullptr, "native instruction replay page allocated");
            std::memcpy(code, native, 8);
            code[8] = 0xE9;
            const std::intptr_t delta =
                reinterpret_cast<std::intptr_t>(&CaptureContinuation) -
                reinterpret_cast<std::intptr_t>(code + 13);
            Check(delta >= std::numeric_limits<std::int32_t>::min() &&
                      delta <= std::numeric_limits<std::int32_t>::max(),
                  "x86 continuation jump is representable");
            const std::int32_t relative = static_cast<std::int32_t>(delta);
            std::memcpy(code + 9, &relative, sizeof(relative));
            code[13] = 0xC3;
            Check(FlushInstructionCache(GetCurrentProcess(), code, 14) != FALSE,
                  "native replay instruction cache flushed");
            g_sites[index].code = code;
        }

        const auto* phase2Native =
            image.Function<const std::uint8_t*>(kPhase2InitializeRva);
        const auto* phase2Continue =
            image.Function<const std::uint8_t*>(
                kPhase2InitializeContinueRva);
        const auto* phase2Already =
            image.Function<const std::uint8_t*>(kPhase2AlreadyAdvancedRva);
        Check(std::memcmp(
                  phase2Native, kPhase2InitializeExpected.data(),
                  kPhase2InitializeExpected.size()) == 0,
              "shipped phase-2 initialization signature matches");
        Check(std::memcmp(
                  phase2Continue, kPhase2ContinueExpected.data(),
                  kPhase2ContinueExpected.size()) == 0,
              "shipped phase-2 continuation signature matches");
        Check(std::memcmp(
                  phase2Already, kPhase2AlreadyExpected.data(),
                  kPhase2AlreadyExpected.size()) == 0,
              "shipped already-advanced packet signature matches");

        const std::size_t nativeBytes =
            kPhase2BroadcastCompleteRva - kPhase2InitializeRva;
        auto* phase2Code = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr, 8192, MEM_RESERVE | MEM_COMMIT,
            PAGE_EXECUTE_READWRITE));
        Check(phase2Code != nullptr,
              "phase-2 native replay pages allocated");
        std::memcpy(phase2Code, phase2Native, nativeBytes);
        phase2Code[nativeBytes] = 0xE9;
        const std::intptr_t completionDelta =
            reinterpret_cast<std::intptr_t>(&CapturePhase2ReplayCompletion) -
            reinterpret_cast<std::intptr_t>(
                phase2Code + nativeBytes + 5);
        Check(completionDelta >=
                  std::numeric_limits<std::int32_t>::min() &&
                  completionDelta <=
                      std::numeric_limits<std::int32_t>::max(),
              "phase-2 completion jump is representable");
        const std::int32_t completionRelative =
            static_cast<std::int32_t>(completionDelta);
        std::memcpy(
            phase2Code + nativeBytes + 1, &completionRelative,
            sizeof(completionRelative));
        g_phase2Initialize.code = phase2Code;
        g_phase2Initialize.alreadyAdvanced =
            phase2Code +
            (kPhase2AlreadyAdvancedRva - kPhase2InitializeRva);
        Check(FlushInstructionCache(
                  GetCurrentProcess(), phase2Code, nativeBytes + 5) != FALSE,
              "phase-2 native replay instruction cache flushed");
    }

    void ReleaseSiteReplays()
    {
        for (auto& site : g_sites) {
            if (site.code) VirtualFree(site.code, 0, MEM_RELEASE);
            site = {};
        }
        if (g_phase2Initialize.code) {
            VirtualFree(g_phase2Initialize.code, 0, MEM_RELEASE);
        }
        g_phase2Initialize = {};
    }

    void PatchRelativeCall(std::uint8_t* instruction, void* target)
    {
        Check(instruction && instruction[0] == 0xE8,
              "native replay external call opcode is exact");
        const std::intptr_t delta =
            reinterpret_cast<std::intptr_t>(target) -
            reinterpret_cast<std::intptr_t>(instruction + 5);
        Check(delta >= std::numeric_limits<std::int32_t>::min() &&
                  delta <= std::numeric_limits<std::int32_t>::max(),
              "native replay external call is representable");
        const std::int32_t relative = static_cast<std::int32_t>(delta);
        std::memcpy(instruction + 1, &relative, sizeof(relative));
    }

    void PatchPhase2ReplayExternalCalls()
    {
        PatchRelativeCall(
            g_phase2Initialize.code + (0x000BA945 - kPhase2InitializeRva),
            reinterpret_cast<void*>(&ReplayClock));
        PatchRelativeCall(
            g_phase2Initialize.code + (0x000BA9EF - kPhase2InitializeRva),
            reinterpret_cast<void*>(&ReplayBroadcast));
        Check(FlushInstructionCache(
                  GetCurrentProcess(), g_phase2Initialize.code,
                  kPhase2BroadcastCompleteRva -
                      kPhase2InitializeRva + 5) != FALSE,
              "patched phase-2 replay instruction cache flushed");
    }

    SdgoModLoadFn LoadEntry(const wchar_t* dll, HMODULE& module)
    {
        module = LoadLibraryW(dll);
        Check(module != nullptr, "actual loading_sync_fix DLL loads");
        auto entry = reinterpret_cast<SdgoModLoadFn>(
            GetProcAddress(module, "SdgoModLoad"));
        Check(entry != nullptr, "actual loading_sync_fix DLL exports SdgoModLoad");
        return entry;
    }

    void TestDisabledBaseline()
    {
        ResetActivation();
        for (std::size_t index = 0; index < kOccupancySiteCount; ++index) {
            const Registers empty = ExecuteStock(index, 3, 0);
            const Registers human = ExecuteStock(index, 3, 10007);
            const Registers ai = ExecuteStock(index, 3, kAiBit | 41u);
            Check((empty.flags & kZeroFlag) != 0 &&
                      (human.flags & kZeroFlag) == 0 &&
                      (ai.flags & kZeroFlag) == 0,
                  "disabled stock CMP treats empty, human and occupied AI exactly as shipped");
        }
        Check(g_hookCalls == 0,
              "disabled activation leaves every shipped native site unhooked");
    }

    void TestRejectedSignatures(const wchar_t* dll)
    {
        {
            HMODULE module = nullptr;
            SdgoModLoadFn entry = LoadEntry(dll, module);
            ResetActivation();
            g_missingSymbol = kDefinitions[4].name;
            SdgoApi api = Api();
            Check(!entry(&api, reinterpret_cast<SdgoMod*>(1)) &&
                      g_hookCalls == 0,
                  "missing core-verified signature refuses before any hook");
            FreeLibrary(module);
        }
        {
            HMODULE module = nullptr;
            SdgoModLoadFn entry = LoadEntry(dll, module);
            ResetActivation();
            const std::uint8_t saved = g_sites[6].code[0];
            g_sites[6].code[0] ^= 1;
            SdgoApi api = Api();
            Check(!entry(&api, reinterpret_cast<SdgoMod*>(1)) &&
                      g_hookCalls == 0,
                  "mismatched native instruction refuses before any hook");
            g_sites[6].code[0] = saved;
            FreeLibrary(module);
        }
        const std::array<const char*, 3> phase2Symbols = {{
            "loading_sync.phase2_initialize",
            "loading_sync.phase2_initialize_continue",
            "loading_sync.phase2_initialize_already_advanced",
        }};
        for (const char* symbol : phase2Symbols) {
            HMODULE module = nullptr;
            SdgoModLoadFn entry = LoadEntry(dll, module);
            ResetActivation();
            g_missingSymbol = symbol;
            SdgoApi api = Api();
            Check(!entry(&api, reinterpret_cast<SdgoMod*>(1)) &&
                      g_hookCalls == 0,
                  "missing phase-2 signature refuses before any hook");
            FreeLibrary(module);
        }

        const std::array<std::uint8_t*, 3> phase2Addresses = {{
            g_phase2Initialize.code,
            g_phase2Initialize.code +
                kPhase2InitializeInstructionBytes,
            g_phase2Initialize.alreadyAdvanced,
        }};
        for (std::uint8_t* address : phase2Addresses) {
            HMODULE module = nullptr;
            SdgoModLoadFn entry = LoadEntry(dll, module);
            ResetActivation();
            const std::uint8_t saved = address[0];
            address[0] ^= 1;
            SdgoApi api = Api();
            Check(!entry(&api, reinterpret_cast<SdgoMod*>(1)) &&
                      g_hookCalls == 0,
                  "mismatched phase-2 signature refuses before any hook");
            address[0] = saved;
            FreeLibrary(module);
        }
    }

    HMODULE Activate(const wchar_t* dll)
    {
        HMODULE module = nullptr;
        SdgoModLoadFn entry = LoadEntry(dll, module);
        ResetActivation();
        SdgoApi api = Api();
        Check(entry(&api, reinterpret_cast<SdgoMod*>(1)) != 0,
              "actual production Mod activates on the supported signatures");
        Check(g_hookCalls == kHookSiteCount,
              "actual production Mod installs all twelve declared site hooks");
        Check(g_logs.size() == 1 &&
                  g_logs[0].find("human loading behavior remains native") !=
                      std::string::npos,
              "activation emits the bounded loading-sync receipt");
        return module;
    }

    void TestSiteAbi()
    {
        for (std::size_t index = 0; index < kOccupancySiteCount; ++index) {
            Check(g_sites[index].detour != nullptr,
                  "production detour was captured for native execution");
            const Registers stockEmpty = ExecuteStock(index, 5, 0);
            const Registers fixedEmpty = ExecuteFixed(index, 5, 0);
            const Registers stockHuman = ExecuteStock(index, 5, 15052);
            const Registers fixedHuman = ExecuteFixed(index, 5, 15052);
            const Registers fixedHighHuman =
                ExecuteFixed(index, 5, 0x7FFFFFFFu);
            const Registers stockHighHuman =
                ExecuteStock(index, 5, 0x7FFFFFFFu);
            const Registers fixedAi =
                ExecuteFixed(index, 5, kAiBit | 15052u);

            Check((fixedEmpty.flags & kComparableFlags) ==
                      (stockEmpty.flags & kComparableFlags),
                  "empty slot preserves the shipped CMP and control flags");
            Check((fixedHuman.flags & kComparableFlags) ==
                      (stockHuman.flags & kComparableFlags) &&
                      (fixedHighHuman.flags & kComparableFlags) ==
                          (stockHighHuman.flags & kComparableFlags),
                  "ordinary human IDs preserve the full shipped site flags");
            Check((fixedAi.flags & kComparableFlags) ==
                      (stockEmpty.flags & kComparableFlags),
                  "high-bit Mod AI produces the exact empty-slot flags");
        }
    }

    void CheckPhase2Broadcast()
    {
        Check(g_replayBroadcastCalls == 1 &&
                  g_replayBroadcastRoom == g_room.data() &&
                  g_replayBroadcastSize == 10 &&
                  g_replayBroadcastOpcode == 0x09C4 &&
                  g_replayBroadcastPilot == 0x11223344 &&
                  g_replayBroadcastToken == 0x55667788,
              "native late-ACK 09C4 packet construction and broadcast remain intact");
    }

    void SeedPhase2Room(std::uint32_t phase, std::uint8_t fill)
    {
        g_room.fill(fill);
        Put<std::uint32_t>(kLoadingPhase, phase);
        Put<std::uint32_t>(kPhaseStart, 0x11110000u + phase);
        Put<std::uint32_t>(kPhaseDeadline, 0x22220000u + phase);
        Put<std::uint32_t>(kPhaseRetryDeadline, 0x33330000u + phase);
        for (unsigned slot = 0; slot < 12; ++slot) {
            SetStage(
                slot, kPhase2,
                static_cast<std::uint8_t>(0x20u + slot));
        }
    }

    void TestPhase2InitializationGuard()
    {
        SeedPhase2Room(2, 0x5A);
        g_replayClockValue = 0x10203040u;
        ExecutePhase2Replay(g_phase2Initialize.code, false);
        Check(g_replayClockCalls == 1 &&
                  Get<std::uint32_t>(kLoadingPhase) == 2 &&
                  Get<std::uint32_t>(kPhaseStart) == g_replayClockValue &&
                  Get<std::uint32_t>(kPhaseDeadline) ==
                      g_replayClockValue + 180 &&
                  Get<std::uint32_t>(kPhaseRetryDeadline) ==
                      g_replayClockValue + 180,
              "stock late ACK re-enters phase 2 and resets its timers");
        for (unsigned slot = 0; slot < 12; ++slot) {
            Check(Get<std::uint8_t>(
                      kPhase2 + slot * kSlotStride) == 0,
                  "stock late ACK clears an existing loading stage");
        }
        CheckPhase2Broadcast();

        SeedPhase2Room(1, 0x6B);
        g_replayClockValue = 0x21314151u;
        ExecutePhase2Replay(g_phase2Initialize.detour, false);
        Check(g_replayClockCalls == 1 &&
                  Get<std::uint32_t>(kLoadingPhase) == 2 &&
                  Get<std::uint32_t>(kPhaseStart) == g_replayClockValue &&
                  Get<std::uint32_t>(kPhaseDeadline) ==
                      g_replayClockValue + 180 &&
                  Get<std::uint32_t>(kPhaseRetryDeadline) ==
                      g_replayClockValue + 180,
              "phase-1 ACK retains the exact native phase-2 initialization");
        for (unsigned slot = 0; slot < 12; ++slot) {
            Check(Get<std::uint8_t>(
                      kPhase2 + slot * kSlotStride) == 0,
                  "phase-1 ACK retains native clearing for every loading slot");
        }
        CheckPhase2Broadcast();

        for (std::uint32_t phase = 2; phase <= 5; ++phase) {
            SeedPhase2Room(
                phase, static_cast<std::uint8_t>(0x70u + phase));
            const auto before = g_room;
            g_replayClockValue = 0xABC00000u + phase;
            ExecutePhase2Replay(
                g_phase2Initialize.detour, phase == 2);
            Check(g_replayClockCalls == 0 &&
                      std::memcmp(
                          g_room.data(), before.data(),
                          g_room.size()) == 0,
                  "late initial ACK preserves the current phase, timers and every slot");
            CheckPhase2Broadcast();
            if (phase == 2) {
                Check(InterlockedCompareExchange(
                          &g_phase2TargetCaptured, 0, 0) == 1,
                      "late ACK reaches the native already-advanced packet path");
                Check(g_phase2TargetSeen.eax == 0x10203040u &&
                          g_phase2TargetSeen.ebx == 0x20304050u &&
                          g_phase2TargetSeen.ecx == 0x30405060u &&
                          g_phase2TargetSeen.edx == 0x40506070u &&
                          g_phase2TargetSeen.esi == 0x50607080u &&
                          g_phase2TargetSeen.edi == 0x60708090u &&
                          g_phase2TargetSeen.ebp ==
                              reinterpret_cast<std::uintptr_t>(
                                  g_phase2ExpectedFrame) &&
                          g_phase2TargetSeen.esp ==
                              g_phase2ExpectedEsp - sizeof(void*) &&
                          (g_phase2TargetSeen.flags &
                           kComparableFlags) ==
                              (0x202u & kComparableFlags),
                      "late ACK guard preserves every register, stack and control flag");
            }
        }
    }

    void PrepareMixedRoom(std::uint8_t completedStage,
                          std::size_t stageOffset)
    {
        g_room.fill(0);
        SetPilot(0, kAiBit | 101u);
        SetPilot(1, 1001u);
        SetPilot(5, 1005u);
        SetStage(0, stageOffset, 0);
        SetStage(1, stageOffset, completedStage);
        SetStage(5, stageOffset, completedStage);
    }

    void TestTimeoutRemoval()
    {
        const std::array<std::pair<std::size_t, std::uint8_t>, 2> phases = {{
            {std::size_t{4}, std::uint8_t{2}},
            {std::size_t{6}, std::uint8_t{3}},
        }};
        for (const auto& phase : phases) {
            PrepareMixedRoom(phase.second, kPhase2);
            LoadingReplay disabled = {2, 0, 0, 0};
            const int stockCandidate =
                FindWaitingCandidate(phase.first, false, kPhase2, phase.second);
            Check(stockCandidate == 0,
                  "disabled shipped timeout scan selects occupied AI first");
            NativeRemoveReplay(disabled, stockCandidate);
            Check(disabled.removedPilot == (kAiBit | 101u) &&
                      disabled.humanCount == 1,
                  "disabled baseline reproduces AI removal and human-count decrement");

            PrepareMixedRoom(phase.second, kPhase2);
            LoadingReplay enabled = {2, 0, 0, 0};
            const int fixedCandidate =
                FindWaitingCandidate(phase.first, true, kPhase2, phase.second);
            Check(fixedCandidate == -1,
                  "enabled timeout scan does not hand ready-room AI to native removal");
            Check(enabled.removeCalls == 0 && enabled.humanCount == 2,
                  "AI-only timeout mismatch leaves the human count stable");

            SetStage(5, kPhase2, 0);
            const int slowHuman =
                FindWaitingCandidate(phase.first, true, kPhase2, phase.second);
            Check(slowHuman == 5,
                  "a genuinely slow human still follows the native timeout path");
            NativeRemoveReplay(enabled, slowHuman);
            Check(enabled.removedPilot == 1005u &&
                      enabled.removeCalls == 1 &&
                      enabled.humanCount == 1,
                  "native slow-human removal and human-count decrement are retained");
        }
    }

    void TestHostCandidates()
    {
        g_room.fill(0);
        SetPilot(0, kAiBit | 77u);
        SetPilot(4, 1004u);
        SetPilot(5, 1005u);
        for (std::size_t site : {std::size_t{8}, std::size_t{9},
                                 std::size_t{10}}) {
            Check(FindFirstOccupied(site, false) == 0,
                  "disabled shipped host scan selects the first occupied AI");
            Check(FindFirstOccupied(site, true) == 4,
                  "enabled host scan selects the first real human candidate");
        }

        g_room.fill(0);
        SetPilot(0, kAiBit | 77u);
        for (std::size_t site : {std::size_t{8}, std::size_t{9},
                                 std::size_t{10}}) {
            Check(FindFirstOccupied(site, true) == -1,
                  "AI-only room has no replacement human host candidate");
        }
    }

    void TestCompletionAnd06F3()
    {
        g_room.fill(0);
        SetPilot(1, 1001u);
        SetPilot(5, 1005u);
        SetStage(1, kPhase2, 2);
        SetStage(5, kPhase2, 2);
        LoadingReplay stock = {2, 0, 0, 0};
        LoadingReplay fixed = stock;
        Send06F3IfComplete(stock, 3, false, kPhase2, 2);
        Send06F3IfComplete(fixed, 3, true, kPhase2, 2);
        Check(stock.send06F3 == 1 && fixed.send06F3 == 1,
              "all-human phase-2 completion retains the original 06F3 send");

        LoadingReplay handler = {2, 0, 0, 0};
        Send06F3IfComplete(handler, 7, true, kPhase2, 2);
        Check(handler.send06F3 == 1,
              "08D7 load-complete recount retains the original 06F3 send");

        SetPilot(0, kAiBit | 88u);
        SetStage(0, kPhase2, 2);
        LoadingReplay stockMixed = {2, 0, 0, 0};
        LoadingReplay fixedMixed = stockMixed;
        Send06F3IfComplete(stockMixed, 3, false, kPhase2, 2);
        Send06F3IfComplete(fixedMixed, 3, true, kPhase2, 2);
        Check(stockMixed.send06F3 == 0 && fixedMixed.send06F3 == 1,
              "AI completion state cannot overcount the native human 06F3 gate");

        g_room.fill(0);
        SetPilot(1, 1001u);
        SetPilot(5, 1005u);
        SetStage(1, kPhase1, 1);
        SetStage(5, kPhase1, 1);
        Check(CountCompleted(0, false, kPhase1, 1) ==
                  CountCompleted(0, true, kPhase1, 1),
              "ordinary phase-1 human completion count is unchanged");
        SetStage(1, kPhase3, 3);
        SetStage(5, kPhase3, 3);
        Check(CountCompleted(5, false, kPhase3, 3) ==
                  CountCompleted(5, true, kPhase3, 3),
              "ordinary phase-3 human completion count is unchanged");
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) {
        std::fprintf(
            stderr,
            "usage: loading_sync_fix_native_test.exe "
            "<loading_sync_fix.dll> <BattleServer.exe>\n");
        return 2;
    }
    try {
        room_title_native::KnownBattleServerImage native(argv[2]);
        Check(native.file_sha256() ==
                  "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4",
              "supported shipped BattleServer SHA-256 is exact");
        BuildSiteReplays(native);
        TestDisabledBaseline();
        TestRejectedSignatures(argv[1]);
        HMODULE module = Activate(argv[1]);
        PatchPhase2ReplayExternalCalls();
        TestSiteAbi();
        TestPhase2InitializationGuard();
        TestTimeoutRemoval();
        TestHostCandidates();
        TestCompletionAnd06F3();
        FreeLibrary(module);
        ReleaseSiteReplays();
        std::printf(
            "PASS %u loading-sync production DLL / shipped-instruction checks; "
            "controlled replay only, no BattleServer main or network started\n",
            g_checks);
        return 0;
    } catch (const std::exception& error) {
        ReleaseSiteReplays();
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
