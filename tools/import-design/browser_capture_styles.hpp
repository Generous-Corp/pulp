#pragma once

#include <pulp/view/design_ir.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::import_design {

/// A layout box in capture page coordinates, matching the semantic report's
/// `bounds` / `paint_bounds` and the DOM snapshot's `layout.bounds`.
struct CapturedBox {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// Chrome's solved appearance for the captured document, indexed for lookup by
/// backend node id.
///
/// `DOMSnapshot.captureSnapshot` returns computed styles as rows of string-table
/// indices parallel to the requested property list, addressed by *layout* node
/// while the semantic report addresses elements by *backend node id*. This type
/// owns that three-hop join (backend id → node index → layout index → style row)
/// so callers can ask for an element's appearance directly.
class CapturedStyleIndex {
public:
    /// Read a `dom-snapshot.json` sidecar. Returns nullopt when the file is
    /// absent, unreadable, or does not carry per-node computed styles — a
    /// capture without solved styles still lowers, it just gains no appearance.
    static std::optional<CapturedStyleIndex> load(
        const std::filesystem::path& snapshot_path);

    /// Computed declarations for the element that paints `paint_box`, searched
    /// within the subtree rooted at `backend_node_id`.
    ///
    /// A control's semantic node is often a wrapper whose own box carries only
    /// text styling while the gradient, radius, and shadow that make it look
    /// like a knob sit on an inner face element. `paint_box` names that face, so
    /// resolving through it is what returns the control's real appearance
    /// rather than its label's. Falls back to the node itself when no
    /// descendant matches.
    std::map<std::string, std::string> styles_for(
        int backend_node_id,
        const std::optional<CapturedBox>& paint_box) const;

    /// The box Chrome laid the element out at, in page coordinates.
    std::optional<CapturedBox> bounds_for(int backend_node_id) const;

    bool empty() const noexcept { return style_rows_.empty(); }

private:
    std::optional<int> layout_for_node(int node_index) const;
    bool is_descendant(int node_index, int ancestor_index) const;

    std::vector<std::string> property_names_;
    std::vector<std::string> strings_;
    std::vector<int> parent_index_;                  ///< node index → parent
    std::unordered_map<int, int> backend_to_node_;
    std::unordered_map<int, int> node_to_layout_;
    std::vector<int> layout_to_node_;                ///< layout index → node
    std::vector<std::vector<int>> style_rows_;       ///< layout index → strings
    std::vector<CapturedBox> layout_bounds_;
};

/// Fold Chrome's computed declarations onto an IR node's style.
///
/// Appearance only: geometry (position, offsets, width/height, z-index) and
/// `display` are deliberately not written, because the caller has already
/// placed the node from the design's paint box and the page's own layout values
/// would fight that placement.
void apply_computed_styles(
    const std::map<std::string, std::string>& computed,
    const std::optional<CapturedBox>& box,
    pulp::view::IRStyle& style);

/// Split a CSS list on top-level commas, ignoring commas nested in functions
/// (`rgba(0, 0, 0, .5)` is one value, not four).
std::vector<std::string> split_css_list(const std::string& value);

/// Parse a CSS `box-shadow` declaration into ordered layers.
std::vector<pulp::view::IRBoxShadow> parse_box_shadow(const std::string& value);

}  // namespace pulp::import_design
