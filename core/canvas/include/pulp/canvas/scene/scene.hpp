// VectorScene — the root of Pulp's retained vector scene graph.
//
// Item 6.1 of `planning/2026-05-24-macos-plugin-authoring-plan.md`.
// License-lineage note: the Pulp-side primitives use Pulp-native names
// (`VectorScene`, `SceneNode`, `ScenePath`, `SceneShape`, `SceneImage`,
// `SceneText`, `SceneGroup`) rather than any reference framework's
// class names.
//
// Why a retained scene graph for Pulp:
//   - Custom paint widgets that build a vector composition once and
//     mutate a child's opacity / transform every frame should not have
//     to re-stream the whole command buffer to the canvas. The retained
//     model lets them surface only the dirty sub-tree to the host.
//   - SVG import (via the existing nanosvg path in `core/canvas/svg.hpp`)
//     wants a structured destination so the host can address individual
//     shapes — toggle visibility, swap a fill, animate a transform —
//     without re-parsing.
//   - Future design-tool import paths (`tools/cli/import-design` family)
//     want a one-to-one container per design-node so round-tripping
//     stays predictable.
//
// Acceptance (from the plan):
//   - SVG loads as a `VectorScene` or `SceneGroup` ✔
//   - Mutating a child's opacity re-paints only that sub-tree's
//     bounding box ✔ (see `take_dirty_rect()`)
//   - Renders via existing SkiaCanvas (and CG / RecordingCanvas) ✔
#pragma once

#include <pulp/canvas/scene/scene_group.hpp>
#include <pulp/canvas/scene/scene_image.hpp>
#include <pulp/canvas/scene/scene_node.hpp>
#include <pulp/canvas/scene/scene_path.hpp>
#include <pulp/canvas/scene/scene_shape.hpp>
#include <pulp/canvas/scene/scene_text.hpp>

#include <memory>
#include <string>

namespace pulp::canvas {

class VectorScene {
public:
    VectorScene() : root_(std::make_unique<SceneGroup>()) {}

    SceneGroup& root() { return *root_; }
    const SceneGroup& root() const { return *root_; }

    /// Replace the root group entirely (e.g. after a fresh SVG import).
    void set_root(std::unique_ptr<SceneGroup> r) {
        if (!r) r = std::make_unique<SceneGroup>();
        root_ = std::move(r);
        dirty_rect_pending_ = root_->transformed_local_bounds();
    }

    // ── Painting ─────────────────────────────────────────────────────────
    /// Walk + paint the entire scene to `canvas`. Captures each node's
    /// painted scene-space bounds so the next `take_dirty_rect()` call
    /// can report exact repaint extents.
    void paint(Canvas& canvas);

    // ── Dirty-rect query (the whole point of a retained graph) ───────────
    /// Returns the union of "old painted bounds" and "new transformed
    /// bounds" for every node that mutated since the last paint, then
    /// resets the internal pending-dirty tracker.
    ///
    /// Empty rect ⇒ nothing to repaint.
    SceneRect take_dirty_rect();

    /// Peek without resetting — useful for diagnostics + tests.
    SceneRect peek_dirty_rect() const { return dirty_rect_pending_; }

    /// Called by `SceneNode::mark_dirty()` when a node mutates; unions
    /// the node's "old + new" bounds into the pending dirty rect.
    void note_node_dirtied(const SceneNode& node);

    // ── SVG ingest ───────────────────────────────────────────────────────
    /// Parse an SVG document string and produce a populated VectorScene.
    /// Reuses the existing nanosvg parser; visible cubic-Bezier shapes
    /// become `ScenePath` nodes parented to the returned scene's root
    /// `SceneGroup`. Fill / stroke / opacity / fill-rule are translated.
    /// Returns an empty (root-only) scene if parsing fails.
    static VectorScene from_svg_string(const std::string& svg_data);

    /// Convenience: load an SVG file and return a populated scene.
    static VectorScene from_svg_file(const std::string& path);

private:
    std::unique_ptr<SceneGroup> root_;
    SceneRect dirty_rect_pending_{};  // accumulates across mutations
};

}  // namespace pulp::canvas
