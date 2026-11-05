#include "program_runtime.h"
#include "sdgo/native_script_registration.h"
#include "sdgo/unit_program_format.h"
#include "sdgo/unit_program_state.h"
#include <windows.h>
#include <memory>

namespace sdgo::unit_programs {
namespace {
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
const bool* g_recordsReady = nullptr;
sdgo::unit_catalog::ProgramBundle g_bundle;
sdgo::scripts::NativeLuaCalls g_calls;
sdgo::scripts::OriginalScriptLoader g_original = nullptr;
std::unique_ptr<sdgo::scripts::NativeRegistration> g_registration;
SRWLOCK g_lock = SRWLOCK_INIT;
SdgoUnitProgramStateV1 g_state = {};

void __cdecl Close(void*, const char* reason) {
    AcquireSRWLockExclusive(&g_lock); g_state.fatal = 1; g_state.active = 0; ReleaseSRWLockExclusive(&g_lock);
    g_api->Log(g_mod, SDGO_LOG_ERROR, "native unit program registration failed: %s; closing partial candidate client", reason);
    TerminateProcess(GetCurrentProcess(), 0xE0C00114u);
    ExitProcess(0xE0C00114u);
}

std::string CatalogHash(const sdgo::unit_catalog::Catalog& catalog) {
    using namespace sdgo::unit_catalog;
    std::vector<uint8_t> bytes(sizeof(Header));
    for (const auto& entry : catalog.records) {
        EntryHeader header = {entry.kind, entry.id, static_cast<uint32_t>(entry.bytes.size()), entry.recordHash, 0, 0};
        const auto* start = reinterpret_cast<const uint8_t*>(&header);
        bytes.insert(bytes.end(), start, start + sizeof(header));
        bytes.insert(bytes.end(), entry.bytes.begin(), entry.bytes.end());
    }
    Header header = {};
    std::memcpy(header.magic, "SDGOUN1\0", 8); header.version = 1; header.headerBytes = sizeof(Header);
    header.count = static_cast<uint32_t>(catalog.records.size());
    header.payloadBytes = static_cast<uint32_t>(bytes.size() - sizeof(Header));
    header.payloadHash = Hash32(bytes.data() + sizeof(Header), header.payloadBytes);
    std::memcpy(bytes.data(), &header, sizeof(header));
    return sdgo::scripts::Sha256(bytes);
}

bool Read(const std::wstring& path, std::vector<uint8_t>& bytes, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "native program sidecar cannot be opened"; return false; }
    BY_HANDLE_FILE_INFORMATION info = {}; LARGE_INTEGER size = {};
    bool ok = GetFileInformationByHandle(file, &info) && GetFileSizeEx(file, &size) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
        size.QuadPart >= 64 && size.QuadPart <= 64 + 128LL * 1024 * 1024;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart)); DWORD received = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr) && received == bytes.size();
    }
    CloseHandle(file);
    if (!ok) { bytes.clear(); error = "native program sidecar extent/link/read failed"; }
    return ok;
}

void __fastcall Load(void* engine, void*, sdgo::scripts::NativeWideString* path) {
    // The adapter independently verifies this boundary too. The extra gate
    // prevents script publication after incomplete native record registration.
    const bool root = path && path->length == 12 && path->capacity >= 12 && path->value.pointer &&
        path->value.pointer[12] == 0 && (!std::memcmp(path->value.pointer, L"scp\\0000.scp", 24) ||
                                        !std::memcmp(path->value.pointer, L"scp/0000.scp", 24));
    if (root && (!g_recordsReady || !*g_recordsReady)) Close(nullptr, "native catalog did not finish registration before root");
    const auto result = g_registration->onOriginalLoadBatch(engine, path, g_bundle.candidates);
    if (!root) return;
    if (!result.active()) Close(nullptr, result.reason.c_str());
    AcquireSRWLockExclusive(&g_lock);
    ++g_state.root_calls; g_state.active = 1; g_state.records_ready = 1;
    g_state.root_thread_id = GetCurrentThreadId();
    g_state.vm = reinterpret_cast<uint32_t>(g_calls.state(nullptr));
    g_state.export_count = static_cast<uint32_t>(result.exports.size());
    if (!result.exports.empty()) {
        g_state.first_export_function = static_cast<uint32_t>(result.exports[0].functionIdentity);
        std::memcpy(g_state.first_export_name, result.exports[0].name.c_str(), result.exports[0].name.size() + 1);
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_api->Log(g_mod, SDGO_LOG_INFO, "native unit programs registered after original root: %u owners, %u exports",
               static_cast<unsigned>(g_bundle.candidates.size()), static_cast<unsigned>(result.exports.size()));
}
}

bool Attach(const SdgoApi* api, SdgoMod* mod, const sdgo::unit_catalog::Catalog& catalog, const bool* recordsReady) {
    const std::wstring path = std::wstring(api->ModDirectory(mod)) + L"\\native-programs.bin";
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    g_api = api; g_mod = mod; g_recordsReady = recordsReady;
    std::string error; std::vector<uint8_t> bytes;
    if (!sdgo::scripts::ProcessInitialThreadId(api)) error = "native programs require a loader-provided initial thread identity";
    else if (!Read(path, bytes, error)) {}
    else if (!g_bundle.Parse(bytes, CatalogHash(catalog), catalog, error)) {}
    else if (!sdgo::scripts::BindNativeLuaCalls(api, mod, g_calls, error)) {}
    if (!error.empty()) { api->Log(mod, SDGO_LOG_ERROR, "%s", error.c_str()); return false; }
    g_state.size = sizeof(g_state); g_state.version = 1;
    g_state.initial_thread_id = sdgo::scripts::ProcessInitialThreadId(api);
    g_state.owner_count = static_cast<uint32_t>(g_bundle.candidates.size());
    for (const auto& candidate : g_bundle.candidates) g_state.program_count += static_cast<uint32_t>(candidate.programs.size());
    const auto hash = sdgo::scripts::Sha256(bytes);
    std::memcpy(g_state.bundle_sha256, hash.c_str(), 65);
    if (!api->HookCode(mod, "lua.script_load", reinterpret_cast<void*>(&Load), reinterpret_cast<void**>(&g_original))) return false;
    g_registration = std::make_unique<sdgo::scripts::NativeRegistration>(
        g_calls, g_original, sdgo::scripts::ProcessInitialThreadId(api), Close, nullptr);
    return true;
}

int ReadState(SdgoUnitProgramStateV1* output) {
    if (!output || output->size != sizeof(*output) || output->version != 1) return 0;
    AcquireSRWLockShared(&g_lock); *output = g_state; ReleaseSRWLockShared(&g_lock);
    return output->version == 1;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoReadUnitProgramStateV1(SdgoUnitProgramStateV1* output) {
    return sdgo::unit_programs::ReadState(output);
}
