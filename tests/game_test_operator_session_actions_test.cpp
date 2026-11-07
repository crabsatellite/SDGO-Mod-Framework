// OFFLINE-ONLY sealed-plan, receipt, and hook-registration tests for the
// operator GameTest session. This fixture never starts GOnline, contacts a
// server, writes SQL, spends currency, or creates an operator record.

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "../mods/game_test_client/hangar_navigation.h"
#include "../mods/game_test_client/native_actions.h"
#include "../mods/game_test_client/operator_flow_actions.h"
#include "../mods/game_test_client/shop_catalog_actions.h"

#define private public
#include "../mods/game_test_client/operator_session_actions.h"
#undef private

namespace
{
constexpr uint32_t kOperatorId = 60047;
constexpr uint64_t kTargetUid = 0x1122334455667788ULL;

int g_failures = 0;
std::vector<std::string> g_hookNames;
int g_failHook = -1;
OperatorSessionActions::CaptureResult g_captureResult =
    OperatorSessionActions::kCapturePending;
uint32_t g_captureCalls = 0;
OperatorSessionActions::Checkpoint g_captureCheckpoint =
    static_cast<OperatorSessionActions::Checkpoint>(0);

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void HookMarker()
{
}

OperatorSessionActions::CaptureResult __cdecl CaptureGate(
    void*,
    OperatorSessionActions::Checkpoint checkpoint,
    const SdgoGameTestContextV1*)
{
    ++g_captureCalls;
    g_captureCheckpoint = checkpoint;
    return g_captureResult;
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void*,
    void** original)
{
    const int index =
        static_cast<int>(g_hookNames.size());
    g_hookNames.emplace_back(name ? name : "");
    if (index == g_failHook)
        return 0;
    if (original)
        *original =
            reinterpret_cast<void*>(&HookMarker);
    return 1;
}

std::wstring TemporaryIni()
{
    wchar_t folder[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = {};
    if (!GetTempPathW(
            static_cast<DWORD>(std::size(folder)),
            folder) ||
        !GetTempFileNameW(
            folder,
            L"ops",
            0,
            path))
        return {};
    DeleteFileW(path);
    return path;
}

bool WriteUtf16(
    const std::wstring& path,
    const std::wstring& content)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool okay =
        WriteFile(
            file,
            &bom,
            sizeof(bom),
            &written,
            nullptr) != FALSE &&
        written == sizeof(bom);
    const DWORD bytes =
        static_cast<DWORD>(
            content.size() * sizeof(wchar_t));
    written = 0;
    okay = okay &&
        WriteFile(
            file,
            content.data(),
            bytes,
            &written,
            nullptr) != FALSE &&
        written == bytes;
    CloseHandle(file);
    return okay;
}

bool WriteUtf8(
    const std::wstring& path,
    const std::wstring& content)
{
    const int bytes = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        content.data(),
        static_cast<int>(content.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (bytes <= 0)
        return false;
    std::vector<char> encoded(
        static_cast<size_t>(bytes));
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            content.data(),
            static_cast<int>(content.size()),
            encoded.data(),
            bytes,
            nullptr,
            nullptr) != bytes)
        return false;
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool okay =
        WriteFile(
            file,
            encoded.data(),
            static_cast<DWORD>(encoded.size()),
            &written,
            nullptr) != FALSE &&
        written == encoded.size();
    CloseHandle(file);
    return okay;
}

