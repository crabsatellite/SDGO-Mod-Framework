#include "room_style_preview_overlay.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"

namespace room_style_preview_overlay {
namespace {

constexpr uint32_t kExpectedSchema = 1;
constexpr uint32_t kExpectedChunkSize = 512;
#if ROOM_STYLE_PREVIEW_MISSION_OVERLAY
constexpr wchar_t kResourceName[] = L"uirs\\0586.uir";
constexpr wchar_t kUiRelative[] = L"ui\\0586.uir";
constexpr char kResourceLogName[] = "uirs\\0586.uir";
constexpr char kOverlayPurpose[] = "six-slot cooperative mission";
#else
constexpr wchar_t kResourceName[] = L"uirs\\0373.uir";
constexpr wchar_t kUiRelative[] = L"ui\\0373.uir";
constexpr char kResourceLogName[] = "uirs\\0373.uir";
constexpr char kOverlayPurpose[] = "12-slot normal-room";
#endif

struct NativeFileInfo {
    std::array<uint32_t, 9> words = {};
};

using OpenFileFn = void*(__thiscall*)(void*, const void*, bool);
using FileCtorFn = void*(__thiscall*)(void*, void*, NativeFileInfo&, uint32_t, bool);
using NewFn = void*(__cdecl*)(size_t);
using WfopenFn = void*(__cdecl*)(const wchar_t*, const wchar_t*);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
OpenFileFn g_original_open = nullptr;
FileCtorFn g_file_ctor = nullptr;
NewFn g_operator_new = nullptr;
WfopenFn g_wfopen = nullptr;
std::wstring g_zpk_path;
NativeFileInfo g_entry;
volatile LONG g_opened = 0;

std::wstring Join(const std::wstring& root, const wchar_t* child) {
    return root + (root.empty() || root.back() == L'\\' ? L"" : L"\\") + child;
}

std::wstring IniValue(
    const std::wstring& ini,
    const wchar_t* section,
    const wchar_t* key) {
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(
        section, key, L"", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    return value;
}

bool ParseUnsigned(const std::wstring& text, uint32_t* out) {
    if (!out || text.empty())
        return false;
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text.c_str(), &end, 10);
    if (!end || *end != L'\0' ||
        value > (std::numeric_limits<uint32_t>::max)())
        return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

bool ValidHash(const std::wstring& value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](wchar_t character) {
            return
                (character >= L'0' && character <= L'9') ||
                (character >= L'a' && character <= L'f') ||
                (character >= L'A' && character <= L'F');
        });
}

std::string LowerHash(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (wchar_t character : value) {
        if (character >= L'A' && character <= L'F')
            character = static_cast<wchar_t>(character + (L'a' - L'A'));
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::string Hex(const std::array<uint8_t, 32>& hash) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t index = 0; index < hash.size(); ++index) {
        result[index * 2] = digits[hash[index] >> 4];
        result[index * 2 + 1] = digits[hash[index] & 0x0f];
    }
    return result;
}

bool HashFile(
    const std::wstring& path,
    std::string* hash,
    uint64_t* size,
    std::string* error) {
    if (!hash || !error)
        return false;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = "cannot open generated overlay file";
        return false;
    }
    LARGE_INTEGER length = {};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) {
        CloseHandle(file);
        *error = "cannot size generated overlay file";
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE context = nullptr;
    DWORD objectSize = 0;
    DWORD returned = 0;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> digest = {};
    bool ok =
        BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &returned, 0) >= 0 &&
        objectSize != 0;
    if (ok) {
        object.resize(objectSize);
        ok = BCryptCreateHash(
            algorithm, &context, object.data(), objectSize,
            nullptr, 0, 0) >= 0;
    }
    std::array<uint8_t, 64 * 1024> buffer = {};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(
                file, buffer.data(), static_cast<DWORD>(buffer.size()),
                &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0)
            break;
        if (BCryptHashData(context, buffer.data(), read, 0) < 0) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = BCryptFinishHash(
            context, digest.data(),
            static_cast<ULONG>(digest.size()), 0) >= 0;
    if (context)
        BCryptDestroyHash(context);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) {
        *error = "cannot SHA-256 generated overlay file";
        return false;
    }
    *hash = Hex(digest);
    if (size)
        *size = static_cast<uint64_t>(length.QuadPart);
    return true;
}

