#include "accessibility_win_fragment_topology.hpp"
#include <pulp/view/accessibility.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {
namespace {

void build_nodes(View& view, int parent_index,
                 std::vector<UiaFragmentNode>& nodes) {
    int own_index = parent_index;
    if (is_accessibility_element(view)) {
        UiaFragmentNode node;
        node.view = &view;
        node.index = static_cast<int>(nodes.size());
        node.parent_index = parent_index;
        nodes.push_back(node);
        own_index = node.index;
    }
    for (std::size_t i = 0; i < view.child_count(); ++i) {
        if (auto* child = view.child_at(i))
            build_nodes(*child, own_index, nodes);
    }
}

void link_children(std::vector<UiaFragmentNode>& nodes) {
    for (auto& node : nodes) {
        int previous = -1;
        for (auto& candidate : nodes) {
            if (candidate.parent_index != node.index) continue;
            if (node.first_child == -1) node.first_child = candidate.index;
            node.last_child = candidate.index;
            candidate.prev_sibling = previous;
            if (previous != -1)
                nodes[static_cast<std::size_t>(previous)].next_sibling =
                    candidate.index;
            previous = candidate.index;
        }
    }
}

} // namespace

UiaFragmentTopology build_uia_fragment_topology(View& root) {
    UiaFragmentTopology topology;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* child = root.child_at(i))
            build_nodes(*child, -1, topology.nodes);
    }
    link_children(topology.nodes);
    int previous = -1;
    for (auto& candidate : topology.nodes) {
        if (candidate.parent_index != -1) continue;
        if (topology.first_child == -1)
            topology.first_child = candidate.index;
        topology.last_child = candidate.index;
        candidate.prev_sibling = previous;
        if (previous != -1)
            topology.nodes[static_cast<std::size_t>(previous)].next_sibling =
                candidate.index;
        previous = candidate.index;
    }
    return topology;
}

} // namespace pulp::view
