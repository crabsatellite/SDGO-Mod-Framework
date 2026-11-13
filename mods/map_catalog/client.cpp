// map_catalog: registers imported maps and missions produced by the neutral
// map package compiler. Map rows (kind 0x0F) are inserted right after the
// native map table loads; mission records are inserted after every table has
// loaded, immediately before the game's derived-data pass. Resources (terrain,
// models, textures, sprites) come from a sealed overlay bundle owned by
// "map_catalog".
#include <windows.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <locale.h>
#include <memory>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/map_catalog_format.h"
#include "presentation.h"
#include "boundary_runtime.h"
#include "shield_contact_presentation.h"
#include "damage_gate_client.h"
#include "sdgo/map_boundary_contact.h"
#include "sdgo/map_boundary_format.h"
#include "sdgo/mission_damage_gate_format.h"
#include "sdgo/mission_retreat_format.h"
#include "sdgo/map_catalog_mission_observer.h"
#include "retreat_client.h"
#include "../../support/native_resource_overlay/native_resource_overlay.h"

namespace {
using namespace sdgo::map_catalog;

struct NativeValue {
    uint32_t type;
    uint8_t* record;
};
struct NativePair {
    uint32_t id;
    NativeValue* value;
};
struct InsertResult {
    void* node;
    uint8_t inserted;
    uint8_t padding[3];
};
static_assert(sizeof(NativeValue) == 8 && sizeof(NativePair) == 8 && sizeof(InsertResult) == 8);

using GridFind = bool(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
using GridData = void*(__cdecl*)();
using LoadTable = void(__thiscall*)(void*, const wchar_t*, uint32_t);
using TablesReady = void(__thiscall*)(void*);
using Allocate = void*(__cdecl*)(size_t);
using Release = void(__cdecl*)(void*);
using MakeNode = void*(__thiscall*)(void*, const NativePair*);
using InsertNode = void(__thiscall*)(void*, InsertResult*, void*, char);
using RegisterRecord = void(__thiscall*)(void*, const void*, uint32_t, uint32_t);
using MapFind = void(__thiscall*)(void*, void**, const uint32_t*);
struct lua_State;
using LuaCFunction = int(__cdecl*)(lua_State*);
using LuaReader = const char*(__cdecl*)(lua_State*, void*, size_t*);
using LuaStateFn = lua_State*(__cdecl*)(lua_State*);
using LuaLoad = int(__cdecl*)(lua_State*, LuaReader, void*, const char*);
using LuaPcall = int(__cdecl*)(lua_State*, int, int, int);
using LuaGetTop = int(__cdecl*)(lua_State*);
using LuaSetTop = void(__cdecl*)(lua_State*, int);
using LuaToString = const char*(__cdecl*)(lua_State*, int);
using LuaPushNumber = void(__cdecl*)(lua_State*, double);
using LuaPushString = void(__cdecl*)(lua_State*, const char*);
using LuaPushCClosure = void(__cdecl*)(lua_State*, LuaCFunction, int);
using LuaSetTable = void(__cdecl*)(lua_State*, int);
using MissionCall = int(__cdecl*)(const char*, void*);
using MissionUpdate = void(__thiscall*)(void*);
using GameApp = uint8_t*(__cdecl*)();
using UiMessage = int(__thiscall*)(void*, const uint8_t*);
using FindObject = uint8_t*(__thiscall*)(uint8_t*, uint32_t);
using ObjectForPilot = uint32_t(__thiscall*)(uint8_t*, uint32_t);
using SetTerrainRect = void(__thiscall*)(uint8_t*, float, float, float, float);
using SetTerrainRectOnOff = void(__thiscall*)(uint8_t*, bool);
using SetHp = void(__thiscall*)(uint8_t*, int32_t);
using Network = uint8_t*(__cdecl*)();
using NetworkSend = int(__thiscall*)(void*, void*, const void*, int, int);
using MapDamageHandler = int(__thiscall*)(void*, const uint8_t*);
using QueueState = bool(__thiscall*)(uint8_t*, uint8_t*);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Catalog g_catalog;
GridFind g_find = nullptr;
GridData g_data = nullptr;
LoadTable g_loadMapTable = nullptr;
TablesReady g_tablesReady = nullptr;
Allocate g_allocate = nullptr;
Release g_release = nullptr;
MakeNode g_makeNode = nullptr;
InsertNode g_insertNode = nullptr;
RegisterRecord g_registerRecord = nullptr;
MapFind g_mapFind = nullptr;
LuaStateFn g_luaState = nullptr;
LuaLoad g_luaLoad = nullptr;
LuaPcall g_luaPcall = nullptr;
LuaGetTop g_luaGetTop = nullptr;
LuaSetTop g_luaSetTop = nullptr;
LuaToString g_luaToString = nullptr;
LuaPushNumber g_luaPushNumber = nullptr;
LuaPushString g_luaPushString = nullptr;
LuaPushCClosure g_luaPushCClosure = nullptr;
LuaSetTable g_luaSetTable = nullptr;
MissionCall g_missionCall = nullptr;
MissionUpdate g_missionUpdate = nullptr;
GameApp g_gameApp = nullptr;
UiMessage g_missionMessage = nullptr;
uint8_t** g_unitManager = nullptr;
FindObject g_findObject = nullptr;
ObjectForPilot g_objectForPilot = nullptr;
SetTerrainRect g_setTerrainRect = nullptr;
SetTerrainRectOnOff g_setTerrainRectOnOff = nullptr;
SetHp g_setHp = nullptr;
Network g_network = nullptr;
NetworkSend g_networkSend = nullptr;
MapDamageHandler g_mapDamageHandler = nullptr;
QueueState g_queueState = nullptr;
std::vector<sdgo::map_boundary::Entry> g_boundaries;
std::vector<sdgo::mission_damage_gate::Entry> g_damageGates;
std::vector<sdgo::mission_retreat::Entry> g_retreats;
struct PendingDamageGateNotice {
    uint32_t recipient = 0;
    uint32_t target = 0;
};
struct DamageGateVisualState {
    sdgo::mission_damage_gate::Entry entry{};
    std::vector<sdgo::map_catalog::damage_gate_client::GuardObservation> guards;
    std::vector<sdgo::map_catalog::damage_gate_client::TargetHealthObservation> targets;
    bool serverDamageObserved = false;
};
constexpr size_t kPendingDamageGateNotices = 16;
SRWLOCK g_damageGateNoticeLock = SRWLOCK_INIT;
std::array<PendingDamageGateNotice, kPendingDamageGateNotices> g_pendingDamageGateNotices{};
size_t g_pendingDamageGateNoticeCount = 0;
std::vector<DamageGateVisualState> g_damageGateStates;
std::vector<sdgo::map_catalog::boundary_runtime::TargetCooldown> g_damageGateShieldCooldowns;
std::vector<sdgo::map_catalog::boundary_runtime::TargetCooldown> g_damageGateWarningCooldowns;
uint32_t g_damageGateMission = 0;
uint32_t g_damageGatePhase = 0;
bool g_damageGateSnapshotFault = false;
bool g_damageGateFxFault = false;
std::vector<sdgo::map_catalog::boundary_runtime::ActorStamp> g_boundaryActors;
uint32_t g_boundaryMission = 0;
uint32_t g_boundaryPhase = 0;
uint64_t g_lastBoundaryWarning = 0;
uint32_t g_lastBoundaryWarningSpeech = 0;
std::vector<sdgo::map_catalog::boundary_runtime::TargetCooldown> g_boundaryDamageCooldowns;
SRWLOCK g_boundaryReceiptLock = SRWLOCK_INIT;
std::vector<sdgo::map_catalog::boundary_runtime::PendingBoundaryReceipt> g_boundaryReceipts;
uint64_t g_lastBoundaryContactFx = 0;
bool g_boundaryContactFxFault = false;
volatile LONG g_pendingBoundaryContactFx = 0;
SRWLOCK g_localBoundaryReceiptLock = SRWLOCK_INIT;
SdgoMapLocalBoundaryReceiptV1 g_localBoundaryReceipt{
    sizeof(SdgoMapLocalBoundaryReceiptV1), SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_VERSION};

void PublishLocalBoundaryReceipt(SdgoMapLocalBoundaryReceiptV1& value) {
    AcquireSRWLockExclusive(&g_localBoundaryReceiptLock);
    value.sequence = g_localBoundaryReceipt.sequence + 1;
    if (!value.sequence) value.sequence = 1;
    g_localBoundaryReceipt = value;
    ReleaseSRWLockExclusive(&g_localBoundaryReceiptLock);
}

SdgoMapLocalBoundaryReceiptV1 LocalBoundaryReceiptSnapshot() {
    AcquireSRWLockShared(&g_localBoundaryReceiptLock);
    auto value = g_localBoundaryReceipt;
    ReleaseSRWLockShared(&g_localBoundaryReceiptLock);
    value.hook_active = g_mapDamageHandler ? 1u : 0u;
    return value;
}
bool g_boundarySnapshotFault = false;
std::string g_missionScript;
lua_State* g_scriptVm = nullptr;
bool g_mapsAttempted = false;
bool g_missionsAttempted = false;
uint32_t g_published = 0;
bool g_nativeLog = false;
void* g_activeMissionMode = nullptr;
constexpr int kLuaGlobals = -10001;  // LUA_GLOBALSINDEX in Lua 5.0

bool SectionClearVisible(uint32_t& mission, uint32_t& phase) {
    mission = phase = 0;
    __try {
        uint8_t* app = g_gameApp ? g_gameApp() : nullptr;
        uint8_t* service = app ? *reinterpret_cast<uint8_t**>(app + 0x4A4) : nullptr;
        uint8_t* mode = service ? *reinterpret_cast<uint8_t**>(service + 0x0C) : nullptr;
        const uint8_t* record = mode ? *reinterpret_cast<const uint8_t**>(mode + 0xE8) : nullptr;
        if (!record) return true;
        mission = U32(record);
        for (const auto& entry : g_catalog.records) {
            if (entry.kind != kGameModeType || entry.id != mission || !(entry.flags & kSectionClearPolicy)) continue;
            const uint8_t* native = nullptr;
            if (!g_find(g_data(), &native, mission, kGameModeType, 0) || native != record) return true;
            phase = U32(mode + 0x620);
            return ShowsSectionClear(entry.flags, phase);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return true;
}

int __fastcall MissionMessageDetour(void* self, void*, const uint8_t* message) {
    // 0x4272 only starts the native section-clear presentation. Its base
    // handler returns 1 without touching state; phase/NPC/result messages
    // keep their original dispatch and ownership.
    if (message && *reinterpret_cast<const uint16_t*>(message + 4) == 0x4272) {
        uint32_t mission = 0, phase = 0;
        const bool visible = SectionClearVisible(mission, phase);
        g_api->Log(g_mod, SDGO_LOG_INFO, "native section clear: mission=%lu phase=%lu visible=%d",
                   static_cast<unsigned long>(mission), static_cast<unsigned long>(phase), visible ? 1 : 0);
        if (!visible) return 1;
    }
    return g_missionMessage(self, message);
}

struct Allocation {
    uint8_t* record = nullptr;
    NativeValue* value = nullptr;
    void* node = nullptr;
    ~Allocation() {
        if (node) g_release(node);
        if (value) g_release(value);
        if (record) g_release(record);
    }
    void Published() { node = nullptr; value = nullptr; record = nullptr; }
};

bool HasAnyKey(void* grid, uint32_t id) {
    void* node = nullptr;
    g_mapFind(grid, &node, &id);
    return node && node != *reinterpret_cast<void**>(static_cast<uint8_t*>(grid) + 4);
}

bool NativeRecord(void* grid, uint32_t id, uint32_t kind, const uint8_t** record) {
    *record = nullptr;
    return g_find(grid, record, id, static_cast<int>(kind), 0) && *record;
}

// Install every catalog record whose kind passes `select`. Records already
// present must be byte-identical (a restarted registration); any other key
// conflict aborts before the first insertion.
int Install(void* grid, bool maps) {
    if (!grid || grid != g_data()) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "map catalog grid identity mismatch");
        return -1;
    }
    std::vector<const Entry*> pending;
    int existing = 0;
    for (const auto& entry : g_catalog.records) {
        if ((entry.kind == kMapType) != maps) continue;
        if (!HasAnyKey(grid, entry.id)) {
            pending.push_back(&entry);
            continue;
        }
        const uint8_t* native = nullptr;
        if (!NativeRecord(grid, entry.id, entry.kind, &native) ||
            std::memcmp(native, entry.bytes.data(), entry.bytes.size()) != 0) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "native grid key %lu conflicts with imported kind 0x%lX; map catalog aborted",
                       static_cast<unsigned long>(entry.id), static_cast<unsigned long>(entry.kind));
            return -1;
        }
        ++existing;
    }
    // Every map list walks map rows from 100001 and stops at the first gap.
    if (maps) {
        for (const Entry* entry : pending) {
            const uint32_t previous = entry->id - 1;
            bool found = HasAnyKey(grid, previous);
            for (const Entry* other : pending) found = found || other->id == previous;
            if (!found) {
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "imported map row %lu does not follow an existing row; map catalog aborted",
                           static_cast<unsigned long>(entry->id));
                return -1;
            }
        }
    }

    std::vector<std::unique_ptr<Allocation>> prepared;
    prepared.reserve(pending.size());
    for (const Entry* entry : pending) {
        auto item = std::make_unique<Allocation>();
        item->record = static_cast<uint8_t*>(g_allocate(entry->bytes.size()));
        item->value = static_cast<NativeValue*>(g_allocate(sizeof(NativeValue)));
        if (!item->record || !item->value) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native allocation failed while preparing map catalog");
            return -1;
        }
        std::memcpy(item->record, entry->bytes.data(), entry->bytes.size());
        *item->value = NativeValue{entry->kind, item->record};
        const NativePair pair{entry->id, item->value};
        item->node = g_makeNode(grid, &pair);
        if (!item->node) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native node allocation failed while preparing map catalog");
            return -1;
        }
        prepared.push_back(std::move(item));
    }
    for (const Entry* entry : pending) {
        if (HasAnyKey(grid, entry->id)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native grid key %lu changed while preparing map catalog",
                       static_cast<unsigned long>(entry->id));
            return -1;
        }
    }

    int added = 0;
    for (size_t index = 0; index < prepared.size(); ++index) {
        Allocation& item = *prepared[index];
        const Entry& entry = *pending[index];
        uint8_t* const raw = item.record;
        void* const node = item.node;
        item.node = nullptr; // native insertion consumes the node on either result
        InsertResult result{};
        g_insertNode(grid, &result, node, 0);
        if (!result.inserted) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native insertion failed for grid key %lu",
                       static_cast<unsigned long>(entry.id));
            return -1;
        }
        item.Published();
        ++g_published;
        g_registerRecord(grid, raw, static_cast<uint32_t>(entry.bytes.size()), entry.kind);
        const uint8_t* found = nullptr;
        if (!NativeRecord(grid, entry.id, entry.kind, &found) || found != raw) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native readback failed for grid key %lu",
                       static_cast<unsigned long>(entry.id));
            return -1;
        }
        ++added;
    }
    g_api->Log(g_mod, SDGO_LOG_INFO, "native %s registered: %d added, %d already present",
               maps ? "map rows" : "mission records", added, existing);
    return added;
}

