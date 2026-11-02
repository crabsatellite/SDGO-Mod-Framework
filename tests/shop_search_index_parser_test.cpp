#include <cstdio>
#include <stdexcept>
#include <string>

#include "../mods/shop_catalog/catalog_runtime.h"

namespace {
bool Check(bool value, const char* message)
{
    if (!value)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}
bool Rejects(const std::wstring& value)
{
    try {
        sdgo::shop_catalog::ParseSearchAliases(value);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}
} // namespace

int main()
{
    bool ok = true;
    const auto aliases = sdgo::shop_catalog::ParseSearchAliases(
        L"[entry:a]\nKind=name\n"
        L"Source=@product:1019001:Gundam\nText=高达\n"
        L"[entry:b]\nKind=name\n"
        L"Source=@product:1019001:GUNDAM\nText=高达\n"
        L"[entry:c]\nKind=name\n"
        L"Source=Gundam\nText=高达\n"
        L"[entry:d]\nKind=description\n"
        L"Source=@product:1019002:Description\nText=说明\n");
    ok = Check(
             aliases.size() == 2 && aliases[0].product == 1019001 &&
                 aliases[0].original == L"GUNDAM" &&
                 aliases[1].original == L"Gundam",
             "only product-scoped name entries become search aliases") &&
        ok;
    ok = Check(
             Rejects(
                 L"[entry:a]\nKind=name\n"
                 L"Source=@product:bad:Gundam\nText=高达\n"),
             "non-numeric product ids are rejected") &&
        ok;
    ok = Check(
             sdgo::shop_catalog::ParseSearchAliases(
                 L"[entry:a]\nKind=name\n"
                 L"Source=@product:1019001:Gundam\nText=Gundam\n"
                 L"[entry:b]\nKind=name\n"
                 L"Source=@search:1019002:FAZZ\nText=FAZZ\n"
                 L"[entry:c]\nKind=name\n"
                 L"Source=@product:1019003:Zaku\nText=扎古\n").size() == 3,
             "displayed model codes and exact billing aliases participate alongside Chinese names") &&
        ok;
    ok = Check(
             Rejects(
                 L"[entry:a]\nKind=name\n"
                 L"Source=@product:1019001:Gundam\nText=" +
                 std::wstring(513, L'机') + L"\n"),
             "aliases over 512 UTF-16 units are rejected") &&
        ok;
    std::printf("SHOP SEARCH INDEX PARSER %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
