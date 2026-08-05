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

/// One inline line box Chrome laid a text run out on.
///
/// A run that wraps produces several of these. The layout node's own `bounds`
/// is their union — the paragraph's block, not any single line — so a measured
/// advance is only comparable against a line box, never against the run's box.
/// `start` and `length` are character offsets into the layout node's text,
/// which is what makes the comparison well-defined: they name exactly the
/// substring whose advance the box records.
///
/// A box can also start to the right of its run's block when the run continues
/// a line an earlier inline sibling began.
struct CapturedTextBox {
    CapturedBox bounds;
    int start = 0;
    int length = 0;
};

/// One node Chrome actually laid out and painted, in the order it painted it.
///
/// The DOM snapshot's layout array holds exactly the nodes that produced a
/// layout object, so it is already the painted set — an element hidden by
/// `display: none` never appears, and neither does anything inside `<head>`.
struct CapturedPaintNode {
    int layout_index = -1;
    int node_index = -1;
    int backend_node_id = -1;
    /// Chrome's own paint order for this layout object. Ties are common (a
    /// paint order groups everything painted in the same phase of the same
    /// stacking context), so a consumer must break them by document order
    /// rather than treat the value as a unique rank.
    int paint_order = -1;
    int node_type = 0;              ///< DOM nodeType: 1 element, 3 text
    std::string tag_name;           ///< lowercase, e.g. "div"; "#text" for text
    std::string text;               ///< laid-out text run, empty for elements
    CapturedBox bounds;
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

    /// Computed declarations for one layout node, addressed directly.
    ///
    /// `styles_for` resolves an *element* through its semantic identity; whole
    /// -tree lowering already holds the layout index it wants, and going back
    /// through the backend id would re-run the paint-box search for a node that
    /// is its own answer.
    std::map<std::string, std::string> styles_for_layout(
        int layout_index) const;

    /// Computed declarations for one DOM node, or empty when that node
    /// produced no layout object.
    ///
    /// Empty is a real answer, not an error: an SVG `<defs>` subtree, a
    /// `display: none` element, and a comment all legitimately have no solved
    /// style. A caller that needs a property from such a node has to fall back
    /// to the authored attribute.
    std::map<std::string, std::string> styles_for_node(int node_index) const;

    /// Whether the capture asked Chrome for this property at all.
    ///
    /// Distinguishes "the browser resolved it to nothing" from "no consumer
    /// can ever know" — an older capture is missing whole properties, and a
    /// caller that reads absence as a value silently invents one.
    bool has_property(std::string_view name) const;

    /// The box Chrome laid the element out at, in page coordinates.
    std::optional<CapturedBox> bounds_for(int backend_node_id) const;

    /// The inline line boxes Chrome broke one layout node's text across, in
    /// document order.
    ///
    /// Empty for a node that laid out no text. A single-line run returns one
    /// box, which is why a caller must not treat "one box" as "the run's own
    /// bounds": the two agree only when the run happens not to wrap, and code
    /// that reads the bounds instead agrees with this on every unwrapped run
    /// and is wrong on every wrapped one.
    std::vector<CapturedTextBox> text_boxes_for_layout(int layout_index) const;

    /// The PostScript name of the face Blink actually shaped this run with, or
    /// empty when the capture recorded none.
    ///
    /// Read from the `platform-fonts.json` sidecar beside the snapshot. The
    /// computed `font-family` is a REQUEST — a list whose entries may be
    /// webfonts that failed to load or families the host lacks — so it cannot
    /// answer which typeface produced the recorded line breaks. Anything
    /// validating captured layout has to compare against the face, and an
    /// empty answer must be treated as "cannot validate" rather than "matches".
    std::string resolved_face_for_layout(int layout_index) const;

    /// Every laid-out node, in Chrome's paint order, ties broken by document
    /// order. This is the set a native renderer has to draw.
    std::vector<CapturedPaintNode> painted_nodes() const;

    /// An element's authored attribute value, or empty when absent.
    std::string attribute(int node_index, std::string_view name) const;

    /// A node's lowercase tag name, for any node in the document — not only the
    /// ones that were laid out. Ancestry questions have to reach elements that
    /// never painted themselves.
    std::string tag_name(int node_index) const;

    /// Walk up `parentIndex` from `node_index`. Returns -1 at the root.
    int parent_of(int node_index) const;

