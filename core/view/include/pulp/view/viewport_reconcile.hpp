// pulp #1899 — viewport reconciliation for runtime-imported content.
//
// Header-only helper for clamping oversize absolute-positioned
// descendants to fit a viewport. Generic enough to live in core/view
// because both the headless screenshot tool and the live host (and
// any future live host running runtime-imported React trees) need
// the same reconciliation.
//
// Background: imports (Spectr, v0.dev, Stitch, Figma exports) routinely
// ship a top-level container with literal-CSS hardcoded dimensions that
// exceed the runtime viewport. Canonical Spectr case
// (`spectr-editor-extracted.js:4140`, originating in
// `dom-adapter.tsx:440-441` as a workaround for what dom-adapter
// perceived as a Yoga absolute-pin bug):
//   `<div style={{ position:'absolute', top:0, left:0,
//                  width:1320, height:860, … }}>`
// and the same hardcoded 1320×860 propagates several layers deep —
// App (depth 1), FilterBank wrap (depth 2), individual canvases
// (depth 3). In a 1280×800 viewport, anything `bottom:0`-anchored at
// any of those depths falls off the captured frame.
//
// In a real browser the same content renders inside the same viewport
// because the editor.html body uses
//   `display:flex; align-items:center; justify-content:center;
//    min-height:100vh`
// with `flex-shrink: 1` (default), so the App flex item shrinks on its
// main axis to fit the body's content box.
//
// This helper emulates that flex-shrink behaviour for runtime-import
// hosts. For any descendant of root_ with
// `position:absolute|fixed` AND a `preferred_width|height` exceeding
// the viewport AND no opposite-edge anchor (`right` for width,
// `bottom` for height — i.e. the source told us a concrete size, not
// "stretch me between two edges"), clamp the explicit size down to
// the viewport size on that axis. Descendants anchored at `bottom:0`
// / `right:0` then anchor to the visible edge instead of falling off.
//
// Scoped strictly to oversize-content + runtime-import usage; never
// fires when content already fits, never modifies anchored content.

#pragma once

#include <pulp/view/view.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace pulp::view {

inline void clamp_oversize_absolute_view(pulp::view::View& view,
                                          float vw, float vh,
                                          std::uint32_t viewport_width,
                                          std::uint32_t viewport_height,
                                          int depth,
                                          const char* parent_label) {
    using pulp::view::View;
    if (view.position() != View::Position::absolute &&
        view.position() != View::Position::fixed) {
        return;
    }
    const float cw = view.flex().preferred_width;
    const float ch = view.flex().preferred_height;
    if (cw <= 0 && ch <= 0) return;
    // Only act when the size is explicit (preferred_* set) AND the
    // opposite edge isn't anchored — i.e. the source said "size me
    // explicitly", not "stretch me from edge to edge".
    //
    // pulp #1906 (Codex P2) — distinguish `right:auto` from explicit
    // `right:0`. The previous predicate `(!has_right() || right==0)`
    // treated `auto` and `0` identically, so an oversize child pinned
    // with explicit `right:0` / `bottom:0` got force-clamped even
    // though the source explicitly anchored it to the opposite edge.
    // The correct test: only clamp when the opposite edge is truly
    // unset (`auto`). Any explicit value — including 0 — is the
    // source declaring edge-anchoring intent, which Yoga will honour
    // via the inset → size derivation; defer to that.
    const bool size_x_is_explicit = cw > 0 && !view.has_right();
    const bool size_y_is_explicit = ch > 0 && !view.has_bottom();
    bool clamped = false;
    if (cw > vw && size_x_is_explicit) {
        view.flex().preferred_width = vw;
        clamped = true;
    }
    if (ch > vh && size_y_is_explicit) {
        view.flex().preferred_height = vh;
        clamped = true;
    }
    if (clamped && std::getenv("PULP_DUMP_BOUNDS")) {
        std::fprintf(stderr,
                     "[viewport-reconcile] clamped oversize node depth=%d parent=%s: "
                     "(%.0fx%.0f) -> (%.0fx%.0f) in %ux%u viewport\n",
                     depth,
                     parent_label ? parent_label : "<root>",
                     cw, ch,
                     view.flex().preferred_width,
                     view.flex().preferred_height,
                     viewport_width, viewport_height);
    }
}

inline void walk_and_clamp(pulp::view::View& view,
                            float vw, float vh,
                            std::uint32_t viewport_width,
                            std::uint32_t viewport_height,
                            int depth,
                            const char* parent_label) {
    using pulp::view::View;
    clamp_oversize_absolute_view(view, vw, vh,
                                  viewport_width, viewport_height,
                                  depth, parent_label);
    char child_label[64];
    std::snprintf(child_label, sizeof(child_label), "depth%d", depth);
    for (std::size_t i = 0; i < view.child_count(); ++i) {
        auto* child = const_cast<View*>(view.child_at(i));
        if (!child) continue;
        walk_and_clamp(*child, vw, vh,
                       viewport_width, viewport_height,
                       depth + 1, child_label);
    }
}

inline void reconcile_oversize_absolute_subtree(pulp::view::View& root,
                                                 std::uint32_t viewport_width,
                                                 std::uint32_t viewport_height) {
    using pulp::view::View;
    const float vw = static_cast<float>(viewport_width);
    const float vh = static_cast<float>(viewport_height);
    // root_ itself is always relative; start the recursive walk at its
    // direct children (depth 1) and descend through every subtree node.
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        auto* child = const_cast<View*>(root.child_at(i));
        if (!child) continue;
        walk_and_clamp(*child, vw, vh,
                       viewport_width, viewport_height,
                       /*depth=*/1, /*parent_label=*/"root");
    }
}

} // namespace pulp::view
