#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sdgo/operator_effects.h"
#include "sdgo/operator_reward_filter.h"
#include "sdgo/sdgo_api.h"

static_assert(
    sizeof(void*) == 4,
    "operator catalog server native fixture requires an x86 build");

namespace {
namespace fs = std::filesystem;

constexpr size_t kRoomSize = 0x33000;
constexpr size_t kRoomMode = 0x05;
constexpr size_t kRoomPilotBase = 0x533;
constexpr size_t kRoomPilotStride = 0x48E;
constexpr size_t kRoomPilotId = 0x429;
constexpr size_t kRoomPilotTeam = 0x42D;
constexpr size_t kBattlePilotBase = 0x3CE4;
constexpr size_t kBattlePilotStride = 0x1F54;
constexpr size_t kBattlePilotState = 0x22;
constexpr size_t kBattlePilotOutcome = 0x1D28;
constexpr size_t kBattlePilotOperatorDefinition = 0x50;
constexpr size_t kRoomStartedAt = 0x2EEB4;

struct FrameContract {
    ptrdiff_t room;
    ptrdiff_t duration;
    ptrdiff_t room_pilot;
    ptrdiff_t battle_pilot;
    ptrdiff_t physical_slot;
    ptrdiff_t rank;
    ptrdiff_t eligibility;
    ptrdiff_t mission_outcomes;
};

constexpr std::array<FrameContract, 10> kFrames = {{
    {-0xB48, -0x880, -0x244, -0x7E0, -0x88C, -0x914, -0x7B8, 0},
    {-0xBBC, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {-0xBBC, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {-0xBBC, -0x840, -0x23C, -0x7C8, -0x8FC, -0x984, -0x7A0, 0},
    {-0xBD0, -0x850, -0x23C, -0x7CC, -0x90C, -0x994, -0x7A4, 0},
    {-0xBD0, -0x840, -0x234, -0x7C8, -0x8FC, -0x990, -0x7A0, 0},
    {-0xBD0, -0x840, -0x234, -0x7C8, -0x8FC, -0x990, -0x7A0, 0},
    {-0xBC0, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {-0xBC8, -0x838, -0x234, -0x7BC, -0x8FC, -0x984, -0x798, 0},
    {-0xB58, -0x844, -0x234, -0x7C8, -0x854, -0x8D8, -0x7A0, -0x828},
}};

constexpr std::array<std::array<ptrdiff_t, 6>, 10> kRewardFields = {{
    {{-0x770, -0x28, -0x89C, -0x18, -0x87C, -0x844}},
    {{-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
    {{-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
    {{-0x760, -0x24, -0x908, -0x14, -0x83C, -0x834}},
    {{-0x764, -0x24, -0x918, -0x14, -0x844, -0x83C}},
    {{-0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834}},
    {{-0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834}},
    {{-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
    {{-0x758, -0x20, -0x90C, -0x10, -0x834, -0x82C}},
    {{-0x760, -0x20, -0x860, -0x10, -0x840, -0x834}},
}};

constexpr std::array<const char*, 11> kExpectedHooks = {{
    "battle.rewards.augment.personal",
    "battle.rewards.augment.team",
    "battle.rewards.augment.leader",
    "battle.rewards.augment.ai",
    "battle.rewards.augment.ai_ship",
    "battle.rewards.augment.mode13",
    "battle.rewards.augment.mode14",
    "battle.rewards.augment.tag",
    "battle.rewards.augment.death_grid",
    "battle.rewards.augment.mission",
    "battle.rewards.final.mission",
}};

struct HookRecord {
    std::string symbol;
    void* detour = nullptr;
};

int g_checks = 0;
int g_failures = 0;
std::wstring g_modDirectory;
std::vector<HookRecord> g_hooks;
std::vector<std::string> g_logs;
int g_extensionMode = 0;
void* const kExtensionUser = reinterpret_cast<void*>(0x41325768u);

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL line " << line << ": " << expression << "\n";
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

template <typename T>
void Put(BYTE* base, ptrdiff_t offset, const T& value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

template <typename T>
T Get(const BYTE* base, ptrdiff_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

void __cdecl Log(SdgoMod*, int, const char* format, ...) {
    char buffer[1024] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, arguments);
    va_end(arguments);
    g_logs.emplace_back(buffer);
}

__declspec(naked) void ResumeSite() {
    __asm { ret }
}

int __cdecl HookSite(
    SdgoMod*, const char* symbol, void* detour, void** original) {
    if (!symbol || !detour || !original) return 0;
    g_hooks.push_back({symbol, detour});
    *original = reinterpret_cast<void*>(&ResumeSite);
    return 1;
}

const wchar_t* __cdecl ModDirectory(SdgoMod*) {
    return g_modDirectory.c_str();
}

SdgoApi MakeApi() {
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &Log;
    api.ModDirectory = &ModDirectory;
    api.HookSite = &HookSite;
    return api;
}

__declspec(naked) void __cdecl InvokeDetour(void*, BYTE*) {
    __asm { push ebp }
    __asm { mov ebp, [esp + 0Ch] }
    __asm { call dword ptr [esp + 08h] }
    __asm { pop ebp }
    __asm { ret }
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return !!output;
}

std::string Hex(const std::string& text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size() * 2);
    for (unsigned char byte : text) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

std::string Configuration(
    const std::string& extensions = "{}", unsigned percent = 10,
    unsigned operator_number = 60001) {
    return
        "[effects]\n"
        "Version=1\n"
        "Revision=0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef\n\n"
        "[operator:" + std::to_string(operator_number) + "]\n"
        "PilotExpPercent=" + std::to_string(percent) + "\n"
        "ExtensionsHex=" + Hex(extensions) + "\n";
}

fs::path NewRuntime(const std::wstring& label) {
    std::error_code error;
    const fs::path root =
        fs::temp_directory_path() /
        (L"sdgo-operator-effects-" + label + L"-" +
         std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root, error);
    CHECK(!error);
    return root;
}

using ModLoadFn = int(__cdecl*)(const SdgoApi*, SdgoMod*);
using RegisterExtensionFn = int(__cdecl*)(
    const SdgoOperatorEffectExtensionSinkV1*);

struct LoadedMod {
    HMODULE module = nullptr;
    ModLoadFn load = nullptr;
    RegisterExtensionFn register_extension = nullptr;
};

LoadedMod OpenMod(const fs::path& path) {
    LoadedMod result;
    result.module = LoadLibraryW(path.c_str());
    if (!result.module) return result;
    result.load = reinterpret_cast<ModLoadFn>(
        GetProcAddress(result.module, "SdgoModLoad"));
    result.register_extension = reinterpret_cast<RegisterExtensionFn>(
        GetProcAddress(
            result.module,
            SDGO_OPERATOR_EFFECTS_EXTENSION_EXPORT));
    return result;
}

using FixtureResetFn = void(__cdecl*)();
using FixtureSetResultFn = void(__cdecl*)(int);
using FixtureHasSinkFn = int(__cdecl*)();
using FixtureInvokeFn = int(__cdecl*)(
    const SdgoOperatorRewardContextV1*, SdgoOperatorRewardResultV1*);

struct RewardFixture {
    HMODULE module = nullptr;
    FixtureResetFn reset = nullptr;
    FixtureSetResultFn set_result = nullptr;
    FixtureHasSinkFn has_sink = nullptr;
    FixtureInvokeFn invoke = nullptr;
};

RewardFixture OpenRewardFixture(const fs::path& path, int result) {
    RewardFixture fixture;
    fixture.module = LoadLibraryW(path.c_str());
    if (!fixture.module) return fixture;
    fixture.reset = reinterpret_cast<FixtureResetFn>(
        GetProcAddress(
            fixture.module, "OperatorRewardFilterFixtureReset"));
    fixture.set_result = reinterpret_cast<FixtureSetResultFn>(
        GetProcAddress(
            fixture.module,
            "OperatorRewardFilterFixtureSetRegistrationResult"));
    fixture.has_sink = reinterpret_cast<FixtureHasSinkFn>(
        GetProcAddress(
            fixture.module, "OperatorRewardFilterFixtureHasSink"));
    fixture.invoke = reinterpret_cast<FixtureInvokeFn>(
        GetProcAddress(
            fixture.module, "OperatorRewardFilterFixtureInvoke"));
    if (fixture.reset && fixture.set_result) {
        fixture.reset();
        fixture.set_result(result);
    }
    return fixture;
}

bool StatusHas(
    const fs::path& runtime, const std::string& mode,
    unsigned profiles = 1) {
    const std::string status = ReadText(runtime / L"status.json");
    return
        status.find(
            "\"pid\": " + std::to_string(GetCurrentProcessId())) !=
            std::string::npos &&
        status.find("\"version\": 1") != std::string::npos &&
        status.find(
            "\"revision\": \"0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef\"") !=
            std::string::npos &&
        status.find(
            "\"loaded_profiles\": " + std::to_string(profiles)) !=
            std::string::npos &&
        status.find(
            "\"native_hook_mode\": \"" + mode + "\"") !=
            std::string::npos;
}

struct SharedNative {
    std::array<BYTE, 0x100> frame{};
    std::array<BYTE, 0x100> room{};
    std::array<BYTE, 0x100> room_pilot{};
    std::array<BYTE, 0x100> battle_pilot{};
    std::array<BYTE, 0x80> definition{};
    std::array<BYTE, 0x20> win_skill{};
    std::array<BYTE, 0x20> other_skill{};
    std::array<BYTE, 0x20> general_skill{};
    std::array<BYTE, 0x20> timed_skill{};
    std::array<BYTE, 0x20> timed_rule{};

    SharedNative() {
        Put(definition.data(), 0, uint32_t{60001});
        Put(
            battle_pilot.data(), kBattlePilotOperatorDefinition,
            reinterpret_cast<uintptr_t>(definition.data()));
    }

    void ConfigureGp(
        int32_t general, int32_t win, int32_t other,
        bool timed = false, int32_t timed_percent = 0) {
        Put(general_skill.data(), 8, general);
        Put(win_skill.data(), 8, win);
        Put(other_skill.data(), 8, other);
        Put(timed_skill.data(), 8, timed_percent);
        Put(
            battle_pilot.data(), 0x54,
            reinterpret_cast<uintptr_t>(win_skill.data()));
        Put(
            battle_pilot.data(), 0x58,
            reinterpret_cast<uintptr_t>(other_skill.data()));
        Put(
            battle_pilot.data(), 0x5C,
            reinterpret_cast<uintptr_t>(general_skill.data()));
        Put(
            definition.data(), 0x38,
            timed ? reinterpret_cast<uintptr_t>(timed_rule.data())
                  : uintptr_t{0});
        if (timed) {
            Put(timed_rule.data(), 0x08, uint8_t{0});
            Put(timed_rule.data(), 0x09, uint8_t{0});
            Put(timed_rule.data(), 0x0A, uint8_t{23});
            Put(timed_rule.data(), 0x0B, uint8_t{59});
            Put(
                timed_rule.data(), 0x0C,
                reinterpret_cast<uintptr_t>(timed_skill.data()));
        }
    }

    SdgoOperatorRewardContextV1 Context(
        uint32_t writable_mask, uint32_t gp_policy =
            SDGO_OPERATOR_REWARD_GP_NATIVE) {
        SdgoOperatorRewardContextV1 context{};
        context.size = sizeof(context);
        context.version = SDGO_OPERATOR_REWARD_FILTER_VERSION;
        context.site = 0;
        context.phase = SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT;
        context.writable_mask = writable_mask;
        context.frame = frame.data();
        context.room = room.data();
        context.room_pilot = room_pilot.data();
        context.battle_pilot = battle_pilot.data();
        context.operator_definition = definition.data();
        context.account = 7001;
        context.participant_role = SDGO_OPERATOR_REWARD_ROLE_HUMAN;
        context.outcome = SDGO_OPERATOR_REWARD_OUTCOME_WIN_OR_CLEAR;
        context.won = 1;
        context.native_eligible = 1;
        context.gp_policy = gp_policy;
        context.native_pilot_exp = 101;
        context.native_pilot_exp_event = 2;
        context.native_unit_exp = 202;
        context.native_unit_exp_event = 3;
        context.native_gp = 303;
        context.native_gp_event = 4;
        context.current_pilot_exp = 101;
        context.current_pilot_exp_event = 2;
        context.current_unit_exp = 202;
        context.current_unit_exp_event = 3;
        context.current_gp = 303;
        context.current_gp_event = 4;
        return context;
    }
};

SdgoOperatorRewardResultV1 NewResult() {
    return {
        sizeof(SdgoOperatorRewardResultV1),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        0,
        0x11111111u,
        0x22222222u,
        0x33333333u};
}

int __stdcall ExtensionCallback(
    const SdgoOperatorEffectExtensionContextV1* context,
    SdgoOperatorEffectExtensionResultV1* result, void* user) {
    if (!context || context->size < sizeof(*context) ||
        context->version != SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION ||
        !result || result->size < sizeof(*result) ||
        result->version != SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION ||
        user != kExtensionUser ||
        context->operator_number != 60001 ||
        context->extension_namespace_size != 12 ||
        std::memcmp(
            context->extension_namespace, "fixture.unit", 12) != 0 ||
        context->payload_json_size != 11 ||
        std::memcmp(context->payload_json, "{\"delta\":7}", 11) != 0) {
        return SDGO_OPERATOR_EFFECT_EXTENSION_REJECT;
    }
    if (g_extensionMode == 1) {
        result->write_mask = SDGO_OPERATOR_REWARD_FIELD_GP;
        result->target_gp = context->current_gp + 1;
        return SDGO_OPERATOR_EFFECT_EXTENSION_APPLY;
    }
    if (g_extensionMode == 2) {
        return SDGO_OPERATOR_EFFECT_EXTENSION_REJECT;
    }
    if (g_extensionMode == 3) {
        return SDGO_OPERATOR_EFFECT_EXTENSION_NO_CHANGE;
    }
    if (g_extensionMode == 4) {
        result->write_mask = context->writable_mask;
        result->target_pilot_exp = context->current_pilot_exp + 1;
        result->target_unit_exp = context->current_unit_exp + 2;
        result->target_gp = context->current_gp + 3;
        return SDGO_OPERATOR_EFFECT_EXTENSION_APPLY;
    }
    if ((context->writable_mask &
         SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP) == 0) {
        return SDGO_OPERATOR_EFFECT_EXTENSION_NO_CHANGE;
    }
    result->write_mask = SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP;
    result->target_unit_exp = context->current_unit_exp + 7;
    return SDGO_OPERATOR_EFFECT_EXTENSION_APPLY;
}

bool RegisterExtension(RegisterExtensionFn function) {
    if (!function) return false;
    const SdgoOperatorEffectExtensionSinkV1 sink = {
        sizeof(SdgoOperatorEffectExtensionSinkV1),
        SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION,
        "fixture.unit",
        12,
        &ExtensionCallback,
        kExtensionUser};
    return function(&sink) == 1;
}

struct MatchFixture {
    std::vector<BYTE> room = std::vector<BYTE>(kRoomSize);
    std::array<BYTE, 0x80> definition{};

    BYTE* RoomPilot(unsigned slot = 0) {
        return room.data() + kRoomPilotBase + slot * kRoomPilotStride;
    }

    BYTE* BattlePilot(unsigned slot = 0) {
        return room.data() + kBattlePilotBase + slot * kBattlePilotStride;
    }

    void Prepare(
        uint32_t room_id, uint32_t started, uint32_t account,
        uint32_t operator_number = 60001, bool ai = false,
        uint8_t state = 3) {
        Put(room.data(), 0, room_id);
        Put(room.data(), kRoomMode, uint8_t{4});
        Put(room.data(), kRoomStartedAt, started);
        Put(
            RoomPilot(), kRoomPilotId,
            ai ? account | 0x80000000u : account);
        Put(RoomPilot(), kRoomPilotTeam, uint8_t{1});
        Put(BattlePilot(), kBattlePilotState, state);
        Put(BattlePilot(), kBattlePilotOutcome, uint8_t{1});
        Put(definition.data(), 0, operator_number);
        Put(
            BattlePilot(), kBattlePilotOperatorDefinition,
            reinterpret_cast<uintptr_t>(definition.data()));
    }
};

struct FrameFixture {
    std::array<BYTE, 0xD00> storage{};

    BYTE* Frame() {
        return storage.data() + 0xC80;
    }

    void Prepare(MatchFixture* match, unsigned site) {
        BYTE* frame = Frame();
        const FrameContract& contract = kFrames[site];
        Put(
            frame, contract.room,
            reinterpret_cast<uintptr_t>(match->room.data()));
        Put(frame, contract.duration, uint32_t{180});
        Put(
            frame, contract.room_pilot,
            reinterpret_cast<uintptr_t>(match->RoomPilot()));
        Put(
            frame, contract.battle_pilot,
            reinterpret_cast<uintptr_t>(match->BattlePilot()));
        Put(frame, contract.physical_slot, int32_t{0});
        Put(frame, contract.rank, int32_t{0});
        Put(frame, contract.eligibility, uint32_t{0});
        if (contract.mission_outcomes != 0) {
            Put(frame, contract.mission_outcomes, uint32_t{1});
        }
        const auto& fields = kRewardFields[site];
        Put(frame, fields[0], uint16_t{100});
        Put(frame, fields[1], uint16_t{22});
        Put(frame, fields[2], uint16_t{200});
        Put(frame, fields[3], uint16_t{2});
        Put(frame, fields[4], uint16_t{3});
        Put(frame, fields[5], uint16_t{4});
    }
};

bool HooksAreExact() {
    if (g_hooks.size() != kExpectedHooks.size()) return false;
    for (size_t i = 0; i < kExpectedHooks.size(); ++i) {
        if (g_hooks[i].symbol != kExpectedHooks[i] ||
            !g_hooks[i].detour) {
            return false;
        }
        if (g_hooks[i].symbol.find("daily") != std::string::npos ||
            g_hooks[i].symbol.find("battle.point") != std::string::npos) {
            return false;
        }
    }
    return true;
}

int LoadActive(LoadedMod& mod, const fs::path& runtime) {
    g_modDirectory = runtime.wstring();
    SdgoApi api = MakeApi();
    return mod.load
        ? mod.load(&api, reinterpret_cast<SdgoMod*>(0x1234u))
        : 0;
}

void SharedCases(
    const fs::path& mod_path, const fs::path& fixture_path) {
    const fs::path runtime = NewRuntime(L"shared");
    CHECK(WriteText(runtime / L"effects.ini", Configuration()));
    RewardFixture fixture = OpenRewardFixture(fixture_path, 1);
    CHECK(fixture.module && fixture.reset && fixture.set_result &&
          fixture.has_sink && fixture.invoke);
    LoadedMod mod = OpenMod(mod_path);
    CHECK(mod.module && mod.load && mod.register_extension);
    g_hooks.clear();
    CHECK(LoadActive(mod, runtime) == 1);
    CHECK(fixture.has_sink && fixture.has_sink() == 1);
    CHECK(g_hooks.empty());
    CHECK(StatusHas(runtime, "shared"));

    SharedNative native;
    native.ConfigureGp(10, 20, -15, true, 30);
    auto context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
        SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
        SDGO_OPERATOR_REWARD_FIELD_GP);
    auto result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.write_mask == SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP);
    CHECK(result.target_pilot_exp == 111);
    CHECK(result.target_unit_exp == 202);
    CHECK(result.target_gp == 303);

    context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_GP,
        SDGO_OPERATOR_REWARD_GP_REPLACE);
    context.current_gp = 101;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.write_mask == SDGO_OPERATOR_REWARD_FIELD_GP);
    CHECK(result.target_gp == 151);

    native.ConfigureGp(-15, 20, -15, false, 0);
    context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_GP,
        SDGO_OPERATOR_REWARD_GP_REPLACE);
    context.current_gp = 101;
    context.outcome = SDGO_OPERATOR_REWARD_OUTCOME_LOSE_OR_FAIL;
    context.won = 0;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.target_gp == 71);

    native.ConfigureGp(10, 0, 0, true, 30);
    std::array<BYTE, 0x20> second_timed_rule{};
    std::array<BYTE, 0x20> second_timed_skill{};
    Put(native.timed_rule.data(), 0x0C, uintptr_t{0});
    Put(second_timed_rule.data(), 0x08, uint8_t{0});
    Put(second_timed_rule.data(), 0x09, uint8_t{0});
    Put(second_timed_rule.data(), 0x0A, uint8_t{23});
    Put(second_timed_rule.data(), 0x0B, uint8_t{59});
    Put(second_timed_skill.data(), 8, int32_t{80});
    Put(
        second_timed_rule.data(), 0x0C,
        reinterpret_cast<uintptr_t>(second_timed_skill.data()));
    Put(
        native.definition.data(), 0x3C,
        reinterpret_cast<uintptr_t>(second_timed_rule.data()));
    context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_GP,
        SDGO_OPERATOR_REWARD_GP_REPLACE);
    context.current_gp = 101;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.target_gp == 111);

    native.ConfigureGp(100, 100, 100, false, 0);
    context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_GP,
        SDGO_OPERATOR_REWARD_GP_REPLACE);
    context.current_gp = 65500;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.target_gp == 65535);

    context = native.Context(SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP);
    context.native_eligible = 0;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
    context.native_eligible = 1;
    context.participant_role = SDGO_OPERATOR_REWARD_ROLE_AI;
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
    context.participant_role = SDGO_OPERATOR_REWARD_ROLE_HUMAN;
    context.operator_definition = nullptr;
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
    context.operator_definition = native.definition.data();
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.write_mask == SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP);
    CHECK(result.target_pilot_exp == 111);

