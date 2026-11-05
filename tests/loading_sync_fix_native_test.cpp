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
    constexpr std::size_t kSiteCount = 11;
    constexpr std::size_t kRoomSize = 0x32880;
    constexpr std::size_t kPilotId = 0x95C;
    constexpr std::size_t kSlotStride = 0x48E;
    constexpr std::size_t kPhase1 = 0x964;
    constexpr std::size_t kPhase2 = 0x966;
    constexpr std::size_t kPhase3 = 0x966;
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

    constexpr SiteDefinition kDefinitions[kSiteCount] = {
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

    struct SiteRuntime
    {
        std::uint8_t* code = nullptr;
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

    std::array<SiteRuntime, kSiteCount> g_sites = {};
    std::array<std::uint8_t, kRoomSize> g_room = {};
    std::vector<std::string> g_logs;
    unsigned g_checks = 0;
    unsigned g_hookCalls = 0;
    int g_missingSymbol = -1;

    void* g_entry = nullptr;
    std::uint8_t* g_roomPointer = nullptr;
    std::uint32_t g_slotOffset = 0;
    std::uint32_t g_expectedEsp = 0;
    Registers g_seen = {};

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
        for (std::size_t index = 0; index < kSiteCount; ++index) {
            if (std::strcmp(name, kDefinitions[index].name) == 0) return index;
        }
        return kSiteCount;
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        if (!name) return nullptr;
        for (std::size_t index = 0; index < kSiteCount; ++index) {
            const std::string continuation =
                std::string(kDefinitions[index].name) + "_continue";
            if (std::strcmp(name, kDefinitions[index].name) == 0) {
                if (g_missingSymbol == static_cast<int>(index)) return nullptr;
                return g_sites[index].code;
            }
            if (continuation == name) return g_sites[index].code + 8;
        }
        return nullptr;
    }

    int HookSite(SdgoMod*, const char* name, void* detour, void** original)
    {
        const std::size_t index = name ? FindSite(name) : kSiteCount;
        Check(index < kSiteCount && detour && original,
              "production Mod requests a declared native site");
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
        g_missingSymbol = -1;
        g_logs.clear();
        for (auto& site : g_sites) site.detour = nullptr;
    }

    void BuildSiteReplays(const room_title_native::KnownBattleServerImage& image)
    {
        for (std::size_t index = 0; index < kSiteCount; ++index) {
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
    }

    void ReleaseSiteReplays()
    {
        for (auto& site : g_sites) {
            if (site.code) VirtualFree(site.code, 0, MEM_RELEASE);
            site = {};
        }
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
        for (std::size_t index = 0; index < kSiteCount; ++index) {
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
            g_missingSymbol = 4;
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
    }

    HMODULE Activate(const wchar_t* dll)
    {
        HMODULE module = nullptr;
        SdgoModLoadFn entry = LoadEntry(dll, module);
        ResetActivation();
        SdgoApi api = Api();
        Check(entry(&api, reinterpret_cast<SdgoMod*>(1)) != 0,
              "actual production Mod activates on the supported signatures");
        Check(g_hookCalls == kSiteCount,
              "actual production Mod installs all eleven declared site hooks");
        Check(g_logs.size() == 1 &&
                  g_logs[0].find("human loading behavior remains native") !=
                      std::string::npos,
              "activation emits the bounded loading-sync receipt");
        return module;
    }

    void TestSiteAbi()
    {
        for (std::size_t index = 0; index < kSiteCount; ++index) {
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
        TestSiteAbi();
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
