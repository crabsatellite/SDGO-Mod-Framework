#ifdef NDEBUG
#undef NDEBUG
#endif

#include <windows.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sdgo/map_catalog_format.h"
#include "sdgo/mission_damage_gate_format.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/map_catalog_mission_observer.h"

namespace {
constexpr std::uint32_t kMission = 70001;
constexpr std::uint32_t kPhase = 6;
constexpr std::uint32_t kNpc = 91002;
constexpr std::uint32_t kClear = 88001;
constexpr std::uint32_t kPilot = 0x80000011u;
constexpr std::uint32_t kObject = 77;

std::wstring g_directory;
std::array<std::uint8_t, 0x500> g_app{};
std::array<std::uint8_t, 0x20> g_service{};
std::array<std::uint8_t, 0x700> g_mode{};
std::array<std::uint8_t, 1090> g_nativeGameMode{};
std::array<std::uint8_t, 391> g_clear{};
std::array<std::uint8_t, 0x3300> g_unit{};
std::array<std::uint8_t, 0x100> g_node{};
std::uint8_t g_grid = 0;
std::uint8_t g_documentStorage = 0;
std::uint8_t* g_document = &g_documentStorage;
std::uint8_t g_luaStateStorage = 0;

void* g_successHook = nullptr;
void* g_missionUpdateHook = nullptr;
void* g_fxHook = nullptr;
void* g_audioHook = nullptr;
const std::uint8_t* g_seenPacket = nullptr;
std::uint16_t g_seenOpcode = 0;
std::string g_lastLua;
LONG g_originalCalls = 0;
LONG g_missionUpdates = 0;
LONG g_fxNative = 0;
LONG g_fxSkip = 0;
LONG g_audioNative = 0;
LONG g_audioSkip = 0;
int g_depth = 0;
bool g_nested = false;
bool g_throw = false;
bool g_mismatchedAudioMessage = false;
bool g_preserveBridgeState = false;
bool g_fxStatePreserved = false;
bool g_audioStatePreserved = false;
void* g_fixtureHook = nullptr;
void* g_fixtureUnit = nullptr;
const void* g_fixtureMessage = nullptr;
std::uint32_t g_fixtureStack = 0;
std::uint32_t g_beforeRegisters[8]{};
std::uint32_t g_afterRegisters[8]{};
DWORD g_beforeFlags = 0;
DWORD g_afterFlags = 0;
std::uint16_t g_fixtureControlWord = 0x077F;
DWORD g_fixtureMxcsr = 0x5F80;
__declspec(align(16)) std::uint32_t g_xmmPatterns[32]{
    0x3F800001, 0x40000002, 0x40400003, 0x40800004,
    0x40A00005, 0x40C00006, 0x40E00007, 0x41000008,
    0x41100009, 0x4120000A, 0x4130000B, 0x4140000C,
    0x4150000D, 0x4160000E, 0x4170000F, 0x41800010,
    0x41880011, 0x41900012, 0x41980013, 0x41A00014,
    0x41A80015, 0x41B00016, 0x41B80017, 0x41C00018,
    0x41C80019, 0x41D0001A, 0x41D8001B, 0x41E0001C,
    0x41E8001D, 0x41F0001E, 0x41F8001F, 0x42000020,
};
__declspec(align(16)) std::uint8_t g_beforeFx[512]{};
__declspec(align(16)) std::uint8_t g_afterFx[512]{};

struct NativeRemovalMessage {
    std::uint16_t type = 2;
    std::uint16_t padding = 0;
    std::uint32_t fields[3]{};
    std::uint8_t tail = 0;
};

template<class T>
void Put(void* base, std::ptrdiff_t offset, const T& value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

void WriteAll(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}

void WriteCatalog(const std::filesystem::path& path) {
    using namespace sdgo::map_catalog;
    std::vector<std::uint8_t> record(ExpectedBytes(kGameModeType));
    Put(record.data(), 0, kMission);
    EntryHeader entry{kGameModeType, kMission, static_cast<std::uint32_t>(record.size()),
                      Hash32(record.data(), record.size()), 0, 0};
    std::vector<std::uint8_t> payload(sizeof(entry) + record.size());
    std::memcpy(payload.data(), &entry, sizeof(entry));
    std::memcpy(payload.data() + sizeof(entry), record.data(), record.size());
    Header header{};
    std::memcpy(header.magic, "SDGOMP1", 8);
    header.version = kFormatVersion;
    header.headerBytes = sizeof(header);
    header.count = 1;
    header.payloadBytes = static_cast<std::uint32_t>(payload.size());
    header.payloadHash = Hash32(payload.data(), payload.size());
    std::vector<std::uint8_t> file(sizeof(header) + payload.size());
    std::memcpy(file.data(), &header, sizeof(header));
    std::memcpy(file.data() + sizeof(header), payload.data(), payload.size());
    WriteAll(path, file);
}

void WriteRetreats(const std::filesystem::path& path) {
    std::vector<std::uint8_t> file(44);
    std::memcpy(file.data(), "SDGORET1", 8);
    Put(file.data(), 8, std::uint32_t{1});
    Put(file.data(), 12, std::uint32_t{32});
    Put(file.data(), 16, std::uint32_t{12});
    Put(file.data(), 20, std::uint32_t{1});
    Put(file.data(), 24, std::uint32_t{12});
    Put(file.data(), 32, kMission);
    Put(file.data(), 36, kPhase);
    Put(file.data(), 40, kNpc);
    const auto checksum = sdgo::mission_damage_gate::Checksum(file.data() + 32, 12);
    Put(file.data(), 28, checksum);
    WriteAll(path, file);
}

void PrepareNativeState(std::int32_t health = 100) {
    g_app.fill(0); g_service.fill(0); g_mode.fill(0); g_nativeGameMode.fill(0);
    g_clear.fill(0); g_unit.fill(0); g_node.fill(0);
    Put(g_app.data(), 0x4A0, std::uint32_t{22});
    auto* service = g_service.data();
    auto* mode = g_mode.data();
    auto* record = g_nativeGameMode.data();
    Put(g_app.data(), 0x4A4, service);
    Put(g_service.data(), 0x0C, mode);
    Put(g_mode.data(), 0xE8, record);
    Put(g_mode.data(), 0x620, kPhase);
    Put(g_nativeGameMode.data(), 0, kMission);
    g_clear[5] = 1;
    Put(g_clear.data(), 41, kMission);
    Put(g_clear.data(), 45, kPhase);
    Put(g_unit.data(), 4, kObject);
    Put(g_unit.data(), 0x2B68, kPilot);
    Put(g_unit.data(), 0x3238, kNpc);
    Put(g_unit.data(), 0x38, health);
    auto* node = g_node.data();
    Put(g_unit.data(), 8, node);
    Put(g_unit.data(), 0xC88, 4.0f);
    Put(g_node.data(), 0x88, 10.0f);
    Put(g_node.data(), 0x8C, 20.0f);
    Put(g_node.data(), 0x90, 30.0f);
}

std::array<std::uint8_t, 55> Packet(std::uint16_t opcode) {
    std::array<std::uint8_t, 55> packet{};
    Put(packet.data(), 0, opcode);
    Put(packet.data(), 6, kClear);
    Put(packet.data(), 10, kPhase);
    packet[14] = 1;
    Put(packet.data(), 15, kPilot);
    return packet;
}

#define MAP_RETREAT_CAPTURE_TARGET(Name, Counter) \
    __declspec(naked) void Name() { \
        __asm { mov dword ptr [g_afterRegisters + 0], eax } \
        __asm { mov dword ptr [g_afterRegisters + 4], ebx } \
        __asm { mov dword ptr [g_afterRegisters + 8], ecx } \
        __asm { mov dword ptr [g_afterRegisters + 12], edx } \
        __asm { mov dword ptr [g_afterRegisters + 16], esi } \
        __asm { mov dword ptr [g_afterRegisters + 20], edi } \
        __asm { mov dword ptr [g_afterRegisters + 24], ebp } \
        __asm { mov dword ptr [g_afterRegisters + 28], esp } \
        __asm { pushfd } \
        __asm { pop dword ptr [g_afterFlags] } \
        __asm { fxsave [g_afterFx] } \
        __asm { pushfd } \
        __asm { inc dword ptr [Counter] } \
        __asm { popfd } \
        __asm { ret } \
    }

MAP_RETREAT_CAPTURE_TARGET(FxNativeTarget, g_fxNative)
MAP_RETREAT_CAPTURE_TARGET(FxSkipTarget, g_fxSkip)
MAP_RETREAT_CAPTURE_TARGET(AudioNativeTarget, g_audioNative)
MAP_RETREAT_CAPTURE_TARGET(AudioSkipTarget, g_audioSkip)
#undef MAP_RETREAT_CAPTURE_TARGET

__declspec(naked) void PassiveSiteTarget() { __asm { ret } }

void InvokeSite(void* hook, void* unit, const void* message) {
    __asm {
        mov ebx, unit
        mov esi, message
        mov eax, hook
        call eax
    }
}

__declspec(naked) void InvokeSitePreserved(void*, void*, const void*) {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, dword ptr [esp + 40] }
    __asm { mov dword ptr [g_fixtureHook], eax }
    __asm { mov eax, dword ptr [esp + 44] }
    __asm { mov dword ptr [g_fixtureUnit], eax }
    __asm { mov eax, dword ptr [esp + 48] }
    __asm { mov dword ptr [g_fixtureMessage], eax }
    __asm { mov dword ptr [g_fixtureStack], esp }
    __asm { fninit }
    __asm { fldcw word ptr [g_fixtureControlWord] }
    __asm { fld1 }
    __asm { ldmxcsr dword ptr [g_fixtureMxcsr] }
    __asm { movaps xmm0, xmmword ptr [g_xmmPatterns + 0] }
    __asm { movaps xmm1, xmmword ptr [g_xmmPatterns + 16] }
    __asm { movaps xmm2, xmmword ptr [g_xmmPatterns + 32] }
    __asm { movaps xmm3, xmmword ptr [g_xmmPatterns + 48] }
    __asm { movaps xmm4, xmmword ptr [g_xmmPatterns + 64] }
    __asm { movaps xmm5, xmmword ptr [g_xmmPatterns + 80] }
    __asm { movaps xmm6, xmmword ptr [g_xmmPatterns + 96] }
    __asm { movaps xmm7, xmmword ptr [g_xmmPatterns + 112] }
    __asm { mov ebx, dword ptr [g_fixtureUnit] }
    __asm { mov esi, dword ptr [g_fixtureMessage] }
    __asm { mov eax, 0x10203040 }
    __asm { mov ecx, 0x50607080 }
    __asm { mov edx, 0x90A0B0C0 }
    __asm { mov edi, 0xD0E0F001 }
    __asm { mov ebp, 0x13579BDF }
    __asm { std }
    __asm { stc }
    __asm { push offset returned_from_site }
    __asm { mov dword ptr [g_beforeRegisters + 0], eax }
    __asm { mov dword ptr [g_beforeRegisters + 4], ebx }
    __asm { mov dword ptr [g_beforeRegisters + 8], ecx }
    __asm { mov dword ptr [g_beforeRegisters + 12], edx }
    __asm { mov dword ptr [g_beforeRegisters + 16], esi }
    __asm { mov dword ptr [g_beforeRegisters + 20], edi }
    __asm { mov dword ptr [g_beforeRegisters + 24], ebp }
    __asm { mov dword ptr [g_beforeRegisters + 28], esp }
    __asm { pushfd }
    __asm { pop dword ptr [g_beforeFlags] }
    __asm { fxsave [g_beforeFx] }
    __asm { jmp dword ptr [g_fixtureHook] }
returned_from_site:
    __asm { mov esp, dword ptr [g_fixtureStack] }
    __asm { popad }
    __asm { popfd }
    __asm { ret }
}

bool MachineStateMatches(DWORD expectedError) {
    const DWORD actualError = GetLastError();
    return actualError == expectedError &&
        std::memcmp(g_beforeRegisters, g_afterRegisters, sizeof(g_beforeRegisters)) == 0 &&
        g_beforeFlags == g_afterFlags && (g_afterFlags & 0x401u) == 0x401u &&
        std::memcmp(g_beforeFx, g_afterFx, sizeof(g_beforeFx)) == 0;
}

int __stdcall FakeSuccessCleanup(const std::uint8_t* packet) {
    ++g_originalCalls;
    g_seenPacket = packet;
    std::memcpy(&g_seenOpcode, packet, sizeof(g_seenOpcode));
    if (g_throw) RaiseException(0xE0421001u, 0, 0, nullptr);
    if (g_nested && g_depth == 0) {
        ++g_depth;
        auto nested = Packet(0x0570);
        const auto invoke = reinterpret_cast<int(__stdcall*)(const std::uint8_t*)>(g_successHook);
        assert(invoke(nested.data()) == 1);
        --g_depth;
    }
    NativeRemovalMessage message{};
    NativeRemovalMessage second{};
    if (g_preserveBridgeState) {
        const DWORD fxError = GetLastError();
        InvokeSitePreserved(g_fxHook, g_unit.data(), &message);
        g_fxStatePreserved = MachineStateMatches(fxError);
        const DWORD audioError = GetLastError();
        InvokeSitePreserved(
            g_audioHook, g_unit.data(), g_mismatchedAudioMessage ? &second : &message);
        g_audioStatePreserved = MachineStateMatches(audioError);
    } else {
        InvokeSite(g_fxHook, g_unit.data(), &message);
        InvokeSite(g_audioHook, g_unit.data(), g_mismatchedAudioMessage ? &second : &message);
    }
    return 1;
}

void __fastcall FakeMissionUpdate(void*, void*) { ++g_missionUpdates; }
int __cdecl FakeMissionCall(const char*, void*) { return 0; }
std::uint8_t* __cdecl FakeGameApp() { return g_app.data(); }
void* __cdecl FakeGridData() { return &g_grid; }

bool __fastcall FakeGridFind(void*, void*, const std::uint8_t** out,
                             std::uint32_t id, int kind, char) {
    if (out) *out = nullptr;
    if (id == kMission && kind == static_cast<int>(sdgo::map_catalog::kGameModeType)) {
        if (out) *out = g_nativeGameMode.data();
        return true;
    }
    if (id == kClear && kind == 0x2F) {
        if (out) *out = g_clear.data();
        return true;
    }
    return false;
}
std::uint32_t __fastcall FakeObjectForPilot(void*, void*, std::uint32_t pilot) {
    return pilot == kPilot ? kObject : 0;
}
std::uint8_t* __fastcall FakeFindObject(void*, void*, std::uint32_t object) {
    return object == kObject ? g_unit.data() : nullptr;
}

void* __cdecl FakeLuaState(void*) { return &g_luaStateStorage; }
int __cdecl FakeLuaGetTop(void*) { return 0; }
void __cdecl FakeLuaSetTop(void*, int) {}
const char* __cdecl FakeLuaToString(void*, int) { return "fixture error"; }
void __cdecl FakeLuaPushNumber(void*, double) {}
void __cdecl FakeLuaPushString(void*, const char*) {}
void __cdecl FakeLuaPushCClosure(void*, void*, int) {}
void __cdecl FakeLuaSetTable(void*, int) {}
int __cdecl FakeLuaPcall(void*, int, int, int) { return 0; }
int __cdecl FakeLuaLoad(void* state, const char* (__cdecl* reader)(void*, void*, std::size_t*),
                        void* data, const char*) {
    std::size_t size = 0;
    const char* text = reader(state, data, &size);
    g_lastLua.assign(text ? text : "", size);
    return 0;
}
void __cdecl Dummy() {}

void* Symbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "grid.data")) return reinterpret_cast<void*>(&FakeGridData);
    if (!std::strcmp(name, "grid.find")) return reinterpret_cast<void*>(&FakeGridFind);
    if (!std::strcmp(name, "game.app")) return reinterpret_cast<void*>(&FakeGameApp);
    if (!std::strcmp(name, "game.unit_manager")) return &g_document;
    if (!std::strcmp(name, "um.find_object")) return reinterpret_cast<void*>(&FakeFindObject);
    if (!std::strcmp(name, "um.object_id_from_pilot")) return reinterpret_cast<void*>(&FakeObjectForPilot);
    if (!std::strcmp(name, "lua_tinker.state")) return reinterpret_cast<void*>(&FakeLuaState);
    if (!std::strcmp(name, "lua.load")) return reinterpret_cast<void*>(&FakeLuaLoad);
    if (!std::strcmp(name, "lua.pcall")) return reinterpret_cast<void*>(&FakeLuaPcall);
    if (!std::strcmp(name, "lua.gettop")) return reinterpret_cast<void*>(&FakeLuaGetTop);
    if (!std::strcmp(name, "lua.settop")) return reinterpret_cast<void*>(&FakeLuaSetTop);
    if (!std::strcmp(name, "lua.tostring")) return reinterpret_cast<void*>(&FakeLuaToString);
    if (!std::strcmp(name, "lua.pushnumber")) return reinterpret_cast<void*>(&FakeLuaPushNumber);
    if (!std::strcmp(name, "lua.pushstring")) return reinterpret_cast<void*>(&FakeLuaPushString);
    if (!std::strcmp(name, "lua.pushcclosure")) return reinterpret_cast<void*>(&FakeLuaPushCClosure);
    if (!std::strcmp(name, "lua.settable")) return reinterpret_cast<void*>(&FakeLuaSetTable);
    if (!std::strcmp(name, "map_catalog.retreat_fx_skip")) return reinterpret_cast<void*>(&FxSkipTarget);
    if (!std::strcmp(name, "map_catalog.retreat_audio_skip")) return reinterpret_cast<void*>(&AudioSkipTarget);
    return reinterpret_cast<void*>(&Dummy);
}

