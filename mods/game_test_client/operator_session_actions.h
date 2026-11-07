#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "hangar_navigation.h"
#include "native_actions.h"
#include "operator_flow_actions.h"
#include "shop_catalog_actions.h"

// Coordinates one sealed operator acceptance run through the original client.
//
// Purchase authorization is intentionally separate from ownership acceptance.
// A purchase=true run enters the ordinary shop and hands its validated options
// popup to OperatorFlowActions. A purchase=false run never enters the shop and
// instead requires exactly one already-owned target operator. Both paths then
// use the shipped operator-inventory UI for the optional first nickname and
// activation. No packet is constructed and no mutation is retried.
class OperatorSessionActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForInventory = 1,
        kWaitingForShop = 2,
        kWaitingForPurchase = 3,
        kWaitingForNaming = 4,
        kWaitingForNamingReply = 5,
        kWaitingForActivation = 6,
        kWaitingForActivationReply = 7,
        kWaitingForCapture = 8,
        kWaitingForRecovery = 9,
        kComplete = 10,
        kWaitingForShopRecovery = 11,

        kInvalidContext = 0x100,
        kRunChanged = 0x101,
        kThreadChanged = 0x102,
        kProcessChanged = 0x103,
        kTimeMovedBackward = 0x104,
        kConfigurationRejected = 0x105,
        kInventoryRejected = 0x106,
        kOwnershipMismatch = 0x107,
        kWalletRejected = 0x108,
        kShopRejected = 0x109,
        kPurchaseRejected = 0x10A,
        kNamingRejected = 0x10B,
        kNamingRequestRejected = 0x10C,
        kNamingReplyRejected = 0x10D,
        kNamingReadbackMismatch = 0x10E,
        kActivationRejected = 0x10F,
        kActivationRequestRejected = 0x110,
        kActivationReplyRejected = 0x111,
        kActivationReadbackMismatch = 0x112,
        kCaptureRejected = 0x113,
        kRecoveryRejected = 0x114,
        kTimedOut = 0x115,
        kNativeFault = 0x116,
        kShopRecoveryRejected = 0x117,
    };

    enum Stage : uint32_t
    {
        kStageIdle = 0,
        kStageBaseline,
        kStageShopRecovery,
        kStageShop,
        kStageOptionsCapture,
        kStagePurchase,
        kStageNaming,
        kStageNamingReply,
        kStageActivation,
        kStageActivationReply,
        kStageActiveCapture,
        kStageRecovery,
        kStageDone,
        kStageFailed,
    };

    enum Checkpoint : uint32_t
    {
        kOptionsCheckpoint = 1,
        kConfirmationCheckpoint = 2,
        kNicknameCheckpoint = 3,
        kActiveCheckpoint = 4,
        kConfirmationTimeoutCheckpoint = 5,
    };

    enum CaptureResult : uint32_t
    {
        kCaptureReady = 0,
        kCapturePending = 1,
        kCaptureFailed = 2,
    };

    struct Config
    {
        uint32_t operator_id = 0;
        uint32_t product_id = 0;
        uint32_t template_id = 0;
        uint32_t option_id = 0;
        std::string item_id;
        uint32_t category_id = 0;
        int32_t price = 0;
        int32_t validity = 0;
        uint8_t payment = 0;
        bool purchase = false;
        bool capture_frames = false;
        std::wstring name;
        std::wstring label;
        std::string nickname;
        std::wstring shop_path;
        std::wstring shop_sha256;
        std::wstring shop_display_path;
        std::wstring shop_display_sha256;
        uint64_t ui_timeout_ms = 15000;
        uint64_t shop_timeout_ms = 90000;
        uint64_t network_timeout_ms = 30000;
        uint64_t readback_timeout_ms = 15000;
    };

    struct Observation
    {
        uint32_t result = kUnbound;
        uint32_t stage = kStageIdle;
        uint32_t operator_id = 0;
        uint32_t product_id = 0;
        uint32_t option_id = 0;
        uint32_t category_id = 0;
        uint32_t payment = 0;
        int32_t price = 0;
        int32_t validity = 0;
        uint32_t baseline_gp = 0;
        uint32_t baseline_cash = 0;
        uint32_t current_gp = 0;
        uint32_t current_cash = 0;
        uint32_t baseline_operator_count = 0;
        uint32_t current_operator_count = 0;
        uint64_t target_uid = 0;
        uint16_t target_slot = 0;
        uint8_t target_enabled = 0;
        uint8_t set_name_state = 0;
        uint8_t purchase_authorized = 0;
        uint8_t shop_handed_off = 0;
        uint8_t nickname_preserved = 0;
        uint8_t nickname_submitted = 0;
        uint8_t activation_submitted = 0;
        uint8_t profile_consistent = 0;
        uint8_t wallet_consistent = 0;
        uint8_t reserved = 0;
        uint32_t naming_request_sequence = 0;
        uint32_t naming_reply_sequence = 0;
        uint32_t activation_request_sequence = 0;
        uint32_t activation_reply_sequence = 0;
        uint16_t naming_reply_opcode = 0;
        uint16_t activation_reply_opcode = 0;
        uint32_t last_checkpoint = 0;
        uint32_t shop_navigation_result = 0;
        uint32_t shop_result = 0;
        uint32_t shop_stage = 0;
        uint32_t shop_product_id = 0;
        uint32_t shop_native_name_status = 0;
        uint32_t shop_native_name_grid_ready = 0;
        uint32_t shop_native_name_row_id = 0;
        uint32_t shop_native_name_occurrences = 0;
        uint32_t shop_native_name_rows_scanned = 0;
        uint32_t shop_native_name_scan_stop_id = 0;
        uint32_t shop_duplicate_native_title_rows = 0;
        uint32_t shop_inventory_failure = 0;
        uint32_t shop_inventory_probe_slot = 0;
        uint32_t shop_inventory_probe_page = 0;
        uint32_t shop_inventory_ready = 0;
        int32_t shop_inventory_capacity = 0;
        uint32_t shop_inventory_brief_unit = 0;
        uint32_t shop_inventory_full_unit = 0;
        uint16_t shop_inventory_brief_slot = 0;
        uint16_t shop_inventory_full_slot = 0;
        uint64_t shop_inventory_brief_uid = 0;
        uint64_t shop_inventory_full_uid = 0;
        std::wstring shop_sealed_name;
        std::wstring shop_native_name;
        std::wstring shop_duplicate_native_name;
        std::wstring shop_visible_name;
        std::string nickname;
    };

    using CaptureGateFn = CaptureResult(__cdecl*)(
        void*,
        Checkpoint,
        const SdgoGameTestContextV1*);

    enum ConfigLoadFailure : uint32_t
    {
        kConfigLoaded = 0,
        kConfigInvalidArguments = 1,
        kConfigValueTruncated = 2,
        kConfigSectionTruncated = 3,
        kConfigDuplicateKey = 4,
        kConfigUnknownKey = 5,
        kConfigPartialScope = 6,
        kConfigInvalidNumeric = 7,
        kConfigInvalidIdentity = 8,
        kConfigInvalidText = 9,
        kConfigInvalidPurchase = 10,
        kConfigInvalidArtifacts = 11,
    };

    static bool LoadConfig(
        const std::wstring& runIni,
        Config* output,
        bool* enabled,
        uint32_t* failure = nullptr)
    {
        if (failure)
            *failure = kConfigLoaded;
        const auto reject =
            [failure](ConfigLoadFailure reason) {
                if (failure)
                    *failure = reason;
                return false;
            };
        if (!output || !enabled || runIni.empty())
            return reject(kConfigInvalidArguments);
        *output = {};
        *enabled = false;

        static const wchar_t* const keys[] = {
            L"operator_flow_operator_id",
            L"operator_flow_product_id",
            L"operator_flow_template_id",
            L"operator_flow_option_id",
            L"operator_flow_item_id",
            L"operator_flow_category_id",
            L"operator_flow_price",
            L"operator_flow_validity",
            L"operator_flow_payment",
            L"operator_flow_name",
            L"operator_flow_label",
            L"operator_flow_nickname",
            L"operator_flow_purchase",
            L"operator_shop_path",
            L"operator_shop_sha256",
            L"operator_shop_display_path",
            L"operator_shop_display_sha256",
        };

        std::array<std::wstring, std::size(keys)> values;
        size_t present = 0;
        for (size_t index = 0; index < std::size(keys); ++index) {
            wchar_t value[256] = {};
            const DWORD length = GetPrivateProfileStringW(
                L"run",
                keys[index],
                L"",
                value,
                static_cast<DWORD>(std::size(value)),
                runIni.c_str());
            if (length >= std::size(value) - 1)
                return reject(kConfigValueTruncated);
            // GetPrivateProfileStringW reports a byte-derived length for some
            // UTF-8 values in a non-BOM profile. Trust the terminator instead,
            // then replace the two non-ASCII fields from the original UTF-8
            // bytes below.
            values[index].assign(value);
            if (length)
                ++present;
        }

        std::vector<wchar_t> section(65536);
        std::array<uint8_t, std::size(keys)>
            occurrences = {};
        const DWORD sectionLength = GetPrivateProfileSectionW(
            L"run",
            section.data(),
            static_cast<DWORD>(section.size()),
            runIni.c_str());
        if (sectionLength >= section.size() - 2)
            return reject(kConfigSectionTruncated);
        for (const wchar_t* entry = section.data();
             *entry;
             entry += std::wcslen(entry) + 1) {
            const wchar_t* equals = std::wcschr(entry, L'=');
            const size_t length = equals
                ? static_cast<size_t>(equals - entry)
                : std::wcslen(entry);
            if (length < 9 ||
                _wcsnicmp(entry, L"operator_", 9) != 0)
                continue;
            bool known = false;
            for (size_t index = 0;
                 index < std::size(keys);
                 ++index) {
                if (std::wcslen(keys[index]) == length &&
                    std::wmemcmp(
                        entry,
                        keys[index],
                        length) == 0) {
                    known = true;
                    if (++occurrences[index] != 1)
                        return reject(kConfigDuplicateKey);
                    break;
                }
            }
            if (!known)
                return reject(kConfigUnknownKey);
        }

        if (!present)
            return true;
        if (present != std::size(keys))
            return reject(kConfigPartialScope);
        bool usedUtf8Text = false;
        std::wstring utf8Name;
        std::wstring utf8Label;
        if (!ReadUtf8RunTextFields(
                runIni,
                &utf8Name,
                &utf8Label,
                &usedUtf8Text))
            return reject(kConfigInvalidText);
        if (usedUtf8Text) {
            values[9] = std::move(utf8Name);
            values[10] = std::move(utf8Label);
        }

        Config config;
        uint32_t numeric[8] = {};
        const uint32_t minimums[8] = {
            60047, 1, 1, 1, 1, 1, 1, 1,
        };
        const uint32_t maximums[8] = {
            60999,
            static_cast<uint32_t>(INT32_MAX),
            static_cast<uint32_t>(INT32_MAX),
            static_cast<uint32_t>(INT32_MAX),
            static_cast<uint32_t>(INT32_MAX),
            static_cast<uint32_t>(INT32_MAX),
            1000000,
            2,
        };
        static const size_t valueIndexes[8] = {
            0, 1, 2, 3, 5, 6, 7, 8,
        };
        for (size_t index = 0; index < 8; ++index) {
            if (!CanonicalPositive(
                    values[valueIndexes[index]],
                    minimums[index],
                    maximums[index],
                    &numeric[index]))
                return reject(kConfigInvalidNumeric);
        }
        config.operator_id = numeric[0];
        config.product_id = numeric[1];
        config.template_id = numeric[2];
        config.option_id = numeric[3];
        config.category_id = numeric[4];
        config.price = static_cast<int32_t>(numeric[5]);
        config.validity = static_cast<int32_t>(numeric[6]);
        config.payment = static_cast<uint8_t>(numeric[7]);
        if (config.category_id !=
                1219000u + config.operator_id - 60000u ||
            config.product_id == config.template_id ||
            !ValidItemId(values[4]))
            return reject(kConfigInvalidIdentity);
        config.item_id.reserve(values[4].size());
        for (const wchar_t value : values[4])
            config.item_id.push_back(
                static_cast<char>(value));

        if (!ValidDisplayText(values[9]) ||
            !ValidDisplayText(values[10]) ||
            !ValidNickname(values[11]))
            return reject(kConfigInvalidText);
        config.name = values[9];
        config.label = values[10];
        config.nickname.reserve(
            values[11].size());
        for (const wchar_t value : values[11])
            config.nickname.push_back(
                static_cast<char>(value));
        if (values[12] == L"0")
            config.purchase = false;
        else if (values[12] == L"1")
            config.purchase = true;
        else
            return reject(kConfigInvalidPurchase);

        if (values[13] != L"inputs/operator-shop.ini" ||
            values[15] !=
                L"inputs/operator-shop-display.ini" ||
            !ValidDigest(values[14]) ||
            !ValidDigest(values[16]))
            return reject(kConfigInvalidArtifacts);
        config.shop_path = values[13];
        config.shop_sha256 = values[14];
        config.shop_display_path = values[15];
        config.shop_display_sha256 = values[16];

        *output = config;
        *enabled = true;
        return true;
    }

    bool Bind(
        const Config& config,
        NativeActions* native,
        HangarNavigation* navigation,
        ShopCatalogActions* shop,
        OperatorFlowActions* purchase,
        OperatorInventoryActions* inventory,
        uint8_t** profilePointer,
        CaptureGateFn captureGate = nullptr,
        void* captureContext = nullptr)
    {
        if (bound_ || !ValidConfig(config) || !native ||
            !navigation || !purchase || !inventory ||
            !profilePointer ||
            (config.purchase && !shop) ||
            (config.capture_frames && !captureGate))
            return false;
        config_ = config;
        native_ = native;
        navigation_ = navigation;
        shop_ = shop;
        purchase_ = purchase;
        inventory_ = inventory;
        profilePointer_ = profilePointer;
        captureGate_ = captureGate;
        captureContext_ = captureContext;
        optionsCapturePrimed_ = false;
        confirmationCapturePrimed_ = false;
        observation_ = {};
        observation_.operator_id = config.operator_id;
        observation_.product_id = config.product_id;
        observation_.option_id = config.option_id;
        observation_.category_id = config.category_id;
        observation_.price = config.price;
        observation_.validity = config.validity;
        observation_.payment = config.payment;
        observation_.purchase_authorized =
            config.purchase ? 1 : 0;
        stage_ = kStageBaseline;
        bound_ = true;
        return Publish(kWaitingForInventory) != kUnbound;
    }

    bool InstallHooks(
        const SdgoApi* api,
        SdgoMod* mod)
    {
        if (!bound_ || hooksInstalled_ || !api || !mod ||
            !api->HookCode ||
            (active_ && active_ != this))
            return false;
        active_ = this;
        if (config_.purchase) {
            const char* const shopSymbols[] = {
                "flow.operator_shop_dispatch_1",
                "flow.operator_shop_dispatch_2",
                "flow.operator_shop_dispatch_3",
            };
            void* const shopDetours[] = {
                reinterpret_cast<void*>(&ObserveShopDispatch1),
                reinterpret_cast<void*>(&ObserveShopDispatch2),
                reinterpret_cast<void*>(&ObserveShopDispatch3),
            };
            for (size_t index = 0;
                 index < std::size(shopSymbols);
                 ++index) {
                if (!api->HookCode(
                        mod,
                        shopSymbols[index],
                        shopDetours[index],
                        reinterpret_cast<void**>(
                            &shopDispatch_[index])))
                    return false;
            }
        }
        if (!api->HookCode(
                mod,
                "flow.operator_inventory_dispatch",
                reinterpret_cast<void*>(
                    &ObserveInventoryDispatch),
                reinterpret_cast<void**>(
                    &inventoryDispatch_)))
            return false;
        hooksInstalled_ = true;
        return true;
    }

    Result Poll(
        const SdgoGameTestContextV1* context)
    {
        if (stage_ == kStageDone ||
            stage_ == kStageFailed)
            return static_cast<Result>(
                observation_.result);
        const Result identity = CheckContext(context);
        if (identity != kComplete)
            return Fail(identity);
        if (!bound_ || !hooksInstalled_)
            return Publish(kUnbound);
        if (UiStageExpired())
            return Fail(kTimedOut);

        switch (stage_) {
        case kStageBaseline:
            return CaptureBaseline();
        case kStageShopRecovery:
            return RestoreShopHangar(context);
        case kStageShop:
            return PollShop(context);
        case kStageOptionsCapture:
            return CaptureOptions(context);
        case kStagePurchase:
            return PollPurchase(context);
        case kStageNaming:
            return PollNaming(context);
        case kStageNamingReply:
            return PollNamingReply();
        case kStageActivation:
            return PollActivation();
        case kStageActivationReply:
            return PollActivationReply();
        case kStageActiveCapture:
            return CaptureActive(context);
        case kStageRecovery:
            return Recover();
        default:
            return Fail(kNativeFault);
        }
    }

    Observation State() const
    {
        Observation value = observation_;
        value.stage = stage_;
        value.naming_reply_sequence =
            ReadPublishedSequence(
                namingReplySequence_);
        value.activation_reply_sequence =
            ReadPublishedSequence(
                activationReplySequence_);
        value.naming_reply_opcode =
            static_cast<uint16_t>(
                ReadAtomic(namingReplyOpcode_));
        value.activation_reply_opcode =
            static_cast<uint16_t>(
                ReadAtomic(activationReplyOpcode_));
        return value;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kWaitingForInventory: return "waiting-for-inventory";
        case kWaitingForShop: return "waiting-for-shop";
        case kWaitingForPurchase: return "waiting-for-purchase";
        case kWaitingForNaming: return "waiting-for-naming";
        case kWaitingForNamingReply: return "waiting-for-naming-reply";
        case kWaitingForActivation: return "waiting-for-activation";
        case kWaitingForActivationReply: return "waiting-for-activation-reply";
        case kWaitingForCapture: return "waiting-for-capture";
        case kWaitingForRecovery: return "waiting-for-recovery";
        case kComplete: return "complete";
        case kWaitingForShopRecovery:
            return "waiting-for-shop-recovery";
        case kInvalidContext: return "invalid-context";
        case kRunChanged: return "run-changed";
        case kThreadChanged: return "thread-changed";
        case kProcessChanged: return "process-changed";
        case kTimeMovedBackward: return "time-moved-backward";
        case kConfigurationRejected: return "configuration-rejected";
        case kInventoryRejected: return "inventory-rejected";
        case kOwnershipMismatch: return "ownership-mismatch";
        case kWalletRejected: return "wallet-rejected";
        case kShopRejected: return "shop-rejected";
        case kPurchaseRejected: return "purchase-rejected";
        case kNamingRejected: return "naming-rejected";
        case kNamingRequestRejected: return "naming-request-rejected";
        case kNamingReplyRejected: return "naming-reply-rejected";
        case kNamingReadbackMismatch: return "naming-readback-mismatch";
        case kActivationRejected: return "activation-rejected";
        case kActivationRequestRejected: return "activation-request-rejected";
        case kActivationReplyRejected: return "activation-reply-rejected";
        case kActivationReadbackMismatch: return "activation-readback-mismatch";
        case kCaptureRejected: return "capture-rejected";
        case kRecoveryRejected: return "recovery-rejected";
        case kTimedOut: return "timed-out";
        case kNativeFault: return "native-fault";
        case kShopRecoveryRejected:
            return "shop-recovery-rejected";
        default: return "unknown";
        }
    }

