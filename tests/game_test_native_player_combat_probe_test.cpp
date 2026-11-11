#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "mods/game_test_client/native_player_combat_probe.h"

namespace
{
    int g_checks = 0;
    int g_failures = 0;
    int g_setting = 1;
    int g_continuousSetting = 0;
    int g_secondarySetting = 0;
    int g_weaponSlotSetting = -1;
    int g_specialSetting = 0;
    int g_weaponRequests = 0;
    uint32_t g_weaponAction = 16;
    bool g_specialCommit = false;
    bool g_specialSourceReady = true;
    bool g_specialComputes = true;
    uint32_t g_specialEvents = 0;
    bool g_consumeAmmo = true;
    int g_symbolCalls = 0;
    const char* g_missingSymbol = nullptr;

    using QueryDetour =
        uint32_t(__fastcall*)(void*, void*, uint32_t);
    QueryDetour g_pressedDetour = nullptr;
    QueryDetour g_heldDetour = nullptr;
    QueryDetour g_downDetour = nullptr;

    constexpr uint32_t kPilotId = 77001;
    constexpr uint32_t kObjectId = 7;
    constexpr uint32_t kEnemyObjectId = 9;
    constexpr uint32_t kEnemyPilotId = 0x80000011u;
    uint8_t g_manager[0x100] = {};
    uint8_t* g_managerPointer = g_manager;
    uint8_t g_unit[0x3300] = {};
    uint8_t g_node[0x200] = {};
    uint8_t g_move[0x300] = {};
    uint8_t g_attack[0xC00] = {};
    uint8_t g_definition[0x300] = {};
    uint8_t g_weapons[3][0x700] = {};
    uint8_t g_specialWeapon[0x700] = {};
    uint8_t g_enemy[0x100] = {};
    uint8_t g_enemyNode[0x100] = {};
    void* g_moveVtable[2] = {};
    void* g_attackVtable[2] = {};
    uint8_t g_nativeInput[0x40] = {};

    int g_moveInputCalls = 0;
    int g_fireInputCalls = 0;
    int g_lockInputCalls = 0;
    int g_moveDispatchCalls = 0;
    int g_attackDispatchCalls = 0;
    bool g_moveHeld = false;
    bool g_fireHeld = false;
    bool g_lockHeld = false;
    void* g_lastMoveInput = nullptr;
    void* g_lastFireInput = nullptr;
    void* g_lastMoveContext = nullptr;
    void* g_lastAttackContext = nullptr;
    std::vector<std::string> g_observations;
    std::vector<int> g_assertions;
    void __fastcall WeaponInput(uint8_t*, void*, void*, int);

