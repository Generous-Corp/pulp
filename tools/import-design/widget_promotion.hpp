// SPDX-License-Identifier: MIT
//
// pulp #1814 (task #84) — post-parse widget promotion for the importer.
//
// External design tools (Figma, Stitch, v0, Pencil, Claude Design) rarely
// emit `<button>` directly. The common pattern is `<div onClick={...}>` or
// `<div role="button">` or just a styled div with `cursor: pointer`. Without
// post-parse promotion the importer treats those as static frames and the
// user's interactive intent is dropped — Pulp ends up with a wall of
// frames where the original design has clickable widgets.
//
// `promote_interactive_frames` walks the IR once after parse + before
// codegen and re-types any `frame` that carries a clickable signal to
// `button`. The pass is intentionally conservative (only frames; never
// downgrades) and source-agnostic so every importer benefits.

#pragma once

#include <cstddef>

namespace pulp::view {
struct IRNode;
}

namespace pulp::import_design {

/// Heuristic signals that a node is interactive. Exposed so the test
/// surface can assert what was matched without re-deriving the
/// classifier.
enum class WidgetPromotionSignal {
    none,
    onclick_attribute,   ///< `onclick=` / `onClick=` on the node
    aria_role_button,    ///< `role="button"` (ARIA semantic)
    cursor_pointer,      ///< `cursor: pointer` style (weakest signal)
};

/// Inspect a single node's attributes / style for a promotion signal.
/// Public for unit-test introspection; `promote_interactive_frames`
/// dispatches through this.
WidgetPromotionSignal classify_interactive_signal(const pulp::view::IRNode& node);

/// Walk the IR tree and promote any `type == "frame"` node carrying an
/// interactive signal to `type == "button"`. Returns the count of
/// promotions so the CLI can surface a "promoted N divs → buttons"
/// stat in its post-import report.
///
/// Order of operations: `promote_interactive_frames` MUST run AFTER
/// the parse path (so `attributes` / `style.cursor` are populated) and
/// BEFORE codegen (so the generated Pulp JS emits Button widgets
/// instead of static frames).
std::size_t promote_interactive_frames(pulp::view::IRNode& root);

}  // namespace pulp::import_design
