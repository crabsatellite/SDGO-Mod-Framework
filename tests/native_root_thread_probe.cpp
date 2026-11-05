// Opt-in, unshipped native probe. Content bytes are sealed by GameTest, never
// embedded here. This observes root registration only, with no actor or SQL.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include "sdgo/native_script_registration.h"
#include "sdgo/unit_program_state.h"

namespace {
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
sdgo::scripts::OriginalScriptLoader g_original = nullptr;
sdgo::scripts::NativeLuaCalls g_calls;
sdgo::scripts::Candidate g_candidate;
std::unique_ptr<sdgo::scripts::NativeRegistration> g_registration;
DWORD g_loadThread = 0;
char g_run[80] = {};
std::wstring g_output;
char g_expectedBundle[80] = {}, g_expectedProgram[80] = {}, g_expectedExport[160] = {};

void __cdecl Close(void*, const char* reason) {
    g_api->Log(g_mod, SDGO_LOG_ERROR, "root probe stopped: %s", reason);
    TerminateProcess(GetCurrentProcess(), 0xE0C00113u);
    ExitProcess(0xE0C00113u);
}

bool Text(const char* key, char* output, uint32_t capacity) {
    const uint32_t size = g_api->SettingString(g_mod, key, "", output, capacity);
    return size > 0 && size + 1 < capacity && output[0];
}

bool Root(const sdgo::scripts::NativeWideString* path) {
    if (!path || path->length != 12 || path->capacity < 12) return false;
    const auto* text = path->value.pointer;
    return text && text[12] == 0 &&
        (!std::memcmp(text, L"scp\\0000.scp", 24) || !std::memcmp(text, L"scp/0000.scp", 24));
}

DWORD WINAPI ObserveProduct(void*) {
    const auto started = GetTickCount64();
    HMODULE module = nullptr;
    SdgoGetUnitProgramStateV1 read = nullptr;
    while (GetTickCount64() - started < 90000) {
        if (!module && GetModuleHandleExW(0, L"unit_catalog.dll", &module))
            read = reinterpret_cast<SdgoGetUnitProgramStateV1>(GetProcAddress(module, "SdgoReadUnitProgramStateV1"));
        SdgoUnitProgramStateV1 state = {}; state.size = sizeof(state); state.version = 1;
        if (read && read(&state) && state.active && state.records_ready && state.root_calls) {
            if (state.fatal || state.owner_count != 1 || state.program_count != 1 || state.export_count != 1 ||
                !state.vm || !state.first_export_function || state.initial_thread_id != sdgo::scripts::ProcessInitialThreadId(g_api) ||
                state.root_thread_id != state.initial_thread_id || std::strcmp(state.bundle_sha256, g_expectedBundle) ||
                std::strcmp(state.first_export_name, g_expectedExport)) Close(nullptr, "product program snapshot differs from sealed candidate");
            FILE* output = nullptr;
            if (_wfopen_s(&output, g_output.c_str(), L"wb") || !output) Close(nullptr, "product receipt unavailable");
            std::fprintf(output,
                "{\"schema\":1,\"run_id\":\"%s\",\"process_id\":%lu,\"api_initial_thread_id\":%lu,"
                "\"mod_load_thread_id\":%lu,\"root_thread_id\":%lu,\"observer_thread_id\":%lu,"
                "\"vm\":\"0x%08lX\",\"registered\":true,\"execution_started\":true,"
                "\"product_consumer\":true,\"bundle_sha256\":\"%s\",\"program_sha256\":\"%s\","
                "\"exports\":[{\"name\":\"%s\",\"function\":\"0x%08lX\"}],\"actor_accepted\":false}\n",
                g_run, GetCurrentProcessId(), static_cast<DWORD>(state.initial_thread_id), g_loadThread,
                static_cast<DWORD>(state.root_thread_id), GetCurrentThreadId(), static_cast<DWORD>(state.vm),
                state.bundle_sha256, g_expectedProgram, state.first_export_name, static_cast<DWORD>(state.first_export_function));
            std::fclose(output); if (module) FreeLibrary(module); return 0;
        }
        Sleep(50);
    }
    if (module) FreeLibrary(module);
    return 1;
}

void __fastcall Load(void* engine, void*, sdgo::scripts::NativeWideString* path) {
    const bool root = Root(path);
    const auto result = g_registration->onOriginalLoad(engine, path, g_candidate);
    if (!root) return;
    // Both identity and closure readback refer to this same original VM/root.
    FILE* output = nullptr;
    if (_wfopen_s(&output, g_output.c_str(), L"wb") || !output) Close(nullptr, "receipt unavailable");
    const auto vm = g_calls.state(nullptr);
    std::fprintf(output,
        "{\"schema\":1,\"run_id\":\"%s\",\"process_id\":%lu,"
        "\"api_initial_thread_id\":%lu,\"mod_load_thread_id\":%lu,\"root_thread_id\":%lu,"
        "\"vm\":\"0x%08lX\",\"registered\":%s,\"execution_started\":%s,"
        "\"program_sha256\":\"%s\",\"exports\":[",
        g_run, GetCurrentProcessId(), sdgo::scripts::ProcessInitialThreadId(g_api),
        g_loadThread, GetCurrentThreadId(), reinterpret_cast<DWORD>(vm),
        result.active() ? "true" : "false", result.executionStarted ? "true" : "false",
        g_candidate.programs[0].sha256.c_str());
    for (size_t i = 0; i < result.exports.size(); ++i) {
        if (i) std::fputc(',', output);
        std::fprintf(output, "{\"name\":\"%s\",\"function\":\"0x%08lX\"}",
            result.exports[i].name.c_str(), static_cast<DWORD>(result.exports[i].functionIdentity));
    }
    std::fputs("],\"actor_accepted\":false}\n", output);
    std::fclose(output);
    if (!result.active()) Close(nullptr, result.reason.c_str());
}
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod; g_loadThread = GetCurrentThreadId();
    if (!api || !sdgo::scripts::ProcessInitialThreadId(api)) return 0;
    if (GetEnvironmentVariableA("SDGO_GAME_TEST_RUN", g_run, sizeof(g_run)) != 36) return 0;
    g_output = std::wstring(api->ModDirectory(mod)) + L"\\..\\..\\gametest\\native-root-receipt.json";
    if (api->SettingInt(mod, "ObserveProduct", 0)) {
        if (!Text("BundleSha256", g_expectedBundle, sizeof(g_expectedBundle)) ||
            !Text("Sha256", g_expectedProgram, sizeof(g_expectedProgram)) ||
            !Text("Export", g_expectedExport, sizeof(g_expectedExport))) return 0;
        HANDLE worker = CreateThread(nullptr, 0, ObserveProduct, nullptr, 0, nullptr);
        if (!worker) return 0;
        CloseHandle(worker); return 1;
    }
    char owner[80] = {}, revision[80] = {}, resource[240] = {}, digest[80] = {}, exportName[160] = {};
    if (!Text("Owner", owner, sizeof(owner)) || !Text("Revision", revision, sizeof(revision)) ||
        !Text("Resource", resource, sizeof(resource)) || !Text("Sha256", digest, sizeof(digest)) ||
        !Text("Export", exportName, sizeof(exportName))) return 0;
    for (const char* p = g_run; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
    const std::wstring directory = api->ModDirectory(mod);
    const std::wstring input = directory + L"\\program.scp";
    const DWORD attributes = GetFileAttributesW(input.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return 0;
    std::ifstream file(input, std::ios::binary | std::ios::ate);
    const auto extent = file.tellg();
    if (!file || extent <= 0 || extent > 8 * 1024 * 1024) return 0;
    file.seekg(0);
    sdgo::scripts::Program program;
    program.bytes.assign(std::istreambuf_iterator<char>(file), {});
    if (program.bytes.size() != static_cast<size_t>(extent)) return 0;
    program.resource = resource; program.sha256 = digest; program.exports = {exportName};
    g_candidate = {owner, revision, {std::move(program)}};
    const auto validation = sdgo::scripts::Validate(g_candidate);
    if (!validation.valid) { api->Log(mod, SDGO_LOG_ERROR, "%s", validation.reason.c_str()); return 0; }
    std::string error;
    if (!sdgo::scripts::BindNativeLuaCalls(api, mod, g_calls, error)) return 0;
    if (!api->HookCode(mod, "lua.script_load", reinterpret_cast<void*>(&Load), reinterpret_cast<void**>(&g_original))) return 0;
    g_registration = std::make_unique<sdgo::scripts::NativeRegistration>(
        g_calls, g_original, sdgo::scripts::ProcessInitialThreadId(api), Close, nullptr);
    return 1;
}