std::wstring ValidPlan(
    const std::wstring& extra = {})
{
    return
        L"[run]\r\n"
        L"operator_flow_operator_id=60047\r\n"
        L"operator_flow_product_id=1039001\r\n"
        L"operator_flow_template_id=60047\r\n"
        L"operator_flow_option_id=9547\r\n"
        L"operator_flow_item_id=290001\r\n"
        L"operator_flow_category_id=1219047\r\n"
        L"operator_flow_price=500\r\n"
        L"operator_flow_validity=30\r\n"
        L"operator_flow_payment=1\r\n"
        L"operator_flow_name=\u6D4B\u8BD5\u526F\u5B98\r\n"
        L"operator_flow_label=\u6D4B\u8BD5\u526F\u5B98 30\u65E5\r\n"
        L"operator_flow_nickname=Pilot_47\r\n"
        L"operator_flow_purchase=1\r\n"
        L"operator_shop_path=inputs/operator-shop.ini\r\n"
        L"operator_shop_sha256="
        L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
        L"operator_shop_display_path=inputs/operator-shop-display.ini\r\n"
        L"operator_shop_display_sha256="
        L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r\n" +
        extra;
}

bool LoadText(
    const std::wstring& text,
    OperatorSessionActions::Config* config,
    bool* enabled)
{
    const std::wstring path = TemporaryIni();
    if (path.empty() ||
        !WriteUtf16(path, text))
        return false;
    const bool loaded =
        OperatorSessionActions::LoadConfig(
            path,
            config,
            enabled);
    DeleteFileW(path.c_str());
    return loaded;
}

