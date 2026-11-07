// Exercise the actual startup-crash and login-showcase DLLs against an owned
// native-ABI fixture. No game, window, server, account or player file is opened.
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"

namespace
{
    using HookFn = void(__fastcall*)(void*, void*, int, const uint8_t*);

    struct Fixture
    {
        std::map<uint32_t, std::array<uint8_t, 4976>> units;
        std::map<uint32_t, std::array<uint8_t, 614>> indexes;
        std::array<uint8_t, 469> loading{};
        std::array<uint8_t, 0x20E4> screen{};
        std::vector<uint32_t> shown;
        HookFn hook = nullptr;
        unsigned checks = 0, revisionCalls = 0, appliedCalls = 0;

        void Check(bool condition, const char* message)
        {
            ++checks;
            if (!condition)
                throw std::runtime_error(message);
        }
    } g;

    template <typename T>
    void Put(void* bytes, size_t offset, T value)
    {
        std::memcpy(static_cast<uint8_t*>(bytes) + offset, &value, sizeof(value));
    }

    template <typename T>
    T Read(const void* bytes, size_t offset)
    {
        T value{};
        std::memcpy(&value, static_cast<const uint8_t*>(bytes) + offset, sizeof(value));
        return value;
    }

    void AddUnit(uint32_t catalog, uint32_t unit, uint8_t rank, uint8_t subgrade)
    {
        auto& definition = g.units[unit];
        definition.fill(0);
        Put(definition.data(), 0, unit);
        definition[308] = rank;
        definition[309] = subgrade;

        auto& index = g.indexes[catalog];
        index.fill(0);
        Put(index.data(), 0, catalog);
        index[5] = 1;
        Put(index.data(), 6, unit);
    }

    void* __cdecl Grid()
    {
        return &g;
    }

    int __fastcall Find(void* grid, void*, const uint8_t** out, uint32_t id,
                        int type, char logging)
    {
        g.Check(grid == &g && logging == 0,
                "both production DLLs use the bound native grid");
        *out = nullptr;
        if (type == 2) {
            const auto found = g.units.find(id);
            if (found != g.units.end()) {
                *out = found->second.data();
                return 1;
            }
        } else if (type == 5 && id == Read<uint32_t>(g.loading.data(), 0)) {
            *out = g.loading.data();
            return 1;
        } else if (type == 0x55) {
            const auto found = g.indexes.find(id);
            if (found != g.indexes.end()) {
                *out = found->second.data();
                return 1;
            }
        }
        return 0;
    }

    void __fastcall Original(void* screen, void*, int arg, const uint8_t* set)
    {
        g.Check(screen == g.screen.data() && arg == 73 && Read<uint32_t>(set,0) == Read<uint32_t>(g.loading.data(),0),
                "the broker retains the original screen, argument and loading record identity");
        g.Check(std::memcmp(set+9,g.loading.data()+9,g.loading.size()-9)==0,"native presentation fields survive the private record copy");
        g.shown.push_back(Read<uint32_t>(set, 5));
    }

    void __fastcall ModelUpdate(void*, void*)
    {
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        if (!std::strcmp(name, "grid.data"))
            return reinterpret_cast<void*>(&Grid);
        if (!std::strcmp(name, "grid.find"))
            return reinterpret_cast<void*>(&Find);
        if (!std::strcmp(name, "model.commit"))
            return reinterpret_cast<void*>(&ModelUpdate);
        return nullptr;
    }

    int Hook(SdgoMod*, const char* name, void* detour, void** original)
    {
        g.Check(!std::strcmp(name, "gate.show_set_unit"),
                "the startup fix remains the sole owner of the native hook");
        g.hook = reinterpret_cast<HookFn>(detour);
        *original = reinterpret_cast<void*>(&Original);
        return 1;
    }

    int SettingInt(SdgoMod*, const char* key, int fallback)
    {
        if (!std::strcmp(key, "Enabled"))
            return 1;
        if (!std::strcmp(key, "RecentCount"))
            return 10;
        return fallback;
    }

