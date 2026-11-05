// Keep server-created high-bit AI pilots out of the native human loading
// synchronization loops. Human loading, progress, timeout and removal behavior
// remains native.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"
#include "loading_sync_policy.h"

#if !defined(_M_IX86)
#error loading_sync_fix requires the 32-bit BattleServer ABI
#endif

namespace
{
    constexpr std::size_t kSiteCount = 12;
    constexpr std::size_t kCmpInstructionBytes = 8;
    constexpr std::size_t kPhase2InitializeInstructionBytes = 13;
    constexpr std::size_t kPhase2AlreadyAdvancedOffset = 0x7C;

    void* g_phase1CountContinue = nullptr;
    void* g_phase1RemoveContinue = nullptr;
    void* g_phase1RetryContinue = nullptr;
    void* g_phase2InitializeContinue = nullptr;
    void* g_phase2InitializeAlreadyAdvanced = nullptr;
    void* g_phase2CountContinue = nullptr;
    void* g_phase2RemoveContinue = nullptr;
    void* g_phase3CountContinue = nullptr;
    void* g_phase3RemoveContinue = nullptr;
    void* g_loadCompleteCountContinue = nullptr;
    void* g_hostRemovedCandidateContinue = nullptr;
    void* g_hostObserverCandidateContinue = nullptr;
    void* g_hostInvalidCandidateContinue = nullptr;
    void* g_originalSites[kSiteCount] = {};