void TestConfigContract()
{
    OperatorSessionActions::Config config;
    bool enabled = true;
    Check(
        LoadText(
            L"[run]\r\nendpoint=127.0.0.1:25060\r\n",
            &config,
            &enabled) &&
            !enabled,
        "an absent operator scope leaves the ordinary run disabled and unchanged");

    enabled = false;
    Check(
        LoadText(
            ValidPlan(),
            &config,
            &enabled) &&
            enabled &&
            config.operator_id == kOperatorId &&
            config.product_id == 1039001 &&
            config.template_id == kOperatorId &&
            config.option_id == 9547 &&
            config.item_id == "290001" &&
            config.category_id == 1219047 &&
            config.price == 500 &&
            config.validity == 30 &&
            config.payment == 1 &&
            config.purchase &&
            config.nickname == "Pilot_47",
        "all seventeen canonical fields bind atomically while item and native option identities remain separate");

    const std::wstring utf8Path = TemporaryIni();
    OperatorSessionActions::Config utf8Config;
    bool utf8Enabled = false;
    uint32_t utf8Failure = UINT32_MAX;
    const bool utf8Written =
        !utf8Path.empty() &&
        WriteUtf8(
            utf8Path,
            ValidPlan());
    const bool utf8Loaded =
        utf8Written &&
        OperatorSessionActions::LoadConfig(
            utf8Path,
            &utf8Config,
            &utf8Enabled,
            &utf8Failure);
    if (!utf8Path.empty())
        DeleteFileW(utf8Path.c_str());
    Check(
        utf8Loaded &&
            utf8Enabled &&
            utf8Failure ==
                OperatorSessionActions::kConfigLoaded &&
            utf8Config.name ==
                L"\u6D4B\u8BD5\u526F\u5B98" &&
            utf8Config.label ==
                L"\u6D4B\u8BD5\u526F\u5B98 30\u65E5",
        "UTF-8 run.ini preserves sealed non-ASCII name and label without embedded profile terminators");

    std::wstring secondSku = ValidPlan();
    const std::pair<const wchar_t*, const wchar_t*> replacements[] = {
        {L"operator_flow_operator_id=60047", L"operator_flow_operator_id=60048"},
        {L"operator_flow_product_id=1039001", L"operator_flow_product_id=1039002"},
        {L"operator_flow_template_id=60047", L"operator_flow_template_id=60048"},
        {L"operator_flow_option_id=9547", L"operator_flow_option_id=9548"},
        {L"operator_flow_item_id=290001", L"operator_flow_item_id=290002"},
        {L"operator_flow_category_id=1219047", L"operator_flow_category_id=1219048"},
    };
    for (const auto& replacement : replacements) {
        const size_t offset =
            secondSku.find(replacement.first);
        secondSku.replace(
            offset,
            std::wcslen(replacement.first),
            replacement.second);
    }
    Check(
        LoadText(
            secondSku,
            &config,
            &enabled) &&
            enabled &&
            config.operator_id == 60048 &&
            config.option_id == 9548 &&
            config.item_id == "290002",
        "a second SKU preserves a distinct six-digit owner ItemId and independent positive native option identity");

    OperatorSessionActions::Config direct = config;
    direct.option_id =
        static_cast<uint32_t>(INT32_MAX);
    Check(
        OperatorSessionActions::ValidConfig(direct),
        "direct session binding accepts the largest positive signed native option identity");
    direct.option_id =
        static_cast<uint32_t>(INT32_MAX) + 1u;
    Check(
        !OperatorSessionActions::ValidConfig(direct),
        "direct session binding rejects a native option identity above INT_MAX");

    std::wstring partial = ValidPlan();
    const std::wstring nicknameLine =
        L"operator_flow_nickname=Pilot_47\r\n";
    partial.erase(
        partial.find(nicknameLine),
        nicknameLine.size());
    Check(
        !LoadText(
            partial,
            &config,
            &enabled),
        "a partial operator scope is rejected");

    Check(
        !LoadText(
            ValidPlan(
                L"operator_flow_secret=hidden\r\n"),
            &config,
            &enabled),
        "an unknown operator key is rejected");

    std::wstring wrongCase = ValidPlan();
    const size_t caseOffset =
        wrongCase.find(
            L"operator_flow_name=");
    wrongCase.replace(
        caseOffset,
        std::wcslen(
            L"operator_flow_name"),
        L"Operator_Flow_Name");
    Check(
        !LoadText(
            wrongCase,
            &config,
            &enabled),
        "case-insensitive profile lookup cannot bypass exact sealed key spelling");

    Check(
        !LoadText(
            ValidPlan(
                L"operator_flow_price=500\r\n"),
            &config,
            &enabled),
        "duplicate sealed fields are rejected");

    std::wstring noncanonical = ValidPlan();
    const size_t priceOffset =
        noncanonical.find(
            L"operator_flow_price=500");
    noncanonical.replace(
        priceOffset,
        std::wcslen(
            L"operator_flow_price=500"),
        L"operator_flow_price=0500");
    Check(
        !LoadText(
            noncanonical,
            &config,
            &enabled),
        "noncanonical decimal values are rejected before native binding");

    std::wstring invalidItem = ValidPlan();
    const size_t itemOffset =
        invalidItem.find(
            L"operator_flow_item_id=290001");
    invalidItem.replace(
        itemOffset,
        std::wcslen(
            L"operator_flow_item_id=290001"),
        L"operator_flow_item_id=9547");
    Check(
        !LoadText(
            invalidItem,
            &config,
            &enabled),
        "owner ItemId must remain a sealed six-digit string and is never derived from native option identity");

    std::wstring oversizedOption = ValidPlan();
    const size_t optionOffset =
        oversizedOption.find(
            L"operator_flow_option_id=9547");
    oversizedOption.replace(
        optionOffset,
        std::wcslen(
            L"operator_flow_option_id=9547"),
        L"operator_flow_option_id=2147483648");
    Check(
        !LoadText(
            oversizedOption,
            &config,
            &enabled),
        "run.ini rejects a native option identity above INT_MAX");
}

OperatorFlowActions::OperatorRecord Record(
    uint64_t uid,
    uint32_t operatorId,
    uint16_t slot,
    uint8_t enabled)
{
    OperatorFlowActions::OperatorRecord value;
    value.uid = uid;
    value.operator_id = operatorId;
    value.slot = slot;
    value.enabled = enabled;
    return value;
}

