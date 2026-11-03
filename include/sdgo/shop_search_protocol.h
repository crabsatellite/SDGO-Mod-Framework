#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace sdgo::shop_search {

inline constexpr std::string_view kChineseMarker = "~CN:";
inline constexpr std::size_t kDatabaseSearchBytes = 100;
inline constexpr std::size_t kMaxChineseUtf16Units = 36;

enum class QueryDisposition {
    PassThrough,
    Encoded,
    TooLong,
    InvalidUtf16
};

struct PreparedQuery {
    QueryDisposition disposition = QueryDisposition::PassThrough;
    std::string bytes;
};

inline bool IsHighSurrogate(char16_t value)
{
    return value >= 0xD800 && value <= 0xDBFF;
}

inline bool IsLowSurrogate(char16_t value)
{
    return value >= 0xDC00 && value <= 0xDFFF;
}

inline bool IsHanCodePoint(std::uint32_t value)
{
    return (value >= 0x3400 && value <= 0x4DBF) ||
           (value >= 0x4E00 && value <= 0x9FFF) ||
           (value >= 0xF900 && value <= 0xFAFF) ||
           (value >= 0x20000 && value <= 0x2FA1F) ||
           (value >= 0x30000 && value <= 0x323AF);
}

inline bool InspectUtf16(std::u16string_view text, bool* containsHan)
{
    bool han = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto first = text[index];
        std::uint32_t codePoint = first;
        if (IsHighSurrogate(first)) {
            if (++index >= text.size() || !IsLowSurrogate(text[index]))
                return false;
            codePoint = 0x10000u +
                ((static_cast<std::uint32_t>(first) - 0xD800u) << 10) +
                (static_cast<std::uint32_t>(text[index]) - 0xDC00u);
        } else if (IsLowSurrogate(first)) {
            return false;
        }
        han = han || IsHanCodePoint(codePoint);
    }
    if (containsHan)
        *containsHan = han;
    return true;
}

inline std::string Base64Encode(std::string_view bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = bytes.size() - index;
        const auto first = static_cast<std::uint8_t>(bytes[index]);
        const auto second = remaining > 1
            ? static_cast<std::uint8_t>(bytes[index + 1]) : std::uint8_t{};
        const auto third = remaining > 2
            ? static_cast<std::uint8_t>(bytes[index + 2]) : std::uint8_t{};
        const std::uint32_t value =
            (static_cast<std::uint32_t>(first) << 16) |
            (static_cast<std::uint32_t>(second) << 8) |
            static_cast<std::uint32_t>(third);
        result.push_back(alphabet[(value >> 18) & 0x3F]);
        result.push_back(alphabet[(value >> 12) & 0x3F]);
        result.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3F] : '=');
        result.push_back(remaining > 2 ? alphabet[value & 0x3F] : '=');
    }
    return result;
}

inline PreparedQuery PrepareQuery(std::u16string_view text)
{
    bool containsHan = false;
    if (!InspectUtf16(text, &containsHan))
        return {QueryDisposition::InvalidUtf16, {}};
    if (!containsHan)
        return {};
    if (text.size() > kMaxChineseUtf16Units)
        return {QueryDisposition::TooLong, {}};

    std::string raw;
    raw.reserve(text.size() * 2);
    for (const auto value : text) {
        raw.push_back(static_cast<char>(value & 0xFF));
        raw.push_back(static_cast<char>((value >> 8) & 0xFF));
    }
    PreparedQuery result;
    result.disposition = QueryDisposition::Encoded;
    result.bytes.assign(kChineseMarker.data(), kChineseMarker.size());
    result.bytes += Base64Encode(raw);
    if (result.bytes.size() > kDatabaseSearchBytes)
        return {QueryDisposition::TooLong, {}};
    return result;
}

inline int Base64Value(char value)
{
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

inline bool DecodeChineseQuery(std::string_view value, std::u16string* output)
{
    if (!output || value.size() <= kChineseMarker.size() ||
        value.size() > kDatabaseSearchBytes ||
        value.substr(0, kChineseMarker.size()) != kChineseMarker)
        return false;
    const auto payload = value.substr(kChineseMarker.size());
    if (payload.size() % 4 != 0)
        return false;

    std::string raw;
    raw.reserve(payload.size() / 4 * 3);
    for (std::size_t index = 0; index < payload.size(); index += 4) {
        const bool last = index + 4 == payload.size();
        const int first = Base64Value(payload[index]);
        const int second = Base64Value(payload[index + 1]);
        const bool thirdPadding = payload[index + 2] == '=';
        const bool fourthPadding = payload[index + 3] == '=';
        const int third = thirdPadding ? 0 : Base64Value(payload[index + 2]);
        const int fourth = fourthPadding ? 0 : Base64Value(payload[index + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (thirdPadding && !fourthPadding) ||
            ((thirdPadding || fourthPadding) && !last))
            return false;
        const std::uint32_t decoded =
            (static_cast<std::uint32_t>(first) << 18) |
            (static_cast<std::uint32_t>(second) << 12) |
            (static_cast<std::uint32_t>(third) << 6) |
            static_cast<std::uint32_t>(fourth);
        raw.push_back(static_cast<char>((decoded >> 16) & 0xFF));
        if (!thirdPadding)
            raw.push_back(static_cast<char>((decoded >> 8) & 0xFF));
        if (!fourthPadding)
            raw.push_back(static_cast<char>(decoded & 0xFF));
    }
    if (raw.empty() || raw.size() % 2 != 0 ||
        raw.size() / 2 > kMaxChineseUtf16Units)
        return false;

    std::u16string decoded;
    decoded.reserve(raw.size() / 2);
    for (std::size_t index = 0; index < raw.size(); index += 2) {
        const auto low = static_cast<std::uint8_t>(raw[index]);
        const auto high = static_cast<std::uint8_t>(raw[index + 1]);
        decoded.push_back(static_cast<char16_t>(low | (high << 8)));
    }
    bool containsHan = false;
    if (!InspectUtf16(decoded, &containsHan) || !containsHan)
        return false;
    *output = std::move(decoded);
    return true;
}

static_assert(
    kChineseMarker.size() +
        ((kMaxChineseUtf16Units * 2 + 2) / 3) * 4 <=
    kDatabaseSearchBytes);

} // namespace sdgo::shop_search
