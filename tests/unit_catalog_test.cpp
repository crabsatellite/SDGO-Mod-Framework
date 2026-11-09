#include <windows.h>
#include <bcrypt.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "sdgo/content_localization_boundary.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"
#include "../mods/unit_catalog/content_localization_boundary_index.h"

#pragma comment(lib, "bcrypt.lib")

using namespace sdgo::unit_catalog;
namespace fs = std::filesystem;

namespace {
int g_checks = 0;
std::wstring g_modDirectory;
uint8_t g_grid[0x58]{};

struct NativeValue {
    uint32_t type;
    uint8_t* record;
};
struct NativeNode {
    void* left;
    void* parent;
    void* right;
    uint32_t key;
    NativeValue* value;
    uint32_t flags;
};
struct NativePair {
    uint32_t key;
    NativeValue* value;
};
struct NativeInsert {
    void* node;
    uint8_t inserted;
    uint8_t padding[3];
};

NativeNode g_head{};
std::map<uint32_t, NativeNode*> g_nodes;
std::map<uint32_t, std::vector<uint8_t>> g_nativeUnits;
std::map<uint32_t, std::vector<uint8_t>> g_nativeWeapons;
std::set<uint32_t> g_foreignKeys;
std::set<void*> g_allocations;
uint32_t g_failInsertKey = 0;
void* g_loadDetour = nullptr;
void* g_openDetour = nullptr;
int g_openHooks = 0;
int g_loadCalls = 0;
int g_integrityRecords = 0;
fs::path g_diagnosticPath;
constexpr DWORD kPartialPublishExitCode = 0xE0C00101u;

void Check(bool value, const char* text) {
    ++g_checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        std::exit(2);
    }
}

void PutUtf16(uint8_t* destination, size_t bytes, const char* ascii) {
    std::memset(destination, 0, bytes);
    const size_t count = std::strlen(ascii);
    Check((count + 1) * 2 <= bytes, "UTF-16 fixture fits");
    for (size_t i = 0; i < count; ++i) destination[i * 2] = static_cast<uint8_t>(ascii[i]);
}

Entry MakeRecord(uint32_t kind, uint32_t id) {
    Entry result{};
    result.kind = kind;
    result.id = id;
    result.bytes.resize(ExpectedBytes(kind));
    Put32(result.bytes.data(), id);
    result.bytes[4] = 0;
    if (kind == kUnitType) {
        PutUtf16(result.bytes.data() + 5, 100, "Fixture Unit Name");
        PutUtf16(result.bytes.data() + 105, 100, "Fixture Unit Detail");
        PutUtf16(result.bytes.data() + 205, 100,
                 "Fixture Unit\\nDisplay\\tMode");
        PutUtf16(result.bytes.data() + kUnitModelPathOffset, kUnitModelPathBytes, "mdrs\\fixture.mod");
        PutUtf16(result.bytes.data() + kUnitPreviewPathOffsets[0], kUnitPreviewPathBytes, "sprs\\shop.spr");
        PutUtf16(result.bytes.data() + kUnitPreviewPathOffsets[1], kUnitPreviewPathBytes, "sprs\\capsule.spr");
        PutUtf16(result.bytes.data() + kUnitPreviewPathOffsets[2], kUnitPreviewPathBytes, "sprs\\room.spr");
        Put32(result.bytes.data() + kUnitTransformOffset, 0);
        Put32(result.bytes.data() + kUnitWeaponOffsets[0], 61165);
    } else if (kind == kWeaponType) {
        // A native weapon row has one text field, its name. The bytes after it are
        // numeric data (special feature at 132) and effect paths (from 164).
        PutUtf16(result.bytes.data() + 5, 100, "Fixture Weapon Name");
    } else if (kind == kSkillType) {
        PutUtf16(result.bytes.data()+5,100,"Fixture Skill");
        PutUtf16(result.bytes.data()+0x213,600,
                 "Fixture Skill\\nDescription\\tTabbed");
    } else if (kind == kSpecialFeatureType) {
        PutUtf16(result.bytes.data()+5,100,"Fixture Feature");
    } else if (kind == kItemCategoryType) {
        PutUtf16(result.bytes.data() + kItemCategoryNameOffset,
                 kItemCategoryNameBytes, "Fixture Item Category");
        Put32(result.bytes.data() + kItemCategoryRefOffset,
              id / kItemCategoryClassDivisor == kBlueprintItemClass ? 200001u : 25060u);
    } else if (kind == kBlueprintType) {
        PutUtf16(result.bytes.data() + 5, 100, "Fixture Blueprint");
        Put32(result.bytes.data() + kBlueprintMaterialCountOffset, 3);
        Put32(result.bytes.data() + kBlueprintOutputOffset, 25060);
        const uint32_t materials[] = {25061, 10001, 10002};
        const uint32_t levels[] = {7, 1, 1};
        for (size_t slot = 0; slot < std::size(materials); ++slot) {
            Put32(result.bytes.data() + kBlueprintMaterialOffset +
                      slot * sizeof(uint32_t), materials[slot]);
            Put32(result.bytes.data() + kBlueprintMaterialLevelOffset +
                      slot * sizeof(uint32_t), levels[slot]);
        }
    } else if (kind == kUnitIndexType) {
        result.bytes[5]=1;
        Put32(result.bytes.data()+kUnitIndexRefOffset,25060);
        Put32(result.bytes.data()+kUnitIndexBlueprintRefOffset,200001);
        PutUtf16(result.bytes.data()+14,100,"sprs\\owned.spr");
        PutUtf16(result.bytes.data()+114,100,"sprs\\missing.spr");
    }
    result.recordHash = Hash32(result.bytes.data(), result.bytes.size());
    return result;
}

