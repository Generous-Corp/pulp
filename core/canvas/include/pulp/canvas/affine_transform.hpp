#pragma once

// AffineTransform — 2D affine transform value type.
//
// Lives in `pulp::canvas` because it is the lowest layer that needs it:
// `pulp::canvas::Path` transforms itself, and `core/view` links `core/canvas`
// (not the other way round), so `pulp::view` re-exports this type rather than
// declaring a second one. See the alias in core/view/include/pulp/view/geometry.hpp.
//
// Matrix layout matches the six floats every 2D graphics API on the planet
// agrees on (and the existing `Canvas::AffineTransform2x3` snapshot type):
//
//     [ a  c  e ]     x' = a*x + c*y + e
//     [ b  d  f ]     y' = b*x + d*y + f
//     [ 0  0  1 ]
//
// Identity is {1, 0, 0, 1, 0, 0}.

#include <pulp/canvas/rectangle_list.hpp>  // pulp::canvas::Rect2D

#include <cmath>
#include <optional>

namespace pulp::canvas {

/// A 2D point.
///
/// NAMED `Point2D`, NOT `Point`, ON PURPOSE. Apple's `MacTypes.h` declares a
/// GLOBAL `Point` (and a global `Rect`) for Carbon compatibility. A
/// `pulp::canvas::Point` becomes ambiguous with it the moment both are visible
/// — and since `pulp::view::geometry.hpp` re-exports this header, it is visible
/// nearly repo-wide, which breaks Apple's own headers mid-parse:
///
///     MacTypes.h:548:16: error: reference to 'Point' is ambiguous
///
/// `Canvas::Point2D` (canvas.hpp) is an ALIAS of this type, not a second one.
struct Point2D {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Point2D operator+(const Point2D& p) const { return {x + p.x, y + p.y}; }
    constexpr Point2D operator-(const Point2D& p) const { return {x - p.x, y - p.y}; }
    constexpr Point2D operator*(float s) const { return {x * s, y * s}; }
    constexpr bool operator==(const Point2D& p) const { return x == p.x && y == p.y; }
    constexpr bool operator!=(const Point2D& p) const { return !(*this == p); }
};

class AffineTransform {
public:
    float a = 1.0f, b = 0.0f;
    float c = 0.0f, d = 1.0f;
    float e = 0.0f, f = 0.0f;

    constexpr AffineTransform() = default;
    constexpr AffineTransform(float a_, float b_, float c_,
                              float d_, float e_, float f_)
        : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}

    // ── Factories ────────────────────────────────────────────────────────
    static constexpr AffineTransform identity() { return {}; }

    static constexpr AffineTransform translation(float tx, float ty) {
        return {1.0f, 0.0f, 0.0f, 1.0f, tx, ty};
    }

