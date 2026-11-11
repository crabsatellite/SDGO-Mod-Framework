#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "../mods/announcements/news_json.h"

namespace fs = std::filesystem;
namespace {
int checks = 0;
int failures = 0;
std::wstring modDirectory;
std::vector<std::string> logs;

void Check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(value) Check(!!(value), #value, __LINE__)

const wchar_t* ModDirectory(SdgoMod*) { return modDirectory.c_str(); }
void Log(SdgoMod*, int, const char* format, ...) {
    char text[2048]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    logs.emplace_back(text);
}
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
std::set<uint32_t> UnitIds(const fs::path& path) {
    using namespace sdgo::announcements::json;
    const std::string bytes = Read(path);
    const Value root = Parser(bytes).Parse();
    std::set<uint32_t> ids;
    if (root.kind != Value::Object || root["pilots"].kind != Value::Array)
        return ids;
    for (const Value& entry : root["pilots"].array) {
        const int64_t raw = entry["unit_id"].Integer(-1);
        if (raw > 0 && raw <= 9999999)
            ids.insert(static_cast<uint32_t>(raw));
    }
    return ids;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: ace_ai_server_test <dll> <staged-mod-dir> <success|overlay|missing|invalid-overlay|unreadable-overlay|oversize-overlay|directory-overlay>\n");
        return 2;
    }
    const fs::path sourceDll = argv[1];
    const fs::path sourceDirectory = argv[2];
    const std::wstring mode = argv[3];
    const fs::path root = fs::temp_directory_path() /
        (L"sdgo-ace-ai-server-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    HANDLE lockedOverlay = INVALID_HANDLE_VALUE;
    fs::remove_all(root, ignored);
    const fs::path directory = root / L"ace_ai_server";
    fs::create_directories(directory);
    const fs::path dll = directory / L"ace_ai_server.dll";
    fs::copy_file(sourceDll, dll, fs::copy_options::overwrite_existing);
    const fs::path config = sourceDirectory / L"config.ini";
    if (fs::exists(config))
        fs::copy_file(config, directory / L"config.ini", fs::copy_options::overwrite_existing);
    const fs::path base = sourceDirectory / L"base-pilots.json";
    if (mode != L"missing")
        fs::copy_file(base, directory / L"base-pilots.json", fs::copy_options::overwrite_existing);
    const fs::path overlay = directory / L"owner-pilots.json";
    if (mode == L"overlay") {
        std::ofstream(overlay, std::ios::binary) <<
            "{\"schema_version\":1,\"pilots\":["
            "{\"unit_id\":9000000,\"display_name\":\"Owner Ace\",\"kind\":\"named\"}]}";
    } else if (mode == L"invalid-overlay") {
        std::ofstream(overlay, std::ios::binary) << "{not-json";
    } else if (mode == L"unreadable-overlay") {
        lockedOverlay = CreateFileW(overlay.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lockedOverlay != INVALID_HANDLE_VALUE);
    } else if (mode == L"oversize-overlay") {
        std::ofstream stream(overlay, std::ios::binary);
        stream.seekp(2 * 1024 * 1024);
        stream.put('x');
        CHECK(static_cast<bool>(stream));
    } else if (mode == L"directory-overlay") {
        CHECK(fs::create_directory(overlay));
    }

    modDirectory = directory.wstring();
    HMODULE module = LoadLibraryW(dll.c_str());
    CHECK(module != nullptr);
    using LoadFn = int(__cdecl*)(const SdgoApi*, SdgoMod*);
    using QueryFn = int(__cdecl*)();
    using EligibleFn = int(__cdecl*)(uint32_t);
    auto load = module ? reinterpret_cast<LoadFn>(GetProcAddress(module, "SdgoModLoad")) : nullptr;
    auto active = module ? reinterpret_cast<QueryFn>(GetProcAddress(module, "SdgoAceAiActiveV1")) : nullptr;
    auto eligible = module ? reinterpret_cast<EligibleFn>(GetProcAddress(module, "SdgoAceAiUnitEligibleV1")) : nullptr;
    CHECK(load != nullptr && active != nullptr && eligible != nullptr);
    if (load && active && eligible) {
        CHECK(active() == 0);
        CHECK(eligible(9000000) == 0);
        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.ModDirectory = &ModDirectory;
        api.Log = &Log;
        const int loaded = load(&api, reinterpret_cast<SdgoMod*>(1));
        if (lockedOverlay != INVALID_HANDLE_VALUE) {
            CHECK(CloseHandle(lockedOverlay) != 0);
            lockedOverlay = INVALID_HANDLE_VALUE;
        }
        const bool expectedSuccess = mode == L"success" || mode == L"overlay";
        CHECK((loaded != 0) == expectedSuccess);
        CHECK(active() == (expectedSuccess ? 1 : 0));
        const auto ids = mode == L"missing" ? std::set<uint32_t>{} : UnitIds(base);
        if (expectedSuccess) {
            CHECK(ids.size() >= 1000);
            for (uint32_t id : ids)
                CHECK(eligible(id) == 1);
            CHECK(eligible(0) == 0);
            CHECK(eligible(10000000) == 0);
            CHECK(eligible(9000000) == (mode == L"overlay" ? 1 : 0));

            // A failed reload must revoke the provider instead of retaining a
            // previously active catalog from the same process.
            std::ofstream(directory / L"owner-pilots.json", std::ios::binary | std::ios::trunc)
                << "{not-json";
            CHECK(load(&api, reinterpret_cast<SdgoMod*>(1)) == 0);
            CHECK(active() == 0);
            if (!ids.empty()) CHECK(eligible(*ids.begin()) == 0);
        } else {
            CHECK(eligible(9000000) == 0);
        }
    }
    if (lockedOverlay != INVALID_HANDLE_VALUE) CloseHandle(lockedOverlay);
    if (module) FreeLibrary(module);
    fs::remove_all(root, ignored);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
