// SceneNode — abstract base of Pulp's retained vector scene graph.
//
// Item 6.1 of `planning/2026-05-24-macos-plugin-authoring-plan.md`.
// License-lineage note: the Pulp scene-graph primitives use Pulp-native
// names (`VectorScene`, `SceneNode`, `ScenePath`, `SceneShape`,
// `SceneImage`, `SceneText`, `SceneGroup`) rather than any reference
// framework's class names. The design is derived from Skia's
// SVG rendering literature plus what custom-paint widgets actually need
// (mutate a sub-tree → re-paint only its bounding box), not from any
// single C++ framework's headers.
#pragma once

#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <string>

namespace pulp::canvas {

class SceneGroup;  // fwd-decl for parent pointer

/// Axis-aligned bounding box used for repaint-rect math.
///
/// Local-coordinate bounds are reported by `local_bounds()`; the
/// scene walker composes a node's transform onto its parent to derive
/// "scene" bounds when the host asks for the bounding box that needs
/// to be repainted after a mutation.
struct SceneRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }

    /// Union with `other`, treating empty rects as "no contribution".
    constexpr SceneRect united(const SceneRect& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        const float minx = x < other.x ? x : other.x;
        const float miny = y < other.y ? y : other.y;
        const float maxx = (x + w) > (other.x + other.w) ? (x + w) : (other.x + other.w);
        const float maxy = (y + h) > (other.y + other.h) ? (y + h) : (other.y + other.h);
        return {minx, miny, maxx - minx, maxy - miny};
    }

    constexpr bool operator==(const SceneRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    constexpr bool operator!=(const SceneRect& o) const { return !(*this == o); }
};

/// 2D affine transform (CanvasRenderingContext2D layout):
///   [ a c e ]
///   [ b d f ]
///   [ 0 0 1 ]
struct SceneTransform {
    float a = 1.0f, b = 0.0f;
    float c = 0.0f, d = 1.0f;
    float e = 0.0f, f = 0.0f;

    static constexpr SceneTransform identity() { return {}; }
    static constexpr SceneTransform translation(float tx, float ty) {
        return {1, 0, 0, 1, tx, ty};
    }

    constexpr bool is_identity() const {
        return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && e == 0.0f && f == 0.0f;
    }

    /// Multiply (this * rhs). Used by the walker when composing a child's
    /// transform onto its parent's CTM during `paint()` and `paint_bounds()`.
    constexpr SceneTransform composed_with(const SceneTransform& r) const {
        return {
            a * r.a + c * r.b,
            b * r.a + d * r.b,
            a * r.c + c * r.d,
            b * r.c + d * r.d,
            a * r.e + c * r.f + e,
            b * r.e + d * r.f + f,
        };
    }

    /// Transform an axis-aligned rect — returns the AABB of the four
    /// transformed corners. Used by the dirty-rect walker so the
    /// reported "sub-tree bounding box" is in scene coordinates.
    SceneRect map_rect(const SceneRect& r) const;
};

/// Kind discriminator — keeps `dynamic_cast` off the hot path.
/// New concrete nodes append below; switch statements use a default arm.
enum class SceneNodeKind {
    group,
    path,
    shape,
    image,
    text,
};

/// Abstract base of every node in a `VectorScene`.
///
/// Mutation contract: any setter that changes the node's *visual* state
/// (transform, opacity, paint, geometry) MUST call `mark_dirty()` before
/// returning. The walker reads `dirty()` + `last_painted_bounds()` to
/// compute the union of "what needs re-painting" on the next frame, then
/// calls `clear_dirty()` after the paint is recorded.
class SceneNode {
public:
    explicit SceneNode(SceneNodeKind kind) : kind_(kind) {}
    virtual ~SceneNode() = default;

    SceneNode(const SceneNode&) = delete;
    SceneNode& operator=(const SceneNode&) = delete;

    SceneNodeKind kind() const { return kind_; }

    // ── Identity (debugging / SVG `<g id="...">` round-trip) ───────────
    const std::string& id() const { return id_; }
    void set_id(std::string v) { id_ = std::move(v); /* identity-only, no dirty */ }

    // ── Visibility ───────────────────────────────────────────────────────
    bool visible() const { return visible_; }
    void set_visible(bool v) {
        if (visible_ == v) return;
        visible_ = v;
        mark_dirty();
    }

    // ── Opacity ──────────────────────────────────────────────────────────
    float opacity() const { return opacity_; }
    void set_opacity(float a) {
        if (opacity_ == a) return;
        opacity_ = a;
        mark_dirty();
    }

    // ── Transform ────────────────────────────────────────────────────────
    const SceneTransform& transform() const { return transform_; }
    void set_transform(const SceneTransform& t) {
        if (transform_.a == t.a && transform_.b == t.b &&
            transform_.c == t.c && transform_.d == t.d &&
            transform_.e == t.e && transform_.f == t.f) return;
        transform_ = t;
        mark_dirty();
    }

    // ── Parent linkage (set by SceneGroup) ───────────────────────────────
    SceneGroup* parent() const { return parent_; }

    // ── Bounds ───────────────────────────────────────────────────────────
    /// Local-coordinate bounding box (pre-transform, pre-opacity).
    /// Subclasses MUST compute this from current geometry.
    virtual SceneRect local_bounds() const = 0;

    /// Bounds after the node's own transform — used for parent group unions.
    SceneRect transformed_local_bounds() const {
        return transform_.map_rect(local_bounds());
    }

    // ── Paint ────────────────────────────────────────────────────────────
    /// Paint this node (and its sub-tree) into `canvas`. The walker calls
    /// `save()` / `restore()` around this call, applies the composed CTM,
    /// and pushes opacity if `opacity_ < 1.0`. Implementations therefore
    /// emit geometry/state only — they MUST NOT touch save/restore.
    virtual void paint_geometry(Canvas& canvas) const = 0;

    // ── Dirty tracking ───────────────────────────────────────────────────
    bool dirty() const { return dirty_; }

    /// Mark this node and (recursively) every ancestor dirty.
    /// Returning the current node's `last_painted_bounds()` union with the
    /// new `transformed_local_bounds()` lets `VectorScene::take_dirty_rect()`
    /// report the exact rect that needs to be re-painted.
    void mark_dirty();

    /// Called by the scene walker after a full paint. Captures the
    /// bounding box that was painted (in scene coordinates) so the next
    /// mutation knows what to invalidate.
    void mark_clean(const SceneRect& painted_scene_bounds) {
        dirty_ = false;
        last_painted_bounds_ = painted_scene_bounds;
    }

    /// The scene-space rect this node occupied at the last paint.
    /// Always united with the post-mutation bounds to derive the
    /// "what needs repainting" rect — this is the whole point of the
    /// retained scene graph: small mutations → small repaint rects.
    const SceneRect& last_painted_bounds() const { return last_painted_bounds_; }

protected:
    friend class SceneGroup;  // sets parent_ during add_child()

private:
    SceneNodeKind kind_;
    std::string id_;
    SceneGroup* parent_ = nullptr;
    SceneTransform transform_{};
    float opacity_ = 1.0f;
    bool visible_ = true;
    bool dirty_ = true;
    SceneRect last_painted_bounds_{};
};

}  // namespace pulp::canvas
