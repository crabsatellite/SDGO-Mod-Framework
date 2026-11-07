// Over-custom (OC) six-stat enhancement, battle server side.
//
// The World Mod writes the player's OC stat deltas into the reserved gap of the
// owned-unit record (_UnitData). The battle server copies that record into the
// room pilot and builds each BattleUnit form from it together with the unit
// definition (UnitDataEx). This Mod runs after the native per-form stat
// builder and adds the deltas with the same per-grade caps the native code
// applies, so a battle computes damage and HP with the OC allocation.
#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/oc_custom_protocol.h"
#include "sdgo/sdgo_api.h"

namespace {

using namespace sdgo::oc_custom;

constexpr char kHost[] = "BattleServer-548A891D-0046C000";

// Native stat builder dispatcher: FUN_004D6790(BattleUnit*, _UnitData*,
// UnitDataEx*, form, mode, type, pilotLevel), __stdcall (RET 0x1C). Returns 0
// on failure.
using BuildStats = int(__stdcall*)(
    std::uint8_t* unit, std::uint8_t* record, std::uint8_t* definition,
    int form, char mode, std::uint8_t type, std::uint8_t pilot_level);
// Native weapon lookup by WeaponNumber, __stdcall (RET 4); DamageSize at +0x3A.
using WeaponLookup = std::uint8_t*(__stdcall*)(std::uint32_t weapon);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
BuildStats g_original = nullptr;
WeaponLookup g_weapon_lookup = nullptr;
const std::uint8_t* g_grade_caps = nullptr;
bool g_log_apply = true;
LONG g_applied = 0;

// BattleUnit layout (verified in the native builder 0x4D6A20):
//   +0x04 int HP (form 0 only), per form stride 0x0C from +0x0E:
//   +0x0E u16 attack, +0x10 u16 defense, +0x12 float special bonus.
constexpr std::size_t kUnitHp = 0x04;
constexpr std::size_t kUnitAttack = 0x0E;
constexpr std::size_t kUnitDefense = 0x10;
constexpr std::size_t kUnitSpecial = 0x12;
constexpr std::size_t kFormStride = 0x0C;
// UnitDataEx: +6 grade, +0x55 WeaponNumber_FatalAttack.
constexpr std::size_t kDefinitionGrade = 0x06;
constexpr std::size_t kDefinitionFatalWeapon = 0x55;
// Downloaded per-grade cap table, 0x10 bytes per grade: HP, attack, defense,
// special (the native builder reads it as DAT_0082B194 + grade * 0x10).
constexpr std::size_t kCapStride = 0x10;
constexpr std::size_t kWeaponDamageSize = 0x3A;

template <class T>
bool ReadAt(const std::uint8_t* base, std::size_t offset, T* value) noexcept {
    __try {
        std::memcpy(value, base + offset, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool WriteAt(std::uint8_t* base, std::size_t offset, const T& value) noexcept {
    __try {
        std::memcpy(base + offset, &value, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadRecordCarrier(const std::uint8_t* record, Carrier* carrier) noexcept {
    std::uint8_t copy[kUnitRecordBytes];
    __try {
        std::memcpy(copy, record, sizeof(copy));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ReadCarrier(copy, carrier);
}

std::int32_t Cap(std::uint8_t grade, std::size_t field) noexcept {
    if (!g_grade_caps || grade == 0 || grade > 4) return 0;
    std::int32_t value = 0;
    if (!ReadAt(g_grade_caps, grade * kCapStride + field, &value)) return 0;
    return value;
}

std::int32_t Clamp(std::int64_t value, std::int32_t cap, std::int32_t floor) noexcept {
    if (cap > 0 && value > cap) value = cap;
    if (value < floor) value = floor;
    return static_cast<std::int32_t>(value);
}

void ApplyUnsafe(
    std::uint8_t* unit,
    std::uint8_t* record,
    std::uint8_t* definition,
    int form) noexcept {
    Carrier carrier;
    if (!ReadRecordCarrier(record, &carrier) ||
        !(carrier.flags & carrier_oc_mode) ||
        !(carrier.flags & carrier_profile) ||
        form < 0 || form > 3)
        return;
    std::uint8_t grade = 0;
    std::uint32_t fatal_weapon = 0;
    if (!ReadAt(definition, kDefinitionGrade, &grade) ||
        !ReadAt(definition, kDefinitionFatalWeapon, &fatal_weapon))
        return;
    const auto stride = static_cast<std::size_t>(form) * kFormStride;

    std::int32_t hp_after = 0;
    if (form == 0 && carrier.delta[stat_hp] != 0) {
        std::int32_t hp = 0;
        if (!ReadAt(unit, kUnitHp, &hp)) return;
        hp_after = Clamp(static_cast<std::int64_t>(hp) + carrier.delta[stat_hp],
                         Cap(grade, 0x0), 1);
        if (!WriteAt(unit, kUnitHp, hp_after)) return;
    }

    std::uint16_t attack = 0, defense = 0;
    if (!ReadAt(unit, kUnitAttack + stride, &attack) ||
        !ReadAt(unit, kUnitDefense + stride, &defense))
        return;
    const auto attack_after = static_cast<std::uint16_t>(
        Clamp(static_cast<std::int64_t>(attack) + carrier.delta[stat_attack],
              Cap(grade, 0x4), 0));
    const auto defense_after = static_cast<std::uint16_t>(
        Clamp(static_cast<std::int64_t>(defense) + carrier.delta[stat_defense],
              Cap(grade, 0x8), 0));
    if (!WriteAt(unit, kUnitAttack + stride, attack_after) ||
        !WriteAt(unit, kUnitDefense + stride, defense_after))
        return;

    float special = 0.0f;
    if (!ReadAt(unit, kUnitSpecial + stride, &special)) return;
    float special_after = special;
    if (carrier.delta[stat_special] != 0 && std::isfinite(special)) {
        special_after = special + static_cast<float>(carrier.delta[stat_special]);
        std::uint32_t damage = 0;
        const auto cap = Cap(grade, 0xC);
        if (cap > 0 && g_weapon_lookup) {
            std::uint8_t* weapon = nullptr;
            __try {
                weapon = g_weapon_lookup(fatal_weapon);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                weapon = nullptr;
            }
            if (weapon && ReadAt(weapon, kWeaponDamageSize, &damage) &&
                static_cast<float>(cap) < static_cast<float>(damage) + special_after)
                special_after = static_cast<float>(cap) - static_cast<float>(damage);
        }
        if (special_after < 0.0f) special_after = 0.0f;
        if (!WriteAt(unit, kUnitSpecial + stride, special_after)) return;
    }

    std::uint64_t uid = 0;
    ReadAt(record, 8, &uid);
    InterlockedIncrement(&g_applied);
    if (g_log_apply && g_api && g_api->Log) {
        g_api->Log(
            g_mod, SDGO_LOG_INFO,
            "oc applied uid=%llu revision=%u form=%d level=%u hp=%d atk=%u def=%u sp=%.1f "
            "(delta hp=%d atk=%d def=%d sp=%d speed=%d agility=%d)",
            static_cast<unsigned long long>(uid), carrier.revision, form, carrier.oc_level,
            hp_after, attack_after, defense_after, special_after,
            carrier.delta[stat_hp], carrier.delta[stat_attack],
            carrier.delta[stat_defense], carrier.delta[stat_special],
            carrier.delta[stat_speed], carrier.delta[stat_agility]);
    }
}

int __stdcall BuildStatsHook(
    std::uint8_t* unit,
    std::uint8_t* record,
    std::uint8_t* definition,
    int form,
    char mode,
    std::uint8_t type,
    std::uint8_t pilot_level) {
    const int result = g_original(unit, record, definition, form, mode, type, pilot_level);
    if (result && unit && record && definition) {
        const DWORD error = GetLastError();
        ApplyUnsafe(unit, record, definition, form);
        SetLastError(error);
    }
    return result;
}

} // namespace

extern "C" __declspec(dllexport) unsigned __cdecl SdgoOcCustomBattleAppliedCountV1() {
    return static_cast<unsigned>(g_applied);
}

SDGO_MOD_ENTRY {
    if (!api || !mod || api->version != SDGO_API_VERSION || !api->host_key ||
        !api->Symbol || !api->HookCode || !api->Log ||
        std::strcmp(api->host_key, kHost) != 0)
        return 0;
    g_api = api;
    g_mod = mod;
    if (!api->SettingInt(mod, "Enabled", 1)) {
        api->Log(mod, SDGO_LOG_INFO, "over-custom battle stats disabled by settings");
        return 0;
    }
    g_log_apply = api->SettingInt(mod, "LogApply", 1) != 0;
    g_weapon_lookup = reinterpret_cast<WeaponLookup>(
        api->Symbol(mod, "oc_custom.battle_weapon_lookup"));
    g_grade_caps = static_cast<const std::uint8_t*>(
        api->Symbol(mod, "oc_custom.battle_grade_caps"));
    if (!g_weapon_lookup || !g_grade_caps ||
        !api->HookCode(mod, "oc_custom.battle_set_unit_stat",
                       reinterpret_cast<void*>(&BuildStatsHook),
                       reinterpret_cast<void**>(&g_original)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "ready: owned-unit OC stat deltas apply to HP, attack, defense and "
             "special damage after the native per-form builder");
    return 1;
}
