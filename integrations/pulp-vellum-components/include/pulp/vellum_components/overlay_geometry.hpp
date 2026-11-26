#pragma once

/// @file overlay_geometry.hpp
/// Pure geometry and hit-testing for the inspector's canvas overlay: a view's
/// rectangle in root space, the eight resize handles and the body of a
/// selection, the cursor each of them asks for, the flattened tree the panel
/// lists, and the panel's own hit rectangle.
///
/// Every function takes the rectangles and points it needs. None reads
/// overlay state or runs audio, so a widget, a test or another subsystem can
/// ask where a handle is without building an overlay.

#include <pulp/vellum_components/aliases.hpp>

#include <vellum/view/geometry.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace pulp::vellum_components::overlay {

using ::vellum::view::Point;
using ::vellum::view::Rect;

/// A resize handle on a selection: four corners and four edge midpoints.
enum class DragHandle : std::uint8_t { none, nw, ne, sw, se, n, s, e, w };

/// What a press at a point would do, and so which cursor to show.
enum class CursorAffordance : std::uint8_t {
    none,
    move,
    resize_nw_se, ///< top-left or bottom-right corner
    resize_ne_sw, ///< top-right or bottom-left corner
    resize_ns,    ///< top or bottom edge
    resize_ew     ///< left or right edge
};

/// Half the side of a handle's grab box. The handle paints 8 px wide and is
/// grabbed over 12, because corners are small targets.
inline constexpr float kHandleGrabRadius = 6.0f;

/// The view's rectangle in `root`'s space: its bounds summed up the parent
/// chain, stopping before `root`. A view outside `root` sums to the top of its
/// own chain. A null view gives an empty rectangle.
Rect bounds_in_root(const View* view, const View& root);

/// The product of the view's scale and every ancestor's, up to `root`
/// exclusive. It mirrors the cumulative scale the paint pass applies down a
/// subtree.
float effective_scale_in_root(const View* view, const View& root);

/// True when `ancestor` is `view` or one of its ancestors.
bool is_self_or_ancestor(const View* ancestor, const View* view);

/// The nearest ancestor of `view` that is not statically positioned, else
/// `root`, which always forms a containing block. `block_in_root` receives
/// that block's rectangle in root space.
const View* containing_block_of(const View* view, const View& root, Rect& block_in_root);

/// The handle whose grab box holds `pos`, for a selection occupying
/// `selection`. Corners win over edge midpoints.
DragHandle hit_test_handle(const Rect& selection, Point pos);

/// True when `pos` is inside `selection` and not on a handle: a press there
/// moves the selection.
bool hit_test_body(const Rect& selection, Point pos);

/// The cursor affordance of a handle.
CursorAffordance affordance_for_handle(DragHandle handle);

/// The affordance of an idle hover at `pos`: a handle resizes, the body
/// moves, anywhere else asks for nothing.
CursorAffordance affordance_at(const Rect& selection, Point pos);

/// The `vellum::view::View::CursorStyle` a host applies for `affordance`,
/// as an int, or -1 to keep the cursor of the view under the pointer.
int cursor_style_value(CursorAffordance affordance);

/// One visible row of the panel's view tree.
struct FlatTreeItem {
    const View* view;
    int depth;
};

/// The rows the panel lists: `root` and every descendant, depth first, with
/// the children of a view in `collapsed` left out.
std::vector<FlatTreeItem> flatten_tree(const View& root,
                                       const std::unordered_set<const View*>& collapsed);

/// The row under `y` in the panel's tree, or nullptr when it falls outside
/// the list. `y` is measured from the top of the first row.
const FlatTreeItem* tree_item_at(const std::vector<FlatTreeItem>& items, float y, float row_height);

/// True when `p` falls in the panel docked at the right edge of a root
/// `root_width` wide. The bare manipulate layer has no panel, so the whole
/// canvas stays live.
bool point_in_panel(Point p, float root_width, float panel_width, bool manipulate_only);

} // namespace pulp::vellum_components::overlay
