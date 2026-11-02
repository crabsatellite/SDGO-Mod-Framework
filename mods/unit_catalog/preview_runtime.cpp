#include <windows.h>
#include <cstring>
#include <string>
#include "preview_runtime.h"

namespace sdgo::unit_preview {
namespace {
uint32_t g_unit = 0, g_category = 0;
std::wstring g_directory;
std::string g_revision, g_nonce;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
using NetworkSendFn = int(__thiscall*)(void*, void*, const void*, int, char);
NetworkSendFn g_send = nullptr;

// Verified original request constructors, not post-purchase refresh packets.
// This guard exists only in a private candidate client, never a player install.
bool SpendingRequest(uint16_t opcode) {
    switch (opcode) {
    case 0x0931: // native shop purchase confirmation
    case 0x0758: // native shop final commit after server preflight/warning
    case 0x06cc: // native gift final confirmation
    case 0x0514: // GP/fixed capsule
    case 0x05a8: // coin capsule
    case 0x05d3: // item machine
    case 0x09b0: // custom capsule
    case 0x072e: // onboarding capsule
        return true;
    default: return false;
    }
}

int __fastcall PreviewSend(void* network, void*, void* stream, const void* packet, int size, char flag) {
    uint16_t opcode = 0;
    __try {
        if (!packet || size < 2) return 0;
        std::memcpy(&opcode, packet, sizeof(opcode));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (SpendingRequest(opcode)) {
        g_api->Log(g_mod, SDGO_LOG_INFO, "private unit preview refused spending request 0x%04X", static_cast<unsigned>(opcode));
        return 0;
    }
    return g_send(network, stream, packet, size, flag);
}

bool Hex(const wchar_t* value, std::string& out) {
    if (wcslen(value) != 64) return false;
    out.clear();
    for (const wchar_t* c = value; *c; ++c) {
        if (!((*c >= L'0' && *c <= L'9') || (*c >= L'a' && *c <= L'f'))) return false;
        out.push_back(static_cast<char>(*c));
    }
    return true;
}

std::string Escape(const char* value) {
    std::string out;
    for (const char* c = value; c && *c; ++c) {
        if (*c == '\\' || *c == '"') out.push_back('\\');
        if (static_cast<unsigned char>(*c) >= 32) out.push_back(*c);
    }
    return out;
}

void Receipt(const char* role, uint32_t unit, bool seen, const char* detail) {
    if (!g_unit || unit != g_unit || (strcmp(role, "shop") && strcmp(role, "capsule"))) return;
    const std::wstring name = !strcmp(role, "shop") ? L"native-preview-shop.json" : L"native-preview-capsule.json";
    const std::wstring path = g_directory + L"\\" + name;
    const std::wstring temporary = path + L".tmp";
    const std::string raw = "{\"schema_version\":1,\"unit_id\":" + std::to_string(unit) +
        ",\"category_id\":" + std::to_string(g_category) + ",\"role\":\"" + role +
        "\",\"content_revision\":\"" + g_revision + "\",\"nonce\":\"" + g_nonce +
        "\",\"native_logic\":" + (seen ? "true" : "false") + ",\"detail\":\"" + Escape(detail) +
        "\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"ui_thread\":" + std::to_string(GetCurrentThreadId()) + "}\n";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    const bool ok = WriteFile(file, raw.data(), static_cast<DWORD>(raw.size()), &wrote, nullptr) && wrote == raw.size();
    if (ok) FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    else DeleteFileW(temporary.c_str());
}
}

bool Target(uint32_t* unit, uint32_t* category) {
    if (!g_unit) return false;
    if (unit) *unit = g_unit;
    if (category) *category = g_category;
    return true;
}

void Seen(const char* role, uint32_t unit, const char* renderer) { Receipt(role, unit, true, renderer); }
void Failed(const char* role, const char* reason) {
    Receipt(role, g_unit, false, reason);
    if (g_api) g_api->Log(g_mod, SDGO_LOG_WARN, "native %s preview pending: %s", role, reason);
}

bool Attach(const SdgoApi* api, SdgoMod* mod, const unit_catalog::Catalog& catalog) {
    g_directory = api->ModDirectory(mod);
    const std::wstring path = g_directory + L"\\native-preview.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    g_api = api; g_mod = mod;
    wchar_t revision[66] = {}, nonce[66] = {};
    GetPrivateProfileStringW(L"preview", L"content_revision", L"", revision, 66, path.c_str());
    GetPrivateProfileStringW(L"preview", L"nonce", L"", nonce, 66, path.c_str());
    const auto unit = GetPrivateProfileIntW(L"preview", L"unit_id", 0, path.c_str());
    const auto category = GetPrivateProfileIntW(L"preview", L"category_id", 0, path.c_str());
    if (!Hex(revision, g_revision) || !Hex(nonce, g_nonce) ||
        !catalog.Contains(unit, unit_catalog::kUnitType) || !catalog.Contains(category, unit_catalog::kItemCategoryType)) return false;
    bool matches = false;
    for (const auto& entry : catalog.records)
        if (entry.kind == unit_catalog::kItemCategoryType && entry.id == category)
            matches = unit_catalog::U32(entry.bytes.data() + unit_catalog::kItemCategoryRefOffset) == unit;
    if (!matches) return false;
    g_unit = unit; g_category = category;
    if (!api->HookCode(mod, "unit_preview.network_send", reinterpret_cast<void*>(&PreviewSend),
                       reinterpret_cast<void**>(&g_send))) return false;
    if (!InstallShop(api, mod) || !InstallCapsule(api, mod)) return false;
    api->Log(mod, SDGO_LOG_INFO, "private native preview enabled for unit %u; normal player packages never enable this mode", unit);
    return true;
}
}
