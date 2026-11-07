#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../support/native_resource_overlay/native_resource_overlay.h"

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;
using sdgo::resource_overlay::BundleRegistration;
using sdgo::resource_overlay::LocalProvider;
using sdgo::resource_overlay::OverlayEntry;

namespace
{
    int g_checks = 0;
    int g_hooks = 0;
    int g_stockOpens = 0;
    int g_overlayArchiveOpens = 0;
    void* g_detour = nullptr;

    void Check(bool value, const char* message)
    {
        ++g_checks;
        if (!value) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(2);
        }
    }

    void PutU32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        const size_t offset = bytes.size();
        bytes.resize(offset + 4);
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::string Sha256(const std::vector<uint8_t>& bytes)
    {
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

    void Write(const fs::path& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        Check(static_cast<bool>(stream), "overlay fixture file written");
    }

    struct Fixture
    {
        fs::path root;
        std::vector<uint8_t> index;
        std::vector<uint8_t> archive;
    };

    Fixture MakeFixture(const fs::path& root, const std::vector<std::wstring>& paths)
    {
        fs::create_directories(root);
        Fixture result;
        result.root = root;
        result.archive.resize(paths.size() * 520u, 0);
        for (size_t index = 0; index < paths.size(); ++index) {
            const uint32_t next = static_cast<uint32_t>(index + 1);
            const uint32_t previous = index ? static_cast<uint32_t>(index - 1) : 0;
            std::memcpy(result.archive.data() + index * 520u, &next, sizeof(next));
            std::memcpy(result.archive.data() + index * 520u + 4, &previous, sizeof(previous));
            result.archive[index * 520u + 8] = static_cast<uint8_t>(0x40 + index);
        }

        for (uint32_t value : {1u, 512u, static_cast<uint32_t>(paths.size()), 0u,
                               static_cast<uint32_t>(paths.size()), 0u})
            PutU32(result.index, value);
        for (size_t entry = 0; entry < paths.size(); ++entry) {
            for (wchar_t value : paths[entry]) {
                const uint16_t encoded = static_cast<uint16_t>(value) ^ 0x00aa;
                const size_t offset = result.index.size();
                result.index.resize(offset + 2);
                std::memcpy(result.index.data() + offset, &encoded, sizeof(encoded));
            }
            const uint16_t terminator = 0x00aa;
            const size_t offset = result.index.size();
            result.index.resize(offset + 2);
            std::memcpy(result.index.data() + offset, &terminator, sizeof(terminator));
            for (uint32_t value : {1u, static_cast<uint32_t>(entry), 1u,
                                   0u, 0u, 0u, 0u, 0u, 0u})
                PutU32(result.index, value);
        }

        Write(root / "resources.zdx", result.index);
        Write(root / "resources.zpk", result.archive);
        std::ofstream ini(root / "resource-overlay.ini", std::ios::binary | std::ios::trunc);
        ini << "[runtime]\r\nschema=1\r\nindex=resources.zdx\r\nindex_sha256="
            << Sha256(result.index)
            << "\r\narchive=resources.zpk\r\narchive_sha256="
            << Sha256(result.archive) << "\r\n";
        Check(static_cast<bool>(ini), "overlay fixture manifest written");
        return result;
    }

    void RewriteManifest(const Fixture& fixture, const std::string& indexHash)
    {
        std::ofstream ini(fixture.root / "resource-overlay.ini",
                          std::ios::binary | std::ios::trunc);
        ini << "[runtime]\r\nschema=1\r\nindex=resources.zdx\r\nindex_sha256="
            << indexHash << "\r\narchive=resources.zpk\r\narchive_sha256="
            << Sha256(fixture.archive) << "\r\n";
        Check(static_cast<bool>(ini), "overlay fixture manifest rewritten");
    }

    // Split archive: entries = {path, global first chunk, chunk count}; parts hold
    // partChunks chunks each (the last may be shorter); chunk links are part-relative.
    struct SplitEntry { std::wstring path; uint32_t first; uint32_t chunks; };

    fs::path MakeSplitFixture(const fs::path& root, const std::vector<SplitEntry>& entries,
                              uint32_t partChunks, uint32_t totalChunks, size_t truncateLast = 0)
    {
        fs::create_directories(root);
        std::vector<uint8_t> index;
        for (uint32_t value : {1u, 512u, totalChunks, 0u, static_cast<uint32_t>(entries.size()), 0u})
            PutU32(index, value);
        for (const SplitEntry& entry : entries) {
            for (wchar_t value : entry.path + L'\0') {
                const uint16_t encoded = static_cast<uint16_t>(value) ^ 0x00aa;
                const size_t offset = index.size();
                index.resize(offset + 2);
                std::memcpy(index.data() + offset, &encoded, sizeof(encoded));
            }
            for (uint32_t value : {1u, entry.first, entry.chunks, 0u, 0u, 0u, 0u, 0u, 0u})
                PutU32(index, value);
        }
        std::vector<uint8_t> whole;
        const uint32_t parts = (totalChunks + partChunks - 1) / partChunks;
        for (uint32_t part = 0; part < parts; ++part) {
            const uint32_t count = (std::min)(partChunks, totalChunks - part * partChunks);
            std::vector<uint8_t> bytes(count * 520u, 0);
            for (uint32_t local = 0; local < count; ++local) {
                const uint32_t next = local + 1, previous = local ? local - 1 : 0;
                std::memcpy(bytes.data() + local * 520u, &next, sizeof(next));
                std::memcpy(bytes.data() + local * 520u + 4, &previous, sizeof(previous));
                bytes[local * 520u + 8] = static_cast<uint8_t>(0x60 + part * partChunks + local);
            }
            if (part + 1 == parts && truncateLast)
                bytes.resize(bytes.size() - truncateLast);
            Write(root / (part ? "resources." + std::to_string(part) + ".zpk" : std::string("resources.zpk")), bytes);
            whole.insert(whole.end(), bytes.begin(), bytes.end());
        }
        Write(root / "resources.zdx", index);
        std::ofstream ini(root / "resource-overlay.ini", std::ios::binary | std::ios::trunc);
        ini << "[runtime]\r\nschema=1\r\nindex=resources.zdx\r\nindex_sha256=" << Sha256(index)
            << "\r\narchive=resources.zpk\r\narchive_sha256=" << Sha256(whole)
            << "\r\nparts=" << parts << "\r\npart_chunks=" << partChunks << "\r\n";
        Check(static_cast<bool>(ini), "split overlay fixture manifest written");
        return root;
    }

    void Log(SdgoMod*, int, const char*, ...)
    {
    }

    void* __cdecl Allocate(size_t size)
    {
        return std::malloc(size);
    }

    void* __cdecl DummyCtor()
    {
        return nullptr;
    }

    void* __cdecl OpenArchive(const wchar_t*, const wchar_t*)
    {
        ++g_overlayArchiveOpens;
        return nullptr;
    }

    void* __fastcall StockOpen(void*, void*, const void*, bool)
    {
        ++g_stockOpens;
        return reinterpret_cast<void*>(0x1010);
    }

    void* Symbol(SdgoMod*, const char* name)
    {
        if (!std::strcmp(name, "capsule.main.file_ctor"))
            return reinterpret_cast<void*>(&DummyCtor);
        if (!std::strcmp(name, "capsule.main.operator_new"))
            return reinterpret_cast<void*>(&Allocate);
        if (!std::strcmp(name, "capsule.main.wfopen"))
            return reinterpret_cast<void*>(&OpenArchive);
        return nullptr;
    }

    int HookCode(SdgoMod*, const char* name, void* detour, void** original)
    {
        if (std::strcmp(name, "capsule.main.open_file") != 0)
            return 0;
        ++g_hooks;
        g_detour = detour;
        *original = reinterpret_cast<void*>(&StockOpen);
        return 1;
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
}

int main()
{
    const fs::path root = fs::temp_directory_path() /
                          ("sdgo-native-overlay-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    const auto wrongHash = MakeFixture(root / "wrong-hash", {L"mdrs\\wrong.mod"});
    RewriteManifest(wrongHash, std::string(64, '0'));
    std::string error;
    Check(!LocalProvider().RegisterBundleFiles(
              wrongHash.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "wrong index hash is rejected before publication");

    const auto traversal = MakeFixture(root / "traversal", {L"mdrs\\..\\escape.mod"});
    Check(!LocalProvider().RegisterBundleFiles(
              traversal.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "traversal path is rejected");

    const auto wrongDomain = MakeFixture(root / "wrong-domain", {L"gdrs\\0280.gdt"});
    Check(!LocalProvider().RegisterBundleFiles(
              wrongDomain.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "unit bundle cannot widen into the capsule domain");

    const auto duplicate = MakeFixture(
        root / "duplicate", {L"mdrs\\DUP.mod", L"mdrs\\dup.mod"});
    Check(!LocalProvider().RegisterBundleFiles(
              duplicate.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "duplicate normalized resource path is rejected");

    // Split archive: three one-chunk entries over parts of two chunks.
    const auto split = MakeSplitFixture(root / "split",
        {{L"mdrs\\a.mod", 0, 1}, {L"mdrs\\b.mod", 1, 1}, {L"txrs\\c.txr", 2, 1}}, 2, 3);
    BundleRegistration inspected;
    Check(sdgo::resource_overlay::InspectUnitBundleFiles(split.c_str(), inspected, error),
          "split archive with exact parts is accepted");
    Check(inspected.archiveParts.size() == 2 && inspected.partChunks == 2 &&
              inspected.archiveParts[1] == (split / "resources.1.zpk").wstring(),
          "split archive lists its part files in order");
    {
        std::vector<uint8_t> whole;
        for (const char* name : {"resources.zpk", "resources.1.zpk"}) {
            std::ifstream stream(split / name, std::ios::binary);
            whole.insert(whole.end(), std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        Check(inspected.identity.substr(inspected.identity.find(':') + 1) == Sha256(whole),
              "split archive identity hashes the parts as one stream");
    }
    {
        std::wstring path = inspected.archivePath;
        auto info = inspected.entries.at(L"txrs\\c.txr").info;
        Check(sdgo::resource_overlay::ResolveArchivePart(inspected.archiveParts, inspected.partChunks, path, info) &&
                  path == inspected.archiveParts[1] && info.words[1] == 0,
              "an entry in the second part opens that part at a part-relative chunk");
        path = inspected.archivePath;
        info = inspected.entries.at(L"mdrs\\b.mod").info;
        Check(sdgo::resource_overlay::ResolveArchivePart(inspected.archiveParts, inspected.partChunks, path, info) &&
                  path == inspected.archiveParts[0] && info.words[1] == 1,
              "an entry in the first part keeps its chunk");
        info.words[1] = 9;
        Check(!sdgo::resource_overlay::ResolveArchivePart(inspected.archiveParts, inspected.partChunks, path, info),
              "a chunk beyond the last part is refused");
    }
    sdgo::resource_overlay::Provider splitProvider;
    Check(splitProvider.RegisterBundleFiles(split.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "split archive registers with a provider");
    Check(!sdgo::resource_overlay::InspectUnitBundleFiles(
              MakeSplitFixture(root / "crossing", {{L"mdrs\\a.mod", 0, 1}, {L"mdrs\\b.mod", 1, 2}}, 2, 3).c_str(),
              inspected, error),
          "an entry crossing a part boundary is refused");
    Check(!sdgo::resource_overlay::InspectUnitBundleFiles(
              MakeSplitFixture(root / "short-part", {{L"mdrs\\a.mod", 0, 1}}, 2, 1, 0).c_str(), inspected, error),
          "parts=1 is refused (a single archive omits parts)");
    {
        const auto shortPart = MakeSplitFixture(root / "short-first",
            {{L"mdrs\\a.mod", 0, 1}, {L"mdrs\\b.mod", 2, 1}}, 2, 3);
        fs::resize_file(shortPart / "resources.zpk", 520);
        std::vector<uint8_t> whole;
        for (const char* name : {"resources.zpk", "resources.1.zpk"}) {
            std::ifstream stream(shortPart / name, std::ios::binary);
            whole.insert(whole.end(), std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        std::ifstream indexStream(shortPart / "resources.zdx", std::ios::binary);
        std::vector<uint8_t> indexBytes((std::istreambuf_iterator<char>(indexStream)), std::istreambuf_iterator<char>());
        indexStream.close();
        std::ofstream ini(shortPart / "resource-overlay.ini", std::ios::binary | std::ios::trunc);
        ini << "[runtime]\r\nschema=1\r\nindex=resources.zdx\r\nindex_sha256=" << Sha256(indexBytes)
            << "\r\narchive=resources.zpk\r\narchive_sha256=" << Sha256(whole)
            << "\r\nparts=2\r\npart_chunks=2\r\n";
        ini.close();
        Check(!sdgo::resource_overlay::InspectUnitBundleFiles(shortPart.c_str(), inspected, error),
              "a non-final part shorter than part_chunks is refused even when its hash matches");
    }

    const std::vector<std::wstring> allDomains = {
        L"mdrs\\package\\model.mod", L"anrs\\package\\motion.ani",
        L"txrs\\package\\texture.txr", L"fxrs\\package\\effect.zfx",
        L"sprs\\package\\shop.spr", L"sdrs\\package\\sound.zsd",
        L"scp\\package\\logic.scp",
    };
    const auto valid = MakeFixture(root / "valid", allDomains);
    Check(LocalProvider().RegisterBundleFiles(
              valid.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "all seven unit resource domains and safe subdirectories are accepted");
    Check(LocalProvider().RegisterBundleFiles(
              valid.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "same owner and sealed hashes are idempotent");

    const auto replacement = MakeFixture(root / "replacement", {L"mdrs\\replacement.mod"});
    Check(!LocalProvider().RegisterBundleFiles(
              replacement.root.c_str(), L"resource-overlay.ini", "unit_catalog", error),
          "same owner cannot replace its sealed bundle in-process");

    BundleRegistration capsule;
    capsule.owner = "capsule_catalog";
    capsule.identity = "capsule-index:capsule-archive";
    capsule.archivePath = (root / "capsule.zpk").wstring();
    capsule.chunkSize = 512;
    capsule.entries.emplace(L"gdrs\\0031.gdt", OverlayEntry{});
    Check(LocalProvider().RegisterTrustedBundle(capsule, error),
          "capsule and unit bundles coexist in one broker");

    BundleRegistration conflict;
    conflict.owner = "conflicting_provider";
    conflict.identity = "conflict";
    conflict.archivePath = (root / "conflict.zpk").wstring();
    conflict.chunkSize = 512;
    conflict.entries.emplace(L"mdrs\\package\\model.mod", OverlayEntry{});
    Check(!LocalProvider().RegisterTrustedBundle(conflict, error),
          "cross-bundle path collision is rejected atomically");

    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &Log;
    api.Symbol = &Symbol;
    api.HookCode = &HookCode;
    Check(LocalProvider().Start(&api, reinterpret_cast<SdgoMod*>(1), nullptr, nullptr, error),
          "standalone provider starts");
    Check(g_hooks == 1 && g_detour, "one native VFS hook serves every bundle");

    const SdgoResourceOverlayBrokerApiV1* broker = SdgoGetResourceOverlayBrokerV1();
    Check(broker && broker->size == sizeof(*broker) && broker->version == 1 &&
              broker->RegisterBundle,
          "active provider publishes the exact broker ABI");
    char brokerError[256] = {};
    Check(broker->RegisterBundle(valid.root.c_str(), L"resource-overlay.ini",
                                 "unit_catalog", brokerError,
                                 static_cast<uint32_t>(sizeof(brokerError))) != 0,
          "public broker registration is idempotent");

    using Detour = void*(__fastcall*)(void*, void*, const void*, bool);
    HostString hit(L"mdrs\\package\\model.mod");
    Check(reinterpret_cast<Detour>(g_detour)(
              reinterpret_cast<void*>(1), nullptr, hit.bytes, false) ==
              reinterpret_cast<void*>(0x1010) &&
              g_overlayArchiveOpens == 1 && g_stockOpens == 1,
          "registered resource reaches its sealed archive before stock fallback");
    HostString miss(L"mdrs\\missing.mod");
    Check(reinterpret_cast<Detour>(g_detour)(
              reinterpret_cast<void*>(1), nullptr, miss.bytes, false) ==
              reinterpret_cast<void*>(0x1010) &&
              g_overlayArchiveOpens == 1 && g_stockOpens == 2,
          "unmatched resource uses the sole stock trampoline");

    fs::remove_all(root, ignored);
    std::printf("native_resource_overlay_test: %d checks passed\n", g_checks);
    return 0;
}