    context.operator_definition =
        reinterpret_cast<void*>(native.definition.data() + 4);
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
    context.operator_definition = native.definition.data();
    Put(native.definition.data(), 0, uint32_t{60002});
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
}

void ExtensionCases(
    const fs::path& mod_path, const fs::path& fixture_path) {
    const fs::path runtime = NewRuntime(L"extension");
    CHECK(WriteText(
        runtime / L"effects.ini",
        Configuration("{\"fixture.unit\":{\"delta\":7}}")));
    RewardFixture fixture = OpenRewardFixture(fixture_path, 1);
    CHECK(fixture.module && fixture.has_sink && fixture.invoke);
    LoadedMod mod = OpenMod(mod_path);
    CHECK(mod.module && mod.load && mod.register_extension);
    CHECK(RegisterExtension(mod.register_extension));

    const SdgoOperatorEffectExtensionSinkV1 duplicate = {
        sizeof(SdgoOperatorEffectExtensionSinkV1),
        SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION,
        "fixture.unit",
        12,
        &ExtensionCallback,
        kExtensionUser};
    CHECK(mod.register_extension(&duplicate) == 0);
    SdgoOperatorEffectExtensionSinkV1 invalid = duplicate;
    invalid.extension_namespace = "fixture/unit";
    CHECK(mod.register_extension(&invalid) == 0);

    void* heap_callback = VirtualAlloc(
        nullptr, 16, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    CHECK(heap_callback != nullptr);
    if (heap_callback) {
        const BYTE code[] = {
            0xB8, 0x01, 0x00, 0x00, 0x00,
            0xC2, 0x0C, 0x00};
        std::memcpy(heap_callback, code, sizeof(code));
        SdgoOperatorEffectExtensionSinkV1 heap = duplicate;
        heap.extension_namespace = "fixture.heap";
        heap.extension_namespace_size = 12;
        heap.callback =
            reinterpret_cast<SdgoOperatorEffectExtensionCallbackV1>(
                heap_callback);
        CHECK(mod.register_extension(&heap) == 0);
        CHECK(VirtualFree(heap_callback, 0, MEM_RELEASE) != 0);
    }

    CHECK(LoadActive(mod, runtime) == 1);
    CHECK(fixture.has_sink && fixture.has_sink() == 1);
    SharedNative native;
    auto context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
        SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP);
    auto result = NewResult();
    g_extensionMode = 0;
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(
        result.write_mask ==
        (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
         SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP));
    CHECK(result.target_pilot_exp == 111);
    CHECK(result.target_unit_exp == 209);

    g_extensionMode = 1;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
    CHECK(result.write_mask == 0);
    CHECK(result.target_pilot_exp == 0x11111111u);

    g_extensionMode = 2;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);

    g_extensionMode = 3;
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(result.write_mask == SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP);
    CHECK(result.target_pilot_exp == 111);

    g_extensionMode = 4;
    context = native.Context(
        SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
        SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
        SDGO_OPERATOR_REWARD_FIELD_GP);
    result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_APPLY);
    CHECK(
        result.write_mask ==
        (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
         SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
         SDGO_OPERATOR_REWARD_FIELD_GP));
    CHECK(result.target_pilot_exp == 112);
    CHECK(result.target_unit_exp == 204);
    CHECK(result.target_gp == 306);
}

