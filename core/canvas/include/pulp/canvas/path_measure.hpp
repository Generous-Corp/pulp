#pragma once

// PathMeasure — arc length over a `Path`, measured once and queried forever.
//
// A `Path` knows its verbs; it does not know how long it is. Everything that
// walks a path by DISTANCE rather than by verb — a knob's value arc, a ring
// meter filling to 0.7, an envelope or waveform revealing left to right, a
// label riding a curve — needs arc length, and each of those callers would
// otherwise re-derive it, differently and wrongly.
//
//   PathMeasure m{ring};                       // once, when the ring changes
//   Path filled = m.segment(0.0f, m.length() * value);   // every frame
//
// ── Why this is a value, not a method on Path ────────────────────────────
// Measuring is the expensive half: every quad and cubic is flattened until it
// is flat to `tolerance`, and the cumulative distance table is built. Querying
// is a binary search over that table. Collapsing the two into
// `Path::point_at()` would hide a full re-flatten inside something that reads
// like an accessor, and a per-frame caller would pay it every frame. So the
// expensive intermediate is the public value — the same shape as
// `TextShaper::prepare()` returning `PreparedText`, and for the same reason.
//
// A PathMeasure holds a COPY of the path's geometry, so it stays valid if the
// source `Path` is mutated or destroyed. Copying a Path is O(1), so this is
// cheap.
//
// ── Accuracy ─────────────────────────────────────────────────────────────
// `tolerance` is the maximum distance (in path units) between a flattened
// chord and the true curve. Lengths are therefore slight UNDER-estimates of
// THE PATH'S OWN arc length, converging as tolerance falls. The default of
// 0.25 keeps that error far below a pixel at ordinary widget scale.
//
// "The path's own" is the part worth reading twice. A PathMeasure measures the
// geometry the Path actually holds — never the ideal shape that geometry was
// built to approximate. `Path::add_circle` flattens to four cubics at build
// time (see path.hpp: the verb set is closed, with no conic), and those cubics
// are very slightly longer than the circle they stand for. Measuring a
// radius-100 circle therefore converges to 628.405, not 2*pi*100 = 628.319 —
// about +0.014%, and it converges from BELOW, crossing the true circumference
// once the tolerance is fine enough. Code that needs the analytic
// circumference of a circle should compute it, not measure it.
//
// `segment()` is exact in a different sense: it splits the bracketing curve
// with de Casteljau, so a trimmed cubic comes back as a cubic, not as a
// polyline. Only the DISTANCE at which the split happens carries the
// flattening error; the curve itself is not degraded.
//
// ── Threading ────────────────────────────────────────────────────────────
// Standard value-type contract: concurrent reads of one PathMeasure are safe;
// concurrent mutation is not. All query methods are const.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <pulp/canvas/path.hpp>

namespace pulp::canvas {

/// A position on a measured path, with the direction of travel there.
struct PathPose {
    Point2D position{};
    /// Unit tangent. Points along increasing distance. For a degenerate
    /// (zero-length) path this is `{1, 0}` rather than a zero vector, so
    /// callers rotating by it never produce a NaN basis.
    Point2D tangent{1.0f, 0.0f};
};

class PathMeasure {
  public:
    /// The default flattening tolerance, in path units.
    static constexpr float default_tolerance = 0.25f;

    PathMeasure() = default;

    /// Measure `path`. `tolerance` is clamped to a small positive minimum, so
    /// a caller passing 0 gets the finest supported flattening rather than a
    /// hang.
    explicit PathMeasure(const Path& path, float tolerance = default_tolerance);

    /// Total length of every contour summed. 0 for an empty path.
    float length() const {
        return total_length_;
    }

    bool is_empty() const {
        return contours_.empty();
    }

    /// Contours are the `move_to`-delimited subpaths, in path order. A path
    /// with no length (a lone `move_to`, or repeated identical points) yields
    /// no contour, so every contour reported here has positive length.
    size_t contour_count() const {
        return contours_.size();
    }

    float contour_length(size_t index) const;

    /// Whether contour `index` was closed with `close()`.
    bool contour_is_closed(size_t index) const;

    /// Distance at which contour `index` begins, measured from the start of
    /// the path. `contour_start(0)` is always 0.
    float contour_start(size_t index) const;

    /// Position and unit tangent at `distance` along the path.
    ///
    /// `distance` is CLAMPED to [0, length()], so an out-of-range value
    /// returns the nearest endpoint rather than nothing — a meter driven past
    /// its maximum should pin, not vanish. `std::nullopt` means the path has
    /// no length at all, which is the one case a caller must actually branch
    /// on.
    std::optional<PathPose> pose_at(float distance) const;

    /// `pose_at(distance)->position`, for callers that do not need the tangent.
    std::optional<Point2D> position_at(float distance) const;

    /// `pose_at(distance)->tangent`, for callers that do not need the position.
    std::optional<Point2D> tangent_at(float distance) const;

    /// The part of the path between two distances, as a new Path.
    ///
    /// This is the trim behind a value arc or a reveal. Both distances are
    /// clamped to [0, length()]; if `start >= end` after clamping the result
    /// is an empty Path. Curves are split with de Casteljau, so the result
    /// keeps the source's curve verbs rather than degrading to line segments.
    ///
    /// A closed contour that is trimmed is no longer closed: the result is the
    /// open run between the two distances, with no closing segment.
    Path segment(float start, float end) const;

  private:
    /// One flattened sample. `segment_index` and `t` locate the sample on the
    /// ORIGINAL curve, which is what lets `segment()` split a real curve
    /// rather than stitch chords.
    struct Sample {
        float distance = 0.0f; ///< cumulative, from the start of the path
        Point2D position{};
        uint32_t segment_index = 0;
        float t = 0.0f; ///< parameter within that segment, in [0, 1]
    };

    /// One curve or line between two on-path points. `count` points are used:
    /// 2 for a line (start, end), 3 for a quad, 4 for a cubic.
    struct Segment {
        Point2D points[4]{};
        uint8_t count = 2;
        uint32_t contour_index = 0;
    };

    struct Contour {
        size_t first_sample = 0;
        size_t sample_count = 0;
        size_t first_segment = 0;
        size_t segment_count = 0;
        float start_distance = 0.0f;
        float length = 0.0f;
        bool closed = false;
    };

    std::vector<Sample> samples_;
    std::vector<Segment> segments_;
    std::vector<Contour> contours_;
    float total_length_ = 0.0f;
    float tolerance_ = default_tolerance;

    /// Index of the last sample whose distance is <= `distance`.
    size_t sample_index_for(float distance) const;
};

} // namespace pulp::canvas
