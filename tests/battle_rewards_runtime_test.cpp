#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sdgo/battle_rewards_damage_observer.h"
#include "sdgo/operator_reward_filter.h"
#include "sdgo/sdgo_api.h"

static_assert(sizeof(void*) == 4,
              "the native battle reward runtime fixture must be built for x86");

namespace {
namespace fs = std::filesystem;

constexpr size_t kRoomSize = 0x33000;
constexpr size_t kRoomMode = 0x05;
constexpr size_t kRoomMission = 0x1B;
constexpr size_t kRoomPilotBase = 0x533;
constexpr size_t kRoomPilotStride = 0x48E;
constexpr size_t kRoomPilotAccount = 0x429;
constexpr size_t kRoomPilotTeam = 0x42D;
constexpr size_t kRoomPilotAgent = 0x476;
constexpr size_t kBattlePilotBase = 0x3CE4;
constexpr size_t kBattlePilotStride = 0x1F54;
constexpr size_t kBattlePilotEntity = 0x00;
constexpr size_t kBattlePilotTeam = 0x04;
constexpr size_t kBattlePilotKills = 0x12;
constexpr size_t kBattlePilotState = 0x22;
constexpr size_t kBattlePilotNpcData = 0x1CEC;
constexpr size_t kBattlePilotOutcome = 0x1D28;
constexpr size_t kBattlePilotOperatorDefinition = 0x50;
constexpr size_t kRoomStartedAt = 0x2EEB4;
constexpr size_t kRoomMissionState = 0x316A4;
constexpr size_t kRoomMissionDifficulty = 0x316F0;

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

// Independent copies of the shipped daily-reward settlement fields:
// pilot EXP, G, unit EXP, then their legacy event buckets.
constexpr std::array<std::array<ptrdiff_t, 6>, 10> kDailyFields = {{
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

// Independently traced EBP sources for packet +1E, +24, +2A and +30.
// Packet +36 is the event bucket in kDailyFields. Only these GB rows change.
constexpr std::array<std::array<ptrdiff_t, 4>, 10> kGDisplayFields = {{
    {{-0x758, -0x10, -0x76C, -0x744}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
    {{-0x74C, -0x08, -0x760, -0x73C}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
}};

constexpr std::array<const char*, 10> kAugmentSymbols = {{
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
}};

constexpr std::array<const char*, 10> kBattleLayoutSymbols = {{
    "battle.rewards.layout.personal",
    "battle.rewards.layout.team",
    "battle.rewards.layout.leader",
    "battle.rewards.layout.ai",
    "battle.rewards.layout.ai_ship",
    "battle.rewards.layout.mode13",
    "battle.rewards.layout.mode14",
    "battle.rewards.layout.tag",
    "battle.rewards.layout.death_grid",
    "battle.rewards.layout.mission",
}};

constexpr std::array<const char*, 10> kDailyFinalSymbols = {{
    "battle.reward.final.personal",
    "battle.reward.final.team",
    "battle.reward.final.leader",
    "battle.reward.final.ai",
    "battle.reward.final.ai_ship",
    "battle.reward.final.mode13",
    "battle.reward.final.mode14",
    "battle.reward.final.tag",
    "battle.reward.final.death_grid",
    "battle.reward.final.mission",
}};

constexpr std::array<const char*, 10> kDailyLayoutSymbols = {{
    "battle.reward.layout.personal",
    "battle.reward.layout.team",
    "battle.reward.layout.leader",
    "battle.reward.layout.ai",
    "battle.reward.layout.ai_ship",
    "battle.reward.layout.mode13",
    "battle.reward.layout.mode14",
    "battle.reward.layout.tag",
    "battle.reward.layout.death_grid",
    "battle.reward.layout.mission",
}};

#define SDGO_FIXTURE_SITE(N)                                                   \
    extern "C" __declspec(noinline) __declspec(naked) void FixtureSite##N() { \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { mov eax, N }                                                   \
        __asm { ret }                                                          \
    }
SDGO_FIXTURE_SITE(0)
SDGO_FIXTURE_SITE(1)
SDGO_FIXTURE_SITE(2)
SDGO_FIXTURE_SITE(3)
SDGO_FIXTURE_SITE(4)
SDGO_FIXTURE_SITE(5)
SDGO_FIXTURE_SITE(6)
SDGO_FIXTURE_SITE(7)
SDGO_FIXTURE_SITE(8)
SDGO_FIXTURE_SITE(9)
#undef SDGO_FIXTURE_SITE

extern "C" __declspec(noinline) __declspec(naked)
void FixtureMissionFinalSite() {
    __asm { nop }
    __asm { nop }
    __asm { nop }
    __asm { nop }
    __asm { nop }
    __asm { nop }
    __asm { ret }
}

extern "C" __declspec(noinline) __declspec(naked) void FixtureDamageTarget() {
    __asm { mov eax, 0C0FFEE01h }
    __asm { ret 14h }
}

using SiteFn = void(__cdecl*)();
using DamageFn = uint32_t(__thiscall*)(
    void*, void*, void*, void*, uint32_t, int32_t);

const std::array<SiteFn, 10> kSiteFunctions = {{
    &FixtureSite0, &FixtureSite1, &FixtureSite2, &FixtureSite3, &FixtureSite4,
    &FixtureSite5, &FixtureSite6, &FixtureSite7, &FixtureSite8, &FixtureSite9,
}};

__declspec(naked) void __cdecl InvokeSite(void*, BYTE*) {
    __asm { push ebp }
    __asm { mov ebp, [esp + 0Ch] }
    __asm { call dword ptr [esp + 08h] }
    __asm { pop ebp }
    __asm { ret }
}

void InvokeSettlement(unsigned site, BYTE* frame) {
    InvokeSite(reinterpret_cast<void*>(kSiteFunctions[site]), frame);
    if (site == 9) {
        InvokeSite(
            reinterpret_cast<void*>(&FixtureMissionFinalSite), frame);
    }
}

struct PacketRecord {
    uint32_t target = 0;
    uint32_t account = 0;
    uint32_t current_mb = 0;
};

std::array<BYTE, 0x200> g_net_frame{};
void* g_net_frame_proc = g_net_frame.data();
std::mutex g_packet_mutex;
std::vector<PacketRecord> g_packets;
std::array<BYTE, 20> g_layout_guards{};
int g_checks = 0;
int g_failures = 0;
volatile LONG g_observed_damage = 0;
volatile LONG g_invalid_observed_damage = 0;
volatile LONG g_filter_calls = 0;
volatile LONG g_filter_early = 0;
volatile LONG g_filter_final = 0;
void* const kFilterUser = reinterpret_cast<void*>(0x76543210);

void __cdecl ObserveDamage(
    const SdgoBattleRewardsDamageEventV1* event, void*) {
    if (!event || event->size < sizeof(*event) ||
        event->version !=
            SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION ||
        !event->room || !event->victim_pilot ||
        !event->attacker_pilot || event->effective_loss == 0) {
        InterlockedIncrement(&g_invalid_observed_damage);
        return;
    }
    InterlockedIncrement(&g_observed_damage);
}

int __stdcall FilterRewards(
    const SdgoOperatorRewardContextV1* context,
    SdgoOperatorRewardResultV1* result, void* user) {
    const bool expected_native_ineligible =
        context && context->account == 9601;
    if (!context || context->size < sizeof(*context) ||
        context->version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        !result || result->size < sizeof(*result) ||
        result->version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        user != kFilterUser ||
        context->participant_role != SDGO_OPERATOR_REWARD_ROLE_HUMAN ||
        context->native_eligible !=
            (expected_native_ineligible ? 0u : 1u) ||
        context->outcome != SDGO_OPERATOR_REWARD_OUTCOME_WIN_OR_CLEAR ||
        context->won != 1 || !context->frame || !context->room ||
        !context->room_pilot || !context->battle_pilot ||
        !context->operator_definition) {
        return SDGO_OPERATOR_REWARD_FILTER_REJECT;
    }
    const uintptr_t expected_definition =
        static_cast<uintptr_t>(0x71000000u + context->account);
    if (reinterpret_cast<uintptr_t>(context->operator_definition) !=
        expected_definition) {
        return SDGO_OPERATOR_REWARD_FILTER_REJECT;
    }
    InterlockedIncrement(&g_filter_calls);
    if (context->account == 8801) {
        if (context->site != 0 ||
            context->phase !=
                SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT ||
            context->writable_mask !=
                (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                 SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
                 SDGO_OPERATOR_REWARD_FIELD_GP) ||
            context->gp_policy != SDGO_OPERATOR_REWARD_GP_REPLACE ||
            context->native_pilot_exp != 65000 ||
            context->native_unit_exp != 65530 ||
             context->native_gp != 22 || context->current_gp != 13310 ||
             context->current_gp_event != 0) {
            return SDGO_OPERATOR_REWARD_FILTER_REJECT;
        }
        result->write_mask =
            SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
            SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
            SDGO_OPERATOR_REWARD_FIELD_GP;
        result->target_pilot_exp = 70000;
        result->target_unit_exp = 70000;
        result->target_gp = 70000;
        InterlockedIncrement(&g_filter_early);
        return SDGO_OPERATOR_REWARD_FILTER_APPLY;
    }
    if (context->account == 8901) {
        if (context->site != 9 ||
            context->gp_policy != SDGO_OPERATOR_REWARD_GP_REPLACE) {
            return SDGO_OPERATOR_REWARD_FILTER_REJECT;
        }
        if (context->phase ==
            SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT) {
            if (context->writable_mask !=
                    SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP ||
                context->native_pilot_exp != 100 ||
                context->native_unit_exp != 200 ||
                context->native_gp != 22 ||
                 context->current_gp != 2092 ||
                 context->current_gp_event != 0) {
                return SDGO_OPERATOR_REWARD_FILTER_REJECT;
            }
            result->write_mask =
                SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP;
            result->target_unit_exp = 250;
            InterlockedIncrement(&g_filter_early);
            return SDGO_OPERATOR_REWARD_FILTER_APPLY;
        }
        if (context->phase ==
            SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS) {
            if (context->writable_mask !=
                    (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                     SDGO_OPERATOR_REWARD_FIELD_GP) ||
                context->native_pilot_exp != 130 ||
                context->native_unit_exp != 250 ||
                context->native_gp != 29 ||
                 context->current_gp != 2092 ||
                 context->current_gp_event != 0) {
                return SDGO_OPERATOR_REWARD_FILTER_REJECT;
            }
            result->write_mask =
                SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                SDGO_OPERATOR_REWARD_FIELD_GP;
            result->target_pilot_exp = 180;
            result->target_gp = 2101;
            InterlockedIncrement(&g_filter_final);
            return SDGO_OPERATOR_REWARD_FILTER_APPLY;
        }
    }
    if (context->account == 9101) {
        if (context->site != 0 ||
            context->phase !=
                SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT ||
            context->writable_mask !=
                (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                 SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
                 SDGO_OPERATOR_REWARD_FIELD_GP) ||
            context->gp_policy != SDGO_OPERATOR_REWARD_GP_NATIVE ||
            context->native_gp != 22 || context->native_gp_event != 3 ||
            context->current_gp != 22 || context->current_gp_event != 3) {
            return SDGO_OPERATOR_REWARD_FILTER_REJECT;
        }
        result->write_mask = SDGO_OPERATOR_REWARD_FIELD_GP;
        result->target_gp = 30;
        InterlockedIncrement(&g_filter_early);
        return SDGO_OPERATOR_REWARD_FILTER_APPLY;
    }
    if (context->account == 9201 || context->account == 9401 ||
        context->account == 9501 || context->account == 9601 ||
        context->account == 9701) {
        const uint32_t expected_mask =
            SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
            SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
            (context->account == 9501
                 ? SDGO_OPERATOR_REWARD_FIELD_GP
                 : 0u);
        if (context->site != 0 ||
            context->phase !=
                SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT ||
            context->writable_mask != expected_mask ||
            context->gp_policy != SDGO_OPERATOR_REWARD_GP_NATIVE ||
            context->native_pilot_exp != 100 ||
            context->native_unit_exp != 200 ||
            context->native_gp != 22 ||
            context->current_pilot_exp != 100 ||
            context->current_unit_exp != 200 ||
            context->current_gp != 22 ||
            context->current_gp_event != 3) {
            return SDGO_OPERATOR_REWARD_FILTER_REJECT;
        }
        result->write_mask =
            SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
            SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP;
        result->target_pilot_exp = 150;
        result->target_unit_exp = 250;
        InterlockedIncrement(&g_filter_early);
        return SDGO_OPERATOR_REWARD_FILTER_APPLY;
    }
    if (context->account == 9301) {
        if (context->site != 9 ||
            context->gp_policy != SDGO_OPERATOR_REWARD_GP_NATIVE) {
            return SDGO_OPERATOR_REWARD_FILTER_REJECT;
        }
        if (context->phase ==
            SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT) {
            if (context->writable_mask !=
                    SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP ||
                context->native_pilot_exp != 100 ||
                context->native_unit_exp != 200 ||
                context->native_gp != 22 ||
                context->current_gp != 22) {
                return SDGO_OPERATOR_REWARD_FILTER_REJECT;
            }
            result->write_mask =
                SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP;
            result->target_unit_exp = 250;
            InterlockedIncrement(&g_filter_early);
            return SDGO_OPERATOR_REWARD_FILTER_APPLY;
        }
        if (context->phase ==
            SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS) {
            if (context->writable_mask !=
                    SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP ||
                context->native_pilot_exp != 130 ||
                context->native_unit_exp != 250 ||
                context->native_gp != 29 ||
                context->current_gp != 29) {
                return SDGO_OPERATOR_REWARD_FILTER_REJECT;
            }
            result->write_mask =
                SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP;
            result->target_pilot_exp = 180;
            InterlockedIncrement(&g_filter_final);
            return SDGO_OPERATOR_REWARD_FILTER_APPLY;
        }
    }
    return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
}

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

extern "C" __declspec(noinline) int __fastcall FixtureNetSend(
    void* self, void*, uint32_t target, const void* packet, int length,
    int flags) {
    if (self != g_net_frame.data() || !packet || length != 10 || flags != 0 ||
        target == 0) {
        return -1;
    }
    const BYTE* bytes = static_cast<const BYTE*>(packet);
    uint16_t packet_id = 0;
    uint32_t account = 0;
    uint32_t current_mb = 0;
    std::memcpy(&packet_id, bytes, sizeof(packet_id));
    std::memcpy(&account, bytes + 2, sizeof(account));
    std::memcpy(&current_mb, bytes + 6, sizeof(current_mb));
    if (packet_id != 0x064B || account == 0) return -1;
    std::lock_guard<std::mutex> lock(g_packet_mutex);
    g_packets.push_back({target, account, current_mb});
    return 0;
}

struct MatchFixture {
    std::vector<BYTE> room = std::vector<BYTE>(kRoomSize);
    uint32_t base_account = 0;

    BYTE* RoomPilot(unsigned slot) {
        return room.data() + kRoomPilotBase + slot * kRoomPilotStride;
    }
    BYTE* BattlePilot(unsigned slot) {
        return room.data() + kBattlePilotBase + slot * kBattlePilotStride;
    }
};

struct FrameFixture {
    std::array<BYTE, 0xD00> storage{};
    BYTE* Frame() { return storage.data() + 0xC80; }
};

std::string HexBytes(const BYTE* bytes, size_t count) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < count; ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

std::string HostKey(HMODULE host, uint32_t* timestamp, uint32_t* image_size) {
    BYTE* base = reinterpret_cast<BYTE*>(host);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    *timestamp = nt->FileHeader.TimeDateStamp;
    *image_size = nt->OptionalHeader.SizeOfImage;
    wchar_t path[32768] = {};
    GetModuleFileNameW(host, path, static_cast<DWORD>(std::size(path)));
    std::ostringstream suffix;
    suffix << fs::path(path).stem().string() << '-' << std::uppercase
           << std::hex << std::setw(8) << std::setfill('0') << *timestamp
           << '-' << std::setw(8) << *image_size;
    return suffix.str();
}

uint32_t Rva(const void* address, HMODULE host, uint32_t image_size) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(host);
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    CHECK(value >= base && value - base < image_size);
    return static_cast<uint32_t>(value - base);
}

void WriteSymbol(std::ofstream& out, const char* name, const char* kind,
                 uint32_t rva, const BYTE* expected = nullptr,
                 size_t expected_count = 0) {
    out << "\n[" << name << "]\nkind=" << kind << "\nrva=0x" << std::hex
        << std::uppercase << rva << std::dec << "\n";
    if (expected && expected_count)
        out << "expect=" << HexBytes(expected, expected_count) << "\n";
}

bool GenerateSymbols(const fs::path& root) {
    HMODULE host = GetModuleHandleW(nullptr);
    uint32_t timestamp = 0;
    uint32_t image_size = 0;
    const std::string key = HostKey(host, &timestamp, &image_size);
    fs::create_directories(root / L"sdgo" / L"symbols");
    const fs::path path = root / L"sdgo" / L"symbols" /
                          fs::path(std::wstring(key.begin(), key.end()) + L".ini");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "[host]\ntimestamp=0x" << std::hex << std::uppercase << timestamp
        << "\nsize_of_image=0x" << image_size << std::dec << "\n";
    for (size_t i = 0; i < kSiteFunctions.size(); ++i) {
        const BYTE* first =
            reinterpret_cast<const BYTE*>(kSiteFunctions[i]);
        WriteSymbol(out, kDailyFinalSymbols[i], "site",
                    Rva(first, host, image_size), first, 6);
        WriteSymbol(out, kAugmentSymbols[i], "site",
                    Rva(first + 6, host, image_size), first + 6, 6);
        WriteSymbol(out, kDailyLayoutSymbols[i], "data",
                    Rva(&g_layout_guards[i], host, image_size));
        WriteSymbol(out, kBattleLayoutSymbols[i], "data",
                    Rva(&g_layout_guards[i + 10], host, image_size));
    }
    const BYTE* mission_final =
        reinterpret_cast<const BYTE*>(&FixtureMissionFinalSite);
    WriteSymbol(
        out, "battle.rewards.final.mission", "site",
        Rva(mission_final, host, image_size), mission_final, 6);
    const BYTE* damage =
        reinterpret_cast<const BYTE*>(&FixtureDamageTarget);
    WriteSymbol(out, "battle.rewards.record_damage", "code",
                Rva(damage, host, image_size), damage, 5);
    const BYTE* sender = reinterpret_cast<const BYTE*>(&FixtureNetSend);
    WriteSymbol(out, "battle.rewards.net_send", "code",
                Rva(sender, host, image_size), sender, 5);
    WriteSymbol(out, "battle.rewards.net_frame_proc", "data",
                Rva(&g_net_frame_proc, host, image_size));
    out.flush();
    return out.good();
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (predicate()) return true;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return predicate();
}

bool WaitStatus(const fs::path& runtime, const std::string& needle,
                DWORD timeout_ms = 8000) {
    const fs::path status =
        runtime / L"status" / L"battle-fixture.json";
    return WaitUntil(
        [&] {
            if (!fs::is_regular_file(status)) return false;
            const std::string text = ReadText(status);
            return text.find("\"settlement_ready\": true") !=
                       std::string::npos &&
                   text.find(needle) != std::string::npos;
        },
        timeout_ms);
}

bool WaitStatusText(const fs::path& runtime, const std::string& needle,
                    DWORD timeout_ms = 8000) {
    const fs::path status =
        runtime / L"status" / L"battle-fixture.json";
    return WaitUntil(
        [&] {
            return fs::is_regular_file(status) &&
                   ReadText(status).find(needle) != std::string::npos;
        },
        timeout_ms);
}

void PrepareMatch(MatchFixture* match, uint32_t room_id, uint32_t started,
                  uint32_t base_account) {
    match->base_account = base_account;
    Put(match->room.data(), 0, room_id);
    Put(match->room.data(), kRoomMode, static_cast<uint8_t>(4));
    Put(match->room.data(), kRoomMission, static_cast<uint32_t>(0));
    Put(match->room.data(), kRoomStartedAt, started);
    for (unsigned slot = 0; slot < 4; ++slot) {
        const uint32_t account = base_account + slot;
        const uint8_t team = slot < 2 ? 1 : 2;
        BYTE* room_pilot = match->RoomPilot(slot);
        BYTE* battle_pilot = match->BattlePilot(slot);
        Put(room_pilot, kRoomPilotAccount, account);
        Put(room_pilot, kRoomPilotTeam, team);
        Put(room_pilot, kRoomPilotAgent, 900000u + account);
        Put(battle_pilot, kBattlePilotEntity, account);
        Put(battle_pilot, kBattlePilotTeam, team);
        Put(battle_pilot, kBattlePilotState, static_cast<uint8_t>(3));
        Put(battle_pilot, kBattlePilotOutcome, static_cast<uint8_t>(1));
        Put(
            battle_pilot, kBattlePilotOperatorDefinition,
            static_cast<uintptr_t>(0x71000000u + account));
    }
}

void RecordCombat(MatchFixture* match, unsigned attacker_slot) {
    static const uint32_t damage[] = {2000, 2000, 4000};
    static const unsigned victims[] = {2, 3, 2};
    DamageFn target = reinterpret_cast<DamageFn>(&FixtureDamageTarget);
    std::array<BYTE, 0x100> victim_unit{};
    Put(victim_unit.data(), 0, static_cast<uint32_t>(0xABCDEF01));
    Put(victim_unit.data(), 0x6C, static_cast<uintptr_t>(0x12345678));
    for (size_t i = 0; i < std::size(damage); ++i) {
        const uint32_t result = target(
            match->room.data(), match->BattlePilot(victims[i]),
            match->BattlePilot(attacker_slot), victim_unit.data(), damage[i], 1);
        CHECK(result == 0xC0FFEE01u);
    }
    Put(match->BattlePilot(attacker_slot), kBattlePilotKills,
        static_cast<uint32_t>(3));
}

void InvalidateCombat(MatchFixture* match, unsigned attacker_slot) {
    DamageFn target = reinterpret_cast<DamageFn>(&FixtureDamageTarget);
    std::array<BYTE, 0x100> victim_unit{};
    Put(victim_unit.data(), 0, static_cast<uint32_t>(0xABCDEF02));
    Put(victim_unit.data(), 0x6C, static_cast<uintptr_t>(0x12345679));
    const uint32_t result = target(
        match->room.data(), match->BattlePilot(2),
        match->BattlePilot(attacker_slot), victim_unit.data(),
        100000001u, 1);
    CHECK(result == 0xC0FFEE01u);
}

void PrepareMission(MatchFixture* match, uint32_t room_id, uint32_t started,
                    uint32_t base_account) {
    PrepareMatch(match, room_id, started, base_account);
    Put(match->room.data(), kRoomMode, static_cast<uint8_t>(5));
    Put(match->room.data(), kRoomMission, static_cast<uint32_t>(10));
    Put(match->room.data(), kRoomMissionState, static_cast<uint32_t>(5));
    Put(match->room.data(), kRoomMissionDifficulty, static_cast<uint8_t>(2));
}

void RecordMissionCombat(MatchFixture* match) {
    DamageFn target = reinterpret_cast<DamageFn>(&FixtureDamageTarget);
    BYTE* victim = match->BattlePilot(12);
    std::array<BYTE, 0x100> victim_unit{};
    // unit+0 is deliberately unrelated to the mission identity. Production
    // classification must use BP+1CEC -> NpcData[0].
    Put(victim_unit.data(), 0, static_cast<uint32_t>(999));
    Put(victim_unit.data(), 0x6C, static_cast<uintptr_t>(0x87654321));
    std::array<BYTE, 0x40> minion_data{};
    Put(minion_data.data(), 0, static_cast<uint32_t>(99));
    for (uint32_t i = 0; i < 15; ++i) {
        Put(victim, kBattlePilotEntity, 0x80001000u + i);
        Put(victim, kBattlePilotTeam, static_cast<uint8_t>(0));
        Put(victim, kBattlePilotNpcData,
            reinterpret_cast<uintptr_t>(minion_data.data()));
        const uint32_t result =
            target(match->room.data(), victim, match->BattlePilot(0),
                   victim_unit.data(), 100, 1);
        CHECK(result == 0xC0FFEE01u);
    }
    std::array<BYTE, 0x40> boss_data{};
    Put(boss_data.data(), 0, static_cast<uint32_t>(20));
    Put(victim, kBattlePilotEntity, static_cast<uint32_t>(0x80002000u));
    Put(victim, kBattlePilotTeam, static_cast<uint8_t>(0));
    Put(victim, kBattlePilotNpcData,
        reinterpret_cast<uintptr_t>(boss_data.data()));
    const uint32_t human_result =
        target(match->room.data(), victim, match->BattlePilot(0),
               victim_unit.data(), 3250, 0);
    CHECK(human_result == 0xC0FFEE01u);
    BYTE* wingman = match->BattlePilot(13);
    Put(wingman, kBattlePilotEntity, static_cast<uint32_t>(0x80003000u));
    Put(wingman, kBattlePilotTeam, static_cast<uint8_t>(1));
    const uint32_t wingman_result =
        target(match->room.data(), victim, wingman,
               victim_unit.data(), 3250, 1);
    CHECK(wingman_result == 0xC0FFEE01u);
}

std::unique_ptr<FrameFixture> PrepareFrame(MatchFixture* match, unsigned site,
                                           unsigned slot, int32_t rank) {
    auto result = std::make_unique<FrameFixture>();
    BYTE* frame = result->Frame();
    const FrameContract& contract = kFrames[site];
    Put(frame, contract.room,
        reinterpret_cast<uintptr_t>(match->room.data()));
    Put(frame, contract.duration, static_cast<uint32_t>(180));
    Put(frame, contract.room_pilot,
        reinterpret_cast<uintptr_t>(match->RoomPilot(slot)));
    Put(frame, contract.battle_pilot,
        reinterpret_cast<uintptr_t>(match->BattlePilot(slot)));
    Put(frame, contract.physical_slot, static_cast<int32_t>(slot));
    Put(frame, contract.rank, rank);
    Put(frame, contract.eligibility + rank * 4,
        static_cast<uint32_t>(0));
    if (contract.mission_outcomes)
        Put(frame, contract.mission_outcomes + rank * 4,
            static_cast<uint32_t>(1));
    const auto& fields = kDailyFields[site];
    Put(frame, fields[0], static_cast<uint16_t>(11));
    Put(frame, fields[1], static_cast<uint16_t>(22));
    Put(frame, fields[2], static_cast<uint16_t>(33));
    Put(frame, fields[3], static_cast<uint16_t>(2));
    Put(frame, fields[4], static_cast<uint16_t>(3));
    Put(frame, fields[5], static_cast<uint16_t>(4));
    for (unsigned i = 0; i < 4; ++i)
        Put(frame, kGDisplayFields[site][i], static_cast<uint16_t>(16 + i));
    return result;
}

void CheckFrame(const FrameFixture& frame_fixture, unsigned site,
                uint16_t expected_g, bool daily) {
    const BYTE* frame = frame_fixture.storage.data() + 0xC80;
    const auto& fields = kDailyFields[site];
    CHECK(Get<uint16_t>(frame, fields[0]) == (daily ? 22 : 11));
    CHECK(Get<uint16_t>(frame, fields[1]) == expected_g);
    CHECK(Get<uint16_t>(frame, fields[2]) == (daily ? 66 : 33));
    CHECK(Get<uint16_t>(frame, fields[3]) == (daily ? 13 : 2));
    CHECK(Get<uint16_t>(frame, fields[4]) == 0);
    CHECK(Get<uint16_t>(frame, fields[5]) == (daily ? 37 : 4));
    CHECK(Get<uint16_t>(frame, kGDisplayFields[site][0]) == expected_g);
    for (unsigned i = 1; i < 4; ++i)
        CHECK(Get<uint16_t>(frame, kGDisplayFields[site][i]) == 0);
}

bool HasPacket(uint32_t account, uint32_t current_mb) {
    std::lock_guard<std::mutex> lock(g_packet_mutex);
    for (const PacketRecord& packet : g_packets) {
        if (packet.account == account && packet.current_mb == current_mb &&
            packet.target == 900000u + account) {
            return true;
        }
    }
    return false;
}

bool WaitPacket(uint32_t account, uint32_t current_mb,
                DWORD timeout_ms = 8000) {
    return WaitUntil([&] { return HasPacket(account, current_mb); },
                     timeout_ms);
}

bool ReplacePolicyWithBaseFive(const fs::path& runtime) {
    const fs::path path = runtime / L"reward-policy.ini";
    const std::string text = ReadText(runtime / L"reward-policy-base-five.ini");
    if (text.empty()) return false;
    const fs::path temporary = path.parent_path() / L"reward-policy.fixture.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void VerifyPatchedSites(bool daily) {
    for (SiteFn function : kSiteFunctions) {
        const BYTE* first = reinterpret_cast<const BYTE*>(function);
        CHECK(first[6] == 0xE9);
        CHECK(!daily || first[0] == 0xE9);
        CHECK(daily || first[0] == 0x90);
    }
    CHECK(*reinterpret_cast<const BYTE*>(&FixtureMissionFinalSite) == 0xE9);
    CHECK(*reinterpret_cast<const BYTE*>(&FixtureDamageTarget) == 0xE9);
}

bool LoadRealCore(const fs::path& root) {
    HMODULE core = LoadLibraryW((root / L"sdgo" / L"core.dll").c_str());
    if (!core) {
        std::cerr << "LoadLibrary(core.dll) failed: " << GetLastError() << "\n";
        return false;
    }
    auto init = reinterpret_cast<SdgoCoreInitFn>(
        GetProcAddress(core, "SdgoCoreInit"));
    if (!init) return false;
    SdgoBoot boot{sizeof(SdgoBoot), "battle_rewards_runtime_test",
                  GetModuleHandleW(nullptr)};
    return init(&boot) != 0;
}

void BasicCases(const fs::path& runtime) {
    std::vector<std::unique_ptr<MatchFixture>> matches;
    std::vector<std::unique_ptr<FrameFixture>> frames;
    for (unsigned site = 0; site < 10; ++site) {
        auto match = std::make_unique<MatchFixture>();
        const uint32_t base_account = 1001u + site * 100u;
        PrepareMatch(match.get(), 0x1000u + site, 1700000000u + site,
                     base_account);
        RecordCombat(match.get(), 0);
        if (site == 0) RecordCombat(match.get(), 1);
        auto frame = PrepareFrame(match.get(), site, 0, 0);
        InvokeSettlement(site, frame->Frame());
        CheckFrame(*frame, site, 13310, false);
        matches.push_back(std::move(match));
        frames.push_back(std::move(frame));
    }
    for (unsigned site = 0; site < 10; ++site)
        CHECK(WaitPacket(matches[site]->base_account, 1125));

    auto mission = std::make_unique<MatchFixture>();
    PrepareMission(mission.get(), 0x1800, 1700000500, 2501);
    RecordMissionCombat(mission.get());
    auto mission_frame = PrepareFrame(mission.get(), 9, 0, 0);
    InvokeSettlement(9, mission_frame->Frame());
    CheckFrame(*mission_frame, 9, 2092, false);
    CHECK(WaitPacket(2501, 1003));

    auto saturated = std::make_unique<MatchFixture>();
    PrepareMatch(saturated.get(), 0x1900, 1700000600, 8801);
    RecordCombat(saturated.get(), 0);
    auto saturated_frame = PrepareFrame(saturated.get(), 0, 0, 0);
    BYTE* saturated_native = saturated_frame->Frame();
    const auto& personal_fields = kDailyFields[0];
    Put(saturated_native, personal_fields[0], static_cast<uint16_t>(65000));
    Put(saturated_native, personal_fields[2], static_cast<uint16_t>(65530));
    Put(saturated_native, personal_fields[3], static_cast<uint16_t>(10));
    Put(saturated_native, personal_fields[4], static_cast<uint16_t>(3));
    Put(saturated_native, personal_fields[5], static_cast<uint16_t>(20));
    InvokeSettlement(0, saturated_native);
    CHECK(Get<uint16_t>(saturated_native, personal_fields[0]) == 65535);
    CHECK(Get<uint16_t>(saturated_native, personal_fields[3]) == 545);
    CHECK(Get<uint16_t>(saturated_native, personal_fields[2]) == 65535);
    CHECK(Get<uint16_t>(saturated_native, personal_fields[5]) == 25);
    CHECK(Get<uint16_t>(saturated_native, personal_fields[1]) == 65535);
    CHECK(Get<uint16_t>(saturated_native, personal_fields[4]) == 0);
    CHECK(
        Get<uint16_t>(saturated_native, kGDisplayFields[0][0]) == 65535);
    for (unsigned i = 1; i < 4; ++i)
        CHECK(Get<uint16_t>(
                  saturated_native, kGDisplayFields[0][i]) == 0);
    CHECK(WaitPacket(8801, 1125));

    auto filtered_mission = std::make_unique<MatchFixture>();
    PrepareMission(
        filtered_mission.get(), 0x1A00, 1700000700, 8901);
    RecordMissionCombat(filtered_mission.get());
    auto filtered_mission_frame =
        PrepareFrame(filtered_mission.get(), 9, 0, 0);
    BYTE* filtered_native = filtered_mission_frame->Frame();
    const auto& mission_fields = kDailyFields[9];
    Put(filtered_native, mission_fields[0], static_cast<uint16_t>(100));
    Put(filtered_native, mission_fields[2], static_cast<uint16_t>(200));
    InvokeSite(
        reinterpret_cast<void*>(kSiteFunctions[9]), filtered_native);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[0]) == 100);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[3]) == 2);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[2]) == 250);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[5]) == 54);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[1]) == 22);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[4]) == 3);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(Get<uint16_t>(
                  filtered_native, kGDisplayFields[9][i]) == 16 + i);
    Put(filtered_native, mission_fields[0], static_cast<uint16_t>(130));
    Put(filtered_native, mission_fields[1], static_cast<uint16_t>(29));
    InvokeSite(
        reinterpret_cast<void*>(&FixtureMissionFinalSite), filtered_native);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[0]) == 180);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[3]) == 52);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[2]) == 250);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[5]) == 54);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[1]) == 2101);
    CHECK(Get<uint16_t>(filtered_native, mission_fields[4]) == 0);
    CHECK(
        Get<uint16_t>(filtered_native, kGDisplayFields[9][0]) == 2101);
    for (unsigned i = 1; i < 4; ++i)
        CHECK(Get<uint16_t>(
                  filtered_native, kGDisplayFields[9][i]) == 0);
    CHECK(WaitPacket(8901, 1003));
    CHECK(InterlockedCompareExchange(&g_filter_early, 0, 0) == 2);
    CHECK(InterlockedCompareExchange(&g_filter_final, 0, 0) == 1);

    CHECK(ReplacePolicyWithBaseFive(runtime));
    const std::string alternate = ReadText(runtime / L"reward-policy-base-five.ini");
    const std::string revision_label = "PolicyRevision=";
    const size_t revision_at = alternate.find(revision_label);
    CHECK(revision_at != std::string::npos);
    if (revision_at != std::string::npos)
        CHECK(WaitStatus(runtime, alternate.substr(revision_at + revision_label.size(), 64)));

    auto frozen_frame = PrepareFrame(matches[0].get(), 0, 1, 1);
    InvokeSettlement(0, frozen_frame->Frame());
    CheckFrame(*frozen_frame, 0, 10648, false);
    CHECK(WaitPacket(matches[0]->base_account + 1, 1045));

    auto next_match = std::make_unique<MatchFixture>();
    PrepareMatch(next_match.get(), 0x2000, 1700001000, 3001);
    RecordCombat(next_match.get(), 0);
    auto next_frame = PrepareFrame(next_match.get(), 1, 0, 0);
    InvokeSettlement(1, next_frame->Frame());
    CheckFrame(*next_frame, 1, 6655, false);
    CHECK(WaitPacket(3001, 1062));
}

