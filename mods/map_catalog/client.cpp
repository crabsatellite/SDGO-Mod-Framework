// map_catalog: registers imported maps and missions produced by the neutral
// map package compiler. Map rows (kind 0x0F) are inserted right after the
// native map table loads; mission records are inserted after every table has
// loaded, immediately before the game's derived-data pass. Resources (terrain,
// models, textures, sprites) come from a sealed overlay bundle owned by
// "map_catalog".
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/map_catalog_format.h"
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
using LuaReader = const char*(__cdecl*)(lua_State*, void*, size_t*);
using LuaStateFn = lua_State*(__cdecl*)(lua_State*);
using LuaLoad = int(__cdecl*)(lua_State*, LuaReader, void*, const char*);
using LuaPcall = int(__cdecl*)(lua_State*, int, int, int);
using LuaGetTop = int(__cdecl*)(lua_State*);
using LuaSetTop = void(__cdecl*)(lua_State*, int);
using LuaToString = const char*(__cdecl*)(lua_State*, int);
using MissionCall = int(__cdecl*)(const char*, void*);

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
MissionCall g_missionCall = nullptr;
std::string g_missionScript;
lua_State* g_scriptVm = nullptr;
bool g_mapsAttempted = false;
bool g_missionsAttempted = false;
uint32_t g_published = 0;

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

// Imported mission scripts (cutscenes, phase events) are plain ASCII text Lua
// generated by the map compiler. They only define globals, so loading them
// once per VM before the first mission script call is enough.
void EnsureMissionScripts() {
    if (g_missionScript.empty()) return;
    lua_State* L = g_luaState(nullptr);
    if (!L || L == g_scriptVm) return;
    g_scriptVm = L;
    const int top = g_luaGetTop(L);
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
    const int result = g_missionCall(name, mode);
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

// Character speech in imported missions. PutOperatorSpeech posts to the "OriginalModeOperator"
// UI, which the battle HUD setup creates only for room modes 8 and 10; listed missions run in
// mode 5. At the HUD's mode check (eax = room mode, ecx = room data) an imported mission's room
// reports mode 8 for that one comparison, so the client builds its own speech UI. Every other
// room keeps its native mode.
std::vector<uint32_t> g_importedModes;
void* g_operatorModeOriginal = nullptr;
constexpr size_t kRoomGameModeId = 0x9C;  // u32 GameModeID in the room data (the 0x66 mode word's object)
constexpr uint32_t kListedMissionMode = 5;
constexpr uint32_t kSpeechMissionMode = 8;

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

// Optional (setting NativeDebugLog=1): let the game write its own debug log (app_log folder),
// which names missing records and failed script lookups while an owner tests a mission.
bool g_nativeLog = false;

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
    HMODULE capsule = GetModuleHandleW(L"capsule_catalog.dll");
    if (capsule) {
        auto getter = reinterpret_cast<GetBroker>(GetProcAddress(capsule, "SdgoGetResourceOverlayBrokerV1"));
        if (!getter) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "loaded capsule_catalog.dll is incompatible: SdgoGetResourceOverlayBrokerV1 is missing");
            return false;
        }
        const SdgoResourceOverlayBrokerApiV1* broker = getter();
        if (broker) {
            if (broker->size < sizeof(SdgoResourceOverlayBrokerApiV1) || broker->version != 1 ||
                !broker->RegisterBundle) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "loaded capsule resource broker has an incompatible ABI");
                return false;
            }
            char error[512] = {};
            if (!broker->RegisterBundle(g_api->ModDirectory(g_mod), L"resource-overlay.ini", "map_catalog",
                                        error, static_cast<uint32_t>(sizeof(error)))) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "capsule resource broker rejected map bundle: %s",
                           error[0] ? error : "unknown broker error");
                return false;
            }
            g_api->Log(g_mod, SDGO_LOG_INFO, "sealed map resources registered with capsule resource broker");
            return true;
        }
    }
    // Without the capsule broker this module runs its own provider. Each module
    // links its own provider copy, so only one standalone provider can own the
    // OpenFile hook; owner clients normally load capsule_catalog as the broker.
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

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION || !api->SettingInt(mod, "Enabled", 1)) return 0;
    InitializeCriticalSection(&g_openLock);

    std::string error;
    if (!g_catalog.Load(std::wstring(api->ModDirectory(mod)) + L"\\catalog.bin", error)) {
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
    if (!LoadMissionScript(std::wstring(api->ModDirectory(mod)) + L"\\missions.lua")) {
        api->Log(mod, SDGO_LOG_ERROR, "missions.lua must be ASCII text Lua under 4 MiB");
        return 0;
    }
    if (!g_missionScript.empty() &&
        (!Bind(g_luaState, "lua_tinker.state") || !Bind(g_luaLoad, "lua.load") || !Bind(g_luaPcall, "lua.pcall") ||
         !Bind(g_luaGetTop, "lua.gettop") || !Bind(g_luaSetTop, "lua.settop") || !Bind(g_luaToString, "lua.tostring") ||
         !api->HookCode(mod, "map_catalog.mission_call", reinterpret_cast<void*>(&MissionCallDetour),
                        reinterpret_cast<void**>(&g_missionCall)))) {
        return 0;
    }
    g_nativeLog = api->SettingInt(mod, "NativeDebugLog", 0) != 0;
    const size_t maps = g_catalog.Count(kMapType);
    const size_t missionRecords = g_catalog.records.size() - maps;
    for (const auto& entry : g_catalog.records)
        if (entry.kind == kGameModeType) g_importedModes.push_back(entry.id);
    if (!g_importedModes.empty() && api->size >= offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) && api->HookSite &&
        !api->HookSite(mod, "map_catalog.operator_mode", reinterpret_cast<void*>(&OperatorModeSite),
                       &g_operatorModeOriginal)) {
        api->Log(mod, SDGO_LOG_WARN, "character speech UI unavailable for imported missions");
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