int GuardedInstall(void* grid, bool maps, DWORD* exceptionCode) {
    __try {
        return Install(grid, maps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = GetExceptionCode();
        return -1;
    }
}

void InstallOrStop(void* grid, bool maps) {
    const uint32_t before = g_published;
    DWORD code = 0;
    if (GuardedInstall(grid, maps, &code) >= 0) return;
    if (g_published == before) {
        g_api->Log(g_mod, SDGO_LOG_ERROR,
                   "map catalog registration failed before publication (%08lX); no records were added",
                   static_cast<unsigned long>(code));
        return;
    }
    g_api->Log(g_mod, SDGO_LOG_ERROR,
               "map catalog registration failed after %lu records were published (%08lX); "
               "terminating the client to prevent use of partial imported state",
               static_cast<unsigned long>(g_published - before), static_cast<unsigned long>(code));
    const DWORD status = code ? code : 0xE0C00102u;
    TerminateProcess(GetCurrentProcess(), status);
    ExitProcess(status);
}

const wchar_t* BaseName(const wchar_t* path) {
    const wchar_t* name = path ? path : L"";
    for (const wchar_t* cursor = name; *cursor; ++cursor)
        if (*cursor == L'\\' || *cursor == L'/') name = cursor + 1;
    return name;
}

void __fastcall LoadMapTable(void* self, void*, const wchar_t* file, uint32_t type) {
    g_loadMapTable(self, file, type);
    if (g_mapsAttempted || type != kMapType || _wcsicmp(BaseName(file), L"0254.gdt") != 0) return;
    g_mapsAttempted = true;
    InstallOrStop(self, true);
}

void __fastcall TablesReadyDetour(void* self, void*) {
    if (!g_missionsAttempted) {
        g_missionsAttempted = true;
        InstallOrStop(self, false);
    }
    g_tablesReady(self);
}

struct Chunk {
    const char* data;
    size_t size;
};

const char* __cdecl ReadChunk(lua_State*, void* context, size_t* size) {
    auto* chunk = static_cast<Chunk*>(context);
    if (!chunk->size) {
        *size = 0;
        return nullptr;
    }
    *size = chunk->size;
    chunk->size = 0;
    return chunk->data;
}

void RegisterMissionHelpers(lua_State* L);

// Imported mission scripts (cutscenes, phase events) are plain ASCII text Lua
// generated by the map compiler. They only define globals, so loading them
// once per VM before the first mission script call is enough. Boundary contact
// feedback also uses this native VM even when no authored mission script exists.
void EnsureMissionScripts() {
    lua_State* L = g_luaState ? g_luaState(nullptr) : nullptr;
    if (!L || L == g_scriptVm) return;
    g_scriptVm = L;
    const int top = g_luaGetTop(L);
    RegisterMissionHelpers(L);
    if (!g_missionScript.empty()) {
        Chunk chunk{g_missionScript.data(), g_missionScript.size()};
        int status = g_luaLoad(L, ReadChunk, &chunk, "=map_catalog/missions.lua");
        if (status == 0) status = g_luaPcall(L, 0, 0, 0);
        if (status != 0) {
            const char* message = g_luaToString(L, -1);
            g_api->Log(g_mod, SDGO_LOG_ERROR, "imported mission scripts failed to load: %s",
                       message ? message : "unknown Lua error");
        } else {
            g_api->Log(g_mod, SDGO_LOG_INFO, "imported mission scripts loaded (%u bytes)",
                       static_cast<unsigned>(g_missionScript.size()));
        }
    }
    g_luaSetTop(L, top);
}

void EnableNativeLog();

// One info line per imported script function (generated names start with SDGOM), plus the
// moment a cutscene Update returns the native end code, so a run shows the mission flow.
constexpr int kCutsceneDone = 456;
std::vector<std::string> g_seenScripts;

void NoteImportedScriptCall(const char* name, int result) {
    if (!name || std::strncmp(name, "SDGOM", 5) != 0 || g_seenScripts.size() >= 512) return;
    const size_t length = strnlen(name, 64);
    if (length >= 64) return;
    const std::string key(name, length);
    const bool update = length > 7 && key.compare(length - 7, 7, "_Update") == 0;
    const std::string doneKey = key + "#done";
    if (std::find(g_seenScripts.begin(), g_seenScripts.end(), key) == g_seenScripts.end()) {
        g_seenScripts.push_back(key);
        g_api->Log(g_mod, SDGO_LOG_INFO, "imported mission script call: %s", key.c_str());
    }
    if (update && result == kCutsceneDone &&
        std::find(g_seenScripts.begin(), g_seenScripts.end(), doneKey) == g_seenScripts.end()) {
        g_seenScripts.push_back(doneKey);
        g_api->Log(g_mod, SDGO_LOG_INFO, "imported cutscene finished: %s", key.c_str());
    }
}

int __cdecl MissionCallDetour(const char* name, void* mode) {
    EnableNativeLog();
    __try {
        EnsureMissionScripts();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while loading imported mission scripts",
                   static_cast<unsigned long>(GetExceptionCode()));
    }
    void* previousMode = g_activeMissionMode;
    g_activeMissionMode = mode;
    const int result = g_missionCall(name, mode);
    g_activeMissionMode = previousMode;
    __try {
        NoteImportedScriptCall(name, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

bool LoadMissionScript(const std::wstring& path) {
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path.c_str(), L"rb") != 0 || !stream) return true;  // optional
    std::string text;
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        text.append(buffer, read);
        if (text.size() > 4u * 1024u * 1024u) break;
    }
    std::fclose(stream);
    if (text.size() > 4u * 1024u * 1024u) return false;
    for (const unsigned char c : text)
        if (c >= 0x80 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) return false;
    g_missionScript = std::move(text);
    return true;
}

bool LoadBoundaries(const std::wstring& path, std::string& error) {
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path.c_str(), L"rb") != 0 || !stream) return true;  // optional
    std::vector<uint8_t> raw;
    uint8_t buffer[8192];
    size_t read = 0;
    const size_t maximum = sdgo::map_boundary::kHeaderBytes +
                           static_cast<size_t>(sdgo::map_boundary::kRowBytes) * sdgo::map_boundary::kMaxRows;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        raw.insert(raw.end(), buffer, buffer + read);
        if (raw.size() > maximum) break;
    }
    const bool fileError = std::ferror(stream) != 0;
    std::fclose(stream);
    if (fileError) {
        error = "boundary declaration could not be read";
        return false;
    }
    if (raw.size() > maximum) {
        error = "boundary declaration is too large";
        return false;
    }
    return sdgo::map_boundary::Parse(raw.data(), raw.size(), g_boundaries, error);
}

