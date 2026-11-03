#pragma once
#include "sdgo_api.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sdgo::resources {
struct Resource { std::string path, sha256; };
struct Weapon { std::uint32_t id = 0; std::string sha256; std::vector<std::uint8_t> record; };
struct FixedEffect { std::uint32_t weapon = 0; std::string target; };
struct Candidate {
    std::string owner, revision;
    std::vector<Resource> resources;
    std::vector<Weapon> weapons;
    std::vector<FixedEffect> fixedEffects;
};

// The importer supplies a trusted, immutable allocation/publication view. These
// facts cannot be accepted from the candidate's own claimed owner or flags.
class Authority {
public:
    virtual ~Authority() = default;
    virtual std::string revision() const = 0;
    virtual bool originalWeapon(std::uint32_t id) const = 0;
    virtual bool allocatedWeapon(const std::string& owner, std::uint32_t id, const std::string& sha256) const = 0;
    virtual bool publishedResource(const std::string& path, const std::string& sha256) const = 0;
};

enum class Resolution { Original, Redirected, Fatal };
struct Result { Resolution resolution = Resolution::Original; const wchar_t* path = nullptr; const char* reason = ""; };

class Bindings {
public:
    static std::shared_ptr<const Bindings> Build(const std::vector<Candidate>& candidates, const Authority& authority, std::string& error);
    bool owns(std::uint32_t id) const;
    Result resolve(std::uint32_t entryWeapon, const std::uint8_t* record, std::size_t bytes, const wchar_t* original) const;
    std::size_t fixedEffectCount() const { return fixedCount_; }
private:
    struct Entry { std::vector<std::uint8_t> record; std::wstring target; };
    std::map<std::uint32_t, Entry> weapons_;
    std::size_t fixedCount_ = 0;
};

using CloseCandidateClient = void (__cdecl*)(void* context, const char* reason);
// One hook per process/module lifetime. This only rewrites the existing string
// argument; the original effect controller and all of its lifecycle still run.
// A fatal constructor context cannot resume. If closeClient returns, the adapter
// terminates the current candidate process rather than continue a partial ctor.
bool InstallFixedEffectHook(const SdgoApi* api, SdgoMod* mod,
    std::shared_ptr<const Bindings> bindings, CloseCandidateClient closeClient,
    void* closeContext, std::string& error);
} // namespace sdgo::resources
