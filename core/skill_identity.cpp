#include "sdgo/skill_identity.h"
#include <set>

namespace sdgo::skill_identity {
namespace {
bool Hash(const std::string& value) {
    if (value.size() != 64) return false;
    for (const char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool Owner(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    for (std::size_t i=0;i<value.size();++i) {
        const char c=value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              (i && (c == '_' || c == '-' || c == '.')))) return false;
    }
    return true;
}
bool Supported(std::uint32_t id) {
    // Audited enable/disable selector entry 5, unchanged original actor-flag
    // branch. Adding another semantic case requires its own native profile.
    return id == 70116 || id == 70138 || id == 70207 || id == 70234;
}
}

std::shared_ptr<const Bindings> Bindings::Build(const std::vector<Candidate>& candidates,
    const Authority& authority, std::string& error) {
    error.clear();
    auto reject=[&](const char* reason)->std::shared_ptr<const Bindings>{ error=reason; return {}; };
    if (candidates.empty() || candidates.size()>128) return reject("skill identity owner count");
    auto result=std::make_shared<Bindings>();std::set<std::string> owners;
    for (const auto& candidate:candidates) {
        if (!Owner(candidate.owner) || !Hash(candidate.revision) || !owners.insert(candidate.owner).second ||
            authority.revision(candidate.owner)!=candidate.revision) return reject("skill identity owner/revision is not authorized");
        if (candidate.skills.empty() || candidate.skills.size()>4096) return reject("skill identity entry count");
        for (const auto& skill:candidate.skills) {
            if (!skill.target || skill.target>9999999 || skill.target==skill.source || !Supported(skill.source))
                return reject("skill identity selector profile is unsupported");
            if (!Hash(skill.clientSha256) || !Hash(skill.serverSha256) || !Hash(skill.templateSha256))
                return reject("skill identity requires complete source/data hashes");
            if (authority.originalSkill(skill.target) || !authority.originalSkill(skill.source) ||
                !authority.templateSkill(skill.source,skill.templateSha256) ||
                !authority.allocatedSkill(candidate.owner,skill.target,skill.clientSha256,skill.serverSha256))
                return reject("skill identity allocation/template authority differs");
            if (result->skills_.size()>=4096 || !result->skills_.emplace(skill.target,skill.source).second)
                return reject("skill identity destinations repeat or exceed the budget");
        }
    }
    return result;
}

Result Bindings::resolve(const void* definition, std::uint32_t actualId, const Lookup& lookup) const {
    const auto entry=skills_.find(actualId);
    if (entry==skills_.end()) return {Resolution::Original,actualId,""};
    if (!definition || lookup.find(actualId)!=definition)
        return {Resolution::Rejected,0,"skill identity is not the actual borrowed destination definition"};
    if (!lookup.find(entry->second))
        return {Resolution::Rejected,0,"skill identity source definition is unavailable"};
    return {Resolution::Aliased,entry->second,""};
}
}