bool LoadDamageGates(const std::wstring& path, std::string& error) {
    FILE* stream = nullptr;
    const errno_t openError = _wfopen_s(&stream, path.c_str(), L"rb");
    if (openError != 0 || !stream) {
        if (openError == ENOENT) return true;  // optional
        error = "damage gate declaration could not be opened";
        return false;
    }
    std::vector<uint8_t> raw;
    uint8_t buffer[8192];
    size_t read = 0;
    const size_t maximum = sdgo::mission_damage_gate::kHeaderBytes +
        static_cast<size_t>(sdgo::mission_damage_gate::kRowBytes) *
            sdgo::mission_damage_gate::kMaxRows;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        raw.insert(raw.end(), buffer, buffer + read);
        if (raw.size() > maximum) break;
    }
    const bool fileError = std::ferror(stream) != 0;
    std::fclose(stream);
    if (fileError) {
        error = "damage gate declaration could not be read";
        return false;
    }
    if (raw.size() > maximum) {
        error = "damage gate declaration is too large";
        return false;
    }
    return sdgo::mission_damage_gate::Parse(raw.data(), raw.size(), g_damageGates, error);
}

bool CatalogContains(uint32_t kind, uint32_t id) {
    for (const auto& entry : g_catalog.records)
        if (entry.kind == kind && entry.id == id) return true;
    return false;
}

bool ValidateBoundaries(std::string& error) {
    for (const auto& entry : g_boundaries) {
        if (!CatalogContains(kGameModeType, entry.missionId)) {
            error = "boundary declaration references a mission outside catalog.bin";
            return false;
        }
        if ((entry.flags & sdgo::map_boundary::kWarningEnabled) &&
            !CatalogContains(kSpeechType, entry.warningSpeechId)) {
            error = "boundary declaration references speech outside catalog.bin";
            return false;
        }
    }
    return true;
}
bool ValidateDamageGates(std::string& error) {
    for (const auto& entry : g_damageGates) {
        if (!CatalogContains(kGameModeType, entry.missionId)) {
            error = "damage gate declaration references a mission outside catalog.bin";
            return false;
        }
    }
    return true;
}

// Character speech in imported missions. PutOperatorSpeech posts to the "OriginalModeOperator"
// UI, which the battle HUD setup creates only for room modes 8 and 10; listed missions run in
// mode 5. At the HUD's mode check (eax = room mode, ecx = room data) an imported mission's room
// reports mode 8 for that one comparison, so the client builds its own speech UI. Every other
// room keeps its native mode.
std::vector<uint32_t> g_importedModes;
std::vector<uint32_t> g_cutinNpcs;
void* g_operatorModeOriginal = nullptr;
void* g_npcOperatorModeOriginal = nullptr;
constexpr size_t kRoomGameModeId = 0x9C;  // u32 GameModeID in the room data (the 0x66 mode word's object)
constexpr uint32_t kListedMissionMode = 5;
constexpr uint32_t kSpeechMissionMode = 8;

struct BoundaryPosition { float x, y, z; };
constexpr uint16_t kBoundaryRequestOpcode = 0x0492;
constexpr uint16_t kBoundaryNoticeOpcode = 0x0793;
constexpr size_t kModeRoom = 0x08;
constexpr size_t kRoomHumansBegin = 0x1C;
constexpr size_t kRoomHumansEnd = 0x20;
constexpr size_t kRoomLocalPlayer = 0x68;
constexpr size_t kHumanRecordBytes = 0x344;
constexpr size_t kModeRoomMaster = 0x61C;
constexpr uint32_t kBoundaryContactFxCooldownMs = 500;
constexpr uint32_t kBoundaryReceiptLifetimeMs = 10000;
constexpr uint32_t kDamageGateShieldFxCooldownMs = 1000;
constexpr uint32_t kDamageGateWarningCooldownMs = 8000;

