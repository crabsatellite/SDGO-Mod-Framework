#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "mods/game_test_client/native_player_combat_probe.h"

namespace
{
    int g_checks = 0;
    int g_failures = 0;
    int g_setting = 1;
    int g_continuousSetting = 0;
    int g_symbolCalls = 0;
    const char* g_missingSymbol = nullptr;

    using QueryDetour =
        uint32_t(__fastcall*)(void*, void*, uint32_t);
    QueryDetour g_pressedDetour = nullptr;
    QueryDetour g_heldDetour = nullptr;
    QueryDetour g_downDetour = nullptr;

    constexpr uint32_t kPilotId = 77001;
    constexpr uint32_t kObjectId = 7;
    uint8_t g_manager[0x100] = {};
    uint8_t* g_managerPointer = g_manager;
    uint8_t g_unit[0x3300] = {};
    uint8_t g_node[0x200] = {};
    uint8_t g_move[0x300] = {};
    uint8_t g_attack[0x300] = {};
    void* g_moveVtable[2] = {};
    void* g_attackVtable[2] = {};
    uint8_t g_nativeInput[0x40] = {};

    int g_moveInputCalls = 0;
    int g_fireInputCalls = 0;
    int g_moveDispatchCalls = 0;
    int g_attackDispatchCalls = 0;
    bool g_moveHeld = false;
    bool g_fireHeld = false;
    void* g_lastMoveInput = nullptr;
    void* g_lastFireInput = nullptr;
    void* g_lastMoveContext = nullptr;
    void* g_lastAttackContext = nullptr;
    std::vector<std::string> g_observations;
    std::vector<int> g_assertions;

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
        return objectId == kObjectId ? g_unit : nullptr;
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
            Put(
                g_attack, 4,
                static_cast<void*>(g_attack + 0x40));
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
        g_moveDispatchCalls = 0;
        g_attackDispatchCalls = 0;
        g_moveHeld = false;
        g_fireHeld = false;
        g_lastMoveInput = nullptr;
        g_lastFireInput = nullptr;
        g_lastMoveContext = nullptr;
        g_lastAttackContext = nullptr;
        g_observations.clear();
        g_assertions.clear();
    }

    void InstallTests()
    {
        SdgoApi api = Api();
        g_setting = 0;
        g_continuousSetting = 0;
        g_symbolCalls = 0;
        g_missingSymbol = nullptr;
        NativePlayerCombatProbe disabled;
        CHECK(disabled.Install(&api, nullptr));
        CHECK(g_symbolCalls == 0);

        g_setting = 2;
        NativePlayerCombatProbe invalid;
        CHECK(!invalid.Install(&api, nullptr));
        CHECK(g_symbolCalls == 0);

        g_setting = 1;
        g_continuousSetting = 2;
        NativePlayerCombatProbe invalidContinuous;
        CHECK(!invalidContinuous.Install(&api, nullptr));
        CHECK(g_symbolCalls == 0);

        g_continuousSetting = 0;
        g_missingSymbol =
            "test.apsalus_primary_input";
        NativePlayerCombatProbe missing;
        CHECK(!missing.Install(&api, nullptr));
        CHECK(g_symbolCalls == 5);
        g_missingSymbol = nullptr;
    }

    void GatingAndAbiTests()
    {
        ResetNativeWorld();
        SdgoApi api = Api();
        g_setting = 1;
        g_continuousSetting = 0;
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
}

int main()
{
    InstallTests();
    GatingAndAbiTests();
    std::cout << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
