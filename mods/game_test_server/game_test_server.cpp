// Passive receipt writer for real PvP AI battles.
//
// This Mod never replaces BattleServer's main function and never creates rooms,
// pilots or packets. A separately driven, normally authenticated client creates
// and starts the room. pvp_ai snapshots its existing real hook boundaries; this
// observer correlates those snapshots and writes durable test receipts.

#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "sdgo/battle_rewards_damage_observer.h"
#include "sdgo/mission_behavior_profile.h"
#include "sdgo/mission_behavior_server_observer.h"
#include "sdgo/oc_custom_battle_observer.h"
#include "sdgo/pvp_ai_observer.h"
#include "sdgo/ace_ai_observer.h"
#include "sdgo/sdgo_api.h"
#include "../game_test_shared/ace_ai_receipts.h"

namespace
{
    constexpr unsigned kQueueCapacity = 256;
    constexpr unsigned kDrainBatch = 64;
    constexpr unsigned kDamageQueueCapacity = 512;
    constexpr unsigned kDamageDrainBatch = 64;
    constexpr unsigned kAceQueueCapacity = 512;
    constexpr unsigned kAceDrainBatch = 64;
    constexpr size_t kRoomMode = 0x05;
    constexpr size_t kRoomTitleLength = 0x79;
    constexpr size_t kRoomTitle = 0x7A;
    constexpr size_t kRoomHost = 0x4CE;
    constexpr size_t kSlotInfo = 0x533;
    constexpr size_t kSlotInfoStride = 0x48E;
    constexpr size_t kSlotPilotId = 0x429;
    constexpr size_t kBattlePilots = 0x3CE4;
    constexpr size_t kBattlePilotStride = 0x1F54;
    constexpr size_t kPilotEntity = 0x00;
    constexpr size_t kPilotTeam = 0x04;
    constexpr size_t kUnitKey = 0x00;
    constexpr size_t kUnitMaxHp = 0x04;
    constexpr size_t kUnitHp = 0x34;
    constexpr size_t kUnitData = 0x6C;
    constexpr int kBattleSlots = 12;

    enum class DamageSource
    {
        None,
        DirectHook,
        BattleRewardsBridge
    };

    struct RunConfig
    {
        char runId[37] = {};
        char suite[32] = {};
        char nonce[9] = {};
        uint32_t hostPilotId = 0;
        uint8_t mode = 0;
        uint8_t minimumAi = 1;
        bool requireEnd = false;
        uint32_t missionBehaviorProfile = 0;
        uint32_t missionGameModeId = 0;
    };

    struct Lifecycle
    {
        enum Outcome { Running, Pass, Fail } outcome = Running;
        uint32_t roomCookie = 0;
        uint32_t hostPilotId = 0;
        uint8_t mode = 0;
        unsigned phase = 0;
        uint64_t eventCount = 0;
        std::string reason = "waiting for authenticated host";
    };

    struct DamageEvent
    {
        uint64_t sequence = 0;
        uint64_t qpc = 0;
        uint32_t threadId = 0;
        uint32_t roomCookie = 0;
        uint32_t hostPilotId = 0;
        uint32_t attackerPilotId = 0;
        uint32_t victimPilotId = 0;
        uint32_t attackerSlotPilotId = 0;
        uint32_t victimSlotPilotId = 0;
        uint32_t victimUnitKey = 0;
        uint32_t victimHp = 0;
        uint32_t victimMaxHp = 0;
        uintptr_t victimUnitData = 0;
        uint32_t effectiveLoss = 0;
        int32_t deathFlag = 0;
        int32_t attackerSlot = -1;
        int32_t victimSlot = -1;
        uint8_t mode = 0;
        uint8_t attackerTeam = 0;
        uint8_t victimTeam = 0;
        uint8_t attackerIsAi = 0;
        uint8_t victimIsAi = 0;
        uint8_t samePilot = 0;
        uint8_t sameSlot = 0;
        uint8_t friendly = 0;
        uint8_t identityValid = 0;
    };

    struct AceEvent
    {
        uint8_t scope = 0; // 1: PvP, 2: mission
        uint8_t reserved[3] = {};
        SdgoAceAiServerSnapshotV1 snapshot{};
    };

    using RecordDamageFn = uint32_t(__thiscall*)(
        void* room, void* victimPilot, void* attackerPilot, void* victimUnit,
        uint32_t effectiveLoss, int32_t deathFlag);

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    RunConfig g_config;
    std::wstring g_directory;
    std::wstring g_runIni;
    std::wstring g_expectIni;
    std::wstring g_statusPath;
    HANDLE g_eventFile = INVALID_HANDLE_VALUE;
    HANDLE g_startGate = nullptr;
    CRITICAL_SECTION g_queueLock;
    SdgoPvpAiObserverEventV1 g_queue[kQueueCapacity] = {};
    unsigned g_queueRead = 0;
    unsigned g_queueWrite = 0;
    unsigned g_queueCount = 0;
    DamageEvent g_damageQueue[kDamageQueueCapacity] = {};
    unsigned g_damageQueueRead = 0;
    unsigned g_damageQueueWrite = 0;
    unsigned g_damageQueueCount = 0;
    AceEvent g_aceQueue[kAceQueueCapacity] = {};
    unsigned g_aceQueueRead = 0;
    unsigned g_aceQueueWrite = 0;
    unsigned g_aceQueueCount = 0;
    AceEvent g_lastAce[2] = {};
    bool g_haveLastAce[2] = {};
    RecordDamageFn g_recordDamage = nullptr;
    bool g_deathEnabled = false;
    bool g_deathSetupFailed = false;
    DamageSource g_damageSource = DamageSource::None;
    volatile LONG g_expectedHost = 0;
    volatile LONG g_dropped = 0;
    volatile LONG g_damageDropped = 0;
    volatile LONG g_aceDropped = 0;
    volatile LONG g_damageSequence = 0;
    volatile LONG g_abortWorker = 0;
    volatile LONG g_writeErrorLogged = 0;
    sdgo::mission_behavior::Bundle g_missionProfileBundle;
    SdgoMissionBehaviorServerObserveV1Fn g_missionProfileObserve = nullptr;
    SdgoMissionBehaviorServerReadRowV1Fn g_missionProfileReadRow = nullptr;
    SdgoMissionBehaviorServerGetCreationV1Fn g_missionProfileCreation = nullptr;

