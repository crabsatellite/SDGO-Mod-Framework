#include <windows.h>
#include <bcrypt.h>
#include <process.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "sdgo/acquisition_sources.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/operator_model.h"
#include "acquisition_sources_fixture.h"
#include "../support/native_resource_overlay/native_resource_overlay.h"

#pragma comment(lib, "bcrypt.lib")

namespace {
namespace fs = std::filesystem;
using namespace sdgo::operators;
struct Box { uint32_t type; void* data; };
struct Pair { uint32_t id; Box* box; };
struct InsertResult { void* node; uint8_t inserted; uint8_t pad[3]; };
struct Row { uint8_t* data; size_t size; Box* box; void* node; bool owned; };
using Key = std::pair<int, int>;

std::map<Key, Row> g_rows;
std::array<uint8_t, kOperatorSize> g_a, g_b, g_unrelated;
std::array<uint8_t, kCategorySize> g_filteredTemplate, g_template, g_existingA, g_existingB;
std::array<uint8_t, kCategorySize> g_filteredA, g_filteredB, g_filteredC, g_conflict;
std::wstring g_directory;
std::vector<std::string> g_logs;
SdgoApi g_apiFixture = {};
void* g_hook = nullptr;
void* g_resourceHook = nullptr;
int g_originalCalls = 0;
int g_allocationCalls = 0;
int g_resourceHookCalls = 0;
int g_overlayArchiveOpens = 0;
int g_stockResourceOpens = 0;
int g_resourceConstructs = 0;
bool g_legacy = false;

void RequireImpl(bool ok, int line)
{
    if (ok) return;
    std::fprintf(stderr, "operator_catalog_test failure at line %d\n", line);
    ExitProcess(90);
}
#define Require(value) RequireImpl((value), __LINE__)
using ConvertFn = int(WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);
ConvertFn g_import = &MultiByteToWideChar;

__declspec(noinline) int NativeDecode(UINT page, const char* text, wchar_t* output, int capacity)
{
    volatile int count = g_import(page, 0, text, -1, output, capacity);
    return count;
}

int Import(SdgoMod*, const char* dll, const char* function, void* replacement, void** original)
{
    Require(!_stricmp(dll, "kernel32.dll") && !strcmp(function, "MultiByteToWideChar"));
    *original = reinterpret_cast<void*>(g_import);
    g_import = reinterpret_cast<ConvertFn>(replacement);
    return 1;
}

void* __cdecl Grid() { return reinterpret_cast<void*>(0x12345678); }

uint8_t __fastcall Find(void* self, void*, void** out, int id, int type, char)
{
    Require(self == Grid());
    const auto found = g_rows.find({id, type});
    if (found == g_rows.end()) return 0;
    *out = found->second.data;
    return 1;
}

void* __cdecl Allocate(size_t size)
{
    ++g_allocationCalls;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void __cdecl Free(void* value) { Require(HeapFree(GetProcessHeap(), 0, value) != 0); }

void* __cdecl OpenArchive(const wchar_t*, const wchar_t*)
{
    ++g_overlayArchiveOpens;
    return reinterpret_cast<void*>(0x51515151);
}

void* __fastcall ConstructResource(
    void* storage, void*, void* archive,
    sdgo::resource_overlay::NativeFileInfo& info,
    uint32_t chunkSize, bool)
{
    Require(storage != nullptr && archive == reinterpret_cast<void*>(0x51515151));
    Require(chunkSize == 512 && info.words[0] == 1 && info.words[2] == 1);
    ++g_resourceConstructs;
    return storage;
}

void* __fastcall StockResourceOpen(void*, void*, const void*, bool)
{
    ++g_stockResourceOpens;
    return reinterpret_cast<void*>(0x61616161);
}

void* __fastcall Make(void* self, void*, const Pair* pair)
{
    Require(self == Grid());
    auto* node = static_cast<uint8_t*>(Allocate(24));
    std::memcpy(node + 12, pair, 8);
    return node;
}

void __fastcall Insert(void* self, void*, InsertResult* result, void* node, char duplicate)
{
    Require(self == Grid() && duplicate == 0);
    Pair pair = {};
    std::memcpy(&pair, static_cast<uint8_t*>(node) + 12, 8);
    const Key key = {static_cast<int>(pair.id), static_cast<int>(pair.box->type)};
    if (g_rows.count(key)) {
        Free(node);
        result->inserted = 0;
        return;
    }
    const size_t size = pair.box->type == 0x2C ? kOperatorSize : kCategorySize;
    g_rows.emplace(key, Row{static_cast<uint8_t*>(pair.box->data), size, pair.box, node, true});
    result->node = node;
    result->inserted = 1;
}

uint8_t __fastcall Original(void* self, void*, const wchar_t*, int)
{
    Require(self == Grid());
    ++g_originalCalls;
    return 1;
}

void* Symbol(SdgoMod*, const char* name)
{
    if (!strcmp(name, "operator.native_name_decode"))
        return g_legacy ? nullptr : reinterpret_cast<void*>(&NativeDecode);
    if (!strcmp(name, "text.ansi_to_wide")) return reinterpret_cast<void*>(&NativeDecode);
    if (!strcmp(name, "operator.grid_data")) return reinterpret_cast<void*>(&Grid);
    if (!strcmp(name, "operator.grid_find")) return reinterpret_cast<void*>(&Find);
    if (!strcmp(name, "operator.grid_make_node")) return reinterpret_cast<void*>(&Make);
    if (!strcmp(name, "operator.grid_insert")) return reinterpret_cast<void*>(&Insert);
    if (!strcmp(name, "operator.native_new")) return reinterpret_cast<void*>(&Allocate);
    if (!strcmp(name, "operator.native_delete")) return reinterpret_cast<void*>(&Free);
    if (!strcmp(name, "capsule.main.file_ctor"))
        return reinterpret_cast<void*>(&ConstructResource);
    if (!strcmp(name, "capsule.main.operator_new"))
        return reinterpret_cast<void*>(&Allocate);
    if (!strcmp(name, "capsule.main.wfopen"))
        return reinterpret_cast<void*>(&OpenArchive);
    return nullptr;
}

int Hook(SdgoMod*, const char* name, void* detour, void** original)
{
    if (!strcmp(name, "operator.load_data")) {
        g_hook = detour;
        *original = reinterpret_cast<void*>(&Original);
        return 1;
    }
    if (!strcmp(name, "capsule.main.open_file")) {
        ++g_resourceHookCalls;
        g_resourceHook = detour;
        *original = reinterpret_cast<void*>(&StockResourceOpen);
        return 1;
    }
    Require(false);
    return 0;
}

void Log(SdgoMod*, int, const char* format, ...)
{
    char text[512] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    g_logs.emplace_back(text);
}

bool HasLog(const char* fragment)
{
    for (const std::string& text : g_logs)
        if (text.find(fragment) != std::string::npos) return true;
    return false;
}

int Setting(SdgoMod*, const char*, int def) { return def; }
const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }

void Put(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    Require(WritePrivateProfileStringW(section, key, value, file.c_str()) != 0);
}

void PutU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::string Sha256(const std::vector<uint8_t>& bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE state = nullptr;
    DWORD stateBytes = 0;
    DWORD hashBytes = 0;
    DWORD received = 0;
    Require(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
    Require(BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&stateBytes), sizeof(stateBytes),
                &received, 0) >= 0);
    Require(BCryptGetProperty(
                algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                &received, 0) >= 0 && hashBytes == 32);
    std::vector<uint8_t> storage(stateBytes);
    std::vector<uint8_t> digest(hashBytes);
    Require(BCryptCreateHash(
                algorithm, &state, storage.data(), stateBytes, nullptr, 0, 0) >= 0);
    Require(bytes.empty() || BCryptHashData(
                state, const_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()), 0) >= 0);
    Require(BCryptFinishHash(state, digest.data(), hashBytes, 0) >= 0);
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

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(stream));
}