// Returns the local human's one-based position in the authoritative room record
// vector. Imported Lua uses that position only to choose the author's warp point;
// PlayerWarp itself receives the local unit's actual runtime object id.
int __cdecl LuaLocalHumanSlot(lua_State* L) {
    __try {
        auto* mode = static_cast<uint8_t*>(g_activeMissionMode);
        uint8_t* room = mode ? *reinterpret_cast<uint8_t**>(mode + kModeRoom) : nullptr;
        const uint32_t localPilot = room ? *reinterpret_cast<uint32_t*>(room + kRoomLocalPlayer) : 0;
        uint8_t* first = room ? *reinterpret_cast<uint8_t**>(room + kRoomHumansBegin) : nullptr;
        uint8_t* last = room ? *reinterpret_cast<uint8_t**>(room + kRoomHumansEnd) : nullptr;
        if (!localPilot || !first || !last || last < first ||
            (last - first) % kHumanRecordBytes != 0 ||
            static_cast<size_t>(last - first) / kHumanRecordBytes > 12) return 0;
        uint32_t slot = 1;
        for (uint8_t* record = first; record != last; record += kHumanRecordBytes, ++slot) {
            if (*reinterpret_cast<uint32_t*>(record) == localPilot) {
                g_luaPushNumber(L, static_cast<double>(slot));
                return 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

#include "party_sources.inl"

void RegisterMissionHelpers(lua_State* L) {
    g_luaPushString(L, "SDGO_MapCatalogClockMs");
    g_luaPushCClosure(L, &LuaMissionClockMs, 0);
    g_luaSetTable(L, kLuaGlobals);
    g_luaPushString(L, "SDGO_MapCatalogLocalHumanSlot");
    g_luaPushCClosure(L, &LuaLocalHumanSlot, 0);
    g_luaSetTable(L, kLuaGlobals);
    g_luaPushString(L, "SDGO_MapCatalogPartySources");
    g_luaPushCClosure(L, &LuaPartySources, 0);
    g_luaSetTable(L, kLuaGlobals);
}

bool ActiveMissionContext(uint8_t* mode, uint32_t& mission, uint32_t& phase) {
    mission = phase = 0;
    if (!mode || !g_gameApp || !g_find || !g_data) return false;
    uint8_t* app = g_gameApp();
    if (!app || *reinterpret_cast<uint32_t*>(app + 0x4A0) != 22) return false;
    uint8_t* service = *reinterpret_cast<uint8_t**>(app + 0x4A4);
    if (!service || *reinterpret_cast<uint8_t**>(service + 0x0C) != mode) return false;
    const uint8_t* record = *reinterpret_cast<const uint8_t**>(mode + 0xE8);
    if (!record) return false;
    mission = U32(record);
    const uint8_t* native = nullptr;
    if (!mission || !g_find(g_data(), &native, mission, kGameModeType, 0) || native != record) {
        mission = 0;
        return false;
    }
    phase = U32(mode + 0x620);
    return true;
}

const sdgo::map_boundary::Entry* ActiveBoundary(uint8_t* mode, uint32_t& mission, uint32_t& phase) {
    if (!ActiveMissionContext(mode, mission, phase) || g_boundaries.empty()) return nullptr;
    return sdgo::map_boundary::Find(g_boundaries, mission, phase);
}

const sdgo::mission_damage_gate::Entry* FindDamageGate(uint32_t mission, uint32_t phase,
                                                       uint32_t targetNpcId) {
    const sdgo::mission_damage_gate::Entry key{mission, phase, targetNpcId, 0, 0};
    const auto found = std::lower_bound(g_damageGates.begin(), g_damageGates.end(), key,
        [](const auto& left, const auto& right) {
            return sdgo::mission_damage_gate::Key(left) < sdgo::mission_damage_gate::Key(right);
        });
    return found != g_damageGates.end() && found->missionId == mission &&
        found->phase == phase && found->targetNpcId == targetNpcId ? &*found : nullptr;
}

bool BoundaryDamageActive(uint32_t mission, uint32_t phase) {
    const auto* entry = g_boundaries.empty() ? nullptr :
        sdgo::map_boundary::Find(g_boundaries, mission, phase);
    return entry && (entry->flags & sdgo::map_boundary::kDamageEnabled) != 0;
}

bool BoundarySnapshot(uint8_t* document, std::vector<uint32_t>& ids) {
    using sdgo::map_catalog::boundary_runtime::CollectCounted;
    using sdgo::map_catalog::boundary_runtime::Node;
    ids.clear();
    if (!document) return false;
    Node* players = *reinterpret_cast<Node**>(document + 0x48);
    Node* npcs = *reinterpret_cast<Node**>(document + 0x70);
    const uint32_t playerCount = *reinterpret_cast<uint32_t*>(document + 0x4C);
    const uint32_t npcCount = *reinterpret_cast<uint32_t*>(document + 0x74);
    if (!CollectCounted(players, playerCount, ids) || !CollectCounted(npcs, npcCount, ids)) {
        ids.clear();
        return false;
    }
    sdgo::map_catalog::boundary_runtime::SortUnique(ids);
    return true;
}

uint8_t* CanonicalObject(uint8_t* document, uint32_t id) {
    uint8_t* object = document && g_findObject ? g_findObject(document, id) : nullptr;
    return object && *reinterpret_cast<uint32_t*>(object + 4) == id ? object : nullptr;
}
bool NpcDataId(const uint8_t* unit, uint32_t& npcId) {
    npcId = 0;
    if (!unit || !g_find || !g_data ||
        (*reinterpret_cast<const uint32_t*>(unit + 0x2B68) & sdgo::map_boundary_contact::kNpc) == 0)
        return false;
    const auto* definition = *reinterpret_cast<const uint8_t* const*>(unit + 0x320C);
    if (!definition) return false;
    npcId = U32(definition);
    const uint8_t* native = nullptr;
    return npcId && g_find(g_data(), &native, npcId, kNpcTemplateType, 0) && native == definition;
}

#include "retreat_client.inl"

void QueueDamageGateNotice(uint32_t recipient, uint32_t target) {
    if (!recipient || !target) return;
    AcquireSRWLockExclusive(&g_damageGateNoticeLock);
    for (size_t i = 0; i < g_pendingDamageGateNoticeCount; ++i) {
        if (g_pendingDamageGateNotices[i].recipient == recipient &&
            g_pendingDamageGateNotices[i].target == target) {
            ReleaseSRWLockExclusive(&g_damageGateNoticeLock);
            return;
        }
    }
    if (g_pendingDamageGateNoticeCount == g_pendingDamageGateNotices.size()) {
        for (size_t i = 1; i < g_pendingDamageGateNoticeCount; ++i)
            g_pendingDamageGateNotices[i - 1] = g_pendingDamageGateNotices[i];
        --g_pendingDamageGateNoticeCount;
    }
    g_pendingDamageGateNotices[g_pendingDamageGateNoticeCount++] = {recipient, target};
    ReleaseSRWLockExclusive(&g_damageGateNoticeLock);
}

size_t DrainDamageGateNotices(
    std::array<PendingDamageGateNotice, kPendingDamageGateNotices>& notices) {
    AcquireSRWLockExclusive(&g_damageGateNoticeLock);
    const size_t count = g_pendingDamageGateNoticeCount;
    std::copy_n(g_pendingDamageGateNotices.begin(), count, notices.begin());
    g_pendingDamageGateNoticeCount = 0;
    ReleaseSRWLockExclusive(&g_damageGateNoticeLock);
    return count;
}

void ClearDamageGateNotices() {
    AcquireSRWLockExclusive(&g_damageGateNoticeLock);
    g_pendingDamageGateNoticeCount = 0;
    ReleaseSRWLockExclusive(&g_damageGateNoticeLock);
}

bool RecordBoundaryRequest(uint32_t target, int32_t before, uint64_t now) {
    AcquireSRWLockExclusive(&g_boundaryReceiptLock);
    const bool recorded = sdgo::map_catalog::boundary_runtime::RecordBoundaryRequest(
        g_boundaryReceipts, target, before, now);
    ReleaseSRWLockExclusive(&g_boundaryReceiptLock);
    return recorded;
}

void CancelBoundaryRequest(uint32_t target, int32_t before, uint64_t now) {
    AcquireSRWLockExclusive(&g_boundaryReceiptLock);
    sdgo::map_catalog::boundary_runtime::CancelBoundaryRequest(
        g_boundaryReceipts, target, before, now);
    ReleaseSRWLockExclusive(&g_boundaryReceiptLock);
}

bool ConsumeBoundaryReceipt(uint32_t target, int32_t current, int32_t maximum,
                            uint32_t notice, uint64_t now, int32_t& requestedBefore) {
    AcquireSRWLockExclusive(&g_boundaryReceiptLock);
    const bool accepted = sdgo::map_catalog::boundary_runtime::ConsumeBoundaryReceipt(
        g_boundaryReceipts, target, current, maximum, notice, now,
        kBoundaryReceiptLifetimeMs, requestedBefore);
    ReleaseSRWLockExclusive(&g_boundaryReceiptLock);
    return accepted;
}

void ClearBoundaryReceipts() {
    AcquireSRWLockExclusive(&g_boundaryReceiptLock);
    g_boundaryReceipts.clear();
    ReleaseSRWLockExclusive(&g_boundaryReceiptLock);
}

void ResetDamageGateState(bool clearWarning) {
    g_damageGateStates.clear();
    g_damageGateShieldCooldowns.clear();
    g_damageGateMission = 0;
    g_damageGatePhase = 0;
    g_damageGateSnapshotFault = false;
    g_damageGateFxFault = false;
    ClearDamageGateNotices();
    if (clearWarning) g_damageGateWarningCooldowns.clear();
}

DamageGateVisualState& DamageGateState(const sdgo::mission_damage_gate::Entry& entry) {
    const auto found = std::find_if(g_damageGateStates.begin(), g_damageGateStates.end(),
        [&entry](const DamageGateVisualState& state) {
            return sdgo::mission_damage_gate::Key(state.entry) ==
                sdgo::mission_damage_gate::Key(entry);
        });
    if (found != g_damageGateStates.end()) return *found;
    g_damageGateStates.push_back({entry, {}, {}, false});
    return g_damageGateStates.back();
}

void ClearBoundaryActors(uint8_t* document) {
    if (document && g_setTerrainRectOnOff) {
        for (const auto& actor : g_boundaryActors) {
            uint8_t* object = CanonicalObject(document, actor.id);
            if (object == actor.object) g_setTerrainRectOnOff(object, false);
        }
    }
    g_boundaryActors.clear();
}

void ResetBoundaryState(uint8_t* document, bool clearWarning) {
    ClearBoundaryActors(document);
    g_boundaryMission = 0;
    g_boundaryPhase = 0;
    if (clearWarning) {
        g_lastBoundaryWarning = 0;
        g_lastBoundaryWarningSpeech = 0;
        g_lastBoundaryContactFx = 0;
        g_boundaryContactFxFault = false;
        InterlockedExchange(&g_pendingBoundaryContactFx, 0);
    }
    g_boundaryDamageCooldowns.clear();
    ClearBoundaryReceipts();
}

bool BoundaryMatches(const uint8_t* object, const sdgo::map_boundary::Entry& entry) {
    const bool enabled = object[0x3158] != 0;
    if ((entry.flags & sdgo::map_boundary::kAreaEnabled) == 0) return !enabled;
    return enabled && *reinterpret_cast<const float*>(object + 0x3148) == entry.left &&
           *reinterpret_cast<const float*>(object + 0x314C) == entry.right &&
           *reinterpret_cast<const float*>(object + 0x3150) == entry.top &&
           *reinterpret_cast<const float*>(object + 0x3154) == entry.bottom;
}

void ApplyBoundaryActors(uint8_t* document, const sdgo::map_boundary::Entry& entry) {
    std::vector<uint32_t> ids;
    if (!BoundarySnapshot(document, ids)) {
        if (!g_boundarySnapshotFault) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "boundary actor snapshot rejected because a native list changed");
            g_boundarySnapshotFault = true;
        }
        return;
    }
    g_boundarySnapshotFault = false;
    std::vector<sdgo::map_catalog::boundary_runtime::ActorStamp> current;
    current.reserve(ids.size());
    for (uint32_t id : ids) {
        uint8_t* object = CanonicalObject(document, id);
        if (!object) continue;
        current.push_back({id, object});
        // Respawn and form changes may reuse both object id and allocation while
        // the native constructor resets its terrain rectangle.  Trust live fields.
        if (BoundaryMatches(object, entry)) continue;
        if (entry.flags & sdgo::map_boundary::kAreaEnabled) {
            g_setTerrainRect(object, entry.left, entry.top, entry.right, entry.bottom);
            g_setTerrainRectOnOff(object, true);
        } else {
            g_setTerrainRectOnOff(object, false);
        }
    }
    sdgo::map_catalog::boundary_runtime::SortStamps(current);
    for (const auto& previous : g_boundaryActors) {
        if (sdgo::map_catalog::boundary_runtime::FindStamp(current, previous.id)) continue;
        uint8_t* object = CanonicalObject(document, previous.id);
        if (object == previous.object) g_setTerrainRectOnOff(object, false);
    }
    g_boundaryActors = std::move(current);
}

uint8_t* LocalBoundaryUnit(uint8_t* document) {
    uint8_t* local = document ? *reinterpret_cast<uint8_t**>(document + 0x88) : nullptr;
    if (!local) return nullptr;
    const uint32_t id = *reinterpret_cast<uint32_t*>(local + 4);
    return id && CanonicalObject(document, id) == local ? local : nullptr;
}

bool BoundaryCombatReleased(uint8_t* mode) {
    uint8_t* app = g_gameApp ? g_gameApp() : nullptr;
    uint8_t* service = app ? *reinterpret_cast<uint8_t**>(app + 0x4A4) : nullptr;
    if (!app || !service || !mode) return false;
    // Native CScriptControlMode::DemoPlay/End write mode+0x104. The local
    // unit's +0x31E9 is input control and may be false while its pilot waits
    // to respawn; it cannot decide whether NPC terrain remains active.
    return sdgo::map_catalog::boundary_runtime::MissionCombatReleased(
        *reinterpret_cast<uint32_t*>(app + 0x4A0),
        *reinterpret_cast<uint8_t**>(service + 0x0C), mode, mode[0x104] != 0);
}

uint32_t BoundaryContact(uint8_t* unit, const sdgo::map_boundary::Entry& entry) {
    uint8_t* node = unit ? *reinterpret_cast<uint8_t**>(unit + 8) : nullptr;
    if (!node) return 0;
    const auto position = *reinterpret_cast<const BoundaryPosition*>(node + 0x88);
    return sdgo::map_boundary::ContactEdges(entry, position.x, position.z);
}

struct BoundaryContactFeedback {
    bool nativeHurt = false;
    bool terrainFx = false;
};

BoundaryContactFeedback BoundaryContactFx(uint8_t* unit, const sdgo::map_boundary::Entry& entry,
                                          uint32_t pilot, uint64_t now) {
    using sdgo::map_catalog::boundary_runtime::ContactFeedbackCoolingDown;
    BoundaryContactFeedback result{};
    if (!unit || ContactFeedbackCoolingDown(g_lastBoundaryContactFx, now,
                                             kBoundaryContactFxCooldownMs)) return result;
    uint8_t* node = *reinterpret_cast<uint8_t**>(unit + 8);
    if (!node) return result;
    const auto position = *reinterpret_cast<const BoundaryPosition*>(node + 0x88);
    sdgo::map_catalog::shield_contact::Plan plan{};
    if (!sdgo::map_catalog::shield_contact::MakePlan(
            entry, {position.x, position.y, position.z}, plan)) return result;

    // The authoritative receipt already committed HP. Queue only the native
    // ordinary-hurt state so Entry/Update/Exit provide the shipped mech body
    // reaction without replaying the damaging 0x0C message.
    if (g_queueState) {
        __try {
            result.nativeHurt = sdgo::map_catalog::shield_contact::QueueNativeHurt(
                unit, plan.directionX, plan.directionZ,
                [](uint8_t* fsm, uint8_t* state) noexcept {
                    return g_queueState(fsm, state);
                });
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result.nativeHurt = false;
        }
    }

    // Terrain context is independent from the mech FSM. Preserve the stock
    // wall effects, but do not add body particles or guess an impact sound.
    if (g_luaLoad && g_luaPcall) {
        char script[768]{};
        const _locale_t locale = _create_locale(LC_NUMERIC, "C");
        if (locale) {
            const int length = _sprintf_s_l(
                script, sizeof(script),
                sdgo::map_catalog::shield_contact::kLuaFormat,
                locale, plan.wall.x, plan.wall.y, plan.wall.z,
                plan.directionX, plan.directionZ,
                plan.wall.x, plan.wall.y, plan.wall.z,
                plan.directionX, plan.directionZ);
            _free_locale(locale);
            if (length > 0 && static_cast<size_t>(length) < sizeof(script)) {
                EnsureMissionScripts();
                lua_State* L = g_scriptVm;
                if (L) {
                    const int top = g_luaGetTop(L);
                    Chunk chunk{script, static_cast<size_t>(length)};
                    int status = g_luaLoad(L, ReadChunk, &chunk,
                                           "=map_catalog/boundary_contact_fx.lua");
                    if (status == 0) status = g_luaPcall(L, 0, 0, 0);
                    if (status != 0) {
                        if (!g_boundaryContactFxFault) {
                            const char* message = g_luaToString(L, -1);
                            g_api->Log(g_mod, SDGO_LOG_ERROR,
                                       "boundary terrain effect failed: %s",
                                       message ? message : "unknown Lua error");
                            g_boundaryContactFxFault = true;
                        }
                    } else {
                        result.terrainFx = true;
                    }
                    g_luaSetTop(L, top);
                }
            }
        }
    }

    if (result.nativeHurt || result.terrainFx) g_lastBoundaryContactFx = now;
    g_api->Log(g_mod, result.nativeHurt ? SDGO_LOG_INFO : SDGO_LOG_WARN,
               "boundary contact feedback queued: pilot=%lu native_hurt=%u terrain_fx=%u "
               "direction=(%.1f,%.1f)",
               static_cast<unsigned long>(pilot), result.nativeHurt ? 1u : 0u,
               result.terrainFx ? 1u : 0u, plan.directionX, plan.directionZ);
    return result;
}
void BoundaryWarning(void* mode, const sdgo::map_boundary::Entry& entry, uint32_t edges, uint64_t now) {
    // When a damage edge is declared it is the authored shield face; other area
    // edges remain ordinary movement limits and must not announce shield contact.
    const uint32_t warningEdges = (entry.flags & sdgo::map_boundary::kDamageEnabled)
        ? (edges & entry.damageEdgeMask) : edges;
    if (!warningEdges || (entry.flags & sdgo::map_boundary::kWarningEnabled) == 0 || !g_missionCall ||
        (g_lastBoundaryWarningSpeech == entry.warningSpeechId && g_lastBoundaryWarning &&
         now - g_lastBoundaryWarning < 12000)) return;
    char name[64]{};
    sprintf_s(name, "SDGOMI_BW_%lu_%lu", static_cast<unsigned long>(entry.missionId),
              static_cast<unsigned long>(entry.phase));
    EnsureMissionScripts();
    g_missionCall(name, mode);
    g_lastBoundaryWarning = now;
    g_lastBoundaryWarningSpeech = entry.warningSpeechId;
}
bool DamageGateShieldFx(uint8_t* unit, uint32_t pilot, bool impact) {
    if (!unit || !g_luaLoad || !g_luaPcall || g_damageGateFxFault) return false;
    uint8_t* node = *reinterpret_cast<uint8_t**>(unit + 8);
    if (!node) return false;
    const auto position = *reinterpret_cast<const BoundaryPosition*>(node + 0x88);
    const float y = position.y + 100.0f;
    if (!std::isfinite(position.x) || !std::isfinite(y) || !std::isfinite(position.z))
        return false;

    char script[640]{};
    const _locale_t locale = _create_locale(LC_NUMERIC, "C");
    if (!locale) return false;
    const char* format = impact
        ? "CreateEffectA('0506.zfx',vector3(%.3f,%.3f,%.3f),vector3(2.5,2.5,2.5),"
          "vector3(0,0,1),vector3(0,1,0))\n"
          "CreateEffectA('0087.zfx',vector3(%.3f,%.3f,%.3f),vector3(4,4,4),"
          "vector3(0,0,1),vector3(0,1,0))"
        : "CreateEffectA('0087.zfx',vector3(%.3f,%.3f,%.3f),vector3(3,3,3),"
          "vector3(0,0,1),vector3(0,1,0))";
    int length = 0;
    if (impact) {
        length = _sprintf_s_l(script, sizeof(script), format, locale,
            position.x, y, position.z, position.x, y, position.z);
    } else {
        length = _sprintf_s_l(script, sizeof(script), format, locale,
            position.x, y, position.z);
    }
    _free_locale(locale);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(script)) return false;

    EnsureMissionScripts();
    lua_State* L = g_scriptVm;
    if (!L) return false;
    const int top = g_luaGetTop(L);
    Chunk chunk{script, static_cast<size_t>(length)};
    int status = g_luaLoad(L, ReadChunk, &chunk, "=map_catalog/damage_gate_fx.lua");
    if (status == 0) status = g_luaPcall(L, 0, 0, 0);
    if (status != 0) {
        const char* message = g_luaToString(L, -1);
        g_api->Log(g_mod, SDGO_LOG_ERROR, "damage gate effect failed: %s",
                   message ? message : "unknown Lua error");
        g_damageGateFxFault = true;
        g_luaSetTop(L, top);
        return false;
    }
    g_luaSetTop(L, top);
    if (impact) {
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "damage gate blocked-hit effect: pilot=%lu x=%.1f y=%.1f z=%.1f",
                   static_cast<unsigned long>(pilot), position.x, y, position.z);
    }
    return true;
}