void UnknownExtensionCase(
    const fs::path& mod_path, const fs::path& fixture_path) {
    const fs::path runtime = NewRuntime(L"unknown-extension");
    CHECK(WriteText(
        runtime / L"effects.ini",
        Configuration("{\"required.missing\":true}")));
    RewardFixture fixture = OpenRewardFixture(fixture_path, 1);
    LoadedMod mod = OpenMod(mod_path);
    CHECK(fixture.module && fixture.invoke && mod.module && mod.load);
    CHECK(LoadActive(mod, runtime) == 1);
    SharedNative native;
    auto context =
        native.Context(SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP);
    auto result = NewResult();
    CHECK(fixture.invoke(&context, &result) ==
          SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE);
    CHECK(result.write_mask == 0);
    CHECK(result.target_pilot_exp == 0x11111111u);
}

void MaximumExtensionJsonCase(
    const fs::path& mod_path, const fs::path& fixture_path) {
    const fs::path runtime = NewRuntime(L"maximum-json");
    const std::string json =
        std::string("{\"required.maximum\":\"") +
        std::string(
            SDGO_OPERATOR_EFFECTS_MAX_EXTENSION_JSON - 23, 'a') +
        "\"}";
    CHECK(
        json.size() == SDGO_OPERATOR_EFFECTS_MAX_EXTENSION_JSON);
    CHECK(WriteText(
        runtime / L"effects.ini", Configuration(json)));
    RewardFixture fixture = OpenRewardFixture(fixture_path, 1);
    LoadedMod mod = OpenMod(mod_path);
    CHECK(fixture.module && fixture.has_sink &&
          mod.module && mod.load);
    CHECK(LoadActive(mod, runtime) == 1);
    CHECK(fixture.has_sink && fixture.has_sink() == 1);
    CHECK(StatusHas(runtime, "shared"));
}

