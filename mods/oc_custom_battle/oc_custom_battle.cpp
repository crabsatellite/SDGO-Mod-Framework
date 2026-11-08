// Over-custom (OC) six-stat enhancement, battle server side.
//
// The World Mod writes the player's OC stat deltas into the reserved gap of the
// owned-unit record (_UnitData). The battle server copies that record into the
// room pilot and builds each BattleUnit form from it together with the unit
// definition (UnitDataEx). This Mod runs after the native per-form stat
// builder and adds the deltas with the same per-grade caps the native code
// applies, so a battle computes damage and HP with the OC allocation.
//
// Room systems ("room" mode, carrier_room_rule): the records keep their
// custom skills and the room decides. All four native calls of the stat
// builder sit in the human-unit builder (battle.build_human_unit, an EBP frame
// with the room at [ebp+8] and the pilot at [ebp+0x10]); a naked entry records
// that caller frame. That builder copies the record's four custom-skill slots
// (record +0x8A) into the battle unit (+0x644) before it builds the stats, and
// the battle (server and clients, which receive the battle unit) plays the
// skills from that copy. For a room whose title carries !oc the copy is
// cleared and the OC deltas are added; the room's record keeps its skills.
// Every other room stays native.
#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/oc_custom_protocol.h"
#include "sdgo/oc_room_system.h"
#include "sdgo/room_titles_lookup.h"
#include "sdgo/sdgo_api.h"

#pragma intrinsic(_ReturnAddress)

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
LONG g_oc_room_builds = 0;
LONG g_parts_room_builds = 0;
LONG g_g1_room_builds = 0;
LONG g_room_log_budget = 64;