void DamageGateWarning(void* mode, const sdgo::mission_damage_gate::Entry& entry,
                       uint32_t recipient, uint32_t localPilot, uint64_t now) {
    using sdgo::map_catalog::boundary_runtime::CommitCooldown;
    using sdgo::map_catalog::boundary_runtime::CoolingDown;
    if (!g_missionCall ||
        !sdgo::map_catalog::damage_gate_client::IsLocalWarning(recipient, localPilot) ||
        CoolingDown(g_damageGateWarningCooldowns, entry.targetNpcId, now,
                    kDamageGateWarningCooldownMs)) return;
    char name[96]{};
    sprintf_s(name, "SDGOMI_DG_%lu_%lu_%lu",
              static_cast<unsigned long>(entry.missionId),
              static_cast<unsigned long>(entry.phase),
              static_cast<unsigned long>(entry.targetNpcId));
    EnsureMissionScripts();
    g_missionCall(name, mode);
    CommitCooldown(g_damageGateWarningCooldowns, entry.targetNpcId, now);
}

bool LocalIsRoomMaster(uint8_t* mode, const uint8_t* local) {
    if (!mode || !local) return false;
    uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
    if (!room) return false;
    return sdgo::map_catalog::boundary_runtime::LocalRoomMaster(
        mode[kModeRoomMaster] != 0,
        *reinterpret_cast<uint32_t*>(room + kRoomLocalPlayer),
        *reinterpret_cast<const uint32_t*>(local + 0x2B68));
}

void SendBoundaryDamageRequest(uint32_t source, uint32_t target, int32_t before,
                               const sdgo::map_boundary::Entry& entry, uint32_t edges, uint64_t now) {
    using namespace sdgo::map_boundary_contact;
    using sdgo::map_catalog::boundary_runtime::CommitCooldown;
    using sdgo::map_catalog::boundary_runtime::CoolingDown;
    if (!(edges & entry.damageEdgeMask) || !g_network || !g_networkSend ||
        CoolingDown(g_boundaryDamageCooldowns, target, now, entry.damageCooldownMs)) return;
    const uint32_t marked = EncodeRequest(source, target);
    if (!marked) return;
    uint8_t packet[14]{};
    std::memcpy(packet, &kBoundaryRequestOpcode, sizeof(kBoundaryRequestOpcode));
    std::memcpy(packet + 6, &marked, sizeof(marked));
    std::memcpy(packet + 10, &target, sizeof(target));
    uint8_t* network = g_network();
    void* socket = network ? *reinterpret_cast<void**>(network + 0x14) : nullptr;
    if (!network || !socket) return;
    // Reserve before the transport call so a receive callback on another thread
    // cannot arrive before its request baseline exists. A rejected send cancels
    // only this exact reservation; an already consumed receipt stays consumed.
    if (!RecordBoundaryRequest(target, before, now)) return;
    ++*reinterpret_cast<uint32_t*>(network + 0x58);
    *reinterpret_cast<uint32_t*>(network + 0x5C) += sizeof(packet);
    if (g_networkSend(network, socket, packet, sizeof(packet), 0)) {
        CommitCooldown(g_boundaryDamageCooldowns, target, now);
    } else {
        CancelBoundaryRequest(target, before, now);
    }
}

void BoundaryDamageRequests(uint8_t* mode, uint8_t* document, uint8_t* local,
                            const sdgo::map_boundary::Entry& entry, uint32_t localEdges, uint64_t now) {
    using namespace sdgo::map_boundary_contact;
    if ((entry.flags & sdgo::map_boundary::kDamageEnabled) == 0) return;
    const uint32_t source = *reinterpret_cast<uint32_t*>(local + 0x2B68);
    SendBoundaryDamageRequest(source, source,
                              *reinterpret_cast<int32_t*>(local + 0x38),
                              entry, localEdges, now);
    if (!LocalIsRoomMaster(mode, local)) return;
    for (const auto& actor : g_boundaryActors) {
        uint8_t* unit = CanonicalObject(document, actor.id);
        if (!unit || unit != actor.object || unit == local) continue;
        const uint32_t target = *reinterpret_cast<uint32_t*>(unit + 0x2B68);
        if ((target & kNpc) == 0) continue;
        SendBoundaryDamageRequest(source, target,
                                  *reinterpret_cast<int32_t*>(unit + 0x38),
                                  entry, BoundaryContact(unit, entry), now);
    }
}

void BoundaryTick(uint8_t* mode) {
    uint8_t* document = g_unitManager ? *g_unitManager : nullptr;
    uint32_t mission = 0, phase = 0;
    const auto* entry = ActiveBoundary(mode, mission, phase);
    // A receipt belongs only to the first completed mission update after its
    // authoritative HP commit. Discard it on phase/demo/mission early exits so
    // it cannot become a delayed reaction in later combat.
    const auto pending = static_cast<uint32_t>(InterlockedExchange(&g_pendingBoundaryContactFx, 0));
    if (!document || !mission) {
        if (!g_boundaryActors.empty() || g_boundaryMission) ResetBoundaryState(document, true);
        return;
    }
    const auto transition = sdgo::map_catalog::boundary_runtime::Transition(
        g_boundaryMission, g_boundaryPhase, mission, phase);
    if (transition.missionExitOrSwitch) ResetBoundaryState(document, true);
    else if (transition.phaseChanged) {
        ClearBoundaryActors(document);
        g_boundaryDamageCooldowns.clear();
        ClearBoundaryReceipts();
    }
    g_boundaryMission = mission;
    g_boundaryPhase = phase;
    if (!entry) {
        if (!g_boundaryActors.empty()) ClearBoundaryActors(document);
        ClearBoundaryReceipts();
        return;
    }
    // Native DemoPlay releases mission actors from combat control. Do not clamp
    // cutscene-only NPCs. Keep mission/phase identity and the 12-second warning
    // cooldown so a door scene cannot retrigger the same speech every frame.
    if (!BoundaryCombatReleased(mode)) {
        if (!g_boundaryActors.empty()) ClearBoundaryActors(document);
        g_boundaryDamageCooldowns.clear();
        ClearBoundaryReceipts();
        return;
    }
    ApplyBoundaryActors(document, *entry);
    uint8_t* local = LocalBoundaryUnit(document);
    if (!local) return;
    const uint32_t edges = BoundaryContact(local, *entry);
    const uint64_t now = GetTickCount64();
    BoundaryWarning(mode, *entry, edges, now);
    BoundaryDamageRequests(mode, document, local, *entry, edges, now);
    // Scene effects and the shared Lua VM stay on the completed mission-update
    // thread. The packet callback only queues this coalesced visual receipt.
    if (pending && pending == *reinterpret_cast<uint32_t*>(local + 0x2B68) &&
        *reinterpret_cast<int32_t*>(local + 0x38) > 0 &&
        (entry->flags & sdgo::map_boundary::kDamageEnabled)) {
        const auto feedback = BoundaryContactFx(local, *entry, pending, now);
        g_api->Log(g_mod, feedback.nativeHurt ? SDGO_LOG_INFO : SDGO_LOG_WARN,
                   "boundary contact feedback: pilot=%lu native_hurt=%u terrain_fx=%u",
                   static_cast<unsigned long>(pending), feedback.nativeHurt ? 1u : 0u,
                   feedback.terrainFx ? 1u : 0u);
    }
}

