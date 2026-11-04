// coop_ai - battle server side of the co-op mission wingmen.
//
// Wingmen join a co-op mission as AI pilots, exactly the way the server's own AI battle
// modes add theirs (FUN_004B79D0 for mode 11, FUN_004B88E0 for mode 12, where slots 4 and
// 5 are AI pilots on the human team). Being real room participants, their hits, kills,
// deaths and explosions all go through the game's own code on server and clients.
//
// The battle-start dispatcher (0x004B9BF0) sets up every human pilot of a co-op mission
// room (modes 5, 8 and 10: the modes it exempts from team checks, and the ones the client
// runs as CMissionMode), then announces the start with every slot that has a pilot id
// (battle.start_notify, 0x004B61E0; for these modes only that path calls it, since
// SDBattlePlus.dll through its own detour stub). This mod runs right before that
// announcement and,
// when the room title asks for wingmen ("!aiN lvN"), adds team-1 AI pilots to free slots
// with the same calls and field writes the AI modes use. The clients learn about them
// from the announcement; the client mod mission_ai spawns their units.
//
// The battle room does not carry the title; the lobby room does (WaitRoom: +0x53 length,
// +0x54 title, written by the create-room and room-settings handlers). It is found the
// way those handlers find it: the lobby proc maps a user id (a human pilot id of the
// battle) to the room index under its lock, then the record is locked by index.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <map>

#include "sdgo/sdgo_api.h"
#include "sdgo/room_titles_lookup.h"
#include "sdgo/room_commands.h"
#include "sdgo/ai_catalog_provider.h"
#include "sdgo/mission_ai_lifecycle.h"

namespace
{
    // ---- battle room layout (WaitRoomEx), as read and written by 0x004B79D0/0x004B88E0 ----
    constexpr size_t kRoomMode = 0x05;          // u8; co-op mission modes 5, 8, 10
    constexpr size_t kRoomTitleMax = 20;
    constexpr int kSlots = 12;
    constexpr size_t kSlotInfo = 0x533;         // slot info records, stride 0x48E
    constexpr size_t kSlotInfoStride = 0x48E;
    constexpr size_t kSlotInfoPilotId = 0x429;  // = room + 0x95C + slot * 0x48E
    constexpr size_t kSlotInfoState = 0x48;     // room + 0x57B + slot * 0x48E (set to 0x1E)
    constexpr size_t kSlotInfoTeamA = 0x42D;
    constexpr size_t kSlotInfoTeamB = 0x434;
    constexpr size_t kPilots = 0x3CE4;          // BattlePilot records, stride 0x1F54
    constexpr size_t kPilotStride = 0x1F54;
    constexpr size_t kPilotForm = 0x33C;        // u8, selects the unit record
    constexpr size_t kPilotUnits = 0x340;       // unit records, stride 0x658
    constexpr size_t kUnitStride = 0x658;
    bool IsMissionMode(uint8_t mode)
    {
        return mode == 5 || mode == 8 || mode == 10;
    }
    constexpr uint8_t kHumanTeam = 1;

    using StartNotifyFn = void(__thiscall*)(void* proc, uint8_t* room, uint32_t key, int index, uint32_t a, uint32_t b);
    using BuildAiPilotFn = int(__thiscall*)(uint8_t* room, uint8_t* pilot, uint8_t* unit, uint32_t npcData);
    using PilotResetFn = void(__thiscall*)(uint8_t* part);
    using PilotStateIdFn = void(__thiscall*)(uint8_t* part, uint32_t id);
    using PilotStateRoomFn = void(__thiscall*)(uint8_t* part, uint8_t* room);
    using PilotStateModeFn = void(__thiscall*)(uint8_t* part, uint8_t a, uint8_t b);
    using SetupAiPilotFn = int(__thiscall*)(void* proc, uint8_t* info, uint8_t* pilot, uint8_t a, uint8_t b);
    using MapFindFn = int(__thiscall*)(void* map, const uint32_t* key, int* value);
    using LobbyRoomFn = uint8_t*(__thiscall*)(void* proc, int index, int tag);
    using LobbyReleaseFn = void(__thiscall*)(void* proc, int index);
    using RoomTickFn = int(__thiscall*)(uint8_t*, uint32_t);
    using BattleEndFn = void(__thiscall*)(uint8_t*, uint16_t, uint16_t, int);
    using RespawnPilotFn = int(__thiscall*)(uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t);
    using RespawnRefreshFn = void(__thiscall*)(uint8_t*, int, uint8_t*, int, int);
    using PilotStateRespawnFn = int(__thiscall*)(uint8_t*);

