#include <windows.h>
#include <cstdio>
#include <string>

#include "catalog_runtime.h"

namespace {
void Log(bool error, const std::wstring& text)
{
    char utf8[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, 4096, nullptr, nullptr);
    fprintf(error ? stderr : stdout, "%s\n", utf8);
}
}
int wmain(int argc, wchar_t** argv)
{
    std::wstring host, catalog, searchIndex, displayProfile, output;
    bool disable = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--disable") disable = true;
        else if (arg == L"--host-ini" && i + 1 < argc) host = argv[++i];
        else if (arg == L"--catalog" && i + 1 < argc) catalog = argv[++i];
        else if (arg == L"--search-index" && i + 1 < argc) searchIndex = argv[++i];
        else if (arg == L"--display-profile" && i + 1 < argc) displayProfile = argv[++i];
        else if (arg == L"--output" && i + 1 < argc) output = argv[++i];
        else { fprintf(stderr, "Invalid arguments\n"); return 2; }
    }
    const bool catalogMode = !catalog.empty();
    const bool searchMode = !searchIndex.empty();
    if (host.empty() || output.empty() || catalogMode == searchMode ||
        (searchMode && disable) || (catalogMode && !displayProfile.empty())) {
        fprintf(stderr, "Invalid arguments\n");
        return 2;
    }
    if (GetFileAttributesW(output.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "Output directory must already exist\n"); return 2;
    }
    if (searchMode)
        return sdgo::shop_catalog::ApplySearchIndex(
            host, searchIndex, displayProfile, output, Log) ? 0 : 1;
    return sdgo::shop_catalog::Apply(
        host, catalog, output, disable, Log) ? 0 : 1;
}