int HookCode(SdgoMod*, const char* name, void* detour, void** original) {
    if (!std::strcmp(name, "map_catalog.success_cleanup")) {
        g_successHook = detour;
        *original = reinterpret_cast<void*>(&FakeSuccessCleanup);
        return 1;
    }
    if (!std::strcmp(name, "map_catalog.mission_update")) {
        g_missionUpdateHook = detour;
        *original = reinterpret_cast<void*>(&FakeMissionUpdate);
        return 1;
    }
    if (!std::strcmp(name, "map_catalog.mission_call")) {
        *original = reinterpret_cast<void*>(&FakeMissionCall);
        return 1;
    }
    if (!std::strcmp(name, "map_catalog.tables_ready")) {
        *original = reinterpret_cast<void*>(&Dummy);
        return 1;
    }
    return 0;
}

int HookSite(SdgoMod*, const char* name, void* detour, void** original) {
    if (!std::strcmp(name, "map_catalog.retreat_fx")) {
        g_fxHook = detour;
        *original = reinterpret_cast<void*>(&FxNativeTarget);
        return 1;
    }
    if (!std::strcmp(name, "map_catalog.retreat_audio")) {
        g_audioHook = detour;
        *original = reinterpret_cast<void*>(&AudioNativeTarget);
        return 1;
    }
    if (!std::strcmp(name, "map_catalog.operator_mode")) {
        *original = reinterpret_cast<void*>(&PassiveSiteTarget);
        return 1;
    }
    return 0;
}