void WriteResourceBundle(uint32_t operatorId,
                         const std::vector<std::wstring>& paths)
{
    const fs::path root =
        fs::path(g_directory) / "assets" / std::to_string(operatorId);
    fs::create_directories(root);
    std::vector<uint8_t> archive(paths.size() * 520u, 0);
    std::vector<uint8_t> index;
    for (size_t entry = 0; entry < paths.size(); ++entry) {
        const uint32_t next = static_cast<uint32_t>(entry + 1);
        const uint32_t previous =
            entry ? static_cast<uint32_t>(entry - 1) : 0;
        std::memcpy(archive.data() + entry * 520u, &next, sizeof(next));
        std::memcpy(archive.data() + entry * 520u + 4, &previous,
                    sizeof(previous));
        archive[entry * 520u + 8] = static_cast<uint8_t>(entry + 1);
    }
    for (uint32_t value :
         {1u, 512u, static_cast<uint32_t>(paths.size()), 0u,
          static_cast<uint32_t>(paths.size()), 0u})
        PutU32(index, value);
    for (size_t entry = 0; entry < paths.size(); ++entry) {
        for (wchar_t value : paths[entry]) {
            const uint16_t encoded =
                static_cast<uint16_t>(value) ^ 0x00aa;
            const size_t offset = index.size();
            index.resize(offset + sizeof(encoded));
            std::memcpy(index.data() + offset, &encoded, sizeof(encoded));
        }
        const uint16_t terminator = 0x00aa;
        const size_t offset = index.size();
        index.resize(offset + sizeof(terminator));
        std::memcpy(index.data() + offset, &terminator, sizeof(terminator));
        for (uint32_t value :
             {1u, static_cast<uint32_t>(entry), 1u,
              0u, 0u, 0u, 0u, 0u, 0u})
            PutU32(index, value);
    }
    WriteBytes(root / "resources.zdx", index);
    WriteBytes(root / "resources.zpk", archive);
    std::ofstream manifest(
        root / "resource-overlay.ini", std::ios::binary | std::ios::trunc);
    manifest << "[runtime]\n"
             << "schema=1\n"
             << "format=sdgo-native-resource-overlay-v1\n"
             << "operator_id=" << operatorId << "\n"
             << "index=resources.zdx\n"
             << "index_sha256=" << Sha256(index) << "\n"
             << "archive=resources.zpk\n"
             << "archive_sha256=" << Sha256(archive) << "\n";
    Require(static_cast<bool>(manifest));
}

