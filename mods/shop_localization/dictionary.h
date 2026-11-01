#pragma once
#include <map>
#include <stdexcept>
#include <string>

namespace sdgo::shop_localization {
enum class Kind { Name, Description, Option };
struct Translation { Kind kind; std::wstring text; };
using Dictionary = std::map<std::wstring, Translation>;
constexpr size_t kMaxText = 8192;

inline std::wstring Unescape(const std::wstring& value)
{
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        wchar_t c = value[i];
        if (c == L'\\') {
            if (++i == value.size()) throw std::runtime_error("incomplete translation escape");
            c = value[i];
            if (c == L'n') c = L'\n';
            else if (c == L'r') c = L'\r';
            else if (c == L't') c = L'\t';
            else if (c == L's') c = L' ';
            else if (c != L'\\') throw std::runtime_error("unknown translation escape");
        }
        if (!c) throw std::runtime_error("NUL in translation");
        out += c;
    }
    if (out.empty() || out.size() > kMaxText) throw std::runtime_error("translation length outside supported range");
    return out;
}
inline Dictionary Parse(const std::wstring& input)
{
    Dictionary result;
    std::wstring section, source, text, kind;
    auto commit = [&]() {
        if (section.empty()) return;
        if (source.empty() || text.empty() || (kind != L"name" && kind != L"description" && kind != L"option"))
            throw std::runtime_error("incomplete shop translation");
        auto key = Unescape(source), value = Unescape(text);
        // Amounts and currency codes are business values, never dictionary keys.
        if (key == L"GB" || key == L"MB" || key.find_first_not_of(L"0123456789., +-\t") == std::wstring::npos)
            throw std::runtime_error("shop amounts/currency cannot be translation keys");
        Kind type = kind == L"name" ? Kind::Name : kind == L"description" ? Kind::Description : Kind::Option;
        if (!result.emplace(key, Translation{type, value}).second) throw std::runtime_error("duplicate shop translation source");
        if (result.size() > 20000) throw std::runtime_error("too many shop translations");
    };
    size_t pos = 0;
    while (pos < input.size()) {
        size_t end = input.find(L'\n', pos); if (end == std::wstring::npos) end = input.size();
        std::wstring line = input.substr(pos, end - pos); pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.size() > kMaxText * 2 + 8) throw std::runtime_error("translation line too long");
        if (line.front() == L'[' && line.back() == L']') {
            commit(); source.clear(); text.clear(); kind.clear(); section = line.substr(1, line.size() - 2);
            if (section.rfind(L"entry:", 0) != 0 || section.size() > 80) throw std::runtime_error("unsupported translation section");
        } else {
            size_t at = line.find(L'='); if (at == std::wstring::npos || section.empty()) throw std::runtime_error("invalid translation entry");
            auto key = line.substr(0, at), value = line.substr(at + 1);
            std::wstring* slot = key == L"Source" ? &source : key == L"Text" ? &text : key == L"Kind" ? &kind : nullptr;
            if (!slot || !slot->empty()) throw std::runtime_error("duplicate or unknown translation field");
            *slot = value;
        }
    }
    commit(); if (result.empty()) throw std::runtime_error("empty shop translation dictionary");
    return result;
}
inline std::wstring OptionLabel(const std::wstring& source)
{
    if (source == L"ถาวร" || source == L"영구" || source == L"Permanent") return L"永久";
    size_t end = source.find_first_not_of(L"0123456789");
    if (!end || end == std::wstring::npos || end > 7) return {};
    const auto digits = source.substr(0, end);
    const auto count = std::stoul(digits);
    if (!count || count > 1000000) return {};
    while (end < source.size() && source[end] == L' ') ++end;
    const auto suffix = source.substr(end);
    if (suffix == L"วัน" || suffix == L"일" || suffix == L"Day" || suffix == L"Days") return digits + L" 天";
    if (suffix == L"ชิ้น" || suffix == L"개" || suffix == L"EA") return digits + L" 件";
    if (suffix == L"ครั้ง") return digits + L" 次";
    return {};
}
} // namespace sdgo::shop_localization
