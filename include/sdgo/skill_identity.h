#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sdgo::skill_identity {
struct Skill {
    std::uint32_t target = 0, source = 0;
    std::string clientSha256, serverSha256, templateSha256;
};
struct Candidate { std::string owner, revision; std::vector<Skill> skills; };

// Supplied by the trusted allocation/deployment controller, independently of
// the content candidate. These methods never take candidate-provided flags.
class Authority {
public:
    virtual ~Authority() = default;
    virtual std::string revision(const std::string& owner) const = 0;
    virtual bool originalSkill(std::uint32_t id) const = 0;
    virtual bool templateSkill(std::uint32_t id, const std::string& sha256) const = 0;
    virtual bool allocatedSkill(const std::string& owner, std::uint32_t id,
        const std::string& clientSha256, const std::string& serverSha256) const = 0;
};

// Returns the borrowed native definition pointer. The verified original native
// registry supplies this lookup; no complete record layout is assumed here.
class Lookup {
public:
    virtual ~Lookup() = default;
    virtual const void* find(std::uint32_t id) const = 0;
};

enum class Resolution { Original, Aliased, Rejected };
struct Result { Resolution resolution = Resolution::Original; std::uint32_t selector = 0; const char* reason = ""; };

class Bindings {
public:
    static std::shared_ptr<const Bindings> Build(const std::vector<Candidate>& candidates,
        const Authority& authority, std::string& error);
    Result resolve(const void* definition, std::uint32_t actualId, const Lookup& lookup) const;
    bool owns(std::uint32_t id) const { return skills_.count(id) != 0; }
private:
    std::map<std::uint32_t,std::uint32_t> skills_;
};
}
