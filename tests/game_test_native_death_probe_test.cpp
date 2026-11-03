#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "mods/game_test_client/native_death_probe.h"

SdgoPvpAiClientSnapshotV1 g_exportSnapshot = {};

extern "C" __declspec(dllexport) int __cdecl
SdgoPvpAiClientGetSnapshotV1(
    SdgoPvpAiClientSnapshotV1* output)
{
    if (!output || output->size < sizeof(*output) ||
        output->version !=
            SDGO_PVP_AI_CLIENT_OBSERVER_VERSION)
        return 0;
    *output = g_exportSnapshot;
    return 1;
}

namespace
{
    int g_checks = 0;
    int g_failures = 0;
    void* g_deadDetour = nullptr;
    void* g_mapDetour = nullptr;
    void* g_movementDetour = nullptr;
    void* g_eventDetour = nullptr;
    int g_deadCalls = 0;
    int g_mapCalls = 0;
    int g_movementCalls = 0;
    int g_eventCalls = 0;
    bool g_teleport = false;
    void* g_expectedSelf = nullptr;
    void* g_expectedControl = nullptr;
    uint8_t* g_expectedPacket = nullptr;
    DWORD g_expectedError = 0;
    void* g_unitManager = reinterpret_cast<void*>(1);
    alignas(void*) uint8_t g_network[0x80] = {};
    alignas(void*) uint8_t g_unit[0x3300] = {};
    alignas(void*) uint8_t g_node[0x100] = {};
    alignas(void*) uint8_t g_movement[0x300] = {};
    alignas(void*) uint8_t g_attack[0x220] = {};
    std::vector<std::string> g_observations;

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

    void __fastcall MovementOriginal(
        void* self, void*, void* control)
    {
        ++g_movementCalls;
        CHECK(self == g_expectedSelf);
        CHECK(control == g_expectedControl);
        CHECK(GetLastError() == g_expectedError);
        static_cast<uint8_t*>(self)[0x218] = 1;
        ++*reinterpret_cast<uint32_t*>(g_network + 0x58);
        *reinterpret_cast<uint32_t*>(g_network + 0x5C) += 0x0E;
        SetLastError(0xD00D0003u);
    }

    int __fastcall EventOriginal(
        void* self, void*, uint8_t* packet)
    {
        ++g_eventCalls;
        CHECK(self == g_expectedSelf);
        CHECK(packet == g_expectedPacket);
        CHECK(GetLastError() == g_expectedError);
        if (g_teleport) {
            *reinterpret_cast<float*>(g_node + 0x88) += 300.0f;
            SetLastError(0xD00D0004u);
            return 0x73C9;
        }
        ++*reinterpret_cast<uint32_t*>(g_network + 0x58);
        *reinterpret_cast<uint32_t*>(g_network + 0x5C) += 0x0E;
        SetLastError(0xD00D0004u);
        return 0x73C9;
    }

    uint8_t* __cdecl Network()
    {
        return g_network;
    }

    int SettingInt(SdgoMod*, const char* key, int fallback)
    {
        return strcmp(key, "BattleAiDeaths") == 0 ? 1 : fallback;
    }

    int __fastcall ObjectIdFromPilot(
        void*, void*, uint32_t pilotId)
    {
        return pilotId == 9001 ? 1005 : 0;
    }

    void* __fastcall FindObject(
        void*, void*, int objectId)
    {
        return objectId == 1005 ? g_unit : nullptr;
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        if (strcmp(name, "game.unit_manager") == 0)
            return &g_unitManager;
        if (strcmp(name, "um.object_id_from_pilot") == 0)
            return reinterpret_cast<void*>(&ObjectIdFromPilot);
        if (strcmp(name, "um.find_object") == 0)
            return reinterpret_cast<void*>(&FindObject);
        if (strcmp(name, "flow.network") == 0)
            return reinterpret_cast<void*>(&Network);
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
        if (strcmp(
                name,
                "game.map_death_movement_sender") == 0) {
            g_movementDetour = detour;
            const auto fixture = &MovementOriginal;
            static_assert(
                sizeof(fixture) == sizeof(*original),
                "x86 movement pointer size changed");
            memcpy(original, &fixture, sizeof(fixture));
            return 1;
        }
        if (strcmp(
                name,
                "game.map_death_event_sender") == 0) {
            g_eventDetour = detour;
            const auto fixture = &EventOriginal;
            static_assert(
                sizeof(fixture) == sizeof(*original),
                "x86 event pointer size changed");
            memcpy(original, &fixture, sizeof(fixture));
            return 1;
        }
        return 0;
    }

