#pragma once

// Path — a retained 2D vector path VALUE type.
//
// A path is fundamentally a value: you build an icon once (in a constructor,
// or from an SVG string at load time), store it as a member, transform copies
// of it, and fill it every frame. The immediate-mode builder on `Canvas`
// (`begin_path` / `move_to` / ... / `fill_current_path`) cannot express that —
// it streams geometry into the backend and forgets it. `Path` is the retained
// half of the pair.
//
//   Path icon = Path::from_svg_string("M0 0 L10 0 L5 8 Z");   // once
//   ...
//   void paint(Canvas& c) {                                    // every frame
//       c.set_fill_color(colour);
//       c.fill_path(icon, FillRule::nonzero);
//   }
//
// ── Copy cost ────────────────────────────────────────────────────────────
// Copying a Path is O(1): the geometry sits behind a copy-on-write buffer, so
// a copy is a refcount bump, not a memcpy. Mutating a shared Path detaches it
// first (O(n), once). This is deliberate — if copying were O(n) every caller
// would hoist paths out of `paint()` and the value semantics would be a lie.
// `test_canvas_path.cpp` pins this with a benchmark.
//
// ── Curve representation ─────────────────────────────────────────────────
// Every curve is stored as a line, quadratic, or cubic. Arcs, ellipses, and
// SVG elliptical-arc segments are flattened to cubics AT BUILD TIME, so the
// verb set is closed at five and every backend (Skia, CoreGraphics, the
// recording target) replays byte-identical geometry. There is no conic verb
// and no backend-specific escape hatch.
//
// ── Threading ────────────────────────────────────────────────────────────
// Standard value-type contract, same as std::vector: concurrent reads of one
// Path are safe; concurrent mutation of the SAME Path object is not. Copies
// handed to other threads are independent even though they share a buffer —
// the copy-on-write detach makes the sharing invisible.