void VerifyFallbackLoad(
    LoadedMod& mod, const fs::path& runtime,
    const std::string& expected_mode = "fallback") {
    g_hooks.clear();
    CHECK(LoadActive(mod, runtime) == 1);
    CHECK(HooksAreExact());
    CHECK(StatusHas(runtime, expected_mode));
}

void FallbackCases(
    const fs::path& mod_path, const fs::path* fixture_path,
    int registration_result, bool full) {
    const fs::path runtime = NewRuntime(
        registration_result == 0 ? L"fallback-refused"
                                 : registration_result == 2
                                     ? L"fallback-nonone"
                                     : L"fallback-missing");
    CHECK(WriteText(runtime / L"effects.ini", Configuration()));
    RewardFixture fixture;
    if (fixture_path) {
        fixture = OpenRewardFixture(*fixture_path, registration_result);
        CHECK(fixture.module && fixture.has_sink);
    }
    LoadedMod mod = OpenMod(mod_path);
    CHECK(mod.module && mod.load);
    VerifyFallbackLoad(mod, runtime);
    if (fixture_path) {
        CHECK(fixture.has_sink && fixture.has_sink() == 0);
    }
    if (!full || !HooksAreExact()) return;

    MatchFixture match;
    match.Prepare(0x1001, 1700000001u, 7001);
    FrameFixture frame;
    frame.Prepare(&match, 0);
    BYTE* native = frame.Frame();
    const auto& fields = kRewardFields[0];
    InvokeDetour(g_hooks[0].detour, native);
    CHECK(Get<uint16_t>(native, fields[0]) == 110);
    CHECK(Get<uint16_t>(native, fields[3]) == 12);
    CHECK(Get<uint16_t>(native, fields[2]) == 200);
    CHECK(Get<uint16_t>(native, fields[5]) == 4);
    CHECK(Get<uint16_t>(native, fields[1]) == 22);
    CHECK(Get<uint16_t>(native, fields[4]) == 3);
    InvokeDetour(g_hooks[0].detour, native);
    CHECK(Get<uint16_t>(native, fields[0]) == 110);

    MatchFixture ai;
    ai.Prepare(0x1002, 1700000002u, 7002, 60001, true);
    FrameFixture ai_frame;
    ai_frame.Prepare(&ai, 0);
    InvokeDetour(g_hooks[0].detour, ai_frame.Frame());
    CHECK(Get<uint16_t>(ai_frame.Frame(), fields[0]) == 100);

    MatchFixture ineligible;
    ineligible.Prepare(
        0x1003, 1700000003u, 7003, 60001, false, 2);
    FrameFixture ineligible_frame;
    ineligible_frame.Prepare(&ineligible, 0);
    InvokeDetour(g_hooks[0].detour, ineligible_frame.Frame());
    CHECK(Get<uint16_t>(ineligible_frame.Frame(), fields[0]) == 100);

    MatchFixture no_definition;
    no_definition.Prepare(
        0x1005, 1700000005u, 7005);
    Put(
        no_definition.BattlePilot(),
        kBattlePilotOperatorDefinition, uintptr_t{0});
    FrameFixture no_definition_frame;
    no_definition_frame.Prepare(&no_definition, 0);
    InvokeDetour(g_hooks[0].detour, no_definition_frame.Frame());
    CHECK(
        Get<uint16_t>(no_definition_frame.Frame(), fields[0]) ==
        100);

    MatchFixture unconfigured;
    unconfigured.Prepare(
        0x1004, 1700000004u, 7004, 60002);
    FrameFixture unconfigured_frame;
    unconfigured_frame.Prepare(&unconfigured, 0);
    InvokeDetour(g_hooks[0].detour, unconfigured_frame.Frame());
    CHECK(Get<uint16_t>(unconfigured_frame.Frame(), fields[0]) == 100);

    MatchFixture later_valid;
    later_valid.Prepare(
        0x1006, 1700000006u, 7006);
    FrameFixture later_valid_frame;
    later_valid_frame.Prepare(&later_valid, 0);
    InvokeDetour(g_hooks[0].detour, later_valid_frame.Frame());
    CHECK(Get<uint16_t>(later_valid_frame.Frame(), fields[0]) == 110);
    CHECK(Get<uint16_t>(later_valid_frame.Frame(), fields[3]) == 12);
}