struct DamageGateNpcUnit {
    uint8_t* unit = nullptr;
    uint32_t pilot = 0;
    uint32_t npcId = 0;
    int32_t health = 0;
};

void DamageGateTick(uint8_t* mode) {
    std::array<PendingDamageGateNotice, kPendingDamageGateNotices> notices{};
    size_t noticeCount = DrainDamageGateNotices(notices);
    uint8_t* document = g_unitManager ? *g_unitManager : nullptr;
    uint32_t mission = 0, phase = 0;
    if (!document || !ActiveMissionContext(mode, mission, phase)) {
        if (g_damageGateMission || !g_damageGateStates.empty()) ResetDamageGateState(true);
        return;
    }
    if (g_damageGateMission != mission || g_damageGatePhase != phase) {
        ResetDamageGateState(true);
        g_damageGateMission = mission;
        g_damageGatePhase = phase;
        noticeCount = 0;
    }
    if (BoundaryDamageActive(mission, phase) || !BoundaryCombatReleased(mode)) return;

    const sdgo::mission_damage_gate::Entry key{mission, phase, 0, 0, 0};
    auto first = std::lower_bound(g_damageGates.begin(), g_damageGates.end(), key,
        [](const auto& left, const auto& right) {
            return sdgo::mission_damage_gate::Key(left) < sdgo::mission_damage_gate::Key(right);
        });
    auto last = first;
    while (last != g_damageGates.end() && last->missionId == mission && last->phase == phase) ++last;
    if (first == last) return;

    std::vector<uint32_t> ids;
    if (!BoundarySnapshot(document, ids)) {
        if (!g_damageGateSnapshotFault) {
            g_api->Log(g_mod, SDGO_LOG_WARN,
                       "damage gate actor snapshot rejected because a native list changed");
            g_damageGateSnapshotFault = true;
        }
        return;
    }
    g_damageGateSnapshotFault = false;
    std::vector<DamageGateNpcUnit> units;
    units.reserve(ids.size());
    for (uint32_t id : ids) {
        uint8_t* unit = CanonicalObject(document, id);
        uint32_t npcId = 0;
        if (!unit || !NpcDataId(unit, npcId)) continue;
        const uint32_t pilot = *reinterpret_cast<uint32_t*>(unit + 0x2B68);
        if (!pilot) continue;
        units.push_back({unit, pilot, npcId,
                         *reinterpret_cast<int32_t*>(unit + 0x38)});
    }

    for (auto entry = first; entry != last; ++entry) {
        auto& state = DamageGateState(*entry);
        bool guardAliveNow = false;
        for (const auto& unit : units) {
            if (unit.npcId != entry->guardNpcId) continue;
            if (unit.health > 0) guardAliveNow = true;
            if (sdgo::map_catalog::damage_gate_client::ObserveGuard(
                    state.guards, unit.pilot, unit.health)) {
                g_api->Log(g_mod, SDGO_LOG_INFO,
                           "damage gate observed guard defeat: mission=%lu phase=%lu guard=%lu pilot=%lu defeats=%lu/%lu",
                           static_cast<unsigned long>(mission), static_cast<unsigned long>(phase),
                           static_cast<unsigned long>(entry->guardNpcId),
                           static_cast<unsigned long>(unit.pilot),
                           static_cast<unsigned long>(
                               sdgo::map_catalog::damage_gate_client::DefeatCount(state.guards)),
                           static_cast<unsigned long>(entry->requiredDefeats));
            }
        }
        const bool guardWasObserved = std::any_of(
            state.guards.begin(), state.guards.end(),
            [](const auto& guard) { return guard.aliveObserved; });
        if (state.serverDamageObserved) continue;
        for (const auto& unit : units) {
            if (unit.npcId != entry->targetNpcId) continue;
            if (!sdgo::map_catalog::damage_gate_client::ObserveAcceptedTargetDamage(
                    state.targets, unit.pilot, unit.health,
                    guardWasObserved, guardAliveNow)) continue;
            state.serverDamageObserved = true;
            g_api->Log(g_mod, SDGO_LOG_INFO,
                       "damage gate observed authoritative target damage after guard departure: mission=%lu phase=%lu target=%lu pilot=%lu",
                       static_cast<unsigned long>(mission), static_cast<unsigned long>(phase),
                       static_cast<unsigned long>(entry->targetNpcId),
                       static_cast<unsigned long>(unit.pilot));
            break;
        }
    }

    const uint64_t now = GetTickCount64();
    uint8_t* local = LocalBoundaryUnit(document);
    const uint32_t localPilot = local ? *reinterpret_cast<uint32_t*>(local + 0x2B68) : 0;
    for (size_t i = 0; i < noticeCount; ++i) {
        const uint32_t objectId = g_objectForPilot(document, notices[i].target);
        uint8_t* target = objectId ? CanonicalObject(document, objectId) : nullptr;
        uint32_t targetNpcId = 0;
        if (!target || *reinterpret_cast<uint32_t*>(target + 0x2B68) != notices[i].target ||
            !NpcDataId(target, targetNpcId)) continue;
        const auto* entry = FindDamageGate(mission, phase, targetNpcId);
        if (!entry) continue;
        const bool effect = DamageGateShieldFx(target, notices[i].target, true);
        if (effect) sdgo::map_catalog::boundary_runtime::CommitCooldown(
            g_damageGateShieldCooldowns, notices[i].target, now);
        DamageGateWarning(mode, *entry, notices[i].recipient, localPilot, now);
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "damage gate blocked-hit receipt: mission=%lu phase=%lu recipient=%lu target=%lu npc=%lu effect=%u warning_local=%u",
                   static_cast<unsigned long>(mission), static_cast<unsigned long>(phase),
                   static_cast<unsigned long>(notices[i].recipient),
                   static_cast<unsigned long>(notices[i].target),
                   static_cast<unsigned long>(targetNpcId), effect ? 1u : 0u,
                   sdgo::map_catalog::damage_gate_client::IsLocalWarning(
                       notices[i].recipient, localPilot) ? 1u : 0u);
    }

    for (auto entry = first; entry != last; ++entry) {
        auto& state = DamageGateState(*entry);
        if (state.serverDamageObserved ||
            !sdgo::map_catalog::damage_gate_client::Locked(
                entry->requiredDefeats, state.guards)) continue;
        for (const auto& unit : units) {
            if (unit.npcId != entry->targetNpcId || unit.health <= 0 ||
                sdgo::map_catalog::boundary_runtime::CoolingDown(
                    g_damageGateShieldCooldowns, unit.pilot, now,
                    kDamageGateShieldFxCooldownMs)) continue;
            if (DamageGateShieldFx(unit.unit, unit.pilot, false)) {
                sdgo::map_catalog::boundary_runtime::CommitCooldown(
                    g_damageGateShieldCooldowns, unit.pilot, now);
            }
        }
    }
}

void __fastcall MissionUpdateDetour(void* mode, void*) {
    g_missionUpdate(mode);
    __try {
        if (!g_boundaries.empty()) BoundaryTick(static_cast<uint8_t*>(mode));
        if (!g_damageGates.empty()) DamageGateTick(static_cast<uint8_t*>(mode));
        if (!g_retreats.empty()) RetreatFxTick(static_cast<uint8_t*>(mode));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_boundarySnapshotFault && !g_damageGateSnapshotFault) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "exception %08lX while applying imported mission client policy",
                       static_cast<unsigned long>(GetExceptionCode()));
            g_boundarySnapshotFault = true;
            g_damageGateSnapshotFault = true;
        }
    }
}