    void Check(
        bool condition, const char* expression, int line)
    {
        ++g_checks;
        if (!condition) {
            ++g_failures;
            std::cerr << "FAIL line " << line << ": "
                      << expression << "\n";
        }
    }

#define CHECK(expression) \
    Check(!!(expression), #expression, __LINE__)

    template<class T>
    void Put(uint8_t* bytes, size_t offset, T value)
    {
        std::memcpy(
            bytes + offset, &value, sizeof(value));
    }

    template<class T>
    T Get(const uint8_t* bytes, size_t offset)
    {
        T value = {};
        std::memcpy(
            &value, bytes + offset, sizeof(value));
        return value;
    }

    template<class T>
    void* FunctionAddress(T function)
    {
        static_assert(
            sizeof(function) == sizeof(void*),
            "the fixture requires Win32 function pointers");
        void* address = nullptr;
        std::memcpy(
            &address, &function, sizeof(address));
        return address;
    }

    uint32_t __fastcall QueryOriginal(
        void*, void*, uint32_t)
    {
        return 0;
    }

    uint32_t __fastcall ObjectIdFromPilot(
        void* manager, void*, uint32_t pilotId)
    {
        CHECK(manager == g_manager);
        return pilotId == kPilotId ? kObjectId : 0;
    }

    uint8_t* __fastcall FindObject(
        void* manager, void*, uint32_t objectId)
    {
        CHECK(manager == g_manager);
        return objectId == kObjectId ? g_unit : objectId == kEnemyObjectId ? g_enemy : nullptr;
    }

    void __fastcall MoveInput(
        uint8_t* unit, void*, void* input)
    {
        ++g_moveInputCalls;
        CHECK(unit == g_unit);
        g_lastMoveInput = input;
        g_moveHeld =
            g_heldDetour(input, nullptr, 0) != 0;
    }

    void __fastcall FireInput(
        uint8_t* unit, void*, void* input)
    {
        ++g_fireInputCalls;
        CHECK(unit == g_unit);
        g_lastFireInput = input;
        g_fireHeld =
            g_heldDetour(input, nullptr, 9) != 0;
    }

    void __fastcall LockInput(
        uint8_t* unit, void*, void* input)
    {
        ++g_lockInputCalls;
        CHECK(unit == g_unit);
        g_lockHeld =
            g_heldDetour(input, nullptr, 10) != 0;
    }

    void __fastcall MoveDispatch(
        uint8_t* move, void*, void* context)
    {
        ++g_moveDispatchCalls;
        CHECK(move == g_move);
        g_lastMoveContext = context;
        if (g_moveHeld) {
            float x = Get<float>(g_node, 0x88);
            Put(g_node, 0x88, x + 2.0f);
        }
    }

    void __fastcall AttackDispatch(
        uint8_t* attack, void*, void* context)
    {
        ++g_attackDispatchCalls;
        CHECK(attack == g_attack);
        g_lastAttackContext = context;
        if (g_fireHeld)
        {
            Put(
                g_attack, 4,
                static_cast<void*>(g_attack + 0x40));
            if (g_consumeAmmo && g_weaponSlotSetting == 2 && Get<int32_t>(g_attack, 0x240) == 2) {
                auto ammo = Get<int16_t>(g_attack, 0x24C + 2 * 0xC8);
                if (ammo > 0) Put(g_attack, 0x24C + 2 * 0xC8, int16_t(ammo - 1));
            }
            // A bound melee special captures only a locked target.
            if (g_specialComputes && Get<int32_t>(g_attack, 0x240) == 3 &&
                (g_specialWeapon[0x6A] != 1 || g_lockHeld))
                ++g_specialEvents;
        }
        // The native FSM commits the queued special-slot selection.
        if (g_specialCommit && Get<int32_t>(g_unit, 0xDD0) == 3) {
            Put(g_attack, 0x240, int32_t(3));
            Put(g_attack, 0x248, static_cast<void*>(g_specialWeapon));
        }
    }

    bool __cdecl SpecialEvents(uint32_t* events)
    {
        if (!g_specialSourceReady || !events) return false;
        *events = g_specialEvents;
        return true;
    }

    int SettingInt(
        SdgoMod*, const char* key, int fallback)
    {
        if (std::strcmp(
                key, "BattlePlayerCombat") == 0)
            return g_setting;
        if (std::strcmp(
                key,
                "BattlePlayerCombatContinuous") == 0)
            return g_continuousSetting;
        if (std::strcmp(
                key, "BattlePlayerSecondary") == 0)
            return g_secondarySetting;
        if (std::strcmp(key, "BattlePlayerWeaponSlot") == 0)
            return g_weaponSlotSetting;
        if (std::strcmp(key, "BattlePlayerSpecial") == 0)
            return g_specialSetting;
        return fallback;
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        ++g_symbolCalls;
        if (g_missingSymbol &&
            std::strcmp(name, g_missingSymbol) == 0)
            return nullptr;
        if (std::strcmp(
                name, "game.unit_manager") == 0)
            return &g_managerPointer;
        if (std::strcmp(
                name, "um.object_id_from_pilot") == 0)
            return FunctionAddress(&ObjectIdFromPilot);
        if (std::strcmp(
                name, "um.find_object") == 0)
            return FunctionAddress(&FindObject);
        if (std::strcmp(
                name, "test.apsalus_move_input") == 0)
            return FunctionAddress(&MoveInput);
        if (std::strcmp(
                name, "test.apsalus_primary_input") == 0)
            return FunctionAddress(&FireInput);
        if (std::strcmp(
                name, "test.player_secondary_input") == 0)
            return FunctionAddress(&LockInput);
        if (std::strcmp(name, "test.player_weapon_input") == 0)
            return FunctionAddress(&WeaponInput);
        return nullptr;
    }

    int HookCode(
        SdgoMod*, const char* name,
        void* detour, void** original)
    {
        const auto fixture = &QueryOriginal;
        std::memcpy(
            original, &fixture, sizeof(fixture));
        if (std::strcmp(
                name, "test.input_action_pressed") == 0)
            g_pressedDetour =
                reinterpret_cast<QueryDetour>(detour);
        else if (std::strcmp(
                     name,
                     "test.input_action_held") == 0)
            g_heldDetour =
                reinterpret_cast<QueryDetour>(detour);
        else if (std::strcmp(
                     name,
                     "test.input_action_down") == 0)
            g_downDetour =
                reinterpret_cast<QueryDetour>(detour);
        else
            return 0;
        return 1;
    }

    void __cdecl AssertResult(
        int passed, const char*)
    {
        g_assertions.push_back(passed);
    }

    void __cdecl Observe(const char* message)
    {
        g_observations.emplace_back(
            message ? message : "");
    }

    SdgoApi Api()
    {
        SdgoApi api = {};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.SettingInt = &SettingInt;
        api.Symbol = &Symbol;
        api.HookCode = &HookCode;
        return api;
    }

    SdgoGameTestContextV1 Context(
        uint64_t elapsed)
    {
        SdgoGameTestContextV1 context = {};
        context.size = sizeof(context);
        context.version = SDGO_GAME_TEST_VERSION;
        context.elapsed_ms = elapsed;
        context.Assert = &AssertResult;
        context.Observe = &Observe;
        return context;
    }

    SdgoPvpAiClientSnapshotV1 Snapshot()
    {
        SdgoPvpAiClientSnapshotV1 snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        snapshot.native_app_state = 22;
        snapshot.profile_pilot_id = kPilotId;
        snapshot.received_battle_start_count = 41;
        return snapshot;
    }

    SdgoPvpAiClientStartStatusV1 StartStatus()
    {
        SdgoPvpAiClientStartStatusV1 status = {};
        status.size = sizeof(status);
        status.version =
            SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        status.battle_start_sequence = 41;
        status.player_control_enabled = 1;
        return status;
    }

    SdgoMissionAiClientSnapshotV1 MissionSnapshot()
    {
        SdgoMissionAiClientSnapshotV1 snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;
        snapshot.active = 1;
        snapshot.battle_generation = 73;
        snapshot.room_object = 0x123400;
        return snapshot;
    }

    void ResetNativeWorld()
    {
        std::memset(g_manager, 0, sizeof(g_manager));
        std::memset(g_unit, 0, sizeof(g_unit));
        std::memset(g_node, 0, sizeof(g_node));
        std::memset(g_move, 0, sizeof(g_move));
        std::memset(g_attack, 0, sizeof(g_attack));
        std::memset(
            g_nativeInput, 0, sizeof(g_nativeInput));
        g_moveVtable[0] = nullptr;
        g_moveVtable[1] =
            FunctionAddress(&MoveDispatch);
        g_attackVtable[0] = nullptr;
        g_attackVtable[1] =
            FunctionAddress(&AttackDispatch);
        g_managerPointer = g_manager;
        Put(g_manager, 0x88, g_unit);
        Put(g_unit, 0x04, kObjectId);
        Put(g_unit, 0x08, g_node);
        Put(g_unit, 0x38, int32_t(1000));
        Put(g_unit, 0x9C8, g_move);
        Put(g_unit, 0x9CC, g_attack);
        Put(g_unit, 0xBC8, int32_t(25000));
        Put(g_unit, 0x2B68, kPilotId);
        g_unit[0x31E9] = 1;
        Put(g_move, 0, g_moveVtable);
        Put(g_move, 0x21C, g_unit);
        Put(g_attack, 0, g_attackVtable);
        Put(
            g_attack, 4,
            static_cast<void*>(g_attack + 0x20));
        Put(g_node, 0x88, 0.0f);
        Put(g_node, 0x8C, 0.0f);
        Put(g_node, 0x90, 0.0f);
        g_moveInputCalls = 0;
        g_fireInputCalls = 0;
        g_lockInputCalls = 0;
        g_moveDispatchCalls = 0;
        g_attackDispatchCalls = 0;
        g_moveHeld = false;
        g_fireHeld = false;
        g_lockHeld = false;
        g_lastMoveInput = nullptr;
        g_lastFireInput = nullptr;
        g_lastMoveContext = nullptr;
        g_lastAttackContext = nullptr;
        g_observations.clear();
        g_assertions.clear();
        g_weaponRequests = 0;
        g_consumeAmmo = true;
    }

    void __fastcall WeaponInput(uint8_t* unit, void*, void* input, int wheel)
    {
        CHECK(unit == g_unit && input == g_nativeInput && wheel == 0);
        CHECK(g_downDetour(input, nullptr, g_weaponAction) == 1);
        ++g_weaponRequests;
        // Real native receipt queues DD0; it does not acknowledge the binding.
        // The special slot is queued only with a full SP gauge.
        if (g_weaponAction != 17 || Get<int32_t>(g_unit, 0xBC8) >= 30000)
            Put(g_unit, 0xDD0, int32_t(g_weaponAction - 14));
    }

    void RangedProfile()
    {
        std::memset(g_definition, 0, sizeof(g_definition));
        std::memset(g_weapons, 0, sizeof(g_weapons));
        Put(g_unit, 0x9C4, g_definition);
        Put(g_definition, 0, uint32_t(10003));
        g_definition[0x247] = 3;
        for (int slot = 0; slot < 3; ++slot) {
            const uint32_t id = slot == 2 ? 40119 : 41001 + slot;
            Put(g_definition, 0x249 + slot * 4, id);
            Put(g_weapons[slot], 0, id);
            g_weapons[slot][0x69] = 1;
            g_weapons[slot][0x6A] = slot == 0 ? 1 : 2;
            Put(g_weapons[slot], 0x6C, int32_t(100));
            Put(g_weapons[slot], 0x72, uint32_t(slot == 0 ? 200 : 2000));
            Put(g_weapons[slot], 0x7C, int16_t(slot == 0 ? 0 : 60));
            g_weapons[slot][0x8F] = 1;
            for (size_t bank : {size_t(0x24C), size_t(0x56C)}) {
                Put(g_attack, bank + slot * 0xC8, int16_t(60));
                Put(g_attack, bank + slot * 0xC8 + 0xC, g_weapons[slot]);
            }
        }
        Put(g_attack, 0x240, int32_t(0));
        Put(g_attack, 0x248, g_weapons[0]);
        for (auto pair : {std::pair<size_t, size_t>(0xBCC, 0xBD0), {0xBE1, 0xBE5}}) {
            for (uint8_t i = 0; i < 4; ++i) g_unit[pair.first + i] = i;
            Put(g_unit, pair.second, 100.0f);
        }
    }

    void RangedWeaponAcknowledgementTests()
    {
        ResetNativeWorld();
        RangedProfile();
        g_setting = g_continuousSetting = 1;
        g_secondarySetting = 0;
        g_weaponSlotSetting = 2;
        auto api = Api();
        ScoreboardActions input;
        CHECK(input.Install(&api, nullptr));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 31) == 0);
        NativePlayerCombatProbe probe;
        CHECK(probe.Install(&api, nullptr));
        auto snapshot = Snapshot();
        auto status = StartStatus();
        auto context = Context(1000);
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 1 && g_moveInputCalls == 0 && g_fireInputCalls == 0);
        CHECK(Get<int32_t>(g_unit, 0xDD0) == 2 && Get<int32_t>(g_attack, 0x240) == 0);
        context.elapsed_ms = 1100;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 2 && g_fireInputCalls == 0);
        context.elapsed_ms = 1300;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 3 && g_fireInputCalls == 0);
        // Only an actual native commit permits movement/fire sampling to begin.
        Put(g_attack, 0x240, int32_t(2));
        Put(g_attack, 0x248, g_weapons[2]);
        context.elapsed_ms = 1400;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_moveInputCalls == 1 && !g_fireHeld);
        CHECK(g_heldDetour(g_nativeInput, nullptr, 16) == 0);
        context.elapsed_ms = 1580;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_fireHeld && Get<int16_t>(g_attack, 0x24C + 2 * 0xC8) == 59);
        context.elapsed_ms = 1600;
        CHECK(probe.Poll(&context, snapshot, status, input));
        probe.Finish(&context, input);
        CHECK(g_assertions.size() == 4);
        for (int result : g_assertions) CHECK(result == 1);
        const int finishedMoves = g_moveInputCalls;
        const int finishedFire = g_fireInputCalls;
        const int finishedDispatch = g_attackDispatchCalls;
        context.elapsed_ms = 300000;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_moveInputCalls == finishedMoves && g_fireInputCalls == finishedFire &&
              g_attackDispatchCalls == finishedDispatch);
        CHECK(g_heldDetour(g_nativeInput, nullptr, 9) == 0 &&
              g_downDetour(g_nativeInput, nullptr, 16) == 0);
        bool boundSeen = false;
        for (const auto& line : g_observations)
            boundSeen = boundSeen || line.find("slot=2 weapon=40119") != std::string::npos;
        CHECK(boundSeen);

        ResetNativeWorld();
        RangedProfile();
        g_consumeAmmo = false;
        Put(g_attack, 0x240, int32_t(2));
        Put(g_attack, 0x248, g_weapons[2]);
        NativePlayerCombatProbe alternate;
        CHECK(alternate.Install(&api, nullptr));
        context.elapsed_ms = 0;
        CHECK(alternate.Poll(&context, snapshot, status, input));
        g_unit[0xE34] = 1;
        Put(g_attack, 0x56C + 2 * 0xC8, int16_t(10));
        context.elapsed_ms = 180;
        CHECK(alternate.Poll(&context, snapshot, status, input));
        CHECK(g_fireHeld);
        context.elapsed_ms = 200;
        CHECK(alternate.Poll(&context, snapshot, status, input));
        alternate.Finish(&context, input);
        CHECK(g_assertions.size() == 4 && g_assertions[0] && g_assertions[1] &&
              g_assertions[2] && !g_assertions[3]);

        // Unavailable live data must release a previously held fire mapping.
        NativePlayerCombatProbe unavailable;
        CHECK(unavailable.Install(&api, nullptr));
        context.elapsed_ms = 0;
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        context.elapsed_ms = 180;
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 9) == 1);
        Put(g_definition, 0, uint32_t(0));
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 9) == 0);
        Put(g_definition, 0, uint32_t(10003));
        context.elapsed_ms = 1000;
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        context.elapsed_ms = 1180;
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 9) == 1);
        g_unit[0x31E9] = 0;
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 9) == 0);
        g_unit[0x31E9] = 1;
        context.elapsed_ms = 1300;
        CHECK(unavailable.Poll(&context, snapshot, status, input));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 0) == 1 &&
              g_heldDetour(g_nativeInput, nullptr, 9) == 0);
        unavailable.Finish(&context, input);

        ResetNativeWorld();
        RangedProfile();
        g_weaponSlotSetting = 0;
        NativePlayerCombatProbe melee;
        CHECK(melee.Install(&api, nullptr));
        CHECK(!melee.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 0 && g_fireInputCalls == 0 && g_assertions.size() == 1 && !g_assertions[0]);
        g_weaponSlotSetting = 3;
        NativePlayerCombatProbe special;
        CHECK(!special.Install(&api, nullptr));
        g_weaponSlotSetting = -1;
    }

    void InstallTests()
    {
        SdgoApi api = Api();
        g_setting = 0;
        g_continuousSetting = 0;
        g_secondarySetting = 0;
        g_symbolCalls = 0;
        g_missingSymbol = nullptr;
        // A disabled probe binds only the read-only unit manager (ReadLocalArea).
        NativePlayerCombatProbe disabled;
        CHECK(disabled.Install(&api, nullptr));
        CHECK(g_symbolCalls == 1);

        g_setting = 2;
        NativePlayerCombatProbe invalid;
        CHECK(!invalid.Install(&api, nullptr));
        CHECK(g_symbolCalls == 1);

        g_setting = 1;
        g_continuousSetting = 2;
        NativePlayerCombatProbe invalidContinuous;
        CHECK(!invalidContinuous.Install(&api, nullptr));
        CHECK(g_symbolCalls == 1);

        g_continuousSetting = 0;
        g_secondarySetting = 2;
        NativePlayerCombatProbe invalidSecondary;
        CHECK(!invalidSecondary.Install(&api, nullptr));
        CHECK(g_symbolCalls == 1);

        g_secondarySetting = 0;
        g_missingSymbol =
            "test.apsalus_primary_input";
        NativePlayerCombatProbe missing;
        CHECK(!missing.Install(&api, nullptr));
        CHECK(g_symbolCalls == 6);
        g_missingSymbol = nullptr;
    }

    void GatingAndAbiTests()
    {
        ResetNativeWorld();
        SdgoApi api = Api();
        g_setting = 1;
        g_continuousSetting = 0;
        g_secondarySetting = 0;
        g_symbolCalls = 0;

        ScoreboardActions input;
        CHECK(input.Install(&api, nullptr));
        CHECK(g_pressedDetour != nullptr);
        CHECK(g_heldDetour != nullptr);
        CHECK(g_downDetour != nullptr);

        NativePlayerCombatProbe probe;
        CHECK(probe.Install(&api, nullptr));
        CHECK(g_symbolCalls == 5);

        auto snapshot = Snapshot();
        auto status = StartStatus();
        auto context = Context(1000);
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(g_moveInputCalls == 0);
        CHECK(g_fireInputCalls == 0);
        CHECK(g_moveDispatchCalls == 0);
        CHECK(g_attackDispatchCalls == 0);

        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 31) == 0);
        CHECK(input.NativeInput() == g_nativeInput);

        const auto noNativeCalls = [&] {
            const int moveBefore = g_moveInputCalls;
            const int fireBefore = g_fireInputCalls;
            CHECK(probe.Poll(
                &context, snapshot, status, input));
            CHECK(g_moveInputCalls == moveBefore);
            CHECK(g_fireInputCalls == fireBefore);
            CHECK(
                g_heldDetour(
                    g_nativeInput, nullptr, 0) == 0);
            CHECK(
                g_heldDetour(
                    g_nativeInput, nullptr, 9) == 0);
        };

        snapshot.native_app_state = 21;
        noNativeCalls();
        snapshot.native_app_state = 22;
        status.battle_start_sequence = 0;
        noNativeCalls();
        status.battle_start_sequence = 40;
        noNativeCalls();
        status.battle_start_sequence = 41;
        status.player_control_enabled = 0;
        noNativeCalls();
        status.player_control_enabled = 1;

        Put(g_unit, 0x04, kObjectId + 1);
        noNativeCalls();
        Put(g_unit, 0x04, kObjectId);
        Put(g_unit, 0x2B68, kPilotId + 1);
        noNativeCalls();
        Put(g_unit, 0x2B68, kPilotId);
        Put(
            g_manager, 0x88,
            static_cast<uint8_t*>(nullptr));
        noNativeCalls();
        Put(g_manager, 0x88, g_unit);
        Put(
            g_move, 0x21C,
            static_cast<uint8_t*>(nullptr));
        noNativeCalls();
        Put(g_move, 0x21C, g_unit);
        Put(g_unit, 0x38, int32_t(0));
        noNativeCalls();
        Put(g_unit, 0x38, int32_t(1000));
        g_unit[0x31E9] = 0;
        noNativeCalls();
        g_unit[0x31E9] = 1;

        const int32_t originalHp =
            Get<int32_t>(g_unit, 0x38);
        const int32_t originalSp =
            Get<int32_t>(g_unit, 0xBC8);
        const uint32_t originalPilot =
            Get<uint32_t>(g_unit, 0x2B68);
        const uint32_t originalObject =
            Get<uint32_t>(g_unit, 0x04);
        uintptr_t before = 0;
        uintptr_t after = 0;
        __asm mov before, esp
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        __asm mov after, esp
        CHECK(before == after);
        CHECK(g_moveInputCalls == 1);
        CHECK(g_fireInputCalls == 1);
        CHECK(g_moveDispatchCalls == 1);
        CHECK(g_attackDispatchCalls == 1);
        CHECK(g_lastMoveInput == g_nativeInput);
        CHECK(g_lastFireInput == g_nativeInput);
        CHECK(
            g_lastMoveContext == g_unit + 0xDC0);
        CHECK(
            g_lastAttackContext == g_unit + 0xDC0);
        CHECK(input.GameplayQueries(0) == 1);
        CHECK(input.GameplayQueries(9) == 0);
        CHECK(Get<float>(g_node, 0x88) == 2.0f);
        CHECK(
            Get<void*>(g_attack, 4) ==
            g_attack + 0x20);
        CHECK(Get<int32_t>(g_unit, 0x38) == originalHp);
        CHECK(Get<int32_t>(g_unit, 0xBC8) == originalSp);
        CHECK(
            Get<uint32_t>(g_unit, 0x2B68) ==
            originalPilot);
        CHECK(
            Get<uint32_t>(g_unit, 0x04) ==
            originalObject);
        CHECK(g_unit[0x31E9] == 1);

        context.elapsed_ms = 1180;
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(input.GameplayQueries(0) == 1);
        CHECK(input.GameplayQueries(9) == 1);
        CHECK(Get<float>(g_node, 0x88) == 2.0f);
        CHECK(
            Get<void*>(g_attack, 4) ==
            g_attack + 0x40);

        context.elapsed_ms = 1540;
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 0) == 0);
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0);

        context.elapsed_ms = 2080;
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(input.GameplayQueries(0) == 1);
        CHECK(input.GameplayQueries(9) == 2);
        CHECK(Get<float>(g_node, 0x88) == 2.0f);

        status.player_control_enabled = 0;
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 0) == 0);
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0);
        status.player_control_enabled = 1;

        Put(g_unit, 0x38, int32_t(0));
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 0) == 0);
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0);
        Put(g_unit, 0x38, int32_t(1000));

        snapshot.native_app_state = 4;
        CHECK(probe.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 0) == 0);
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0);

        __asm mov before, esp
        probe.Finish(&context, input);
        __asm mov after, esp
        CHECK(before == after);
        CHECK(g_assertions.size() == 3);
        if (g_assertions.size() == 3) {
            CHECK(
                g_assertions[0] == 1 &&
                g_assertions[1] == 1 &&
                g_assertions[2] == 1);
        }
        CHECK(g_observations.size() >= 3);
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 0) == 0);
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0);

        ResetNativeWorld();
        g_continuousSetting = 1;
        NativePlayerCombatProbe continuous;
        CHECK(continuous.Install(&api, nullptr));
        snapshot = Snapshot();
        status = StartStatus();
        context = Context(3000);
        const uint32_t fireQueriesBeforeContinuous =
            input.GameplayQueries(9);
        CHECK(continuous.Poll(
            &context, snapshot, status, input));
        context.elapsed_ms = 3180;
        CHECK(continuous.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 1);
        context.elapsed_ms = 3540;
        CHECK(continuous.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 1);
        CHECK(
            input.GameplayQueries(9) ==
            fireQueriesBeforeContinuous + 4);
        status.player_control_enabled = 0;
        CHECK(continuous.Poll(
            &context, snapshot, status, input));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0);
        continuous.Finish(&context, input);
        CHECK(g_assertions.size() == 3);
        if (g_assertions.size() == 3) {
            CHECK(
                g_assertions[0] == 1 &&
                g_assertions[1] == 1 &&
                g_assertions[2] == 1);
        }
        bool continuousObserved = false;
        for (const auto& message : g_observations)
            continuousObserved =
                continuousObserved ||
                message.find(
                    "continuous_primary=1") !=
                    std::string::npos;
        CHECK(continuousObserved);
    }

    void MissionLeaseTests()
    {
        ResetNativeWorld();
        SdgoApi api = Api();
        g_setting = 1;
        g_continuousSetting = 1;
        g_secondarySetting = 1;
        g_weaponSlotSetting = -1;
        g_symbolCalls = 0;

        ScoreboardActions input;
        CHECK(input.Install(&api, nullptr));
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 31) == 0);
        CHECK(input.NativeInput() == g_nativeInput);

        NativePlayerCombatProbe probe;
        CHECK(probe.Install(&api, nullptr));
        CHECK(g_symbolCalls == 6);

        auto mission = MissionSnapshot();
        const auto originalMission = mission;
        auto context = Context(5000);
        const auto noNativeCalls =
            [&](uint32_t expectedGeneration,
                uint32_t expectedRoom,
                uint32_t appState,
                uint32_t pilotId) {
                const int moveBefore =
                    g_moveInputCalls;
                const int fireBefore =
                    g_fireInputCalls;
                const int lockBefore =
                    g_lockInputCalls;
                CHECK(probe.PollMission(
                    &context, mission,
                    expectedGeneration,
                    expectedRoom,
                    appState, pilotId, input));
                CHECK(
                    g_moveInputCalls == moveBefore &&
                    g_fireInputCalls == fireBefore &&
                    g_lockInputCalls == lockBefore);
                CHECK(
                    g_heldDetour(
                        g_nativeInput,
                        nullptr, 0) == 0 &&
                    g_heldDetour(
                        g_nativeInput,
                        nullptr, 9) == 0 &&
                    g_heldDetour(
                        g_nativeInput,
                        nullptr, 10) == 0);
            };

        mission.active = 0;
        noNativeCalls(
            originalMission.battle_generation,
            originalMission.room_object,
            22, kPilotId);
        mission = originalMission;
        noNativeCalls(
            mission.battle_generation,
            mission.room_object,
            18, kPilotId);
        noNativeCalls(
            mission.battle_generation + 1,
            mission.room_object,
            22, kPilotId);
        noNativeCalls(
            mission.battle_generation,
            mission.room_object + 4,
            22, kPilotId);
        noNativeCalls(
            mission.battle_generation,
            mission.room_object,
            22, 0);

        CHECK(probe.PollMission(
            &context, mission,
            mission.battle_generation,
            mission.room_object,
            22, kPilotId, input));
        CHECK(
            g_moveInputCalls == 1 &&
            g_fireInputCalls == 1 &&
            g_lockInputCalls == 1);
        CHECK(g_moveHeld && !g_fireHeld && g_lockHeld);
        CHECK(
            input.GameplayQueries(0) == 1 &&
            input.GameplayQueries(9) == 0 &&
            input.GameplayQueries(10) == 1);
        CHECK(Get<float>(g_node, 0x88) == 2.0f);

        context.elapsed_ms = 5180;
        CHECK(probe.PollMission(
            &context, mission,
            mission.battle_generation,
            mission.room_object,
            22, kPilotId, input));
        CHECK(
            !g_moveHeld && g_fireHeld &&
            g_lockHeld);
        CHECK(
            Get<void*>(g_attack, 4) ==
            g_attack + 0x40);
        CHECK(
            std::memcmp(
                &mission, &originalMission,
                sizeof(mission)) == 0);

        probe.Finish(&context, input);
        CHECK(g_assertions.size() == 3);
        if (g_assertions.size() == 3) {
            CHECK(
                g_assertions[0] == 1 &&
                g_assertions[1] == 1 &&
                g_assertions[2] == 1);
        }
        CHECK(
            g_heldDetour(
                g_nativeInput, nullptr, 0) == 0 &&
            g_heldDetour(
                g_nativeInput, nullptr, 9) == 0 &&
            g_heldDetour(
                g_nativeInput, nullptr, 10) == 0);

        const int finishedMove =
            g_moveInputCalls;
        const int finishedFire =
            g_fireInputCalls;
        const int finishedLock =
            g_lockInputCalls;
        context.elapsed_ms = 9000;
        CHECK(probe.PollMission(
            &context, mission,
            mission.battle_generation,
            mission.room_object,
            18, kPilotId, input));
        CHECK(
            g_moveInputCalls == finishedMove &&
            g_fireInputCalls == finishedFire &&
            g_lockInputCalls == finishedLock);

        g_secondarySetting = 0;
        g_continuousSetting = 0;
    }

    void SpecialProfile(uint8_t rangeType)
    {
        RangedProfile();
        std::memset(g_specialWeapon, 0, sizeof(g_specialWeapon));
        Put(g_definition, 0x255, uint32_t(50001));
        Put(g_specialWeapon, 0, uint32_t(50001));
        g_specialWeapon[0x69] = 2;
        g_specialWeapon[0x6A] = rangeType;
        Put(g_specialWeapon, 0x6C, int32_t(4200));
        Put(g_specialWeapon, 0x72, uint32_t(450));
        for (size_t bank : {size_t(0x24C), size_t(0x56C)})
            Put(g_attack, bank + 3 * 0xC8 + 0xC, static_cast<void*>(g_specialWeapon));
        std::memset(g_enemy, 0, sizeof(g_enemy));
        std::memset(g_enemyNode, 0, sizeof(g_enemyNode));
        Put(g_enemy, 0x04, kEnemyObjectId);
        Put(g_enemy, 0x08, static_cast<void*>(g_enemyNode));
        Put(g_enemy, 0x38, int32_t(800));
        g_weaponAction = 17;
        g_specialCommit = true;
        g_specialSourceReady = true;
        g_specialEvents = 0;
    }

    // One enemy AI of the PvP snapshot, `distance` ahead of the local node.
    void PlaceEnemy(SdgoPvpAiClientSnapshotV1& snapshot, float distance)
    {
        const float x = Get<float>(g_node, 0x88) + distance;
        Put(g_enemyNode, 0x88, x);
        Put(g_enemyNode, 0x90, Get<float>(g_node, 0x90));
        snapshot.local_team = 1;
        snapshot.bot_count = 1;
        auto& bot = snapshot.bots[0];
        bot.pilot_id = kEnemyPilotId;
        bot.object_id = kEnemyObjectId;
        bot.team = 2;
        bot.valid_registered_object = 1;
        bot.position_valid = 1;
        bot.position_x = x;
        bot.position_z = Get<float>(g_node, 0x90);
    }

    bool Observed(const char* text)
    {
        for (const auto& line : g_observations)
            if (line.find(text) != std::string::npos) return true;
        return false;
    }

    void SpecialModeTests()
    {
        SdgoApi api = Api();
        g_setting = 1;
        g_continuousSetting = 1;
        g_secondarySetting = 0;
        g_weaponSlotSetting = -1;
        g_missingSymbol = nullptr;

        // Invalid special configurations are rejected before any binding.
        g_symbolCalls = 0;
        g_specialSetting = 2;
        NativePlayerCombatProbe invalidSpecial;
        CHECK(!invalidSpecial.Install(&api, nullptr));
        g_specialSetting = 1;
        g_setting = 0;
        NativePlayerCombatProbe specialWithoutCombat;
        CHECK(!specialWithoutCombat.Install(&api, nullptr));
        g_setting = 1;
        g_weaponSlotSetting = 2;
        NativePlayerCombatProbe specialWithSlot;
        CHECK(!specialWithSlot.Install(&api, nullptr));
        g_weaponSlotSetting = 3;
        NativePlayerCombatProbe specialSlot;
        CHECK(!specialSlot.Install(&api, nullptr));
        g_weaponSlotSetting = -1;
        CHECK(g_symbolCalls == 0);
        for (const char* symbol : {"test.player_secondary_input", "test.player_weapon_input"}) {
            g_missingSymbol = symbol;
            NativePlayerCombatProbe missing;
            CHECK(!missing.Install(&api, nullptr));
        }
        g_missingSymbol = nullptr;

        ResetNativeWorld();
        SpecialProfile(1);
        ScoreboardActions input;
        CHECK(input.Install(&api, nullptr));
        CHECK(g_heldDetour(g_nativeInput, nullptr, 31) == 0);
        g_symbolCalls = 0;
        NativePlayerCombatProbe probe;
        CHECK(probe.Install(&api, nullptr));
        CHECK(g_symbolCalls == 7 && probe.SpecialEnabled());
        probe.SetSpecialEventSource(&SpecialEvents);
        auto snapshot = Snapshot();
        auto status = StartStatus();
        auto context = Context(1000);

        // No enemy within the special's reach (RangeSize 450): ordinary
        // movement and primary fire, no gauge fill or special selection.
        PlaceEnemy(snapshot, 2000.0f);
        CHECK(probe.Poll(&context, snapshot, status, input));
        context.elapsed_ms = 1180;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 0 && g_fireHeld && !g_lockHeld);
        CHECK(Get<int32_t>(g_attack, 0x240) == 0 && g_specialEvents == 0);
        CHECK(probe.SpecialGauge() == 25000 && probe.SpecialAttempts() == 0);
        context.elapsed_ms = 11200;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(Observed("waits for an enemy within reach=450 nearest=1998") && g_weaponRequests == 0);
        // A dead enemy is no target.
        PlaceEnemy(snapshot, 300.0f);
        Put(g_enemy, 0x38, int32_t(0));
        context.elapsed_ms = 11250;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 0 && Get<int32_t>(g_unit, 0xBC8) == 25000);
        Put(g_enemy, 0x38, int32_t(800));

        // An enemy in reach: the fixture fills the gauge, the mapped down edge
        // of action 17 goes to the original consumer and the attack FSM is
        // dispatched; nothing fires this tick.
        const int fireBefore = g_fireInputCalls;
        const int dispatchBefore = g_attackDispatchCalls;
        context.elapsed_ms = 11300;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(Get<int32_t>(g_unit, 0xBC8) == 30000 && probe.SpecialFills() == 1);
        CHECK(g_weaponRequests == 1 && Get<int32_t>(g_unit, 0xDD0) == 3);
        CHECK(g_fireInputCalls == fireBefore && g_attackDispatchCalls == dispatchBefore + 1);
        CHECK(g_heldDetour(g_nativeInput, nullptr, 9) == 0);
        CHECK(probe.SpecialAttempts() == 1 &&
              Observed("attempt=1 target=80000011 distance=300 reach=450: fixture SP 25000 -> 30000"));

        // The committed AttackStyle-2 melee weapon fires with the lock held.
        context.elapsed_ms = 11320;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(Observed("slot=3 weapon=50001 range_type=1 damage_size=4200"));
        CHECK(g_fireHeld && g_lockHeld && g_lockInputCalls > 0);
        CHECK(g_specialEvents == 1 && probe.SpecialWeapon() == 50001 &&
              probe.SpecialFamily() == 1 && probe.SpecialDamageSize() == 4200);
        CHECK(g_heldDetour(g_nativeInput, nullptr, 17) == 0);
        context.elapsed_ms = 11340;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(probe.SpecialObserved() && Observed("special computed attempt=1 weapon=50001"));
        // Apart from the declared gauge fill nothing is written: not the weapon
        // row, the HP or the enemy.
        CHECK(Get<int32_t>(g_specialWeapon, 0x6C) == 4200 && Get<int32_t>(g_unit, 0x38) == 1000 &&
              Get<int32_t>(g_enemy, 0x38) == 800);
        const int requests = g_weaponRequests;
        context.elapsed_ms = 12000;
        CHECK(probe.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == requests);
        probe.Finish(&context, input);
        CHECK(g_assertions.size() == 5);
        for (int result : g_assertions) CHECK(result == 1);
        CHECK(Observed("fixture_fills=1") && Observed("computed=1"));

        // A minute without a target in reach doubles the reach. A selection that
        // is not committed and a special without any computation time out and
        // wait for a target again; a battle without the special fails its two
        // special assertions.
        ResetNativeWorld();
        SpecialProfile(2);
        g_specialCommit = false;
        NativePlayerCombatProbe timeout;
        CHECK(timeout.Install(&api, nullptr));
        timeout.SetSpecialEventSource(&SpecialEvents);
        snapshot = Snapshot();
        PlaceEnemy(snapshot, 800.0f);
        context.elapsed_ms = 10000;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        context.elapsed_ms = 70000;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(Observed("the reach doubles") && g_weaponRequests == 0);
        context.elapsed_ms = 70020;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(g_weaponRequests == 1 && Observed("reach=900: fixture SP 25000 -> 30000"));
        PlaceEnemy(snapshot, 300.0f);
        context.elapsed_ms = 76020;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(Observed("special slot not bound attempt=1"));
        context.elapsed_ms = 76040;
        g_specialCommit = true;
        g_specialComputes = false;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(timeout.SpecialAttempts() == 2 && Get<int32_t>(g_attack, 0x240) == 3 &&
              timeout.SpecialFills() == 1);
        // A bound ranged special fires without the lock.
        context.elapsed_ms = 76060;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(g_fireHeld && !g_lockHeld && g_specialEvents == 0);
        CHECK(Observed("slot=3 weapon=50001 range_type=2"));
        // The next case restarts elapsed_ms; the probe's own time continues.
        context.elapsed_ms = 100;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(!Observed("fired for") && g_fireHeld);
        context.elapsed_ms = 20100;
        CHECK(timeout.Poll(&context, snapshot, status, input));
        CHECK(Observed("special attempt=2 fired for 20100 ms") && !timeout.SpecialObserved());
        g_assertions.clear();
        timeout.Finish(&context, input);
        CHECK(g_assertions.size() == 5);
        if (g_assertions.size() == 5)
            CHECK(g_assertions[0] && g_assertions[1] && g_assertions[2] &&
                  g_assertions[3] && !g_assertions[4]);
        g_specialComputes = true;

        // An unreadable special source fails the case once a target is in reach.
        ResetNativeWorld();
        SpecialProfile(1);
        g_specialSourceReady = false;
        NativePlayerCombatProbe unreadable;
        CHECK(unreadable.Install(&api, nullptr));
        unreadable.SetSpecialEventSource(&SpecialEvents);
        snapshot = Snapshot();
        PlaceEnemy(snapshot, 300.0f);
        context.elapsed_ms = 50000;
        CHECK(!unreadable.Poll(&context, snapshot, status, input));
        CHECK(g_assertions.size() == 1 && !g_assertions[0]);

        g_specialSetting = 0;
        g_weaponAction = 16;
        g_specialCommit = false;
        g_specialSourceReady = true;
        g_continuousSetting = 0;
    }
}

int main()
{
    InstallTests();
    GatingAndAbiTests();
    MissionLeaseTests();
    RangedWeaponAcknowledgementTests();
    SpecialModeTests();
    std::cout << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