std::vector<uint8_t> Encode(std::vector<Entry> entries) {
    size_t payloadBytes = 0;
    for (const auto& entry : entries) payloadBytes += sizeof(EntryHeader) + entry.bytes.size();
    std::vector<uint8_t> output(sizeof(Header) + payloadBytes);
    size_t cursor = sizeof(Header);
    for (const auto& entry : entries) {
        EntryHeader native{entry.kind, entry.id, static_cast<uint32_t>(entry.bytes.size()),
                           Hash32(entry.bytes.data(), entry.bytes.size()), 0, 0};
        std::memcpy(output.data() + cursor, &native, sizeof(native));
        cursor += sizeof(native);
        std::memcpy(output.data() + cursor, entry.bytes.data(), entry.bytes.size());
        cursor += entry.bytes.size();
    }
    Header header{};
    std::memcpy(header.magic, "SDGOUN1", 8);
    header.version = kFormatVersion;
    header.headerBytes = sizeof(Header);
    header.count = static_cast<uint32_t>(entries.size());
    header.payloadBytes = static_cast<uint32_t>(payloadBytes);
    header.payloadHash = Hash32(output.data() + sizeof(Header), payloadBytes);
    std::memcpy(output.data(), &header, sizeof(header));
    return output;
}

void* __cdecl NativeAllocate(size_t size) {
    void* value = std::malloc(size);
    if (value) g_allocations.insert(value);
    return value;
}

void __cdecl NativeRelease(void* value) {
    if (!value) return;
    Check(g_allocations.erase(value) == 1, "native allocation released exactly once");
    std::free(value);
}

void* __cdecl NativeGrid() {
    return g_grid;
}

bool __fastcall NativeFind(void*, void*, const uint8_t** out, uint32_t id, int type, char) {
    if (!out) return false;
    if (type == static_cast<int>(kUnitType) && g_nativeUnits.count(id)) {
        *out = g_nativeUnits.at(id).data();
        return true;
    }
    if (type == static_cast<int>(kWeaponType) && g_nativeWeapons.count(id)) {
        *out = g_nativeWeapons.at(id).data();
        return true;
    }
    const auto found = g_nodes.find(id);
    if (found != g_nodes.end() && found->second->value->type == static_cast<uint32_t>(type)) {
        *out = found->second->value->record;
        return true;
    }
    return false;
}

void __fastcall NativeMapFind(void*, void*, void** out, const uint32_t* key) {
    const auto found = g_nodes.find(*key);
    if (found != g_nodes.end()) {
        *out = found->second;
    } else if (g_foreignKeys.count(*key)) {
        *out = reinterpret_cast<void*>(static_cast<uintptr_t>(*key) + 0x10000u);
    } else {
        *out = &g_head;
    }
}

void* __fastcall NativeMake(void*, void*, const NativePair* pair) {
    auto* node = static_cast<NativeNode*>(NativeAllocate(sizeof(NativeNode)));
    if (node) {
        *node = {};
        node->key = pair->key;
        node->value = pair->value;
    }
    return node;
}

void __fastcall NativeInsertNode(void*, void*, NativeInsert* result, void* raw, char) {
    auto* node = static_cast<NativeNode*>(raw);
    if (node->key == g_failInsertKey) {
        result->node = &g_head;
        result->inserted = 0;
        NativeRelease(node);
        return;
    }
    const bool inserted = g_nodes.emplace(node->key, node).second;
    result->node = inserted ? node : g_nodes.at(node->key);
    result->inserted = inserted ? 1 : 0;
    if (!inserted) NativeRelease(node);
}

void __fastcall NativeRegister(void*, void*, const void* raw, uint32_t size, uint32_t type) {
    Check(raw != nullptr && size == ExpectedBytes(type), "integrity registration uses the full typed record");
    ++g_integrityRecords;
}

void __fastcall NativeLoad(void*, void*, const wchar_t*, uint32_t) {
    ++g_loadCalls;
}

void* __fastcall NativeOpen(void*, void*, const void*, bool) {
    return nullptr;
}

void* __fastcall NativeFileCtor(void*, void*, void*, void*, uint32_t, bool) {
    return nullptr;
}

void* __cdecl NativeWfopen(const wchar_t*, const wchar_t*) {
    return nullptr;
}

void Log(SdgoMod*, int, const char* format, ...) {
    if (g_diagnosticPath.empty()) return;
    char text[2048]{};
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
    va_end(arguments);
    const HANDLE stream = CreateFileW(g_diagnosticPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stream == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(stream, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    WriteFile(stream, "\r\n", 2, &written, nullptr);
    CloseHandle(stream);
}

void* Symbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "grid.data")) return reinterpret_cast<void*>(&NativeGrid);
    if (!std::strcmp(name, "grid.find")) return reinterpret_cast<void*>(&NativeFind);
    if (!std::strcmp(name, "grid.make_node")) return reinterpret_cast<void*>(&NativeMake);
    if (!std::strcmp(name, "grid.insert_node")) return reinterpret_cast<void*>(&NativeInsertNode);
    if (!std::strcmp(name, "grid.register_record")) return reinterpret_cast<void*>(&NativeRegister);
    if (!std::strcmp(name, "grid.map_find")) return reinterpret_cast<void*>(&NativeMapFind);
    if (!std::strcmp(name, "crt.operator_new")) return reinterpret_cast<void*>(&NativeAllocate);
    if (!std::strcmp(name, "crt.operator_delete")) return reinterpret_cast<void*>(&NativeRelease);
    if (!std::strcmp(name, "capsule.main.file_ctor")) return reinterpret_cast<void*>(&NativeFileCtor);
    if (!std::strcmp(name, "capsule.main.operator_new")) return reinterpret_cast<void*>(&NativeAllocate);
    if (!std::strcmp(name, "capsule.main.wfopen")) return reinterpret_cast<void*>(&NativeWfopen);
    return nullptr;
}

int HookCode(SdgoMod*, const char* name, void* detour, void** original) {
    if (!std::strcmp(name, "unit_catalog.load_last_table")) {
        g_loadDetour = detour;
        *original = reinterpret_cast<void*>(&NativeLoad);
        return 1;
    }
    if (!std::strcmp(name, "capsule.main.open_file")) {
        g_openDetour = detour;
        ++g_openHooks;
        *original = reinterpret_cast<void*>(&NativeOpen);
        return 1;
    }
    return 0;
}