void TestFinalIntegrity()
{
    OperatorSessionActions session;
    session.config_.operator_id = kOperatorId;
    session.config_.price = 500;
    session.config_.payment = 1;
    session.config_.purchase = true;
    session.baselineWallet_ = {2000, 300};
    session.baselineInventory_.records = {
        Record(11, 60001, 1, 1),
        Record(12, 60002, 2, 0),
    };
    session.targetUid_ = kTargetUid;
    session.observation_.target_slot = 3;

    OperatorFlowActions::InventoryImage after;
    after.records = session.baselineInventory_.records;
    after.records[0].enabled = 0;
    after.records.push_back(
        Record(kTargetUid, kOperatorId, 3, 1));
    Check(
        session.WalletPreserved({1500, 300}) &&
            session.InventoryPreserved(after),
        "purchase recovery preserves every old record, adds one target UID, activates it, and keeps the exact debit");

    after.records[0].enabled = 1;
    Check(
        !session.InventoryPreserved(after),
        "purchase recovery rejects an unrelated operator that remains enabled after native activation");
    after.records[0].enabled = 0;
    Check(
        !session.WalletPreserved({1499, 300}),
        "purchase recovery rejects any later wallet drift");

    OperatorSessionActions owned;
    owned.config_.operator_id = kOperatorId;
    owned.config_.price = 500;
    owned.config_.payment = 2;
    owned.config_.purchase = false;
    owned.baselineWallet_ = {2000, 300};
    owned.baselineInventory_.records = {
        Record(kTargetUid, kOperatorId, 4, 0),
        Record(22, 60003, 5, 1),
    };
    owned.targetUid_ = kTargetUid;
    owned.observation_.target_slot = 4;
    OperatorFlowActions::InventoryImage activated =
        owned.baselineInventory_;
    activated.records[0].enabled = 1;
    activated.records[1].enabled = 0;
    Check(
        owned.WalletPreserved({2000, 300}) &&
            owned.InventoryPreserved(activated),
        "purchase=false accepts only the same owned UID with unchanged wallet and one native enabled transition");
    activated.records.push_back(
        Record(23, 60004, 6, 0));
    Check(
        !owned.InventoryPreserved(activated),
        "purchase=false rejects unrelated inventory additions");
}

void TestReplyPublication()
{
    OperatorSessionActions session;
    session.namingActionArmed_ = 1;
    std::array<uint8_t, 0x0E> naming = {};
    std::memcpy(
        naming.data() + 6,
        &kTargetUid,
        sizeof(kTargetUid));
    const auto copied =
        OperatorSessionActions::CopyPacket(
            0x08CB,
            naming.data(),
            false);
    std::memset(
        naming.data() + 6,
        0,
        sizeof(kTargetUid));
    session.ObserveInventoryReply(
        0x08CB,
        copied.bytes.data(),
        copied.size);
    Check(
        copied.valid &&
            copied.size == 0x0E &&
            OperatorSessionActions::
                ReadPublishedSequence(
                    session.namingReplySequence_) == 1 &&
            OperatorSessionActions::ReadAtomic(
                session.namingReplyUidLow_) ==
                static_cast<uint32_t>(kTargetUid) &&
            OperatorSessionActions::ReadAtomic(
                session.namingReplyUidHigh_) ==
                static_cast<uint32_t>(
                    kTargetUid >> 32),
        "nickname reply identity is copied before original mutation and published only after all fields");

    std::array<uint8_t, 7> emptyPage = {};
    const auto copiedEmptyPage =
        OperatorSessionActions::CopyPacket(
            0x059F,
            emptyPage.data(),
            false);
    Check(
        copiedEmptyPage.valid &&
            copiedEmptyPage.size ==
                emptyPage.size() &&
            copiedEmptyPage.bytes[6] == 0,
        "a native count-zero 059F reply remains a valid seven-byte packet for the pending-page observer");

    const uint64_t otherUid = 7;
    std::memcpy(
        naming.data() + 6,
        &otherUid,
        sizeof(otherUid));
    session.ObserveInventoryReply(
        0x08CB,
        naming.data(),
        static_cast<uint32_t>(
            naming.size()));
    Check(
        OperatorSessionActions::ReadAtomic(
            session.namingReplyUidLow_) ==
            static_cast<uint32_t>(kTargetUid),
        "the one-shot nickname receipt cannot be replaced by a later reply");

    session.activationActionArmed_ = 1;
    std::array<uint8_t, 0x56> activation = {};
    const uint32_t operatorId = kOperatorId;
    const uint16_t slot = 9;
    const uint8_t enabled = 0; // Native reply can carry the pre-activation flag.
    std::memcpy(
        activation.data() + 6,
        &kTargetUid,
        sizeof(kTargetUid));
    std::memcpy(
        activation.data() + 0x0E,
        &operatorId,
        sizeof(operatorId));
    std::memcpy(
        activation.data() + 0x16,
        &slot,
        sizeof(slot));
    std::memcpy(
        activation.data() + 0x18,
        &enabled,
        sizeof(enabled));
    session.ObserveInventoryReply(
        0x08A0,
        activation.data(),
        static_cast<uint32_t>(
            activation.size()));
    Check(
        OperatorSessionActions::
                ReadPublishedSequence(
                    session.activationReplySequence_) == 1 &&
            OperatorSessionActions::ReadAtomic(
                session.activationReplyOperatorId_) ==
                kOperatorId &&
            OperatorSessionActions::ReadAtomic(
                session.activationReplySlot_) == slot &&
            OperatorSessionActions::ReadAtomic(
                session.activationReplyEnabled_) == 1,
        "activation reply publishes the exact full-record identity used by profile readback");
}

