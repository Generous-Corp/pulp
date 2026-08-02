#pragma once

#include <vector>

namespace pulp::view {

class View;

struct UiaFragmentNode {
    View* view = nullptr;
    int index = 0;
    int parent_index = -1;
    int first_child = -1;
    int last_child = -1;
    int prev_sibling = -1;
    int next_sibling = -1;
};

struct UiaFragmentTopology {
    std::vector<UiaFragmentNode> nodes;
    int first_child = -1;
    int last_child = -1;
};

UiaFragmentTopology build_uia_fragment_topology(View& root);

} // namespace pulp::view
