#include <pulp/view/accessibility_tree.hpp>
#include <pulp/view/accessibility.hpp>

namespace pulp::view {
namespace {

void walk(const View& v, int depth,
          std::vector<AccessibilityNodeSnapshot>& out) {
    AccessibilityNodeSnapshot snap;
    snap.view  = &v;
    snap.role  = v.access_role();
    snap.label = v.access_label();
    snap.value = v.access_value();
    snap.depth = depth;

    // If the view implements AccessibilityValueInterface, fill value range.
    if (const auto* vif = dynamic_cast<const AccessibilityValueInterface*>(&v)) {
        snap.has_value      = true;
        snap.min_value      = vif->get_minimum_value();
        snap.max_value      = vif->get_maximum_value();
        snap.current_value  = vif->get_current_value();
        snap.value_string   = vif->get_value_string();
    }

    out.push_back(std::move(snap));

    for (size_t i = 0; i < v.child_count(); ++i) {
        if (const View* child = v.child_at(i)) {
            walk(*child, depth + 1, out);
        }
    }
}

} // namespace

std::vector<AccessibilityNodeSnapshot>
snapshot_accessibility_tree(const View& root) {
    std::vector<AccessibilityNodeSnapshot> out;
    walk(root, 0, out);
    return out;
}

std::size_t count_announceable(const View& root) {
    std::size_t n = 0;
    for (const auto& node : snapshot_accessibility_tree(root)) {
        if (node.role != View::AccessRole::none) ++n;
    }
    return n;
}

const View* find_by_role_and_label(const View& root,
                                   View::AccessRole role,
                                   std::string_view label) {
    for (const auto& node : snapshot_accessibility_tree(root)) {
        if (node.role == role && node.label == label) return node.view;
    }
    return nullptr;
}

} // namespace pulp::view
