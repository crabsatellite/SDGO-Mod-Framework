// Production client Mod fixture: real configuration/bundle loading, detour
// registration, x86 site bridge, full-set apply/restore and fail-closed
// preflight. It uses only owned memory and never starts the game.
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "sdgo/mission_behavior_profile.h"
#include "sdgo/sdgo_api.h"

#include "../client_policy.h"
#include "../observer.h"

namespace {
using namespace sdgo::mission_behavior;

struct FakeGrid {
    int marker = 1;
};

FakeGrid g_fakeGrid;
std::array<std::array<std::uint8_t, kNpcRecordBytes>, kM1Bindings.size()>
    g_records{};
std::wstring g_modDirectory;
SdgoApi g_fixtureApi{};
HMODULE g_module = nullptr;
SdgoModLoadFn g_modLoad = nullptr;
SdgoMissionBehaviorClientGetStatusV1Fn g_statusProvider = nullptr;
void* g_registerDetour = nullptr;
void* g_siteDetour = nullptr;
std::uint32_t g_nativeRegistrations = 0;
std::array<std::array<char, kNpcAiNameCharacters>, kM1Bindings.size()>
    g_registeredNames{};
int g_checks = 0;

void Check(bool value, const char* message) {
    ++g_checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(2);
    }
}

void ResetRecords(const char* ai) {
    for (std::size_t index = 0; index < g_records.size(); ++index) {
        auto& record = g_records[index];
        record.fill(0);
        std::memcpy(
            record.data(),
            &kM1Bindings[index].npc_id,
            sizeof(kM1Bindings[index].npc_id));
        Check(
            WriteAiName(record.data(), record.size(), ai),
            "fixture native AI field initializes");
    }
    g_nativeRegistrations = 0;
    for (auto& value : g_registeredNames) value.fill('\0');
}

std::size_t RecordIndex(std::uint32_t id) {
    for (std::size_t index = 0; index < kM1Bindings.size(); ++index)
        if (kM1Bindings[index].npc_id == id) return index;
    return kM1Bindings.size();
}

void* __cdecl FakeGridData() {
    return &g_fakeGrid;
}

bool __fastcall FakeGridFind(
    void* self,
    void*,
    const std::uint8_t** output,
    std::uint32_t id,
    int kind,
    char) {
    if (!output) return false;
    *output = nullptr;
    if (self != &g_fakeGrid ||
        kind != static_cast<int>(kNpcRecordKind))
        return false;
    const std::size_t index = RecordIndex(id);
    if (index == kM1Bindings.size()) return false;
    *output = g_records[index].data();
    return true;
}

void __fastcall FakeRegisterRecord(
    void* self,
    void*,
    const void* record,
    std::uint32_t bytes,
    std::uint32_t kind) {
    Check(self == &g_fakeGrid, "native registration keeps the live grid");
    Check(bytes == kNpcRecordBytes, "native registration keeps 598 bytes");
    Check(kind == kNpcRecordKind, "native registration keeps type 41");
    const auto* raw = static_cast<const std::uint8_t*>(record);
    const std::size_t index = RecordIndex(ReadU32(raw));
    Check(index != kM1Bindings.size(), "only the twelve exact records register");
    Check(
        ReadAiName(raw, bytes, g_registeredNames[index]),
        "registered record has a readable AI name");
    ++g_nativeRegistrations;
}

__declspec(naked) void ReturnFromSite() {
    __asm { ret }
}

void* FakeSymbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "grid.data"))
        return reinterpret_cast<void*>(&FakeGridData);
    if (!std::strcmp(name, "grid.find"))
        return reinterpret_cast<void*>(&FakeGridFind);
    if (!std::strcmp(name, "grid.register_record"))
        return reinterpret_cast<void*>(&FakeRegisterRecord);
    if (!std::strcmp(name, "mission_behavior.tables_loaded"))
        return reinterpret_cast<void*>(&ReturnFromSite);
    return nullptr;
}

int FakeHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original) {
    if (std::strcmp(name, "grid.register_record") ||
        !detour || !original)
        return 0;
    g_registerDetour = detour;
    *original = reinterpret_cast<void*>(&FakeRegisterRecord);
    return 1;
}

int FakeHookSite(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original) {
    if (std::strcmp(name, "mission_behavior.tables_loaded") ||
        !detour || !original)
        return 0;
    g_siteDetour = detour;
    *original = reinterpret_cast<void*>(&ReturnFromSite);
    return 1;
}