    // ---- lobby room layout (WaitRoom), as the create-room handler writes it ----------
    constexpr size_t kLobbyMapLock = 0x38;      // CRITICAL_SECTION in the lobby proc
    constexpr size_t kLobbyMap = 0x14;          // user id -> room index
    constexpr size_t kLobbyTitleLength = 0x53;  // u8
    constexpr size_t kLobbyTitle = 0x54;        // up to 20 bytes in the native encoding

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    const SdgoRoomTitleApi* g_titleApi = nullptr;
    StartNotifyFn g_startNotify = nullptr;
    uint8_t** g_aiNpcList = nullptr;
    void* g_waitRoomProc = nullptr;
    BuildAiPilotFn g_buildAiPilot = nullptr;
    PilotResetFn g_pilotReset = nullptr;
    PilotStateIdFn g_pilotStateId = nullptr;
    PilotStateRoomFn g_pilotStateRoom = nullptr;
    PilotStateModeFn g_pilotStateMode = nullptr;
    SetupAiPilotFn g_setupAiPilot = nullptr;
    uint8_t* g_lobbyProc = nullptr;
    MapFindFn g_lobbyMapFind = nullptr;
    LobbyRoomFn g_lobbyRoom = nullptr;
    LobbyReleaseFn g_lobbyRelease = nullptr;
    RoomTickFn g_roomTick = nullptr;
    BattleEndFn g_battleEnd = nullptr;
    RespawnPilotFn g_respawnPilot = nullptr;
    RespawnRefreshFn g_respawnRefresh = nullptr;
    PilotStateRespawnFn g_pilotStateRespawn = nullptr;
    uint32_t& SlotPilotId(uint8_t* room, int slot);

    struct MissionLives {
        bool infinite = false;
        std::array<uint32_t, kSlots> pilots{};
    };
    SRWLOCK g_livesLock = SRWLOCK_INIT;
    std::map<uint8_t*, MissionLives> g_lives;
    // Only our own native respawn call may bypass the mission quota. Never
    // change death statistics, mission data, room mode or human life counts.
    thread_local uint8_t* g_infinitePilot = nullptr;

    bool CopyLives(uint8_t* room, MissionLives* lives)
    {
        AcquireSRWLockShared(&g_livesLock);
        auto found = g_lives.find(room);
        const bool tracked = found != g_lives.end();
        if (tracked) *lives = found->second;
        ReleaseSRWLockShared(&g_livesLock);
        return tracked;
    }