    void __cdecl Observe(const char* message)
    {
        g_observations.emplace_back(
            message ? message : "");
    }

    void __cdecl Assert(
        int passed, const char* message)
    {
        Check(passed != 0, message, __LINE__);
    }

    bool Observed(const char* text)
    {
        for (const auto& message : g_observations)
            if (message.find(text) != std::string::npos)
                return true;
        return false;
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

        SdgoPvpAiClientSnapshotV1 snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        snapshot.bot_count = 2;
        snapshot.bots[0].pilot_id = 9001;
        snapshot.bots[0].object_id = 1005;
        snapshot.bots[0].valid_registered_object = 1;
        snapshot.bots[0].ai_enabled = 1;
        snapshot.bots[0].local_control = 1;
        snapshot.bots[0].master_npc = 1;
        snapshot.bots[1].pilot_id = 9002;
        snapshot.bots[1].object_id = 1006;
        snapshot.bots[1].valid_registered_object = 0;
        const auto registered =
            sdgo_game_test_death::FindRegisteredAi(
                snapshot, 9001, 0);
        CHECK(registered.registered == 1);
        CHECK(registered.aiEnabled == 1);
        CHECK(registered.localControl == 1);
        CHECK(registered.remoteCopy == 0);
        CHECK(registered.masterNpc == 1);
        CHECK(
            sdgo_game_test_death::FindRegisteredAi(
                snapshot, 0, 1005).registered == 1);
        CHECK(
            sdgo_game_test_death::FindRegisteredAi(
                snapshot, 9002, 1006).registered == 0);
        CHECK(!sdgo_game_test_death::ParseDeadPacket(
            nullptr, &parsed));
        CHECK(!sdgo_game_test_death::ParseMapDamagePacket(
            map, nullptr));
    }