const wchar_t* FakeModDirectory(SdgoMod*) {
    return g_modDirectory.c_str();
}

void FakeLog(SdgoMod*, int, const char*, ...) {}

std::uint32_t FakeSettingString(
    SdgoMod*,
    const char*,
    const char* fallback,
    char* output,
    std::uint32_t size) {
    if (!output || !size) return 0;
    const std::size_t length = std::strlen(fallback);
    if (length + 1 > size) return size - 1;
    std::memcpy(output, fallback, length + 1);
    return static_cast<std::uint32_t>(length);
}

void WriteConfig(const char* profile) {
    std::ofstream output(
        std::filesystem::path(g_modDirectory) / L"config.ini",
        std::ios::binary | std::ios::trunc);
    output << "[settings]\r\nProfile=" << profile << "\r\n";
    Check(output.good(), "fixture config writes");
}

void WriteProfile() {
    Header header{};
    std::memcpy(header.magic, "SDGOMBP1", 8);
    header.version = kVersion;
    header.header_bytes = sizeof(Header);
    header.flags = kBossBindings | kNpcOverrides;
    header.entry_bytes = sizeof(NpcOverride);
    header.entry_count = 1;
    header.payload_bytes = sizeof(NpcOverride);
    header.bindings_generation = kBindingsGeneration;
    std::memcpy(header.label, "client fixture", 15);
    NpcOverride entry{};
    entry.mask = kChangeX;
    entry.baseline = {
        520001,88009,2,1,85063,
        100,0,200,5,0,0,2,1,0};
    entry.oc = entry.baseline;
    entry.oc.x = -100;
    header.payload_crc32 = Crc32(
        reinterpret_cast<const std::uint8_t*>(&entry),
        sizeof(entry));
    std::ofstream output(
        std::filesystem::path(g_modDirectory) / L"profile.bin",
        std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(&header),
        sizeof(header));
    output.write(
        reinterpret_cast<const char*>(&entry),
        sizeof(entry));
    Check(output.good(), "bounded synthetic owner profile writes");
}

SdgoApi Api() {
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &FakeLog;
    api.Symbol = &FakeSymbol;
    api.HookCode = &FakeHookCode;
    api.SettingString = &FakeSettingString;
    api.ModDirectory = &FakeModDirectory;
    api.HookSite = &FakeHookSite;
    return api;
}

using RegisterDetour = void(__fastcall*)(
    void*,
    void*,
    const void*,
    std::uint32_t,
    std::uint32_t);

void DeferEveryRecord() {
    Check(g_registerDetour != nullptr, "registration detour is captured");
    const auto detour =
        reinterpret_cast<RegisterDetour>(g_registerDetour);
    for (const auto& record : g_records)
        detour(
            &g_fakeGrid,
            nullptr,
            record.data(),
            static_cast<std::uint32_t>(record.size()),
            kNpcRecordKind);
    Check(
        g_nativeRegistrations == 0,
        "all twelve registrations remain deferred until completion");
}

void InvokeLoadedSite() {
    Check(g_siteDetour != nullptr, "whole-table site detour is captured");
    void* detour = g_siteDetour;
    void* grid = &g_fakeGrid;
    __asm {
        mov esi, grid
        call detour
    }
}

std::string Ai(std::size_t index) {
    std::array<char, kNpcAiNameCharacters> output{};
    Check(
        ReadAiName(g_records[index].data(), kNpcRecordBytes, output),
        "fixture AI field reads");
    return output.data();
}

SdgoMissionBehaviorClientStatusV1 Status() {
    SdgoMissionBehaviorClientStatusV1 status{};
    status.size = sizeof(status);
    Check(
        g_statusProvider && g_statusProvider(&status) != 0,
        "observer status reads");
    return status;
}

