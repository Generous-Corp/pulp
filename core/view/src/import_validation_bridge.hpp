#pragma once

// Import / validation diagnostic helpers.
//
// These are pure functions that operate only on the public View API and the
// ClaudeRuntimeOptions value type. They were extracted verbatim from
// widget_bridge.cpp (make_layout_rect_value / make_layout_ancestor_chain_value)
// and claude_bundle.cpp (layout_runtime_snapshot_root_if_requested) so the two
// translation units can share one definition without copy/paste drift. No
// behavior, ordering, or contract changes were made during the extraction (#3151).

#include <choc/containers/choc_Value.h>

#include <pulp/view/design_sources.hpp>  // ClaudeRuntimeOptions
#include <pulp/view/view.hpp>            // View

namespace pulp::view {

// Compute the presentation-space bounding client rectangle for a view.  The
// returned AABB includes the complete ancestor transform/scroll chain and is in
// root viewport coordinates, matching DOM getBoundingClientRect().
choc::value::Value make_layout_rect_value(View* v);

// Compute the untransformed local CSS box metrics for a view. localX/localY are
// the border-box origin in the native parent's coordinate space, offset* is
// the border box, client* is the padding box, and border*Width exposes each
// effective edge for translating browser border-box evidence into Yoga's
// padding-edge-relative absolute coordinate space.
// Keeping this separate from make_layout_rect_value is essential for DOM code
// that maps viewport-space pointer coordinates into an authored canvas.
choc::value::Value make_layout_box_metrics_value(View* v);

// Build the ancestor chain (root → view) as a choc array of {id, bounds} entries,
// skipping ancestors without an anchor/id. Used by the getLayoutAncestorRects
// diagnostic trace.
choc::value::Value make_layout_ancestor_chain_value(View* v);

// If a runtime-snapshot viewport is requested in opts, resize the root view to
// that viewport and re-run layout. No-op when the viewport is unset/non-positive.
void layout_runtime_snapshot_root_if_requested(View& root, const ClaudeRuntimeOptions& opts);

}  // namespace pulp::view