int __fastcall MapDamageDetour(void* handler, void*, const uint8_t* packet) {
    using namespace sdgo::map_boundary_contact;
    using sdgo::map_catalog::damage_gate_client::NoticeRoute;
    __try {
        if (packet && *reinterpret_cast<const uint16_t*>(packet) == kBoundaryNoticeOpcode) {
            // The native room broadcast rewrites +2 for each route. It is the
            // recipient on the client, not a stable producer discriminator.
            const uint32_t recipient = *reinterpret_cast<const uint32_t*>(packet + 2);
            const uint32_t marked = *reinterpret_cast<const uint32_t*>(packet + 6);
            if ((marked & kMarker) != 0) {
                uint8_t* document = g_unitManager ? *g_unitManager : nullptr;
                const uint32_t exactObjectId = document ? g_objectForPilot(document, marked) : 0;
                uint8_t* exactUnit = exactObjectId ? CanonicalObject(document, exactObjectId) : nullptr;
                const uint32_t exactPilot = exactUnit
                    ? *reinterpret_cast<uint32_t*>(exactUnit + 0x2B68) : 0;

                uint8_t* app = g_gameApp ? g_gameApp() : nullptr;
                uint8_t* service = app ? *reinterpret_cast<uint8_t**>(app + 0x4A4) : nullptr;
                auto* mode = service ? *reinterpret_cast<uint8_t**>(service + 0x0C) : nullptr;
                uint32_t mission = 0, phase = 0;
                const bool activeContext = ActiveMissionContext(mode, mission, phase);
                const uint32_t pilot =
                    sdgo::map_catalog::damage_gate_client::NoticeTarget(marked, kMarker);
                const uint32_t objectId = document && pilot
                    ? g_objectForPilot(document, pilot) : 0;
                uint8_t* unit = objectId ? CanonicalObject(document, objectId) : nullptr;
                const bool canonicalTarget = unit &&
                    *reinterpret_cast<uint32_t*>(unit + 0x2B68) == pilot;
                const auto* boundary = activeContext && !g_boundaries.empty()
                    ? sdgo::map_boundary::Find(g_boundaries, mission, phase) : nullptr;
                const bool activeBoundaryTarget = canonicalTarget && boundary &&
                    (boundary->flags & sdgo::map_boundary::kDamageEnabled) != 0;
                uint32_t targetNpcId = 0;
                const bool activeProtectedTarget = canonicalTarget && activeContext &&
                    !activeBoundaryTarget && NpcDataId(unit, targetNpcId) &&
                    FindDamageGate(mission, phase, targetNpcId) != nullptr;
                const auto route = sdgo::map_catalog::damage_gate_client::RouteNotice(
                    marked, exactPilot, kMarker, activeBoundaryTarget,
                    activeProtectedTarget);

                if (route == NoticeRoute::Protected) {
                    // The packet callback publishes identities only. The completed
                    // mission update resolves the target again before Lua/scene work.
                    QueueDamageGateNotice(recipient, pilot);
                    return 1;
                }
                if (route == NoticeRoute::Boundary) {
                    const uint32_t hp = DecodeHealth(packet + 10);
                    const int32_t before = *reinterpret_cast<int32_t*>(unit + 0x38);
                    const int32_t maximum = *reinterpret_cast<int32_t*>(unit + 0x3C);
                    const uint64_t now = GetTickCount64();
                    int32_t requestedBefore = 0;
                    const bool freshReceipt = ConsumeBoundaryReceipt(
                        pilot, before, maximum, hp, now, requestedBefore);
                    bool applied = false;
                    int32_t after = before;
                    if (sdgo::map_catalog::boundary_runtime::ShouldApplyHealth(
                            before, maximum, hp)) {
                        g_setHp(unit, static_cast<int32_t>(hp));
                        after = *reinterpret_cast<int32_t*>(unit + 0x38);
                        applied = after == static_cast<int32_t>(hp);
                    }
                    const bool contactFeedback = freshReceipt &&
                        after == static_cast<int32_t>(hp) &&
                        BoundaryCombatReleased(mode) &&
                        unit == LocalBoundaryUnit(document);
                    g_api->Log(g_mod, SDGO_LOG_INFO,
                        "boundary health receipt: mission=%lu phase=%lu recipient=%lu "
                        "pilot=%lu object=%lu request_hp=%ld notice=%lu local_hp=%ld->%ld/%ld "
                        "applied=%u fresh=%u feedback=%u",
                        static_cast<unsigned long>(mission),
                        static_cast<unsigned long>(phase),
                        static_cast<unsigned long>(recipient),
                        static_cast<unsigned long>(pilot),
                        static_cast<unsigned long>(objectId),
                        static_cast<long>(requestedBefore),
                        static_cast<unsigned long>(hp),
                        static_cast<long>(before), static_cast<long>(after),
                        static_cast<long>(maximum), applied ? 1u : 0u,
                        freshReceipt ? 1u : 0u, contactFeedback ? 1u : 0u);
                    if (contactFeedback) {
                        SdgoMapLocalBoundaryReceiptV1 observation{
                            sizeof(SdgoMapLocalBoundaryReceiptV1), SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_VERSION,
                            1, 0, static_cast<uint32_t>(now), mission, phase, marked, pilot,
                            objectId, 0, hp, before, after, requestedBefore, maximum, 1};
                        PublishLocalBoundaryReceipt(observation);
                        InterlockedExchange(&g_pendingBoundaryContactFx,
                                            static_cast<LONG>(pilot));
                    }
                    // Every active boundary receipt is consumed here. A malformed,
                    // duplicate or stale value cannot enter native death processing.
                    return 1;
                }
                // Exact native identities and unowned marked packets retain the
                // original 0793 path; the native lookup is nil-safe when absent.
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return g_mapDamageHandler(handler, packet);
}
uint32_t __stdcall DecideOperatorMode(uint32_t mode, const uint8_t* room) {
    if ((mode & 0xFFFF) != kListedMissionMode || !room || g_importedModes.empty()) return mode;
    __try {
        const uint32_t id = U32(room + kRoomGameModeId);
        if (std::find(g_importedModes.begin(), g_importedModes.end(), id) != g_importedModes.end()) {
            g_api->Log(g_mod, SDGO_LOG_INFO, "imported mission %lu: character speech UI enabled",
                       static_cast<unsigned long>(id));
            return kSpeechMissionMode;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return mode;
}

__declspec(naked) void OperatorModeSite() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 0x210
        and esp, 0xfffffff0
        fxsave [esp]
        fninit
        cld
        push dword ptr [ebx + 24]
        push dword ptr [ebx + 28]
        call DecideOperatorMode
        mov [ebx + 28], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_operatorModeOriginal]
    }
}

// The NPC-create notification has a separate mode check before it queues the
// original full-width unit illustration. Opt in only the imported NPC records
// that declare both its native sprite and dialogue; ordinary NPCs stay silent.
uint32_t __stdcall DecideNpcOperatorMode(uint32_t mode, const uint8_t* room, const uint8_t* packet) {
    if (!packet || (mode & 0xFFFF) != kListedMissionMode) return mode;
    __try {
        const uint32_t npc = U32(packet + 0x0E);
        if (g_nativeLog && room &&
            std::find(g_importedModes.begin(), g_importedModes.end(), U32(room + kRoomGameModeId)) != g_importedModes.end())
            g_api->Log(g_mod, SDGO_LOG_INFO, "native imported NPC created: mission=%lu npc=%lu tick=%llu",
                       static_cast<unsigned long>(U32(room + kRoomGameModeId)),
                       static_cast<unsigned long>(npc), static_cast<unsigned long long>(GetTickCount64()));
        if (std::find(g_cutinNpcs.begin(), g_cutinNpcs.end(), npc) == g_cutinNpcs.end()) return mode;
        const uint32_t selected = DecideOperatorMode(mode, room);
        if (selected == kSpeechMissionMode)
            g_api->Log(g_mod, SDGO_LOG_INFO, "imported NPC %lu: native unit cut-in queued", static_cast<unsigned long>(npc));
        return selected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return mode;
    }
}

__declspec(naked) void NpcOperatorModeSite() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 0x210
        and esp, 0xfffffff0
        fxsave [esp]
        fninit
        cld
        push dword ptr [ebx + 4]  // esi: native create-NPC notification
        push dword ptr [ebx + 24] // ecx: current room data
        push dword ptr [ebx + 28] // eax: room mode
        call DecideNpcOperatorMode
        mov [ebx + 28], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_npcOperatorModeOriginal]
    }
}

// Mission loading screen. The battle loading screen shows a mission catalogue's poster (catalogue +1209)
// only for room mode 8; listed missions run in mode 5 and get the client's random pictures. At the
// loading screen's mode check (ecx = room mode, eax = room data) an imported mission whose catalogue
// row has a poster reports mode 8 for that one comparison. The client then reads the GameMode row
// (room +0x9C), takes its group (+106) and walks catalogue rows from 191001 until one has that group
// (+5), so mode 8 is only reported when that walk reaches the imported row without a gap.
struct PosterMode {
    uint32_t mode;
    uint32_t catalogue;
    uint32_t group;
    int state;  // 0 unchecked, 1 usable, -1 not reachable
};
std::vector<PosterMode> g_posterModes;
void* g_loadingModeOriginal = nullptr;
constexpr uint32_t kCatalogueFirst = 191001;
constexpr size_t kCatalogueGroup = 5;
constexpr size_t kCataloguePoster = 1209;
constexpr size_t kGameModeGroup = 106;

void CollectPosterModes() {
    for (const auto& mode : g_catalog.records) {
        if (mode.kind != kGameModeType) continue;
        const uint32_t group = U32(mode.bytes.data() + kGameModeGroup);
        for (const auto& row : g_catalog.records) {
            if (row.kind != kCatalogueType || U32(row.bytes.data() + kCatalogueGroup) != group) continue;
            if (row.bytes[kCataloguePoster] || row.bytes[kCataloguePoster + 1])
                g_posterModes.push_back(PosterMode{mode.id, row.id, group, 0});
            break;
        }
    }
}

bool PosterReachable(const PosterMode& entry) {
    void* grid = g_data();
    if (!grid || entry.catalogue < kCatalogueFirst || entry.catalogue - kCatalogueFirst > 4096) return false;
    const uint8_t* record = nullptr;
    if (!NativeRecord(grid, entry.mode, kGameModeType, &record) || U32(record + kGameModeGroup) != entry.group)
        return false;
    for (uint32_t id = kCatalogueFirst; id <= entry.catalogue; ++id) {
        if (!NativeRecord(grid, id, kCatalogueType, &record)) return false;
        if (U32(record + kCatalogueGroup) == entry.group) return id == entry.catalogue;
    }
    return false;
}