void TestHookScope()
{
    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.HookCode = &ApiHookCode;
    SdgoMod* mod =
        reinterpret_cast<SdgoMod*>(1);

    OperatorSessionActions existing;
    existing.bound_ = true;
    existing.config_.purchase = false;
    g_hookNames.clear();
    g_failHook = -1;
    OperatorSessionActions::active_ = nullptr;
    Check(
        existing.InstallHooks(&api, mod) &&
            g_hookNames.size() == 1 &&
            g_hookNames[0] ==
                "flow.operator_inventory_dispatch",
        "purchase=false installs only the inventory reply observer and never enters shop dispatch");

    OperatorSessionActions purchase;
    purchase.bound_ = true;
    purchase.config_.purchase = true;
    g_hookNames.clear();
    OperatorSessionActions::active_ = nullptr;
    Check(
        purchase.InstallHooks(&api, mod) &&
            g_hookNames ==
                std::vector<std::string>{
                    "flow.operator_shop_dispatch_1",
                    "flow.operator_shop_dispatch_2",
                    "flow.operator_shop_dispatch_3",
                    "flow.operator_inventory_dispatch",
                },
        "purchase=true registers the three exact shop aliases and inventory observer once");

    OperatorSessionActions partial;
    partial.bound_ = true;
    partial.config_.purchase = true;
    g_hookNames.clear();
    g_failHook = 1;
    OperatorSessionActions::active_ = nullptr;
    Check(
        !partial.InstallHooks(&api, mod) &&
            OperatorSessionActions::active_ ==
                &partial &&
            !partial.hooksInstalled_,
        "a partial hook failure keeps installed detours able to forward through their live owner while startup fails closed");
    OperatorSessionActions::active_ = nullptr;
    g_failHook = -1;
}