    // Every intercepted instruction is:
    //   cmp dword ptr [room + slot * 0x48E + 0x95C], 0
    // and is immediately followed by JNE. For a high-bit Mod AI pilot, TEST
    // with zero leaves ZF set so the native branch treats that slot exactly as
    // empty. Human IDs replay the original CMP flags. No register or stack
    // value is changed, and each detour jumps directly past the intercepted
    // eight-byte CMP so the HookSite trampoline cannot compare the AI ID again.
    __declspec(naked) void Phase1CountSite()
    {
        __asm {
            test dword ptr [eax + edx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase1CountContinue]
        ai_slot:
            test dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase1CountContinue]
        }
    }

    __declspec(naked) void Phase1RemoveSite()
    {
        __asm {
            test dword ptr [eax + edx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase1RemoveContinue]
        ai_slot:
            test dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase1RemoveContinue]
        }
    }

    __declspec(naked) void Phase1RetrySite()
    {
        __asm {
            test dword ptr [edx + ecx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_phase1RetryContinue]
        ai_slot:
            test dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_phase1RetryContinue]
        }
    }

    // The stock initial-load ACK handler enters phase 2 after every human has
    // reached phase 1. The periodic check may already have advanced the room
    // while one peer was slow. Its first late ACK, or a repeated ACK, must not
    // reset every +0x966 stage and strand clients whose completion was accepted.
    //
    // At this site EBP is the native handler frame and [EBP-0x14] is the room.
    // Preserve the incoming context while deciding. Phase 1 replays the exact
    // intercepted writes. Later phases skip only the destructive phase/timer
    // initialization and stage clearing, then resume at the native 09C4
    // construction so the late peer still receives the stock notification.
    __declspec(naked) void Phase2InitializeSite()
    {
        __asm {
            pushfd
            push eax
            mov eax, dword ptr [ebp - 014h]
            cmp dword ptr [eax + 02EEB0h], 1
            jne already_advanced
            pop eax
            popfd
            mov eax, dword ptr [ebp - 014h]
            mov dword ptr [eax + 02EEB0h], 2
            jmp dword ptr [g_phase2InitializeContinue]
        already_advanced:
            pop eax
            popfd
            jmp dword ptr [g_phase2InitializeAlreadyAdvanced]
        }
    }

    __declspec(naked) void Phase2CountSite()
    {
        __asm {
            test dword ptr [eax + edx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase2CountContinue]
        ai_slot:
            test dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase2CountContinue]
        }
    }

    __declspec(naked) void Phase2RemoveSite()
    {
        __asm {
            test dword ptr [edx + ecx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_phase2RemoveContinue]
        ai_slot:
            test dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_phase2RemoveContinue]
        }
    }

    __declspec(naked) void Phase3CountSite()
    {
        __asm {
            test dword ptr [eax + edx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase3CountContinue]
        ai_slot:
            test dword ptr [eax + edx + 095Ch], 0
            jmp dword ptr [g_phase3CountContinue]
        }
    }

    __declspec(naked) void Phase3RemoveSite()
    {
        __asm {
            test dword ptr [ecx + eax + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [ecx + eax + 095Ch], 0
            jmp dword ptr [g_phase3RemoveContinue]
        ai_slot:
            test dword ptr [ecx + eax + 095Ch], 0
            jmp dword ptr [g_phase3RemoveContinue]
        }
    }

    __declspec(naked) void LoadCompleteCountSite()
    {
        __asm {
            test dword ptr [edx + ecx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_loadCompleteCountContinue]
        ai_slot:
            test dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_loadCompleteCountContinue]
        }
    }

    // RoomRemove has three fallback scans that select the first occupied slot
    // as a replacement human host. A high-bit AI is occupied for gameplay but
    // cannot own the human wait-room session, so these scans use the same
    // empty-slot projection. Victim lookup and existing-host identity lookup
    // remain untouched.
    __declspec(naked) void HostRemovedCandidateSite()
    {
        __asm {
            test dword ptr [edx + ecx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_hostRemovedCandidateContinue]
        ai_slot:
            test dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_hostRemovedCandidateContinue]
        }
    }

    __declspec(naked) void HostObserverCandidateSite()
    {
        __asm {
            test dword ptr [edx + ecx + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_hostObserverCandidateContinue]
        ai_slot:
            test dword ptr [edx + ecx + 095Ch], 0
            jmp dword ptr [g_hostObserverCandidateContinue]
        }
    }

    __declspec(naked) void HostInvalidCandidateSite()
    {
        __asm {
            test dword ptr [ecx + eax + 095Ch], 080000000h
            jnz ai_slot
            cmp dword ptr [ecx + eax + 095Ch], 0
            jmp dword ptr [g_hostInvalidCandidateContinue]
        ai_slot:
            test dword ptr [ecx + eax + 095Ch], 0
            jmp dword ptr [g_hostInvalidCandidateContinue]
        }
    }

    struct SiteBinding
    {
        const char* siteSymbol;
        const char* continueSymbol;
        void* detour;
        void** original;
        const std::uint8_t* expected;
        std::size_t instructionBytes;
        void** continuation;
        const std::uint8_t* continuationExpected;
        std::size_t continuationExpectedBytes;
    };

    constexpr std::uint8_t kEaxEdxCmp[kCmpInstructionBytes] =
        {0x83, 0xBC, 0x10, 0x5C, 0x09, 0x00, 0x00, 0x00};
    constexpr std::uint8_t kEdxEcxCmp[kCmpInstructionBytes] =
        {0x83, 0xBC, 0x0A, 0x5C, 0x09, 0x00, 0x00, 0x00};
    constexpr std::uint8_t kEcxEaxCmp[kCmpInstructionBytes] =
        {0x83, 0xBC, 0x01, 0x5C, 0x09, 0x00, 0x00, 0x00};
    constexpr std::uint8_t
        kPhase2Initialize[kPhase2InitializeInstructionBytes] = {
            0x8B, 0x45, 0xEC,
            0xC7, 0x80, 0xB0, 0xEE, 0x02, 0x00,
            0x02, 0x00, 0x00, 0x00};
    constexpr std::uint8_t kPhase2InitializeContinue[16] = {
        0x6A, 0x00, 0xE8, 0x10, 0x8D, 0x27, 0x00, 0x83,
        0xC4, 0x04, 0x8B, 0x4D, 0xEC, 0x89, 0x81, 0xB4};
    constexpr std::uint8_t kPhase2AlreadyAdvanced[16] = {
        0x8B, 0x55, 0xE8, 0x89, 0x95, 0x7C, 0xEB, 0xFF,
        0xFF, 0xB8, 0xC4, 0x09, 0x00, 0x00, 0x66, 0x89};
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->Symbol || !api->HookSite || !api->Log) {
        return 0;
    }

    SiteBinding sites[kSiteCount] = {
        {"loading_sync.phase1_count", "loading_sync.phase1_count_continue",
         reinterpret_cast<void*>(&Phase1CountSite), &g_originalSites[0],
         kEaxEdxCmp, kCmpInstructionBytes, &g_phase1CountContinue, nullptr, 0},
        {"loading_sync.phase1_remove", "loading_sync.phase1_remove_continue",
         reinterpret_cast<void*>(&Phase1RemoveSite), &g_originalSites[1],
         kEaxEdxCmp, kCmpInstructionBytes, &g_phase1RemoveContinue, nullptr, 0},
        {"loading_sync.phase1_retry", "loading_sync.phase1_retry_continue",
         reinterpret_cast<void*>(&Phase1RetrySite), &g_originalSites[2],
         kEdxEcxCmp, kCmpInstructionBytes, &g_phase1RetryContinue, nullptr, 0},
        {"loading_sync.phase2_initialize",
         "loading_sync.phase2_initialize_continue",
         reinterpret_cast<void*>(&Phase2InitializeSite), &g_originalSites[3],
         kPhase2Initialize, kPhase2InitializeInstructionBytes,
         &g_phase2InitializeContinue, kPhase2InitializeContinue,
         sizeof(kPhase2InitializeContinue)},
        {"loading_sync.phase2_count", "loading_sync.phase2_count_continue",
         reinterpret_cast<void*>(&Phase2CountSite), &g_originalSites[4],
         kEaxEdxCmp, kCmpInstructionBytes, &g_phase2CountContinue, nullptr, 0},
        {"loading_sync.phase2_remove", "loading_sync.phase2_remove_continue",
         reinterpret_cast<void*>(&Phase2RemoveSite), &g_originalSites[5],
         kEdxEcxCmp, kCmpInstructionBytes, &g_phase2RemoveContinue, nullptr, 0},
        {"loading_sync.phase3_count", "loading_sync.phase3_count_continue",
         reinterpret_cast<void*>(&Phase3CountSite), &g_originalSites[6],
         kEaxEdxCmp, kCmpInstructionBytes, &g_phase3CountContinue, nullptr, 0},
        {"loading_sync.phase3_remove", "loading_sync.phase3_remove_continue",
         reinterpret_cast<void*>(&Phase3RemoveSite), &g_originalSites[7],
         kEcxEaxCmp, kCmpInstructionBytes, &g_phase3RemoveContinue, nullptr, 0},
        {"loading_sync.load_complete_count",
         "loading_sync.load_complete_count_continue",
         reinterpret_cast<void*>(&LoadCompleteCountSite), &g_originalSites[8],
         kEdxEcxCmp, kCmpInstructionBytes, &g_loadCompleteCountContinue,
         nullptr, 0},
        {"loading_sync.host_removed_candidate",
         "loading_sync.host_removed_candidate_continue",
         reinterpret_cast<void*>(&HostRemovedCandidateSite),
         &g_originalSites[9], kEdxEcxCmp, kCmpInstructionBytes,
         &g_hostRemovedCandidateContinue, nullptr, 0},
        {"loading_sync.host_observer_candidate",
         "loading_sync.host_observer_candidate_continue",
         reinterpret_cast<void*>(&HostObserverCandidateSite),
         &g_originalSites[10], kEdxEcxCmp, kCmpInstructionBytes,
         &g_hostObserverCandidateContinue, nullptr, 0},
        {"loading_sync.host_invalid_candidate",
         "loading_sync.host_invalid_candidate_continue",
         reinterpret_cast<void*>(&HostInvalidCandidateSite),
         &g_originalSites[11], kEcxEaxCmp, kCmpInstructionBytes,
         &g_hostInvalidCandidateContinue, nullptr, 0},
    };

    void* alreadyAdvanced = api->Symbol(
        mod, "loading_sync.phase2_initialize_already_advanced");
    if (!alreadyAdvanced ||
        std::memcmp(
            alreadyAdvanced, kPhase2AlreadyAdvanced,
            sizeof(kPhase2AlreadyAdvanced)) != 0) {
        return 0;
    }
    g_phase2InitializeAlreadyAdvanced = alreadyAdvanced;

    for (auto& site : sites) {
        void* address = api->Symbol(mod, site.siteSymbol);
        void* continuation = api->Symbol(mod, site.continueSymbol);
        if (!address || !continuation ||
            static_cast<std::uint8_t*>(continuation) !=
                static_cast<std::uint8_t*>(address) + site.instructionBytes ||
            std::memcmp(
                address, site.expected, site.instructionBytes) != 0 ||
            (site.detour == reinterpret_cast<void*>(&Phase2InitializeSite) &&
             static_cast<std::uint8_t*>(alreadyAdvanced) !=
                 static_cast<std::uint8_t*>(address) +
                     kPhase2AlreadyAdvancedOffset) ||
            (site.continuationExpected &&
             std::memcmp(
                 continuation, site.continuationExpected,
                 site.continuationExpectedBytes) != 0)) {
            return 0;
        }
        *site.continuation = continuation;
    }

    for (auto& site : sites) {
        if (!api->HookSite(mod, site.siteSymbol, site.detour, site.original)) {
            return 0;
        }
    }

    api->Log(
        mod, SDGO_LOG_INFO,
        "high-bit Mod AI pilots are excluded from native phase 1-3 loading "
        "counts, retries and timeout removals, and cannot become replacement "
        "human room hosts; late initial ACKs preserve an already-advanced "
        "loading phase while retaining the native notification path; human "
        "loading behavior remains native");
    return 1;
}
