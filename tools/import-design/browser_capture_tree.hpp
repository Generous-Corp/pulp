// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_styles.hpp"

#include <pulp/view/design_ir.hpp>

#include <string>
#include <vector>

namespace pulp::import_design {

/// How a painted node reaches the screen.
///
/// The distinction is the whole point of whole-tree lowering: a panel is only
/// "drawn" to the extent its nodes are `native`, and the two other classes are
/// the honest accounting of what still arrives as pixels.
enum class PaintClass {
    /// Drawn from computed appearance — fills, borders, effects, text.
    native,
    /// Needs a decoded raster the IR references by asset, not a page capture:
    /// an `<img>`, or a `url()` background.
    image_asset,
    /// Cannot be described by style at all, so the element itself is captured.
    /// A `<canvas>` is imperative JS drawing with no styles to reproduce; this
    /// is the correct permanent answer for it, not a stopgap.
    element_capture_fallback,
};

std::string_view to_string(PaintClass paint_class);

/// What whole-tree lowering found, so a panel that quietly stops being drawn is
/// a number rather than a picture nobody can attribute.
struct PaintedTreeCounts {
    int painted = 0;               ///< layout nodes Chrome reported
    int lowered = 0;               ///< IR nodes emitted
    int native = 0;
    int image_asset = 0;
    int element_capture_fallback = 0;
    /// Area, in CSS px², of the fallback nodes that reach the screen carrying
    /// NO raster — so the renderer draws nothing where the browser drew
    /// something. A count cannot say how bad that is in either direction:
    /// eighteen `<svg>` icons are 0.4% of a panel, while two full-window
    /// `<canvas>` elements are the whole panel twice over and arrive as the
    /// same "2". The area is the number that separates them.
    double unpainted_fallback_area = 0.0;
    int text = 0;                  ///< of the lowered, how many carry a string
    int pooled_into_fallback = 0;  ///< descendants of a captured element
    /// Rotated elements drawn as a rectangle plus an angle instead of being
    /// refused as unpaintable. Counted so the census can tell "this design has
    /// no rotations" apart from "its rotations were solved" — the two produce
    /// the same zero in `element_capture_fallback` and mean different things
    /// about how much of the design the renderer is actually reproducing.
    int rotation_recovered = 0;
    /// `<svg>` elements whose whole shape tree became vector nodes.
    int svg_lowered = 0;
    /// `<svg>` elements that still arrive as a captured element. Each carries
    /// `capture_fallback_reason` naming the construct that refused, so the
    /// residual is a list rather than a total.
    int svg_refused = 0;
    /// Of those, the ones refused because the CAPTURE carries no SVG paint —
    /// a stale snapshot, not a property of the design. Counted separately
    /// because it is the only SVG refusal a caller can fix, and the fix is
    /// "capture again", which nothing about the design tells them.
    int svg_refused_stale_capture = 0;