void TestStageTimeouts()
{
    OperatorSessionActions session;
    session.config_.ui_timeout_ms = 15;
    session.config_.shop_timeout_ms = 90;
    session.stageStartedAt_ = 100;
    session.lastElapsed_ = 114;
    session.stage_ =
        OperatorSessionActions::kStageNaming;
    Check(
        !session.UiStageExpired(),
        "a native UI stage remains pending inside its bound");
    session.lastElapsed_ = 115;
    Check(
        session.UiStageExpired(),
        "baseline, UI, capture, activation, and recovery waits fail closed at the configured bound");
    session.stage_ =
        OperatorSessionActions::kStageNamingReply;
    Check(
        !session.UiStageExpired(),
        "network and readback stages retain their separate timeout accounting");
    session.stage_ =
        OperatorSessionActions::kStageShopRecovery;
    session.shopStartedAt_ = 100;
    session.lastElapsed_ = 189;
    Check(
        !session.UiStageExpired(),
        "pre-shop native unit-manager restoration uses the bounded operator shop budget");
    session.lastElapsed_ = 190;
    Check(
        session.UiStageExpired(),
        "pre-shop restoration cannot wait past the bounded operator shop budget");
    session.stage_ =
        OperatorSessionActions::kStageShop;
    Check(
        session.UiStageExpired(),
        "full native inventory loading and shop entry share the same bounded shop budget");
}

void TestShopDiagnostics()
{
    OperatorSessionActions session;
    ShopCatalogActions::Observation shop;
    shop.stage =
        ShopCatalogActions::kStageBaseline;
    shop.inventory_failure = 6;
    shop.inventory_probe_slot = 119;
    shop.inventory_ready = 1;
    shop.inventory_capacity = 1200;
    shop.inventory_brief_unit = 23010;
    shop.inventory_full_unit = 23010;
    shop.inventory_brief_slot = 119;
    shop.inventory_full_slot = 119;
    shop.inventory_brief_uid = 11938;
    shop.inventory_full_uid = 11938;
    session.RecordShopState(
        ShopCatalogActions::
            kWaitingForFullInventory,
        shop);
    const auto state = session.State();
    Check(
        state.shop_result ==
                ShopCatalogActions::
                    kWaitingForFullInventory &&
            state.shop_stage ==
                ShopCatalogActions::
                    kStageBaseline &&
            state.shop_inventory_failure == 6 &&
            state.shop_inventory_probe_slot == 119 &&
            state.shop_inventory_probe_page == 19 &&
            state.shop_inventory_ready == 1 &&
            state.shop_inventory_capacity == 1200 &&
            state.shop_inventory_brief_unit == 23010 &&
            state.shop_inventory_full_unit == 23010 &&
            state.shop_inventory_brief_uid == 11938 &&
            state.shop_inventory_full_uid == 11938,
        "session observation preserves the child shop result, stage, and exact full-inventory page diagnostic");
}

void TestOptionsCapturePriming()
{
    g_captureCalls = 0;
    g_captureCheckpoint =
        static_cast<OperatorSessionActions::Checkpoint>(0);
    g_captureResult =
        OperatorSessionActions::kCapturePending;

    OperatorSessionActions session;
    session.config_.capture_frames = true;
    session.captureGate_ = &CaptureGate;
    Check(
        session.PrimeOptionsCapture(nullptr) ==
                OperatorSessionActions::kComplete &&
            session.optionsCapturePrimed_ &&
            session.observation_.last_checkpoint ==
                OperatorSessionActions::kOptionsCheckpoint &&
            g_captureCalls == 1 &&
            g_captureCheckpoint ==
                OperatorSessionActions::kOptionsCheckpoint,
        "the options frame is queued before the original BuyBtn transition without blocking that transition");
    Check(
        session.PrimeOptionsCapture(nullptr) ==
                OperatorSessionActions::kComplete &&
            g_captureCalls == 1,
        "the pre-transition options capture is queued at most once");

    g_captureCalls = 0;
    g_captureResult =
        OperatorSessionActions::kCaptureFailed;
    OperatorSessionActions failed;
    failed.config_.capture_frames = true;
    failed.captureGate_ = &CaptureGate;
    Check(
        failed.PrimeOptionsCapture(nullptr) ==
                OperatorSessionActions::kCaptureRejected &&
            failed.stage_ ==
                OperatorSessionActions::kStageFailed &&
            g_captureCalls == 1,
        "a failed pre-transition capture rejects the session before any shop action");
}