    uint32_t SettingString(
        SdgoMod*, const char* key, const char* fallback, char* out, uint32_t size)
    {
        const char* value = fallback;
        if (!std::strcmp(key, "Mode"))
            value = "grades";
        else if (!std::strcmp(key, "Grades"))
            value = "SS,SR,SU";
        else if (!std::strcmp(key, "UnitIDs"))
            value = "";
        strncpy_s(out, size, value, _TRUNCATE);
        return static_cast<uint32_t>(std::strlen(out));
    }

    uint32_t Revision(SdgoMod*, char* out, uint32_t size)
    {
        ++g.revisionCalls;
        const std::string value(64, 'a');
        strncpy_s(out, size, value.c_str(), _TRUNCATE);
        return static_cast<uint32_t>(value.size());
    }

    int Applied(SdgoMod*, const char* revision)
    {
        ++g.appliedCalls;
        return revision && std::strlen(revision) == 64;
    }

    void Log(SdgoMod*, int, const char*, ...)
    {
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
        return 2;
    try {
        AddUnit(0x375BE1, 20001, 4, 2); // ordinary S, excluded by default
        AddUnit(0x375BE2, 20002, 4, 3); // SS
        AddUnit(0x375BE3, 20003, 4, 4); // SR
        AddUnit(0x375BE4, 20004, 4, 5); // SU
        Put(g.loading.data(), 0, uint32_t{27101});
        Put(g.loading.data(), 5, uint32_t{20001});
        Put(g.screen.data(), 0x20E0, uint32_t{27101});

        HMODULE startup = LoadLibraryW(argv[1]);
        const auto startupEntry =
            startup ? reinterpret_cast<SdgoModLoadFn>(
                          GetProcAddress(startup, "SdgoModLoad"))
                    : nullptr;
        g.Check(startupEntry != nullptr, "startup fix DLL exports SdgoModLoad");

        SdgoApi startupApi{};
        startupApi.size = sizeof(startupApi);
        startupApi.version = SDGO_API_VERSION;
        startupApi.Symbol = &Symbol;
        startupApi.HookCode = &Hook;
        startupApi.Log = &Log;
        g.Check(startupEntry(&startupApi, nullptr) != 0 && g.hook,
                "startup fix installed its single native loading detour");

        HMODULE showcase = LoadLibraryW(argv[2]);
        const auto showcaseEntry =
            showcase ? reinterpret_cast<SdgoModLoadFn>(
                           GetProcAddress(showcase, "SdgoModLoad"))
                     : nullptr;
        g.Check(showcaseEntry != nullptr, "login showcase DLL exports SdgoModLoad");

        SdgoApi showcaseApi{};
        showcaseApi.size = sizeof(showcaseApi);
        showcaseApi.version = SDGO_API_VERSION;
        showcaseApi.Symbol = &Symbol;
        showcaseApi.SettingInt = &SettingInt;
        showcaseApi.SettingString = &SettingString;
        showcaseApi.ConfigurationRevision = &Revision;
        showcaseApi.ConfigurationApplied = &Applied;
        showcaseApi.Log = &Log;
        g.Check(showcaseEntry(&showcaseApi, nullptr) != 0,
                "login showcase registered through the startup-fix broker");
        g.Check(g.revisionCalls == 2 && g.appliedCalls == 0,
                "initial load rechecks the revision and leaves receipt publication to the core");

        for (int index = 0; index < 4; ++index) {
            g.hook(g.screen.data(), nullptr, 73, g.loading.data());
            g.Check(Read<uint32_t>(g.loading.data(), 5) == 20001,
                    "the native loading record is restored after each original call");
        }
        const std::set<uint32_t> allowed{20002, 20003, 20004};
        g.Check(g.shown.size() == 4,
                "four simulated login presentations reached the original function");
        for (uint32_t unit : g.shown)
            g.Check(allowed.count(unit) == 1,
                    "the default pool contains SS, SR and SU but not ordinary S");
        g.Check(std::set<uint32_t>(g.shown.begin(), g.shown.begin() + 3).size() == 3,
                "one rotation shows every default candidate before repeating");
        g.Check(g.shown[2] != g.shown[3],
                "the next rotation does not repeat the preceding unit immediately");

        FreeLibrary(showcase);
        FreeLibrary(startup);
        std::printf("%u login showcase native ABI checks passed; no game process or UI\n",
                    g.checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
