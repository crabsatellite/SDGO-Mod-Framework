#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/login_showcase_api.h"
#include "sdgo/sdgo_api.h"

#include "selection.h"

namespace
{
    using namespace sdgo::login_showcase;
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn =
        int(__thiscall*)(void* grid, const uint8_t** record, uint32_t id, int type, char log);

    constexpr uint32_t kFirstUnitIndex = 0x375BE1;
    constexpr uint32_t kMaximumUnitIndexes = 20000;
    constexpr int kUnitType = 2;
    constexpr int kUnitIndexType = 0x55;
    constexpr size_t kUnitIndexEnabled = 5;
    constexpr size_t kUnitIndexUnit = 6;
    constexpr size_t kUnitRank = 308;
    constexpr size_t kUnitSubgrade = 309;

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;
    Config g_config;
    Rotation g_rotation;
    SRWLOCK g_lock = SRWLOCK_INIT;
    bool g_poolBuilt = false;
    uint32_t g_randomState = 0;
    SdgoRegisterLoginShowcaseChooserV1Fn g_register = nullptr;

    uint32_t Read32(const uint8_t* bytes)
    {
        uint32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }

    bool ReadSetting(const char* key, const char* fallback, std::string& value)
    {
        std::vector<char> buffer(32768);
        const uint32_t length =
            g_api->SettingString(g_mod, key, fallback, buffer.data(),
                                 static_cast<uint32_t>(buffer.size()));
        value.assign(buffer.data());
        return length == value.size() && value.size() + 1 < buffer.size();
    }

    const char* ModeName(Mode mode)
    {
        switch (mode) {
        case Mode::Grades: return "grades";
        case Mode::All: return "all";
        case Mode::Recent: return "recent";
        case Mode::Units: return "units";
        }
        return "unknown";
    }

    bool LoadConfig()
    {
        std::string mode;
        if (!ReadSetting("Mode", "grades", mode))
            return false;
        if (!ParseMode(mode,g_config.mode)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "invalid Mode; use grades, all, recent or units");
            return false;
        }