bool VerifyFile(
    const std::wstring& path,
    const std::wstring& expected,
    uint64_t* size,
    std::string* error) {
    if (!ValidHash(expected)) {
        *error = "runtime.ini contains an invalid SHA-256";
        return false;
    }
    std::string actual;
    if (!HashFile(path, &actual, size, error))
        return false;
    if (actual != LowerHash(expected)) {
        *error = "generated overlay SHA-256 differs";
        return false;
    }
    return true;
}

bool ReadFileBounded(
    const std::wstring& path,
    size_t limit,
    std::vector<uint8_t>* bytes,
    std::string* error) {
    if (!bytes || !error)
        return false;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = "cannot open generated overlay index";
        return false;
    }
    LARGE_INTEGER length = {};
    bool ok =
        GetFileSizeEx(file, &length) &&
        length.QuadPart >= 0 &&
        static_cast<uint64_t>(length.QuadPart) <= limit;
    if (ok) {
        bytes->resize(static_cast<size_t>(length.QuadPart));
        size_t cursor = 0;
        while (cursor < bytes->size()) {
            DWORD read = 0;
            const DWORD want = static_cast<DWORD>((std::min)(
                bytes->size() - cursor, static_cast<size_t>(1u << 20)));
            if (!ReadFile(file, bytes->data() + cursor, want, &read, nullptr) ||
                read != want) {
                ok = false;
                break;
            }
            cursor += read;
        }
    }
    CloseHandle(file);
    if (!ok) {
        *error = "generated overlay index is unreadable or oversized";
        return false;
    }
    return true;
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool ParseIndex(
    const std::vector<uint8_t>& bytes,
    uint64_t zpkSize,
    NativeFileInfo* entry,
    std::string* error) {
    if (!entry || !error || bytes.size() < 24) {
        if (error)
            *error = "generated overlay index is truncated";
        return false;
    }
    const uint32_t version = ReadU32(bytes, 0);
    const uint32_t chunkSize = ReadU32(bytes, 4);
    const uint32_t chunkCount = ReadU32(bytes, 8);
    const uint32_t entryCount = ReadU32(bytes, 16);
    if (version != 1 || chunkSize != kExpectedChunkSize || entryCount != 1 ||
        zpkSize !=
            static_cast<uint64_t>(chunkCount) * (chunkSize + 8u)) {
        *error = "generated overlay archive header differs";
        return false;
    }
    size_t cursor = 24;
    std::wstring name;
    bool terminated = false;
    for (size_t count = 0; count < 260; ++count) {
        if (cursor + 2 > bytes.size()) {
            *error = "generated overlay path is truncated";
            return false;
        }
        const uint16_t encoded = static_cast<uint16_t>(
            bytes[cursor] | (bytes[cursor + 1] << 8));
        cursor += 2;
        wchar_t value = static_cast<wchar_t>(encoded ^ 0x00AA);
        if (value == L'\0') {
            terminated = true;
            break;
        }
        if (value == L'/')
            value = L'\\';
        if (value >= L'A' && value <= L'Z')
            value = static_cast<wchar_t>(value + (L'a' - L'A'));
        name.push_back(value);
    }
    if (!terminated || name != kResourceName ||
        cursor + sizeof(entry->words) != bytes.size()) {
        *error = "generated overlay resource identity differs";
        return false;
    }
    for (size_t index = 0; index < entry->words.size(); ++index)
        entry->words[index] = ReadU32(bytes, cursor + index * 4);
    const uint32_t storedBytes = entry->words[0];
    const uint32_t firstChunk = entry->words[1];
    const uint32_t chunks = entry->words[2];
    if (storedBytes == 0 || chunks == 0 ||
        static_cast<uint64_t>(storedBytes) >
            static_cast<uint64_t>(chunks) * chunkSize ||
        static_cast<uint64_t>(firstChunk) + chunks > chunkCount ||
        std::any_of(
            entry->words.begin() + 3, entry->words.end(),
            [](uint32_t word) { return word != 0; })) {
        *error = "generated overlay resource metadata differs";
        return false;
    }
    return true;
}

