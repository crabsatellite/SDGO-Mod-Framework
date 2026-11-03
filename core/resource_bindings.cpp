#include "sdgo/resource_bindings.h"
#include "sdgo/script_registration.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace sdgo::resources {
namespace {
constexpr std::size_t kRecordBytes = 1871;
bool Owner(const std::string& value) {
    if (value.empty() || value.size() > 48) return false;
    for (std::size_t i = 0; i < value.size(); ++i) if (!((value[i] >= 'a' && value[i] <= 'z') || value[i] == '_' || (i && value[i] >= '0' && value[i] <= '9'))) return false;
    return true;
}
bool Hash(const std::string& value) { return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool Path(const std::string& path, const std::string& owner) {
    const auto slash = path.find('/'); if (slash == std::string::npos || path.size() > 240 || path.find('/', slash + 1) != std::string::npos || path.find("..") != std::string::npos) return false;
    static const std::set<std::string> roots = {"gdrs", "mdrs", "anrs", "txrs", "fxrs", "sprs", "sdrs", "scp"};
    if (!roots.count(path.substr(0, slash)) || path.compare(slash + 1, owner.size() + 1, owner + "_") != 0) return false;
    return path.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_./-") == std::string::npos;
}
std::uint32_t Id(const std::uint8_t* bytes) { std::uint32_t id = 0; std::memcpy(&id, bytes, 4); return id; }
bool FixedBranch(const std::vector<std::uint8_t>& record) { return record[348] == 0 && record[349] == 0 && record[1769] != 1 && record[1769] != 2; }
bool OriginalPath(const wchar_t* value) {
    if (!value) return false;
    const wchar_t* expected = L"fxrs/0121.zfx";
    for (std::size_t i = 0; ; ++i) { auto c = value[i]; if (c == L'\\') c = L'/'; if (c >= L'A' && c <= L'Z') c += L'a' - L'A'; if (c != expected[i]) return false; if (!c) return true; }
}
}

std::shared_ptr<const Bindings> Bindings::Build(const std::vector<Candidate>& candidates, const Authority& authority, std::string& error) {
    error.clear(); auto result = std::shared_ptr<Bindings>(new Bindings);
    try {
        const auto revision = authority.revision(); if (revision.empty()) throw std::runtime_error("allocation/publication snapshot is unavailable");
        std::set<std::string> owners, resourcePaths;
        for (const auto& candidate : candidates) {
            if (!Owner(candidate.owner) || candidate.revision.empty() || candidate.revision.size() > 128 || !owners.insert(candidate.owner).second || candidate.weapons.empty()) throw std::runtime_error("owner candidate identity or records invalid");
            std::map<std::string, std::string> resources;
            for (const auto& resource : candidate.resources) {
                if (!Path(resource.path, candidate.owner) || !Hash(resource.sha256) || !resourcePaths.insert(resource.path).second || !authority.publishedResource(resource.path, resource.sha256)) throw std::runtime_error("owned resource is not authenticated in the native publication snapshot");
                resources.emplace(resource.path, resource.sha256);
            }
            std::map<std::uint32_t, std::string> fixed;
            for (const auto& binding : candidate.fixedEffects) {
                if (!binding.weapon || !resources.count(binding.target) || binding.target.compare(0, 5, "fxrs/") != 0 || binding.target.size() < 4 || binding.target.substr(binding.target.size() - 4) != ".zfx" || !fixed.emplace(binding.weapon, binding.target).second) throw std::runtime_error("fixed-effect resource declaration invalid");
            }
            for (const auto& weapon : candidate.weapons) {
                if (!weapon.id || weapon.record.size() != kRecordBytes || Id(weapon.record.data()) != weapon.id || scripts::Sha256(weapon.record) != weapon.sha256) throw std::runtime_error("target weapon record identity/hash invalid");
                if (authority.originalWeapon(weapon.id) || !authority.allocatedWeapon(candidate.owner, weapon.id, weapon.sha256)) throw std::runtime_error("target weapon is not uniquely allocated to this owner");
                auto target = fixed.find(weapon.id); const bool branch = FixedBranch(weapon.record);
                if (branch != (target != fixed.end())) throw std::runtime_error("native fallback branch and fixed-effect declaration differ");
                Entry entry; entry.record = weapon.record;
                if (target != fixed.end()) { entry.target.assign(target->second.begin(), target->second.end()); ++result->fixedCount_; fixed.erase(target); }
                if (!result->weapons_.emplace(weapon.id, std::move(entry)).second) throw std::runtime_error("target weapon ID is shared across owner records");
            }
            if (!fixed.empty()) throw std::runtime_error("fixed-effect declaration has no owned record");
        }
        if (authority.revision() != revision) throw std::runtime_error("allocation/publication snapshot changed while preparing bindings");
        return result;
    } catch (const std::exception& e) { error = e.what(); return {}; }
}

bool Bindings::owns(std::uint32_t id) const { return weapons_.count(id) != 0; }
Result Bindings::resolve(std::uint32_t entryWeapon, const std::uint8_t* record, std::size_t bytes, const wchar_t* original) const {
    const auto found = weapons_.find(entryWeapon); if (found == weapons_.end()) return {Resolution::Original, original, ""};
    if (!record || bytes != kRecordBytes || Id(record) != entryWeapon || std::memcmp(record, found->second.record.data(), kRecordBytes) != 0) return {Resolution::Fatal, nullptr, "owned native weapon record no longer matches the allocated snapshot"};
    if (found->second.target.empty()) return {Resolution::Fatal, nullptr, "native fixed-effect call reached an incompatible owned constructor branch"};
    if (!OriginalPath(original)) return {Resolution::Fatal, nullptr, "owned native fixed-effect argument differs from the verified source path"};
    return {Resolution::Redirected, found->second.target.c_str(), ""};
}
} // namespace sdgo::resources