void Load(const char* profile, bool withBundle) {
    WriteConfig(profile);
    const auto bundle =
        std::filesystem::path(g_modDirectory) / L"profile.bin";
    std::filesystem::remove(bundle);
    if (withBundle) WriteProfile();
    g_registerDetour = nullptr;
    g_siteDetour = nullptr;
    g_fixtureApi = Api();
    Check(
        g_modLoad &&
            g_modLoad(&g_fixtureApi, reinterpret_cast<SdgoMod*>(1)) != 0,
        "production client Mod activates");
}
} // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "runtime test receives the production DLL path");
    g_module = LoadLibraryA(argv[1]);
    Check(g_module != nullptr, "production client DLL loads");
    g_modLoad = reinterpret_cast<SdgoModLoadFn>(
        GetProcAddress(g_module, "SdgoModLoad"));
    g_statusProvider =
        reinterpret_cast<SdgoMissionBehaviorClientGetStatusV1Fn>(
            GetProcAddress(
                g_module,
                SDGO_MISSION_BEHAVIOR_CLIENT_STATUS_EXPORT));
    Check(
        g_modLoad && g_statusProvider,
        "production Mod and observer exports resolve");

    const auto root =
        std::filesystem::temp_directory_path() /
        ("sdgo-mission-behavior-client-" +
         std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    g_modDirectory = root.wstring();

    Load("oc", true);
    auto status = Status();
    Check(
        status.requested_profile ==
            SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_OC &&
            status.profile_data_valid == 1 &&
            status.hook_installed == 1,
        "OC startup reports a valid owner profile and both hooks");

    ResetRecords(kGcoM1BossAi);
    DeferEveryRecord();
    SetLastError(0x1234);
    InvokeLoadedSite();
    Check(
        GetLastError() == 0x1234,
        "x86 completion bridge preserves Win32 last error");
    Check(
        g_nativeRegistrations == kM1Bindings.size(),
        "every final OC record receives native integrity registration");
    for (std::size_t index = 0; index < g_records.size(); ++index) {
        Check(
            Ai(index) == kM1Bindings[index].oc_ai,
            "OC profile writes the exact per-ID binding");
        Check(
            std::strcmp(
                g_registeredNames[index].data(),
                kM1Bindings[index].oc_ai) == 0,
            "native registration observes the final OC bytes");
    }
    status = Status();
    Check(
        status.active_profile ==
            SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_OC &&
            status.observed_records == 12 &&
            status.applied_records == 12 &&
            status.conflict_records == 0,
        "observer reports the complete OC transaction");

    Load("gco", true);
    ResetRecords("MM001_NpcBoss001");
    for (std::size_t index : {std::size_t{3}, std::size_t{4}})
        Check(
            WriteAiName(
                g_records[index].data(),
                g_records[index].size(),
                "MM001_NpcBoss002"),
            "Boss002 OC fixture writes");
    DeferEveryRecord();
    InvokeLoadedSite();
    for (std::size_t index = 0; index < g_records.size(); ++index)
        Check(
            Ai(index) == kGcoM1BossAi,
            "GCO profile restores the current client binding");
    status = Status();
    Check(
        status.active_profile ==
            SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_GCO &&
            status.restored_records == 12 &&
            status.conflict_records == 0,
        "observer reports the complete GCO restore");

    Load("oc", true);
    ResetRecords(kGcoM1BossAi);
    Check(
        WriteAiName(
            g_records[7].data(),
            g_records[7].size(),
            "Foreign_Mission_AI"),
        "foreign binding fixture writes");
    const auto beforeConflict = g_records;
    DeferEveryRecord();
    InvokeLoadedSite();
    Check(
        g_records == beforeConflict,
        "one foreign binding leaves the entire native set byte-identical");
    Check(
        g_nativeRegistrations == kM1Bindings.size(),
        "refused transaction still registers every original native record");
    status = Status();
    Check(
        status.active_profile ==
            SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_UNAVAILABLE &&
            status.applied_records == 0 &&
            status.conflict_records != 0,
        "observer reports whole-set refusal without a partial OC claim");

    Load("gco", false);
    ResetRecords(kGcoM1BossAi);
    DeferEveryRecord();
    InvokeLoadedSite();
    status = Status();
    Check(
        status.profile_data_valid == 0 &&
            status.active_profile ==
                SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_GCO &&
            status.unchanged_records == 12,
        "GCO remains usable without an owner profile bundle");

    Load("oc", false);
    ResetRecords(kGcoM1BossAi);
    const auto beforeMissing = g_records;
    DeferEveryRecord();
    InvokeLoadedSite();
    Check(
        g_records == beforeMissing,
        "missing OC profile retains every original client byte");
    status = Status();
    Check(
        status.profile_data_valid == 0 &&
            status.active_profile ==
                SDGO_MISSION_BEHAVIOR_CLIENT_PROFILE_UNAVAILABLE &&
            status.applied_records == 0,
        "missing OC profile is reported unavailable");

    std::filesystem::remove_all(root);
    FreeLibrary(g_module);
    std::printf(
        "mission behavior client runtime passed: %d checks, "
        "real production hooks and observer ABI\n",
        g_checks);
    return 0;
}
