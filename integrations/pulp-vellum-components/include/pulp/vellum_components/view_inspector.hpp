#pragma once

/// @file view_inspector.hpp
/// Read-only introspection of a Vellum view tree: JSON serialization, lookup
/// by id or design-import anchor, and the row models the inspector window
/// shows for a view.

#include <pulp/vellum_components/aliases.hpp>

#include <vellum/view/property_list.hpp>
#include <vellum/view/tree_view.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace pulp::vellum_components {

class ViewInspector {
  public:
    /// Serialize the entire view tree to formatted JSON.
    static std::string to_json(const View& root);

    /// Find a view by id in the tree; nullptr when none matches.
    static View* find_by_id(View& root, const std::string& id);

    /// Find a view by its design-import anchor id. An empty anchor never
    /// matches: views without an anchor leave it empty, and "" must not
    /// resolve to the first such view.
    static View* find_by_anchor(View& root, const std::string& anchor);

    /// Count the views in the tree, the root included.
    static std::size_t count_views(const View& root);

    /// The type name the inspector shows ("Knob", "Fader", "View", ...),
    /// shared with Vellum's own diagnostics.
    static std::string type_name(const View& view);

    /// Recursively add a TreeNode per view under `parent`. Each label is the
    /// type name plus " #id" when the view has one, and `user_data` carries
    /// the View pointer.
    static void populate_tree(::vellum::view::TreeNode& parent, const View& root);

    /// The property rows for a view, grouped Identity, Layout, Visual and
    /// Widget.
    static std::vector<::vellum::view::PropertyList::Property> view_properties(const View& view);

    /// Absolute bounds, walking the parent chain and honouring each
    /// container's child paint offset (a scrolled ScrollView shifts its
    /// children).
    static ::vellum::view::Rect absolute_bounds(const View& view);
};

} // namespace pulp::vellum_components
