// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_styles.hpp"

#include <pulp/view/design_ir.hpp>

#include <string>

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
    int text = 0;                  ///< of the lowered, how many carry a string
    int pooled_into_fallback = 0;  ///< descendants of a captured element
    int skipped_empty_box = 0;     ///< zero-area layout objects
    int skipped_blank_text = 0;    ///< collapsed whitespace runs
    int skipped_non_visual = 0;    ///< the document node, doctype, comments
    int missing_paint_order = 0;   ///< layout objects Chrome did not rank
};

/// Lower every painted node in the captured document into absolutely positioned
/// children of `root`, in Chrome's paint order.
///
/// The children are FLAT and each carries the absolute box Chrome solved. Yoga
/// therefore does nothing but place them, so a Yoga-versus-Blink layout
/// divergence cannot exist by construction — which is the entire argument for
/// this design over re-solving the page's layout natively.
///
/// `dx`/`dy` shift page coordinates into the cropped panel's frame, matching
/// the offset the semantic controls are placed with.
PaintedTreeCounts lower_painted_tree(const CapturedStyleIndex& index,
                                     double dx,
                                     double dy,
                                     pulp::view::IRNode& root);

}  // namespace pulp::import_design
