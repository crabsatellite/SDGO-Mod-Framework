#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sdgo::unit_forms {
enum class NodeOrigin : std::uint32_t { owned = 1, original = 2 };
struct GraphNode {
    std::uint32_t id = 0, target = 0;
    NodeOrigin origin = NodeOrigin::owned;
    std::string clientSha256, serverSha256;
};
struct GraphOwner {
    std::string owner, revisionSha256;
    std::vector<std::uint32_t> roots;
    std::vector<GraphNode> nodes;
};

// Supplied independently by the trusted server installation controller. A
// candidate cannot make its own nodes or auxiliary forms into inventory roots.
class GraphAuthority {
public:
    virtual ~GraphAuthority() = default;
    virtual std::string revision(const std::string& owner) const = 0;
    virtual bool entry(const std::string& owner, std::uint32_t root) const = 0;
    virtual bool unit(const std::string& owner, const GraphNode&) const = 0;
};

enum class EdgeResolution { unmanaged_root, allowed, rejected };
struct EdgeResult { EdgeResolution resolution = EdgeResolution::rejected; const char* reason = ""; };

// Immutable, process-local policy only. Native callers still validate real
// actor pointers, immutable inventory root, lifecycle generation and original
// sender/alive/room checks. This class neither hooks nor changes actors.
class GraphBindings {
public:
    static std::shared_ptr<const GraphBindings> Build(const std::vector<GraphOwner>&,
                                                    const GraphAuthority&, std::string& error);
    EdgeResult evaluate(std::uint32_t inventoryRoot, std::uint32_t current, std::uint32_t target) const;
    bool hasEntry(std::uint32_t root) const { return roots_.count(root) != 0; }
    // Data inspection only, including auxiliary/unreachable nodes. Never use
    // this method as permission to compose or commit a requested transform.
    const GraphNode* inspectNode(std::uint32_t inventoryRoot, std::uint32_t id) const;
    // Returns the target only after the full directed/reachable edge policy.
    // Native actor/generation/room permission checks remain the caller's job.
    const GraphNode* resolveTarget(std::uint32_t inventoryRoot, std::uint32_t current, std::uint32_t target) const;
private:
    struct Owner { std::map<std::uint32_t, GraphNode> nodes; };
    std::map<std::string, Owner> owners_;
    std::map<std::uint32_t, std::string> roots_;
};
} // namespace sdgo::unit_forms