struct HostString
{
    uint8_t bytes[24] = {};
    std::wstring text;

    explicit HostString(std::wstring value) : text(std::move(value))
    {
        static_assert(sizeof(void*) == 4);
        const wchar_t* pointer = text.c_str();
        std::memcpy(bytes, &pointer, sizeof(pointer));
        const uint32_t size = static_cast<uint32_t>(text.size());
        const uint32_t capacity = size;
        std::memcpy(bytes + 0x10, &size, sizeof(size));
        std::memcpy(bytes + 0x14, &capacity, sizeof(capacity));
    }
};

void NativeRow(std::array<uint8_t, kOperatorSize>& row, uint32_t id, const wchar_t* marker)
{
    row.fill(0x3A);
    WriteId(row.data(), id);
    row[4] = 0;
    Require(Text(row.data() + 5, 50, marker));
    Require(Text(row.data()+105,50,L"gdrs\\"+std::to_wstring(id)+L".gdt"));
    Require(Text(row.data()+933,50,L"sdrs\\"+std::to_wstring(id)+L".zsd"));
    for (size_t i = 0; i < 7; ++i)
        Require(Text(row.data() + kAvatarOffsets[i], kAvatarCapacities[i],
                     std::wstring(marker) + std::to_wstring(i)));
}

void CategoryRow(std::array<uint8_t, kCategorySize>& row, uint32_t id, uint8_t security,
                 uint32_t ref, uint8_t time, uint32_t option, uint8_t seed)
{
    for (size_t i = 0; i < row.size(); ++i)
        row[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i * 37));
    WriteId(row.data(), id);
    row[4] = security;
    WriteId(row.data() + 105, ref);
    row[109] = time;
    std::memcpy(row.data() + 110, &option, sizeof(option));
}

// Mirrors the native type-0x22 decoder: secured rows never enter the runtime map.
void DecodeCategory(std::array<uint8_t, kCategorySize>& row)
{
    if (row[4] != 0) return;
    const uint32_t id = ReadId(row.data());
    g_rows[{static_cast<int>(id), 0x22}] = {row.data(), row.size(), nullptr, nullptr, false};
}

void RuntimeCategory(uint32_t key, std::array<uint8_t, kCategorySize>& row)
{
    g_rows[{static_cast<int>(key), 0x22}] = {row.data(), row.size(), nullptr, nullptr, false};
}

