#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>

#include "mods/game_test_client/native_death_probe.h"

namespace
{
    int g_checks = 0;
    int g_failures = 0;
    void* g_deadDetour = nullptr;
    void* g_mapDetour = nullptr;
    int g_deadCalls = 0;
    int g_mapCalls = 0;
    void* g_expectedSelf = nullptr;
    uint8_t* g_expectedPacket = nullptr;
    DWORD g_expectedError = 0;
    void* g_unitManager = reinterpret_cast<void*>(1);

    void Check(bool condition, const char* expression, int line)
    {
        ++g_checks;
        if (!condition) {
            ++g_failures;
            std::cerr << "FAIL line " << line << ": "
                      << expression << "\n";
        }
    }

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

    int __fastcall DeadOriginal(
        void* self, void*, uint8_t* packet)
    {
        ++g_deadCalls;
        CHECK(self == g_expectedSelf);
        CHECK(packet == g_expectedPacket);
        CHECK(GetLastError() == g_expectedError);
        SetLastError(0xD00D0001u);
        return 0x51A7;
    }

    int __fastcall MapOriginal(
        void* self, void*, uint8_t* packet)
    {
        ++g_mapCalls;
        CHECK(self == g_expectedSelf);
        CHECK(packet == g_expectedPacket);
        CHECK(GetLastError() == g_expectedError);
        SetLastError(0xD00D0002u);
        return 0x62B8;
    }

    int SettingInt(SdgoMod*, const char* key, int fallback)
    {
        return strcmp(key, "BattleAiDeaths") == 0 ? 1 : fallback;
    }

    int __fastcall ObjectIdFromPilot(
        void*, void*, uint32_t)
    {
        return 0;
    }

    void* __fastcall FindObject(void*, void*, int)
    {
        return nullptr;
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        if (strcmp(name, "game.unit_manager") == 0)
            return &g_unitManager;
        if (strcmp(name, "um.object_id_from_pilot") == 0)
            return reinterpret_cast<void*>(&ObjectIdFromPilot);
        if (strcmp(name, "um.find_object") == 0)
            return reinterpret_cast<void*>(&FindObject);
        return nullptr;
    }

    int HookCode(
        SdgoMod*, const char* name, void* detour, void** original)
    {
        if (strcmp(name, "game.dead_unit_handler") == 0) {
            g_deadDetour = detour;
            const auto fixture = &DeadOriginal;
            static_assert(
                sizeof(fixture) == sizeof(*original),
                "x86 handler pointer size changed");
            memcpy(original, &fixture, sizeof(fixture));
            return 1;
        }
        if (strcmp(name, "game.map_damage_handler") == 0) {
            g_mapDetour = detour;
            const auto fixture = &MapOriginal;
            static_assert(
                sizeof(fixture) == sizeof(*original),
                "x86 handler pointer size changed");
            memcpy(original, &fixture, sizeof(fixture));
            return 1;
        }
        return 0;
    }

    void ParserTests()
    {
        uint8_t dead[
            0x0C +
            sdgo_game_test_death::kMaximumVictims * 0x0B] = {};
        *reinterpret_cast<uint32_t*>(dead + 0x06) = 7001;
        dead[0x0B] = 2;
        *reinterpret_cast<uint32_t*>(dead + 0x0C) = 8001;
        dead[0x10] = 4;
        dead[0x15] = 9;
        dead[0x16] = 10;
        *reinterpret_cast<uint32_t*>(dead + 0x17) = 8002;
        dead[0x1B] = 14;
        dead[0x20] = 19;
        dead[0x21] = 20;

        sdgo_game_test_death::DeadPacket parsed = {};
        CHECK(sdgo_game_test_death::ParseDeadPacket(dead, &parsed));
        CHECK(parsed.killerPilotId == 7001);
        CHECK(parsed.nativeVictimCount == 2);
        CHECK(parsed.capturedVictimCount == 2);
        CHECK(parsed.truncated == 0);
        CHECK(parsed.victims[0].pilotId == 8001);
        CHECK(parsed.victims[0].raw4 == 4);
        CHECK(parsed.victims[0].raw9 == 9);
        CHECK(parsed.victims[0].raw10 == 10);
        CHECK(parsed.victims[1].pilotId == 8002);
        CHECK(parsed.victims[1].raw4 == 14);
        CHECK(parsed.victims[1].raw9 == 19);
        CHECK(parsed.victims[1].raw10 == 20);

        dead[0x0B] = 13;
        CHECK(sdgo_game_test_death::ParseDeadPacket(dead, &parsed));
        CHECK(parsed.nativeVictimCount == 13);
        CHECK(parsed.capturedVictimCount ==
              sdgo_game_test_death::kMaximumVictims);
        CHECK(parsed.truncated == 1);

        uint8_t map[0x0D] = {};
        *reinterpret_cast<uint32_t*>(map + 0x06) = 9001;
        map[0x0A] = 21;
        map[0x0B] = 22;
        map[0x0C] = 23;
        sdgo_game_test_death::MapDamagePacket mapParsed = {};
        CHECK(sdgo_game_test_death::ParseMapDamagePacket(
            map, &mapParsed));
        CHECK(mapParsed.victimPilotId == 9001);
        CHECK(mapParsed.raw10 == 21);
        CHECK(mapParsed.raw11 == 22);
        CHECK(mapParsed.raw12 == 23);
        CHECK(!sdgo_game_test_death::ParseDeadPacket(
            nullptr, &parsed));
        CHECK(!sdgo_game_test_death::ParseMapDamagePacket(
            map, nullptr));
    }

    void OriginalAbiTests()
    {
        SdgoApi api = {};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.Symbol = &Symbol;
        api.HookCode = &HookCode;
        api.SettingInt = &SettingInt;
        NativeDeathProbe probe;
        CHECK(probe.Install(&api, nullptr));
        CHECK(g_deadDetour != nullptr);
        CHECK(g_mapDetour != nullptr);

        using DetourFn =
            int(__fastcall*)(void*, void*, uint8_t*);
        const auto dead =
            reinterpret_cast<DetourFn>(g_deadDetour);
        const auto map =
            reinterpret_cast<DetourFn>(g_mapDetour);
        uint8_t packet[0x20] = {};
        g_expectedSelf = packet + 1;
        g_expectedPacket = packet;
        g_expectedError = 0x11223344u;

        uintptr_t before = 0;
        uintptr_t after = 0;
        SetLastError(g_expectedError);
        __asm mov before, esp
        const int deadResult =
            dead(g_expectedSelf, nullptr, packet);
        __asm mov after, esp
        CHECK(before == after);
        CHECK(deadResult == 0x51A7);
        CHECK(g_deadCalls == 1);
        CHECK(GetLastError() == 0xD00D0001u);

        SetLastError(g_expectedError);
        __asm mov before, esp
        const int mapResult =
            map(g_expectedSelf, nullptr, packet);
        __asm mov after, esp
        CHECK(before == after);
        CHECK(mapResult == 0x62B8);
        CHECK(g_mapCalls == 1);
        CHECK(GetLastError() == 0xD00D0002u);
    }
}

int main()
{
    ParserTests();
    OriginalAbiTests();
    std::cout << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