    std::string EscapeJson(const char* text)
    {
        std::string output;
        if (!text)
            return output;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
            switch (*p) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (*p < 0x20) {
                    char escaped[8] = {};
                    sprintf_s(escaped, "\\u%04x", static_cast<unsigned>(*p));
                    output += escaped;
                } else {
                    output += static_cast<char>(*p);
                }
            }
        }
        return output;
    }

    bool IsUuid(const char* value)
    {
        if (!value || strlen(value) != 36)
            return false;
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (value[i] != '-')
                    return false;
            } else if (!((value[i] >= '0' && value[i] <= '9') ||
                         (value[i] >= 'a' && value[i] <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    bool ReadAscii(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                   char* output, size_t capacity)
    {
        if (!output || capacity == 0)
            return false;
        wchar_t wide[128] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", wide, static_cast<DWORD>(_countof(wide)), path.c_str());
        if (length == 0 || static_cast<size_t>(length) >= capacity)
            return false;
        for (DWORD i = 0; i < length; ++i) {
            if (wide[i] > 0x7f)
                return false;
            output[i] = static_cast<char>(wide[i]);
        }
        output[length] = '\0';
        return true;
    }

    enum class NumberResult { Missing, Valid, Invalid };

    NumberResult ReadNumber(
        const std::wstring& path, const wchar_t* section, const wchar_t* key, uint32_t* value)
    {
        wchar_t text[64] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", text, static_cast<DWORD>(_countof(text)), path.c_str());
        if (length == 0)
            return NumberResult::Missing;
        for (DWORD i = 0; i < length; ++i)
            if (text[i] < L'0' || text[i] > L'9')
                return NumberResult::Invalid;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(text, &end, 10);
        if (errno == ERANGE || !end || *end != L'\0')
            return NumberResult::Invalid;
        *value = static_cast<uint32_t>(parsed);
        return NumberResult::Valid;
    }

    bool ReadConfig(std::string* error)
    {
        char environmentRun[64] = {};
        char environmentRole[16] = {};
        const DWORD runLength = GetEnvironmentVariableA(
            "SDGO_GAME_TEST_RUN", environmentRun, static_cast<DWORD>(sizeof(environmentRun)));
        const DWORD roleLength = GetEnvironmentVariableA(
            "SDGO_GAME_TEST_ROLE", environmentRole, static_cast<DWORD>(sizeof(environmentRole)));
        if (runLength == 0 && roleLength == 0)
            return false;
        if (runLength == 0 || runLength >= sizeof(environmentRun) ||
            roleLength == 0 || roleLength >= sizeof(environmentRole) ||
            strcmp(environmentRole, "server") != 0 || !IsUuid(environmentRun)) {
            *error = "launcher environment is invalid";
            return false;
        }

        wchar_t executable[32768] = {};
        const DWORD executableLength = GetModuleFileNameW(
            nullptr, executable, static_cast<DWORD>(_countof(executable)));
        if (executableLength == 0 || executableLength >= _countof(executable)) {
            *error = "BattleServer image path is unavailable";
            return false;
        }
        g_directory.assign(executable, executableLength);
        const size_t slash = g_directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            *error = "BattleServer image path has no directory";
            return false;
        }
        g_directory.resize(slash);
        g_directory += L"\\sdgo\\gametest";
        g_runIni = g_directory + L"\\run.ini";
        g_expectIni = g_directory + L"\\expect.ini";
        g_statusPath = g_directory + L"\\server-observer-result.json";

        char configuredRun[64] = {};
        char configuredRole[16] = {};
        if (!ReadAscii(g_runIni, L"run", L"id", configuredRun, sizeof(configuredRun)) ||
            !ReadAscii(g_runIni, L"run", L"role", configuredRole, sizeof(configuredRole)) ||
            !ReadAscii(g_runIni, L"run", L"suite", g_config.suite, sizeof(g_config.suite)) ||
            strcmp(configuredRun, environmentRun) != 0 ||
            strcmp(configuredRole, "server") != 0 ||
            (strcmp(g_config.suite, "pvp-ai-real") != 0 &&
             strcmp(g_config.suite, "pvp-ai") != 0 &&
             strcmp(g_config.suite, "native-mission-behavior") != 0)) {
            *error = "run.ini and launcher do not describe the same supported server job";
            return false;
        }
        strcpy_s(g_config.runId, environmentRun);
        if (!ReadAscii(g_runIni, L"observer", L"nonce", g_config.nonce, sizeof(g_config.nonce))) {
            *error = "observer nonce is missing or too long";
            return false;
        }
        const size_t nonceLength = strlen(g_config.nonce);
        if (nonceLength < 3 || nonceLength > 8) {
            *error = "observer nonce must contain 3 to 8 ASCII letters or digits";
            return false;
        }
        for (size_t i = 0; i < nonceLength; ++i)
            if (!((g_config.nonce[i] >= 'a' && g_config.nonce[i] <= 'z') ||
                  (g_config.nonce[i] >= 'A' && g_config.nonce[i] <= 'Z') ||
                  (g_config.nonce[i] >= '0' && g_config.nonce[i] <= '9'))) {
                *error = "observer nonce must contain 3 to 8 ASCII letters or digits";
                return false;
            }

        uint32_t number = 0;
        NumberResult result = ReadNumber(
            g_runIni, L"observer", L"host_pilot_id", &number);
        if (result == NumberResult::Invalid) {
            *error = "observer host_pilot_id is invalid";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.hostPilotId = number;

        number = 0;
        result = ReadNumber(g_runIni, L"observer", L"mode", &number);
        if (result == NumberResult::Invalid || number > 255) {
            *error = "observer mode is invalid";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.mode = static_cast<uint8_t>(number);

        number = 1;
        result = ReadNumber(g_runIni, L"observer", L"minimum_ai", &number);
        if (result == NumberResult::Invalid || number == 0 || number > 11) {
            *error = "observer minimum_ai must be between 1 and 11";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.minimumAi = static_cast<uint8_t>(number);

        number = 0;
        result = ReadNumber(g_runIni, L"observer", L"require_end", &number);
        if (result == NumberResult::Invalid || number > 1) {
            *error = "observer require_end must be 0 or 1";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.requireEnd = number != 0;
        if (strcmp(g_config.suite, "native-mission-behavior") == 0) {
            char profile[8] = {};
            if (!ReadAscii(
                    g_runIni, L"run", L"mission_behavior_profile",
                    profile, sizeof(profile))) {
                *error = "mission behavior profile is missing";
                return false;
            }
            g_config.missionBehaviorProfile =
                strcmp(profile, "oc") == 0 ? 1u :
                strcmp(profile, "gco") == 0 ? 2u : 0u;
            number = 0;
            result = ReadNumber(
                g_runIni, L"run", L"mission_game_mode_id", &number);
            if (!g_config.missionBehaviorProfile ||
                result != NumberResult::Valid ||
                (number != 88006 && number != 88009)) {
                *error = "mission behavior selection is invalid";
                return false;
            }
            g_config.missionGameModeId = number;
        }
        return true;
    }

    uint32_t ExpectedHost()
    {
        return static_cast<uint32_t>(
            InterlockedCompareExchange(&g_expectedHost, 0, 0));
    }

    bool TitleContainsNonce(const char title[21])
    {
        const size_t nonceLength = strlen(g_config.nonce);
        for (size_t offset = 0; offset + nonceLength <= 20; ++offset) {
            bool equal = true;
            for (size_t i = 0; i < nonceLength; ++i) {
                if (title[offset + i] == '\0' || title[offset + i] != g_config.nonce[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal)
                return true;
            if (title[offset] == '\0')
                break;
        }
        return false;
    }

    template <typename T>
    bool SafeRead(const void* base, size_t offset, T* output)
    {
        if (!base || !output)
            return false;
        __try {
            *output = *reinterpret_cast<const volatile T*>(
                static_cast<const uint8_t*>(base) + offset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    int PilotSlot(const void* room, const void* pilot)
    {
        if (!room || !pilot)
            return -1;
        const uintptr_t first =
            reinterpret_cast<uintptr_t>(room) + kBattlePilots;
        const uintptr_t candidate = reinterpret_cast<uintptr_t>(pilot);
        for (int slot = 0; slot < kBattleSlots; ++slot)
            if (candidate ==
                first + static_cast<uintptr_t>(slot) * kBattlePilotStride)
                return slot;
        return -1;
    }

    uint32_t RoomSlotPilotId(const void* room, int slot)
    {
        uint32_t value = 0;
        if (slot < 0 || slot >= kBattleSlots)
            return 0;
        SafeRead(
            room,
            kSlotInfo + static_cast<size_t>(slot) * kSlotInfoStride +
                kSlotPilotId,
            &value);
        return value;
    }

    bool MatchingDamageRoom(
        const void* room, uint32_t* hostPilotId, uint8_t* mode)
    {
        if (!g_deathEnabled || !room || !hostPilotId || !mode)
            return false;
        const uint32_t expectedHost = ExpectedHost();
        uint8_t titleLength = 0;
        char title[21] = {};
        if (expectedHost == 0 ||
            !SafeRead(room, kRoomHost, hostPilotId) ||
            *hostPilotId != expectedHost ||
            !SafeRead(room, kRoomMode, mode) ||
            (g_config.mode != 0 && *mode != g_config.mode) ||
            !SafeRead(room, kRoomTitleLength, &titleLength) ||
            titleLength > 20) {
            return false;
        }
        __try {
            memcpy(
                title, static_cast<const uint8_t*>(room) + kRoomTitle,
                titleLength);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        title[titleLength] = '\0';
        return TitleContainsNonce(title);
    }

    bool CaptureDamage(
        void* room, void* victimPilot, void* attackerPilot, void* victimUnit,
        uint32_t effectiveLoss, int32_t deathFlag, DamageEvent* output)
    {
        if (!output || !victimPilot || !attackerPilot || !victimUnit ||
            effectiveLoss == 0) {
            return false;
        }
        DamageEvent event = {};
        if (!MatchingDamageRoom(
                room, &event.hostPilotId, &event.mode))
            return false;
        event.sequence = static_cast<uint32_t>(
            InterlockedIncrement(&g_damageSequence));
        LARGE_INTEGER qpc = {};
        QueryPerformanceCounter(&qpc);
        event.qpc = static_cast<uint64_t>(qpc.QuadPart);
        event.threadId = GetCurrentThreadId();
        event.roomCookie =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(room));
        event.effectiveLoss = effectiveLoss;
        event.deathFlag = deathFlag;
        event.attackerSlot = PilotSlot(room, attackerPilot);
        event.victimSlot = PilotSlot(room, victimPilot);
        if (!SafeRead(
                attackerPilot, kPilotEntity, &event.attackerPilotId) ||
            !SafeRead(
                victimPilot, kPilotEntity, &event.victimPilotId) ||
            !SafeRead(
                attackerPilot, kPilotTeam, &event.attackerTeam) ||
            !SafeRead(
                victimPilot, kPilotTeam, &event.victimTeam)) {
            return false;
        }
        event.attackerSlotPilotId =
            RoomSlotPilotId(room, event.attackerSlot);
        event.victimSlotPilotId =
            RoomSlotPilotId(room, event.victimSlot);
        event.attackerIsAi =
            (event.attackerPilotId & 0x80000000u) != 0 ? 1 : 0;
        event.victimIsAi =
            (event.victimPilotId & 0x80000000u) != 0 ? 1 : 0;
        event.samePilot =
            event.attackerPilotId != 0 &&
                    event.attackerPilotId == event.victimPilotId
                ? 1
                : 0;
        event.sameSlot =
            event.attackerSlot >= 0 &&
                    event.attackerSlot == event.victimSlot
                ? 1
                : 0;
        event.friendly =
            event.attackerTeam != 0 &&
                    event.attackerTeam == event.victimTeam
                ? 1
                : 0;
        event.identityValid =
            event.attackerSlot >= 0 && event.victimSlot >= 0 &&
                    event.attackerSlotPilotId == event.attackerPilotId &&
                    event.victimSlotPilotId == event.victimPilotId
                ? 1
                : 0;
        SafeRead(victimUnit, kUnitKey, &event.victimUnitKey);
        SafeRead(victimUnit, kUnitHp, &event.victimHp);
        SafeRead(victimUnit, kUnitMaxHp, &event.victimMaxHp);
        SafeRead(victimUnit, kUnitData, &event.victimUnitData);
        *output = event;
        return true;
    }

    void QueueDamage(const DamageEvent& event)
    {
        if (!TryEnterCriticalSection(&g_queueLock)) {
            InterlockedIncrement(&g_damageDropped);
            return;
        }
        if (g_damageQueueCount == kDamageQueueCapacity) {
            InterlockedIncrement(&g_damageDropped);
        } else {
            g_damageQueue[g_damageQueueWrite] = event;
            g_damageQueueWrite =
                (g_damageQueueWrite + 1) % kDamageQueueCapacity;
            ++g_damageQueueCount;
        }
        LeaveCriticalSection(&g_queueLock);
    }

    void QueueAce(const AceEvent& event)
    {
        if (event.scope < 1 || event.scope > 2 ||
            event.snapshot.size < sizeof(event.snapshot) ||
            event.snapshot.version != SDGO_ACE_AI_OBSERVER_VERSION ||
            event.snapshot.unit_count > 12 || !event.snapshot.room_cookie)
            return;
        if (!TryEnterCriticalSection(&g_queueLock)) {
            InterlockedIncrement(&g_aceDropped);
            return;
        }
        const unsigned index = event.scope - 1;
        if (g_haveLastAce[index] &&
            std::memcmp(
                &g_lastAce[index].snapshot, &event.snapshot,
                sizeof(event.snapshot)) == 0) {
            LeaveCriticalSection(&g_queueLock);
            return;
        }
        if (g_aceQueueCount == kAceQueueCapacity) {
            InterlockedIncrement(&g_aceDropped);
        } else {
            g_aceQueue[g_aceQueueWrite] = event;
            g_aceQueueWrite = (g_aceQueueWrite + 1) % kAceQueueCapacity;
            ++g_aceQueueCount;
            g_lastAce[index] = event;
            g_haveLastAce[index] = true;
        }
        LeaveCriticalSection(&g_queueLock);
    }

    void __cdecl ObserveAceServerSnapshot(
        const SdgoAceAiServerSnapshotV1* snapshot, void* user)
    {
        const DWORD incomingError = GetLastError();
        __try {
            const uintptr_t scope = reinterpret_cast<uintptr_t>(user);
            if (snapshot && scope >= 1 && scope <= 2 &&
                snapshot->size >= sizeof(*snapshot) &&
                snapshot->version == SDGO_ACE_AI_OBSERVER_VERSION &&
                snapshot->unit_count <= 12 && snapshot->room_cookie) {
                AceEvent event{};
                event.scope = static_cast<uint8_t>(scope);
                std::memcpy(&event.snapshot, snapshot, sizeof(event.snapshot));
                QueueAce(event);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_aceDropped);
        }
        SetLastError(incomingError);
    }

    uint32_t __fastcall ObserveRecordDamage(
        void* room, void*, void* victimPilot, void* attackerPilot,
        void* victimUnit, uint32_t effectiveLoss, int32_t deathFlag)
    {
        const DWORD incomingError = GetLastError();
        DamageEvent event = {};
        bool captured = false;
        __try {
            captured = CaptureDamage(
                room, victimPilot, attackerPilot, victimUnit,
                effectiveLoss, deathFlag, &event);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        SetLastError(incomingError);
        const uint32_t result = g_recordDamage(
            room, victimPilot, attackerPilot, victimUnit, effectiveLoss,
            deathFlag);
        const DWORD originalError = GetLastError();
        __try {
            if (captured)
                QueueDamage(event);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        SetLastError(originalError);
        return result;
    }

    void __cdecl ObserveBattleRewardsDamage(
        const SdgoBattleRewardsDamageEventV1* nativeEvent, void*)
    {
        const DWORD incomingError = GetLastError();
        __try {
            if (nativeEvent &&
                nativeEvent->size >= sizeof(*nativeEvent) &&
                nativeEvent->version ==
                    SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION) {
                DamageEvent event = {};
                if (CaptureDamage(
                        nativeEvent->room,
                        nativeEvent->victim_pilot,
                        nativeEvent->attacker_pilot,
                        nativeEvent->victim_unit,
                        nativeEvent->effective_loss,
                        nativeEvent->death_flag,
                        &event))
                    QueueDamage(event);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        SetLastError(incomingError);
    }

    void __cdecl ObservePvpAi(const SdgoPvpAiObserverEventV1* event, void*)
    {
        if (!event || event->size < sizeof(*event) ||
            event->version != SDGO_PVP_AI_OBSERVER_VERSION)
            return;
        const uint32_t expectedHost = ExpectedHost();
        if (expectedHost == 0 || event->host_pilot_id != expectedHost ||
            (g_config.mode != 0 && event->mode != g_config.mode) ||
            !TitleContainsNonce(event->title))
            return;

        EnterCriticalSection(&g_queueLock);
        if (g_queueCount == kQueueCapacity) {
            InterlockedIncrement(&g_dropped);
        } else {
            SdgoPvpAiObserverEventV1 copy = *event;
            copy.title[sizeof(copy.title) - 1] = '\0';
            g_queue[g_queueWrite] = copy;
            g_queueWrite = (g_queueWrite + 1) % kQueueCapacity;
            ++g_queueCount;
        }
        LeaveCriticalSection(&g_queueLock);
    }

    unsigned Drain(SdgoPvpAiObserverEventV1* output)
    {
        EnterCriticalSection(&g_queueLock);
        unsigned count = 0;
        while (count < kDrainBatch && g_queueCount != 0) {
            output[count++] = g_queue[g_queueRead];
            g_queueRead = (g_queueRead + 1) % kQueueCapacity;
            --g_queueCount;
        }
        LeaveCriticalSection(&g_queueLock);
        return count;
    }

    unsigned DrainDamage(DamageEvent* output)
    {
        EnterCriticalSection(&g_queueLock);
        unsigned count = 0;
        while (count < kDamageDrainBatch && g_damageQueueCount != 0) {
            output[count++] = g_damageQueue[g_damageQueueRead];
            g_damageQueueRead =
                (g_damageQueueRead + 1) % kDamageQueueCapacity;
            --g_damageQueueCount;
        }
        LeaveCriticalSection(&g_queueLock);
        return count;
    }

    unsigned DrainAce(AceEvent* output)
    {
        EnterCriticalSection(&g_queueLock);
        unsigned count = 0;
        while (count < kAceDrainBatch && g_aceQueueCount != 0) {
            output[count++] = g_aceQueue[g_aceQueueRead];
            g_aceQueueRead = (g_aceQueueRead + 1) % kAceQueueCapacity;
            --g_aceQueueCount;
        }
        LeaveCriticalSection(&g_queueLock);
        return count;
    }

    bool WriteReceipt(const std::string& line)
    {
        DWORD written = 0;
        const bool ok = g_eventFile != INVALID_HANDLE_VALUE &&
            line.size() <= MAXDWORD &&
            WriteFile(g_eventFile, line.data(), static_cast<DWORD>(line.size()),
                      &written, nullptr) != 0 &&
            written == line.size() &&
            FlushFileBuffers(g_eventFile) != 0;
        if (!ok && InterlockedCompareExchange(&g_writeErrorLogged, 1, 0) == 0)
            g_api->Log(g_mod, SDGO_LOG_ERROR, "GameTest server observer could not flush its JSONL receipt");
        return ok;
    }

    const char* EventName(uint32_t type)
    {
        switch (type) {
        case SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED: return "request_accepted";
        case SDGO_PVP_AI_EVENT_SETUP_COMPLETE: return "setup_complete";
        case SDGO_PVP_AI_EVENT_BATTLE_ACTIVE: return "battle_active";
        case SDGO_PVP_AI_EVENT_BATTLE_END: return "battle_end";
        case SDGO_PVP_AI_EVENT_SETUP_FAILED: return "setup_failed";
        default: return "unknown";
        }
    }

    std::string OcRoomJson(const char* title);

    std::string EventJson(const SdgoPvpAiObserverEventV1& event)
    {
        std::string line =
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" + EscapeJson(g_config.suite) +
            "\",\"event\":\"" + EventName(event.type) +
            "\",\"status_code\":" + std::to_string(event.status) +
            ",\"sequence\":" + std::to_string(event.sequence) +
            ",\"qpc\":" + std::to_string(event.qpc) +
            ",\"thread_id\":" + std::to_string(event.thread_id) +
            ",\"room_cookie\":" + std::to_string(event.room_cookie) +
            ",\"host_pilot_id\":" + std::to_string(event.host_pilot_id) +
            ",\"room_type\":" + std::to_string(event.room_type) +
            ",\"mode\":" + std::to_string(event.mode) +
            ",\"map_selection\":" + std::to_string(event.map_selection) +
            ",\"map_number\":" + std::to_string(event.map_number) +
            ",\"manual_map_valid\":" + std::to_string(event.manual_map_valid) +
            ",\"title\":\"" + EscapeJson(event.title) +
            "\",\"humans_team_1\":" + std::to_string(event.humans_team_1) +
            ",\"humans_team_2\":" + std::to_string(event.humans_team_2) +
            ",\"ai_team_1\":" + std::to_string(event.ai_team_1) +
            ",\"ai_team_2\":" + std::to_string(event.ai_team_2) +
            ",\"active_slots\":" + std::to_string(event.active_slots) +
            ",\"score_team_1\":" + std::to_string(event.score_team_1) +
            ",\"score_team_2\":" + std::to_string(event.score_team_2) +
            ",\"slots\":[";
        bool first = true;
        for (const auto& slot : event.slots) {
            if (slot.pilot_id == 0)
                continue;
            if (!first)
                line += ',';
            first = false;
            line += "{\"slot\":" + std::to_string(slot.slot) +
                ",\"pilot_id\":" + std::to_string(slot.pilot_id) +
                ",\"unit_or_npc_id\":" + std::to_string(slot.unit_or_npc_id) +
                ",\"team\":" + std::to_string(slot.team) +
                ",\"colour\":" + std::to_string(slot.colour) +
                ",\"is_ai\":" + std::string(slot.is_ai ? "true" : "false") + '}';
        }
        line += "]";
        if (event.type == SDGO_PVP_AI_EVENT_BATTLE_END)
            line += ",\"oc_room\":" + OcRoomJson(event.title);
        line += "}\n";
        return line;
    }

    // The OC Battle Mod's room-system decisions for this room's units (room
    // mode only; an empty list when the Mod is absent or decided nothing).
    std::string OcRoomJson(const char* title)
    {
        const HMODULE library = GetModuleHandleW(L"oc_custom_battle.dll");
        const auto recent = library ? reinterpret_cast<SdgoOcCustomBattleRecentDecisionsV1Fn>(
            GetProcAddress(library, SDGO_OC_CUSTOM_BATTLE_DECISIONS_EXPORT)) : nullptr;
        std::string json = "[";
        if (!recent)
            return json + "]";
        SdgoOcCustomBattleDecisionV1 decisions[32] = {};
        const uint32_t count = recent(decisions, 32);
        bool first = true;
        for (uint32_t i = 0; i < count && i < 32; ++i) {
            const auto& d = decisions[i];
            if (d.size != sizeof(d) || strncmp(d.title, title, sizeof(d.title) - 1) != 0)
                continue;
            if (!first)
                json += ',';
            first = false;
            json += "{\"sequence\":" + std::to_string(d.sequence) +
                ",\"room_key\":" + std::to_string(d.room_key) +
                ",\"uid\":" + std::to_string(d.uid) +
                ",\"system\":" + std::to_string(d.system) +
                ",\"form\":" + std::to_string(d.form) +
                ",\"oc_applied\":" + std::to_string(d.oc_applied) +
                ",\"g1_profile\":" + std::to_string(d.g1_profile) + ",\"native_skills\":[";
            for (int s = 0; s < 4; ++s)
                json += (s ? "," : "") + std::to_string(d.native_skills[s]);
            json += "],\"battle_skills\":[";
            for (int s = 0; s < 4; ++s)
                json += (s ? "," : "") + std::to_string(d.battle_skills[s]);
            json += "]}";
        }
        return json + "]";
    }

    std::string AceJson(const AceEvent& event)
    {
        const char* scope = event.scope == 1 ? "pvp" : "mission";
        const std::string message =
            sdgo_game_test_ace::Server(scope, event.snapshot);
        return
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" +
            EscapeJson(g_config.suite) +
            "\",\"event\":\"observation\",\"pid\":" +
            std::to_string(GetCurrentProcessId()) +
            ",\"nonce\":\"" + EscapeJson(g_config.nonce) +
            "\",\"room_cookie\":" +
            std::to_string(event.snapshot.room_cookie) +
            ",\"message\":\"" + EscapeJson(message.c_str()) +
            "\"}\n";
    }

    std::string DamageJson(const DamageEvent& event)
    {
        const char* source =
            g_damageSource == DamageSource::BattleRewardsBridge
                ? "battle_rewards_bridge"
                : "direct_native_hook";
        return
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" +
            EscapeJson(g_config.suite) +
            "\",\"event\":\"native_effective_hp_loss\"" +
            ",\"source\":\"" + source + "\"" +
            ",\"sequence\":" + std::to_string(event.sequence) +
            ",\"qpc\":" + std::to_string(event.qpc) +
            ",\"thread_id\":" + std::to_string(event.threadId) +
            ",\"room_cookie\":" + std::to_string(event.roomCookie) +
            ",\"host_pilot_id\":" +
            std::to_string(event.hostPilotId) +
            ",\"mode\":" + std::to_string(event.mode) +
            ",\"attacker_slot\":" +
            std::to_string(event.attackerSlot) +
            ",\"victim_slot\":" + std::to_string(event.victimSlot) +
            ",\"attacker_pilot_id\":" +
            std::to_string(event.attackerPilotId) +
            ",\"victim_pilot_id\":" +
            std::to_string(event.victimPilotId) +
            ",\"attacker_slot_pilot_id\":" +
            std::to_string(event.attackerSlotPilotId) +
            ",\"victim_slot_pilot_id\":" +
            std::to_string(event.victimSlotPilotId) +
            ",\"attacker_team\":" +
            std::to_string(event.attackerTeam) +
            ",\"victim_team\":" + std::to_string(event.victimTeam) +
            ",\"attacker_is_ai\":" +
            std::string(event.attackerIsAi ? "true" : "false") +
            ",\"victim_is_ai\":" +
            std::string(event.victimIsAi ? "true" : "false") +
            ",\"same_pilot\":" +
            std::string(event.samePilot ? "true" : "false") +
            ",\"same_slot\":" +
            std::string(event.sameSlot ? "true" : "false") +
            ",\"friendly\":" +
            std::string(event.friendly ? "true" : "false") +
            ",\"identity_valid\":" +
            std::string(event.identityValid ? "true" : "false") +
            ",\"effective_loss\":" +
            std::to_string(event.effectiveLoss) +
            ",\"death_flag\":" + std::to_string(event.deathFlag) +
            ",\"victim_hp_at_hook\":" +
            std::to_string(event.victimHp) +
            ",\"victim_max_hp\":" +
            std::to_string(event.victimMaxHp) +
            ",\"victim_unit_key\":" +
            std::to_string(event.victimUnitKey) +
            ",\"victim_unit_data\":" +
            std::to_string(
                static_cast<unsigned long long>(event.victimUnitData)) +
            "}\n";
    }

    const char* OutcomeName(Lifecycle::Outcome outcome)
    {
        switch (outcome) {
        case Lifecycle::Pass: return "pass";
        case Lifecycle::Fail: return "fail";
        default: return "running";
        }
    }

    const char* DamageSourceName(DamageSource source)
    {
        switch (source) {
        case DamageSource::BattleRewardsBridge:
            return "battle_rewards_bridge";
        case DamageSource::DirectHook:
            return "direct_native_hook";
        default:
            return "disabled";
        }
    }

    std::string ObserverReadyJson(uint32_t processId, const char* hostKey)
    {
        return
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" +
            EscapeJson(g_config.suite) +
            "\",\"event\":\"observer_ready\",\"status\":\"running\"" +
            ",\"normal_server_main\":true,\"pid\":" +
            std::to_string(processId) +
            ",\"host_key\":\"" + EscapeJson(hostKey) +
            "\",\"nonce\":\"" + EscapeJson(g_config.nonce) +
            "\",\"expected_host_pilot_id\":" +
            std::to_string(ExpectedHost()) +
            ",\"mode\":" + std::to_string(g_config.mode) +
            ",\"minimum_ai\":" +
            std::to_string(g_config.minimumAi) +
            ",\"require_end\":" +
            (g_config.requireEnd ? "true" : "false") +
            ",\"battle_ai_deaths\":" +
            (g_deathEnabled ? "true" : "false") +
            ",\"death_source\":\"" +
            std::string(DamageSourceName(g_damageSource)) +
            "\"}\n";
    }

    bool WriteStatus(const Lifecycle& lifecycle)
    {
        const std::wstring temporary = g_statusPath + L".tmp";
        const std::string body =
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" + EscapeJson(g_config.suite) +
            "\",\"status\":\"" + OutcomeName(lifecycle.outcome) +
            "\",\"reason\":\"" + EscapeJson(lifecycle.reason.c_str()) +
            "\",\"normal_server_main\":true,\"expected_host_pilot_id\":" +
            std::to_string(ExpectedHost()) +
            ",\"room_cookie\":" + std::to_string(lifecycle.roomCookie) +
            ",\"mode\":" + std::to_string(lifecycle.mode) +
            ",\"phase\":" + std::to_string(lifecycle.phase) +
            ",\"events\":" + std::to_string(lifecycle.eventCount) +
            ",\"dropped\":" +
            std::to_string(static_cast<unsigned long>(
                InterlockedCompareExchange(&g_dropped, 0, 0))) +
             ",\"damage_dropped\":" +
             std::to_string(static_cast<unsigned long>(
                 InterlockedCompareExchange(&g_damageDropped, 0, 0))) +
             ",\"ace_dropped\":" +
             std::to_string(static_cast<unsigned long>(
                 InterlockedCompareExchange(&g_aceDropped, 0, 0))) +
            "}\n";
        HANDLE file = CreateFileW(
            temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const bool wrote = body.size() <= MAXDWORD &&
            WriteFile(file, body.data(), static_cast<DWORD>(body.size()),
                      &written, nullptr) != 0 &&
            written == body.size() &&
            FlushFileBuffers(file) != 0;
        const bool closed = CloseHandle(file) != 0;
        if (!wrote || !closed)
            return false;
        return MoveFileExW(
            temporary.c_str(), g_statusPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    void Fail(Lifecycle* lifecycle, const char* reason)
    {
        if (lifecycle->outcome == Lifecycle::Fail)
            return;
        lifecycle->outcome = Lifecycle::Fail;
        lifecycle->reason = reason;
    }

    bool SameBattle(const Lifecycle& lifecycle, const SdgoPvpAiObserverEventV1& event)
    {
        return lifecycle.roomCookie == event.room_cookie &&
            lifecycle.hostPilotId == event.host_pilot_id &&
            lifecycle.mode == event.mode;
    }

    void ProcessEvent(Lifecycle* lifecycle, const SdgoPvpAiObserverEventV1& event)
    {
        ++lifecycle->eventCount;
        if (lifecycle->outcome == Lifecycle::Fail)
            return;
        if (lifecycle->outcome == Lifecycle::Pass) {
            if (event.type == SDGO_PVP_AI_EVENT_BATTLE_END &&
                lifecycle->phase == 3 && SameBattle(*lifecycle, event)) {
                lifecycle->phase = 4;
                lifecycle->reason =
                    "real battle completed after verified PvP AI activity";
            }
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_SETUP_FAILED) {
            lifecycle->roomCookie = event.room_cookie;
            lifecycle->hostPilotId = event.host_pilot_id;
            lifecycle->mode = event.mode;
            Fail(lifecycle, "PvP AI setup raised a native exception");
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED) {
            if (lifecycle->phase == 0) {
                lifecycle->roomCookie = event.room_cookie;
                lifecycle->hostPilotId = event.host_pilot_id;
                lifecycle->mode = event.mode;
                lifecycle->phase = 1;
                lifecycle->reason = "matching real battle request accepted";
            } else if (!SameBattle(*lifecycle, event)) {
                Fail(lifecycle, "multiple matching battle rooms made the receipt ambiguous");
            }
            return;
        }
        if (lifecycle->phase == 0 || !SameBattle(*lifecycle, event)) {
            Fail(lifecycle, "observer lifecycle event arrived without its matching request");
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_SETUP_COMPLETE) {
            if (lifecycle->phase != 1) {
                Fail(lifecycle, "PvP AI setup completed out of order");
                return;
            }
            const unsigned aiCount =
                static_cast<unsigned>(event.ai_team_1) + event.ai_team_2;
            if (aiCount < g_config.minimumAi) {
                Fail(lifecycle, "PvP AI setup produced fewer AI pilots than required");
                return;
            }
            lifecycle->phase = 2;
            lifecycle->reason = "matching real room contains the required AI pilots";
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_BATTLE_ACTIVE) {
            if (lifecycle->phase != 2) {
                Fail(lifecycle, "battle became active before verified PvP AI setup");
                return;
            }
            lifecycle->phase = 3;
            lifecycle->reason = "real battle tick observed with AI pilots active";
            if (!g_config.requireEnd)
                lifecycle->outcome = Lifecycle::Pass;
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_BATTLE_END) {
            if (lifecycle->phase != 3) {
                Fail(lifecycle, "battle end arrived before an active battle tick");
                return;
            }
            lifecycle->phase = 4;
            lifecycle->reason = "real battle completed after verified PvP AI activity";
            lifecycle->outcome = Lifecycle::Pass;
        }
    }

    bool RefreshExpectedHost(uint32_t* host, std::string* error)
    {
        uint32_t selected = g_config.hostPilotId;
        uint32_t fromFile = 0;
        const NumberResult result = ReadNumber(
            g_expectIni, L"observer", L"host_pilot_id", &fromFile);
        if (result == NumberResult::Invalid) {
            *error = "expect.ini observer host_pilot_id is invalid";
            return false;
        }
        if (result == NumberResult::Valid)
            selected = fromFile;
        if (*host != selected) {
            *host = selected;
            InterlockedExchange(&g_expectedHost, static_cast<LONG>(selected));
        }
        return true;
    }

    bool BindMissionProfileObserver(std::string* error)
    {
        HMODULE module = GetModuleHandleW(L"mission_behavior_server.dll");
        if (!module) {
            *error = "mission_behavior_server is not loaded";
            return false;
        }
        g_missionProfileObserve =
            reinterpret_cast<SdgoMissionBehaviorServerObserveV1Fn>(
                GetProcAddress(
                    module,
                    SDGO_MISSION_BEHAVIOR_SERVER_SNAPSHOT_EXPORT));
        g_missionProfileReadRow =
            reinterpret_cast<SdgoMissionBehaviorServerReadRowV1Fn>(
                GetProcAddress(
                    module,
                    SDGO_MISSION_BEHAVIOR_SERVER_READ_ROW_EXPORT));
        g_missionProfileCreation =
            reinterpret_cast<SdgoMissionBehaviorServerGetCreationV1Fn>(
                GetProcAddress(
                    module,
                    SDGO_MISSION_BEHAVIOR_SERVER_CREATION_EXPORT));
        if (!g_missionProfileObserve ||
            !g_missionProfileReadRow ||
            !g_missionProfileCreation) {
            *error = "mission behavior server observer ABI is incomplete";
            return false;
        }
        wchar_t modulePath[32768] = {};
        const DWORD length = GetModuleFileNameW(
            module,
            modulePath,
            static_cast<DWORD>(_countof(modulePath)));
        if (!length || length >= _countof(modulePath)) {
            *error = "mission behavior server path is unavailable";
            return false;
        }
        std::string profileError;
        const std::filesystem::path profilePath =
            std::filesystem::path(modulePath).parent_path() / L"profile.bin";
        if (!sdgo::mission_behavior::LoadProfileFile(
                profilePath,
                g_missionProfileBundle,
                profileError)) {
            *error = "mission behavior profile data is invalid: " + profileError;
            return false;
        }
        return true;
    }

    bool VerifyMissionProfile(
        const SdgoMissionBehaviorServerSnapshotV1& status,
        uint32_t* rowCrc,
        uint32_t* selectedMissionRows,
        std::string* error)
    {
        using namespace sdgo::mission_behavior;
        const uint32_t expectedState =
            g_config.missionBehaviorProfile == 1 ? 1u : 2u;
        if (status.profile != g_config.missionBehaviorProfile ||
            status.state != expectedState ||
            status.entries != g_missionProfileBundle.entries.size() ||
            status.payload_crc32 !=
                g_missionProfileBundle.header.payload_crc32 ||
            !status.ready_epochs ||
            status.conflict_id) {
            *error = "mission behavior server status does not match the sealed profile";
            return false;
        }
        if ((expectedState == 1 &&
             status.applied != g_missionProfileBundle.entries.size()) ||
            (expectedState == 2 && status.applied != 0) ||
            status.baseline_matches != g_missionProfileBundle.entries.size()) {
            *error = "mission behavior server did not accept every profile row";
            return false;
        }

        std::vector<NpcRow> rows;
        rows.reserve(g_missionProfileBundle.entries.size());
        uint32_t selected = 0;
        for (const auto& entry : g_missionProfileBundle.entries) {
            SdgoMissionBehaviorServerRowV1 observed{};
            if (!g_missionProfileReadRow(
                    entry.baseline.id,
                    &observed,
                    sizeof(observed))) {
                *error = "mission behavior server row readback is incomplete";
                return false;
            }
            static_assert(
                sizeof(observed) == sizeof(NpcRow),
                "mission behavior row observer layout differs");
            NpcRow native{};
            std::memcpy(&native, &observed, sizeof(native));
            const NpcRow& expected =
                expectedState == 1 ? entry.oc : entry.baseline;
            if (!SameRow(native, expected)) {
                *error = "mission behavior server row readback differs";
                return false;
            }
            rows.push_back(native);
            if (native.mission == g_config.missionGameModeId)
                ++selected;
        }
        const uint32_t crc = Crc32(
            reinterpret_cast<const uint8_t*>(rows.data()),
            rows.size() * sizeof(NpcRow));
        if (crc != status.native_rows_crc32) {
            *error = "mission behavior server aggregate row receipt differs";
            return false;
        }
        *rowCrc = crc;
        *selectedMissionRows = selected;
        return true;
    }

    std::string MissionProfileJson(
        const SdgoMissionBehaviorServerSnapshotV1& status,
        uint32_t rowCrc,
        uint32_t selectedMissionRows)
    {
        return
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" +
            EscapeJson(g_config.suite) +
            "\",\"event\":\"mission_profile_ready\"" +
            ",\"profile\":" + std::to_string(status.profile) +
            ",\"state\":" + std::to_string(status.state) +
            ",\"entries\":" + std::to_string(status.entries) +
            ",\"applied\":" + std::to_string(status.applied) +
            ",\"baseline_matches\":" +
            std::to_string(status.baseline_matches) +
            ",\"payload_crc32\":" +
            std::to_string(status.payload_crc32) +
            ",\"native_rows_crc32\":" +
            std::to_string(status.native_rows_crc32) +
            ",\"verified_rows\":" +
            std::to_string(g_missionProfileBundle.entries.size()) +
            ",\"verified_rows_crc32\":" + std::to_string(rowCrc) +
            ",\"selected_mission_rows\":" +
            std::to_string(selectedMissionRows) +
            ",\"ready_epochs\":" + std::to_string(status.ready_epochs) +
            "}\n";
    }

    std::string MissionCreationJson(
        const SdgoMissionBehaviorServerCreationV1& receipt)
    {
        return
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" +
            EscapeJson(g_config.suite) +
            "\",\"event\":\"mission_profile_creation\"" +
            ",\"sequence\":" + std::to_string(receipt.sequence) +
            ",\"profile\":" + std::to_string(receipt.profile) +
            ",\"original_result\":" +
            std::to_string(receipt.original_result) +
            ",\"tracked\":" + std::to_string(receipt.tracked) +
            ",\"profile_match\":" +
            std::to_string(receipt.profile_match) +
            ",\"mission_npc_id\":" +
            std::to_string(receipt.mission_npc_id) +
            ",\"mission_id\":" + std::to_string(receipt.mission_id) +
            ",\"difficulty\":" + std::to_string(receipt.difficulty) +
            ",\"part\":" + std::to_string(receipt.part) +
            ",\"npc_id\":" + std::to_string(receipt.npc_id) +
            ",\"native_pilot_id\":" +
            std::to_string(receipt.native_pilot_id) +
            ",\"native_unit_hp\":" +
            std::to_string(receipt.native_unit_hp) +
            "}\n";
    }

    DWORD WINAPI MissionWorker(void*)
    {
        WaitForSingleObject(g_startGate, INFINITE);
        if (InterlockedCompareExchange(&g_abortWorker, 0, 0) != 0) {
            CloseHandle(g_eventFile);
            g_eventFile = INVALID_HANDLE_VALUE;
            return 0;
        }

        Lifecycle lifecycle;
        lifecycle.reason = "waiting for mission behavior tables";
        bool readyPublished = false;
        uint64_t lastCreation = 0;
        for (;;) {
            bool statusChanged = false;
            if (!readyPublished) {
                SdgoMissionBehaviorServerSnapshotV1 status{};
                status.size = sizeof(status);
                status.version = 1;
                if (!g_missionProfileObserve(
                        &status,
                        sizeof(status))) {
                    Fail(&lifecycle, "mission behavior server status is unreadable");
                    statusChanged = true;
                } else if (status.state == 3 || status.state == 4) {
                    Fail(
                        &lifecycle,
                        status.state == 4
                            ? "mission behavior server rollback failed"
                            : "mission behavior server rejected its profile");
                    statusChanged = true;
                } else if (status.state == 1 || status.state == 2) {
                    uint32_t rowCrc = 0;
                    uint32_t selectedMissionRows = 0;
                    std::string error;
                    if (!VerifyMissionProfile(
                            status,
                            &rowCrc,
                            &selectedMissionRows,
                            &error)) {
                        Fail(&lifecycle, error.c_str());
                        statusChanged = true;
                    } else if (!WriteReceipt(
                                   MissionProfileJson(
                                       status,
                                       rowCrc,
                                       selectedMissionRows)) ||
                               !WriteReceipt(
                                   ObserverReadyJson(
                                       GetCurrentProcessId(),
                                       g_api->host_key))) {
                        Fail(
                            &lifecycle,
                            "mission profile readiness receipt could not be flushed");
                        statusChanged = true;
                    } else {
                        readyPublished = true;
                        lifecycle.reason =
                            "mission profile ready; waiting for native row consumption";
                        statusChanged = true;
                    }
                }
            } else {
                SdgoMissionBehaviorServerCreationV1 creation{};
                creation.size = sizeof(creation);
                creation.version = 1;
                if (g_missionProfileCreation(
                        &creation,
                        sizeof(creation)) &&
                    creation.sequence > lastCreation) {
                    lastCreation = creation.sequence;
                    if (creation.tracked &&
                        creation.mission_id ==
                            g_config.missionGameModeId) {
                        if (!WriteReceipt(MissionCreationJson(creation))) {
                            Fail(
                                &lifecycle,
                                "mission creation receipt could not be flushed");
                        } else if (!creation.original_result ||
                                   !creation.profile_match ||
                                   creation.profile !=
                                       g_config.missionBehaviorProfile) {
                            Fail(
                                &lifecycle,
                                "mission creation did not consume the sealed profile");
                        } else {
                            lifecycle.reason =
                                "native mission creation consumed the sealed profile";
                        }
                        statusChanged = true;
                    }
                }
            }
            if (statusChanged && !WriteStatus(lifecycle))
                g_api->Log(
                    g_mod,
                    SDGO_LOG_ERROR,
                    "GameTest server observer could not update mission profile status");
            Sleep(25);
        }
    }

    DWORD WINAPI Worker(void*)
    {
        WaitForSingleObject(g_startGate, INFINITE);
        if (InterlockedCompareExchange(&g_abortWorker, 0, 0) != 0) {
            CloseHandle(g_eventFile);
            g_eventFile = INVALID_HANDLE_VALUE;
            return 0;
        }

        Lifecycle lifecycle;
        uint32_t selectedHost = UINT32_MAX;
        std::string hostError;
        if (!RefreshExpectedHost(&selectedHost, &hostError))
            Fail(&lifecycle, hostError.c_str());
        else if (selectedHost != 0)
            lifecycle.reason = "waiting for matching room request";

        const std::string ready = ObserverReadyJson(
            GetCurrentProcessId(), g_api->host_key);
        if (!WriteReceipt(ready))
            Fail(&lifecycle, "observer_ready receipt could not be flushed");
        if (g_deathSetupFailed)
            Fail(
                &lifecycle,
                "native damage observer could not register with battle_rewards");
        if (!WriteStatus(lifecycle))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "GameTest server observer could not publish its status file");

        ULONGLONG nextHostRefresh = 0;
        for (;;) {
            bool statusChanged = false;
            const ULONGLONG now = GetTickCount64();
            if (now >= nextHostRefresh) {
                nextHostRefresh = now + 250;
                const uint32_t previousHost = selectedHost;
                hostError.clear();
                if (!RefreshExpectedHost(&selectedHost, &hostError)) {
                    Fail(&lifecycle, hostError.c_str());
                    statusChanged = true;
                } else if (selectedHost != previousHost) {
                    if (lifecycle.phase != 0)
                        Fail(&lifecycle, "expected host changed after battle correlation began");
                    else
                        lifecycle.reason = selectedHost == 0
                            ? "waiting for authenticated host"
                            : "waiting for matching room request";
                    statusChanged = true;
                }
            }

            if (InterlockedCompareExchange(&g_dropped, 0, 0) != 0 &&
                lifecycle.outcome != Lifecycle::Fail) {
                Fail(&lifecycle, "observer queue overflowed; lifecycle evidence was lost");
                statusChanged = true;
            }
            if (InterlockedCompareExchange(&g_damageDropped, 0, 0) != 0 &&
                lifecycle.outcome != Lifecycle::Fail) {
                Fail(
                    &lifecycle,
                    "native damage queue overflowed; death evidence was lost");
                statusChanged = true;
            }
            if (InterlockedCompareExchange(&g_aceDropped, 0, 0) != 0 &&
                lifecycle.outcome != Lifecycle::Fail) {
                Fail(
                    &lifecycle,
                    "Ace snapshot queue overflowed; native evidence was lost");
                statusChanged = true;
            }

            SdgoPvpAiObserverEventV1 batch[kDrainBatch] = {};
            unsigned count = 0;
            while ((count = Drain(batch)) != 0) {
                for (unsigned i = 0; i < count; ++i) {
                    if (!WriteReceipt(EventJson(batch[i]))) {
                        Fail(&lifecycle, "observer event receipt could not be flushed");
                        statusChanged = true;
                        continue;
                    }
                    const Lifecycle before = lifecycle;
                    ProcessEvent(&lifecycle, batch[i]);
                    if (before.outcome != lifecycle.outcome ||
                        before.phase != lifecycle.phase ||
                        before.eventCount != lifecycle.eventCount ||
                        before.reason != lifecycle.reason)
                        statusChanged = true;
                }
            }
            DamageEvent damageBatch[kDamageDrainBatch] = {};
            while ((count = DrainDamage(damageBatch)) != 0) {
                for (unsigned i = 0; i < count; ++i) {
                    if (!WriteReceipt(DamageJson(damageBatch[i]))) {
                        Fail(
                            &lifecycle,
                            "native damage receipt could not be flushed");
                        statusChanged = true;
                    }
                }
            }
            AceEvent aceBatch[kAceDrainBatch] = {};
            while ((count = DrainAce(aceBatch)) != 0) {
                for (unsigned i = 0; i < count; ++i) {
                    if (!WriteReceipt(AceJson(aceBatch[i]))) {
                        Fail(
                            &lifecycle,
                            "Ace snapshot receipt could not be flushed");
                        statusChanged = true;
                    }
                }
            }
            if (statusChanged && !WriteStatus(lifecycle))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "GameTest server observer could not update its status file");
            Sleep(25);
        }
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;

    std::string error;
    if (!ReadConfig(&error)) {
        if (!error.empty())
            api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: %s", error.c_str());
        return 0;
    }
    const bool missionBehaviorRun =
        strcmp(g_config.suite, "native-mission-behavior") == 0;
    if (missionBehaviorRun && !BindMissionProfileObserver(&error)) {
        api->Log(
            mod,
            SDGO_LOG_ERROR,
            "GameTest server observer inactive: %s",
            error.c_str());
        return 0;
    }
    const int deathSetting =
        api->SettingInt(mod, "BattleAiDeaths", 0);
    if (deathSetting != 0 && deathSetting != 1) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "GameTest server observer inactive: BattleAiDeaths must be 0 or 1");
        return 0;
    }
    g_deathEnabled = !missionBehaviorRun && deathSetting != 0;

    HMODULE pvpAi =
        missionBehaviorRun ? nullptr : GetModuleHandleW(L"pvp_ai.dll");
    auto registerObserver = pvpAi
        ? reinterpret_cast<SdgoPvpAiRegisterObserverV1Fn>(
            GetProcAddress(pvpAi, SDGO_PVP_AI_OBSERVER_EXPORT))
        : nullptr;
    if (!missionBehaviorRun && !registerObserver) {
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: pvp_ai observer ABI unavailable");
        return 0;
    }

    InitializeCriticalSection(&g_queueLock);
    const std::wstring eventPath = g_directory + L"\\server-observer-events.jsonl";
    g_eventFile = CreateFileW(
        eventPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_eventFile == INVALID_HANDLE_VALUE) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "GameTest server observer inactive: event receipt already exists or cannot be created");
        DeleteCriticalSection(&g_queueLock);
        return 0;
    }
    g_startGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_startGate) {
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: worker gate creation failed");
        CloseHandle(g_eventFile);
        g_eventFile = INVALID_HANDLE_VALUE;
        DeleteCriticalSection(&g_queueLock);
        return 0;
    }
    HANDLE worker = CreateThread(
        nullptr,
        0,
        missionBehaviorRun ? &MissionWorker : &Worker,
        nullptr,
        0,
        nullptr);
    if (!worker) {
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: worker creation failed");
        CloseHandle(g_startGate);
        g_startGate = nullptr;
        CloseHandle(g_eventFile);
        g_eventFile = INVALID_HANDLE_VALUE;
        DeleteCriticalSection(&g_queueLock);
        return 0;
    }

    HMODULE battleRewards =
        g_deathEnabled
            ? GetModuleHandleW(L"battle_rewards.dll")
            : nullptr;
    const auto registerDamageObserver =
        battleRewards
            ? reinterpret_cast<
                  SdgoBattleRewardsRegisterDamageObserverV1Fn>(
                  GetProcAddress(
                      battleRewards,
                      SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_EXPORT))
            : nullptr;
    if (g_deathEnabled && !battleRewards &&
        !api->HookCode(
            mod, "battle.rewards.record_damage",
            reinterpret_cast<void*>(&ObserveRecordDamage),
            reinterpret_cast<void**>(&g_recordDamage))) {
        InterlockedExchange(&g_abortWorker, 1);
        SetEvent(g_startGate);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
        CloseHandle(g_startGate);
        g_startGate = nullptr;
        if (g_eventFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_eventFile);
            g_eventFile = INVALID_HANDLE_VALUE;
        }
        DeleteCriticalSection(&g_queueLock);
        api->Log(
            mod, SDGO_LOG_ERROR,
            "GameTest server observer inactive: native HP-loss hook unavailable");
        return 0;
    }
    if (g_deathEnabled && !battleRewards)
        g_damageSource = DamageSource::DirectHook;

    const SdgoPvpAiObserverSinkV1 sink = {
        sizeof(sink), SDGO_PVP_AI_OBSERVER_VERSION, &ObservePvpAi, nullptr};
    if (!missionBehaviorRun && !registerObserver(&sink)) {
        InterlockedExchange(&g_abortWorker, 1);
        SetEvent(g_startGate);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
        CloseHandle(g_startGate);
        g_startGate = nullptr;
        if (g_eventFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_eventFile);
            g_eventFile = INVALID_HANDLE_VALUE;
        }
        DeleteCriticalSection(&g_queueLock);
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: pvp_ai rejected registration");
        return 0;
    }

    if (!missionBehaviorRun && g_deathEnabled && battleRewards) {
        const SdgoBattleRewardsDamageObserverSinkV1 damageSink = {
            sizeof(damageSink),
            SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION,
            &ObserveBattleRewardsDamage,
            nullptr};
        if (!registerDamageObserver ||
            !registerDamageObserver(&damageSink)) {
            // pvp_ai registration is process-lifetime, so remain loaded and
            // publish a fail-closed run status instead of unloading a callback.
            g_deathSetupFailed = true;
            api->Log(
                mod, SDGO_LOG_ERROR,
                "GameTest server observer: battle_rewards damage observer registration failed");
        } else {
            g_damageSource = DamageSource::BattleRewardsBridge;
        }
    }

    if (!missionBehaviorRun) {
        const auto registerAceProvider = [&](HMODULE provider,
                                             const char* exportName,
                                             uintptr_t scope) {
            const auto registerObserver = provider
                ? reinterpret_cast<SdgoAceAiRegisterServerObserverV1Fn>(
                      GetProcAddress(provider, exportName))
                : nullptr;
            if (!registerObserver)
                return;
            const SdgoAceAiServerObserverSinkV1 aceSink = {
                sizeof(aceSink), SDGO_ACE_AI_OBSERVER_VERSION,
                &ObserveAceServerSnapshot, reinterpret_cast<void*>(scope)};
            if (!registerObserver(&aceSink))
                api->Log(
                    mod, SDGO_LOG_ERROR,
                    "GameTest server observer: Ace snapshot provider rejected registration");
        };
        registerAceProvider(
            GetModuleHandleW(L"pvp_ai.dll"),
            SDGO_ACE_AI_PVP_SERVER_OBSERVER_EXPORT, 1);
        registerAceProvider(
            GetModuleHandleW(L"coop_ai.dll"),
            SDGO_ACE_AI_MISSION_SERVER_OBSERVER_EXPORT, 2);
    }

    SetEvent(g_startGate);
    CloseHandle(worker);
    if (missionBehaviorRun) {
        api->Log(
            mod,
            SDGO_LOG_INFO,
            "ready: passive GameTest observer verifies mission profile rows and native consumption");
    } else {
        api->Log(
            mod,
            SDGO_LOG_INFO,
            "ready: passive GameTest observer follows normal BattleServer startup and real PvP AI rooms%s",
            g_deathEnabled && !g_deathSetupFailed
                ? " with native HP-loss receipts"
                : "");
    }
    return 1;
}