void OperatorConfig(const std::wstring& file, uint32_t id, uint32_t category,
                    const wchar_t* name, const wchar_t* sprite = nullptr)
{
    const std::wstring section = L"operator:" + std::to_wstring(id);
    Put(file, section.c_str(), L"Category", std::to_wstring(category).c_str());
    Put(file, section.c_str(), L"Template", L"60003");
    Put(file, section.c_str(), L"AvatarProfile", L"60013");
    Put(file, section.c_str(), L"Name", name);
    if (sprite) Put(file, section.c_str(), L"AvatarSprite", sprite);
}

void WriteConfig(int mode)
{
    const std::wstring file = g_directory + L"\\display.ini";
    DeleteFileW(file.c_str());
    Put(file, L"catalog", L"Version", L"1");
    OperatorConfig(file, 60003, 1210002, L"display");
    Put(file, L"operator:60003", L"Template", mode == 2 ? L"69999" : L"60003");
    Put(file, L"operator:60003", L"FixedName", g_legacy ? nullptr : L"1");
    // UTF16LE hex supports quotes and non-ASCII names without INI quote stripping.
    Put(file, L"operator:60003", L"NameHex", mode == 1 ? L"GG00" : L"0C54F3892200");
    OperatorConfig(file, 60013, 1210012, L"default support");
    OperatorConfig(file, 60018, 1250002, L"default support");
    OperatorConfig(file, 60025, 1220001, L"default support");
    OperatorConfig(file, 60041, 1220008, L"default support");
    OperatorConfig(file, 60047, 1219047, L"conflicting operator");
    OperatorConfig(file, 60048, 1219048, L"new operator",
                   L"sdgo\\mods\\operator_catalog\\assets\\60048.spr");
    if(mode>=10) {
        Put(file,L"catalog",L"Version",L"2");
        Put(file,L"operator:60048",L"AvatarSprite",L"");
        Put(file,L"operator:60048",L"MissionParts",L"0");
        Put(file,L"operator:60048",L"Dialogue",L"sdgo\\mods\\operator_catalog\\assets\\60048\\0.gdt");
        Put(file,L"operator:60048",L"IntroVoice",L"sdgo\\mods\\operator_catalog\\assets\\60048\\1.zsd");
        for(size_t i=0;i<7;++i) {
            const std::wstring key=L"Sprite"+std::to_wstring(i);
            const std::wstring value=L"sdgo\\mods\\operator_catalog\\assets\\60048\\"+std::to_wstring(i+2)+L".spr";
            Put(file,L"operator:60048",key.c_str(),value.c_str());
        }
        Put(file,L"operator:60003",L"Enabled",L"0");
    }
    if (mode == 12) {
        for (uint32_t id : {60003u, 60013u, 60018u, 60025u, 60041u}) {
            const std::wstring section = L"operator:" + std::to_wstring(id);
            Put(file, section.c_str(), L"FixedName", L"0");
        }
    }

    if (mode == 1 || mode == 2 || mode == 11)
        return;
    std::vector<std::wstring> paths;
    if (mode >= 10) {
        paths = {
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\0.gdt",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\1.zsd",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\2.spr",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\3.spr",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\4.spr",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\5.spr",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\6.spr",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\7.spr",
            L"sdgo\\mods\\operator_catalog\\assets\\60048\\8.spr",
        };
        if (mode == 13)
            paths.pop_back();
    } else {
        paths.push_back(
            L"sdgo\\mods\\operator_catalog\\assets\\60048.txr");
        for (uint32_t frame = 0; frame < 7; ++frame)
            paths.push_back(
                L"sdgo\\mods\\operator_catalog\\assets\\60048_" +
                std::to_wstring(frame) + L".spr");
    }
    WriteResourceBundle(60048, paths);
}

void BaseRows()
{
    NativeRow(g_a, 60003, L"avatarA");
    NativeRow(g_b, 60013, L"avatarB");
    NativeRow(g_unrelated, 60001, L"unrelated");
    g_rows[{60003, 0x2C}] = {g_a.data(), g_a.size(), nullptr, nullptr, false};
    g_rows[{60013, 0x2C}] = {g_b.data(), g_b.size(), nullptr, nullptr, false};
    g_rows[{60001, 0x2C}] = {g_unrelated.data(), g_unrelated.size(), nullptr, nullptr, false};
}