uint32_t __stdcall DecideLoadingMode(uint32_t mode, const uint8_t* room) {
    if ((mode & 0xFFFF) != kListedMissionMode || !room || g_posterModes.empty()) return mode;
    __try {
        const uint32_t id = U32(room + kRoomGameModeId);
        for (auto& entry : g_posterModes) {
            if (entry.mode != id) continue;
            if (entry.state == 0) {
                entry.state = PosterReachable(entry) ? 1 : -1;
                if (entry.state < 0)
                    g_api->Log(g_mod, SDGO_LOG_WARN,
                               "imported mission %lu: catalogue row %lu is not reachable from %lu; random loading screen",
                               static_cast<unsigned long>(id), static_cast<unsigned long>(entry.catalogue),
                               static_cast<unsigned long>(kCatalogueFirst));
            }
            if (entry.state > 0) {
                g_api->Log(g_mod, SDGO_LOG_INFO, "imported mission %lu: mission poster loading screen",
                           static_cast<unsigned long>(id));
                return kSpeechMissionMode;
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return mode;
}

__declspec(naked) void LoadingModeSite() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 0x210
        and esp, 0xfffffff0
        fxsave [esp]
        fninit
        cld
        push dword ptr [ebx + 28]
        push dword ptr [ebx + 24]
        call DecideLoadingMode
        mov [ebx + 24], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        jmp dword ptr [g_loadingModeOriginal]
    }
}

// Optional (setting NativeDebugLog=1): let the game write its own debug log (app_log folder),
// which names missing records and failed script lookups while an owner tests a mission.
void EnableNativeLog() {
    static bool done = false;
    if (!g_nativeLog || done) return;
    __try {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto* holder = *reinterpret_cast<uint8_t**>(base + 0xC05604);
        auto* logger = holder ? *reinterpret_cast<int**>(holder + 0x4A8) : nullptr;
        if (logger) {
            logger[1] = 1;
            logger[2] = 9;
            done = true;
            g_api->Log(g_mod, SDGO_LOG_INFO, "native debug log enabled (client app_log)");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

template<class T>
bool Bind(T& target, const char* name) {
    target = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return target != nullptr;
}

// First read of each imported resource is logged: it proves which imported
// terrain/model/texture a battle actually loaded (and helps owners debug).
CRITICAL_SECTION g_openLock;
std::vector<std::wstring> g_opened;

void __cdecl ResourceOpened(const char* owner, const wchar_t* path, void*, void*) {
    if (!owner || std::strcmp(owner, "map_catalog") != 0 || !path) return;
    EnterCriticalSection(&g_openLock);
    bool first = g_opened.size() < 512;
    for (const auto& seen : g_opened)
        if (seen == path) { first = false; break; }
    if (first) g_opened.emplace_back(path);
    LeaveCriticalSection(&g_openLock);
    if (first) g_api->Log(g_mod, SDGO_LOG_INFO, "imported resource loaded: %ls", path);
}

bool RegisterResourceOverlay() {
    using GetBroker = const SdgoResourceOverlayBrokerApiV1*(__cdecl*)();
    const SdgoResourceOverlayBrokerApiV1* shared = nullptr;
    const wchar_t* sharedModule = nullptr;
    for (const wchar_t* name : {L"capsule_catalog.dll", L"unit_catalog.dll"}) {
        HMODULE module = GetModuleHandleW(name);
        if (!module) continue;
        auto getter = reinterpret_cast<GetBroker>(GetProcAddress(module, "SdgoGetResourceOverlayBrokerV1"));
        if (!getter) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "loaded %ls is incompatible: SdgoGetResourceOverlayBrokerV1 is missing", name);
            return false;
        }
        const SdgoResourceOverlayBrokerApiV1* broker = getter();
        if (broker) {
            if (broker->size < sizeof(SdgoResourceOverlayBrokerApiV1) || broker->version != 1 ||
                !broker->RegisterBundle) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "loaded %ls has an incompatible resource broker ABI", name);
                return false;
            }
            if (shared && shared != broker) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "multiple native resource brokers are active");
                return false;
            }
            shared = broker;
            sharedModule = name;
        }
    }
    if (shared) {
            char error[512] = {};
            if (!shared->RegisterBundle(g_api->ModDirectory(g_mod), L"resource-overlay.ini", "map_catalog",
                                        error, static_cast<uint32_t>(sizeof(error)))) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "%ls resource broker rejected map bundle: %s",
                           sharedModule, error[0] ? error : "unknown broker error");
                return false;
            }
            g_api->Log(g_mod, SDGO_LOG_INFO, "sealed map resources registered with %ls resource broker", sharedModule);
            return true;
    }
    // Only when no earlier catalogue owns the hook do maps start a provider.
    std::string error;
    auto& provider = sdgo::resource_overlay::LocalProvider();
    if (!provider.RegisterBundleFiles(g_api->ModDirectory(g_mod), L"resource-overlay.ini", "map_catalog", error) ||
        (!provider.Active() && !provider.Start(g_api, g_mod, &ResourceOpened, nullptr, error))) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "standalone map resource broker rejected: %s", error.c_str());
        return false;
    }
    g_api->Log(g_mod, SDGO_LOG_INFO, "sealed map resources registered with standalone resource broker");
    return true;
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl
SdgoMapCatalogGetMissionSuccessV1(SdgoMapMissionSuccessV1* value) {
    if (!value || value->size < sizeof(*value) ||
        value->version != SDGO_MAP_MISSION_SUCCESS_VERSION) return 0;
    *value = MissionSuccessSnapshot();
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl
SdgoMapCatalogGetLocalBoundaryReceiptV1(SdgoMapLocalBoundaryReceiptV1* value) {
    if (!value || value->size < sizeof(*value) ||
        value->version != SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_VERSION) return 0;
    *value = LocalBoundaryReceiptSnapshot();
    return 1;
}

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION || !api->SettingInt(mod, "Enabled", 1)) return 0;
    InitializeCriticalSection(&g_openLock);

    std::string error;
    const std::wstring modDirectory = api->ModDirectory(mod);
    if (!g_catalog.Load(modDirectory + L"\\catalog.bin", error) ||
        !LoadBoundaries(modDirectory + L"\\boundaries.bin", error) || !ValidateBoundaries(error) ||
        !LoadDamageGates(modDirectory + L"\\damage-gates.bin", error) ||
        !ValidateDamageGates(error) ||
        !LoadRetreats(modDirectory + L"\\retreats.bin", error) ||
        !ValidateRetreats(error)) {
        api->Log(mod, SDGO_LOG_ERROR, "%s", error.c_str());
        return 0;
    }
    if (!Bind(g_data, "grid.data") || !Bind(g_find, "grid.find") || !Bind(g_allocate, "crt.operator_new") ||
        !Bind(g_release, "crt.operator_delete") || !Bind(g_makeNode, "grid.make_node") ||
        !Bind(g_insertNode, "grid.insert_node") || !Bind(g_registerRecord, "grid.register_record") ||
        !Bind(g_mapFind, "grid.map_find")) {
        return 0;
    }
    if (!RegisterResourceOverlay()) return 0;
    if (!LoadMissionScript(modDirectory + L"\\missions.lua")) {
        api->Log(mod, SDGO_LOG_ERROR, "missions.lua must be ASCII text Lua under 4 MiB");
        return 0;
    }
    const bool scriptedMissionPolicy = !g_missionScript.empty() ||
        !g_boundaries.empty() || !g_damageGates.empty() || !g_retreats.empty();
    if (scriptedMissionPolicy &&
        (!Bind(g_luaState, "lua_tinker.state") || !Bind(g_luaLoad, "lua.load") ||
         !Bind(g_luaPcall, "lua.pcall") || !Bind(g_luaGetTop, "lua.gettop") ||
         !Bind(g_luaSetTop, "lua.settop") || !Bind(g_luaToString, "lua.tostring") ||
         !Bind(g_luaPushNumber, "lua.pushnumber") || !Bind(g_luaPushString, "lua.pushstring") ||
         !Bind(g_luaPushCClosure, "lua.pushcclosure") || !Bind(g_luaSetTable, "lua.settable") ||
         !api->HookCode(mod, "map_catalog.mission_call", reinterpret_cast<void*>(&MissionCallDetour),
                        reinterpret_cast<void**>(&g_missionCall)))) {
        return 0;
    }
    bool boundaryWarning = false;
    bool boundaryDamage = false;
    for (const auto& entry : g_boundaries) {
        boundaryWarning = boundaryWarning ||
            (entry.flags & sdgo::map_boundary::kWarningEnabled) != 0;
        boundaryDamage = boundaryDamage ||
            (entry.flags & sdgo::map_boundary::kDamageEnabled) != 0;
    }
    if ((boundaryWarning || !g_damageGates.empty()) && g_missionScript.empty()) {
        api->Log(mod, SDGO_LOG_ERROR,
                 boundaryWarning ? "boundary warnings require missions.lua" :
                                   "damage gates require missions.lua");
        return 0;
    }
    if (scriptedMissionPolicy &&
        (!Bind(g_gameApp, "game.app") || !Bind(g_unitManager, "game.unit_manager") ||
         !Bind(g_findObject, "um.find_object") ||
         !Bind(g_objectForPilot, "um.object_id_from_pilot"))) return 0;
    if (!g_boundaries.empty() &&
        (!Bind(g_setTerrainRect, "map_catalog.set_terrain_rect") ||
         !Bind(g_setTerrainRectOnOff, "map_catalog.set_terrain_rect_enabled"))) return 0;
    if (boundaryDamage &&
        (!Bind(g_setHp, "map_catalog.set_hp") || !Bind(g_network, "flow.network") ||
         !Bind(g_networkSend, "flow.network_send") ||
         !Bind(g_queueState, "fsm.queue_state"))) return 0;
    if ((boundaryDamage || !g_damageGates.empty()) &&
        !api->HookCode(mod, "game.map_damage_handler", reinterpret_cast<void*>(&MapDamageDetour),
                       reinterpret_cast<void**>(&g_mapDamageHandler))) return 0;
    if ((!g_boundaries.empty() || !g_damageGates.empty() || !g_retreats.empty()) &&
        !api->HookCode(mod, "map_catalog.mission_update", reinterpret_cast<void*>(&MissionUpdateDetour),
                       reinterpret_cast<void**>(&g_missionUpdate))) return 0;
    if (!g_retreats.empty()) {
        if (api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
            !api->HookSite ||
            !Bind(g_retreatFxSkip, "map_catalog.retreat_fx_skip") ||
            !Bind(g_retreatAudioSkip, "map_catalog.retreat_audio_skip") ||
            !api->HookSite(mod, "map_catalog.retreat_fx", reinterpret_cast<void*>(&RetreatFxSite),
                           &g_retreatFxOriginal) ||
            !api->HookSite(mod, "map_catalog.retreat_audio", reinterpret_cast<void*>(&RetreatAudioSite),
                           &g_retreatAudioOriginal) ||
            !api->HookCode(mod, "map_catalog.success_cleanup",
                           reinterpret_cast<void*>(&SuccessCleanupDetour),
                           reinterpret_cast<void**>(&g_successCleanup))) {
            api->Log(mod, SDGO_LOG_ERROR,
                     "authoritative allied retreat presentation hooks unavailable");
            return 0;
        }
    }
    bool groupedSections = false;
    for (const auto& entry : g_catalog.records)
        groupedSections = groupedSections || (entry.kind == kGameModeType && (entry.flags & kSectionClearPolicy));
    if (groupedSections && (!Bind(g_gameApp, "game.app") ||
        !api->HookCode(mod, "map_catalog.mission_message", reinterpret_cast<void*>(&MissionMessageDetour),
                       reinterpret_cast<void**>(&g_missionMessage)))) return 0;
    g_nativeLog = api->SettingInt(mod, "NativeDebugLog", 0) != 0;
    const size_t maps = g_catalog.Count(kMapType);
    const size_t missionRecords = g_catalog.records.size() - maps;
    unsigned inheritedCutins = 0;
    for (const auto& entry : g_catalog.records) {
        if (entry.kind == kGameModeType) g_importedModes.push_back(entry.id);
        if (DeclaresUnitCutin(g_catalog, entry)) g_cutinNpcs.push_back(entry.id);
        else if (entry.kind == kNpcTemplateType && entry.bytes.size() == 598 &&
                 U32(entry.bytes.data() + 390) && entry.bytes[398]) ++inheritedCutins;
    }
    api->Log(mod, SDGO_LOG_INFO, "native unit cut-ins: %lu declared, %u inherited references excluded",
             static_cast<unsigned long>(g_cutinNpcs.size()), inheritedCutins);
    if (!g_importedModes.empty() && api->size >= offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) && api->HookSite &&
        !api->HookSite(mod, "map_catalog.operator_mode", reinterpret_cast<void*>(&OperatorModeSite),
                       &g_operatorModeOriginal)) {
        api->Log(mod, SDGO_LOG_WARN, "character speech UI unavailable for imported missions");
    }
    if (!g_cutinNpcs.empty() &&
        (api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->HookSite ||
         !api->HookSite(mod, "map_catalog.npc_operator_mode", reinterpret_cast<void*>(&NpcOperatorModeSite),
                        &g_npcOperatorModeOriginal))) {
        api->Log(mod, SDGO_LOG_ERROR, "native unit cut-ins unavailable for imported missions");
        return 0;
    }
    CollectPosterModes();
    if (!g_posterModes.empty() && api->size >= offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) &&
        api->HookSite &&
        !api->HookSite(mod, "map_catalog.loading_mode", reinterpret_cast<void*>(&LoadingModeSite),
                       &g_loadingModeOriginal)) {
        api->Log(mod, SDGO_LOG_WARN, "mission posters unavailable on the loading screen");
    }
    if (maps && !api->HookCode(mod, "map_catalog.load_map_table", reinterpret_cast<void*>(&LoadMapTable),
                               reinterpret_cast<void**>(&g_loadMapTable))) {
        return 0;
    }
    if (missionRecords && !api->HookCode(mod, "map_catalog.tables_ready", reinterpret_cast<void*>(&TablesReadyDetour),
                                         reinterpret_cast<void**>(&g_tablesReady))) {
        return 0;
    }
    api->Log(mod, SDGO_LOG_INFO, "ready: %u map rows and %u mission records; registration deferred to table load",
             static_cast<unsigned>(maps), static_cast<unsigned>(missionRecords));
    return 1;
}