    // -1: another room keeps its native scan; 0: humans exhausted; 1: a human
    // still has a native life available. AI revival timing is independent.
    int HumanMissionState(uint8_t* room)
    {
        MissionLives lives;
        if (!room || !IsMissionMode(room[kRoomMode]) || !CopyLives(room, &lives)) return -1;
        bool owned = false;
        for (int slot = 0; slot < kSlots; ++slot)
            if (lives.pilots[slot] && SlotPilotId(room, slot) == lives.pilots[slot] &&
                *reinterpret_cast<uint32_t*>(room + kPilots + slot * kPilotStride) == lives.pilots[slot]) owned = true;
        if (!owned) return -1;
        uint8_t* rules = *reinterpret_cast<uint8_t**>(room + 0x316A0);
        const uint32_t limit = rules ? *reinterpret_cast<uint32_t*>(rules + 0x28) : 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            const uint32_t id = SlotPilotId(room, slot);
            uint8_t* pilot = room + kPilots + slot * kPilotStride;
            if (!id || (id & 0x80000000u) || *reinterpret_cast<uint32_t*>(pilot) != id) continue;
            if (*reinterpret_cast<uint32_t*>(pilot + 0x1A) < limit) return 1;
        }
        return 0;
    }

    void __cdecl MissionRespawnTick(uint8_t* room, uint32_t now)
    {
        if (!room || !IsMissionMode(room[kRoomMode])) return;
        MissionLives lives;
        if (!CopyLives(room, &lives)) return;
        __try {
            for (int slot = 0; slot < kSlots; ++slot) {
                const uint32_t id = lives.pilots[slot];
                if (!id || SlotPilotId(room, slot) != id) continue;
                uint8_t* pilot = room + kPilots + slot * kPilotStride;
                if (*reinterpret_cast<uint32_t*>(pilot) != id || pilot[kPilotForm] >= 4) continue;
                uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
                if (*reinterpret_cast<uint32_t*>(unit + 0x34) != 0) continue;
                uint8_t* previous = g_infinitePilot;
                int result = 0;
                __try {
                    g_infinitePilot = lives.infinite ? pilot : nullptr;
                    result = g_respawnPilot(room, now, pilot, id,
                        *reinterpret_cast<uint32_t*>(room + kSlotInfo + slot * kSlotInfoStride + 0x472));
                } __finally {
                    g_infinitePilot = previous;
                }
                if (!result || *reinterpret_cast<uint32_t*>(unit + 0x34) == 0) continue;
                g_respawnRefresh(room, 0, pilot, 0, 0);
                g_pilotStateRespawn(pilot + 0x1D90);
                g_api->Log(g_mod, SDGO_LOG_INFO,
                    "mission AI %08lX respawned: %s, native deaths %lu",
                    static_cast<unsigned long>(id), lives.infinite ? "infinite" : "player",
                    static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(pilot + 0x1A)));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX in mission AI respawn", GetExceptionCode());
        }
    }

    void __cdecl MissionEnded(uint8_t* room)
    {
        // Native end has already released pilot battle state. Remove only this
        // Mod's exact AI records before the lobby updates human account data.
        AcquireSRWLockExclusive(&g_livesLock);
        auto found = g_lives.find(room);
        if (found != g_lives.end()) {
            for (int slot = 0; slot < kSlots; ++slot)
                if (found->second.pilots[slot] && SlotPilotId(room, slot) == found->second.pilots[slot]) {
                    memset(room + kSlotInfo + slot * kSlotInfoStride, 0, kSlotInfoStride);
                    memset(room + kPilots + slot * kPilotStride, 0, kPilotStride);
                }
            g_lives.erase(found);
        }
        ReleaseSRWLockExclusive(&g_livesLock);
    }

    int __fastcall RoomTick(uint8_t* room, void*, uint32_t now)
    {
        const int result = g_roomTick(room, now);
        MissionRespawnTick(room, now);
        return result;
    }

    void __fastcall BattleEnd(uint8_t* room, void*, uint16_t a, uint16_t b, int now)
    {
        g_battleEnd(room, a, b, now);
        MissionEnded(room);
    }

    struct Settings
    {
        int requireRoomCommand = 1;
        int defaultCount = 0; // 0 = fill the free player slots
        int squadSize = 4;
        int defaultLevel = 2;
        int logRoom = 1;
        std::vector<uint32_t> levels[6]; // [1..5]
    } g_settings;

    unsigned g_random = 0;

    unsigned NextRandom()
    {
        g_random ^= g_random << 13;
        g_random ^= g_random >> 17;
        g_random ^= g_random << 5;
        return g_random;
    }

    std::vector<uint32_t> ParseIds(const char* text)
    {
        std::vector<uint32_t> ids;
        const char* p = text;
        while (*p) {
            while (*p == ' ' || *p == ',')
                ++p;
            if (*p < '0' || *p > '9')
                break;
            uint32_t value = 0;
            bool overflow = false;
            while (*p >= '0' && *p <= '9') {
                uint32_t digit = static_cast<uint32_t>(*p++ - '0');
                if (value > (UINT32_MAX - digit) / 10)
                    overflow = true;
                if (!overflow)
                    value = value * 10 + digit;
            }
            if (!overflow)
                ids.push_back(value);
        }
        return ids;
    }

    void LoadSettings()
    {
        g_settings.requireRoomCommand = g_api->SettingInt(g_mod, "RequireRoomCommand", 1);
        g_settings.defaultCount = g_api->SettingInt(g_mod, "DefaultCount", 0);
        g_settings.squadSize = g_api->SettingInt(g_mod, "SquadSize", 4);
        g_settings.defaultLevel = g_api->SettingInt(g_mod, "DefaultLevel", 2);
        if (g_settings.defaultCount < 0 || g_settings.defaultCount > kSlots)
            g_settings.defaultCount = 0;
        if (g_settings.squadSize < 1 || g_settings.squadSize > kSlots)
            g_settings.squadSize = 4;
        if (g_settings.defaultLevel < 1 || g_settings.defaultLevel > 5)
            g_settings.defaultLevel = 2;
        g_settings.logRoom = g_api->SettingInt(g_mod, "LogRoom", 1);
        for (int level = 1; level <= 5; ++level) {
            char key[16];
            char value[1024];
            sprintf_s(key, "Level%d", level);
            g_api->SettingString(g_mod, key, "", value, sizeof(value));
            g_settings.levels[level] = ParseIds(value);
        }
    }

    // ---- room ------------------------------------------------------------------

    uint32_t& SlotPilotId(uint8_t* room, int slot)
    {
        return *reinterpret_cast<uint32_t*>(room + kSlotInfo + slot * kSlotInfoStride + kSlotInfoPilotId);
    }

    std::string Printable(const char* text)
    {
        std::string out;
        for (const char* c = text; *c; ++c)
            out += (static_cast<unsigned char>(*c) >= 0x20 && static_cast<unsigned char>(*c) < 0x7F) ? *c : '?';
        return out;
    }

    struct Plan
    {
        bool enabled = false;
        int count = 0;
        int level = 2;
        bool infinite = false;
    };

    // "!ai", "!ai3", options after it: lvN. Case-insensitive.
    Plan ParseTitle(const char* title)
    {
        Plan plan;
        plan.count = g_settings.defaultCount;
        plan.level = g_settings.defaultLevel;
        namespace commands=sdgo::room_commands;
        SdgoRoomCommand command{};
        if (!commands::Find(g_api,title,SDGO_ROOM_TITLE_MAX_UTF8_BYTES,"ai",
                            SDGO_ROOM_COMMAND_ALLOW_COUNT,&command)) {
            plan.enabled = g_settings.requireRoomCommand == 0;
            return plan;
        }
        plan.enabled = true;
        if(command.has_count) plan.count=static_cast<int>((std::min)(command.count,uint32_t{kSlots}));
        uint32_t cursor=command.arguments_offset;
        commands::Token argument;
        while(commands::Next(title,command.arguments_offset+command.arguments_length,cursor,argument)) {
            const char* token=title+argument.offset;
            if(argument.length==3 && commands::Lower(token[0])=='l' && commands::Lower(token[1])=='v'
               && token[2]>='1' && token[2]<='5') plan.level=token[2]-'0';
            std::string option(token, argument.length);
            for (char& c : option) c = commands::Lower(c);
            if (option == "inf" || option == "infinite" || option == "respawn=infinite") plan.infinite = true;
            if (option == "player" || option == "respawn=player") plan.infinite = false;
        }
        return plan;
    }

    // ---- AI NPC list ---------------------------------------------------------------

    // The NPC data ids in the server's AI-NPC list, walked like 0x00428820 does.
    std::vector<uint32_t> AiNpcIds()
    {
        std::vector<uint32_t> ids;
        uint8_t* list = *g_aiNpcList;
        if (!list)
            return ids;
        auto lock = reinterpret_cast<CRITICAL_SECTION*>(list + 0x18);
        EnterCriticalSection(lock);
        uint8_t* end = *reinterpret_cast<uint8_t**>(list + 0x8);
        uint8_t* node = *reinterpret_cast<uint8_t**>(list + 0x4);
        int count = *reinterpret_cast<int*>(list + 0xC);
        for (int i = 0; i < count && node; ++i) {
            node = *reinterpret_cast<uint8_t**>(node + 0x8);
            if (!node || node == end)
                break;
            auto data = *reinterpret_cast<uint32_t**>(node);
            if (data)
                ids.push_back(*data);
        }
        LeaveCriticalSection(lock);
        return ids;
    }

    bool Contains(const std::vector<uint32_t>& ids, uint32_t id)
    {
        for (uint32_t x : ids)
            if (x == id)
                return true;
        return false;
    }

    // ---- adding one AI pilot (same steps as the AI battle modes) ----------------------

    bool AddAiPilot(uint8_t* room, int slot, uint32_t npcData)
    {
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        uint8_t* info = room + kSlotInfo + slot * kSlotInfoStride;
        memset(pilot, 0, kPilotStride);
        uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
        if (!g_buildAiPilot(room, pilot, unit, npcData)) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: building the AI pilot for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            memset(pilot, 0, kPilotStride);
            return false;
        }
        uint32_t id = *reinterpret_cast<uint32_t*>(pilot);
        SlotPilotId(room, slot) = id;
        info[kSlotInfoState] = 0x1E;

        // per-slot battle setup, AI branch, human team (0x004B95D8..0x004B9861)
        *reinterpret_cast<uint32_t*>(pilot + 0x09) = 1;
        pilot[0x0D] = 0;
        pilot[0x04] = kHumanTeam;
        pilot[0x39] = kHumanTeam;
        *reinterpret_cast<uint32_t*>(pilot + 0x05) = 30000;
        info[kSlotInfoTeamA] = kHumanTeam;
        info[kSlotInfoTeamB] = kHumanTeam;
        *reinterpret_cast<uint16_t*>(pilot + 0x1E) = 1000;
        pilot[0x1CA0] = 0;
        pilot[0x1CA1] = 0;
        pilot[0x1CAA] = 0;
        pilot[0x1CAB] = 0;
        pilot[0x1CA8] = 0;
        pilot[0x1CAC] = 0;
        pilot[0x1CAD] = 0;
        *reinterpret_cast<uint32_t*>(pilot + 0x1CB0) = 0;
        g_pilotReset(pilot + 0x1CE0);
        *reinterpret_cast<uint32_t*>(pilot + 0x1F44) = 1;
        pilot[0x1F48] = 0;
        g_pilotStateId(pilot + 0x1D90, id);
        pilot[0x1D93] = static_cast<uint8_t>(pilot[0x1CAC] + 1);
        g_pilotStateRoom(pilot + 0x1D90, room);
        g_pilotStateMode(pilot + 0x1D90, room[4], room[5]);
        if (!g_setupAiPilot(g_waitRoomProc, info, pilot, room[4], room[5])) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: AI pilot setup for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            SlotPilotId(room, slot) = 0;
            memset(pilot, 0, kPilotStride);
            return false;
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "slot %d: AI pilot %08lX (NPC %lu) joined on team %u", slot,
                   static_cast<unsigned long>(id), static_cast<unsigned long>(npcData), kHumanTeam);
        return true;
    }

    // Title of the lobby room of a user; false when the user is in none.
    bool LobbyTitle(uint32_t userId, char* title, int* index)
    {
        *index = 0;
        auto lock = reinterpret_cast<CRITICAL_SECTION*>(g_lobbyProc + kLobbyMapLock);
        EnterCriticalSection(lock);
        g_lobbyMapFind(g_lobbyProc + kLobbyMap, &userId, index);
        LeaveCriticalSection(lock);
        if (*index == 0)
            return false;
        char extended[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
        const uint32_t extendedLength = g_titleApi ? g_titleApi->QueryForPilot(userId, extended, sizeof(extended)) : 0;
        uint8_t* lobby = g_lobbyRoom(g_lobbyProc, *index, 0);
        if (!lobby)
            return false;
        size_t length = lobby[kLobbyTitleLength];
        if (length > kRoomTitleMax)
            length = kRoomTitleMax;
        memcpy(title, lobby + kLobbyTitle, length);
        title[length] = '\0';
        title[strnlen(title, length)] = '\0';
        if (extendedLength && extendedLength <= SDGO_ROOM_TITLE_MAX_UTF8_BYTES && extended[extendedLength] == '\0')
            memcpy(title, extended, extendedLength + 1);
        g_lobbyRelease(g_lobbyProc, *index);
        return true;
    }

    void AddWingmen(uint8_t* room)
    {
        if (!IsMissionMode(room[kRoomMode]))
            return;
        int humans = 0;
        int ai = 0;
        char title[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
        bool titled = false;
        uint32_t titleOwner = 0;
        int lobbyIndex = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (id == 0)
                continue;
            if (id & 0x80000000u) {
                ++ai;
                continue;
            }
            ++humans;
            if (!titled && LobbyTitle(id, title, &lobbyIndex)) {
                titled = true;
                titleOwner = id;
            }
        }
        if (!titled) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "mission start: none of the %d human(s) is in a lobby room; no title", humans);
            return;
        }
        Plan plan = ParseTitle(title);
        if (g_settings.logRoom)
            g_api->Log(g_mod, SDGO_LOG_INFO, "mission start: lobby room %d (user %lu) title '%s', %d human(s), %d AI; wingmen %s",
                       lobbyIndex, static_cast<unsigned long>(titleOwner), Printable(title).c_str(), humans, ai,
                       plan.enabled ? "requested" : "not requested");
        if (!plan.enabled || humans == 0)
            return;

        int wanted = g_settings.squadSize - humans - ai;
        if (plan.count > 0 && plan.count < wanted)
            wanted = plan.count;
        if (wanted <= 0)
            return;

        std::vector<uint32_t> available = AiNpcIds();
        std::vector<uint32_t> pool;
        const bool catalog = sdgo::AiCatalogPool(g_api, g_mod, plan.level, pool);
        if (!catalog)
            for (uint32_t id : g_settings.levels[plan.level])
                if (Contains(available, id))
                    pool.push_back(id);
        if (pool.empty()) {
            if (catalog) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "AI catalog has no usable Level%d entries; check catalog source-data diagnostics", plan.level);
                return;
            }
            g_api->Log(g_mod, SDGO_LOG_WARN, "no NPC of Level%d is in the server's AI-NPC list (%u entries)",
                       plan.level, static_cast<unsigned>(available.size()));
            return;
        }

        int added = 0;
        for (int slot = 0; slot < kSlots && added < wanted; ++slot) {
            if (SlotPilotId(room, slot) != 0)
                continue;
            uint32_t pick = pool[NextRandom() % pool.size()];
            if (pool.size() > static_cast<size_t>(wanted)) {
                // no duplicate units in one squad while the pool allows it
                for (int tries = 0; tries < 20; ++tries) {
                    bool used = false;
                    for (int other = 0; other < kSlots; ++other) {
                        uint32_t id = SlotPilotId(room, other);
                        if (id & 0x80000000u) {
                            uint8_t* p = room + kPilots + other * kPilotStride;
                            used = used || *reinterpret_cast<uint32_t*>(p + kPilotUnits + p[kPilotForm] * kUnitStride) ==
                                               pick;
                        }
                    }
                    if (!used)
                        break;
                    pick = pool[NextRandom() % pool.size()];
                }
            }
            if (AddAiPilot(room, slot, pick)) {
                AcquireSRWLockExclusive(&g_livesLock);
                MissionLives& lives = g_lives[room];
                lives.infinite = plan.infinite;
                lives.pilots[slot] = SlotPilotId(room, slot);
                ReleaseSRWLockExclusive(&g_livesLock);
                ++added;
            }
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "%d wingmen added (level %d)", added, plan.level);
    }

    bool GuardedAddWingmen(uint8_t* room, DWORD* code)
    {
        __try {
            AddWingmen(room);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *code = GetExceptionCode();
            return false;
        }
    }

    void __fastcall StartNotify(void* proc, void* /*edx*/, uint8_t* room, uint32_t key, int index, uint32_t a,
                                uint32_t b)
    {
        if (room && IsMissionMode(room[kRoomMode])) {
            // A second start announcement for the same native pilots must not
            // discard their policy. New slot identities invalidate old state.
            AcquireSRWLockExclusive(&g_livesLock);
            auto found = g_lives.find(room);
            if (found != g_lives.end()) {
                for (int slot = 0; slot < kSlots; ++slot)
                    if (found->second.pilots[slot] && found->second.pilots[slot] != SlotPilotId(room, slot)) {
                        g_lives.erase(found);
                        break;
                    }
            }
            ReleaseSRWLockExclusive(&g_livesLock);
            DWORD code = 0;
            if (!GuardedAddWingmen(room, &code))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while adding wingmen; this battle has none", code);
        }
        g_startNotify(proc, room, key, index, a, b);
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
}