void NormalCategories()
{
    CategoryRow(g_filteredTemplate, 1210001, 1, 60001, 1, 0, 0x11);
    DecodeCategory(g_filteredTemplate);
    Require(!g_rows.count({1210001, 0x22}));

    CategoryRow(g_template, 1210017, 0, 60021, 1, 0, 0x27);
    DecodeCategory(g_template);
    CategoryRow(g_existingA, 1210012, 0, 60013, 1, 0, 0x31);
    DecodeCategory(g_existingA);
    CategoryRow(g_existingB, 1220008, 0, 60041, 0, 0, 0x42);
    DecodeCategory(g_existingB);

    CategoryRow(g_filteredA, 1210002, 1, 60003, 1, 0, 0x53);
    CategoryRow(g_filteredB, 1250002, 1, 60018, 1, 0, 0x64);
    CategoryRow(g_filteredC, 1220001, 1, 60025, 1, 0, 0x75);
    DecodeCategory(g_filteredA);
    DecodeCategory(g_filteredB);
    DecodeCategory(g_filteredC);
    Require(!g_rows.count({1210002, 0x22}) && !g_rows.count({1250002, 0x22}) &&
            !g_rows.count({1220001, 0x22}));

    CategoryRow(g_conflict, 1219047, 0, 60013, 0, 0, 0x86);
    DecodeCategory(g_conflict);
}

void InvalidCandidate(int mode)
{
    CategoryRow(g_filteredTemplate, 1210001, 1, 60001, 1, 0, 0x11);
    DecodeCategory(g_filteredTemplate);
    if (mode == 4) return;
    const uint32_t rowId = mode == 9 ? 1210999 : 1210017;
    const uint8_t security = mode == 5 ? 1 : 0;
    const uint32_t ref = mode == 6 ? 61000 : 60021;
    const uint8_t time = mode == 7 ? 3 : 1;
    const uint32_t option = mode == 8 ? 1 : 0;
    CategoryRow(g_template, rowId, security, ref, time, option, 0x27);
    // Security and ID mismatch are injected directly to exercise defensive
    // validation even though the stock decoder would not produce these rows.
    RuntimeCategory(1210017, g_template);
}

using HookFn = uint8_t(__fastcall*)(void*, void*, const wchar_t*, int);
using ResourceHookFn = void*(__fastcall*)(void*, void*, const void*, bool);

void CheckResourceOpen(const std::wstring& path)
{
    Require(g_resourceHook != nullptr);
    const int archiveBefore = g_overlayArchiveOpens;
    const int constructBefore = g_resourceConstructs;
    const int stockBefore = g_stockResourceOpens;
    HostString native(path);
    void* stream = reinterpret_cast<ResourceHookFn>(g_resourceHook)(
        reinterpret_cast<void*>(1), nullptr, native.bytes, false);
    Require(stream != nullptr && stream != reinterpret_cast<void*>(0x61616161));
    Require(g_overlayArchiveOpens == archiveBefore + 1);
    Require(g_resourceConstructs == constructBefore + 1);
    Require(g_stockResourceOpens == stockBefore);
    Free(stream);
}

HMODULE LoadMod(const wchar_t* path, int mode)
{
    HMODULE module = LoadLibraryW(path);
    Require(module != nullptr);
    const auto acquisition = reinterpret_cast<SdgoAcquisitionSourcesV1Fn>(
        GetProcAddress(module, SDGO_ACQUISITION_SOURCES_EXPORT));
    Require(acquisition != nullptr);
    const SdgoAcquisitionRequestV1 request{
        sizeof(SdgoAcquisitionRequestV1), 1, SDGO_ACQUISITION_BLUEPRINT, 200001};
    acquisition_fixture::Capture capture;
    const SdgoAcquisitionSinkV1 sink{
        sizeof(SdgoAcquisitionSinkV1), 1, &capture, &acquisition_fixture::Emit};
    Require(acquisition(&request, &sink) == SDGO_ACQUISITION_UNAVAILABLE &&
            capture.rows.empty());
    auto entry = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad"));
    Require(entry != nullptr);
    g_apiFixture = {};
    g_apiFixture.size = sizeof(g_apiFixture);
    g_apiFixture.version = 1;
    g_apiFixture.Log = &Log;
    g_apiFixture.Symbol = &Symbol;
    g_apiFixture.HookCode = &Hook;
    g_apiFixture.SettingInt = &Setting;
    g_apiFixture.ModDirectory = &Directory;
    g_apiFixture.HookImport = &Import;
    const int loaded = entry(&g_apiFixture, reinterpret_cast<SdgoMod*>(1));
    if (mode == 1 || mode == 2 || mode == 11 || mode == 13) {
        Require(loaded == 0 && !g_hook && !g_resourceHook);
    } else {
        Require(loaded != 0 && g_hook && g_resourceHook);
    }
    capture.rows.clear();
    if (mode == 0) {
        Require(acquisition(&request, &sink) == SDGO_ACQUISITION_READY);
        Require(capture.rows.size() == 1 &&
                capture.rows[0].first == "operator:60003:blueprint:200001" &&
                capture.rows[0].second == L"operator fixture blueprint drop");
    } else {
        Require(acquisition(&request, &sink) == SDGO_ACQUISITION_UNAVAILABLE &&
                capture.rows.empty());
    }
    return module;
}