// Room context of the current stat-builder call (set by the naked entry).
std::uintptr_t g_human_build = 0; // battle.build_human_unit
constexpr std::uintptr_t kHumanBuildBytes = 0xAC4;
thread_local std::uint8_t* t_caller_frame = nullptr;
thread_local std::uintptr_t t_caller_return = 0;
// Lobby room lookup (native WaitRoom of a user) for the title fallback.
using LobbyFindFn = int(__thiscall*)(void*, const std::uint32_t*, int*);
using LobbyRoomFn = std::uint8_t*(__thiscall*)(void*, int, std::uint16_t);
using LobbyUnlockFn = void(__thiscall*)(void*, int);
void* g_lobby = nullptr;
LobbyFindFn g_lobby_find = nullptr;
LobbyRoomFn g_lobby_room = nullptr;
LobbyUnlockFn g_lobby_unlock = nullptr;
const SdgoRoomTitleApi* g_title_api = nullptr;
// WaitRoomEx: title length +0x79, title +0x7A (20 native bytes); twelve slots
// from +0x533, stride 0x48E, owner account at +0x429 (AI owners carry bit 31).
constexpr std::size_t kRoomTitleLength = 0x79, kRoomTitle = 0x7A, kRoomTitleMax = 20;
constexpr std::size_t kSlotBase = 0x533, kSlotStride = 0x48E, kSlotOwner = 0x429;
constexpr std::uint32_t kAiOwnerBit = 0x80000000u;

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
    int form,
    bool oc_room) noexcept {
    Carrier carrier;
    if (!ReadRecordCarrier(record, &carrier) ||
        !((carrier.flags & carrier_oc_mode) || ((carrier.flags & carrier_room_rule) && oc_room)) ||
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

bool RawTitle(const std::uint8_t* room, char* text) noexcept {
    __try {
        const std::size_t length = room[kRoomTitleLength];
        if (length > kRoomTitleMax) return false;
        std::memcpy(text, room + kRoomTitle, length);
        text[length] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LobbyTitle(std::uint32_t owner, char* text) noexcept {
    if (!g_lobby || !g_lobby_find || !g_lobby_room || !g_lobby_unlock) return false;
    __try {
        int index = 0;
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(static_cast<std::uint8_t*>(g_lobby) + 0x38);
        EnterCriticalSection(lock);
        const int found = g_lobby_find(static_cast<std::uint8_t*>(g_lobby) + 0x14, &owner, &index);
        LeaveCriticalSection(lock);
        if (!found || index <= 0) return false;
        std::uint8_t* room = g_lobby_room(g_lobby, index, 0);
        if (!room) return false;
        const std::size_t length = room[0x53];
        const bool ok = length <= kRoomTitleMax;
        if (ok) {
            std::memcpy(text, room + 0x54, length);
            text[length] = 0;
        }
        g_lobby_unlock(g_lobby, index);
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SlotOwners(const std::uint8_t* room, std::uint32_t (&owners)[12]) noexcept {
    __try {
        for (int index = 0; index < 12; ++index)
            std::memcpy(&owners[index], room + kSlotBase + index * kSlotStride + kSlotOwner, sizeof(owners[index]));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The room's system, from the full confirmed title when the long-title Mod
// provides one, else the native battle-room title, else the lobby room title.
RoomSystem SystemOfRoom(const std::uint8_t* room, char* shown, std::size_t shown_capacity) {
    std::uint32_t owners[12] = {};
    const bool have_owners = SlotOwners(room, owners);
    if (!g_title_api && g_api && g_api->ModDirectory) g_title_api = sdgo::FindRoomTitleApi(g_api->ModDirectory(g_mod));
    if (g_title_api && have_owners) {
        for (auto owner : owners) {
            if (!owner || (owner & kAiOwnerBit)) continue;
            char full[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
            const std::uint32_t length = g_title_api->QueryForPilot(owner, full, sizeof(full));
            if (length && length <= SDGO_ROOM_TITLE_MAX_UTF8_BYTES && full[length] == 0) {
                strncpy_s(shown, shown_capacity, full, _TRUNCATE);
                return RoomSystemOfTitle(g_api, full, length);
            }
        }
    }
    char title[kRoomTitleMax + 1] = {};
    if (RawTitle(room, title) && title[0]) {
        strncpy_s(shown, shown_capacity, title, _TRUNCATE);
        return RoomSystemOfTitle(g_api, title, static_cast<std::uint32_t>(std::strlen(title)));
    }
    if (have_owners) {
        for (auto owner : owners) {
            if (!owner || (owner & kAiOwnerBit) || !LobbyTitle(owner, title)) continue;
            strncpy_s(shown, shown_capacity, title, _TRUNCATE);
            return RoomSystemOfTitle(g_api, title, static_cast<std::uint32_t>(std::strlen(title)));
        }
    }
    return RoomSystem::parts;
}

// Room of the human-unit build that called the stat builder, or null.
std::uint8_t* CallerRoom() noexcept {
    const auto ret = t_caller_return;
    auto* frame = t_caller_frame;
    t_caller_frame = nullptr;
    t_caller_return = 0;
    if (!g_human_build || !frame || ret <= g_human_build || ret >= g_human_build + kHumanBuildBytes) return nullptr;
    std::uint8_t* room = nullptr;
    return ReadAt(frame, 8, &room) ? room : nullptr;
}

// The human-unit builder copies the record's four custom-skill slots
// (record +0x8A) into the battle unit (+0x644) before it builds the stats;
// battles read the skills from that copy. An OC room clears the copy, so the
// battle plays without the custom skills while the room's record keeps them.
constexpr std::size_t kUnitCustomSkills = 0x644;

bool ReadSkills(const std::uint8_t* source, std::size_t offset, std::uint8_t (&skills)[kCustomSkillCount * 2]) noexcept {
    __try {
        std::memcpy(skills, source + offset, sizeof(skills));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// G改 1.0 rooms: the unit's G改 1.0 loadout replaces the record's custom skills.
bool WriteUnitSkills(std::uint8_t* unit, const std::array<std::uint16_t, 4>& skills) noexcept {
    __try {
        std::memcpy(unit + kUnitCustomSkills, skills.data(), kCustomSkillCount * 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ClearUnitSkills(std::uint8_t* unit) noexcept {
    __try {
        std::memset(unit + kUnitCustomSkills, 0, kCustomSkillCount * 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogNative(RoomSystem system, const std::uint8_t* room, const char* title, const std::uint8_t* unit,
               const std::uint8_t* record, int form, const std::uint8_t* skills) noexcept {
    if (!g_log_apply || InterlockedDecrement(&g_room_log_budget) < 0) return;
    std::uint64_t uid = 0;
    std::uint32_t key = 0;
    std::int32_t hp = 0;
    std::uint16_t attack = 0, defense = 0;
    ReadAt(record, 8, &uid);
    ReadAt(room, 0, &key);
    ReadAt(unit, kUnitHp, &hp);
    const auto stride = static_cast<std::size_t>(form < 0 || form > 3 ? 0 : form) * kFormStride;
    ReadAt(unit, kUnitAttack + stride, &attack);
    ReadAt(unit, kUnitDefense + stride, &defense);
    std::uint16_t slot[kCustomSkillCount] = {};
    std::memcpy(slot, skills, sizeof(slot));
    g_api->Log(g_mod, SDGO_LOG_INFO,
               "oc room system=%s room=%u title=\"%s\" uid=%llu form=%d native hp=%d atk=%u def=%u "
               "skills=%u/%u/%u/%u%s",
               RoomSystemName(system), key, title, static_cast<unsigned long long>(uid), form, hp, attack, defense,
               slot[0], slot[1], slot[2], slot[3],
               system == RoomSystem::oc   ? " (cleared from the battle unit)"
               : system == RoomSystem::g1 ? " (replaced by the G1 loadout)"
                                          : " (kept)");
}

int __stdcall BuildStatsHook(
    std::uint8_t* unit,
    std::uint8_t* record,
    std::uint8_t* definition,
    int form,
    char mode,
    std::uint8_t type,
    std::uint8_t pilot_level) {
    std::uint8_t* room = CallerRoom();
    Carrier carrier;
    const bool room_rule = record && ReadRecordCarrier(record, &carrier) && (carrier.flags & carrier_room_rule);
    if (!room_rule || !room) {
        const int result = g_original(unit, record, definition, form, mode, type, pilot_level);
        if (result && unit && record && definition) {
            const DWORD error = GetLastError();
            ApplyUnsafe(unit, record, definition, form, false);
            SetLastError(error);
        }
        return result;
    }
    const int result = g_original(unit, record, definition, form, mode, type, pilot_level);
    const DWORD error = GetLastError();
    if (result && unit && definition) {
        char title[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
        const RoomSystem system = SystemOfRoom(room, title, sizeof(title));
        std::uint8_t skills[kCustomSkillCount * 2] = {};
        ReadSkills(unit, kUnitCustomSkills, skills);
        LogNative(system, room, title, unit, record, form, skills);
        if (system == RoomSystem::oc) {
            InterlockedIncrement(&g_oc_room_builds);
            ClearUnitSkills(unit);
            ApplyUnsafe(unit, record, definition, form, true);
        } else if (system == RoomSystem::g1) {
            InterlockedIncrement(&g_g1_room_builds);
            // A unit without a G改 1.0 loadout plays without attributes.
            const std::array<std::uint16_t, 4> loadout =
                (carrier.flags & carrier_g1_profile) ? carrier.g1_skills : std::array<std::uint16_t, 4>{};
            WriteUnitSkills(unit, loadout);
            if (g_log_apply && InterlockedDecrement(&g_room_log_budget) >= 0)
                g_api->Log(g_mod, SDGO_LOG_INFO, "oc room system=g1 form=%d g1 skills=%u/%u/%u/%u", form, loadout[0],
                           loadout[1], loadout[2], loadout[3]);
        } else {
            InterlockedIncrement(&g_parts_room_builds);
        }
    }
    SetLastError(error);
    return result;
}

// Records the caller's frame and return address, then continues in the C++
// hook with the stack and every register untouched (ECX included: the native
// builder stores it on entry).
void __stdcall CaptureCaller(std::uint8_t* frame, std::uintptr_t ret) {
    t_caller_frame = frame;
    t_caller_return = ret;
}

__declspec(naked) void BuildStatsEntry() {
    __asm {
        push ecx
        push edx
        push eax
        mov eax, [esp + 12]
        push eax
        push ebp
        call CaptureCaller
        pop eax
        pop edx
        pop ecx
        jmp BuildStatsHook
    }
}

} // namespace

extern "C" __declspec(dllexport) unsigned __cdecl SdgoOcCustomBattleAppliedCountV1() {
    return static_cast<unsigned>(g_applied);
}

// Stat builds decided by a room in "room" mode: [0] OC rooms, [1] G改 2.0
// (current-system) rooms, [2] G改 1.0 rooms when the caller passes three slots.
extern "C" __declspec(dllexport) unsigned __cdecl SdgoOcCustomBattleRoomBuildsV1(unsigned* builds) {
    if (!builds) return 0;
    builds[0] = static_cast<unsigned>(g_oc_room_builds);
    builds[1] = static_cast<unsigned>(g_parts_room_builds);
    return 2;
}

extern "C" __declspec(dllexport) unsigned __cdecl SdgoOcCustomBattleRoomBuildsV2(unsigned* builds, unsigned capacity) {
    if (!builds || capacity < 3) return 0;
    builds[0] = static_cast<unsigned>(g_oc_room_builds);
    builds[1] = static_cast<unsigned>(g_parts_room_builds);
    builds[2] = static_cast<unsigned>(g_g1_room_builds);
    return 3;
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
    g_human_build = reinterpret_cast<std::uintptr_t>(api->Symbol(mod, "battle.build_human_unit"));
    if (auto* lobby = static_cast<void**>(api->Symbol(mod, "battle.lobby_proc"))) g_lobby = *lobby;
    g_lobby_find = reinterpret_cast<LobbyFindFn>(api->Symbol(mod, "battle.lobby_map_find"));
    g_lobby_room = reinterpret_cast<LobbyRoomFn>(api->Symbol(mod, "battle.lobby_room"));
    g_lobby_unlock = reinterpret_cast<LobbyUnlockFn>(api->Symbol(mod, "battle.lobby_release"));
    if (!g_weapon_lookup || !g_grade_caps ||
        !api->HookCode(mod, "oc_custom.battle_set_unit_stat",
                       reinterpret_cast<void*>(&BuildStatsEntry),
                       reinterpret_cast<void**>(&g_original)))
        return 0;
    if (!g_human_build)
        api->Log(mod, SDGO_LOG_WARN, "battle.build_human_unit unavailable; room systems fall back to the current system");
    api->Log(mod, SDGO_LOG_INFO,
             "ready: owned-unit OC stat deltas apply to HP, attack, defense and "
             "special damage after the native per-form builder; room systems %s",
             g_human_build ? "read from the building room's title (!oc, !g1)" : "unavailable");
    return 1;
}
