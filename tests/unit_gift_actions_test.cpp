#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

#include "../mods/game_test_client/unit_gift_actions.h"

namespace {

constexpr char kRunId[] = "12345678-1234-1234-1234-123456789abc";

SdgoGameTestContextV1 Context(const char* runId = kRunId) {
    SdgoGameTestContextV1 result{};
    result.size = sizeof(result);
    result.version = SDGO_GAME_TEST_VERSION;
    result.run_id = runId;
    result.role = "client";
    return result;
}

SdgoFriendUnitGiftClientSnapshotV1 Snapshot(
    const SdgoGameTestContextV1& context) {
    SdgoFriendUnitGiftClientSnapshotV1 result{};
    result.size = sizeof(result);
    result.version = SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION;
    result.run_hash = friends::UnitGiftActions::RunHash(&context);
    result.process_id = GetCurrentProcessId();
    result.ready = 1;
    return result;
}

std::string WrittenSnapshot(
    friends::UnitGiftActions& actions,
    const SdgoGameTestContextV1* context) {
    FILE* file = std::tmpfile();
    if (!file) return {};
    actions.Write(file, context);
    std::fflush(file);
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return {};
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size && std::fread(result.data(), 1, result.size(), file) != result.size())
        result.clear();
    std::fclose(file);
    return result;
}