bool CategoryOwnedField(size_t offset)
{
    return offset < 4 || (offset >= 5 && offset < 114);
}

void CheckCreatedCategory(uint32_t category, uint32_t op, const std::wstring& expectedName)
{
    const Row& created = g_rows.at({static_cast<int>(category), 0x22});
    Require(created.owned && ReadId(created.data) == category && created.data[4] == g_template[4]);
    uint32_t ref = 0, option = 1;
    std::memcpy(&ref, created.data + 105, sizeof(ref));
    std::memcpy(&option, created.data + 110, sizeof(option));
    Require(ref == op && created.data[109] == 0 && option == 0);
    wchar_t name[50] = {};
    std::memcpy(name, created.data + 5, sizeof(name));
    Require(std::wstring(name) == expectedName);
    for (size_t i = 0; i < kCategorySize; ++i)
        if (!CategoryOwnedField(i)) Require(created.data[i] == g_template[i]);
}

std::map<Key, std::vector<uint8_t>> SnapshotRows()
{
    std::map<Key, std::vector<uint8_t>> result;
    for (const auto& pair : g_rows)
        result[pair.first] = std::vector<uint8_t>(pair.second.data, pair.second.data + pair.second.size);
    return result;
}

void Cleanup(HMODULE module)
{
    for (auto& pair : g_rows) {
        if (!pair.second.owned) continue;
        Free(pair.second.data);
        Free(pair.second.box);
        Free(pair.second.node);
    }
    FreeLibrary(module);
}

