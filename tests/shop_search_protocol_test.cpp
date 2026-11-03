#include <cstdio>
#include <string>

#include "sdgo/shop_search_protocol.h"

namespace {
bool Check(bool value, const char* message)
{
    if (!value)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}
} // namespace

int main()
{
    using namespace sdgo::shop_search;
    bool ok = true;

    const auto empty = PrepareQuery(u"");
    ok = Check(
             empty.disposition == QueryDisposition::PassThrough &&
                 empty.bytes.empty(),
             "empty query remains native") &&
        ok;

    const auto english = PrepareQuery(
        u"Strike Freedom Gundam with a deliberately long native keyword");
    ok = Check(
             english.disposition == QueryDisposition::PassThrough,
             "English query remains native regardless of marker limit") &&
        ok;

    const auto thai = PrepareQuery(u"\u0E01\u0E31\u0E19\u0E14\u0E31\u0E21");
    ok = Check(
             thai.disposition == QueryDisposition::PassThrough,
             "Thai query remains on the native CP874 path") &&
        ok;

    const auto chinese = PrepareQuery(u"\u9AD8\u8FBE");
    ok = Check(
             chinese.disposition == QueryDisposition::Encoded &&
                 chinese.bytes == "~CN:2Jq+jw==",
             "Chinese query uses UTF-16LE Base64 marker") &&
        ok;
    std::u16string decoded;
    ok = Check(
             DecodeChineseQuery(chinese.bytes, &decoded) &&
                 decoded == u"\u9AD8\u8FBE",
             "Chinese marker round-trips exactly") &&
        ok;

    const auto mixed = PrepareQuery(u"A\u9AD8\u8FBE");
    ok = Check(
             mixed.disposition == QueryDisposition::Encoded &&
                 mixed.bytes == "~CN:QQDYmr6P",
             "mixed English and Chinese preserves every UTF-16 unit") &&
        ok;

    const std::u16string maximum(36, u'\u673A');
    const auto maximumEncoded = PrepareQuery(maximum);
    ok = Check(
             maximumEncoded.disposition == QueryDisposition::Encoded &&
                 maximumEncoded.bytes.size() == 100 &&
                 maximumEncoded.bytes.size() <= kDatabaseSearchBytes,
             "36 UTF-16 units fit the varchar(100) contract") &&
        ok;
    ok = Check(
             DecodeChineseQuery(maximumEncoded.bytes, &decoded) &&
                 decoded == maximum,
             "maximum marker round-trips") &&
        ok;

    const std::u16string overlong(37, u'\u673A');
    ok = Check(
             PrepareQuery(overlong).disposition ==
                 QueryDisposition::TooLong,
             "37 Chinese UTF-16 units are rejected") &&
        ok;

    const std::u16string extensionB{0xD840, 0xDC00};
    const auto astral = PrepareQuery(extensionB);
    ok = Check(
             astral.disposition == QueryDisposition::Encoded &&
                 astral.bytes == "~CN:QNgA3A==",
             "astral Han is detected and counted as two UTF-16 units") &&
        ok;

    const std::u16string unpairedHigh{0xD840};
    const std::u16string unpairedLow{0xDC00};
    ok = Check(
             PrepareQuery(unpairedHigh).disposition ==
                     QueryDisposition::InvalidUtf16 &&
                 PrepareQuery(unpairedLow).disposition ==
                     QueryDisposition::InvalidUtf16,
             "unpaired surrogates are rejected") &&
        ok;

    ok = Check(
             !DecodeChineseQuery("~CN:not-base64", &decoded) &&
                 !DecodeChineseQuery("native", &decoded) &&
                 !DecodeChineseQuery("~CN:QQA=", &decoded),
             "decoder rejects malformed, unmarked, and non-Chinese payloads") &&
        ok;

    std::printf("SHOP SEARCH PROTOCOL %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