void FallbackMissionCase(
    const fs::path& mod_path, const fs::path& fixture_path) {
    const fs::path runtime = NewRuntime(L"fallback-mission");
    CHECK(WriteText(
        runtime / L"effects.ini",
        Configuration("{\"fixture.unit\":{\"delta\":7}}")));
    RewardFixture fixture = OpenRewardFixture(fixture_path, 0);
    LoadedMod mod = OpenMod(mod_path);
    CHECK(fixture.module && mod.module && mod.load &&
          mod.register_extension);
    CHECK(RegisterExtension(mod.register_extension));
    VerifyFallbackLoad(mod, runtime);
    if (!HooksAreExact()) return;

    MatchFixture match;
    match.Prepare(0x2001, 1700000101u, 7101);
    FrameFixture frame;
    frame.Prepare(&match, 9);
    BYTE* native = frame.Frame();
    const auto& fields = kRewardFields[9];
    g_extensionMode = 0;
    InvokeDetour(g_hooks[9].detour, native);
    CHECK(Get<uint16_t>(native, fields[0]) == 100);
    CHECK(Get<uint16_t>(native, fields[3]) == 2);
    CHECK(Get<uint16_t>(native, fields[2]) == 207);
    CHECK(Get<uint16_t>(native, fields[5]) == 11);
    CHECK(Get<uint16_t>(native, fields[1]) == 22);
    CHECK(Get<uint16_t>(native, fields[4]) == 3);

    Put(native, fields[0], uint16_t{130});
    Put(native, fields[1], uint16_t{29});
    InvokeDetour(g_hooks[10].detour, native);
    CHECK(Get<uint16_t>(native, fields[0]) == 143);
    CHECK(Get<uint16_t>(native, fields[3]) == 15);
    CHECK(Get<uint16_t>(native, fields[2]) == 207);
    CHECK(Get<uint16_t>(native, fields[5]) == 11);
    CHECK(Get<uint16_t>(native, fields[1]) == 29);
    CHECK(Get<uint16_t>(native, fields[4]) == 3);
    InvokeDetour(g_hooks[10].detour, native);
    CHECK(Get<uint16_t>(native, fields[0]) == 143);
}