static void* g_missionGateOriginal = nullptr;
static void* g_missionGateAllowed = nullptr;
static void* g_missionDefeatOriginal = nullptr;
static void* g_missionDefeatContinue = nullptr;
static void* g_missionDefeatExhausted = nullptr;
static int __stdcall InfiniteMissionPilot(uint8_t* pilot)
{
    return pilot && pilot == g_infinitePilot;
}
static __declspec(naked) void MissionRespawnGate()
{
    __asm {
        pushfd
        pushad
        push dword ptr [ebp + 0x0C]
        call InfiniteMissionPilot
        test eax, eax
        jz native_gate
        popad
        popfd
        jmp dword ptr [g_missionGateAllowed]
    native_gate:
        popad
        popfd
        jmp dword ptr [g_missionGateOriginal]
    }
}

static int __stdcall HumanMissionRoom(uint8_t* room)
{
    __try { return HumanMissionState(room); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Replace only the mission's all-participant life scan with a human-only
// decision. A living AI cannot prevent defeat once the humans are exhausted.
// Objective, protected-target, timer and scripted failures remain native.
static __declspec(naked) void MissionDefeatGate()
{
    __asm {
        pushfd
        pushad
        push dword ptr [ebp - 0x480]
        call HumanMissionRoom
        test eax, eax
        js native_scan
        jz humans_exhausted
        popad
        popfd
        jmp dword ptr [g_missionDefeatContinue]
    humans_exhausted:
        popad
        popfd
        jmp dword ptr [g_missionDefeatExhausted]
    native_scan:
        popad
        popfd
        jmp dword ptr [g_missionDefeatOriginal]
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    g_titleApi = sdgo::FindRoomTitleApi(api->ModDirectory(mod));
    bool bound = Bind(g_aiNpcList, "battle.ai_npc_list") && Bind(g_waitRoomProc, "battle.waitroom_proc") &&
                 Bind(g_buildAiPilot, "battle.build_ai_pilot") && Bind(g_pilotReset, "battle.pilot_reset") &&
                 Bind(g_pilotStateId, "battle.pilot_state_id") && Bind(g_pilotStateRoom, "battle.pilot_state_room") &&
                 Bind(g_pilotStateMode, "battle.pilot_state_mode") && Bind(g_setupAiPilot, "battle.setup_ai_pilot") &&
                 Bind(g_lobbyProc, "battle.lobby_proc") &&
                 Bind(g_lobbyMapFind, "battle.lobby_map_find") && Bind(g_lobbyRoom, "battle.lobby_room") &&
                 Bind(g_lobbyRelease, "battle.lobby_release") &&
                 Bind(g_respawnPilot, "battle.respawn_pilot") &&
                 Bind(g_respawnRefresh, "battle.respawn_refresh") &&
                 Bind(g_pilotStateRespawn, "battle.pilot_state_respawn") &&
                 Bind(g_missionGateAllowed, "battle.mission_respawn_allowed") &&
                 Bind(g_missionDefeatContinue, "battle.mission_defeat_continue") &&
                 Bind(g_missionDefeatExhausted, "battle.mission_lives_exhausted");
    if (!bound)
        return 0;
    LoadSettings();
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    g_random = static_cast<unsigned>(counter.LowPart ^ GetTickCount() ^ GetCurrentProcessId()) | 1u;
    if (!api->HookCode(mod, "battle.start_notify", reinterpret_cast<void*>(&StartNotify),
                       reinterpret_cast<void**>(&g_startNotify)))
        return 0;
    if (!api->HookCode(mod, "battle.mission_respawn_gate", reinterpret_cast<void*>(&MissionRespawnGate),
                       &g_missionGateOriginal)) return 0;
    if (!api->HookCode(mod, "battle.mission_human_lives_check", reinterpret_cast<void*>(&MissionDefeatGate),
                       &g_missionDefeatOriginal)) return 0;
    static const SdgoMissionServerLifecycleV1 lifecycle = {
        sizeof(lifecycle), 1, &MissionRespawnTick, &MissionEnded};
    HMODULE pvp = sdgo::MissionSibling(api, mod, L"pvp_ai\\pvp_ai.dll");
    if (pvp) {
        auto share = reinterpret_cast<SdgoRegisterMissionServerV1>(
            GetProcAddress(pvp, "SdgoPvpAiRegisterMissionV1"));
        if (!share || !share(&lifecycle)) {
            api->Log(mod, SDGO_LOG_ERROR, "matching pvp_ai lifecycle bridge is required; update both Mods");
            return 0;
        }
    } else if (!api->HookCode(mod, "battle.room_tick", reinterpret_cast<void*>(&RoomTick),
                              reinterpret_cast<void**>(&g_roomTick)) ||
               !api->HookCode(mod, "battle.end", reinterpret_cast<void*>(&BattleEnd),
                              reinterpret_cast<void**>(&g_battleEnd))) return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: co-op missions whose room title has !ai get AI wingmen");
    return 1;
}