void TestConfirmationCapturePriming()
{
    g_captureCalls = 0;
    g_captureCheckpoint =
        static_cast<OperatorSessionActions::Checkpoint>(0);
    g_captureResult =
        OperatorSessionActions::kCapturePending;

    OperatorSessionActions session;
    session.config_.capture_frames = true;
    session.captureGate_ = &CaptureGate;
    Check(
        session.PrimeConfirmationCapture(nullptr) ==
                OperatorSessionActions::kComplete &&
            session.confirmationCapturePrimed_ &&
            session.observation_.last_checkpoint ==
                OperatorSessionActions::kConfirmationCheckpoint &&
            g_captureCalls == 1 &&
            g_captureCheckpoint ==
                OperatorSessionActions::kConfirmationCheckpoint,
        "the confirmation frame is queued before the original Options BuyBtn callback without blocking that transition");
    Check(
        session.PrimeConfirmationCapture(nullptr) ==
                OperatorSessionActions::kComplete &&
            g_captureCalls == 1,
        "the pre-transition confirmation capture is queued at most once");
    Check(
        session.RefreshConfirmationCapture(nullptr) ==
                OperatorSessionActions::kComplete &&
            g_captureCalls == 2 &&
            g_captureCheckpoint ==
                OperatorSessionActions::kConfirmationCheckpoint,
        "an in-flight confirmation capture does not block native fade-in state observation");
    g_captureResult =
        OperatorSessionActions::kCaptureReady;
    Check(
        session.RefreshConfirmationCapture(nullptr) ==
                OperatorSessionActions::kComplete &&
            g_captureCalls == 3,
        "an early confirmation frame receipt can be refreshed before the financial callback boundary");

    g_captureCalls = 0;
    g_captureResult =
        OperatorSessionActions::kCaptureFailed;
    OperatorSessionActions failed;
    failed.config_.capture_frames = true;
    failed.captureGate_ = &CaptureGate;
    Check(
        failed.PrimeConfirmationCapture(nullptr) ==
                OperatorSessionActions::kCaptureRejected &&
            failed.stage_ ==
                OperatorSessionActions::kStageFailed &&
            g_captureCalls == 1,
        "a failed pre-transition confirmation capture rejects the session before the Options callback");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 &&
        std::strcmp(argv[1], "--config") == 0) {
        wchar_t path[MAX_PATH] = {};
        if (!MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                argv[2],
                -1,
                path,
                static_cast<int>(std::size(path)))) {
            std::fprintf(
                stderr,
                "CONFIG_PATH_INVALID\n");
            return 2;
        }
        OperatorSessionActions::Config config;
        bool enabled = false;
        uint32_t failure = 0;
        const bool loaded =
            OperatorSessionActions::LoadConfig(
                path,
                &config,
                &enabled,
                &failure);
        std::printf(
            "LOAD_CONFIG=%u FAILURE=%u ENABLED=%u OPERATOR=%u PRODUCT=%u TEMPLATE=%u OPTION=%u ITEM=%s CATEGORY=%u PURCHASE=%u\n",
            loaded ? 1u : 0u,
            failure,
            enabled ? 1u : 0u,
            config.operator_id,
            config.product_id,
            config.template_id,
            config.option_id,
            config.item_id.c_str(),
            config.category_id,
            config.purchase ? 1u : 0u);
        return loaded && enabled ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--suite") != 0) {
        std::fprintf(
            stderr,
            "usage: game_test_operator_session_actions_test [--suite] [--config run.ini]\n");
        return 2;
    }
    std::puts(
        "OFFLINE ONLY: operator session plan/receipt regression; no game, server, SQL, or account mutation");
    TestConfigContract();
    TestFinalIntegrity();
    TestReplyPublication();
    TestHookScope();
    TestStageTimeouts();
    TestShopDiagnostics();
    TestOptionsCapturePriming();
    TestConfirmationCapturePriming();
    OperatorSessionActions::active_ = nullptr;
    return g_failures ? 1 : 0;
}
