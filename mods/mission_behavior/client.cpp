// Client-side mission behavior profile binding.
//
// The Mod defers only the twelve exact M1 type-41 registrations. At the
// native whole-table completion boundary it resolves and validates the full
// set, applies one process-local profile transaction, then lets the original
// registration routine derive matching integrity metadata from the final
// bytes. No original table, record pointer or private owner data is retained.
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include "sdgo/mission_behavior_profile.h"
#include "sdgo/mod_settings.h"
#include "sdgo/sdgo_api.h"

#include "client_policy.h"
#include "observer.h"

#if !defined(_M_IX86)
#error mission_behavior client requires the 32-bit GOnline host
#endif

namespace {
using namespace sdgo::mission_behavior;

using GridData = void*(__cdecl*)();
using GridFind =
    bool(__thiscall*)(void*, const std::uint8_t**, std::uint32_t, int, char);
using RegisterRecord =
    void(__thiscall*)(void*, const void*, std::uint32_t, std::uint32_t);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
GridData g_gridData = nullptr;
GridFind g_gridFind = nullptr;
RegisterRecord g_registerRecord = nullptr;
void* g_tablesLoadedTrampoline = nullptr;
const DWORD g_defaultMxcsr = 0x1F80;

Profile g_requestedProfile = Profile::Oc;
bool g_requestedProfileValid = true;
bool g_profileDataValid = false;
std::uint32_t g_profileDataFlags = 0;
volatile LONG g_deferredMask = 0;
volatile LONG g_deferredDuplicates = 0;
SRWLOCK g_statusLock = SRWLOCK_INIT;
SdgoMissionBehaviorClientStatusV1 g_status = {
    sizeof(SdgoMissionBehaviorClientStatusV1),
    1,
    SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_OC,
};

constexpr LONG kAllBindingsMask =
    (LONG{1} << static_cast<LONG>(kM1Bindings.size())) - 1;

std::uint32_t CountBits(LONG value) {
    std::uint32_t count = 0;
    auto bits = static_cast<unsigned long>(value);
    while (bits) {
        bits &= bits - 1;
        ++count;
    }
    return count;
}

int BindingIndex(std::uint32_t npcId) {
    for (std::size_t index = 0; index < kM1Bindings.size(); ++index)
        if (kM1Bindings[index].npc_id == npcId)
            return static_cast<int>(index);
    return -1;
}

LONG AddDeferredBit(LONG bit) {
    LONG observed = InterlockedCompareExchange(&g_deferredMask, 0, 0);
    for (;;) {
        const LONG desired = observed | bit;
        const LONG previous =
            InterlockedCompareExchange(&g_deferredMask, desired, observed);
        if (previous == observed) return observed;
        observed = previous;
    }
}

bool TryReadId(const void* record, std::uint32_t& id) {
    __try {
        if (!record) return false;
        std::memcpy(&id, record, sizeof(id));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
        return false;
    }
}

bool TryFind(
    void* grid,
    std::uint32_t id,
    const std::uint8_t*& record) {
    record = nullptr;
    __try {
        return grid && g_gridFind &&
            g_gridFind(grid, &record, id, static_cast<int>(kNpcRecordKind), 0) &&
            record;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        record = nullptr;
        return false;
    }
}

bool TryCopyRecord(
    const std::uint8_t* source,
    std::array<std::uint8_t, kNpcRecordBytes>& output) {
    __try {
        if (!source) return false;
        std::memcpy(output.data(), source, output.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.fill(0);
        return false;
    }
}

bool TryReadAi(
    const std::uint8_t* record,
    std::array<char, kNpcAiNameCharacters>& output) {
    __try {
        return ReadAiName(record, kNpcRecordBytes, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output.fill('\0');
        return false;
    }
}

bool TryWriteAi(std::uint8_t* record, const char* value) {
    __try {
        return WriteAiName(record, kNpcRecordBytes, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryRegister(void* grid, const std::uint8_t* record) {
    __try {
        if (!grid || !record || !g_registerRecord) return false;
        g_registerRecord(
            grid,
            record,
            static_cast<std::uint32_t>(kNpcRecordBytes),
            kNpcRecordKind);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* TryCurrentGrid() {
    __try {
        return g_gridData ? g_gridData() : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void PublishStatus(
    std::uint32_t generation,
    std::uint32_t observed,
    std::uint32_t applied,
    std::uint32_t restored,
    std::uint32_t unchanged,
    std::uint32_t conflicts,
    std::uint32_t activeProfile,
    std::uint32_t lastNpcId) {
    AcquireSRWLockExclusive(&g_statusLock);
    g_status.table_generation = generation;
    g_status.observed_records = observed;
    g_status.applied_records = applied;
    g_status.restored_records = restored;
    g_status.unchanged_records = unchanged;
    g_status.conflict_records = conflicts;
    g_status.active_profile = activeProfile;
    g_status.last_npc_id = lastNpcId;
    ReleaseSRWLockExclusive(&g_statusLock);
}

std::uint32_t NextGeneration() {
    AcquireSRWLockExclusive(&g_statusLock);
    const std::uint32_t result = ++g_status.table_generation;
    ReleaseSRWLockExclusive(&g_statusLock);
    return result;
}

std::uint32_t RegisterDeferred(
    void* grid,
    LONG mask,
    const std::array<const std::uint8_t*, kM1Bindings.size()>& records,
    std::uint32_t& lastNpcId) {
    std::uint32_t failures = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (!(mask & (LONG{1} << static_cast<LONG>(index)))) continue;
        lastNpcId = kM1Bindings[index].npc_id;
        if (!TryRegister(grid, records[index])) ++failures;
    }
    return failures;
}

std::uint32_t RollBack(
    const std::array<const std::uint8_t*, kM1Bindings.size()>& records,
    const M1SetPlan& plan,
    std::size_t modified,
    std::uint32_t& lastNpcId) {
    std::uint32_t failures = 0;
    while (modified > 0) {
        --modified;
        const auto& decision = plan.records[modified];
        if (decision.result == RecordResult::Unchanged) continue;
        lastNpcId = decision.npc_id;
        auto* record = const_cast<std::uint8_t*>(records[modified]);
        std::array<char, kNpcAiNameCharacters> readback{};
        if (!TryWriteAi(record, decision.before.data()) ||
            !TryReadAi(record, readback) ||
            std::strcmp(readback.data(), decision.before.data()) != 0)
            ++failures;
    }
    return failures;
}

void __stdcall OnTablesLoaded(void* grid) {
    const DWORD incomingError = GetLastError();
    const LONG deferredMask = InterlockedExchange(&g_deferredMask, 0);
    const LONG duplicateCount = InterlockedExchange(&g_deferredDuplicates, 0);
    const std::uint32_t generation = NextGeneration();

    std::array<const std::uint8_t*, kM1Bindings.size()> records{};
    std::array<std::array<std::uint8_t, kNpcRecordBytes>, kM1Bindings.size()>
        snapshots{};
    std::array<const std::uint8_t*, kM1Bindings.size()> snapshotPointers{};
    std::uint32_t observed = 0;
    std::uint32_t conflicts = 0;
    std::uint32_t lastNpcId = 0;
    bool recordsValid = true;

    if (deferredMask != kAllBindingsMask) {
        recordsValid = false;
        conflicts += CountBits(kAllBindingsMask & ~deferredMask);
    }
    if (duplicateCount) {
        recordsValid = false;
        conflicts += static_cast<std::uint32_t>(duplicateCount);
    }
    if (TryCurrentGrid() != grid) {
        recordsValid = false;
        ++conflicts;
    }

    for (std::size_t index = 0; index < records.size(); ++index) {
        lastNpcId = kM1Bindings[index].npc_id;
        const std::uint8_t* record = nullptr;
        if (!TryFind(grid, lastNpcId, record)) {
            recordsValid = false;
            ++conflicts;
            continue;
        }
        records[index] = record;
        ++observed;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (records[previous] == record) {
                recordsValid = false;
                ++conflicts;
            }
        }
        if (!TryCopyRecord(record, snapshots[index])) {
            recordsValid = false;
            ++conflicts;
            continue;
        }
        snapshotPointers[index] = snapshots[index].data();
        if (ReadU32(snapshots[index].data()) != kM1Bindings[index].npc_id) {
            recordsValid = false;
            ++conflicts;
        }
    }

    const bool profileAvailable =
        g_requestedProfileValid &&
        (g_requestedProfile == Profile::Gco || g_profileDataValid);
    M1SetPlan plan;
    if (recordsValid && profileAvailable) {
        plan = PlanM1Set(
            snapshotPointers,
            kNpcRecordBytes,
            g_requestedProfile);
        if (!plan.valid) {
            recordsValid = false;
            ++conflicts;
            lastNpcId = plan.conflict_npc_id;
        }
    }

    std::size_t modified = 0;
    std::uint32_t rollbackFailures = 0;
    if (recordsValid && profileAvailable) {
        for (std::size_t index = 0; index < records.size(); ++index) {
            const auto& decision = plan.records[index];
            lastNpcId = decision.npc_id;
            modified = index + 1;
            if (decision.result == RecordResult::Unchanged) {
                continue;
            }
            std::array<char, kNpcAiNameCharacters> liveName{};
            auto* record = const_cast<std::uint8_t*>(records[index]);
            if (!TryReadAi(record, liveName) ||
                std::strcmp(liveName.data(), decision.before.data()) != 0 ||
                !TryWriteAi(record, decision.after.data()) ||
                !TryReadAi(record, liveName) ||
                std::strcmp(liveName.data(), decision.after.data()) != 0) {
                ++conflicts;
                recordsValid = false;
                rollbackFailures =
                    RollBack(records, plan, modified, lastNpcId);
                conflicts += rollbackFailures;
                break;
            }
        }
    }

    const std::uint32_t registrationFailures =
        RegisterDeferred(grid, deferredMask, records, lastNpcId);
    conflicts += registrationFailures;

    std::uint32_t activeProfile =
        SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_UNAVAILABLE;
    std::uint32_t applied = 0;
    std::uint32_t restored = 0;
    std::uint32_t unchanged = 0;
    if (recordsValid && profileAvailable && !registrationFailures) {
        activeProfile = ProfileWireValue(g_requestedProfile);
        unchanged = plan.unchanged;
        if (g_requestedProfile == Profile::Oc)
            applied = plan.changed;
        else
            restored = plan.changed;
    } else if (!profileAvailable && observed == kM1Bindings.size()) {
        unchanged = static_cast<std::uint32_t>(kM1Bindings.size());
    }

    PublishStatus(
        generation,
        observed,
        applied,
        restored,
        unchanged,
        conflicts,
        activeProfile,
        lastNpcId);

    if (activeProfile != SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_UNAVAILABLE) {
        g_api->Log(
            g_mod,
            SDGO_LOG_INFO,
            "mission behavior %s active: %lu observed, %lu changed, "
            "%lu already matching; native integrity registration completed",
            g_requestedProfile == Profile::Oc ? "OC" : "GCO",
            static_cast<unsigned long>(observed),
            static_cast<unsigned long>(applied + restored),
            static_cast<unsigned long>(unchanged));
    } else if (!profileAvailable) {
        g_api->Log(
            g_mod,
            SDGO_LOG_ERROR,
            "requested OC mission behavior is unavailable because profile.bin "
            "is missing or invalid; original client behavior retained");
    } else {
        g_api->Log(
            g_mod,
            SDGO_LOG_ERROR,
            "mission behavior transaction refused: %lu conflict(s), "
            "%lu registration failure(s), %lu rollback failure(s); "
            "active profile unavailable",
            static_cast<unsigned long>(conflicts),
            static_cast<unsigned long>(registrationFailures),
            static_cast<unsigned long>(rollbackFailures));
    }
    SetLastError(incomingError);
}

void __fastcall RegisterNpcRecord(
    void* self,
    void*,
    const void* record,
    std::uint32_t recordBytes,
    std::uint32_t kind) {
    const DWORD incomingError = GetLastError();
    std::uint32_t npcId = 0;
    const int index =
        kind == kNpcRecordKind &&
        recordBytes == kNpcRecordBytes &&
        TryReadId(record, npcId)
            ? BindingIndex(npcId)
            : -1;
    if (index >= 0) {
        const LONG bit = LONG{1} << index;
        if (AddDeferredBit(bit) & bit)
            InterlockedIncrement(&g_deferredDuplicates);
        SetLastError(incomingError);
        return;
    }
    SetLastError(incomingError);
    g_registerRecord(self, record, recordBytes, kind);
}

__declspec(naked) void TablesLoadedSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0FFFFFFF0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { push esi }
    __asm { call OnTablesLoaded }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_tablesLoadedTrampoline] }
}

template<class T>
bool Bind(T& target, const char* name) {
    target = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return target != nullptr;
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl
SdgoMissionBehaviorClientGetStatusV1(
    SdgoMissionBehaviorClientStatusV1* output) {
    if (!output || output->size < sizeof(*output)) return 0;
    AcquireSRWLockShared(&g_statusLock);
    const SdgoMissionBehaviorClientStatusV1 snapshot = g_status;
    ReleaseSRWLockShared(&g_statusLock);
    *output = snapshot;
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->Log || !api->Symbol || !api->HookCode || !api->HookSite ||
        !api->ModDirectory)
        return 0;

    g_api = api;
    g_mod = mod;

    char profileText[16]{};
    const sdgo::ModSettings settings(api, mod);
    const std::uint32_t profileLength =
        settings.String("Profile", "oc", profileText, sizeof(profileText));
    g_requestedProfileValid =
        profileLength > 0 &&
        profileLength < sizeof(profileText) - 1 &&
        ParseClientProfile(profileText, g_requestedProfile);

    std::string profileError;
    Bundle bundle;
    const std::filesystem::path profilePath =
        std::filesystem::path(api->ModDirectory(mod)) / L"profile.bin";
    g_profileDataValid =
        LoadProfileFile(profilePath, bundle, profileError);
    if (g_profileDataValid &&
        (!(bundle.header.flags & kNpcOverrides) ||
         bundle.entries.empty())) {
        g_profileDataValid = false;
        profileError =
            "owner mission profile has no server task-row differences";
    }
    g_profileDataFlags =
        g_profileDataValid ? bundle.header.flags : 0;

    AcquireSRWLockExclusive(&g_statusLock);
    g_status = {};
    g_status.size = sizeof(g_status);
    g_status.version = 1;
    g_status.requested_profile =
        g_requestedProfileValid
            ? ProfileWireValue(g_requestedProfile)
            : SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_UNAVAILABLE;
    g_status.profile_data_valid = g_profileDataValid ? 1u : 0u;
    g_status.profile_data_flags = g_profileDataFlags;
    ReleaseSRWLockExclusive(&g_statusLock);

    if (!g_requestedProfileValid) {
        api->Log(
            mod,
            SDGO_LOG_ERROR,
            "invalid mission behavior Profile value; use oc or gco; "
            "original behavior will be retained");
    } else if (!g_profileDataValid) {
        api->Log(
            mod,
            g_requestedProfile == Profile::Oc ? SDGO_LOG_ERROR
                                              : SDGO_LOG_WARN,
            "owner mission profile unavailable: %s",
            profileError.c_str());
    }

    if (!Bind(g_gridData, "grid.data") ||
        !Bind(g_gridFind, "grid.find") ||
        !Bind(g_registerRecord, "grid.register_record"))
        return 0;

    if (!api->HookCode(
            mod,
            "grid.register_record",
            reinterpret_cast<void*>(&RegisterNpcRecord),
            reinterpret_cast<void**>(&g_registerRecord)) ||
        !api->HookSite(
            mod,
            "mission_behavior.tables_loaded",
            reinterpret_cast<void*>(&TablesLoadedSite),
            &g_tablesLoadedTrampoline))
        return 0;

    AcquireSRWLockExclusive(&g_statusLock);
    g_status.hook_installed = 1;
    ReleaseSRWLockExclusive(&g_statusLock);

    api->Log(
        mod,
        SDGO_LOG_INFO,
        "ready: requested profile %s; twelve M1 registrations will be "
        "validated and finalized as one native table transaction",
        g_requestedProfileValid
            ? (g_requestedProfile == Profile::Oc ? "OC" : "GCO")
            : "invalid");
    return 1;
}