int SettingInt(SdgoMod*, const char*, int) {
    return 1;
}

const wchar_t* ModDirectory(SdgoMod*) {
    return g_modDirectory.c_str();
}

void CleanupNativeTree() {
    for (const auto& [id, node] : g_nodes) {
        (void)id;
        NativeRelease(node->value->record);
        NativeRelease(node->value);
        NativeRelease(node);
    }
    g_nodes.clear();
    Check(g_allocations.empty(), "native fixture cleanup releases all game-owned allocations");
}

void PutU32(std::vector<uint8_t>& bytes, uint32_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + 4);
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE state = nullptr;
    DWORD stateBytes = 0;
    DWORD hashBytes = 0;
    DWORD received = 0;
    Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
          "SHA-256 provider opened");
    Check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&stateBytes), sizeof(stateBytes),
                            &received, 0) >= 0,
          "SHA-256 state size read");
    Check(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                            reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                            &received, 0) >= 0 && hashBytes == 32,
          "SHA-256 digest size read");
    std::vector<uint8_t> storage(stateBytes);
    std::vector<uint8_t> digest(hashBytes);
    Check(BCryptCreateHash(algorithm, &state, storage.data(), stateBytes,
                           nullptr, 0, 0) >= 0,
          "SHA-256 state created");
    Check(bytes.empty() ||
          BCryptHashData(state, const_cast<PUCHAR>(bytes.data()),
                         static_cast<ULONG>(bytes.size()), 0) >= 0,
          "SHA-256 bytes accepted");
    Check(BCryptFinishHash(state, digest.data(), hashBytes, 0) >= 0,
          "SHA-256 digest finished");
    BCryptDestroyHash(state);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = digits[digest[index] >> 4];
        result[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    return result;
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(stream), "sealed overlay fixture written");
}

void WriteOverlayFixture(const fs::path& root) {
    std::vector<uint8_t> archive(520, 0);
    uint32_t next = 1;
    std::memcpy(archive.data(), &next, sizeof(next));
    archive[8] = 0x5a;

    std::vector<uint8_t> index;
    for (uint32_t value : {1u, 512u, 1u, 0u, 1u, 0u}) PutU32(index, value);
    for (const wchar_t value : std::wstring(L"mdrs\\fixture.mod\0", 17)) {
        const uint16_t encoded = static_cast<uint16_t>(value) ^ 0x00aa;
        const size_t offset = index.size();
        index.resize(offset + 2);
        std::memcpy(index.data() + offset, &encoded, sizeof(encoded));
    }
    for (uint32_t value : {1u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u})
        PutU32(index, value);
    WriteBytes(root / "resources.zdx", index);
    WriteBytes(root / "resources.zpk", archive);
    std::ofstream ini(root / "resource-overlay.ini", std::ios::binary | std::ios::trunc);
    ini << "[runtime]\r\nschema=1\r\nindex=resources.zdx\r\nindex_sha256="
        << Sha256(index) << "\r\narchive=resources.zpk\r\narchive_sha256="
        << Sha256(archive) << "\r\n";
    Check(static_cast<bool>(ini), "sealed overlay manifest written");
}

std::vector<uint8_t> MakeNativeFixture(const std::string& scenario) {
    Entry first = MakeRecord(kUnitType, 25060);
    Entry second = MakeRecord(kUnitType, 25061);
    Entry weapon = MakeRecord(kWeaponType, 61165);
    Entry blueprint = MakeRecord(kBlueprintType, 200001);
    Entry category = MakeRecord(kItemCategoryType, 1250600);
    Entry blueprintCategory = MakeRecord(kItemCategoryType, 1520001);
    Entry index = MakeRecord(kUnitIndexType,kUnitIndexFirstId);
    if(scenario=="index_reference_missing")Put32(index.bytes.data()+kUnitIndexRefOffset,999998);
    if(scenario=="index_blueprint_reference_missing")
        Put32(index.bytes.data()+kUnitIndexBlueprintRefOffset,999998);
    if(scenario=="index_blueprint_output_mismatch")
        Put32(index.bytes.data()+kUnitIndexRefOffset,25061);
    if(scenario=="blueprint_output_missing")
        Put32(blueprint.bytes.data()+kBlueprintOutputOffset,999998);
    if(scenario=="blueprint_material_missing")
        Put32(blueprint.bytes.data()+kBlueprintMaterialOffset,999998);
    if(scenario=="blueprint_item_reference_missing")
        Put32(blueprintCategory.bytes.data()+kItemCategoryRefOffset,999998);
    if(scenario=="skill_reference_missing")Put32(first.bytes.data()+kUnitSkillOffsets[0],999998);
    if(scenario=="funnel_reference_missing")Put32(weapon.bytes.data()+kWeaponFunnelOffset,999998);
    if(scenario=="special_feature_reference_missing")
        Put32(weapon.bytes.data()+kWeaponSpecialFeatureOffset,999998);
    if(scenario=="dependency_records_success" || scenario=="linked_skill_reference_missing") {
        Entry skill=MakeRecord(kSkillType,9000001);
        Entry linkedSkill=MakeRecord(kSkillType,9000002);
        Entry funnel=MakeRecord(kFunnelType,198131);
        Entry feature=MakeRecord(kSpecialFeatureType,59051);
        Put32(first.bytes.data()+kUnitSkillOffsets[0],skill.id);
        Put32(weapon.bytes.data()+kWeaponFunnelOffset,funnel.id);
        Put32(weapon.bytes.data()+kWeaponSpecialFeatureOffset,feature.id);
        Put32(skill.bytes.data()+kSkillLinkedSkillOffset,linkedSkill.id);
        std::vector<Entry> entries={first,second,feature,weapon,funnel,blueprint,category,blueprintCategory,index,skill};
        if(scenario=="dependency_records_success")entries.push_back(linkedSkill);
        return Encode(entries);
    }
    Put32(first.bytes.data() + kUnitTransformOffset, second.id);
    Put32(second.bytes.data() + kUnitTransformOffset, first.id);
    Put32(first.bytes.data() + kUnitWeaponOffsets[0], weapon.id);
    Put32(second.bytes.data() + kUnitWeaponOffsets[0],
          scenario == "missing_reference" ? 999999u : 45082u);
    Put32(category.bytes.data() + kItemCategoryRefOffset, first.id);
    if (scenario == "conflict")
        std::memset(first.bytes.data() + 205, 0x41, 100);
    for (Entry* entry : {&first, &second, &weapon, &blueprint, &category,
                         &blueprintCategory, &index})
        entry->recordHash = Hash32(entry->bytes.data(), entry->bytes.size());
    if (scenario == "large_catalog") {
        // Fill the container to the exact runtime record limit, well past the former 4096 bound.
        std::vector<Entry> entries{first, second, weapon, blueprint, category, blueprintCategory, index};
        for (uint32_t id = 300001; entries.size() < kMaxRecords; ++id)
            entries.push_back(MakeRecord(kFunnelType, id));
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& left, const Entry& right) { return left.id < right.id; });
        return Encode(entries);
    }
    return Encode({first, second, weapon, blueprint, category, blueprintCategory, index});
}

