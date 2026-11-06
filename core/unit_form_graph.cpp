#include "sdgo/unit_form_graph.h"

namespace sdgo::unit_forms {
namespace {
bool Id(std::uint32_t id) { return id >= 10001 && id <= 30000; }
bool Hash(const std::string& hash) {
    if (hash.size() != 64) return false;
    for (const char c : hash) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool OwnerName(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!(letter || (i && (c == '.' || c == '_' || c == '-')))) return false;
    }
    return true;
}
bool Same(const GraphNode& a, const GraphNode& b) {
    return a.id == b.id && a.target == b.target && a.origin == b.origin &&
           a.clientSha256 == b.clientSha256 && a.serverSha256 == b.serverSha256;
}
}

std::shared_ptr<const GraphBindings> GraphBindings::Build(const std::vector<GraphOwner>& candidates,
                                                         const GraphAuthority& authority, std::string& error) {
    error.clear();
    auto reject = [&](const char* reason)->std::shared_ptr<const GraphBindings> { error = reason;return {}; };
    if (candidates.empty() || candidates.size() > 512) return reject("unit form owner count budget");
    auto result = std::make_shared<GraphBindings>();
    std::map<std::uint32_t, GraphNode> identities;std::size_t total = 0;
    for (const auto& candidate : candidates) {
        if (!OwnerName(candidate.owner) || !Hash(candidate.revisionSha256) ||
            result->owners_.count(candidate.owner) || authority.revision(candidate.owner) != candidate.revisionSha256)
            return reject("unit form owner/revision is not authorized");
        if (candidate.nodes.empty() || candidate.nodes.size() > 4096 || candidate.roots.empty() || candidate.roots.size() > 4096)
            return reject("unit form node/root count budget");
        Owner owner;
        for (const auto& node : candidate.nodes) {
            if (!Id(node.id) || (node.target && !Id(node.target)) || !Hash(node.clientSha256) || !Hash(node.serverSha256) ||
                (node.origin != NodeOrigin::owned && node.origin != NodeOrigin::original))
                return reject("unit form node domain/origin/hash differs");
            if (!authority.unit(candidate.owner, node)) return reject("unit form installed record authority differs");
            if (!owner.nodes.emplace(node.id, node).second) return reject("unit form node is repeated");
            const auto previous = identities.find(node.id);
            if (previous != identities.end() && (node.origin != NodeOrigin::original || !Same(previous->second, node)))
                return reject("unit form shared identity crosses owners or versions");
            identities.emplace(node.id, node);
            if (++total > 16384 || identities.size() > 4096) return reject("unit form aggregate node budget");
        }
        for (const auto& entry : owner.nodes)
            if (entry.second.target && !owner.nodes.count(entry.second.target))
                return reject("unit form directed graph is not closed");
        for (const auto root : candidate.roots) {
            const auto found = owner.nodes.find(root);
            if (!Id(root) || found == owner.nodes.end() || found->second.origin != NodeOrigin::owned ||
                !authority.entry(candidate.owner, root) || !result->roots_.emplace(root, candidate.owner).second)
                return reject("unit form inventory root is not authorized or repeats");
        }
        if (result->roots_.size() > 4096) return reject("unit form aggregate root budget");
        result->owners_.emplace(candidate.owner, std::move(owner));
    }
    return result;
}

const GraphNode* GraphBindings::inspectNode(std::uint32_t root, std::uint32_t id) const {
    const auto entry = roots_.find(root);if (entry == roots_.end()) return nullptr;
    const auto& nodes = owners_.at(entry->second).nodes;
    const auto found = nodes.find(id);return found == nodes.end() ? nullptr : &found->second;
}

const GraphNode* GraphBindings::resolveTarget(std::uint32_t root, std::uint32_t current, std::uint32_t target) const {
    return evaluate(root, current, target).resolution == EdgeResolution::allowed ? inspectNode(root, target) : nullptr;
}

EdgeResult GraphBindings::evaluate(std::uint32_t root, std::uint32_t current, std::uint32_t target) const {
    const auto entry = roots_.find(root);
    if (entry == roots_.end()) return {EdgeResolution::unmanaged_root, ""};
    if (!Id(current) || !Id(target)) return {EdgeResolution::rejected, "unit form requested ID is outside the native domain"};
    const auto& nodes = owners_.at(entry->second).nodes;
    auto cursor = root;
    // A deterministic closed graph either terminates or repeats within its
    // node count. Keep the packet-time query bounded and allocation-free.
    for (std::size_t step = 0; cursor && step < nodes.size(); ++step) {
        const auto& actual = nodes.at(cursor);
        if (cursor == current) {
            if (!actual.target || actual.target != target)
                return {EdgeResolution::rejected, "unit form request is not the recorded directed edge"};
            if (current == target)
                return {EdgeResolution::rejected, "same-ID form changes require a separate native behavior profile"};
            return {EdgeResolution::allowed, ""};
        }
        cursor = actual.target;
    }
    return {EdgeResolution::rejected, "unit form current node is not reachable from the inventory root"};
}
} // namespace sdgo::unit_forms