void CheckNameHook()
{
    wchar_t rendered[64] = {};
    Require(NativeDecode(874, "@SDGO:60003", rendered, 64) > 0 &&
            !wcscmp(rendered, L"\x540c\x89f3\""));
    Require(NativeDecode(874, "@SDGO:60003", nullptr, 0) == 4);
    Require(NativeDecode(874, "@SDGO:60003", rendered, 2) == 0 &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    Require(NativeDecode(874, "personal-name", rendered, 64) > 0 &&
            !wcscmp(rendered, L"personal-name"));
    Require(NativeDecode(874, "@SDGO:60048", rendered, 64) > 0 &&
            !wcscmp(rendered, L"@SDGO:60048"));
    Require(NativeDecode(65001, "@SDGO:60003", rendered, 64) > 0 &&
            !wcscmp(rendered, L"@SDGO:60003"));
    Require(g_import(874, 0, "@SDGO:60003", -1, rendered, 64) > 0 &&
            !wcscmp(rendered, L"@SDGO:60003"));
}

void RunNormal(const wchar_t* dll, int mode)
{
    NormalCategories();
    const auto before = g_a;
    const auto otherBefore = g_unrelated;
    const auto templateBefore = g_template;
    const auto existingABefore = g_existingA;
    const auto existingBBefore = g_existingB;
    const auto conflictBefore = g_conflict;
    HMODULE module = LoadMod(dll, mode);
    Require(g_resourceHookCalls == 1);
    CheckResourceOpen(
        L"sdgo\\mods\\operator_catalog\\assets\\60048_0.spr");
    CheckNameHook();
    auto invoke = reinterpret_cast<HookFn>(g_hook);
    Require(invoke(Grid(), nullptr, L"other.gdt", 2) == 1 && g_a == before &&
            g_originalCalls == 1);
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1 &&
            g_originalCalls == 2);

    Require(g_unrelated == otherBefore && g_template == templateBefore);
    Require(g_existingA == existingABefore && g_existingB == existingBBefore);
    Require(g_conflict == conflictBefore);
    Require(!g_rows.count({60047, 0x2C}));
    Require(HasLog("operator item 1219047 conflicts"));

    const std::array<std::pair<uint32_t, uint32_t>, 3> filteredDefaults = {{
        {1210002, 60003}, {1250002, 60018}, {1220001, 60025}
    }};
    for (const auto& item : filteredDefaults)
        CheckCreatedCategory(item.first, item.second,
                             item.second == 60003 ? L"\x540c\x89f3\"" : L"default support");
    CheckCreatedCategory(1219048, 60048, L"new operator");

    Require(g_a[1200] == before[1200] && g_a[kMissionPartsOffset] == 1);
    Require(!memcmp(g_a.data()+105,before.data()+105,100));
    Require(!memcmp(g_a.data()+933,before.data()+933,100));
    for (uint32_t id : {60013u, 60018u, 60025u, 60041u})
        Require(g_rows.at({static_cast<int>(id), 0x2C}).data[kMissionPartsOffset] == 1);
    for (size_t i = 0; i < 7; ++i)
        Require(!memcmp(g_a.data() + kAvatarOffsets[i], g_b.data() + kAvatarOffsets[i],
                        kAvatarCapacities[i] * 2));
    wchar_t name[100] = {};
    std::memcpy(name, g_a.data() + 5, 200);
    Require(name[0] == 0x540C && name[1] == 0x89F3 && name[2] == L'"');

    const Row& custom = g_rows.at({60048, 0x2C});
    Require(ReadId(custom.data) == 60048 && custom.data[1200] == before[1200]);
    Require(custom.data[kMissionPartsOffset] == before[kMissionPartsOffset]);
    for (size_t i = 0; i < 7; ++i) {
        wchar_t path[54] = {};
        std::memcpy(path, custom.data + kAvatarOffsets[i], kAvatarCapacities[i] * 2);
        Require(std::wstring(path) ==
                L"sdgo\\mods\\operator_catalog\\assets\\60048_" + std::to_wstring(i) + L".spr");
    }

    const auto firstApply = SnapshotRows();
    const int allocations = g_allocationCalls;
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1 &&
            g_originalCalls == 3);
    Require(SnapshotRows() == firstApply && g_allocationCalls == allocations);
    Cleanup(module);
}

void RunInvalidCandidate(const wchar_t* dll, int mode)
{
    InvalidCandidate(mode);
    const auto beforeA = g_a;
    const auto beforeB = g_b;
    const auto beforeOther = g_unrelated;
    const auto beforeRows = SnapshotRows();
    HMODULE module = LoadMod(dll, mode);
    auto invoke = reinterpret_cast<HookFn>(g_hook);
    Require(invoke(Grid(), nullptr, L"other.gdt", 2) == 1 && g_originalCalls == 1);
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1 &&
            g_originalCalls == 2);
    Require(g_a == beforeA && g_b == beforeB && g_unrelated == beforeOther);
    Require(SnapshotRows() == beforeRows && g_allocationCalls == 0);
    Require(!g_rows.count({60048, 0x2C}) && !g_rows.count({1219048, 0x22}));
    Require(HasLog("native operator item template unavailable"));
    Cleanup(module);
}

void RunChildCases(wchar_t** argv)
{
    for (int mode = 4; mode <= 9; ++mode) {
        const std::wstring value = std::to_wstring(mode);
        const wchar_t* child[] = {argv[0], argv[1], argv[2], value.c_str(), nullptr};
        Require(_wspawnv(_P_WAIT, argv[0], child) == 0);
    }
}