void FallbackIdentityFailureCase(
    const fs::path& mod_path, const fs::path& fixture_path) {
    const fs::path runtime = NewRuntime(L"fallback-identity");
    CHECK(WriteText(runtime / L"effects.ini", Configuration()));
    RewardFixture fixture = OpenRewardFixture(fixture_path, 0);
    LoadedMod mod = OpenMod(mod_path);
    CHECK(fixture.module && mod.module && mod.load);
    VerifyFallbackLoad(mod, runtime);
    if (!HooksAreExact()) return;

    MatchFixture match;
    match.Prepare(0x3001, 1700000201u, 7201);
    FrameFixture frame;
    frame.Prepare(&match, 9);
    BYTE* native = frame.Frame();
    InvokeDetour(g_hooks[9].detour, native);

    std::array<BYTE, 0x80> replacement_definition{};
    Put(replacement_definition.data(), 0, uint32_t{60001});
    Put(
        match.BattlePilot(), kBattlePilotOperatorDefinition,
        reinterpret_cast<uintptr_t>(replacement_definition.data()));
    const auto& mission_fields = kRewardFields[9];
    Put(native, mission_fields[0], uint16_t{130});
    InvokeDetour(g_hooks[10].detour, native);
    CHECK(Get<uint16_t>(native, mission_fields[0]) == 130);

    MatchFixture later;
    later.Prepare(0x3002, 1700000202u, 7202);
    FrameFixture later_frame;
    later_frame.Prepare(&later, 0);
    InvokeDetour(g_hooks[0].detour, later_frame.Frame());
    CHECK(
        Get<uint16_t>(later_frame.Frame(), kRewardFields[0][0]) ==
        100);
}