    static constexpr AffineTransform scaling(float sx, float sy) {
        return {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
    }

    /// Scale about the pivot (px, py) — the pivot is a fixed point.
    static constexpr AffineTransform scaling(float sx, float sy,
                                             float px, float py) {
        return {sx, 0.0f, 0.0f, sy, px - px * sx, py - py * sy};
    }

    static AffineTransform rotation(float radians) {
        const float s = std::sin(radians);
        const float co = std::cos(radians);
        return {co, s, -s, co, 0.0f, 0.0f};
    }

    /// Rotate about the pivot (px, py) — the pivot is a fixed point.
    static AffineTransform rotation(float radians, float px, float py) {
        const float s = std::sin(radians);
        const float co = std::cos(radians);
        return {co, s, -s, co,
                px - px * co + py * s,
                py - px * s - py * co};
    }

    /// Shear. `shx` slants x by y; `shy` slants y by x.
    static constexpr AffineTransform shear(float shx, float shy) {
        return {1.0f, shy, shx, 1.0f, 0.0f, 0.0f};
    }

    // ── Composition ──────────────────────────────────────────────────────
    /// Return the transform that applies `*this` FIRST, then `next`.
    /// Reads left-to-right in the order the caller thinks about it:
    ///   scale(2).followed_by(translate(10, 0))  →  scale, then translate.
    constexpr AffineTransform followed_by(const AffineTransform& next) const {
        return {
            next.a * a + next.c * b,
            next.b * a + next.d * b,
            next.a * c + next.c * d,
            next.b * c + next.d * d,
            next.a * e + next.c * f + next.e,
            next.b * e + next.d * f + next.f,
        };
    }

    /// Standard matrix product: `(m1 * m2)` applies m2 first, then m1.
    /// (The mirror image of `followed_by`, for callers who think in matrices.)
    constexpr AffineTransform operator*(const AffineTransform& rhs) const {
        return rhs.followed_by(*this);
    }

    constexpr AffineTransform translated(float tx, float ty) const {
        return followed_by(translation(tx, ty));
    }
    constexpr AffineTransform scaled(float sx, float sy) const {
        return followed_by(scaling(sx, sy));
    }
    AffineTransform rotated(float radians) const {
        return followed_by(rotation(radians));
    }
    AffineTransform rotated(float radians, float px, float py) const {
        return followed_by(rotation(radians, px, py));
    }
    constexpr AffineTransform sheared(float shx, float shy) const {
        return followed_by(shear(shx, shy));
    }

    // ── Inversion ────────────────────────────────────────────────────────
    constexpr float determinant() const { return a * d - b * c; }

    /// True when the transform collapses area to zero and therefore cannot
    /// be inverted (e.g. scale(0, 1)).
    bool is_singular() const {
        const float det = determinant();
        return !std::isfinite(det) || std::abs(det) < 1e-12f;
    }

    /// The inverse, or nullopt when singular. Returning an optional (rather
    /// than silently handing back identity) forces callers to decide what a
    /// non-invertible transform means for them.
    std::optional<AffineTransform> inverted() const {
        if (is_singular()) return std::nullopt;
        const float det = determinant();
        const float inv = 1.0f / det;
        return AffineTransform{
             d * inv,
            -b * inv,
            -c * inv,
             a * inv,
            (c * f - d * e) * inv,
            (b * e - a * f) * inv,
        };
    }

    // ── Application ──────────────────────────────────────────────────────
    constexpr Point2D transform_point(Point2D p) const {
        return {a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }

    constexpr void transform_point(float& x, float& y) const {
        const float nx = a * x + c * y + e;
        const float ny = b * x + d * y + f;
        x = nx;
        y = ny;
    }

    /// Axis-aligned bounding box of the four transformed corners. Under
    /// rotation or shear the result is the AABB of the rotated rect, which is
    /// larger than the source — that is the only meaningful answer for an
    /// axis-aligned rect type.
    Rect2D transform_rect(const Rect2D& r) const {
        const Point2D p0 = transform_point({r.x, r.y});
        const Point2D p1 = transform_point({r.right(), r.y});
        const Point2D p2 = transform_point({r.right(), r.bottom()});
        const Point2D p3 = transform_point({r.x, r.bottom()});
        const float minx = std::min(std::min(p0.x, p1.x), std::min(p2.x, p3.x));
        const float maxx = std::max(std::max(p0.x, p1.x), std::max(p2.x, p3.x));
        const float miny = std::min(std::min(p0.y, p1.y), std::min(p2.y, p3.y));
        const float maxy = std::max(std::max(p0.y, p1.y), std::max(p2.y, p3.y));
        return {minx, miny, maxx - minx, maxy - miny};
    }

    // ── Queries ──────────────────────────────────────────────────────────
    constexpr bool is_identity() const {
        return a == 1.0f && b == 0.0f && c == 0.0f &&
               d == 1.0f && e == 0.0f && f == 0.0f;
    }

    constexpr bool operator==(const AffineTransform& o) const {
        return a == o.a && b == o.b && c == o.c &&
               d == o.d && e == o.e && f == o.f;
    }
    constexpr bool operator!=(const AffineTransform& o) const {
        return !(*this == o);
    }
};

}  // namespace pulp::canvas
