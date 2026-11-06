#include "sdgo/unit_form_graph.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <set>

using namespace sdgo::unit_forms;
namespace {
const std::string hash(64, 'a');
GraphNode Node(unsigned id, unsigned target, NodeOrigin origin = NodeOrigin::owned) {
    return {id, target, origin, hash, hash};
}
GraphOwner Fixture() {
    return {"Example.family", hash, {15001}, {Node(15001, 15002), Node(15002, 15003), Node(15003, 15002), Node(15004, 15003)}};
}
class Authority final : public GraphAuthority {
public:
    std::vector<GraphOwner> allowed;
    explicit Authority(std::vector<GraphOwner> values) : allowed(std::move(values)) {}
    std::string revision(const std::string& name) const override {
        for (const auto& owner : allowed) if (owner.owner == name) return owner.revisionSha256;
        return "";
    }
    bool entry(const std::string& name, unsigned root) const override {
        for (const auto& owner : allowed) if (owner.owner == name)
            for (auto id : owner.roots) if (id == root) return true;
        return false;
    }
    bool unit(const std::string& name, const GraphNode& node) const override {
        for (const auto& owner : allowed) if (owner.owner == name)
            for (const auto& n : owner.nodes)
                if (n.id == node.id) return n.target == node.target && n.origin == node.origin &&
                    n.clientSha256 == node.clientSha256 && n.serverSha256 == node.serverSha256;
        return false;
    }
};
}

int main() {
    auto source = Fixture();Authority authority({source});std::string error;
    auto graph = GraphBindings::Build({source}, authority, error);assert(graph && error.empty());
    for (auto edge : {std::pair<unsigned,unsigned>{15001,15002}, {15002,15003}, {15003,15002}})
        assert(graph->evaluate(15001, edge.first, edge.second).resolution == EdgeResolution::allowed);
    assert(graph->evaluate(15001, 15002, 15001).resolution == EdgeResolution::rejected); // No invented reverse edge.
    assert(graph->evaluate(15001, 15004, 15003).resolution == EdgeResolution::rejected); // Incoming auxiliary is not reachable.
    assert(graph->inspectNode(15001, 15004) && graph->inspectNode(15001, 15004)->target == 15003); // Data is retained.
    assert(!graph->resolveTarget(15001, 15004, 15003));
    assert(graph->resolveTarget(15001, 15002, 15003)->id == 15003);
    assert(graph->evaluate(15004, 15004, 15003).resolution == EdgeResolution::unmanaged_root); // Not an entry root.
    assert(graph->evaluate(15001, 35000, 15002).resolution == EdgeResolution::rejected);
    auto changed = source;changed.roots.push_back(15004);
    assert(!GraphBindings::Build({changed}, authority, error));
    changed = source;changed.nodes[0].target = 15003;
    assert(!GraphBindings::Build({changed}, authority, error));
    changed = source;changed.revisionSha256[0] = 'b';
    assert(!GraphBindings::Build({changed}, authority, error));
    changed = source;changed.nodes.push_back(changed.nodes[0]);Authority duplicate({changed});
    assert(!GraphBindings::Build({changed}, duplicate, error));
    changed = source;changed.nodes[0].target = 15009;Authority open({changed});
    assert(!GraphBindings::Build({changed}, open, error));
    GraphOwner self{"Self", hash, {15001}, {Node(15001,15001)}};Authority selfAuthority({self});
    auto selfGraph = GraphBindings::Build({self}, selfAuthority, error);assert(selfGraph);
    assert(selfGraph->inspectNode(15001,15001)->target == 15001);
    assert(selfGraph->evaluate(15001,15001,15001).resolution == EdgeResolution::rejected);
    GraphOwner terminal{"Terminal",hash,{15001},{Node(15001,0)}};Authority terminalAuthority({terminal});
    assert(GraphBindings::Build({terminal},terminalAuthority,error)->evaluate(15001,15001,15002).resolution == EdgeResolution::rejected);
    GraphOwner first{"First",hash,{15001},{Node(15001,15003),Node(15003,0,NodeOrigin::original)}};
    GraphOwner second{"Second",hash,{15002},{Node(15002,15003),Node(15003,0,NodeOrigin::original)}};
    Authority shared({first,second});assert(GraphBindings::Build({first,second},shared,error));
    second.nodes[1].serverSha256[0] = 'b';Authority mismatched({first,second});
    assert(!GraphBindings::Build({first,second},mismatched,error));
    second.nodes[1] = Node(15003,0);Authority ownedCross({first,second});
    assert(!GraphBindings::Build({first,second},ownedCross,error));
    std::puts("unit_form_graph_test: PASS (policy only; no native actor changes)");
}
