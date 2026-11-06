// Unshipped observer for a controlled, table-ready native test backend. It only
// calls the verified existing skill lookup and reads the four-byte identity.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "../mods/battle_ai/native_loadout.h"

namespace {
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
void* g_registry = nullptr;
battle_ai::loadout::FindServerSkillFn g_find = nullptr;
std::vector<std::uint32_t> g_ids;
std::wstring g_request, g_output;
char g_token[80] = {};
wchar_t g_wideToken[80] = {};

bool Query(std::uint32_t identity, void** record, std::uint32_t* observed, DWORD* exception) {
    __try {
        *record = nullptr; *observed = 0;
        g_find(g_registry, identity, record);
        if (*record) std::memcpy(observed, *record, 4);
        return *record && *observed == identity;
    } __except(EXCEPTION_EXECUTE_HANDLER) { *exception = GetExceptionCode(); return false; }
}

DWORD WINAPI Observe(void*) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 900000) {
        wchar_t token[80] = {};
        GetPrivateProfileStringW(L"probe", L"token", L"", token, 80, g_request.c_str());
        if (!std::wcscmp(token, g_wideToken)) {
            FILE* output = nullptr;
            if (_wfopen_s(&output, g_output.c_str(), L"wb") || !output) return 3;
            std::fprintf(output, "{\"schema\":1,\"token\":\"%s\",\"process_id\":%lu,\"observer_thread_id\":%lu,\"skills\":[",
                         g_token, GetCurrentProcessId(), GetCurrentThreadId());
            bool passed = true;
            for (std::size_t i = 0; i < g_ids.size(); ++i) {
                void* record = nullptr; std::uint32_t observed = 0; DWORD exception = 0;
                const bool valid = Query(g_ids[i], &record, &observed, &exception);
                passed = passed && valid;
                if (i) std::fputc(',', output);
                std::fprintf(output, "{\"id\":%lu,\"record\":\"0x%08lX\",\"observed_id\":%lu,\"found\":%s,\"exception\":%lu}",
                    static_cast<DWORD>(g_ids[i]), reinterpret_cast<DWORD>(record), static_cast<DWORD>(observed), valid ? "true" : "false", exception);
            }
            std::fprintf(output, "],\"passed\":%s,\"record_bytes_read\":4,\"read_only\":true,\"actor_accepted\":false}\n", passed ? "true" : "false");
            std::fclose(output);
            g_api->Log(g_mod, SDGO_LOG_INFO, "table-ready skill registry observer completed: %s", passed ? "pass" : "fail");
            return passed ? 0 : 1;
        }
        Sleep(200);
    }
    return 2;
}
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION) return 0;
    char ids[1024] = {};
    api->SettingString(mod, "SkillIds", "", ids, sizeof(ids));
    api->SettingString(mod, "Token", "", g_token, sizeof(g_token));
    if (std::strlen(g_token) != 36) return 0;
    for (const char* p = g_token; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-')) return 0;
    for (std::size_t i = 0; i < std::strlen(g_token); ++i) g_wideToken[i] = static_cast<wchar_t>(g_token[i]);
    const char* cursor = ids;
    while (*cursor) {
        std::uint32_t value = 0; bool digit = false;
        while (*cursor >= '0' && *cursor <= '9') {
            digit = true;
            const auto next = value * 10 + static_cast<std::uint32_t>(*cursor++ - '0');
            if (next > 9999999 || next < value) return 0;
            value = next;
        }
        if (!digit || !value || g_ids.size() >= 64) return 0;
        g_ids.push_back(value);
        if (!*cursor) break;
        if (*cursor++ != ',' || !*cursor) return 0;
    }
    if (g_ids.empty()) return 0;
    g_registry = api->Symbol(mod, "loadout.skill_registry");
    g_find = reinterpret_cast<battle_ai::loadout::FindServerSkillFn>(api->Symbol(mod, "loadout.find_skill"));
    if (!g_registry || !g_find) return 0;
    g_request = std::wstring(api->ModDirectory(mod)) + L"\\query.request.ini";
    g_output = std::wstring(api->ModDirectory(mod)) + L"\\registry-result.json";
    HANDLE worker = CreateThread(nullptr, 0, Observe, nullptr, 0, nullptr);
    if (!worker) return 0;
    CloseHandle(worker); return 1;
}