void RunResources(const wchar_t* dll) {
    NormalCategories();g_a[kMissionPartsOffset]=1;
    const auto original=g_a;const auto unrelated=g_unrelated;
    HMODULE module=LoadMod(dll,10);auto invoke=reinterpret_cast<HookFn>(g_hook);
    Require(g_resourceHookCalls==1);
    CheckResourceOpen(
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\0.gdt");
    Require(invoke(Grid(),nullptr,L"gdrs\\0255.gdt",0x2C)==1);
    Require(g_a==original && g_unrelated==unrelated);
    const auto& created=g_rows.at({60048,0x2C});
    wchar_t value[100]{};
    memcpy(value,created.data+105,100);
    Require(std::wstring(value)==L"sdgo\\mods\\operator_catalog\\assets\\60048\\0.gdt");
    memcpy(value,created.data+933,100);
    Require(std::wstring(value)==L"sdgo\\mods\\operator_catalog\\assets\\60048\\1.zsd");
    for(size_t i=0;i<7;++i) {
        memcpy(value,created.data+kAvatarOffsets[i],100);
        Require(std::wstring(value)==L"sdgo\\mods\\operator_catalog\\assets\\60048\\"+std::to_wstring(i+2)+L".spr");
    }
    Require(created.data[kMissionPartsOffset]==0);
    Cleanup(module);
}

void RunResourceFailure(const wchar_t* dll, int mode)
{
    NormalCategories();
    const auto beforeRows = SnapshotRows();
    HMODULE module = LoadMod(dll, mode);
    Require(SnapshotRows() == beforeRows);
    Require(g_resourceHookCalls == 0 && !g_resourceHook && !g_hook);
    if (mode == 11)
        Require(HasLog("resources rejected before publication"));
    else
        Require(HasLog("missing required resource"));
    FreeLibrary(module);
}

void RunSharedProvider(const wchar_t* dll)
{
    NormalCategories();
    const fs::path providerPath = fs::path(g_directory) / "unit_catalog.dll";
    Require(CopyFileW(dll, providerPath.c_str(), FALSE) != 0);
    HMODULE provider = LoadMod(providerPath.c_str(), 12);
    Require(GetModuleHandleW(L"unit_catalog.dll") == provider);
    Require(g_resourceHookCalls == 1 && g_resourceHook);

    g_hook = nullptr;
    HMODULE consumer = LoadMod(dll, 12);
    Require(g_resourceHookCalls == 1);
    Require(HasLog("registered with unit_catalog.dll"));
    CheckResourceOpen(
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\0.gdt");
    auto invoke = reinterpret_cast<HookFn>(g_hook);
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1);
    Require(g_rows.count({60048, 0x2C}) != 0);

    Cleanup(consumer);
    Require(FreeLibrary(provider) != 0);
}

void RunResourceChildCases(wchar_t** argv)
{
    for (int mode = 11; mode <= 13; ++mode) {
        const std::wstring value = std::to_wstring(mode);
        const wchar_t* child[] = {
            argv[0], argv[1], argv[2], value.c_str(), nullptr,
        };
        Require(_wspawnv(_P_WAIT, argv[0], child) == 0);
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    Require(argc == 4);
    const int mode = _wtoi(argv[3]);
    Require(mode >= 0 && mode <= 13);
    g_legacy = mode == 3;
    Require(CreateDirectoryW(argv[2], nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS);
    g_directory = std::wstring(argv[2]) + L"\\runtime-" + std::to_wstring(mode);
    std::error_code cleanupError;
    fs::remove_all(g_directory, cleanupError);
    Require(!cleanupError);
    Require(fs::create_directories(g_directory));
    WriteConfig(mode);
    if (mode == 0) {
        Require(acquisition_fixture::Write(
            fs::path(g_directory) / "acquisition-sources.bin", 2,
            {{SDGO_ACQUISITION_BLUEPRINT, 200001,
              "operator:60003:blueprint:200001",
              L"operator fixture blueprint drop"}}));
    }
    BaseRows();
    if (mode == 1 || mode == 2) {
        const auto before = g_a;
        HMODULE module = LoadMod(argv[1], mode);
        Require(g_a == before);
        FreeLibrary(module);
        return 0;
    }
    if(mode==10) {
        RunResources(argv[1]);
        RunResourceChildCases(argv);
    }
    else if(mode==11 || mode==13)RunResourceFailure(argv[1],mode);
    else if(mode==12)RunSharedProvider(argv[1]);
    else if (mode <= 3) RunNormal(argv[1], mode);
    else RunInvalidCandidate(argv[1], mode);
    if (mode == 0) RunChildCases(argv);
    return 0;
}