void DailyCase() {
    auto match = std::make_unique<MatchFixture>();
    PrepareMatch(match.get(), 0x3000, 1700002000, 4001);
    RecordCombat(match.get(), 0);
    auto frame = PrepareFrame(match.get(), 0, 0, 0);
    InvokeSettlement(0, frame->Frame());
    CheckFrame(*frame, 0, 26620, true);
    CHECK(WaitPacket(4001, 1250));

    auto mission = std::make_unique<MatchFixture>();
    PrepareMission(mission.get(), 0x4000, 1700003000, 5001);
    RecordMissionCombat(mission.get());
    auto mission_frame = PrepareFrame(mission.get(), 9, 0, 0);
    InvokeSettlement(9, mission_frame->Frame());
    CheckFrame(*mission_frame, 9, 4185, true);
    CHECK(WaitPacket(5001, 1006));
}

void OutageCase(const fs::path& runtime) {
    CHECK(WaitUntil([&]{return fs::is_regular_file(runtime/L"worker-offline.ready");},10000));
    CHECK(WaitStatus(runtime,"\"wallet_worker_healthy\": false"));
    auto match=std::make_unique<MatchFixture>();
    PrepareMatch(match.get(),0x6000,1700006000,7001);
    RecordCombat(match.get(),0);
    auto frame=PrepareFrame(match.get(),0,0,0);
    const LONG filter_before =
        InterlockedCompareExchange(&g_filter_calls, 0, 0);
    InvokeSettlement(0,frame->Frame());
    const LONG filter_after_first =
        InterlockedCompareExchange(&g_filter_calls, 0, 0);
    CHECK(filter_after_first == filter_before + 1);
    CheckFrame(*frame,0,13310,false);
    unsigned queued=0;
    for(const auto& item:fs::directory_iterator(runtime/L"outbox"))
        if(item.path().extension()==L".ini")++queued;
    CHECK(queued==1);
    {std::ofstream signal(runtime/L"native-settled.ready");signal<<"native settlement completed with consumer offline\n";}
    CHECK(WaitPacket(7001,1125));
    // A repeated native callback cannot repeat either GP or the durable cash job.
    auto replay=PrepareFrame(match.get(),0,0,0);
    InvokeSettlement(0,replay->Frame());
    CheckFrame(*replay,0,0,false);
    CHECK(
        InterlockedCompareExchange(&g_filter_calls, 0, 0) ==
        filter_after_first);
}