private:
    using DispatchFn =
        int(__thiscall*)(
            void*,
            int,
            int,
            int,
            int,
            void*);

    static constexpr uint16_t kRequestNickname = 0x04D8;
    static constexpr uint16_t kReplyNickname = 0x08CB;
    static constexpr uint16_t kRequestActivation = 0x04AD;
    static constexpr uint16_t kReplyActivation = 0x08A0;
    static constexpr uint16_t kReplyPage = 0x059F;
    static constexpr uint32_t kRequestNicknameSize = 0x1F;
    static constexpr uint32_t kReplyNicknameSize = 0x0E;
    static constexpr uint32_t kRequestActivationSize = 0x0E;
    static constexpr uint32_t kReplyActivationSize = 0x56;
    static constexpr uint32_t kReplyPageMaximumSize =
        7 + 6 * 0x50;
    static constexpr uint32_t kShop074DSize = 0x33;
    static constexpr uint32_t kShop0574Size = 0xD4;
    static constexpr size_t kProfileGp = 0x18F;
    static constexpr size_t kProfileCash = 0x58C;

    struct PacketCopy
    {
        std::array<
            uint8_t,
            kReplyPageMaximumSize> bytes = {};
        uint32_t size = 0;
        bool valid = false;
    };

    static bool CanonicalPositive(
        const std::wstring& text,
        uint32_t minimum,
        uint32_t maximum,
        uint32_t* output)
    {
        if (!output || text.empty() ||
            text.front() < L'1' || text.front() > L'9')
            return false;
        for (const wchar_t value : text)
            if (value < L'0' || value > L'9')
                return false;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long value =
            std::wcstoul(text.c_str(), &end, 10);
        if (errno || !end || *end ||
            value < minimum || value > maximum ||
            std::to_wstring(value) != text)
            return false;
        *output = static_cast<uint32_t>(value);
        return true;
    }

    static bool ValidDisplayText(
        const std::wstring& text)
    {
        if (text.empty() || text.size() > 50)
            return false;
        for (size_t index = 0;
             index < text.size();
             ++index) {
            const wchar_t value = text[index];
            if (!value || value == L'\r' ||
                value == L'\n')
                return false;
            if (value >= 0xD800 &&
                value <= 0xDBFF) {
                if (++index >= text.size() ||
                    text[index] < 0xDC00 ||
                    text[index] > 0xDFFF)
                    return false;
            } else if (value >= 0xDC00 &&
                       value <= 0xDFFF) {
                return false;
            }
        }
        return true;
    }

    static bool ReadUtf8RunTextFields(
        const std::wstring& path,
        std::wstring* name,
        std::wstring* label,
        bool* applied)
    {
        if (!name || !label || !applied)
            return false;
        *applied = false;
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size = {};
        bool okay =
            GetFileSizeEx(file, &size) != FALSE &&
            size.QuadPart > 0 &&
            size.QuadPart <= 1024 * 1024;
        std::vector<char> bytes;
        if (okay) {
            bytes.resize(
                static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            okay =
                ReadFile(
                    file,
                    bytes.data(),
                    static_cast<DWORD>(bytes.size()),
                    &read,
                    nullptr) != FALSE &&
                read == bytes.size();
        }
        CloseHandle(file);
        if (!okay)
            return false;
        if (bytes.size() >= 2 &&
            static_cast<uint8_t>(bytes[0]) == 0xFF &&
            static_cast<uint8_t>(bytes[1]) == 0xFE)
            return true;
        if (bytes.size() >= 2 &&
            static_cast<uint8_t>(bytes[0]) == 0xFE &&
            static_cast<uint8_t>(bytes[1]) == 0xFF)
            return false;
        size_t offset = 0;
        if (bytes.size() >= 3 &&
            static_cast<uint8_t>(bytes[0]) == 0xEF &&
            static_cast<uint8_t>(bytes[1]) == 0xBB &&
            static_cast<uint8_t>(bytes[2]) == 0xBF)
            offset = 3;
        if (std::find(
                bytes.begin() +
                    static_cast<ptrdiff_t>(offset),
                bytes.end(),
                '\0') != bytes.end())
            return false;

        const auto decode =
            [](const char* value,
               size_t length,
               std::wstring* output) {
                if (!length) {
                    output->clear();
                    return true;
                }
                const int count = MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value,
                    static_cast<int>(length),
                    nullptr,
                    0);
                if (count <= 0)
                    return false;
                output->assign(
                    static_cast<size_t>(count),
                    L'\0');
                return MultiByteToWideChar(
                           CP_UTF8,
                           MB_ERR_INVALID_CHARS,
                           value,
                           static_cast<int>(length),
                           output->data(),
                           count) == count;
            };

        const std::string nameKey =
            "operator_flow_name=";
        const std::string labelKey =
            "operator_flow_label=";
        bool inRun = false;
        bool foundName = false;
        bool foundLabel = false;
        while (offset <= bytes.size()) {
            size_t end = offset;
            while (end < bytes.size() &&
                   bytes[end] != '\n')
                ++end;
            size_t length = end - offset;
            if (length && bytes[offset + length - 1] == '\r')
                --length;
            const char* line = bytes.data() + offset;
            if (length >= 2 && line[0] == '[' &&
                line[length - 1] == ']') {
                inRun =
                    length == 5 &&
                    std::memcmp(
                        line,
                        "[run]",
                        5) == 0;
            } else if (inRun) {
                const auto readField =
                    [&](const std::string& key,
                        bool* found,
                        std::wstring* output) {
                        if (length < key.size() ||
                            std::memcmp(
                                line,
                                key.data(),
                                key.size()) != 0)
                            return true;
                        if (*found)
                            return false;
                        *found = true;
                        return decode(
                            line + key.size(),
                            length - key.size(),
                            output);
                    };
                if (!readField(
                        nameKey,
                        &foundName,
                        name) ||
                    !readField(
                        labelKey,
                        &foundLabel,
                        label))
                    return false;
            }
            if (end == bytes.size())
                break;
            offset = end + 1;
        }
        *applied = true;
        return foundName && foundLabel;
    }

    static bool ValidNickname(
        const std::wstring& text)
    {
        if (text.empty() || text.size() > 16)
            return false;
        for (const wchar_t value : text)
            if (!((value >= L'a' && value <= L'z') ||
                  (value >= L'A' && value <= L'Z') ||
                  (value >= L'0' && value <= L'9') ||
                  value == L'_'))
                return false;
        return true;
    }

    static bool ValidItemId(
        const std::wstring& text)
    {
        if (text.size() != 6)
            return false;
        for (const wchar_t value : text)
            if (value < L'0' || value > L'9')
                return false;
        return true;
    }

    static bool ValidItemId(
        const std::string& text)
    {
        if (text.size() != 6)
            return false;
        for (const unsigned char value : text)
            if (value < '0' || value > '9')
                return false;
        return true;
    }

    static bool ValidDigest(
        const std::wstring& text)
    {
        if (text.size() != 64)
            return false;
        for (const wchar_t value : text)
            if (!((value >= L'0' && value <= L'9') ||
                  (value >= L'a' && value <= L'f')))
                return false;
        return true;
    }

    static bool ValidConfig(const Config& config)
    {
        if (config.operator_id < 60047 ||
            config.operator_id > 60999 ||
            !config.product_id || !config.template_id ||
            config.product_id == config.template_id ||
            !config.option_id ||
            config.option_id >
                static_cast<uint32_t>(INT32_MAX) ||
            !ValidItemId(config.item_id) ||
            config.category_id !=
                1219000u + config.operator_id - 60000u ||
            config.price <= 0 ||
            config.validity <= 0 ||
            config.validity > 1000000 ||
            (config.payment != 1 &&
             config.payment != 2) ||
            !ValidDisplayText(config.name) ||
            !ValidDisplayText(config.label) ||
            config.nickname.empty() ||
            config.nickname.size() > 16 ||
            config.shop_path !=
                L"inputs/operator-shop.ini" ||
            config.shop_display_path !=
                L"inputs/operator-shop-display.ini" ||
            !ValidDigest(config.shop_sha256) ||
            !ValidDigest(config.shop_display_sha256) ||
            !config.ui_timeout_ms ||
            config.shop_timeout_ms <
                config.ui_timeout_ms ||
            config.shop_timeout_ms > 300000 ||
            !config.network_timeout_ms ||
            !config.readback_timeout_ms)
            return false;
        for (const unsigned char value :
             config.nickname)
            if (!((value >= 'a' && value <= 'z') ||
                  (value >= 'A' && value <= 'Z') ||
                  (value >= '0' && value <= '9') ||
                  value == '_'))
                return false;
        return true;
    }

    Result CaptureBaseline()
    {
        OperatorFlowActions::InventoryImage image;
        const auto ready =
            inventory_->ReadFullInventory(
                &image);
        if (ready >=
            OperatorInventoryActions::
                kNavigationRejected)
            return Fail(kInventoryRejected);
        if (ready !=
            OperatorInventoryActions::kReady)
            return Publish(kWaitingForInventory);

        if (!InventoryValid(image))
            return Fail(kInventoryRejected);
        OperatorFlowActions::WalletImage wallet;
        if (!ReadWallet(&wallet))
            return Fail(kWalletRejected);

        baselineInventory_ = image;
        baselineWallet_ = wallet;
        observation_.baseline_gp = wallet.gp;
        observation_.baseline_cash = wallet.cash;
        observation_.baseline_operator_count =
            static_cast<uint32_t>(
                image.records.size());

        const auto matches =
            TargetRecords(image);
        if (config_.purchase) {
            if (!matches.empty() ||
                !purchase_->PrimeBaseline(
                    wallet,
                    image))
                return Fail(kOwnershipMismatch);
            if (!navigation_->
                    AllowUnitManagerReentry(
                        HangarNavigation::
                            kBeforeOperatorShop))
                return Fail(
                    kShopRecoveryRejected);
            shopStartedAt_ = lastElapsed_;
            Transition(kStageShopRecovery);
            return RestoreShopHangar(nullptr);
        }
        if (matches.size() != 1)
            return Fail(kOwnershipMismatch);
        SetTarget(matches.front());
        Transition(kStageNaming);
        return Publish(kWaitingForNaming);
    }

    Result RestoreShopHangar(
        const SdgoGameTestContextV1* context)
    {
        const auto result =
            navigation_->EnsureHangar();
        observation_.shop_navigation_result =
            static_cast<uint32_t>(result);
        if (result >=
                HangarNavigation::
                    kDownmenuServiceMismatch ||
            result == HangarNavigation::kUnbound)
            return Fail(kShopRecoveryRejected);
        if (result != HangarNavigation::kReady)
            return Publish(
                kWaitingForShopRecovery);
        Transition(kStageShop);
        return context
            ? PollShop(context)
            : Publish(kWaitingForShop);
    }

    Result PollShop(
        const SdgoGameTestContextV1* context)
    {
        // Queue the real backbuffer capture before the original BuyBtn
        // callback opens the modal. A static modal can finish its last Present
        // before HandoffBuyOptions observes state 4, leaving a later request
        // pending forever. The genuine UI transition now supplies the frame
        // that consumes this request; CaptureOptions still verifies completion
        // before handing the popup to the purchase flow.
        const auto before = shop_->State();
        if (before.stage ==
                ShopCatalogActions::kStageOpenBuy &&
            !optionsCapturePrimed_) {
            const Result primed =
                PrimeOptionsCapture(context);
            if (primed != kComplete)
                return primed;
        }
        const auto result = shop_->Poll(context);
        RecordShopState(result, shop_->State());
        if (result >=
                ShopCatalogActions::kInvalidContext ||
            result == ShopCatalogActions::kUnbound ||
            result == ShopCatalogActions::kUnconfigured)
            return Fail(kShopRejected);

        void* popup = nullptr;
        if (!shop_->HandoffBuyOptions(
                config_.product_id,
                config_.category_id,
                &popup))
            return Publish(kWaitingForShop);
        if (!popup)
            return Fail(kShopRejected);
        optionsPopup_ = popup;
        observation_.shop_handed_off = 1;
        Transition(kStageOptionsCapture);
        return CaptureOptions(context);
    }

    Result PrimeOptionsCapture(
        const SdgoGameTestContextV1* context)
    {
        if (optionsCapturePrimed_)
            return kComplete;
        if (!config_.capture_frames) {
            optionsCapturePrimed_ = true;
            return kComplete;
        }
        const CaptureResult captured =
            captureGate_(
                captureContext_,
                kOptionsCheckpoint,
                context);
        if (captured == kCaptureFailed)
            return Fail(kCaptureRejected);
        observation_.last_checkpoint =
            kOptionsCheckpoint;
        optionsCapturePrimed_ = true;
        return kComplete;
    }

    void RecordShopState(
        ShopCatalogActions::Result result,
        const ShopCatalogActions::Observation& state)
    {
        observation_.shop_result =
            static_cast<uint32_t>(result);
        observation_.shop_stage = state.stage;
        observation_.shop_product_id=state.product_id;
        observation_.shop_native_name_status=
            state.native_name_status;
        observation_.shop_native_name_grid_ready=
            state.native_name_grid_ready;
        observation_.shop_native_name_row_id=
            state.native_name_row_id;
        observation_.shop_native_name_occurrences=
            state.native_name_occurrences;
        observation_.shop_native_name_rows_scanned=
            state.native_name_rows_scanned;
        observation_.shop_native_name_scan_stop_id=
            state.native_name_scan_stop_id;
        observation_.shop_duplicate_native_title_rows=
            state.duplicate_native_title_rows;
        observation_.shop_sealed_name=state.sealed_name;
        observation_.shop_native_name=state.native_name;
        observation_.shop_duplicate_native_name=
            state.duplicate_native_name;
        observation_.shop_visible_name=state.visible_name;
        observation_.shop_inventory_failure =
            state.inventory_failure;
        observation_.shop_inventory_probe_slot =
            state.inventory_probe_slot;
        observation_.shop_inventory_probe_page =
            state.inventory_probe_slot
                ? (state.inventory_probe_slot - 1) / 6
                : 0;
        observation_.shop_inventory_ready =
            state.inventory_ready;
        observation_.shop_inventory_capacity =
            state.inventory_capacity;
        observation_.shop_inventory_brief_unit =
            state.inventory_brief_unit;
        observation_.shop_inventory_full_unit =
            state.inventory_full_unit;
        observation_.shop_inventory_brief_slot =
            static_cast<uint16_t>(
                state.inventory_brief_slot);
        observation_.shop_inventory_full_slot =
            static_cast<uint16_t>(
                state.inventory_full_slot);
        observation_.shop_inventory_brief_uid =
            state.inventory_brief_uid;
        observation_.shop_inventory_full_uid =
            state.inventory_full_uid;
    }

    Result CaptureOptions(
        const SdgoGameTestContextV1* context)
    {
        const Result capture =
            Capture(kOptionsCheckpoint, context);
        if (capture != kComplete)
            return capture;
        if (!purchase_->AdoptOptionsPopup(
                optionsPopup_))
            return Fail(kPurchaseRejected);
        optionsPopup_ = nullptr;
        Transition(kStagePurchase);
        return Publish(kWaitingForPurchase);
    }

    Result PollPurchase(
        const SdgoGameTestContextV1* context)
    {
        const auto before = purchase_->State();
        if (before.stage == OperatorFlowActions::kStageReadback &&
            native_->AppState() == 9 &&
            !navigation_->BeginOperatorShopReturn())
            return Fail(kShopRecoveryRejected);
        // Queue checkpoint 2 before the original Options BuyBtn callback
        // opens the confirmation dialog. The graphics gate keeps that request
        // pending until one genuine Present observes the exact initialized
        // confirmation state.
        if (before.stage ==
                OperatorFlowActions::
                    kStageInvokeOptions &&
            !confirmationCapturePrimed_) {
            const Result primed =
                PrimeConfirmationCapture(
                    context);
            if (primed != kComplete)
                return primed;
        }
        if (before.stage ==
                OperatorFlowActions::
                    kStageWaitConfirmation &&
            confirmationCapturePrimed_) {
            const Result refreshed =
                RefreshConfirmationCapture(
                    context);
            if (refreshed != kComplete)
                return refreshed;
        }
        if (before.stage ==
            OperatorFlowActions::
                kStageInvokeConfirmation) {
            if (config_.capture_frames &&
                !confirmationCapturePrimed_)
                return Fail(
                    kCaptureRejected);
            const Result capture =
                Capture(
                    kConfirmationCheckpoint,
                    context);
            if (capture != kComplete)
                return capture;
        }

        const auto result = purchase_->Poll(context);
        const auto purchaseState = purchase_->State();
        const bool confirmationTimedOut =
            result == OperatorFlowActions::kTimedOut &&
            (before.stage ==
                 OperatorFlowActions::
                     kStageWaitConfirmation ||
             (purchaseState.stage ==
                  OperatorFlowActions::kStageFailed &&
              purchaseState.options_actions == 1 &&
              !purchaseState.confirmation_validated &&
              !purchaseState.mutation_attempted));
        if (confirmationTimedOut) {
            const Result capture =
                Capture(
                    kConfirmationTimeoutCheckpoint,
                    context);
            if (capture != kComplete)
                return capture;
        }
        if (result >=
                OperatorFlowActions::
                    kInvalidContext ||
            result == OperatorFlowActions::kUnbound ||
            result ==
                OperatorFlowActions::kUnconfigured)
            return Fail(kPurchaseRejected);
        if (result != OperatorFlowActions::kComplete)
            return Publish(kWaitingForPurchase);

        const auto state = purchaseState;
        if (!state.new_operator_uid ||
            !state.wallet_verified ||
            !state.operator_verified ||
            !state.original_0574_completed)
            return Fail(kPurchaseRejected);
        targetUid_ = state.new_operator_uid;

        OperatorFlowActions::InventoryImage image;
        const auto inventoryResult =
            inventory_->ReadFullInventory(
                &image);
        if (inventoryResult <
                OperatorInventoryActions::
                    kNavigationRejected &&
            inventoryResult !=
                OperatorInventoryActions::
                    kReady)
            return Publish(
                kWaitingForPurchase);
        if (inventoryResult !=
                OperatorInventoryActions::
                    kReady ||
            !InventoryValid(image))
            return Fail(kInventoryRejected);
        const auto matches = TargetRecords(image);
        if (matches.size() != 1 ||
            matches.front().uid != targetUid_)
            return Fail(kOwnershipMismatch);
        SetTarget(matches.front());
        Transition(kStageNaming);
        return Publish(kWaitingForNaming);
    }

    Result PrimeConfirmationCapture(
        const SdgoGameTestContextV1* context)
    {
        if (confirmationCapturePrimed_)
            return kComplete;
        if (!config_.capture_frames) {
            confirmationCapturePrimed_ = true;
            return kComplete;
        }
        const CaptureResult captured =
            captureGate_(
                captureContext_,
                kConfirmationCheckpoint,
                context);
        if (captured == kCaptureFailed)
            return Fail(kCaptureRejected);
        observation_.last_checkpoint =
            kConfirmationCheckpoint;
        confirmationCapturePrimed_ = true;
        return kComplete;
    }

    Result RefreshConfirmationCapture(
        const SdgoGameTestContextV1* context)
    {
        if (!config_.capture_frames)
            return kComplete;
        const CaptureResult captured =
            captureGate_(
                captureContext_,
                kConfirmationCheckpoint,
                context);
        if (captured == kCaptureFailed)
            return Fail(kCaptureRejected);
        observation_.last_checkpoint =
            kConfirmationCheckpoint;
        // Poll the prequeued request without blocking native state progress.
        // Only Capture() at the exact financial callback boundary waits for
        // the state-3 Present receipt.
        return kComplete;
    }

    Result PollNaming(
        const SdgoGameTestContextV1* context)
    {
        OperatorFlowActions::OperatorRecord record;
        std::string nickname;
        uint8_t setNameState = 0;
        const auto read =
            inventory_->ReadOperatorState(
                targetUid_,
                &record,
                &nickname,
                &setNameState);
        if (read <
                OperatorInventoryActions::
                    kNavigationRejected &&
            read !=
                OperatorInventoryActions::
                    kReady)
            return Publish(
                kWaitingForNaming);
        if (read !=
                OperatorInventoryActions::
                    kReady ||
            record.operator_id !=
                config_.operator_id)
            return Fail(kNamingReadbackMismatch);
        SetTarget(record);
        observation_.set_name_state = setNameState;

        const bool popupReady =
            inventory_->NamingPopupReady(
                config_.operator_id,
                targetUid_);
        if (!popupReady &&
            setNameState == 1 &&
            !nickname.empty() &&
            nickname != "Operator") {
            observation_.nickname = nickname;
            observation_.nickname_preserved = 1;
            Transition(kStageActivation);
            return Publish(kWaitingForActivation);
        }
        if (!popupReady &&
            setNameState > 1)
            return Fail(kNamingReadbackMismatch);

        if (popupReady) {
            const Result capture =
                Capture(
                    kNicknameCheckpoint,
                    context);
            if (capture != kComplete)
                return capture;
        }

        namingRequestBaseline_ =
            native_->TransportState().send_sequence;
        InterlockedExchange(
            &namingActionArmed_,
            1);
        const std::wstring wideNickname(
            config_.nickname.begin(),
            config_.nickname.end());
        const auto result =
            inventory_->SubmitNickname(
                config_.operator_id,
                targetUid_,
                wideNickname.c_str());
        if (result ==
                OperatorInventoryActions::
                    kWaitingForHangar ||
            result ==
                OperatorInventoryActions::
                    kOpeningInventory ||
            result ==
                OperatorInventoryActions::
                    kWaitingForInventory ||
            result ==
                OperatorInventoryActions::
                    kNavigatingPage ||
            result ==
                OperatorInventoryActions::
                    kWaitingForNamingPopup) {
            InterlockedExchange(
                &namingActionArmed_,
                0);
            return Publish(kWaitingForNaming);
        }
        if (result ==
            OperatorInventoryActions::
                kNameAlreadyApplied) {
            InterlockedExchange(
                &namingActionArmed_,
                0);
            return Publish(kWaitingForNaming);
        }
        if (result !=
            OperatorInventoryActions::
                kNamingSubmitted)
            return Fail(kNamingRejected);
        observation_.nickname_submitted = 1;
        Transition(kStageNamingReply);
        return PollNamingReply();
    }

    Result PollNamingReply()
    {
        const auto request =
            native_->FindTransportSince(
                namingRequestBaseline_,
                kRequestNickname);
        if (!request.send_sequence) {
            if (Expired(config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(kWaitingForNamingReply);
        }
        observation_.naming_request_sequence =
            request.send_sequence;
        if (request.packet_size !=
                kRequestNicknameSize ||
            !request.result)
            return Fail(kNamingRequestRejected);

        const uint32_t replySequence =
            ReadPublishedSequence(
                namingReplySequence_);
        if (!replySequence) {
            if (Expired(config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(kWaitingForNamingReply);
        }
        if (ReadAtomic(namingReplyUidLow_) !=
                static_cast<uint32_t>(targetUid_) ||
            ReadAtomic(namingReplyUidHigh_) !=
                static_cast<uint32_t>(
                    targetUid_ >> 32))
            return Fail(kNamingReplyRejected);
        if (!namingReplyObserved_) {
            namingReplyObserved_ = true;
            stageStartedAt_ = lastElapsed_;
        }

        OperatorFlowActions::OperatorRecord record;
        std::string nickname;
        uint8_t setNameState = 0;
        const auto read =
            inventory_->ReadOperatorState(
                targetUid_,
                &record,
                &nickname,
                &setNameState);
        if (read <
                OperatorInventoryActions::
                    kNavigationRejected &&
            read !=
                OperatorInventoryActions::
                    kReady) {
            if (Expired(config_.readback_timeout_ms))
                return Fail(
                    kNamingReadbackMismatch);
            return Publish(kWaitingForNamingReply);
        }
        if (read !=
                OperatorInventoryActions::
                    kReady ||
            record.operator_id != config_.operator_id ||
            nickname != config_.nickname ||
            setNameState != 1)
            return Fail(kNamingReadbackMismatch);
        SetTarget(record);
        observation_.nickname = nickname;
        observation_.set_name_state = setNameState;
        Transition(kStageActivation);
        return Publish(kWaitingForActivation);
    }

    Result PollActivation()
    {
        OperatorFlowActions::OperatorRecord record;
        std::string nickname;
        uint8_t setNameState = 0;
        const auto read =
            inventory_->ReadOperatorState(
                targetUid_,
                &record,
                &nickname,
                &setNameState);
        if (read <
                OperatorInventoryActions::
                    kNavigationRejected &&
            read !=
                OperatorInventoryActions::
                    kReady)
            return Publish(
                kWaitingForActivation);
        if (read !=
                OperatorInventoryActions::
                    kReady ||
            record.operator_id != config_.operator_id ||
            nickname.empty() || setNameState != 1)
            return Fail(kActivationReadbackMismatch);
        SetTarget(record);
        observation_.nickname = nickname;
        observation_.set_name_state = setNameState;

        if (record.enabled == 1) {
            const auto result = inventory_->Activate(
                config_.operator_id,
                targetUid_);
            if (result ==
                    OperatorInventoryActions::
                        kWaitingForHangar ||
                result ==
                    OperatorInventoryActions::
                        kOpeningInventory ||
                result ==
                    OperatorInventoryActions::
                        kWaitingForInventory ||
                result ==
                    OperatorInventoryActions::
                        kNavigatingPage)
                return Publish(kWaitingForActivation);
            if (result !=
                OperatorInventoryActions::kAlreadyActive)
                return Fail(kActivationRejected);
            if (!ActiveMatches(record)) {
                if (Expired(config_.readback_timeout_ms))
                    return Fail(
                        kActivationReadbackMismatch);
                return Publish(kWaitingForActivation);
            }
            Transition(kStageActiveCapture);
            return Publish(kWaitingForCapture);
        }

        activationRequestBaseline_ =
            native_->TransportState().send_sequence;
        InterlockedExchange(
            &activationActionArmed_,
            1);
        const auto result = inventory_->Activate(
            config_.operator_id,
            targetUid_);
        if (result ==
                OperatorInventoryActions::
                    kWaitingForHangar ||
            result ==
                OperatorInventoryActions::
                    kOpeningInventory ||
            result ==
                OperatorInventoryActions::
                    kWaitingForInventory ||
            result ==
                OperatorInventoryActions::
                    kNavigatingPage) {
            InterlockedExchange(
                &activationActionArmed_,
                0);
            return Publish(kWaitingForActivation);
        }
        if (result ==
            OperatorInventoryActions::kAlreadyActive) {
            InterlockedExchange(
                &activationActionArmed_,
                0);
            return Publish(kWaitingForActivation);
        }
        if (result !=
            OperatorInventoryActions::
                kActivationSubmitted)
            return Fail(kActivationRejected);
        observation_.activation_submitted = 1;
        Transition(kStageActivationReply);
        return PollActivationReply();
    }

    Result PollActivationReply()
    {
        const auto request =
            native_->FindTransportSince(
                activationRequestBaseline_,
                kRequestActivation);
        if (!request.send_sequence) {
            if (Expired(config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForActivationReply);
        }
        observation_.activation_request_sequence =
            request.send_sequence;
        if (request.packet_size !=
                kRequestActivationSize ||
            !request.result)
            return Fail(
                kActivationRequestRejected);

        const uint32_t replySequence =
            ReadPublishedSequence(
                activationReplySequence_);
        if (!replySequence) {
            if (Expired(config_.network_timeout_ms))
                return Fail(kTimedOut);
            return Publish(
                kWaitingForActivationReply);
        }
        if (!activationReplyObserved_) {
            activationReplyObserved_ = true;
            stageStartedAt_ = lastElapsed_;
        }

        OperatorFlowActions::OperatorRecord observed;
        observed.uid =
            static_cast<uint64_t>(
                ReadAtomic(activationReplyUidLow_)) |
            (static_cast<uint64_t>(
                 ReadAtomic(
                     activationReplyUidHigh_))
             << 32);
        observed.operator_id =
            ReadAtomic(
                activationReplyOperatorId_);
        observed.slot =
            static_cast<uint16_t>(
                ReadAtomic(
                    activationReplySlot_));
        observed.enabled =
            static_cast<uint8_t>(
                ReadAtomic(
                    activationReplyEnabled_));
        if (observed.uid != targetUid_ ||
            observed.operator_id != config_.operator_id ||
            !observed.slot ||
            observed.enabled != 1)
            return Fail(
                kActivationReplyRejected);

        OperatorFlowActions::OperatorRecord current;
        std::string nickname;
        uint8_t setNameState = 0;
        const auto read =
            inventory_->ReadOperatorState(
                targetUid_,
                &current,
                &nickname,
                &setNameState);
        if (read <
                OperatorInventoryActions::
                    kNavigationRejected &&
            read !=
                OperatorInventoryActions::
                    kReady) {
            if (Expired(config_.readback_timeout_ms))
                return Fail(
                    kActivationReadbackMismatch);
            return Publish(
                kWaitingForActivationReply);
        }
        if (read !=
                OperatorInventoryActions::
                    kReady ||
            !SameRecord(current, observed) ||
            nickname.empty() ||
            setNameState != 1 ||
            !ActiveMatches(current)) {
            if (Expired(config_.readback_timeout_ms))
                return Fail(
                    kActivationReadbackMismatch);
            return Publish(
                kWaitingForActivationReply);
        }
        SetTarget(current);
        observation_.nickname = nickname;
        observation_.set_name_state = setNameState;
        Transition(kStageActiveCapture);
        return Publish(kWaitingForCapture);
    }

    Result CaptureActive(
        const SdgoGameTestContextV1* context)
    {
        const Result capture =
            Capture(kActiveCheckpoint, context);
        if (capture != kComplete)
            return capture;
        if (!navigation_->AllowUnitManagerReentry())
            return Fail(kRecoveryRejected);
        Transition(kStageRecovery);
        return Recover();
    }

    Result Recover()
    {
        if (!recoveryVerified_) {
            OperatorFlowActions::InventoryImage image;
            OperatorFlowActions::WalletImage wallet;
            const auto inventoryResult =
                inventory_->ReadFullInventory(
                    &image);
            if (inventoryResult <
                    OperatorInventoryActions::
                        kNavigationRejected &&
                inventoryResult !=
                    OperatorInventoryActions::
                        kReady)
                return Publish(
                    kWaitingForRecovery);
            if (inventoryResult !=
                    OperatorInventoryActions::
                        kReady ||
                !InventoryValid(image) ||
                !ReadWallet(&wallet))
                return Fail(kInventoryRejected);
            const auto matches = TargetRecords(image);
            if (matches.size() != 1 ||
                matches.front().uid != targetUid_)
                return Fail(kOwnershipMismatch);
            if (!WalletPreserved(wallet))
                return Fail(kWalletRejected);
            if (!InventoryPreserved(image))
                return Fail(kOwnershipMismatch);
            observation_.current_gp = wallet.gp;
            observation_.current_cash = wallet.cash;
            observation_.current_operator_count =
                static_cast<uint32_t>(
                    image.records.size());
            observation_.wallet_consistent = 1;
            recoveryVerified_ = true;
        }

        const auto hangar =
            navigation_->EnsureHangar();
        if (hangar >=
                HangarNavigation::
                    kDownmenuServiceMismatch ||
            hangar == HangarNavigation::kUnbound)
            return Fail(kRecoveryRejected);
        if (hangar != HangarNavigation::kReady)
            return Publish(kWaitingForRecovery);
        stage_ = kStageDone;
        return Publish(kComplete);
    }

    Result Capture(
        Checkpoint checkpoint,
        const SdgoGameTestContextV1* context)
    {
        if (!config_.capture_frames)
            return kComplete;
        const CaptureResult captured =
            captureGate_(
                captureContext_,
                checkpoint,
                context);
        if (captured == kCaptureFailed)
            return Fail(kCaptureRejected);
        if (captured == kCapturePending) {
            observation_.last_checkpoint =
                checkpoint;
            return Publish(kWaitingForCapture);
        }
        observation_.last_checkpoint = checkpoint;
        return kComplete;
    }

    bool ActiveMatches(
        const OperatorFlowActions::
            OperatorRecord& current)
    {
        OperatorFlowActions::OperatorRecord active;
        if (!inventory_->ReadActiveOperator(&active) ||
            !SameRecord(current, active) ||
            active.uid != targetUid_ ||
            active.operator_id != config_.operator_id ||
            active.enabled != 1)
            return false;
        observation_.profile_consistent = 1;
        return true;
    }

    static bool SameRecord(
        const OperatorFlowActions::OperatorRecord& left,
        const OperatorFlowActions::OperatorRecord& right)
    {
        return left.uid == right.uid &&
               left.operator_id == right.operator_id &&
               left.slot == right.slot &&
               left.enabled == right.enabled;
    }

    static bool InventoryValid(
        const OperatorFlowActions::
            InventoryImage& image)
    {
        if (image.records.size() > 4096)
            return false;
        for (size_t index = 0;
             index < image.records.size();
             ++index) {
            const auto& record =
                image.records[index];
            if (!record.uid || !record.operator_id ||
                !record.slot || record.enabled > 1)
                return false;
            for (size_t other = 0;
                 other < index;
                 ++other)
                if (image.records[other].uid ==
                        record.uid ||
                    image.records[other].slot ==
                        record.slot)
                    return false;
        }
        return true;
    }

    std::vector<
        OperatorFlowActions::OperatorRecord>
    TargetRecords(
        const OperatorFlowActions::
            InventoryImage& image) const
    {
        std::vector<
            OperatorFlowActions::OperatorRecord>
            matches;
        for (const auto& record : image.records)
            if (record.operator_id ==
                config_.operator_id)
                matches.push_back(record);
        return matches;
    }

    bool WalletPreserved(
        const OperatorFlowActions::WalletImage& wallet)
        const
    {
        uint32_t expectedGp = baselineWallet_.gp;
        uint32_t expectedCash = baselineWallet_.cash;
        if (config_.purchase) {
            const uint32_t price =
                static_cast<uint32_t>(config_.price);
            if ((config_.payment == 1 &&
                 expectedGp < price) ||
                (config_.payment == 2 &&
                 expectedCash < price))
                return false;
            if (config_.payment == 1)
                expectedGp -= price;
            else
                expectedCash -= price;
        }
        return wallet.gp == expectedGp &&
               wallet.cash == expectedCash;
    }

    bool InventoryPreserved(
        const OperatorFlowActions::InventoryImage& image)
        const
    {
        const size_t expectedCount =
            baselineInventory_.records.size() +
            (config_.purchase ? 1u : 0u);
        if (image.records.size() != expectedCount)
            return false;

        for (const auto& baseline :
             baselineInventory_.records) {
            const auto current = std::find_if(
                image.records.begin(),
                image.records.end(),
                [&baseline](const auto& candidate) {
                    return candidate.uid == baseline.uid;
                });
            if (current == image.records.end() ||
                current->operator_id !=
                    baseline.operator_id ||
                current->slot != baseline.slot)
                return false;
            if (baseline.uid != targetUid_ &&
                current->enabled != 0)
                return false;
        }

        const auto target = std::find_if(
            image.records.begin(),
            image.records.end(),
            [this](const auto& candidate) {
                return candidate.uid == targetUid_;
            });
        if (target == image.records.end() ||
            target->operator_id != config_.operator_id ||
            target->slot != observation_.target_slot ||
            target->enabled != 1)
            return false;

        if (config_.purchase) {
            const auto existed = std::find_if(
                baselineInventory_.records.begin(),
                baselineInventory_.records.end(),
                [this](const auto& candidate) {
                    return candidate.uid == targetUid_;
                });
            if (existed !=
                baselineInventory_.records.end())
                return false;
        }
        return true;
    }

    void SetTarget(
        const OperatorFlowActions::
            OperatorRecord& record)
    {
        targetUid_ = record.uid;
        observation_.target_uid = record.uid;
        observation_.target_slot = record.slot;
        observation_.target_enabled =
            record.enabled;
    }

    bool ReadWallet(
        OperatorFlowActions::WalletImage* output) const
    {
        if (!output || !profilePointer_)
            return false;
        __try {
            const uint8_t* profile =
                *profilePointer_;
            if (!profile)
                return false;
            memcpy(
                &output->gp,
                profile + kProfileGp,
                sizeof(output->gp));
            memcpy(
                &output->cash,
                profile + kProfileCash,
                sizeof(output->cash));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Result CheckContext(
        const SdgoGameTestContextV1* context)
    {
        if (!context ||
            context->size !=
                sizeof(SdgoGameTestContextV1) ||
            context->version !=
                SDGO_GAME_TEST_VERSION ||
            !context->run_id || !context->role ||
            !context->case_id)
            return kInvalidContext;
        const uint64_t hash = HashContext(context);
        const DWORD process = GetCurrentProcessId();
        const DWORD thread = GetCurrentThreadId();
        if (!contextCaptured_) {
            contextCaptured_ = true;
            contextHash_ = hash;
            processId_ = process;
            uiThread_ = thread;
            lastElapsed_ = context->elapsed_ms;
            stageStartedAt_ =
                context->elapsed_ms;
            return kComplete;
        }
        if (process != processId_)
            return kProcessChanged;
        if (thread != uiThread_)
            return kThreadChanged;
        if (hash != contextHash_)
            return kRunChanged;
        if (context->elapsed_ms < lastElapsed_)
            return kTimeMovedBackward;
        lastElapsed_ = context->elapsed_ms;
        return kComplete;
    }

    static uint64_t HashString(
        uint64_t hash,
        const char* value)
    {
        while (value && *value) {
            hash ^= static_cast<uint8_t>(
                *value++);
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFF;
        return hash * 1099511628211ULL;
    }

    static uint64_t HashContext(
        const SdgoGameTestContextV1* context)
    {
        uint64_t hash =
            1469598103934665603ULL;
        hash = HashString(hash, context->run_id);
        hash = HashString(hash, context->role);
        return HashString(hash, context->case_id);
    }

    void Transition(Stage stage)
    {
        stage_ = stage;
        stageStartedAt_ = lastElapsed_;
    }

    bool Expired(uint64_t timeout) const
    {
        return lastElapsed_ >= stageStartedAt_ &&
               lastElapsed_ - stageStartedAt_ >=
                   timeout;
    }

    bool UiStageExpired() const
    {
        switch (stage_) {
        case kStageBaseline:
        case kStageOptionsCapture:
        case kStageNaming:
        case kStageActivation:
        case kStageActiveCapture:
        case kStageRecovery:
            return Expired(config_.ui_timeout_ms);
        case kStageShopRecovery:
        case kStageShop:
            return lastElapsed_ >=
                       shopStartedAt_ &&
                   lastElapsed_ -
                           shopStartedAt_ >=
                       config_.shop_timeout_ms;
        default:
            return false;
        }
    }

    Result Publish(Result result)
    {
        observation_.result = result;
        observation_.stage = stage_;
        return result;
    }

    Result Fail(Result result)
    {
        stage_ = kStageFailed;
        return Publish(result);
    }

    static uint32_t ReadAtomic(
        const volatile LONG& value)
    {
        return static_cast<uint32_t>(
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(&value),
                0,
                0));
    }

    static uint32_t ReadPublishedSequence(
        const volatile LONG& value)
    {
        const LONG observed =
            InterlockedCompareExchange(
                const_cast<volatile LONG*>(&value),
                0,
                0);
        return observed > 0
            ? static_cast<uint32_t>(observed)
            : 0;
    }

    static PacketCopy CopyPacket(
        uint16_t opcode,
        const void* packet,
        bool shop)
    {
        PacketCopy copy;
        if (!packet)
            return copy;
        if (shop) {
            if (opcode == 0x074D)
                copy.size = kShop074DSize;
            else if (opcode == 0x0574)
                copy.size = kShop0574Size;
            else if (opcode == 0x05D8 ||
                     opcode == 0x05DA ||
                     opcode == 0x09BD)
                copy.size = 8;
        } else {
            if (opcode == kReplyNickname)
                copy.size = kReplyNicknameSize;
            else if (opcode == kReplyActivation)
                copy.size = kReplyActivationSize;
            else if (opcode == kReplyPage) {
                __try {
                    const uint8_t count =
                        static_cast<
                            const uint8_t*>(
                            packet)[6];
                    if (count <= 6)
                        copy.size =
                            7u +
                            static_cast<uint32_t>(
                                count) *
                                0x50u;
                } __except (
                    EXCEPTION_EXECUTE_HANDLER) {
                    copy.size = 0;
                }
            }
        }
        if (!copy.size ||
            copy.size > copy.bytes.size())
            return copy;
        __try {
            memcpy(
                copy.bytes.data(),
                packet,
                copy.size);
            copy.valid = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            copy.valid = false;
        }
        return copy;
    }

    static int ObserveShop(
        size_t index,
        void* self,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        OperatorSessionActions* session = active_;
        if (!session || index >=
                std::size(session->shopDispatch_) ||
            !session->shopDispatch_[index])
            return 0;
        const uint16_t normalized =
            static_cast<uint16_t>(opcode);
        const PacketCopy copy =
            CopyPacket(normalized, packet, true);
        const int result =
            session->shopDispatch_[index](
                self, a, b, c, opcode, packet);
        const DWORD error = GetLastError();
        if (copy.valid && session->purchase_)
            session->purchase_->ObserveShopReply(
                normalized,
                copy.bytes.data(),
                copy.size,
                true);
        SetLastError(error);
        return result;
    }

    static int __fastcall ObserveShopDispatch1(
        void* self, void*, int a, int b, int c,
        int opcode, void* packet)
    {
        return ObserveShop(
            0, self, a, b, c, opcode, packet);
    }

    static int __fastcall ObserveShopDispatch2(
        void* self, void*, int a, int b, int c,
        int opcode, void* packet)
    {
        return ObserveShop(
            1, self, a, b, c, opcode, packet);
    }

    static int __fastcall ObserveShopDispatch3(
        void* self, void*, int a, int b, int c,
        int opcode, void* packet)
    {
        return ObserveShop(
            2, self, a, b, c, opcode, packet);
    }

    static int __fastcall ObserveInventoryDispatch(
        void* self, void*, int a, int b, int c,
        int opcode, void* packet)
    {
        OperatorSessionActions* session = active_;
        if (!session || !session->inventoryDispatch_)
            return 0;
        const uint16_t normalized =
            static_cast<uint16_t>(opcode);
        const PacketCopy copy =
            CopyPacket(normalized, packet, false);
        const int result =
            session->inventoryDispatch_(
                self, a, b, c, opcode, packet);
        const DWORD error = GetLastError();
        if (copy.valid)
            session->ObserveInventoryReply(
                normalized,
                copy.bytes.data(),
                copy.size);
        SetLastError(error);
        return result;
    }

    void ObserveInventoryReply(
        uint16_t opcode,
        const uint8_t* packet,
        uint32_t size)
    {
        if (!packet)
            return;
        if (opcode == kReplyPage) {
            if (inventory_)
                inventory_->ObservePageReply(
                    opcode,
                    packet,
                    size);
            return;
        }
        if (opcode == kReplyNickname &&
            size >= kReplyNicknameSize &&
            InterlockedCompareExchange(
                &namingActionArmed_, 0, 0) != 0) {
            uint32_t low = 0;
            uint32_t high = 0;
            memcpy(&low, packet + 6, sizeof(low));
            memcpy(&high, packet + 10, sizeof(high));
            if (InterlockedCompareExchange(
                    &namingReplySequence_,
                    -1,
                    0) == 0) {
                InterlockedExchange(
                    &namingReplyUidLow_,
                    static_cast<LONG>(low));
                InterlockedExchange(
                    &namingReplyUidHigh_,
                    static_cast<LONG>(high));
                InterlockedExchange(
                    &namingReplyOpcode_,
                    opcode);
                MemoryBarrier();
                InterlockedExchange(
                    &namingReplySequence_,
                    1);
            }
            return;
        }
        if (opcode == kReplyActivation &&
            size >= kReplyActivationSize &&
            InterlockedCompareExchange(
                &activationActionArmed_,
                0,
                0) != 0) {
            uint64_t uid = 0;
            uint32_t operatorId = 0;
            uint16_t slot = 0;
            uint8_t enabled = 0;
            memcpy(&uid, packet + 6, sizeof(uid));
            memcpy(
                &operatorId,
                packet + 0x0E,
                sizeof(operatorId));
            memcpy(
                &slot,
                packet + 0x16,
                sizeof(slot));
            // Original 08A0 dispatch clears every active flag and sets the
            // returned slot to active. The serialized old flag is not an ACK.
            enabled = 1;
            if (InterlockedCompareExchange(
                    &activationReplySequence_,
                    -1,
                    0) == 0) {
                InterlockedExchange(
                    &activationReplyUidLow_,
                    static_cast<LONG>(
                        static_cast<uint32_t>(uid)));
                InterlockedExchange(
                    &activationReplyUidHigh_,
                    static_cast<LONG>(
                        static_cast<uint32_t>(
                            uid >> 32)));
                InterlockedExchange(
                    &activationReplyOperatorId_,
                    static_cast<LONG>(operatorId));
                InterlockedExchange(
                    &activationReplySlot_,
                    static_cast<LONG>(slot));
                InterlockedExchange(
                    &activationReplyEnabled_,
                    static_cast<LONG>(enabled));
                InterlockedExchange(
                    &activationReplyOpcode_,
                    opcode);
                MemoryBarrier();
                InterlockedExchange(
                    &activationReplySequence_,
                    1);
            }
        }
    }

    Config config_ = {};
    NativeActions* native_ = nullptr;
    HangarNavigation* navigation_ = nullptr;
    ShopCatalogActions* shop_ = nullptr;
    OperatorFlowActions* purchase_ = nullptr;
    OperatorInventoryActions* inventory_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    CaptureGateFn captureGate_ = nullptr;
    void* captureContext_ = nullptr;
    bool optionsCapturePrimed_ = false;
    bool confirmationCapturePrimed_ = false;
    Observation observation_ = {};
    OperatorFlowActions::WalletImage baselineWallet_ = {};
    OperatorFlowActions::InventoryImage baselineInventory_;
    Stage stage_ = kStageIdle;
    bool bound_ = false;
    bool hooksInstalled_ = false;
    bool contextCaptured_ = false;
    bool namingReplyObserved_ = false;
    bool activationReplyObserved_ = false;
    bool recoveryVerified_ = false;
    uint64_t contextHash_ = 0;
    DWORD processId_ = 0;
    DWORD uiThread_ = 0;
    uint64_t lastElapsed_ = 0;
    uint64_t stageStartedAt_ = 0;
    uint64_t shopStartedAt_ = 0;
    uint64_t targetUid_ = 0;
    uint32_t namingRequestBaseline_ = 0;
    uint32_t activationRequestBaseline_ = 0;
    void* optionsPopup_ = nullptr;

    DispatchFn shopDispatch_[3] = {};
    DispatchFn inventoryDispatch_ = nullptr;

    volatile LONG namingActionArmed_ = 0;
    volatile LONG namingReplySequence_ = 0;
    volatile LONG namingReplyOpcode_ = 0;
    volatile LONG namingReplyUidLow_ = 0;
    volatile LONG namingReplyUidHigh_ = 0;
    volatile LONG activationActionArmed_ = 0;
    volatile LONG activationReplySequence_ = 0;
    volatile LONG activationReplyOpcode_ = 0;
    volatile LONG activationReplyUidLow_ = 0;
    volatile LONG activationReplyUidHigh_ = 0;
    volatile LONG activationReplyOperatorId_ = 0;
    volatile LONG activationReplySlot_ = 0;
    volatile LONG activationReplyEnabled_ = 0;

    inline static OperatorSessionActions* active_ = nullptr;
};