int SettingInt(SdgoMod*, const char*, int value) { return value; }
const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }
void Log(SdgoMod*, int, const char*, ...) {}

void ResetCounters() {
    g_originalCalls = g_missionUpdates = 0;
    g_fxNative = g_fxSkip = g_audioNative = g_audioSkip = 0;
    g_seenPacket = nullptr;
    g_seenOpcode = 0;
    g_lastLua.clear();
    g_nested = g_throw = g_mismatchedAudioMessage = false;
    g_preserveBridgeState = false;
    g_fxStatePreserved = g_audioStatePreserved = false;
    g_depth = 0;
}

using SuccessInvoke = int(__stdcall*)(const std::uint8_t*);

bool InvokeExpectingFixtureException(SuccessInvoke invoke, const std::uint8_t* packet) {
    bool raised = false;
    __try {
        invoke(packet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        raised = true;
    }
    return raised;
}
}

int main(int argc, char** argv) {
    assert(argc == 3);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto temp = std::filesystem::temp_directory_path() /
        std::filesystem::path(L"sdgo-map-retreat-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(temp);
    g_directory = temp.wstring();
    WriteCatalog(temp / L"catalog.bin");
    WriteRetreats(temp / L"retreats.bin");

    const HMODULE broker = LoadLibraryW(std::filesystem::path(argv[2]).c_str());
    assert(broker);
    const HMODULE module = LoadLibraryW(std::filesystem::path(argv[1]).c_str());
    assert(module);
    const auto load = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad"));
    assert(load);

    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.host_key = "GOnline-54BE1568-00C97000";
    api.Log = &Log;
    api.Symbol = &Symbol;
    api.HookCode = &HookCode;
    api.SettingInt = &SettingInt;
    api.ModDirectory = &Directory;
    api.HookSite = &HookSite;
    PrepareNativeState();
    assert(load(&api, reinterpret_cast<SdgoMod*>(1)) == 1);
    assert(g_successHook && g_missionUpdateHook && g_fxHook && g_audioHook);
    const auto getSuccess = reinterpret_cast<SdgoMapCatalogGetMissionSuccessV1Fn>(
        GetProcAddress(module, SDGO_MAP_MISSION_SUCCESS_EXPORT));
    assert(getSuccess);
    SdgoMapMissionSuccessV1 observed{};
    assert(getSuccess(&observed) == 0);
    observed.size = sizeof(observed);
    observed.version = SDGO_MAP_MISSION_SUCCESS_VERSION;
    assert(getSuccess(&observed) == 1 && observed.hook_active == 1 && observed.sequence == 0);
    const auto getBoundary = reinterpret_cast<SdgoMapCatalogGetLocalBoundaryReceiptV1Fn>(
        GetProcAddress(module, SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_EXPORT));
    assert(getBoundary);
    SdgoMapLocalBoundaryReceiptV1 boundary{};
    assert(getBoundary(&boundary) == 0);
    boundary.size = sizeof(boundary);
    boundary.version = SDGO_MAP_LOCAL_BOUNDARY_RECEIPT_VERSION;
    assert(getBoundary(&boundary) == 1 && boundary.hook_active == 0 && boundary.sequence == 0);

    const auto invoke = reinterpret_cast<int(__stdcall*)(const std::uint8_t*)>(g_successHook);
    const auto update = reinterpret_cast<void(__thiscall*)(void*)>(g_missionUpdateHook);

    ResetCounters();
    auto success = Packet(0x0905);
    const auto originalSuccess = success;
    g_preserveBridgeState = true;
    SetLastError(73);
    assert(invoke(success.data()) == 1);
    assert(GetLastError() == 73);
    assert(success == originalSuccess);
    assert(g_fxStatePreserved && g_audioStatePreserved);
    assert(g_seenPacket == success.data() && g_seenOpcode == 0x0905 && g_originalCalls == 1);
    assert(getSuccess(&observed) == 1 && observed.sequence == 1);
    assert(observed.mission_id == kMission && observed.phase == kPhase);
    assert(observed.clear_id == kClear && observed.event_order == kPhase);
    assert(observed.original_result == 1 && observed.count == 1 && observed.pilot_ids[0] == kPilot);
    assert(g_fxSkip == 1 && g_audioSkip == 1 && g_fxNative == 0 && g_audioNative == 0);
    update(g_mode.data());
    assert(g_missionUpdates == 1 && g_lastLua.find("0416.zfx") != std::string::npos);

    ResetCounters();
    auto ordinary = Packet(0x0570);
    const auto originalOrdinary = ordinary;
    g_preserveBridgeState = true;
    SetLastError(91);
    assert(invoke(ordinary.data()) == 1);
    assert(GetLastError() == 91);
    assert(ordinary == originalOrdinary);
    assert(g_fxStatePreserved && g_audioStatePreserved);
    assert(g_seenPacket == ordinary.data() && g_seenOpcode == 0x0570 && g_originalCalls == 1);
    assert(getSuccess(&observed) == 1 && observed.sequence == 1);
    assert(g_fxNative == 1 && g_audioNative == 1 && g_fxSkip == 0 && g_audioSkip == 0);
    update(g_mode.data());
    assert(g_lastLua.empty());

    ResetCounters();
    PrepareNativeState(0);
    assert(invoke(success.data()) == 1);
    assert(g_originalCalls == 1 && g_fxNative == 1 && g_audioNative == 1);
    assert(g_fxSkip == 0 && g_audioSkip == 0);
    update(g_mode.data());
    assert(g_lastLua.empty());

    ResetCounters();
    PrepareNativeState();
    std::uint8_t* noNode = nullptr;
    Put(g_unit.data(), 8, noNode);
    assert(invoke(success.data()) == 1);
    assert(g_originalCalls == 1 && g_fxSkip == 1 && g_audioSkip == 1);
    assert(g_fxNative == 0 && g_audioNative == 0);
    update(g_mode.data());
    assert(g_lastLua.empty());

    ResetCounters();
    PrepareNativeState();
    Put(g_mode.data(), 0x620, kPhase - 1);
    assert(invoke(success.data()) == 1);
    assert(g_originalCalls == 1 && g_fxNative == 1 && g_audioNative == 1);
    assert(g_fxSkip == 0 && g_audioSkip == 0);
    update(g_mode.data());
    assert(g_lastLua.empty());

    ResetCounters();
    PrepareNativeState();
    auto wrongPacketPhase = success;
    Put(wrongPacketPhase.data(), 10, kPhase - 1);
    assert(invoke(wrongPacketPhase.data()) == 1);
    assert(g_originalCalls == 1 && g_fxNative == 1 && g_audioNative == 1);
    assert(g_fxSkip == 0 && g_audioSkip == 0);

    ResetCounters();
    PrepareNativeState();
    Put(g_clear.data(), 45, kPhase - 1);
    assert(invoke(success.data()) == 1);
    assert(g_originalCalls == 1 && g_fxNative == 1 && g_audioNative == 1);
    assert(g_fxSkip == 0 && g_audioSkip == 0);

    ResetCounters();
    PrepareNativeState();
    auto wrongClear = success;
    Put(wrongClear.data(), 6, kClear + 1);
    assert(invoke(wrongClear.data()) == 1);
    assert(g_originalCalls == 1 && g_fxNative == 1 && g_audioNative == 1);
    assert(g_fxSkip == 0 && g_audioSkip == 0);

    ResetCounters();
    PrepareNativeState();
    g_mismatchedAudioMessage = true;
    assert(invoke(success.data()) == 1);
    assert(g_fxSkip == 1 && g_audioNative == 1 && g_audioSkip == 0);

    ResetCounters();
    g_nested = true;
    assert(invoke(success.data()) == 1);
    assert(g_originalCalls == 2);
    assert(g_fxNative == 1 && g_audioNative == 1 && g_fxSkip == 1 && g_audioSkip == 1);
    update(g_mode.data());
    assert(g_lastLua.find("0416.zfx") != std::string::npos);

    ResetCounters();
    g_throw = true;
    assert(InvokeExpectingFixtureException(invoke, success.data()));
    g_throw = false;
    NativeRemovalMessage outside{};
    InvokeSite(g_fxHook, g_unit.data(), &outside);
    InvokeSite(g_audioHook, g_unit.data(), &outside);
    assert(g_fxNative == 1 && g_audioNative == 1 && g_fxSkip == 0 && g_audioSkip == 0);

    FreeLibrary(module);
    FreeLibrary(broker);
    std::filesystem::remove_all(temp);
    return 0;
}