void CheckOperatorOnlyFrame(
    const FrameFixture& fixture, unsigned site, uint16_t pilot,
    uint16_t pilot_event, uint16_t unit, uint16_t unit_event,
    uint16_t gp, uint16_t gp_event) {
    const BYTE* frame = fixture.storage.data() + 0xC80;
    const auto& fields = kDailyFields[site];
    CHECK(Get<uint16_t>(frame, fields[0]) == pilot);
    CHECK(Get<uint16_t>(frame, fields[3]) == pilot_event);
    CHECK(Get<uint16_t>(frame, fields[2]) == unit);
    CHECK(Get<uint16_t>(frame, fields[5]) == unit_event);
    CHECK(Get<uint16_t>(frame, fields[1]) == gp);
    CHECK(Get<uint16_t>(frame, fields[4]) == gp_event);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(Get<uint16_t>(
                  frame, kGDisplayFields[site][i]) == 16 + i);
}

void PrepareOperatorFrame(FrameFixture* fixture, unsigned site) {
    BYTE* frame = fixture->Frame();
    const auto& fields = kDailyFields[site];
    Put(frame, fields[0], static_cast<uint16_t>(100));
    Put(frame, fields[2], static_cast<uint16_t>(200));
}

void FilterFailureCases(const fs::path& runtime) {
    CHECK(WaitStatusText(runtime, "\"wallet_worker_healthy\": false"));

    auto worker = std::make_unique<MatchFixture>();
    PrepareMatch(worker.get(), 0x7200, 1700007200, 9501);
    RecordCombat(worker.get(), 0);
    auto worker_frame = PrepareFrame(worker.get(), 0, 0, 0);
    PrepareOperatorFrame(worker_frame.get(), 0);
    InvokeSettlement(0, worker_frame->Frame());
    CheckOperatorOnlyFrame(*worker_frame, 0, 150, 52, 250, 54, 22, 3);
    CHECK(WaitPacket(9501, 1125));

    const fs::path outbox = runtime / L"outbox";
    const fs::path hidden_outbox =
        runtime / L"outbox.filter-test-hidden";
    CHECK(MoveFileExW(
              outbox.c_str(), hidden_outbox.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
    {
        std::ofstream blocked(outbox, std::ios::binary);
        CHECK(!!blocked);
        blocked << "blocked";
        blocked.close();
        CHECK(!!blocked);
    }
    auto publication = std::make_unique<MatchFixture>();
    PrepareMatch(publication.get(), 0x7250, 1700007250, 9701);
    RecordCombat(publication.get(), 0);
    auto publication_frame =
        PrepareFrame(publication.get(), 0, 0, 0);
    PrepareOperatorFrame(publication_frame.get(), 0);
    InvokeSettlement(0, publication_frame->Frame());
    CheckOperatorOnlyFrame(
        *publication_frame, 0, 150, 52, 250, 54, 22, 3);
    CHECK(DeleteFileW(outbox.c_str()) != 0);
    CHECK(MoveFileExW(
              hidden_outbox.c_str(), outbox.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);

    const fs::path binding = runtime / L"wallet-binding.ini";
    const fs::path hidden_binding =
        runtime / L"wallet-binding.filter-test-hidden";
    CHECK(MoveFileExW(
              binding.c_str(), hidden_binding.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
    {
        std::ofstream invalid(binding, std::ios::binary);
        CHECK(!!invalid);
        invalid << "[binding]\nProtocol=invalid\n";
        invalid.close();
        CHECK(!!invalid);
    }
    auto wallet = std::make_unique<MatchFixture>();
    PrepareMatch(wallet.get(), 0x7300, 1700007300, 9201);
    RecordCombat(wallet.get(), 0);
    auto wallet_frame = PrepareFrame(wallet.get(), 0, 0, 0);
    PrepareOperatorFrame(wallet_frame.get(), 0);
    InvokeSettlement(0, wallet_frame->Frame());
    CheckOperatorOnlyFrame(*wallet_frame, 0, 150, 52, 250, 54, 22, 3);
    const LONG wallet_filter_calls =
        InterlockedCompareExchange(&g_filter_calls, 0, 0);
    auto wallet_replay = PrepareFrame(wallet.get(), 0, 0, 0);
    PrepareOperatorFrame(wallet_replay.get(), 0);
    InvokeSettlement(0, wallet_replay->Frame());
    CheckOperatorOnlyFrame(
        *wallet_replay, 0, 100, 2, 200, 4, 22, 3);
    CHECK(
        InterlockedCompareExchange(&g_filter_calls, 0, 0) ==
        wallet_filter_calls);
    CHECK(DeleteFileW(binding.c_str()) != 0);
    CHECK(MoveFileExW(
              hidden_binding.c_str(), binding.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);

    const fs::path policy = runtime / L"reward-policy.ini";
    const fs::path hidden_policy =
        runtime / L"reward-policy.filter-test-hidden";
    CHECK(MoveFileExW(
              policy.c_str(), hidden_policy.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
    CHECK(WaitStatusText(runtime, "\"policy_loaded\": false"));
    auto mission = std::make_unique<MatchFixture>();
    PrepareMission(mission.get(), 0x7400, 1700007400, 9301);
    RecordMissionCombat(mission.get());
    auto mission_frame = PrepareFrame(mission.get(), 9, 0, 0);
    PrepareOperatorFrame(mission_frame.get(), 9);
    InvokeSite(
        reinterpret_cast<void*>(kSiteFunctions[9]),
        mission_frame->Frame());
    CheckOperatorOnlyFrame(
        *mission_frame, 9, 100, 2, 250, 54, 22, 3);
    const auto& mission_fields = kDailyFields[9];
    Put(
        mission_frame->Frame(), mission_fields[0],
        static_cast<uint16_t>(130));
    Put(
        mission_frame->Frame(), mission_fields[1],
        static_cast<uint16_t>(29));
    InvokeSite(
        reinterpret_cast<void*>(&FixtureMissionFinalSite),
        mission_frame->Frame());
    CheckOperatorOnlyFrame(
        *mission_frame, 9, 180, 52, 250, 54, 29, 3);
    const LONG mission_filter_calls =
        InterlockedCompareExchange(&g_filter_calls, 0, 0);
    auto mission_replay = PrepareFrame(mission.get(), 9, 0, 0);
    PrepareOperatorFrame(mission_replay.get(), 9);
    InvokeSite(
        reinterpret_cast<void*>(kSiteFunctions[9]),
        mission_replay->Frame());
    CheckOperatorOnlyFrame(
        *mission_replay, 9, 100, 2, 200, 4, 22, 3);
    Put(
        mission_replay->Frame(), mission_fields[0],
        static_cast<uint16_t>(130));
    Put(
        mission_replay->Frame(), mission_fields[1],
        static_cast<uint16_t>(29));
    InvokeSite(
        reinterpret_cast<void*>(&FixtureMissionFinalSite),
        mission_replay->Frame());
    CheckOperatorOnlyFrame(
        *mission_replay, 9, 130, 2, 200, 4, 29, 3);
    CHECK(
        InterlockedCompareExchange(&g_filter_calls, 0, 0) ==
        mission_filter_calls);
    CHECK(MoveFileExW(
              hidden_policy.c_str(), policy.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
    CHECK(WaitStatusText(runtime, "\"policy_loaded\": true"));

    auto combat = std::make_unique<MatchFixture>();
    PrepareMatch(combat.get(), 0x7500, 1700007500, 9401);
    InvalidateCombat(combat.get(), 0);
    auto combat_frame = PrepareFrame(combat.get(), 0, 0, 0);
    PrepareOperatorFrame(combat_frame.get(), 0);
    InvokeSettlement(0, combat_frame->Frame());
    CheckOperatorOnlyFrame(*combat_frame, 0, 150, 52, 250, 54, 22, 3);

    auto ineligible = std::make_unique<MatchFixture>();
    PrepareMatch(ineligible.get(), 0x7600, 1700007600, 9601);
    Put(
        ineligible->BattlePilot(0), kBattlePilotState,
        static_cast<uint8_t>(2));
    auto ineligible_frame =
        PrepareFrame(ineligible.get(), 0, 0, 0);
    PrepareOperatorFrame(ineligible_frame.get(), 0);
    InvokeSettlement(0, ineligible_frame->Frame());
    CheckOperatorOnlyFrame(
        *ineligible_frame, 0, 150, 52, 250, 54, 22, 3);

    CHECK(InterlockedCompareExchange(&g_filter_early, 0, 0) == 6);
    CHECK(InterlockedCompareExchange(&g_filter_final, 0, 0) == 1);
}

void NativeGpCase() {
    auto original = std::make_unique<MatchFixture>();
    PrepareMatch(original.get(), 0x7000, 1700007000, 9001);
    RecordCombat(original.get(), 0);
    auto original_frame = PrepareFrame(original.get(), 0, 0, 0);
    InvokeSettlement(0, original_frame->Frame());
    BYTE* original_native = original_frame->Frame();
    const auto& fields = kDailyFields[0];
    CHECK(Get<uint16_t>(original_native, fields[0]) == 11);
    CHECK(Get<uint16_t>(original_native, fields[1]) == 22);
    CHECK(Get<uint16_t>(original_native, fields[2]) == 33);
    CHECK(Get<uint16_t>(original_native, fields[3]) == 2);
    CHECK(Get<uint16_t>(original_native, fields[4]) == 3);
    CHECK(Get<uint16_t>(original_native, fields[5]) == 4);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(Get<uint16_t>(
                  original_native, kGDisplayFields[0][i]) == 16 + i);
    CHECK(WaitPacket(9001, 1125));

    auto filtered = std::make_unique<MatchFixture>();
    PrepareMatch(filtered.get(), 0x7100, 1700007100, 9101);
    RecordCombat(filtered.get(), 0);
    auto filtered_frame = PrepareFrame(filtered.get(), 0, 0, 0);
    InvokeSettlement(0, filtered_frame->Frame());
    BYTE* filtered_native = filtered_frame->Frame();
    CHECK(Get<uint16_t>(filtered_native, fields[0]) == 11);
    CHECK(Get<uint16_t>(filtered_native, fields[1]) == 30);
    CHECK(Get<uint16_t>(filtered_native, fields[2]) == 33);
    CHECK(Get<uint16_t>(filtered_native, fields[3]) == 2);
    CHECK(Get<uint16_t>(filtered_native, fields[4]) == 11);
    CHECK(Get<uint16_t>(filtered_native, fields[5]) == 4);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(Get<uint16_t>(
                  filtered_native, kGDisplayFields[0][i]) == 16 + i);
    CHECK(WaitPacket(9101, 1125));
    CHECK(InterlockedCompareExchange(&g_filter_early, 0, 0) == 1);
    CHECK(InterlockedCompareExchange(&g_filter_final, 0, 0) == 0);
}

void ProgressionCase() {
    std::vector<std::unique_ptr<MatchFixture>> matches;
    std::vector<std::unique_ptr<FrameFixture>> frames;
    const uint32_t balances[] = {4125, 4250, 31375, 31500};
    for (unsigned i = 0; i < 4; ++i) {
        auto match = std::make_unique<MatchFixture>();
        PrepareMatch(match.get(), 0x5000u + i, 1700004000u + i, 6001);
        RecordCombat(match.get(), 0);
        auto frame = PrepareFrame(match.get(), 0, 0, 0);
        InvokeSettlement(0, frame->Frame());
        CheckFrame(*frame, 0, 13310, false);
        CHECK(WaitPacket(6001, balances[i]));
        matches.push_back(std::move(match));
        frames.push_back(std::move(frame));
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: battle_rewards_runtime_test "
                     "<runtime> <basic|daily|progression|outage|native|"
                     "filter_failures>\n";
        return 2;
    }
    const fs::path runtime = fs::absolute(argv[1]);
    const std::wstring mode = argv[2];
    const bool daily = mode == L"daily";
    if (!daily && mode != L"basic" && mode != L"progression" &&
        mode != L"outage" && mode != L"native" &&
        mode != L"filter_failures") {
        return 2;
    }

    wchar_t executable[32768] = {};
    GetModuleFileNameW(nullptr, executable,
                       static_cast<DWORD>(std::size(executable)));
    const fs::path root = fs::path(executable).parent_path();
    void* transport = g_net_frame.data();
    Put(g_net_frame.data(), 0xAC, transport);
    for (size_t i = 0; i < g_layout_guards.size(); ++i)
        g_layout_guards[i] = static_cast<BYTE>(0x40 + i);

    CHECK(GenerateSymbols(root));
    HMODULE preloadedBattleRewards = LoadLibraryW(
        (root / L"sdgo" / L"mods" / L"battle_rewards" /
         L"battle_rewards.dll").c_str());
    CHECK(preloadedBattleRewards != nullptr);
    const auto preloadedRegisterOperatorFilter =
        preloadedBattleRewards
            ? reinterpret_cast<
                  SdgoBattleRewardsRegisterOperatorRewardFilterV1Fn>(
                  GetProcAddress(
                      preloadedBattleRewards,
                      SDGO_OPERATOR_REWARD_FILTER_EXPORT))
            : nullptr;
    CHECK(preloadedRegisterOperatorFilter != nullptr);
    const SdgoOperatorRewardFilterSinkV1 preloadedFilterSink = {
        sizeof(preloadedFilterSink),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        &FilterRewards,
        kFilterUser};
    CHECK(
        preloadedRegisterOperatorFilter &&
        preloadedRegisterOperatorFilter(&preloadedFilterSink) == 0);
    CHECK(LoadRealCore(root));
    HMODULE battleRewards =
        GetModuleHandleW(L"battle_rewards.dll");
    CHECK(battleRewards != nullptr);
    const auto registerDamageObserver =
        battleRewards
            ? reinterpret_cast<
                  SdgoBattleRewardsRegisterDamageObserverV1Fn>(
                  GetProcAddress(
                      battleRewards,
                      SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_EXPORT))
            : nullptr;
    CHECK(registerDamageObserver != nullptr);
    const SdgoBattleRewardsDamageObserverSinkV1 damageSink = {
        sizeof(damageSink),
        SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION,
        &ObserveDamage,
        nullptr};
    CHECK(registerDamageObserver &&
          registerDamageObserver(&damageSink) == 1);
    CHECK(registerDamageObserver &&
          registerDamageObserver(&damageSink) == 0);
    const auto registerOperatorFilter =
        battleRewards
            ? reinterpret_cast<
                  SdgoBattleRewardsRegisterOperatorRewardFilterV1Fn>(
                  GetProcAddress(
                      battleRewards,
                      SDGO_OPERATOR_REWARD_FILTER_EXPORT))
            : nullptr;
    CHECK(registerOperatorFilter != nullptr);
    CHECK(registerOperatorFilter && registerOperatorFilter(nullptr) == 0);
    SdgoOperatorRewardFilterSinkV1 invalidFilterSink = {
        sizeof(SdgoOperatorRewardFilterSinkV1) - 1,
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        &FilterRewards,
        kFilterUser};
    CHECK(
        registerOperatorFilter &&
        registerOperatorFilter(&invalidFilterSink) == 0);
    invalidFilterSink.size = sizeof(invalidFilterSink);
    invalidFilterSink.version =
        SDGO_OPERATOR_REWARD_FILTER_VERSION + 1;
    CHECK(
        registerOperatorFilter &&
        registerOperatorFilter(&invalidFilterSink) == 0);
    invalidFilterSink.version = SDGO_OPERATOR_REWARD_FILTER_VERSION;
    invalidFilterSink.callback = nullptr;
    CHECK(
        registerOperatorFilter &&
        registerOperatorFilter(&invalidFilterSink) == 0);
    const SdgoOperatorRewardFilterSinkV1 filterSink = {
        sizeof(filterSink),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        &FilterRewards,
        kFilterUser};
    CHECK(
        registerOperatorFilter &&
        registerOperatorFilter(&filterSink) == 1);
    CHECK(
        registerOperatorFilter &&
        registerOperatorFilter(&filterSink) == 0);
    CHECK((GetModuleHandleW(L"daily_rewards.dll") != nullptr) == daily);
    VerifyPatchedSites(daily);
    CHECK(WaitStatus(runtime, "\"policy_loaded\": true"));

    if (mode == L"outage")
        OutageCase(runtime);
    else if (mode == L"filter_failures")
        FilterFailureCases(runtime);
    else if (mode == L"native")
        NativeGpCase();
    else if (mode == L"progression")
        ProgressionCase();
    else if (daily)
        DailyCase();
    else
        BasicCases(runtime);
    CHECK(InterlockedCompareExchange(
              &g_observed_damage, 0, 0) > 0);
    CHECK(InterlockedCompareExchange(
              &g_invalid_observed_damage, 0, 0) == 0);

    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    if (!g_failures)
        std::cout << "ALL REAL CORE BATTLE REWARD RUNTIME CASES PASSED\n";
    return g_failures ? 1 : 0;
}
