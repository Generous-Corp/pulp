#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/tree_view.hpp>
#include <pulp/view/property_list.hpp>
#include <string>
#include <vector>

namespace pulp::view {

// Introspects a View tree and produces a JSON representation
// Used by the component inspector and for debugging
class ViewInspector {
public:
    // Serialize the entire view tree to formatted JSON
    static std::string to_json(const View& root);

    // Find a view by ID in the tree
    static View* find_by_id(View& root, const std::string& id);

    // Find a view by its design-import anchor id (Phase 0b
    // `set_anchor_id`). Returns nullptr for an empty anchor or when no
    // view in the tree carries it. Used by the inspector's source-jump
    // resolver (Phase 5.1) to map an anchor back to a live View*.
    static View* find_by_anchor(View& root, const std::string& anchor);

    // Count total views in the tree
    static size_t count_views(const View& root);

    // Get the type name of a view (e.g., "Knob", "Fader", "View")
    static std::string type_name(const View& view);

    // ── Inspector window helpers ────────────────────────────────────────

    /// Recursively populate a TreeNode hierarchy from a View tree.
    /// Sets label = type_name + id, user_data = View*.
    static void populate_tree(TreeNode& parent, const View& root);

    /// Build a list of properties for display in a PropertyList.
    /// Categories: Identity, Layout, Visual, Widget.
    static std::vector<PropertyList::Property> view_properties(const View& view);

    /// Compute absolute bounds by walking the parent chain.
    static Rect absolute_bounds(const View& view);
};

} // namespace pulp::view