    /// The product of every ancestor `transform` scale above `node_index`.
    ///
    /// Nested transforms multiply, so a run three levels down inherits the
    /// product rather than its nearest wrapper's factor. Type carries one
    /// scalar — a `font-size` — so only a uniform, positive, unrotated scale
    /// can be folded into it: `scale(0.9, 1.2)` needs two axes and a flip or a
    /// rotation needs a matrix. Those are REFUSED rather than approximated,
    /// because a plausible wrong number is harder to find later than a
    /// recorded refusal.
    struct InheritedTypeScale {
        double scale = 1.0;
        /// Empty when the chain reduced. Otherwise the offending computed
        /// `transform`, so the refusal names the value that caused it.
        std::string refused;
        bool ok() const { return refused.empty(); }
    };
    InheritedTypeScale inherited_type_scale(int node_index) const;

    /// A node's DOM `nodeType` (1 element, 3 text), or 0 when out of range.
    ///
    /// A doctype carries the same `nodeName` as the root element, so tag name
    /// alone cannot tell them apart — and an anchor that counted the doctype as
    /// an `html` sibling would number the real root element `html[1]`.
    int node_type_of(int node_index) const noexcept {
        if (node_index < 0 || node_index >= static_cast<int>(node_type_.size()))
            return 0;
        return node_type_[static_cast<size_t>(node_index)];
    }

    /// How many nodes the document declares, laid out or not.
    ///
    /// A stable anchor is a DOM path, and a path segment's ordinal has to count
    /// ALL same-signature siblings — not only the ones that painted — or the
    /// anchor moves when a sibling is hidden rather than when the design
    /// changes.
    int node_count() const noexcept {
        return static_cast<int>(parent_index_.size());
    }

    bool empty() const noexcept { return style_rows_.empty(); }

private:
    std::optional<int> layout_for_node(int node_index) const;
    bool is_descendant(int node_index, int ancestor_index) const;
    std::string string_at(int index) const;

    std::vector<std::string> property_names_;
    std::vector<std::string> strings_;
    std::vector<int> parent_index_;                  ///< node index → parent
    std::vector<int> node_type_;                     ///< node index → nodeType
    std::vector<int> node_name_;                     ///< node index → string
    std::vector<std::vector<int>> node_attributes_;  ///< node index → name/value
    std::unordered_map<int, int> backend_to_node_;
    std::vector<int> node_to_backend_;               ///< node index → backend
    std::unordered_map<int, int> node_to_layout_;
    std::vector<int> layout_to_node_;                ///< layout index → node
    std::vector<int> layout_text_;                   ///< layout index → string
    std::vector<int> layout_paint_order_;            ///< layout index → order
    std::vector<std::vector<int>> style_rows_;       ///< layout index → strings
    std::vector<CapturedBox> layout_bounds_;
    /// layout index → its line boxes. Sized with the layout, so a lookup for a
    /// node that laid out no text is a bounds check rather than a miss.
    std::vector<std::vector<CapturedTextBox>> layout_text_boxes_;
    /// layout index → the PostScript name Blink resolved, when the capture
    /// carried a platform-fonts sidecar.
    std::unordered_map<int, std::string> layout_resolved_face_;
};

/// Which half of an element's appearance to fold onto a node.
///
/// A text run's computed style IS its parent element's, so writing the box half
/// onto a text node repaints the parent's background, border and shadow inside
/// the text's own rectangle — a second, smaller copy of the panel's every
/// surface. Naming the scope keeps one owner of the property list; clearing the
/// fields afterwards would silently leak every property added later.
enum class ComputedStyleScope {
    box_and_text,  ///< an element: fills, borders, effects, and typography
    text_only,     ///< a text run: typography and colour, no box decoration
};

/// Fold Chrome's computed declarations onto an IR node's style.
///
/// Appearance only: geometry (position, offsets, width/height, z-index) and
/// `display` are deliberately not written, because the caller has already
/// placed the node from the design's paint box and the page's own layout values
/// would fight that placement.
///
/// `type_scale` is the uniform scale the node's box already carries from an
/// ancestor `transform` — see `inherited_type_scale`. The snapshot's box is
/// post-transform while `font-size` and `letter-spacing` are the untransformed
/// computed values, so placing that box and filling it with unscaled type
/// draws every run `1 / scale` too wide. Multiplying the type lengths by the
/// same factor the box already carries is what puts them in one space.
void apply_computed_styles(
    const std::map<std::string, std::string>& computed,
    const std::optional<CapturedBox>& box,
    pulp::view::IRStyle& style,
    ComputedStyleScope scope = ComputedStyleScope::box_and_text,
    double type_scale = 1.0);

/// Split a CSS list on top-level commas, ignoring commas nested in functions
/// (`rgba(0, 0, 0, .5)` is one value, not four).
std::vector<std::string> split_css_list(const std::string& value);

/// Parse a CSS `box-shadow` declaration into ordered layers.
std::vector<pulp::view::IRBoxShadow> parse_box_shadow(const std::string& value);

}  // namespace pulp::import_design
