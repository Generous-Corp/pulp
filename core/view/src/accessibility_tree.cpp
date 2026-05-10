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
    // pulp #1737 — surface ARIA state attributes through the cross-
    // platform snapshot so AT bridges (and offline tests) see the state
    // alongside role/label/value. Platform bridges (NSAccessibility
    // here; AT-SPI / UIA when wired per #217) read the same View slots
    // directly; the snapshot is the cross-platform mirror.
    snap.pressed  = v.access_pressed();
    snap.checked  = v.access_checked();
    snap.disabled = v.access_disabled();
    snap.hidden   = v.access_hidden();
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
    // Match platform bridge output by excluding the root node itself
    // and returning only its descendants. macOS (accessibility_mac.mm)
    // and iOS (accessibility_ios.mm) VoiceOver bridges iterate
    // root.child_count() and never push the root; including it here
    // created false cross-platform parity failures when the root had a
    // non-none role (e.g. group). Fix per #192 review.
    std::vector<AccessibilityNodeSnapshot> out;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (const View* child = root.child_at(i)) {
            walk(*child, 0, out);
        }
    }
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
