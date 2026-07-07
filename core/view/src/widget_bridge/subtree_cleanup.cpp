#include <pulp/view/widget_bridge.hpp>

#include <string>
#include <vector>

namespace pulp::view {

namespace {

void collect_widget_subtree_ids(View* node, std::vector<std::string>& ids) {
    if (node == nullptr) {
        return;
    }

    for (std::size_t i = 0; i < node->child_count(); ++i) {
        collect_widget_subtree_ids(node->child_at(i), ids);
    }

    if (!node->id().empty()) {
        ids.push_back(node->id());
    }
}

} // namespace

void WidgetBridge::forget_widget_subtree(View* node) {
    std::vector<std::string> ids;
    collect_widget_subtree_ids(node, ids);
    if (ids.empty()) return;

    for (const auto& id : ids) {
        widgets_.erase(id);
        pointer_registered_.erase(id);
        wheel_registered_.erase(id);
    }
    prune_dangling_bindings();
}

} // namespace pulp::view