#include <pulp/canvas/affine_transform.hpp>
#include <pulp/canvas/rectangle_list.hpp>  // pulp::canvas::Rect2D

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulp::canvas {

/// Winding rule used to decide what counts as "inside" a path.
/// Declared here (rather than pulled from canvas.hpp) so `path.hpp` stays
/// independent of the 1500-line Canvas header. `canvas.hpp` includes this
/// file, so the two names are the same type everywhere.
enum class FillRule { nonzero, evenodd };

/// Line cap applied to the ends of open subpaths when stroking.
enum class LineCap { butt, round, square };

/// Join applied where two segments meet when stroking.
enum class LineJoin { miter, round, bevel };

/// Everything needed to stroke a path, as one value. Bundling these means
/// `stroke_path(path, style)` is a single call that cannot be half-configured
/// — the immediate-mode API's set_line_width / set_line_cap / set_line_join
/// triplet leaks state between draws and is a recurring source of "why is this
/// stroke the wrong width" bugs.
struct StrokeStyle {
    float width = 1.0f;
    LineCap cap = LineCap::butt;
    LineJoin join = LineJoin::miter;
    float miter_limit = 4.0f;

    /// Dash pattern: alternating on/off lengths in user units. Empty = solid.
    /// An odd-length pattern is repeated to make it even, per the SVG and
    /// Canvas2D convention ([5] behaves as [5, 5]).
    std::vector<float> dash;
    float dash_phase = 0.0f;

    bool operator==(const StrokeStyle& o) const {
        return width == o.width && cap == o.cap && join == o.join &&
               miter_limit == o.miter_limit && dash == o.dash &&
               dash_phase == o.dash_phase;
    }
    bool operator!=(const StrokeStyle& o) const { return !(*this == o); }
};

class Path {
public:
    /// The closed verb set. Arcs and ellipses flatten to `cubic` at build time.
    enum class Verb : uint8_t {
        move,   ///< 1 point: the new subpath's start
        line,   ///< 1 point: the endpoint
        quad,   ///< 2 points: control, endpoint
        cubic,  ///< 3 points: control 1, control 2, endpoint
        close,  ///< 0 points: close the current subpath
    };

    /// How many points a verb consumes.
    static constexpr int points_for(Verb v) {
        switch (v) {
            case Verb::move:  return 1;
            case Verb::line:  return 1;
            case Verb::quad:  return 2;
            case Verb::cubic: return 3;
            case Verb::close: return 0;
        }
        return 0;
    }

    /// One step of an iteration over the path.
    struct Element {
        Verb verb = Verb::move;
        const Point2D* points = nullptr;  ///< `count` points, or null for `close`
        int count = 0;
    };

    Path() = default;

    // Value semantics. Copy is O(1) (copy-on-write); see the header comment.
    Path(const Path&) = default;
    Path& operator=(const Path&) = default;
    Path(Path&&) noexcept = default;
    Path& operator=(Path&&) noexcept = default;
    ~Path() = default;

    /// Geometric equality: same verbs, same points, in the same order. Two
    /// paths that enclose the same area by different routes are NOT equal.
    bool operator==(const Path& o) const;
    bool operator!=(const Path& o) const { return !(*this == o); }

    // ── Building ─────────────────────────────────────────────────────────
    Path& move_to(float x, float y);
    Path& line_to(float x, float y);
    Path& quad_to(float cx, float cy, float x, float y);
    Path& cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y);

    /// Tangent arc, matching Canvas2D `ctx.arcTo(x1, y1, x2, y2, radius)`:
    /// the arc of `radius` tangent to both the line (current → P1) and the
    /// line (P1 → P2), preceded by a line from the current point to the first
    /// tangent point. Degenerate input (no current point, zero radius,
    /// collinear points) falls back to a straight `line_to(x1, y1)`, which is
    /// what the Canvas2D spec requires.
    Path& arc_to(float x1, float y1, float x2, float y2, float radius);

    Path& close();

    /// Drop every verb. Keeps any allocated capacity for reuse.
    void clear();

    // ── Shape helpers ────────────────────────────────────────────────────
    Path& add_rect(float x, float y, float w, float h);
    Path& add_rect(const Rect2D& r) { return add_rect(r.x, r.y, r.width, r.height); }

    /// Uniform corner radius, clamped to half the shorter side.
    Path& add_rounded_rect(float x, float y, float w, float h, float radius);

    /// Per-corner radii, clockwise from the top-left. Each is clamped so that
    /// adjacent radii on one side never sum to more than that side's length —
    /// the CSS `border-radius` overlap rule.
    Path& add_rounded_rect(float x, float y, float w, float h,
                           float top_left, float top_right,
                           float bottom_right, float bottom_left);

    Path& add_ellipse(float cx, float cy, float rx, float ry);
    Path& add_circle(float cx, float cy, float radius) {
        return add_ellipse(cx, cy, radius, radius);
    }

    /// Arc as its own subpath (an implicit `move_to` to the arc's start).
    /// Angles in radians, measured clockwise from the +x axis in Pulp's
    /// y-down coordinate space. `sweep` may be negative (counter-clockwise).
    Path& add_arc(float cx, float cy, float rx, float ry,
                  float start_angle, float sweep_angle);

    /// A pie slice / wedge: center → arc → back to center, closed.
    Path& add_pie(float cx, float cy, float rx, float ry,
                  float start_angle, float sweep_angle);

    /// Closed polygon through `count` points. Fewer than 2 points draws nothing.
    Path& add_polygon(const Point2D* points, size_t count);

    /// Append `other`, optionally transformed. Subpaths stay separate — this
    /// never joins `other`'s first point to this path's current point.
    Path& add_path(const Path& other,
                   const AffineTransform& transform = AffineTransform::identity());

    // ── Transforms ───────────────────────────────────────────────────────
    /// Transform every point in place.
    void apply_transform(const AffineTransform& t);

    /// A transformed copy; leaves this path alone.
    Path transformed(const AffineTransform& t) const;

    /// The transform that would scale this path to fit `(x, y, w, h)`.
    ///
    /// With `preserve_proportions`, the path is scaled uniformly by
    /// `min(w / bounds.width, h / bounds.height)` and then **centerd** on both
    /// axes within the target rect — so the slack on the axis that did not
    /// bind is split evenly, not dumped at one edge. (Derived empirically; see
    /// `test_canvas_path.cpp`, which pins the centring with a case whose
    /// scaled height is half the target's.)
    ///
    /// Returns identity — a deliberate no-op — when the path's bounds are
    /// degenerate on either axis (a horizontal or vertical line, a single
    /// point, an empty path) or when the target rect is zero-sized. Scaling a
    /// zero-width path to a non-zero width is a division by zero; the guard is
    /// why `scale_to_fit` on a vertical line yields an unchanged path rather
    /// than one whose coordinates are all NaN.
    AffineTransform transform_to_fit(float x, float y, float w, float h,
                                     bool preserve_proportions) const;

    /// `apply_transform(transform_to_fit(...))`.
    void scale_to_fit(float x, float y, float w, float h,
                      bool preserve_proportions);

    // ── Queries ──────────────────────────────────────────────────────────
    bool is_empty() const { return !data_ || data_->verbs.empty(); }

    /// Number of verbs.
    size_t verb_count() const { return data_ ? data_->verbs.size() : 0; }

    /// TIGHT bounds: the true visual bounding box, computed from each curve's
    /// extrema (the roots of its derivative), not from its control hull.
    ///
    /// This differs from `control_bounds()` for any curve whose control points
    /// fall outside the curve — which is most of them. Tight bounds are what
    /// `scale_to_fit` and hit-testing need: fitting by hull bounds would leave
    /// the shape mysteriously inset from the box it was asked to fill.
    /// Cached; invalidated on mutation.
    Rect2D bounds() const;

    /// Cheap conservative bounds: the AABB of the control points. Never
    /// smaller than `bounds()`, sometimes larger, and computed without solving
    /// anything. This is the right choice for a repaint rect (where
    /// over-estimating costs a few pixels of redraw and under-estimating is a
    /// rendering artifact) and the wrong choice for anything that must be
    /// exact.
    Rect2D control_bounds() const;

    /// Is `p` inside the path under `rule`? Open subpaths are treated as
    /// closed, matching how they fill.
    bool contains(Point2D p, FillRule rule = FillRule::nonzero) const;
    bool contains(float x, float y, FillRule rule = FillRule::nonzero) const {
        return contains(Point2D{x, y}, rule);
    }

    /// The last point of the path, or nullopt if there is no current point.
    std::optional<Point2D> current_point() const;

    // ── Iteration ────────────────────────────────────────────────────────
    /// Forward iterator over the verbs. Dereferencing yields an `Element`
    /// pointing INTO the path's buffer — it is valid only until the path is
    /// mutated.
    class Iterator {
    public:
        Iterator() = default;
        Iterator(const Path* path, size_t verb_index, size_t point_index)
            : path_(path), verb_(verb_index), point_(point_index) {}

        Element operator*() const;
        Iterator& operator++();
        bool operator==(const Iterator& o) const { return verb_ == o.verb_ && path_ == o.path_; }
        bool operator!=(const Iterator& o) const { return !(*this == o); }

    private:
        const Path* path_ = nullptr;
        size_t verb_ = 0;
        size_t point_ = 0;
    };

    Iterator begin() const { return Iterator(this, 0, 0); }
    Iterator end() const { return Iterator(this, verb_count(), 0); }

    /// Replay this path into any sink that exposes the Canvas path-builder
    /// verbs. Used by every backend and by `ScenePath`; also the simplest way
    /// for a caller to walk a path without touching the iterator.
    template <typename Sink>
    void replay(Sink& sink) const {
        for (Element el : *this) {
            switch (el.verb) {
                case Verb::move:
                    sink.move_to(el.points[0].x, el.points[0].y);
                    break;
                case Verb::line:
                    sink.line_to(el.points[0].x, el.points[0].y);
                    break;
                case Verb::quad:
                    sink.quad_to(el.points[0].x, el.points[0].y,
                                 el.points[1].x, el.points[1].y);
                    break;
                case Verb::cubic:
                    sink.cubic_to(el.points[0].x, el.points[0].y,
                                  el.points[1].x, el.points[1].y,
                                  el.points[2].x, el.points[2].y);
                    break;
                case Verb::close:
                    sink.close_path();
                    break;
            }
        }
    }

    /// Flatten every curve to line segments, within `tolerance` user units of
    /// the true curve. Returns one point list per subpath, plus whether that
    /// subpath was explicitly closed.
    struct FlatSubpath {
        std::vector<Point2D> points;
        bool closed = false;
    };
    std::vector<FlatSubpath> flatten(float tolerance = 0.25f) const;

    // ── SVG ──────────────────────────────────────────────────────────────
    /// Parse an SVG path `d` string. Supports the full grammar:
    /// M/m L/l H/h V/v C/c S/s Q/q T/t A/a Z/z, with implicit repeated
    /// coordinate sets and elliptical arcs (converted to cubics).
    /// Malformed input yields whatever prefix parsed cleanly — never throws.
    static Path from_svg_string(const std::string& d);

    /// Emit a standard SVG `d` string using absolute commands (M/L/Q/C/Z).
    /// Round-trips through `from_svg_string`.
    std::string to_svg_string() const;

private:
    // Copy-on-write geometry buffer. Verbs and points are stored in parallel
    // arrays rather than as a fat 32-byte command struct: a `line_to` costs
    // 1 byte + 8 bytes here, versus 32 bytes in a tagged-union layout.
    struct Data {
        std::vector<Verb> verbs;
        std::vector<Point2D> points;
    };

    /// Detach for mutation (the copy in copy-on-write) and drop the bounds cache.
    Data& mutable_data();

    /// Append cubic segments approximating an elliptical arc, WITHOUT an
    /// initial move_to (the caller decides whether this continues a subpath or
    /// starts one). Every generated point is passed through `xf`, which is how
    /// a ROTATED ellipse is produced: generate on the axis-aligned ellipse and
    /// map through `rotation(phi, cx, cy)`.
    void append_arc_cubics(float cx, float cy, float rx, float ry,
                           float start_angle, float sweep_angle,
                           const AffineTransform& xf = AffineTransform::identity());

    /// The SVG `A`/`a` segment: an arc in ENDPOINT parameterization (radii,
    /// x-axis rotation, large-arc and sweep flags, and an endpoint) rather
    /// than the center parameterization every renderer actually wants.
    /// Converts to center form per the SVG spec's implementation notes, then
    /// emits cubics. Out-of-range radii are scaled up to reach the endpoint,
    /// exactly as the spec requires; zero radii degrade to a straight line.
    void append_svg_arc(Point2D from, float rx, float ry, float x_axis_rotation_deg,
                        bool large_arc, bool sweep, Point2D to);

    std::shared_ptr<Data> data_;

    // The bounds cache lives on the Path, not in the shared Data, so two
    // copies sharing one buffer can never race computing it. Copies inherit a
    // valid cache — the geometry is identical, after all.
    mutable bool bounds_valid_ = false;
    mutable Rect2D bounds_cache_{};

    friend class Iterator;
};

}  // namespace pulp::canvas