void InvalidConfigurationCases(const fs::path& mod_path) {
    LoadedMod mod = OpenMod(mod_path);
    CHECK(mod.module && mod.load);
    std::vector<std::pair<std::wstring, std::string>> cases = {
        {L"bad-version",
         "[effects]\nVersion=2\nRevision=" +
             std::string(64, '0') + "\n"},
        {L"bad-revision",
         "[effects]\nVersion=1\nRevision=abc\n"},
        {L"bad-percent", Configuration("{}", 101)},
        {L"bad-hex",
         "[effects]\nVersion=1\nRevision=" +
             std::string(64, '0') +
             "\n[operator:60001]\nPilotExpPercent=10\n"
             "ExtensionsHex=7\n"},
        {L"bad-utf8", Configuration(std::string("{\"x\":\"") +
                                    static_cast<char>(0xC0) + "\"}")},
        {L"bad-surrogate",
         Configuration("{\"x\":\"\\uD800\"}")},
        {L"duplicate-extension",
         Configuration("{\"x\":1,\"x\":2}")},
        {L"unsupported-section",
         "[effects]\nVersion=1\nRevision=" +
             std::string(64, '0') +
             "\n[other]\nValue=1\n"},
    };
    cases.push_back(
        {L"oversized-json",
         Configuration(
             std::string("{\"x\":\"") +
             std::string(
                 SDGO_OPERATOR_EFFECTS_MAX_EXTENSION_JSON, 'a') +
             "\"}")});
    for (const auto& one : cases) {
        const fs::path runtime = NewRuntime(one.first);
        CHECK(WriteText(runtime / L"effects.ini", one.second));
        g_hooks.clear();
        CHECK(LoadActive(mod, runtime) == 0);
        CHECK(g_hooks.empty());
        CHECK(!fs::exists(runtime / L"status.json"));
    }
    const fs::path missing = NewRuntime(L"missing-config");
    g_hooks.clear();
    CHECK(LoadActive(mod, missing) == 0);
    CHECK(g_hooks.empty());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::cerr
            << "usage: operator_catalog_server_test "
               "<operator_catalog_server.dll> <battle_rewards.dll> "
               "<shared|extension|unknown|fallback_refused|"
               "fallback_nonone|fallback_missing|fallback_mission|"
               "fallback_identity|maximum_json|invalid_config>\n";
        return 2;
    }
    const fs::path mod_path = fs::absolute(argv[1]);
    const fs::path fixture_path = fs::absolute(argv[2]);
    const std::wstring mode = argv[3];

    if (mode == L"shared") {
        SharedCases(mod_path, fixture_path);
    } else if (mode == L"extension") {
        ExtensionCases(mod_path, fixture_path);
    } else if (mode == L"unknown") {
        UnknownExtensionCase(mod_path, fixture_path);
    } else if (mode == L"fallback_refused") {
        FallbackCases(mod_path, &fixture_path, 0, true);
    } else if (mode == L"fallback_nonone") {
        FallbackCases(mod_path, &fixture_path, 2, false);
    } else if (mode == L"fallback_missing") {
        FallbackCases(mod_path, nullptr, -1, false);
    } else if (mode == L"fallback_mission") {
        FallbackMissionCase(mod_path, fixture_path);
    } else if (mode == L"fallback_identity") {
        FallbackIdentityFailureCase(mod_path, fixture_path);
    } else if (mode == L"maximum_json") {
        MaximumExtensionJsonCase(mod_path, fixture_path);
    } else if (mode == L"invalid_config") {
        InvalidConfigurationCases(mod_path);
    } else {
        return 2;
    }

    std::cout << g_checks << " checks, " << g_failures
              << " failures\n";
    return g_failures ? 1 : 0;
}