bool TryNativeName(
    const void* nativeString,
    const wchar_t** text,
    size_t* length) {
    if (!nativeString || !text || !length)
        return false;
    __try {
        const auto* bytes = static_cast<const uint8_t*>(nativeString);
        const size_t size =
            *reinterpret_cast<const uint32_t*>(bytes + 0x10);
        const size_t capacity =
            *reinterpret_cast<const uint32_t*>(bytes + 0x14);
        if (size > 259 || capacity < size)
            return false;
        const wchar_t* value =
            capacity < 8
                ? reinterpret_cast<const wchar_t*>(nativeString)
                : *reinterpret_cast<const wchar_t* const*>(nativeString);
        if (!value || value[size] != L'\0')
            return false;
        *text = value;
        *length = size;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsRoomResource(const void* nativeName) {
    const wchar_t* text = nullptr;
    size_t length = 0;
    if (!TryNativeName(nativeName, &text, &length))
        return false;
    if (length != std::size(kResourceName) - 1)
        return false;
    for (size_t index = 0; index < length; ++index) {
        wchar_t value = text[index];
        if (value == L'/')
            value = L'\\';
        if (value >= L'A' && value <= L'Z')
            value = static_cast<wchar_t>(value + (L'a' - L'A'));
        if (value != kResourceName[index])
            return false;
    }
    return true;
}

void* CreateNativeFile(bool fullMemory) {
    void* file = g_wfopen(g_zpk_path.c_str(), L"rb");
    if (!file)
        return nullptr;
    void* storage = g_operator_new(0x7c);
    if (!storage)
        return nullptr;
    __try {
        return g_file_ctor(
            storage, file, g_entry, kExpectedChunkSize, fullMemory);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* __fastcall OpenFileDetour(
    void* pack,
    void*,
    const void* nativeName,
    bool fullMemory) {
    if (IsRoomResource(nativeName)) {
        if (void* file = CreateNativeFile(fullMemory)) {
            if (InterlockedCompareExchange(&g_opened, 1, 0) == 0) {
                g_api->Log(
                    g_mod, SDGO_LOG_INFO,
                    "room style preview opened generated native %s",
                    kResourceLogName);
            }
            return file;
        }
        g_api->Log(
            g_mod, SDGO_LOG_ERROR,
            "room style preview overlay open failed; using stock %s",
            kResourceLogName);
    }
    return g_original_open(pack, nativeName, fullMemory);
}

template<class Function>
bool Bind(Function* target, const char* symbol) {
    if (!target)
        return false;
    *target = reinterpret_cast<Function>(g_api->Symbol(g_mod, symbol));
    return *target != nullptr;
}

} // namespace

bool Initialize(const SdgoApi* api, SdgoMod* mod) {
    if (!api || !mod || !api->Symbol || !api->HookCode ||
        !api->ModDirectory || !api->Log)
        return false;
    g_api = api;
    g_mod = mod;

    const std::wstring root = api->ModDirectory(mod);
    const std::wstring ini = Join(root, L"runtime.ini");
    uint32_t schema = 0;
    if (!ParseUnsigned(IniValue(ini, L"overlay", L"schema"), &schema) ||
        schema != kExpectedSchema ||
        IniValue(ini, L"overlay", L"overlay") != L"overlay" ||
        IniValue(ini, L"overlay", L"ui") != kUiRelative) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "room style preview generated runtime.ini is missing or unsupported");
        return false;
    }

    std::string error;
    uint64_t zpkSize = 0;
    if (!VerifyFile(
            Join(root, kUiRelative),
            IniValue(ini, L"overlay", L"ui_sha256"),
            nullptr, &error) ||
        !VerifyFile(
            Join(root, L"overlay.zdx"),
            IniValue(ini, L"overlay", L"overlay_zdx_sha256"),
            nullptr, &error) ||
        !VerifyFile(
            Join(root, L"overlay.zpk"),
            IniValue(ini, L"overlay", L"overlay_zpk_sha256"),
            &zpkSize, &error)) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "room style preview generated overlay rejected: %s",
            error.c_str());
        return false;
    }
    std::vector<uint8_t> index;
    if (!ReadFileBounded(
            Join(root, L"overlay.zdx"), 4096, &index, &error) ||
        !ParseIndex(index, zpkSize, &g_entry, &error)) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "room style preview generated overlay rejected: %s",
            error.c_str());
        return false;
    }
    g_zpk_path = Join(root, L"overlay.zpk");

    if (!Bind(&g_file_ctor, "capsule.main.file_ctor") ||
        !Bind(&g_operator_new, "capsule.main.operator_new") ||
        !Bind(&g_wfopen, "capsule.main.wfopen")) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "room style preview native pack symbols are unavailable");
        return false;
    }
    if (!api->HookCode(
            mod, "capsule.main.open_file",
            reinterpret_cast<void*>(&OpenFileDetour),
            reinterpret_cast<void**>(&g_original_open))) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "room style preview cannot own the native pack hook");
        return false;
    }
    api->Log(
        mod, SDGO_LOG_INFO,
        "room style preview verified one-resource %s overlay",
        kOverlayPurpose);
    return true;
}

} // namespace room_style_preview_overlay
