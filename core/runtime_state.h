#pragma once
#include <string>
#include <vector>

namespace sdgo::runtime {
struct ModState {
    std::string id, version, configuration;
    std::vector<std::string> files;
    bool requested = false, active = false;
};
std::vector<std::string> ConfigurationFiles(const std::wstring& manifest);
std::string ConfigurationRevision(const std::wstring& directory, const std::vector<std::string>& files) noexcept;
void Write(const std::wstring& root, const std::wstring& executable, const std::string& host,
           const std::vector<ModState>& mods) noexcept;
}