bool AllZero(const std::array<std::uint16_t, 200>& values) {
    for (const auto value : values)
        if (value != 0) return false;
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: unit_gift_actions_test.exe "
                     "<observer fixture DLL> <missing-export fixture DLL>\n";
        return 2;
    }

    int checks = 0;
    int failures = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
        if (!value) ++failures;
    };

    const auto context = Context();
    const auto expectedHash = friends::UnitGiftActions::RunHash(&context);
    friends::UnitGiftActions actions;

    check(GetModuleHandleW(L"friend_unit_gift_client.dll") == nullptr,
          "product-named module is absent before the fixture loads");
    check(!actions.Capture(&context),
          "capture rejects an absent observer module");
    const auto absentIni = WrittenSnapshot(actions, &context);
    check(absentIni.find("available=0\nvalid=0\ncapture_error=2\n") !=
              std::string::npos,
          "capture reports the stable module-missing diagnostic");
    const std::uint16_t absentNote[] = {0x4F60, 0x597D};
    check(actions.Invoke(
              &context, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE,
              7, UINT64_C(0x123456789ABCDEF0),
              UINT64_C(0xFEDCBA9876543210), absentNote, 2) ==
              friends::FriendActions::kUnsupported,
          "invoke performs no action when the observer module is absent");

    HMODULE missingExport = LoadLibraryW(argv[2]);
    check(missingExport != nullptr,
          "isolated missing-export fake observer DLL loads");
    if (!missingExport) return 1;
    check(GetModuleHandleW(L"friend_unit_gift_client.dll") == missingExport,
          "missing-export fixture has the exact product basename");
    const auto missingExportIni = WrittenSnapshot(actions, &context);
    check(missingExportIni.find(
              "available=0\nvalid=0\ncapture_error=3\n") !=
              std::string::npos,
          "capture reports the stable export-missing diagnostic");
    FreeLibrary(missingExport);

    HMODULE fixture = LoadLibraryW(argv[1]);
    check(fixture != nullptr, "isolated fake observer DLL loads");
    if (!fixture) return 1;
    check(GetModuleHandleW(L"friend_unit_gift_client.dll") == fixture,
          "fixture has the exact product basename used by the gateway");

    using ResetFn = void(__cdecl*)();
    using SetSnapshotFn =
        void(__cdecl*)(const SdgoFriendUnitGiftClientSnapshotV1*);
    using SetResultFn = void(__cdecl*)(int);
    using CallsFn = std::uint32_t(__cdecl*)();
    using CopyRequestFn =
        int(__cdecl*)(SdgoFriendUnitGiftClientActionRequestV1*);
    const auto reset = reinterpret_cast<ResetFn>(
        GetProcAddress(fixture, "TestUnitGiftReset"));
    const auto setSnapshot = reinterpret_cast<SetSnapshotFn>(
        GetProcAddress(fixture, "TestUnitGiftSetSnapshot"));
    const auto setSnapshotResult = reinterpret_cast<SetResultFn>(
        GetProcAddress(fixture, "TestUnitGiftSetSnapshotResult"));
    const auto setActionResult = reinterpret_cast<SetResultFn>(
        GetProcAddress(fixture, "TestUnitGiftSetActionResult"));
    const auto snapshotCalls = reinterpret_cast<CallsFn>(
        GetProcAddress(fixture, "TestUnitGiftSnapshotCalls"));
    const auto actionCalls = reinterpret_cast<CallsFn>(
        GetProcAddress(fixture, "TestUnitGiftActionCalls"));
    const auto copyRequest = reinterpret_cast<CopyRequestFn>(
        GetProcAddress(fixture, "TestUnitGiftCopyLastRequest"));
    check(reset && setSnapshot && setSnapshotResult && setActionResult &&
              snapshotCalls && actionCalls && copyRequest,
          "all fake observer controls resolve");
    if (!reset || !setSnapshot || !setSnapshotResult || !setActionResult ||
        !snapshotCalls || !actionCalls || !copyRequest) {
        FreeLibrary(fixture);
        return 1;
    }

    auto baseline = Snapshot(context);
    reset();
    setSnapshot(&baseline);
    check(actions.Capture(&context), "canonical scoped snapshot is accepted");
    const auto validIni = WrittenSnapshot(actions, &context);
    check(validIni.find("available=1\nvalid=1\ncapture_error=0\n") !=
              std::string::npos,
          "capture reports no error only for a valid snapshot");

    auto invalid = baseline;
    invalid.run_hash ^= UINT64_C(0x100);
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=6\n") != std::string::npos,
          "foreign run hash is rejected with scope diagnostic");

    invalid = baseline;
    ++invalid.process_id;
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=6\n") != std::string::npos,
          "foreign process ID is rejected with scope diagnostic");

    invalid = baseline;
    ++invalid.version;
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=5\n") != std::string::npos,
          "foreign observer version is rejected with ABI diagnostic");

    invalid = baseline;
    --invalid.size;
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=5\n") != std::string::npos,
          "truncated observer snapshot is rejected with ABI diagnostic");

    invalid = baseline;
    invalid.row_count = 21;
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=7\n") != std::string::npos,
          "oversized row array is rejected with bounds diagnostic");

    invalid = baseline;
    invalid.row_count = 1;
    invalid.rows[0].peer_name_code_units = 21;
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=8\n") != std::string::npos,
          "oversized peer name is rejected with bounds diagnostic");

    invalid = baseline;
    invalid.provenance_text_code_units = 513;
    setSnapshot(&invalid);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=9\n") != std::string::npos,
          "oversized provenance text is rejected with bounds diagnostic");

    invalid = baseline;
    invalid.provenance_text_code_units = 320;
    std::fill(std::begin(invalid.provenance_text),std::begin(invalid.provenance_text)+320,std::uint16_t{0x613F});
    setSnapshot(&invalid);
    check(actions.Capture(&context),"full note plus sender and timestamps fits the declared 512-unit native observation");

    setSnapshot(&baseline);
    setSnapshotResult(0);
    check(!actions.Capture(&context) &&
              WrittenSnapshot(actions, &context).find(
                  "valid=0\ncapture_error=4\n") != std::string::npos,
          "observer query failure is rejected with query diagnostic");
    setSnapshotResult(1);

    const auto queryCallsBeforeBadScope = snapshotCalls();
    check(!actions.Capture(nullptr) &&
              snapshotCalls() == queryCallsBeforeBadScope &&
              WrittenSnapshot(actions, nullptr).find(
                  "valid=0\ncapture_error=1\n") != std::string::npos &&
              snapshotCalls() == queryCallsBeforeBadScope,
          "null context rejects without querying and reports unscoped");
    auto missingRun = Context(nullptr);
    check(!actions.Capture(&missingRun) &&
              snapshotCalls() == queryCallsBeforeBadScope &&
              WrittenSnapshot(actions, &missingRun).find(
                  "valid=0\ncapture_error=1\n") != std::string::npos &&
              snapshotCalls() == queryCallsBeforeBadScope,
          "missing run ID rejects without querying and reports unscoped");
    auto emptyRun = Context("");
    check(!actions.Capture(&emptyRun) &&
              snapshotCalls() == queryCallsBeforeBadScope,
          "empty run ID rejects without querying the observer");
    auto zeroRun = Context("00000000-0000-0000-0000-000000000000");
    check(friends::UnitGiftActions::RunHash(&zeroRun) == 0 &&
              !actions.Capture(&zeroRun) &&
              snapshotCalls() == queryCallsBeforeBadScope,
          "zero canonical run ID cannot create an unscoped snapshot");
    auto malformedRun = Context("1234");
    check(friends::UnitGiftActions::RunHash(&malformedRun) == 0 &&
              !actions.Capture(&malformedRun) &&
              snapshotCalls() == queryCallsBeforeBadScope,
          "malformed run ID cannot create an unscoped snapshot");

    const auto actionCallsBeforeBadScope = actionCalls();
    check(actions.Invoke(
              nullptr, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE,
              0, 0, 0, nullptr, 0) ==
              friends::FriendActions::kInvalidCommand &&
              actionCalls() == actionCallsBeforeBadScope,
          "null context invokes no action");
    check(actions.Invoke(
              &emptyRun, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE,
              0, 0, 0, nullptr, 0) ==
              friends::FriendActions::kInvalidCommand &&
              actionCalls() == actionCallsBeforeBadScope,
          "empty run ID invokes no action");

    const auto actionCallsBeforeBadBounds = actionCalls();
    std::array<std::uint16_t, 201> tooLong{};
    check(actions.Invoke(&context, 0, 0, 0, 0, nullptr, 0) ==
              friends::FriendActions::kInvalidCommand &&
              actions.Invoke(&context, 17, 0, 0, 0, nullptr, 0) ==
              friends::FriendActions::kInvalidCommand &&
              actions.Invoke(
                  &context, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE,
                  0, 0, 0, tooLong.data(), 201) ==
              friends::FriendActions::kInvalidCommand &&
              actions.Invoke(
                  &context, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE,
                  0, 0, 0, nullptr, 1) ==
              friends::FriendActions::kInvalidCommand &&
              actionCalls() == actionCallsBeforeBadBounds,
          "bad action and note bounds invoke no fixture action");

    reset();
    setSnapshot(&baseline);
    const std::uint16_t note[] = {0x4F60, 0x597D, 0xD83D, 0xDE00};
    const std::uint64_t unitUid = UINT64_C(0xFEDCBA9876543210);
    const std::uint64_t giftId = UINT64_C(0x8123456789ABCDEF);
    check(actions.Invoke(
              &context, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CONFIRM,
              UINT32_C(0xF1234567), unitUid, giftId, note, 4) ==
              friends::FriendActions::kReady,
          "fixture accepts a valid scoped action");
    SdgoFriendUnitGiftClientActionRequestV1 request{};
    check(actionCalls() == 1 && copyRequest(&request) &&
              request.size == sizeof(request) &&
              request.version ==
                  SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION &&
              request.run_hash == expectedHash &&
              request.action == SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CONFIRM &&
              request.receiver_id == UINT32_C(0xF1234567) &&
              request.unit_uid == unitUid && request.gift_id == giftId &&
              request.note_code_units == 4 &&
              std::memcmp(request.note, note, sizeof(note)) == 0,
          "invoke preserves run scope, uint64 IDs, and UTF-16 note exactly");

    setActionResult(0);
    check(actions.Invoke(
              &context, SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CANCEL,
              0, 0, 0, nullptr, 0) ==
              friends::FriendActions::kReplyRejected,
          "fixture action rejection is preserved");

    std::array<std::uint16_t, 200> decoded{};
    std::uint16_t decodedLength = 0;
    std::wstring exactBoundary;
    exactBoundary.reserve(800);
    for (std::size_t index = 0; index < 200; ++index)
        exactBoundary += L"0041";
    check(friends::UnitGiftActions::DecodeHexNote(
              exactBoundary.c_str(), decoded.data(), &decodedLength) &&
              decodedLength == 200 && decoded.front() == 0x41 &&
              decoded.back() == 0x41,
          "DecodeHexNote accepts the exact 200-code-unit boundary");

    std::wstring overBoundary = exactBoundary + L"0041";
    decoded.fill(0xBEEF);
    decodedLength = 99;
    check(!friends::UnitGiftActions::DecodeHexNote(
              overBoundary.c_str(), decoded.data(), &decodedLength) &&
              decodedLength == 0 && AllZero(decoded),
          "DecodeHexNote rejects over 200 units and clears output");

    decoded.fill(0xBEEF);
    decodedLength = 99;
    check(!friends::UnitGiftActions::DecodeHexNote(
              L"4F600000597D", decoded.data(), &decodedLength) &&
              decodedLength == 0 && AllZero(decoded),
          "DecodeHexNote rejects encoded NUL and clears output");

    decoded.fill(0xBEEF);
    decodedLength = 99;
    check(!friends::UnitGiftActions::DecodeHexNote(
              L"4F6G", decoded.data(), &decodedLength) &&
              decodedLength == 0 && AllZero(decoded),
          "DecodeHexNote rejects malformed hex and clears output");

    decoded.fill(0xBEEF);
    decodedLength = 99;
    check(!friends::UnitGiftActions::DecodeHexNote(
              L"4F60\r\n597D", decoded.data(), &decodedLength) &&
              decodedLength == 0 && AllZero(decoded),
          "DecodeHexNote rejects CRLF injection and clears output");

    check(friends::UnitGiftActions::DecodeHexNote(
              L"4F60597DD83DDE00", decoded.data(), &decodedLength) &&
              decodedLength == 4 && decoded[0] == 0x4F60 &&
              decoded[1] == 0x597D && decoded[2] == 0xD83D &&
              decoded[3] == 0xDE00,
          "DecodeHexNote preserves Chinese and a surrogate pair");

    for (const wchar_t* unpaired :
         {L"D83D", L"DE00", L"D83D0041", L"0041DE00"}) {
        decoded.fill(0xBEEF);
        decodedLength = 99;
        check(!friends::UnitGiftActions::DecodeHexNote(
                  unpaired, decoded.data(), &decodedLength) &&
                  decodedLength == 0 && AllZero(decoded),
              "DecodeHexNote rejects an unpaired UTF-16 surrogate");
    }

    auto written = baseline;
    written.server_epoch = UINT64_C(18446744073709551615);
    written.last_reply_gift_id = UINT64_C(18446744073709551614);
    written.last_reply_unit_uid = UINT64_C(9223372036854775809);
    written.selected_unit_uid = unitUid;
    written.selected_gift_id = giftId;
    written.row_count = 1;
    written.rows[0].gift_id = UINT64_C(18446744073709551613);
    written.rows[0].unit_uid = UINT64_C(1311768467463790320);
    written.rows[0].offered_unix_millis =
        UINT64_C(18446744073709551612);
    constexpr std::uint16_t injected[] = {
        0x000D, 0x000A, 0x005B, 0x0069, 0x006E, 0x006A, 0x0065,
        0x0063, 0x0074, 0x0065, 0x0064, 0x005D, 0x000D, 0x000A};
    written.rows[0].peer_name_code_units =
        static_cast<std::uint16_t>(std::size(injected));
    std::memcpy(written.rows[0].peer_name, injected, sizeof(injected));
    written.provenance_present = 1;
    written.provenance_text_code_units =
        static_cast<std::uint16_t>(std::size(injected));
    std::memcpy(
        written.provenance_text, injected, sizeof(injected));
    setSnapshot(&written);
    const auto ini = WrittenSnapshot(actions, &context);
    check(ini.find("\r\n[injected]\r\n") == std::string::npos &&
              ini.find("000D000A005B0069006E006A00650063007400650064005D000D000A") !=
                  std::string::npos,
          "Write hex-encodes free text so it cannot inject an INI section");
    check(ini.find("server_epoch=18446744073709551615") !=
                  std::string::npos &&
              ini.find("last_reply_gift_id=18446744073709551614") !=
                  std::string::npos &&
              ini.find("last_reply_unit_uid=9223372036854775809") !=
                  std::string::npos &&
              ini.find("selected_unit_uid=18364758544493064720") !=
                  std::string::npos &&
              ini.find("selected_gift_id=9305357566071262703") !=
                  std::string::npos &&
              ini.find("gift_id=18446744073709551613") !=
                  std::string::npos &&
              ini.find("unit_uid=1311768467463790320") !=
                  std::string::npos &&
              ini.find("offered_unix_millis=18446744073709551612") !=
                  std::string::npos,
          "Write preserves full unsigned 64-bit decimal precision");

    FreeLibrary(fixture);
    std::cout << "UNIT GIFT ACTION GATEWAY CHECKS "
              << (failures ? "FAILED " : "PASSED ")
              << (checks - failures) << '/' << checks << '\n';
    return failures ? 1 : 0;
}
