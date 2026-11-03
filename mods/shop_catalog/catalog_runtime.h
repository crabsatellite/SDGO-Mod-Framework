#pragma once
#include <string>
#include <vector>

#include "search_sql.h"

namespace sdgo::shop_catalog {
using LogFn = void(*)(bool error, const std::wstring& message);
bool Apply(const std::wstring& hostIni, const std::wstring& catalogIni,
           const std::wstring& outputDirectory, bool disable, LogFn log);
std::vector<sdgo::shop_search::Alias> ParseSearchAliases(
    const std::wstring& dictionaryText);
bool ApplySearchIndex(
    const std::wstring& hostIni, const std::wstring& dictionary,
    const std::wstring& displayProfile, const std::wstring& outputDirectory,
    LogFn log);
}