    /// Text runs that carry captured line boxes but no resolved FACE.
    ///
    /// The renderer refuses such a basis on purpose — a font family is a
    /// request, and without the face the capture broke against there is no way
    /// to know the cache still describes this machine's text. So every one of
    /// these runs silently re-derives its own line breaking, and a run that
    /// resumes mid-line after an inline `<span>` loses the horizontal offset
    /// that placed it, printing on top of its own sibling.
    ///
    /// Same shape as the SVG case and the same one-line fix — capture again —
    /// which nothing about the design tells a reader.
    int text_line_boxes_without_face = 0;
    /// Vector nodes emitted from those shape trees. Counted separately from
    /// `native` so "the panel draws more nodes" cannot be read as "the panel
    /// draws more of the design" when the extra nodes are all one icon.
    int svg_shapes = 0;
    int skipped_empty_box = 0;     ///< zero-area layout objects
    int skipped_blank_text = 0;    ///< collapsed whitespace runs
    int skipped_non_visual = 0;    ///< the document node, doctype, comments
    int missing_paint_order = 0;   ///< layout objects Chrome did not rank
    int root_children = 0;         ///< direct children lowering added to root
    int max_depth = 0;             ///< deepest lowered node, root children = 1
    /// Nodes attached above their DOM parent because they paint BEFORE it —
    /// a negative-`z-index` child or a descendant that escaped to an outer
    /// stacking context. A nested painter draws a parent's own box before any
    /// descendant, so that order is unrepresentable in place; the node is
    /// hoisted and flagged rather than silently drawn in the wrong order.
    int hoisted_escapes = 0;
    /// Nodes the emitted tree clips that CSS would not, because the tree
    /// applies `overflow` by DOM parentage while CSS applies it along the
    /// containing-block chain. An absolutely positioned node whose containing
    /// block sits ABOVE an `overflow: hidden` ancestor escapes that ancestor's
    /// clip in a browser; nested under it here, it is clipped — and where the
    /// boxes do not intersect, it disappears entirely.
    int clip_over_applied = 0;
    /// Nodes that lose a clip CSS would apply, because the ancestor carrying it
    /// is no longer above them in the emitted tree — a hoisted node regrafted
    /// past its clipping parent, which then paints outside the box that
    /// contained it in the browser.
    int clip_lost = 0;
    /// Composed-order inversions against Chrome that are actually VISIBLE:
    /// a pair whose relative paint order differs from Chrome's AND whose boxes
    /// overlap. Re-expressing a flat paint order hierarchically reorders
    /// disjoint boxes freely — that is unobservable — so this is the honest
    /// measure of whether nesting cost any fidelity. Zero is the claim.
    int overlapping_reorders = 0;
    /// Which pairs inverted, as `over<under` anchor ids, in composed order.
    /// A count says a panel can paint wrong; only the pair says where, and the
    /// pairing has to be captured here because the audit runs after the nodes
    /// have been moved into the emitted tree and can no longer be written to.
    std::vector<std::string> overlapping_reorder_pairs;
    /// Nodes whose type lengths were multiplied by the scale their box already
    /// carried from an ancestor `transform`. Chrome scales glyphs along with
    /// the box; the snapshot reports the box post-transform and `font-size`
    /// pre-transform, so without this a run under `scale(.9)` draws 11% wide.
    int type_scaled = 0;
    /// Nodes under a transform chain that does not reduce to one positive
    /// uniform scale — two axes, a flip, a rotation, or a `matrix3d`. Type
    /// carries a single scalar and cannot express any of those, so the factor
    /// is NOT applied and the node records the value that refused it. A
    /// recorded refusal is findable; a plausible wrong number is not.
    int type_scale_refused = 0;
};

/// Lower every painted node in the captured document into a TREE under `root`
/// that mirrors the captured DOM.
///
/// Two invariants, and they are different things:
///
///   * **Yoga must not re-solve.** Every lowered node is `position: absolute`
///     with its width, height, and offsets taken from the box Chrome already
///     solved. Yoga takes an absolutely positioned child out of flow, so it
///     performs offset arithmetic and no flex resolution — a Yoga-versus-Blink
///     divergence remains impossible by construction. That is the invariant.
///   * **The tree must be editable.** Structure is NOT the same invariant, and
///     flattening it costs the whole editing surface: two `div.face` nodes with
///     no parent cannot be told apart, and there is no group to grab. So DOM
///     parentage, ids, and class-derived names are preserved, and a child's box
///     is stored RELATIVE to its parent. Moving a container then moves its
///     children by construction rather than by special case.
///
/// Ordering follows Chrome's own paint model, which is hierarchical: within a
/// stacking context, siblings are emitted in Chrome's paint order and carry it
/// as `z-index`. A node that paints before its own parent cannot be expressed
/// in place at all, so it is hoisted and counted in `hoisted_escapes`.
///
/// `dx`/`dy` shift page coordinates into the cropped panel's frame, matching
/// the offset the semantic controls are placed with. They define the root's own
/// page origin, so a root child's relative box is its page box plus the shift
/// and the composition of offsets down any chain returns Chrome's absolute box
/// exactly — Blink's 1/64px grid values survive the arithmetic unrounded.
PaintedTreeCounts lower_painted_tree(const CapturedStyleIndex& index,
                                     double dx,
                                     double dy,
                                     pulp::view::IRNode& root);

}  // namespace pulp::import_design