        std::string grades;
        if (!ReadSetting("Grades", "SS,SR,SU", grades) ||
            !ParseGrades(grades, g_config.grade_mask)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "invalid Grades; use comma-separated grades such as SS,SR,SU");
            return false;
        }

        const int recent = g_api->SettingInt(g_mod, "RecentCount", 10);
        if (recent < 1 || recent > 20000) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "RecentCount must be between 1 and 20000");
            return false;
        }
        g_config.recent_count = static_cast<uint32_t>(recent);

        std::string units;
        if (!ReadSetting("UnitIDs", "", units) ||
            !ParseUnits(units, g_config.unit_ids)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "invalid UnitIDs; use at most 2048 comma-separated positive unit IDs");
            return false;
        }
        return true;
    }

    bool NativeRecord(uint32_t id, int type, const uint8_t** record)
    {
        *record = nullptr;
        void* grid = g_gridData();
        return grid && g_gridFind(grid, record, id, type, 0) != 0 && *record;
    }

    void BuildPool()
    {
        std::vector<Unit> units;
        units.reserve(2048);
        uint32_t rows = 0;
        for (uint32_t offset = 0; offset < kMaximumUnitIndexes; ++offset) {
            const uint32_t catalogId = kFirstUnitIndex + offset;
            const uint8_t* index = nullptr;
            if (!NativeRecord(catalogId, kUnitIndexType, &index))
                continue;
            ++rows;
            if (index[4] != 0 || index[kUnitIndexEnabled] != 1)
                continue;
            const uint32_t unitId = Read32(index + kUnitIndexUnit);
            const uint8_t* definition = nullptr;
            if (!unitId || !NativeRecord(unitId, kUnitType, &definition) ||
                Read32(definition) != unitId || definition[4] != 0)
                continue;
            units.push_back(
                {unitId, catalogId, definition[kUnitRank], definition[kUnitSubgrade]});
        }
        const auto candidates = Candidates(g_config, units);
        g_rotation.SetPool(candidates);
        g_poolBuilt = true;
        g_api->Log(g_mod, candidates.empty() ? SDGO_LOG_WARN : SDGO_LOG_INFO,
                   "login showcase mode=%s: %lu encyclopedia rows, %lu eligible definitions, %lu candidates",
                   ModeName(g_config.mode), static_cast<unsigned long>(rows),
                   static_cast<unsigned long>(units.size()),
                   static_cast<unsigned long>(candidates.size()));
    }

    uint32_t Random()
    {
        if (!g_randomState) {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            g_randomState = static_cast<uint32_t>(counter.LowPart) ^
                            GetTickCount() ^ GetCurrentProcessId() ^
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_randomState));
            if (!g_randomState)
                g_randomState = 0x6D2B79F5u;
        }
        uint32_t value = g_randomState;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        g_randomState = value;
        return value;
    }

    uint32_t SelectUnit()
    {
        if (!g_poolBuilt)
            BuildPool();
        return g_rotation.Next([] { return Random(); });
    }

    uint32_t GuardedSelect(DWORD* exception)
    {
        __try {
            return SelectUnit();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *exception = GetExceptionCode();
            return 0;
        }
    }

    uint32_t __cdecl ChooseUnit()
    {
        AcquireSRWLockExclusive(&g_lock);
        DWORD exception = 0;
        const uint32_t result = GuardedSelect(&exception);
        if (exception) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "native encyclopedia scan failed (%08lX); original login unit retained",
                       static_cast<unsigned long>(exception));
            g_rotation.SetPool({});
            g_poolBuilt = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (result)
            g_api->Log(g_mod, SDGO_LOG_INFO, "selected login showcase unit %lu",
                       static_cast<unsigned long>(result));
        return result;
    }

    template <typename Fn>
    bool Bind(Fn& target, const char* name)
    {
        target = reinterpret_cast<Fn>(g_api->Symbol(g_mod, name));
        return target != nullptr;
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION || !api->SettingInt ||
        !api->SettingString || !api->Symbol || !api->Log ||
        !api->SettingInt(mod, "Enabled", 1))
        return 0;

    char revision[65] = {};
    const bool receipt =
        api->size >= offsetof(SdgoApi, ConfigurationApplied) +
                         sizeof(api->ConfigurationApplied) &&
        api->ConfigurationRevision && api->ConfigurationApplied;
    if (receipt && !api->ConfigurationRevision(mod, revision, sizeof(revision)))
        return 0;
    if (!LoadConfig() ||
        !Bind(g_gridData, "grid.data") ||
        !Bind(g_gridFind, "grid.find"))
        return 0;

    HMODULE startup = GetModuleHandleW(L"startup_crash_fix.dll");
    g_register = startup
        ? reinterpret_cast<SdgoRegisterLoginShowcaseChooserV1Fn>(
              GetProcAddress(startup, SDGO_LOGIN_SHOWCASE_REGISTER_EXPORT))
        : nullptr;
    if (!g_register || !g_register(&ChooseUnit)) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "compatible startup_crash_fix chooser broker is unavailable");
        return 0;
    }
    char currentRevision[65] = {};
    // The core records a stable initial revision after this entry returns and
    // marks the Mod active. ConfigurationApplied is for later active reloads.
    if (receipt &&
        (!revision[0] ||
         !api->ConfigurationRevision(mod, currentRevision,
                                     sizeof(currentRevision)) ||
         std::strcmp(revision, currentRevision) != 0)) {
        g_register(nullptr);
        api->Log(mod, SDGO_LOG_ERROR,
                 "configuration changed while login showcase was loading; restart to retry");
        return 0;
    }
    api->Log(mod, SDGO_LOG_INFO,
             "ready: original login presentation retained; selection waits for native encyclopedia");
    return 1;
}
