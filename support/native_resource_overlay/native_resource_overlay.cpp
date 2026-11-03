#include "native_resource_overlay.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    constexpr uint32_t kBrokerVersion = 1;
    constexpr uint32_t kSchema = 1;
    constexpr uint32_t kChunkSize = 512;
    constexpr uint32_t kMaxEntries = 100000;
    constexpr uint64_t kMaxIndexBytes = 64ull * 1024ull * 1024ull;
    constexpr uint64_t kMaxArchiveBytes = 512ull * 1024ull * 1024ull;
    constexpr uint32_t kMaxStoredBytes = 64u * 1024u * 1024u;
    constexpr size_t kMaxPathCharacters = 240;

    using OpenFileFn = void*(__thiscall*)(void*, const void*, bool);
    using FileCtorFn = void*(__thiscall*)(
        void*, void*, sdgo::resource_overlay::NativeFileInfo&, uint32_t, bool);
    using NewFn = void*(__cdecl*)(size_t);
    using WfopenFn = void*(__cdecl*)(const wchar_t*, const wchar_t*);

    std::string Narrow(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t value : text)
            result.push_back(value >= 0x20 && value < 0x7f ? static_cast<char>(value) : '?');
        return result;
    }

    std::wstring Join(const std::wstring& root, const std::wstring& relative)
    {
        return root + (root.empty() || root.back() == L'\\' ? L"" : L"\\") + relative;
    }

    std::wstring IniValue(const std::wstring& ini, const wchar_t* section, const wchar_t* key)
    {
        wchar_t value[1024] = {};
        GetPrivateProfileStringW(section, key, L"", value,
                                 static_cast<DWORD>(std::size(value)), ini.c_str());
        return value;
    }

    bool ParseUnsigned(const std::wstring& text, uint32_t& value)
    {
        if (text.empty())
            return false;
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
        if (!end || *end != L'\0' || parsed > (std::numeric_limits<uint32_t>::max)())
            return false;
        value = static_cast<uint32_t>(parsed);
        return true;
    }

    bool ValidHash(const std::wstring& text)
    {
        return text.size() == 64 &&
               std::all_of(text.begin(), text.end(), [](wchar_t value) {
                   return (value >= L'0' && value <= L'9') ||
                          (value >= L'a' && value <= L'f') ||
                          (value >= L'A' && value <= L'F');
               });
    }

    std::string LowerHash(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t value : text) {
            if (value >= L'A' && value <= L'F')
                value = static_cast<wchar_t>(value + (L'a' - L'A'));
            result.push_back(static_cast<char>(value));
        }
        return result;
    }

    bool SafeRelativeFile(const std::wstring& path)
    {
        if (path.empty() || path.size() > kMaxPathCharacters || path[0] == L'\\' ||
            path[0] == L'/' || path.find(L':') != std::wstring::npos)
            return false;
        size_t begin = 0;
        while (begin <= path.size()) {
            const size_t end = path.find_first_of(L"\\/", begin);
            const std::wstring part =
                path.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
            if (part.empty() || part == L"." || part == L".." ||
                part.back() == L'.' || part.back() == L' ')
                return false;
            for (wchar_t value : part) {
                if (value < 0x20 || value > 0x7e ||
                    wcschr(L":*?\"<>|", value) != nullptr)
                    return false;
            }
            if (end == std::wstring::npos)
                break;
            begin = end + 1;
        }
        return true;
    }

    bool ReadFileLimited(const std::wstring& path, uint64_t limit,
                         std::vector<uint8_t>& bytes, std::string& error)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "cannot open " + Narrow(path);
            return false;
        }
        LARGE_INTEGER length = {};
        bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= 0 &&
                  static_cast<uint64_t>(length.QuadPart) <= limit;
        if (ok) {
            bytes.resize(static_cast<size_t>(length.QuadPart));
            size_t cursor = 0;
            while (cursor < bytes.size()) {
                const DWORD want = static_cast<DWORD>((std::min)(
                    bytes.size() - cursor, static_cast<size_t>(1u << 20)));
                DWORD got = 0;
                if (!ReadFile(file, bytes.data() + cursor, want, &got, nullptr) ||
                    got != want) {
                    ok = false;
                    break;
                }
                cursor += got;
            }
        }
        CloseHandle(file);
        if (!ok) {
            error = "cannot read or file is too large: " + Narrow(path);
            return false;
        }
        return true;
    }

    std::string Hex(const uint8_t* hash, size_t length)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result(length * 2, '0');
        for (size_t index = 0; index < length; ++index) {
            result[index * 2] = digits[hash[index] >> 4];
            result[index * 2 + 1] = digits[hash[index] & 0x0f];
        }
        return result;
    }

    bool HashFile(const std::wstring& path, uint64_t limit, std::string& hash,
                  uint64_t& size, std::string& error)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "cannot open " + Narrow(path);
            return false;
        }
        LARGE_INTEGER length = {};
        if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 ||
            static_cast<uint64_t>(length.QuadPart) > limit) {
            CloseHandle(file);
            error = "cannot size or file is too large: " + Narrow(path);
            return false;
        }

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE state = nullptr;
        std::vector<uint8_t> stateBytes;
        std::array<uint8_t, 32> digest = {};
        DWORD stateSize = 0;
        DWORD digestSize = 0;
        DWORD received = 0;
        bool ok =
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&stateSize), sizeof(stateSize),
                              &received, 0) >= 0 &&
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize),
                              &received, 0) >= 0 &&
            digestSize == digest.size();
        if (ok) {
            stateBytes.resize(stateSize);
            ok = BCryptCreateHash(algorithm, &state, stateBytes.data(), stateSize,
                                  nullptr, 0, 0) >= 0;
        }

        std::array<uint8_t, 64 * 1024> buffer = {};
        while (ok) {
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
                ok = false;
                break;
            }
            if (got == 0)
                break;
            ok = BCryptHashData(state, buffer.data(), got, 0) >= 0;
        }
        if (ok)
            ok = BCryptFinishHash(state, digest.data(),
                                  static_cast<ULONG>(digest.size()), 0) >= 0;
        if (state)
            BCryptDestroyHash(state);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        if (!ok) {
            error = "cannot hash " + Narrow(path);
            return false;
        }
        size = static_cast<uint64_t>(length.QuadPart);
        hash = Hex(digest.data(), digest.size());
        return true;
    }

    uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    bool UnitResourcePath(const std::wstring& path)
    {
        static constexpr std::pair<const wchar_t*, const wchar_t*> domains[] = {
            {L"mdrs", L".mod"}, {L"anrs", L".ani"}, {L"txrs", L".txr"},
            {L"fxrs", L".zfx"}, {L"sprs", L".spr"}, {L"sdrs", L".zsd"},
            {L"scp", L".scp"},
        };
        const size_t slash = path.find(L'\\');
        if (slash == std::wstring::npos || slash == 0 || slash + 1 >= path.size())
            return false;
        const std::wstring root = path.substr(0, slash);
        for (const auto& domain : domains) {
            if (root != domain.first)
                continue;
            const size_t extensionLength = wcslen(domain.second);
            return path.size() > extensionLength &&
                   path.compare(path.size() - extensionLength, extensionLength,
                                domain.second) == 0;
        }
        return false;
    }

    bool NormalizeResourcePath(std::wstring& path)
    {
        if (path.empty() || path.size() > kMaxPathCharacters)
            return false;
        for (wchar_t& value : path) {
            if (value == L'/')
                value = L'\\';
            if (value >= L'A' && value <= L'Z')
                value = static_cast<wchar_t>(value + (L'a' - L'A'));
        }
        return SafeRelativeFile(path);
    }

    bool ParseUnitIndex(const std::vector<uint8_t>& bytes, uint64_t archiveSize,
                        sdgo::resource_overlay::BundleRegistration& bundle,
                        std::string& error)
    {
        if (bytes.size() < 24) {
            error = "resources.zdx header is truncated";
            return false;
        }
        const uint32_t version = ReadU32(bytes, 0);
        const uint32_t chunkSize = ReadU32(bytes, 4);
        const uint32_t chunkCount = ReadU32(bytes, 8);
        const uint32_t reserved0 = ReadU32(bytes, 12);
        const uint32_t entryCount = ReadU32(bytes, 16);
        const uint32_t reserved1 = ReadU32(bytes, 20);
        if (version != 1 || chunkSize != kChunkSize || entryCount > kMaxEntries ||
            reserved0 != 0 || reserved1 != 0) {
            error = "resources.zdx header is unsupported";
            return false;
        }
        const uint64_t physicalSize =
            static_cast<uint64_t>(chunkCount) * (static_cast<uint64_t>(chunkSize) + 8u);
        if (physicalSize != archiveSize) {
            error = "resources.zpk size does not match its chunk table";
            return false;
        }

        size_t cursor = 24;
        for (uint32_t index = 0; index < entryCount; ++index) {
            std::wstring path;
            bool terminated = false;
            for (size_t count = 0; count <= kMaxPathCharacters; ++count) {
                if (cursor + 2 > bytes.size()) {
                    error = "resources.zdx path is truncated";
                    return false;
                }
                const uint16_t encoded =
                    static_cast<uint16_t>(bytes[cursor] | (bytes[cursor + 1] << 8));
                cursor += 2;
                wchar_t value = static_cast<wchar_t>(encoded ^ 0x00aa);
                if (value == L'\0') {
                    terminated = true;
                    break;
                }
                if (value > 0x7e || value < 0x20) {
                    error = "resource path is not safe ASCII";
                    return false;
                }
                path.push_back(value);
            }
            if (!terminated || !NormalizeResourcePath(path) || !UnitResourcePath(path) ||
                cursor + 36 > bytes.size()) {
                error = "resource path or metadata is invalid";
                return false;
            }

            sdgo::resource_overlay::OverlayEntry entry;
            for (size_t word = 0; word < entry.info.words.size(); ++word)
                entry.info.words[word] = ReadU32(bytes, cursor + word * 4);
            cursor += 36;
            const uint32_t storedBytes = entry.info.words[0];
            const uint32_t firstChunk = entry.info.words[1];
            const uint32_t chunks = entry.info.words[2];
            const bool reservedClear =
                std::all_of(entry.info.words.begin() + 3, entry.info.words.end(),
                            [](uint32_t value) { return value == 0; });
            if (storedBytes == 0 || storedBytes > kMaxStoredBytes || chunks == 0 ||
                !reservedClear ||
                static_cast<uint64_t>(storedBytes) >
                    static_cast<uint64_t>(chunks) * chunkSize ||
                static_cast<uint64_t>(firstChunk) + chunks > chunkCount) {
                error = "resource chunk range or metadata is invalid";
                return false;
            }
            if (!bundle.entries.emplace(path, entry).second) {
                error = "resource archive contains a duplicate normalized path";
                return false;
            }
        }
        if (cursor != bytes.size()) {
            error = "resources.zdx has trailing bytes";
            return false;
        }
        bundle.chunkSize = chunkSize;
        return true;
    }

    bool LoadUnitBundle(const wchar_t* modDirectory, const wchar_t* configName,
                        sdgo::resource_overlay::BundleRegistration& bundle,
                        std::string& error)
    {
        if (!modDirectory || !*modDirectory || !configName ||
            wcscmp(configName, L"resource-overlay.ini") != 0) {
            error = "unit_catalog requires resource-overlay.ini";
            return false;
        }
        const std::wstring root(modDirectory);
        const std::wstring ini = Join(root, configName);
        uint32_t schema = 0;
        if (!ParseUnsigned(IniValue(ini, L"runtime", L"schema"), schema) ||
            schema != kSchema) {
            error = "resource-overlay.ini schema is missing or unsupported";
            return false;
        }
        const std::wstring indexName = IniValue(ini, L"runtime", L"index");
        const std::wstring archiveName = IniValue(ini, L"runtime", L"archive");
        const std::wstring indexExpected = IniValue(ini, L"runtime", L"index_sha256");
        const std::wstring archiveExpected = IniValue(ini, L"runtime", L"archive_sha256");
        if (indexName != L"resources.zdx" || archiveName != L"resources.zpk" ||
            !ValidHash(indexExpected) || !ValidHash(archiveExpected)) {
            error = "resource-overlay.ini declarations are invalid";
            return false;
        }

        std::string indexHash;
        std::string archiveHash;
        uint64_t indexSize = 0;
        uint64_t archiveSize = 0;
        const std::wstring indexPath = Join(root, indexName);
        const std::wstring archivePath = Join(root, archiveName);
        if (!HashFile(indexPath, kMaxIndexBytes, indexHash, indexSize, error) ||
            !HashFile(archivePath, kMaxArchiveBytes, archiveHash, archiveSize, error))
            return false;
        if (indexHash != LowerHash(indexExpected)) {
            error = "SHA-256 mismatch for resources.zdx";
            return false;
        }
        if (archiveHash != LowerHash(archiveExpected)) {
            error = "SHA-256 mismatch for resources.zpk";
            return false;
        }

        std::vector<uint8_t> index;
        if (!ReadFileLimited(indexPath, kMaxIndexBytes, index, error))
            return false;
        bundle.owner = "unit_catalog";
        bundle.identity = indexHash + ":" + archiveHash;
        bundle.archivePath = archivePath;
        if (!ParseUnitIndex(index, archiveSize, bundle, error))
            return false;
        return true;
    }

    bool TryMainName(const void* nativeString, std::wstring& result)
    {
        __try {
            const auto* bytes = static_cast<const uint8_t*>(nativeString);
            const size_t size = *reinterpret_cast<const uint32_t*>(bytes + 0x10);
            const size_t capacity = *reinterpret_cast<const uint32_t*>(bytes + 0x14);
            if (size > 259 || capacity < size)
                return false;
            const wchar_t* value = capacity < 8
                ? reinterpret_cast<const wchar_t*>(nativeString)
                : *reinterpret_cast<const wchar_t* const*>(nativeString);
            if (!value || value[size] != L'\0')
                return false;
            result.assign(value, size);
            for (wchar_t& character : result) {
                if (character == L'/')
                    character = L'\\';
                if (character >= L'A' && character <= L'Z')
                    character = static_cast<wchar_t>(character + (L'a' - L'A'));
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* CallNativeConstructor(
        FileCtorFn construct,
        void* storage,
        void* archive,
        sdgo::resource_overlay::NativeFileInfo& info,
        uint32_t chunkSize,
        bool fullMemory)
    {
        __try {
            return construct(storage, archive, info, chunkSize, fullMemory);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void CopyError(char* destination, uint32_t capacity, const std::string& error)
    {
        if (!destination || capacity == 0)
            return;
        const size_t count = (std::min)(static_cast<size_t>(capacity - 1), error.size());
        std::memcpy(destination, error.data(), count);
        destination[count] = '\0';
    }
}

namespace sdgo::resource_overlay
{
    struct Provider::Impl
    {
        struct StoredBundle
        {
            std::string owner;
            std::string identity;
            std::wstring archivePath;
            uint32_t chunkSize = 0;
            std::unordered_map<std::wstring, OverlayEntry> entries;
        };

        struct FoundResource
        {
            bool found = false;
            std::string owner;
            std::wstring path;
            std::wstring archivePath;
            uint32_t chunkSize = 0;
            NativeFileInfo info;
        };

        const SdgoApi* api = nullptr;
        SdgoMod* mod = nullptr;
        OpenFileFn original = nullptr;
        FileCtorFn construct = nullptr;
        NewFn allocate = nullptr;
        WfopenFn open = nullptr;
        OpenObserver observer = nullptr;
        void* observerContext = nullptr;
        bool active = false;
        SRWLOCK lock = SRWLOCK_INIT;
        std::vector<StoredBundle> bundles;

        bool Register(const BundleRegistration& candidate, std::string& error)
        {
            if (candidate.owner.empty() || candidate.identity.empty() ||
                candidate.archivePath.empty() || candidate.chunkSize != kChunkSize) {
                error = "overlay bundle metadata is invalid";
                return false;
            }
            AcquireSRWLockExclusive(&lock);
            for (const StoredBundle& existing : bundles) {
                if (existing.owner == candidate.owner) {
                    if (existing.identity == candidate.identity) {
                        ReleaseSRWLockExclusive(&lock);
                        return true;
                    }
                    ReleaseSRWLockExclusive(&lock);
                    error = "owner already registered a different sealed overlay bundle";
                    return false;
                }
            }
            for (const StoredBundle& existing : bundles) {
                for (const auto& item : candidate.entries) {
                    if (existing.entries.find(item.first) != existing.entries.end()) {
                        ReleaseSRWLockExclusive(&lock);
                        error = "resource path overlaps bundle owned by " + existing.owner +
                                ": " + Narrow(item.first);
                        return false;
                    }
                }
            }
            StoredBundle stored;
            stored.owner = candidate.owner;
            stored.identity = candidate.identity;
            stored.archivePath = candidate.archivePath;
            stored.chunkSize = candidate.chunkSize;
            stored.entries = candidate.entries;
            bundles.push_back(std::move(stored));
            ReleaseSRWLockExclusive(&lock);
            return true;
        }

        FoundResource Find(const std::wstring& path)
        {
            FoundResource result;
            AcquireSRWLockShared(&lock);
            for (const StoredBundle& bundle : bundles) {
                const auto found = bundle.entries.find(path);
                if (found == bundle.entries.end())
                    continue;
                result.found = true;
                result.owner = bundle.owner;
                result.path = path;
                result.archivePath = bundle.archivePath;
                result.chunkSize = bundle.chunkSize;
                result.info = found->second.info;
                break;
            }
            ReleaseSRWLockShared(&lock);
            return result;
        }

        void* Open(void* pack, const void* nativeName, bool fullMemory)
        {
            std::wstring path;
            if (TryMainName(nativeName, path)) {
                FoundResource resource = Find(path);
                if (resource.found) {
                    void* archive = open(resource.archivePath.c_str(), L"rb");
                    if (archive) {
                        void* storage = allocate(0x7c);
                        if (storage) {
                            void* stream = CallNativeConstructor(
                                construct, storage, archive, resource.info,
                                resource.chunkSize, fullMemory);
                            if (stream) {
                                if (observer)
                                    observer(resource.owner.c_str(), resource.path.c_str(),
                                             stream, observerContext);
                                return stream;
                            }
                        }
                    }
                    api->Log(mod, SDGO_LOG_ERROR,
                             "sealed resource overlay open failed for %ls; using stock pack",
                             resource.path.c_str());
                }
            }
            return original(pack, nativeName, fullMemory);
        }
    };

    namespace
    {
        Provider* g_activeProvider = nullptr;

        int __cdecl RegisterBundleThunk(
            const wchar_t* modDirectory,
            const wchar_t* configName,
            const char* owner,
            char* error,
            uint32_t errorCapacity)
        {
            std::string problem;
            bool ok = g_activeProvider &&
                      g_activeProvider->RegisterBundleFiles(
                          modDirectory, configName, owner, problem);
            if (!ok && problem.empty())
                problem = "resource overlay broker is not active";
            CopyError(error, errorCapacity, problem);
            return ok ? 1 : 0;
        }

        const SdgoResourceOverlayBrokerApiV1 kBrokerApi = {
            sizeof(SdgoResourceOverlayBrokerApiV1),
            kBrokerVersion,
            &RegisterBundleThunk,
        };
    }

    Provider::Provider() : impl_(std::make_unique<Impl>())
    {
    }

    Provider::~Provider() = default;

    void* __fastcall Provider::OpenDetour(
        void* pack, void*, const void* nativeName, bool fullMemory)
    {
        return g_activeProvider->impl_->Open(pack, nativeName, fullMemory);
    }

    bool Provider::RegisterTrustedBundle(const BundleRegistration& bundle, std::string& error)
    {
        return impl_->Register(bundle, error);
    }

    bool Provider::RegisterBundleFiles(const wchar_t* modDirectory, const wchar_t* configName,
                                       const char* owner, std::string& error)
    {
        if (!owner || std::strcmp(owner, "unit_catalog") != 0) {
            error = "broker does not support the requested resource domain";
            return false;
        }
        BundleRegistration candidate;
        if (!LoadUnitBundle(modDirectory, configName, candidate, error))
            return false;
        return impl_->Register(candidate, error);
    }

    bool Provider::Start(const SdgoApi* api, SdgoMod* mod, OpenObserver observer,
                         void* observerContext, std::string& error)
    {
        if (impl_->active)
            return true;
        if (!api || !mod || g_activeProvider) {
            error = "resource overlay provider state is invalid";
            return false;
        }
        impl_->api = api;
        impl_->mod = mod;
        impl_->observer = observer;
        impl_->observerContext = observerContext;
        impl_->construct = reinterpret_cast<FileCtorFn>(
            api->Symbol(mod, "capsule.main.file_ctor"));
        impl_->allocate = reinterpret_cast<NewFn>(
            api->Symbol(mod, "capsule.main.operator_new"));
        impl_->open = reinterpret_cast<WfopenFn>(
            api->Symbol(mod, "capsule.main.wfopen"));
        if (!impl_->construct || !impl_->allocate || !impl_->open) {
            error = "native file constructor symbols are unavailable";
            return false;
        }

        g_activeProvider = this;
        if (!api->HookCode(mod, "capsule.main.open_file",
                           reinterpret_cast<void*>(&Provider::OpenDetour),
                           reinterpret_cast<void**>(&impl_->original))) {
            g_activeProvider = nullptr;
            error = "cannot install the native resource overlay hook";
            return false;
        }
        impl_->active = true;
        return true;
    }

    const SdgoResourceOverlayBrokerApiV1* Provider::BrokerApi() const
    {
        return impl_->active ? &kBrokerApi : nullptr;
    }

    bool Provider::Active() const
    {
        return impl_->active;
    }

    Provider& LocalProvider()
    {
        static Provider provider;
        return provider;
    }
}

extern "C" __declspec(dllexport)
const SdgoResourceOverlayBrokerApiV1* __cdecl SdgoGetResourceOverlayBrokerV1()
{
    return sdgo::resource_overlay::LocalProvider().BrokerApi();
}
