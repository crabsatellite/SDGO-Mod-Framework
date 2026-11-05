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
    constexpr std::size_t kSiteCount = 11;
    constexpr std::size_t kInstructionBytes = 8;

    void* g_phase1CountContinue = nullptr;
    void* g_phase1RemoveContinue = nullptr;
    void* g_phase1RetryContinue = nullptr;
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
        void** continuation;
    };

    constexpr std::uint8_t kEaxEdxCmp[kInstructionBytes] =
        {0x83, 0xBC, 0x10, 0x5C, 0x09, 0x00, 0x00, 0x00};
    constexpr std::uint8_t kEdxEcxCmp[kInstructionBytes] =
        {0x83, 0xBC, 0x0A, 0x5C, 0x09, 0x00, 0x00, 0x00};
    constexpr std::uint8_t kEcxEaxCmp[kInstructionBytes] =
        {0x83, 0xBC, 0x01, 0x5C, 0x09, 0x00, 0x00, 0x00};
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
         kEaxEdxCmp, &g_phase1CountContinue},
        {"loading_sync.phase1_remove", "loading_sync.phase1_remove_continue",
         reinterpret_cast<void*>(&Phase1RemoveSite), &g_originalSites[1],
         kEaxEdxCmp, &g_phase1RemoveContinue},
        {"loading_sync.phase1_retry", "loading_sync.phase1_retry_continue",
         reinterpret_cast<void*>(&Phase1RetrySite), &g_originalSites[2],
         kEdxEcxCmp, &g_phase1RetryContinue},
        {"loading_sync.phase2_count", "loading_sync.phase2_count_continue",
         reinterpret_cast<void*>(&Phase2CountSite), &g_originalSites[3],
         kEaxEdxCmp, &g_phase2CountContinue},
        {"loading_sync.phase2_remove", "loading_sync.phase2_remove_continue",
         reinterpret_cast<void*>(&Phase2RemoveSite), &g_originalSites[4],
         kEdxEcxCmp, &g_phase2RemoveContinue},
        {"loading_sync.phase3_count", "loading_sync.phase3_count_continue",
         reinterpret_cast<void*>(&Phase3CountSite), &g_originalSites[5],
         kEaxEdxCmp, &g_phase3CountContinue},
        {"loading_sync.phase3_remove", "loading_sync.phase3_remove_continue",
         reinterpret_cast<void*>(&Phase3RemoveSite), &g_originalSites[6],
         kEcxEaxCmp, &g_phase3RemoveContinue},
        {"loading_sync.load_complete_count",
         "loading_sync.load_complete_count_continue",
         reinterpret_cast<void*>(&LoadCompleteCountSite), &g_originalSites[7],
         kEdxEcxCmp, &g_loadCompleteCountContinue},
        {"loading_sync.host_removed_candidate",
         "loading_sync.host_removed_candidate_continue",
         reinterpret_cast<void*>(&HostRemovedCandidateSite),
         &g_originalSites[8], kEdxEcxCmp, &g_hostRemovedCandidateContinue},
        {"loading_sync.host_observer_candidate",
         "loading_sync.host_observer_candidate_continue",
         reinterpret_cast<void*>(&HostObserverCandidateSite),
         &g_originalSites[9], kEdxEcxCmp, &g_hostObserverCandidateContinue},
        {"loading_sync.host_invalid_candidate",
         "loading_sync.host_invalid_candidate_continue",
         reinterpret_cast<void*>(&HostInvalidCandidateSite),
         &g_originalSites[10], kEcxEaxCmp, &g_hostInvalidCandidateContinue},
    };

    for (auto& site : sites) {
        void* address = api->Symbol(mod, site.siteSymbol);
        void* continuation = api->Symbol(mod, site.continueSymbol);
        if (!address || !continuation ||
            static_cast<std::uint8_t*>(continuation) !=
                static_cast<std::uint8_t*>(address) + kInstructionBytes ||
            std::memcmp(address, site.expected, kInstructionBytes) != 0) {
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
        "human room hosts; human loading behavior remains native");
    return 1;
}
