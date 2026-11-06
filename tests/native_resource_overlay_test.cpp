#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
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

    Fixture MakeFixture(const fs::path& root, const std::vector<std::wstring>& paths,
                        uint32_t operatorId = 0)
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
        ini << "[runtime]\r\nschema=1\r\n";
        if (operatorId) {
            ini << "format=sdgo-native-resource-overlay-v1\r\noperator_id="
                << operatorId << "\r\n";
        }
        ini << "index=resources.zdx\r\nindex_sha256=" << Sha256(result.index)
            << "\r\narchive=resources.zpk\r\narchive_sha256="
            << Sha256(result.archive) << "\r\n";
        Check(static_cast<bool>(ini), "overlay fixture manifest written");
        return result;
    }

    Fixture MakeOperatorFixture(const fs::path& modRoot, uint32_t operatorId,
                                const std::vector<std::wstring>& paths)
    {
        return MakeFixture(
            modRoot / "assets" / std::to_string(operatorId), paths, operatorId);
    }

    std::wstring OperatorConfig(uint32_t operatorId)
    {
        return L"assets\\" + std::to_wstring(operatorId) +
               L"\\resource-overlay.ini";
    }

    std::string OperatorOwner(uint32_t operatorId)
    {
        return "operator_catalog:" + std::to_string(operatorId);
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

    void RewriteOperatorManifest(const Fixture& fixture, uint32_t operatorId,
                                 const std::string& format,
                                 const std::string& archiveHash = {})
    {
        std::ofstream ini(fixture.root / "resource-overlay.ini",
                          std::ios::binary | std::ios::trunc);
        ini << "[runtime]\nschema=1\nformat=" << format << "\noperator_id="
            << operatorId << "\nindex=resources.zdx\nindex_sha256="
            << Sha256(fixture.index)
            << "\narchive=resources.zpk\narchive_sha256="
            << (archiveHash.empty() ? Sha256(fixture.archive) : archiveHash)
            << "\n";
        Check(static_cast<bool>(ini), "operator overlay manifest rewritten");
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

    const auto ownerMismatch = MakeOperatorFixture(
        root / "operator-owner-mismatch", 60048,
        {L"sdgo\\mods\\operator_catalog\\assets\\60048\\dialogue.gdt"});
    const std::wstring ownerMismatchConfig = OperatorConfig(60048);
    const std::string wrongOwner = OperatorOwner(60049);
    Check(!LocalProvider().RegisterBundleFiles(
              (root / "operator-owner-mismatch").c_str(),
              ownerMismatchConfig.c_str(), wrongOwner.c_str(), error),
          "operator owner ID cannot select another operator config");
    Check(!LocalProvider().RegisterBundleFiles(
              (root / "operator-owner-mismatch").c_str(),
              ownerMismatchConfig.c_str(), "operator_catalog:+60048", error),
          "operator owner syntax is exact");
    Check(!LocalProvider().RegisterBundleFiles(
              (root / "operator-owner-mismatch").c_str(),
              L"assets/60048/resource-overlay.ini", "operator_catalog:60048",
              error),
          "operator config path spelling is exact");

    const auto manifestMismatch = MakeOperatorFixture(
        root / "operator-manifest-mismatch", 60049,
        {L"sdgo\\mods\\operator_catalog\\assets\\60049\\dialogue.gdt"});
    RewriteOperatorManifest(manifestMismatch, 60050,
                            "sdgo-native-resource-overlay-v1");
    const std::wstring manifestMismatchConfig = OperatorConfig(60049);
    const std::string manifestMismatchOwner = OperatorOwner(60049);
    Check(!LocalProvider().RegisterBundleFiles(
              (root / "operator-manifest-mismatch").c_str(),
              manifestMismatchConfig.c_str(), manifestMismatchOwner.c_str(), error),
          "operator manifest ID must match owner and config path");

    const auto formatMismatch = MakeOperatorFixture(
        root / "operator-format-mismatch", 60050,
        {L"sdgo\\mods\\operator_catalog\\assets\\60050\\dialogue.gdt"});
    RewriteOperatorManifest(formatMismatch, 60050, "unsupported");
    const std::wstring formatMismatchConfig = OperatorConfig(60050);
    const std::string formatMismatchOwner = OperatorOwner(60050);
    Check(!LocalProvider().RegisterBundleFiles(
              (root / "operator-format-mismatch").c_str(),
              formatMismatchConfig.c_str(), formatMismatchOwner.c_str(), error),
          "operator manifest format is fail closed");

    const auto extraManifest = MakeOperatorFixture(
        root / "operator-extra-manifest", 60062,
        {L"sdgo\\mods\\operator_catalog\\assets\\60062\\dialogue.gdt"});
    {
        std::ofstream manifest(
            extraManifest.root / "resource-overlay.ini",
            std::ios::binary | std::ios::app);
        manifest << "unexpected=1\n";
        Check(static_cast<bool>(manifest),
              "operator manifest extra-key fixture written");
    }
    const std::wstring extraManifestConfig = OperatorConfig(60062);
    const std::string extraManifestOwner = OperatorOwner(60062);
    Check(!LocalProvider().RegisterBundleFiles(
              (root / "operator-extra-manifest").c_str(),
              extraManifestConfig.c_str(), extraManifestOwner.c_str(), error),
          "operator manifest rejects unknown keys and sections");

    const struct {
        uint32_t id;
        const wchar_t* path;
        const char* message;
    } refusedOperatorPaths[] = {
        {60051, L"sdgo\\mods\\operator_catalog\\assets\\60051\\UPPER.gdt",
         "operator keys must already be lowercase"},
        {60052, L"sdgo/mods/operator_catalog/assets/60052/dialogue.gdt",
         "operator keys must already use native separators"},
        {60053, L"sdgo\\mods\\operator_catalog\\assets\\60054\\dialogue.gdt",
         "operator bundle cannot cross into another operator namespace"},
        {60054, L"gdrs\\0280.gdt",
         "operator bundle cannot publish a native top-level domain"},
        {60055, L"sdgo\\mods\\another_mod\\assets\\60055\\dialogue.gdt",
         "operator bundle cannot publish another Mod path"},
        {60056, L"sdgo\\mods\\operator_catalog\\assets\\60056\\payload.dll",
         "operator bundle rejects program and unsupported suffixes"},
        {60057, L"sdgo\\mods\\operator_catalog\\assets\\60057.spr",
         "operator flat namespace accepts only the exact legacy frame keys"},
        {60058, L"sdgo\\mods\\operator_catalog\\assets\\60058_7.spr",
         "operator flat namespace rejects frames outside zero through six"},
        {60064, L"sdgo\\optex\\60065\\0.txr",
         "short operator texture domain cannot cross into another operator ID"},
        {60065, L"sdgo\\optex\\abcde\\0.txr",
         "short operator texture domain requires an exact numeric operator ID"},
        {60066, L"sdgo\\optex\\60066\\portrait.txr",
         "short operator texture domain requires a numeric texture index"},
        {60067, L"sdgo\\optex\\60067\\0.spr",
         "short operator texture domain accepts TXR resources only"},
        {60068, L"sdgo\\optex\\60068\\12345678901.txr",
         "short operator texture path must fit the native 32-wchar field with a terminator"},
        {60069, L"sdgo/optex/60069/0.txr",
         "short operator texture keys must already use native separators"},
    };
    for (const auto& refused : refusedOperatorPaths) {
        const fs::path modRoot =
            root / ("operator-path-" + std::to_string(refused.id));
        MakeOperatorFixture(modRoot, refused.id, {refused.path});
        const std::wstring config = OperatorConfig(refused.id);
        const std::string owner = OperatorOwner(refused.id);
        Check(!LocalProvider().RegisterBundleFiles(
                  modRoot.c_str(), config.c_str(), owner.c_str(), error),
              refused.message);
    }

    const fs::path emptyRoot = root / "operator-empty";
    MakeOperatorFixture(emptyRoot, 60059, {});
    const std::wstring emptyConfig = OperatorConfig(60059);
    const std::string emptyOwner = OperatorOwner(60059);
    Check(!LocalProvider().RegisterBundleFiles(
              emptyRoot.c_str(), emptyConfig.c_str(), emptyOwner.c_str(), error),
          "operator bundle cannot publish an empty index");

    const fs::path duplicateOperatorRoot = root / "operator-duplicate";
    MakeOperatorFixture(
        duplicateOperatorRoot, 60060,
        {L"sdgo\\mods\\operator_catalog\\assets\\60060\\same.spr",
         L"sdgo\\mods\\operator_catalog\\assets\\60060\\same.spr"});
    const std::wstring duplicateOperatorConfig = OperatorConfig(60060);
    const std::string duplicateOperatorOwner = OperatorOwner(60060);
    Check(!LocalProvider().RegisterBundleFiles(
              duplicateOperatorRoot.c_str(), duplicateOperatorConfig.c_str(),
              duplicateOperatorOwner.c_str(), error),
          "operator archive rejects duplicate exact keys");

    const fs::path invalidRangeRoot = root / "operator-invalid-range";
    auto invalidRange = MakeOperatorFixture(
        invalidRangeRoot, 60063,
        {L"sdgo\\mods\\operator_catalog\\assets\\60063\\dialogue.gdt"});
    const size_t metadata =
        24 + (std::wstring(
            L"sdgo\\mods\\operator_catalog\\assets\\60063\\dialogue.gdt").size() + 1) * 2;
    const uint32_t outsideArchive = 2;
    std::memcpy(invalidRange.index.data() + metadata + 4,
                &outsideArchive, sizeof(outsideArchive));
    Write(invalidRange.root / "resources.zdx", invalidRange.index);
    RewriteOperatorManifest(
        invalidRange, 60063, "sdgo-native-resource-overlay-v1");
    const std::wstring invalidRangeConfig = OperatorConfig(60063);
    const std::string invalidRangeOwner = OperatorOwner(60063);
    Check(!LocalProvider().RegisterBundleFiles(
              invalidRangeRoot.c_str(), invalidRangeConfig.c_str(),
              invalidRangeOwner.c_str(), error),
          "operator archive rejects chunk ranges outside the sealed archive");

    const fs::path sizeRoot = root / "operator-size";
    auto sizeFixture = MakeOperatorFixture(
        sizeRoot, 60061,
        {L"sdgo\\mods\\operator_catalog\\assets\\60061\\dialogue.gdt"});
    HANDLE oversized = CreateFileW(
        (sizeFixture.root / "resources.zpk").c_str(), GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(oversized != INVALID_HANDLE_VALUE, "operator size fixture opened");
    LARGE_INTEGER exactLimit = {};
    exactLimit.QuadPart = 64ll * 1024ll * 1024ll;
    Check(SetFilePointerEx(oversized, exactLimit, nullptr, FILE_BEGIN) != 0 &&
              SetEndOfFile(oversized) != 0,
          "operator size fixture extended to exact limit");
    CloseHandle(oversized);
    RewriteOperatorManifest(sizeFixture, 60061,
                            "sdgo-native-resource-overlay-v1",
                            std::string(64, '0'));
    const std::wstring sizeConfig = OperatorConfig(60061);
    const std::string sizeOwner = OperatorOwner(60061);
    Check(!LocalProvider().RegisterBundleFiles(
              sizeRoot.c_str(), sizeConfig.c_str(), sizeOwner.c_str(), error),
          "operator archive must be strictly smaller than 64 MiB");

    const fs::path validOperatorRoot = root / "operator-valid";
    const std::vector<std::wstring> validOperatorPaths = {
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\dialogue.gdt",
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\portrait.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\voice.zsd",
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\texture.txr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048.txr",
        L"sdgo\\optex\\60048\\0.txr",
        L"sdgo\\optex\\60048\\47.txr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_0.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_1.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_2.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_3.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_4.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_5.spr",
        L"sdgo\\mods\\operator_catalog\\assets\\60048_6.spr",
    };
    MakeOperatorFixture(validOperatorRoot, 60048, validOperatorPaths);
    const std::wstring validOperatorConfig = OperatorConfig(60048);
    const std::string validOperatorOwner = OperatorOwner(60048);
    Check(LocalProvider().RegisterBundleFiles(
              validOperatorRoot.c_str(), validOperatorConfig.c_str(),
              validOperatorOwner.c_str(), error),
          "operator nested resources and exact legacy upload keys are accepted");
    Check(LocalProvider().RegisterBundleFiles(
              validOperatorRoot.c_str(), validOperatorConfig.c_str(),
              validOperatorOwner.c_str(), error),
          "same sealed operator owner registration is idempotent");

    BundleRegistration stolenOperatorTexture;
    stolenOperatorTexture.owner = "capsule_catalog";
    stolenOperatorTexture.identity = "capsule-optex";
    stolenOperatorTexture.archivePath = (root / "capsule-optex.zpk").wstring();
    stolenOperatorTexture.chunkSize = 512;
    stolenOperatorTexture.entries.emplace(
        L"sdgo\\optex\\60070\\0.txr", OverlayEntry{});
    Check(!LocalProvider().RegisterTrustedBundle(stolenOperatorTexture, error),
          "non-operator trusted bundle cannot claim the reserved short TXR domain");

    BundleRegistration wrongOperatorTextureOwner;
    wrongOperatorTextureOwner.owner = "operator_catalog:60070";
    wrongOperatorTextureOwner.identity = "wrong-operator-optex";
    wrongOperatorTextureOwner.archivePath =
        (root / "wrong-operator-optex.zpk").wstring();
    wrongOperatorTextureOwner.chunkSize = 512;
    wrongOperatorTextureOwner.entries.emplace(
        L"sdgo\\optex\\60071\\0.txr", OverlayEntry{});
    Check(!LocalProvider().RegisterTrustedBundle(
              wrongOperatorTextureOwner, error),
          "trusted operator bundle cannot claim another operator short TXR domain");

    BundleRegistration trustedOperatorTexture;
    trustedOperatorTexture.owner = "operator_catalog:60070";
    trustedOperatorTexture.identity = "trusted-operator-optex";
    trustedOperatorTexture.archivePath =
        (root / "trusted-operator-optex.zpk").wstring();
    trustedOperatorTexture.chunkSize = 512;
    trustedOperatorTexture.entries.emplace(
        L"sdgo\\optex\\60070\\0.txr", OverlayEntry{});
    Check(LocalProvider().RegisterTrustedBundle(trustedOperatorTexture, error),
          "matching operator owner can claim its canonical short TXR domain");

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
    Check(broker->RegisterBundle(
              validOperatorRoot.c_str(), validOperatorConfig.c_str(),
              validOperatorOwner.c_str(), brokerError,
              static_cast<uint32_t>(sizeof(brokerError))) != 0,
          "public ABI registers the strict operator domain without a version change");

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
    HostString operatorHit(
        L"sdgo\\mods\\operator_catalog\\assets\\60048\\dialogue.gdt");
    Check(reinterpret_cast<Detour>(g_detour)(
              reinterpret_cast<void*>(1), nullptr, operatorHit.bytes, false) ==
              reinterpret_cast<void*>(0x1010) &&
              g_overlayArchiveOpens == 2 && g_stockOpens == 3,
          "operator resource reaches its sealed archive through the same hook");
    HostString legacyOperatorTextureHit(
        L"sdgo\\mods\\operator_catalog\\assets\\60048.txr");
    Check(reinterpret_cast<Detour>(g_detour)(
              reinterpret_cast<void*>(1), nullptr,
              legacyOperatorTextureHit.bytes, false) ==
              reinterpret_cast<void*>(0x1010) &&
              g_overlayArchiveOpens == 3 && g_stockOpens == 4,
          "legacy long upload texture key remains readable");
    HostString shortOperatorTextureHit(L"sdgo\\optex\\60048\\47.txr");
    Check(reinterpret_cast<Detour>(g_detour)(
              reinterpret_cast<void*>(1), nullptr,
              shortOperatorTextureHit.bytes, false) ==
              reinterpret_cast<void*>(0x1010) &&
              g_overlayArchiveOpens == 4 && g_stockOpens == 5,
          "short operator texture reaches its same-owner sealed archive");

    fs::remove_all(root, ignored);
    std::printf("native_resource_overlay_test: %d checks passed\n", g_checks);
    return 0;
}
