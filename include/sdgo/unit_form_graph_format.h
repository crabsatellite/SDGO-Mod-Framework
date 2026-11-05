#pragma once
#include "sdgo/unit_form_graph.h"
#include "sdgo/script_registration.h"
#include <cstring>
#include <set>
#include <stdexcept>

namespace sdgo::unit_forms {
namespace graph_format {
constexpr std::size_t kMaxBytes = 8u * 1024u * 1024u;
class Reader {
public:
    const std::vector<std::uint8_t>& bytes;std::size_t at = 0;
    explicit Reader(const std::vector<std::uint8_t>& data) : bytes(data) {}
    void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
    std::uint32_t word() {
        require(at <= bytes.size() && bytes.size()-at >= 4, "truncated unit form integer");
        std::uint32_t result = 0;std::memcpy(&result, bytes.data()+at, 4);at += 4;return result;
    }
    std::string hash() {
        require(at <= bytes.size() && bytes.size()-at >= 32, "truncated unit form SHA-256");
        const char hex[] = "0123456789abcdef";std::string result;
        for (unsigned i = 0; i < 32; ++i) { const auto c = bytes[at++];result += hex[c >> 4];result += hex[c & 15]; }
        return result;
    }
    std::string owner() {
        const auto size = word();require(size && size <= 128 && size <= bytes.size()-at, "unit form owner text extent");
        std::string result(reinterpret_cast<const char*>(bytes.data()+at), size);at += size;return result;
    }
    void magic(const char* value) { require(bytes.size() >= 8 && !std::memcmp(bytes.data(), value, 8), "unit form runtime magic");at = 8; }
    std::vector<GraphOwner> owners(std::uint32_t count) {
        require(count && count <= 512, "unit form runtime owner count");
        std::vector<GraphOwner> result;std::size_t total = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            GraphOwner entry;entry.owner = owner();entry.revisionSha256 = hash();
            auto size = word();require(size && size <= 4096, "unit form runtime root count");
            std::uint32_t previous = 0;
            for (std::uint32_t j = 0; j < size; ++j) {
                const auto id = word();require(id > previous && id >= 10001 && id <= 30000, "unit form runtime root order/domain");
                entry.roots.push_back(id);previous = id;
            }
            size = word();require(size && size <= 4096 && total + size <= 16384, "unit form runtime node count");total += size;previous = 0;
            for (std::uint32_t j = 0; j < size; ++j) {
                GraphNode node;node.id = word();node.target = word();node.origin = static_cast<NodeOrigin>(word());
                require(node.id > previous, "unit form runtime node order");previous = node.id;
                node.clientSha256 = hash();node.serverSha256 = hash();entry.nodes.push_back(std::move(node));
            }
            result.push_back(std::move(entry));
        }
        return result;
    }
};
}

class InstalledGraphAuthority final : public GraphAuthority {
public:
    std::string revision(const std::string& owner) const override {
        const auto found = owners_.find(owner);return found == owners_.end() ? "" : found->second.revisionSha256;
    }
    bool entry(const std::string& owner, std::uint32_t root) const override {
        const auto found = owners_.find(owner);if (found == owners_.end()) return false;
        for (auto id : found->second.roots) if (id == root) return true;
        return false;
    }
    bool unit(const std::string& owner, const GraphNode& node) const override {
        const auto found = owners_.find(owner);if (found == owners_.end()) return false;
        for (const auto& allowed : found->second.nodes)
            if (allowed.id == node.id) return allowed.target == node.target && allowed.origin == node.origin &&
                allowed.clientSha256 == node.clientSha256 && allowed.serverSha256 == node.serverSha256;
        return false;
    }
private:
    friend class GraphRuntimeInput;
    std::map<std::string, GraphOwner> owners_;
};

class GraphRuntimeInput {
public:
    std::vector<GraphOwner> candidates;
    InstalledGraphAuthority authority;
    std::shared_ptr<const GraphBindings> bindings;
    std::string candidateSha256, serverFingerprint;
    bool Parse(const std::vector<std::uint8_t>& candidate, const std::vector<std::uint8_t>& trusted, std::string& error) {
        candidates.clear();authority = {};bindings.reset();candidateSha256.clear();serverFingerprint.clear();error.clear();
        try {
            graph_format::Reader content(candidate), installation(trusted);
            content.require(candidate.size() >= 64 && candidate.size() <= 64+graph_format::kMaxBytes, "unit form candidate byte budget");
            content.magic("SDGOUF1\0");content.require(content.word() == 1 && content.word() == 64, "unit form candidate version/header");
            const auto count = content.word(), size = content.word();content.require(size == candidate.size()-64, "unit form candidate extent");
            const auto digest = content.hash();content.require(content.word() == 0 && content.word() == 0, "unit form candidate reserved bytes");
            content.require(scripts::Sha256(std::vector<std::uint8_t>(candidate.begin()+64, candidate.end())) == digest, "unit form payload SHA-256 differs");
            auto requested = content.owners(count);content.require(content.at == candidate.size(), "unit form candidate trailing bytes");
            installation.require(trusted.size() >= 128 && trusted.size() <= 128+graph_format::kMaxBytes, "unit form authority byte budget");
            installation.magic("SDGOUA1\0");installation.require(installation.word() == 1 && installation.word() == 128, "unit form authority version/header");
            const auto owners = installation.word(), extent = installation.word();
            installation.require(owners == count && extent == trusted.size()-128 && extent == size, "unit form authority extent/count");
            const auto candidateHash = installation.hash(), fingerprint = installation.hash();
            installation.require(installation.hash() == "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4" &&
                installation.word() == 0 && installation.word() == 0, "unit form authority host/reserved bytes");
            installation.require(candidateHash == scripts::Sha256(candidate), "unit form candidate differs from installed authority");
            installation.require(!std::memcmp(candidate.data()+64, trusted.data()+128, size), "unit form installed graph payload differs");
            auto allowed = installation.owners(owners);installation.require(installation.at == trusted.size(), "unit form authority trailing bytes");
            InstalledGraphAuthority resolved;
            for (const auto& owner : allowed)
                installation.require(resolved.owners_.emplace(owner.owner, owner).second, "unit form authority repeats owner");
            std::string reason;auto policy = GraphBindings::Build(requested, resolved, reason);
            installation.require(bool(policy), reason.empty() ? "unit form policy rejected" : reason.c_str());
            candidates = std::move(requested);authority = std::move(resolved);bindings = std::move(policy);
            candidateSha256 = candidateHash;serverFingerprint = fingerprint;return true;
        } catch (const std::exception& exception) { error = exception.what();return false; }
    }
};
} // namespace sdgo::unit_forms