void NativeFixture(const fs::path& dll, const std::string& scenario,
                   const fs::path& suppliedRoot = {}) {
    Check(scenario == "success" || scenario == "conflict" ||
          scenario == "missing_reference" || scenario == "partial_failure_child" ||
          scenario=="index_reference_missing" || scenario=="index_conflict" ||
          scenario=="blueprint_output_missing" || scenario=="skill_reference_missing" ||
          scenario=="funnel_reference_missing" || scenario=="dependency_records_success" ||
          scenario=="special_feature_reference_missing" ||
          scenario=="linked_skill_reference_missing" ||
          scenario=="blueprint_material_missing" ||
          scenario=="blueprint_item_reference_missing" ||
          scenario=="index_blueprint_reference_missing" ||
          scenario=="index_blueprint_output_mismatch" || scenario=="large_catalog",
          "native fixture scenario is supported");
    g_nodes.clear();
    g_nativeUnits.clear();
    g_nativeWeapons.clear();
    g_foreignKeys.clear();
    g_allocations.clear();
    g_failInsertKey = scenario == "partial_failure_child" ? 25061u : 0u;
    g_loadDetour = nullptr;
    g_openDetour = nullptr;
    g_openHooks = 0;
    g_loadCalls = 0;
    g_integrityRecords = 0;
    *reinterpret_cast<void**>(g_grid + 4) = &g_head;
    g_nativeWeapons[45082] = std::vector<uint8_t>(kWeaponBytes);
    Put32(g_nativeWeapons[45082].data(), 45082);
    for (const uint32_t unit : {10001u, 10002u}) {
        g_nativeUnits[unit] = std::vector<uint8_t>(kUnitBytes);
        Put32(g_nativeUnits[unit].data(), unit);
    }
    if (scenario == "conflict") g_foreignKeys.insert(25060);
    if (scenario == "index_conflict")g_foreignKeys.insert(kUnitIndexFirstId);

    const fs::path root = suppliedRoot.empty()
        ? fs::temp_directory_path() /
              ("sdgo-unit-catalog-native-" + std::to_string(GetCurrentProcessId()))
        : suppliedRoot;
    fs::create_directories(root);
    g_diagnosticPath = suppliedRoot.empty() ? fs::path{} : root / "unit_catalog.log";
    g_modDirectory = root.wstring();
    {
        const auto bytes = MakeNativeFixture(scenario);
        std::ofstream file(root / "catalog.bin", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Check(static_cast<bool>(file), "native fixture catalog written");
    }
    WriteOverlayFixture(root);

    const HMODULE module = LoadLibraryW(dll.c_str());
    Check(module != nullptr, "production unit_catalog DLL loaded");
    const auto localizationBoundary =
        reinterpret_cast<SdgoUnitCatalogLocalizationBoundaryV1Fn>(
            GetProcAddress(
                module,
                SDGO_UNIT_CATALOG_LOCALIZATION_BOUNDARY_V1_EXPORT));
    Check(localizationBoundary != nullptr,
          "production unit_catalog exports the optional localization boundary");
    Check(localizationBoundary(kUnitType, 25060, nullptr, 0) ==
              SDGO_CONTENT_LOCALIZATION_INDETERMINATE &&
          sdgo::content_localization_boundary::Query(
              kUnitType, 25060, nullptr, 0) ==
              SDGO_CONTENT_LOCALIZATION_INDETERMINATE,
          "loaded provider remains indeterminate before validated catalog initialization");
    const auto load = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad"));
    Check(load != nullptr, "production unit_catalog entry found");
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &Log;
    api.Symbol = &Symbol;
    api.HookCode = &HookCode;
    api.SettingInt = &SettingInt;
    api.ModDirectory = &ModDirectory;
    Check(load(&api, reinterpret_cast<SdgoMod*>(1)) != 0, "production unit_catalog accepts native fixture");
    if (scenario == "conflict") {
        Check(localizationBoundary(kUnitType, 25060, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE &&
              sdgo::content_localization_boundary::Query(
                  0, 0, L"Fixture Weapon Name", 19) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE,
              "invalid protected text leaves ownership indeterminate without blocking catalog load");
    } else {
        Check(localizationBoundary(kUnitType, 25060, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED &&
              localizationBoundary(kWeaponType, 61165, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED &&
              localizationBoundary(kItemCategoryType, 1250600, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED,
              "validated current-package record identities are protected before native publication");
        Check(localizationBoundary(0, 0, L"Fixture Unit Name", 17) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED &&
              localizationBoundary(kUnitType, 999999,
                                   L"Fixture Unit Name", 17) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED &&
              localizationBoundary(0, 0,
                                   L"Fixture Unit\nDisplay\tMode", 25) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED &&
              localizationBoundary(0, 0, L"Fixture Item Category", 21) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED,
              "raw and display-expanded current-package text is protected exactly");
        Check(localizationBoundary(kUnitType, 999999, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_STOCK_OR_UNKNOWN &&
              localizationBoundary(0, 0, L"fixture unit name", 17) ==
                  SDGO_CONTENT_LOCALIZATION_STOCK_OR_UNKNOWN &&
              localizationBoundary(0, 0, L" Fixture Unit Name", 18) ==
                  SDGO_CONTENT_LOCALIZATION_STOCK_OR_UNKNOWN,
              "unknown identity, case changes and whitespace changes remain unowned");
        const wchar_t embeddedNull[] = {L'F', L'\0', L'X'};
        Check(localizationBoundary(kUnitType, 0, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE &&
              localizationBoundary(99, 1, nullptr, 0) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE &&
              localizationBoundary(0, 0, embeddedNull, 3) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE &&
              localizationBoundary(0, 0,
                                   reinterpret_cast<const wchar_t*>(1), 1) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE &&
              localizationBoundary(
                  0, 0, L"x",
                  SDGO_CONTENT_LOCALIZATION_MAX_SOURCE_CHARS + 1) ==
                  SDGO_CONTENT_LOCALIZATION_INDETERMINATE,
              "invalid, unbounded and unreadable queries fail closed without crashing");
        Check(sdgo::content_localization_boundary::Query(
                  0, 0, L"Fixture Weapon Name", 19) ==
                  SDGO_CONTENT_LOCALIZATION_OWNED,
              "shared optional helper reaches the initialized provider without loading it");
        if (scenario == "dependency_records_success") {
            Check(localizationBoundary(kSkillType, 9000001, nullptr, 0) ==
                      SDGO_CONTENT_LOCALIZATION_OWNED &&
                  localizationBoundary(
                      0, 0, L"Fixture Skill\nDescription\tTabbed", 32) ==
                      SDGO_CONTENT_LOCALIZATION_OWNED,
                  "actual DLL protects imported kind-13 identity and display-expanded description");
        }
    }
    Check(g_loadDetour != nullptr, "final native loader detour registered");
    Check(g_openDetour != nullptr && g_openHooks == 1,
          "standalone unit provider owns exactly one native resource hook");

    using Detour = void(__fastcall*)(void*, void*, const wchar_t*, uint32_t);
    reinterpret_cast<Detour>(g_loadDetour)(g_grid, nullptr, L"gdrs\\0279.gdt", 0x0d);
    Check(g_loadCalls == 1 && g_nodes.empty() && !g_integrityRecords,
          "catalog waits for the exact final native table");
    reinterpret_cast<Detour>(g_loadDetour)(g_grid, nullptr, L"gdrs\\0280.gdt", 0x0d);
    Check(g_loadCalls == 2, "original native table loader called before both detours");
    if (scenario == "success" || scenario=="dependency_records_success" || scenario=="large_catalog") {
        const size_t expected=scenario=="dependency_records_success" ? 11u:
                              scenario=="large_catalog" ? static_cast<size_t>(kMaxRecords) : 7u;
        Check(g_nodes.size() == expected && static_cast<size_t>(g_integrityRecords) == expected,
               "all unit, weapon, blueprint, item-category and encyclopedia records entered the native tree");
        Catalog parsed;
        std::string error;
        Check(parsed.Parse(MakeNativeFixture(scenario), error),
              "native fixture parses independently before byte readback");
        for (const auto& entry : parsed.records) {
            const uint8_t* found = nullptr;
            Check(NativeFind(nullptr, nullptr, &found, entry.id, static_cast<int>(entry.kind), 0) &&
                  found && !std::memcmp(found, entry.bytes.data(), entry.bytes.size()),
                  "native readback preserves every raw byte");
        }
        reinterpret_cast<Detour>(g_loadDetour)(g_grid, nullptr, L"0280.gdt", 0x0d);
        Check(g_loadCalls == 3 && g_nodes.size() == expected && static_cast<size_t>(g_integrityRecords) == expected,
              "catalog publication runs only once after the original loader");
        CleanupNativeTree();
    } else {
        Check(g_nodes.empty() && !g_integrityRecords && g_allocations.empty(),
              "preflight failure publishes no native records or allocations");
    }
    Check(FreeLibrary(module) != 0, "production unit_catalog DLL unloaded");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

std::wstring Quote(const fs::path& path) {
    const std::wstring value = path.wstring();
    Check(value.find(L'"') == std::wstring::npos, "fixture command path has no quote");
    return L"\"" + value + L"\"";
}

void FailStopFixture(const fs::path& dll) {
    wchar_t executableBuffer[32768]{};
    constexpr DWORD executableCapacity = static_cast<DWORD>(std::size(executableBuffer));
    const DWORD length = GetModuleFileNameW(nullptr, executableBuffer, executableCapacity);
    Check(length && length < executableCapacity, "fixture executable path resolved");
    const fs::path executable(executableBuffer);
    const fs::path root = fs::temp_directory_path() /
        ("sdgo-unit-catalog-failstop-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    std::wstring command = Quote(executable) + L" " + Quote(dll) +
                           L" partial_failure_child " + Quote(root);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        fs::remove_all(root, ignored);
        Check(false, "partial-publication fixture child created");
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 0;
    const BOOL gotExitCode = GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::string diagnostic;
    {
        std::ifstream stream(root / "unit_catalog.log", std::ios::binary);
        diagnostic.assign(std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>());
    }
    fs::remove_all(root, ignored);
    Check(wait == WAIT_OBJECT_0, "partial-publication fixture child stopped without timeout");
    Check(gotExitCode && exitCode == kPartialPublishExitCode,
          "partial-publication fixture child used the fail-stop exit code");
    Check(diagnostic.find("failed after 1 records were published") != std::string::npos &&
          diagnostic.find("terminating the client") != std::string::npos,
          "partial-publication fixture persisted a diagnostic before fail-stop");
}

void OldCapsuleFixture(const fs::path& unitDll, const fs::path& oldCapsuleDll) {
    const fs::path root = fs::temp_directory_path() /
                          ("sdgo-unit-old-capsule-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    g_modDirectory = root.wstring();
    g_openDetour = nullptr;
    g_openHooks = 0;
    {
        const auto bytes = MakeNativeFixture("success");
        std::ofstream file(root / "catalog.bin", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        Check(static_cast<bool>(file), "old-provider catalog fixture written");
    }
    WriteOverlayFixture(root);

    const HMODULE oldCapsule = LoadLibraryW(oldCapsuleDll.c_str());
    Check(oldCapsule != nullptr, "old capsule fixture loaded");
    Check(GetModuleHandleW(L"capsule_catalog.dll") == oldCapsule,
          "old capsule fixture has the production module basename");
    Check(GetProcAddress(oldCapsule, "SdgoGetResourceOverlayBrokerV1") == nullptr,
          "old capsule fixture lacks the broker ABI");

    const HMODULE unit = LoadLibraryW(unitDll.c_str());
    Check(unit != nullptr, "production unit_catalog loaded beside old capsule");
    const auto load = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(unit, "SdgoModLoad"));
    Check(load != nullptr, "production unit_catalog entry found for old-provider test");
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &Log;
    api.Symbol = &Symbol;
    api.HookCode = &HookCode;
    api.SettingInt = &SettingInt;
    api.ModDirectory = &ModDirectory;
    Check(load(&api, reinterpret_cast<SdgoMod*>(1)) == 0,
          "loaded old capsule provider is rejected explicitly");
    Check(g_openHooks == 0 && g_openDetour == nullptr,
          "old-provider rejection does not install a competing VFS hook");

    Check(FreeLibrary(unit) != 0, "unit_catalog unloaded after old-provider rejection");
    Check(FreeLibrary(oldCapsule) != 0, "old capsule fixture unloaded");
    fs::remove_all(root, ignored);
}

void Rehash(std::vector<uint8_t>& bytes) {
    auto* header = reinterpret_cast<Header*>(bytes.data());
    header->payloadHash = Hash32(bytes.data() + header->headerBytes, header->payloadBytes);
}

EntryHeader* HeaderAt(std::vector<uint8_t>& bytes, size_t index) {
    size_t cursor = sizeof(Header);
    for (size_t i = 0; i < index; ++i) {
        const auto* entry = reinterpret_cast<const EntryHeader*>(bytes.data() + cursor);
        cursor += sizeof(EntryHeader) + entry->byteCount;
    }
    return reinterpret_cast<EntryHeader*>(bytes.data() + cursor);
}

uint8_t* RecordAt(std::vector<uint8_t>& bytes, size_t index) {
    return reinterpret_cast<uint8_t*>(HeaderAt(bytes, index)) + sizeof(EntryHeader);
}
} // namespace

int ParserFixture() {
    Check(sdgo::content_localization_boundary::Query(
              0, 0, L"provider absent", 15) ==
              SDGO_CONTENT_LOCALIZATION_STOCK_OR_UNKNOWN,
          "absent optional unit catalog leaves stock localization eligible");
    Check(Hash32("hello", 5) == 0x4f9f2cabu, "FNV-1a matches independent test vector");
    const std::vector<Entry> source{
        MakeRecord(kUnitType, 25060),
        MakeRecord(kWeaponType, 61165),
        MakeRecord(kBlueprintType, 200001),
        MakeRecord(kItemCategoryType, 1250600),
    };
    const auto valid = Encode(source);
    Catalog catalog;
    std::string error;
    Check(catalog.Parse(valid, error), "valid mixed native catalog accepted");
    Check(catalog.records.size() == 4, "all native records retained");
    Check(catalog.Contains(25060, kUnitType) &&
          catalog.Contains(61165, kWeaponType) &&
          catalog.Contains(200001, kBlueprintType) &&
          catalog.Contains(1250600, kItemCategoryType), "typed lookup succeeds");
    Check(catalog.Find(7) == catalog.records.size(), "missing lookup is bounded");
    {
        Catalog owned;
        std::string boundaryError;
        Check(owned.Parse(
                  Encode({MakeRecord(kUnitType, 25060),
                          MakeRecord(kWeaponType, 61165),
                          MakeRecord(kItemCategoryType, 1250600),
                          MakeRecord(kSkillType, 9000001)}),
                  boundaryError),
              "display ownership fixture is a validated catalog");
        ContentLocalizationBoundaryIndex boundary;
        Check(boundary.Build(owned) && boundary.RecordCount() == 4,
              "display ownership index covers all supported native record kinds");
        Check(boundary.ContainsRecord(kUnitType, 25060) &&
                  boundary.ContainsRecord(kWeaponType, 61165) &&
                  boundary.ContainsRecord(kSkillType, 9000001) &&
                  boundary.ContainsRecord(kItemCategoryType, 1250600) &&
                  !boundary.ContainsRecord(kSkillType, 25060),
              "display ownership identity requires the exact native kind and id");
        for (const wchar_t* text :
             {L"Fixture Unit Name", L"Fixture Unit Detail",
              L"Fixture Unit\\nDisplay\\tMode",
              L"Fixture Unit\nDisplay\tMode", L"Fixture Weapon Name",
              L"Fixture Skill",
              L"Fixture Skill\\nDescription\\tTabbed",
              L"Fixture Skill\nDescription\tTabbed",
              L"Fixture Item Category"}) {
            Check(boundary.ContainsText(text, std::wcslen(text)),
                  "every protected raw or display-expanded field is indexed");
        }
        Check(!boundary.ContainsText(L"fixture unit name", 17) &&
                  !boundary.ContainsText(L" Fixture Unit Name", 18) &&
                  !boundary.ContainsText(L"Fixture Unit Name ", 18),
              "display ownership text remains case and whitespace exact");

        Catalog unterminated;
        Entry invalid = MakeRecord(kUnitType, 25060);
        std::memset(invalid.bytes.data() + 205, 0x41, 100);
        Check(unterminated.Parse(Encode({invalid}), boundaryError),
              "catalog parser independently accepts the unrelated display-field fixture");
        Check(!boundary.Build(unterminated) &&
                  boundary.RecordCount() == 0 &&
                  boundary.TextCount() == 0,
              "unterminated protected text refuses the complete index without stale ownership");
    }
    {
        Entry skill=MakeRecord(kSkillType,9000001);
        Entry funnel=MakeRecord(kFunnelType,198131);
        Check(catalog.Parse(Encode({funnel,skill}),error),"explicit 732-byte funnel and projected 1132-byte skill parsed");
        for(uint8_t dispatch=0;dispatch<=3;++dispatch) {
            skill.bytes[1131]=dispatch;
            Check(catalog.Parse(Encode({skill}),error),"observed skill dispatch range preserved");
            Check(catalog.records[0].bytes[1131]==dispatch,"skill dispatch is never normalized");
        }
        for(const size_t offset:{size_t(105),size_t(154),size_t(931),size_t(1130)}) {
            Entry invalid=skill;invalid.bytes[offset]=1;
            Check(!catalog.Parse(Encode({invalid}),error),"non-profile skill padding rejected even with fresh hashes");
        }
        for(const uint8_t dispatch:{uint8_t(107),uint8_t(165)}) {
            Entry invalid=skill;invalid.bytes[1131]=dispatch;
            Check(!catalog.Parse(Encode({invalid}),error),"unknown source skill dispatch rejected without truncation");
        }
        funnel.bytes.pop_back();
        Check(!catalog.Parse(Encode({funnel}),error),"incomplete funnel rejected");
        Entry feature=MakeRecord(kSpecialFeatureType,59051);
        Check(catalog.Parse(Encode({feature}),error) && catalog.Contains(59051,kSpecialFeatureType),
              "1559-byte special feature parsed and typed");
        Entry unnamed=feature;std::fill(unnamed.bytes.begin()+5,unnamed.bytes.begin()+105,uint8_t(0));
        Check(!catalog.Parse(Encode({unnamed}),error),"special feature without a name rejected");
        feature.bytes.pop_back();
        Check(!catalog.Parse(Encode({feature}),error),"incomplete special feature rejected");
    }
    {
        const Entry validBlueprint = MakeRecord(kBlueprintType, 200001);
        for (uint32_t count = kBlueprintMinimumMaterials;
             count <= kBlueprintMaximumMaterials; ++count) {
            Entry boundary = validBlueprint;
            Put32(boundary.bytes.data() + kBlueprintMaterialCountOffset, count);
            for (uint32_t slot = 0; slot < kBlueprintMaterialSlots; ++slot) {
                Put32(boundary.bytes.data() + kBlueprintMaterialOffset +
                          slot * sizeof(uint32_t),
                      slot < count ? 10001u + slot : 0u);
                Put32(boundary.bytes.data() + kBlueprintMaterialLevelOffset +
                          slot * sizeof(uint32_t),
                      slot < count ? 1u + slot : (slot == 6 ? 255u : 0u));
            }
            Check(catalog.Parse(Encode({boundary}), error),
                  "blueprint material counts 3..7 and retained inactive levels are accepted");
            Check(catalog.records.size() == 1 &&
                      catalog.records[0].bytes == boundary.bytes,
                  "valid blueprint boundary bytes are preserved exactly");
        }
        for (int mutation = 0; mutation < 9; ++mutation) {
            Entry invalid = validBlueprint;
            if (mutation == 0)
                Put32(invalid.bytes.data() + kBlueprintMaterialCountOffset, 2);
            if (mutation == 1)
                Put32(invalid.bytes.data() + kBlueprintMaterialCountOffset, 8);
            if (mutation == 2)
                Put32(invalid.bytes.data() + kBlueprintOutputOffset, 0);
            if (mutation == 3)
                Put32(invalid.bytes.data() + kBlueprintMaterialOffset, 0);
            if (mutation == 4)
                Put32(invalid.bytes.data() + kBlueprintMaterialLevelOffset, 0);
            if (mutation == 5)
                Put32(invalid.bytes.data() + kBlueprintMaterialLevelOffset, 256);
            if (mutation == 6)
                Put32(invalid.bytes.data() + kBlueprintMaterialOffset +
                      3 * sizeof(uint32_t), 10003);
            if (mutation == 7)
                Put32(invalid.bytes.data() + kBlueprintMaterialOffset +
                      sizeof(uint32_t), 25061);
            if (mutation == 8)
                Put32(invalid.bytes.data() + kBlueprintMaterialOffset, 25060);
            Check(!catalog.Parse(Encode({invalid}), error),
                  "malformed blueprint count, identity or material slots are rejected");
            Check(catalog.records.empty(), "malformed blueprint parse remains atomic");
        }
        Entry wideCount = validBlueprint;
        Put32(wideCount.bytes.data() + kBlueprintMaterialCountOffset, 256);
        Check(!catalog.Parse(Encode({wideCount}), error),
              "blueprint material count is validated as a DWORD without byte truncation");
        Check(catalog.records.empty(), "wide invalid blueprint count remains atomic");
    }
    {
        auto extended=source;
        extended.push_back(MakeRecord(kUnitIndexType,kUnitIndexFirstId));
        Check(catalog.Parse(Encode(extended),error) && catalog.Contains(kUnitIndexFirstId,kUnitIndexType),
              "encyclopedia records retain their separate native kind and full record");
        extended.back().bytes[5]=2;
        Check(!catalog.Parse(Encode(extended),error),"invalid encyclopedia enablement rejected");
        extended.back()=MakeRecord(kUnitIndexType,kUnitIndexFirstId);
        Put32(extended.back().bytes.data()+kUnitIndexRefOffset,0);
        Check(!catalog.Parse(Encode(extended),error),"encyclopedia entry requires a real unit reference");
    }

    for (const size_t length : {size_t(0), sizeof(Header) - 1, valid.size() - 1}) {
        const std::vector<uint8_t> truncated(valid.begin(), valid.begin() + length);
        Check(!catalog.Parse(truncated, error), "truncated catalog rejected");
        Check(catalog.records.empty(), "failed parse clears prior catalog");
    }

    for (int mutation = 0; mutation < 12; ++mutation) {
        auto invalid = valid;
        auto* header = reinterpret_cast<Header*>(invalid.data());
        auto* first = HeaderAt(invalid, 0);
        auto* second = HeaderAt(invalid, 1);
        uint8_t* unit = RecordAt(invalid, 0);
        if (mutation == 0) header->magic[0] ^= 1;
        if (mutation == 1) header->reserved = 1;
        if (mutation == 2) first->kind = 99;
        if (mutation == 3) first->byteCount -= 1;
        if (mutation == 4) second->id = first->id;
        if (mutation == 5) first->flags = 1;
        if (mutation == 6) unit[0] ^= 1;
        if (mutation == 7) unit[4] = 1;
        if (mutation == 8) std::memset(unit + kUnitModelPathOffset, 0x41, kUnitModelPathBytes);
        if (mutation == 9) PutUtf16(unit + kUnitModelPathOffset, kUnitModelPathBytes, "mdrs\\..\\escape.mod");
        if (mutation == 10) {
            uint8_t* category = RecordAt(invalid, 3);
            Put32(category + kItemCategoryRefOffset, 0);
            HeaderAt(invalid, 3)->recordHash =
                Hash32(category, HeaderAt(invalid, 3)->byteCount);
        }
        if (mutation == 11) first->recordHash ^= 1;
        if (mutation >= 2 && mutation <= 9) first->recordHash = Hash32(unit, first->byteCount);
        Rehash(invalid);
        Check(!catalog.Parse(invalid, error), "malformed native catalog rejected after outer rehash");
        Check(catalog.records.empty(), "semantic failure remains atomic");
    }

    {
        // Record-count bound: the exact runtime limit parses, one more record is refused.
        Check(kMaxRecords == 8192, "native catalog record limit is 8192");
        std::vector<Entry> bounded;
        bounded.reserve(kMaxRecords + 1);
        for (uint32_t id = 300001; bounded.size() < kMaxRecords; ++id)
            bounded.push_back(MakeRecord(kFunnelType, id));
        Check(catalog.Parse(Encode(bounded), error) && catalog.records.size() == kMaxRecords,
              "a catalog with exactly kMaxRecords records is accepted");
        Check(catalog.Contains(300001 + 4096, kFunnelType) &&
                  catalog.Contains(300001 + kMaxRecords - 1, kFunnelType),
              "records past the former 4096 bound stay addressable");
        bounded.push_back(MakeRecord(kFunnelType, 300001 + kMaxRecords));
        Check(!catalog.Parse(Encode(bounded), error) && catalog.records.empty(),
              "one record over kMaxRecords is refused atomically");
    }

    auto trailing = valid;
    trailing.push_back(0);
    auto* trailingHeader = reinterpret_cast<Header*>(trailing.data());
    ++trailingHeader->payloadBytes;
    Rehash(trailing);
    Check(!catalog.Parse(trailing, error), "trailing unframed payload rejected");

    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryName[MAX_PATH]{};
    const DWORD temporaryDirectoryLength =
        GetTempPathW(static_cast<DWORD>(std::size(temporaryDirectory)), temporaryDirectory);
    Check(temporaryDirectoryLength &&
          temporaryDirectoryLength < static_cast<DWORD>(std::size(temporaryDirectory)),
          "test-owned temporary directory resolved");
    Check(GetTempFileNameW(temporaryDirectory, L"sdu", 0, temporaryName) != 0,
          "test-owned unique catalog path created");
    const fs::path temporary(temporaryName);
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(valid.data()), static_cast<std::streamsize>(valid.size()));
        Check(static_cast<bool>(file), "catalog fixture written");
    }
    Check(catalog.Load(temporary.wstring(), error), "file loader accepts read-back fixture");
    std::error_code ignored;
    fs::remove(temporary, ignored);

    std::printf("unit_catalog_test: %d checks passed\n", g_checks);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 1) return ParserFixture();
    if (argc == 3 && !std::strcmp(argv[1], "--parse")) {
        Catalog catalog;
        std::string error;
        const bool parsed = catalog.Load(fs::absolute(fs::path(argv[2])).wstring(), error);
        Check(parsed, error.empty() ? "supplied catalog parsed" : error.c_str());
        std::printf("unit_catalog_parse: %u records accepted\n",
                    static_cast<unsigned>(catalog.records.size()));
        return 0;
    }
    if (argc == 3) {
        if (!std::strcmp(argv[2], "partial_failure")) {
            FailStopFixture(fs::absolute(fs::path(argv[1])));
            std::printf("unit_catalog_native_test[partial_failure]: %d checks passed\n", g_checks);
            return 0;
        }
        NativeFixture(fs::absolute(fs::path(argv[1])), argv[2]);
        std::printf("unit_catalog_native_test[%s]: %d checks passed\n", argv[2], g_checks);
        return 0;
    }
    if (argc == 4 && !std::strcmp(argv[2], "old_capsule")) {
        OldCapsuleFixture(fs::absolute(fs::path(argv[1])),
                          fs::absolute(fs::path(argv[3])));
        std::printf("unit_catalog_native_test[old_capsule]: %d checks passed\n", g_checks);
        return 0;
    }
    if (argc == 4 && !std::strcmp(argv[2], "partial_failure_child")) {
        NativeFixture(fs::absolute(fs::path(argv[1])), argv[2], fs::absolute(fs::path(argv[3])));
        Check(false, "partial-publication fixture child must be terminated by the production DLL");
    }
    std::fprintf(stderr,
                 "usage: unit_catalog_test [--parse <catalog.bin> | "
                 "<production-unit_catalog.dll> "
                  "<success|conflict|missing_reference|partial_failure|"
                  "blueprint_output_missing|blueprint_material_missing|"
                  "blueprint_item_reference_missing|index_blueprint_reference_missing|"
                  "index_blueprint_output_mismatch|large_catalog> | "
                  "<production-unit_catalog.dll> old_capsule <old-capsule.dll>]\n");
    return 2;
}