    void OriginalAbiTests()
    {
        memset(g_network, 0, sizeof(g_network));
        memset(g_unit, 0, sizeof(g_unit));
        memset(g_node, 0, sizeof(g_node));
        memset(g_movement, 0, sizeof(g_movement));
        memset(g_attack, 0, sizeof(g_attack));
        g_observations.clear();
        *reinterpret_cast<uint32_t*>(g_network + 0x58) = 20;
        *reinterpret_cast<uint32_t*>(g_network + 0x5C) = 200;
        *reinterpret_cast<uint32_t*>(g_unit + 0x04) = 1005;
        *reinterpret_cast<uint8_t**>(g_unit + 0x08) = g_node;
        *reinterpret_cast<int32_t*>(g_unit + 0x20) = 2;
        *reinterpret_cast<int32_t*>(g_unit + 0x38) = 5547;
        *reinterpret_cast<int32_t*>(g_unit + 0x3C) = 12500;
        *reinterpret_cast<uint8_t**>(g_unit + 0x9C8) =
            g_movement;
        *reinterpret_cast<uint8_t**>(g_unit + 0x9CC) =
            g_attack;
        *reinterpret_cast<uint32_t*>(g_unit + 0x2B68) = 9001;
        g_unit[0x31E9] = 0;
        *reinterpret_cast<uint32_t*>(g_unit + 0x3238) =
            3604443;
        *reinterpret_cast<uint8_t**>(g_movement + 0x04) =
            g_movement + 0x20;
        *reinterpret_cast<uint8_t**>(g_movement + 0x08) =
            g_movement + 0x34;
        *reinterpret_cast<uint8_t**>(g_movement + 0x21C) =
            g_unit;
        *reinterpret_cast<uint8_t**>(g_attack + 0x04) =
            g_attack + 0x48;
        *reinterpret_cast<uint8_t**>(g_attack + 0x08) =
            g_attack + 0x5C;
        float* position =
            reinterpret_cast<float*>(g_node + 0x88);
        position[0] = 898.872f;
        position[1] = 62.570f;
        position[2] = 717.411f;
        g_exportSnapshot = {};
        g_exportSnapshot.size =
            sizeof(g_exportSnapshot);
        g_exportSnapshot.version =
            SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        g_exportSnapshot.bot_count = 1;
        auto& bot = g_exportSnapshot.bots[0];
        bot.pilot_id = 9001;
        bot.object_id = 1005;
        bot.npc_id = 3604443;
        bot.ai_enabled = 1;
        bot.valid_registered_object = 1;
        bot.local_control = 1;
        bot.master_npc = 1;

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
        CHECK(g_movementDetour != nullptr);
        CHECK(g_eventDetour != nullptr);

        using DetourFn =
            int(__fastcall*)(void*, void*, uint8_t*);
        using MovementDetourFn =
            void(__fastcall*)(void*, void*, void*);
        const auto dead =
            reinterpret_cast<DetourFn>(g_deadDetour);
        const auto map =
            reinterpret_cast<DetourFn>(g_mapDetour);
        const auto movement =
            reinterpret_cast<MovementDetourFn>(
                g_movementDetour);
        const auto event =
            reinterpret_cast<DetourFn>(g_eventDetour);
        uint8_t packet[0x20] = {};
        *reinterpret_cast<uint16_t*>(packet) = 0x07D4;
        *reinterpret_cast<uint32_t*>(packet + 0x06) = 9001;
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

        g_expectedSelf = g_movement;
        g_expectedControl = g_unit + 0xDC0;
        SetLastError(g_expectedError);
        __asm mov before, esp
        movement(
            g_expectedSelf, nullptr,
            g_expectedControl);
        __asm mov after, esp
        CHECK(before == after);
        CHECK(g_movementCalls == 1);
        CHECK(g_movement[0x218] == 1);
        CHECK(GetLastError() == 0xD00D0003u);

        g_expectedSelf = packet + 1;
        SetLastError(g_expectedError);
        __asm mov before, esp
        const int eventResult =
            event(g_expectedSelf, nullptr, packet);
        __asm mov after, esp
        CHECK(before == after);
        CHECK(eventResult == 0x73C9);
        CHECK(g_eventCalls == 1);
        CHECK(GetLastError() == 0xD00D0004u);

        SdgoGameTestContextV1 context = {};
        context.size = sizeof(context);
        context.version = SDGO_GAME_TEST_VERSION;
        context.Assert = &Assert;
        context.Observe = &Observe;
        CHECK(probe.Observe(&context));
        CHECK(Observed(
            "cause=map_request_movement"));
        CHECK(Observed(
            "cause=map_request_event"));
        CHECK(Observed("victim_registered_ai=1"));
        CHECK(Observed("victim_movement=20/34"));
        CHECK(Observed("send_0492=1"));
        g_teleport = true;
        SetLastError(g_expectedError);
        __asm mov before, esp
        CHECK(event(g_expectedSelf, nullptr, packet) == 0x73C9);
        __asm mov after, esp
        CHECK(before == after);
        CHECK(GetLastError() == 0xD00D0004u);
        CHECK(probe.Observe(&context));
        CHECK(Observed("alive_relocation=1"));
        CHECK(Observed("post_object=1005 post_hp=5547"));
        g_teleport = false;
        probe.Finish(&context);
        CHECK(Observed("alive_relocations=1 required=0"));
        CHECK(Observed(
            "registered_ai_movement_0492=1"));
        CHECK(Observed(
            "registered_ai_event_0492=1"));
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
