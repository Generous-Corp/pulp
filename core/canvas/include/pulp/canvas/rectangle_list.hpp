#pragma once

// RectangleList — union of rectangles for clip regions and dirty tracking.
// Supports intersection, subtraction, containment, and bounding box queries.

#include <vector>
#include <algorithm>
#include <cmath>

namespace pulp::canvas {

/// Axis-aligned rectangle in canvas coordinates.
///
/// NAMED `Rect2D`, NOT `Rect`, ON PURPOSE — do not "simplify" this back.
/// Apple's `MacTypes.h` declares a GLOBAL `struct Rect` (and a global `Point`)
/// for Carbon compatibility. Any translation unit that says
/// `using namespace pulp::canvas;` AND transitively includes an Apple header
/// then hits an unfixable ambiguity inside Apple's own headers:
///
///     MacTypes.h:556:16: error: reference to 'Rect' is ambiguous
///       note: candidate found by name lookup is 'pulp::canvas::Rect'
///
/// 65 TUs in this repo use that using-directive. The name stayed survivable
/// only while this header was rarely included; once `Path` (and therefore
/// `canvas.hpp`) needed a rect type, it became visible nearly repo-wide and the
/// collision became unavoidable. `Point2D` in affine_transform.hpp is named for
/// exactly the same reason.
struct Rect2D {
    float x = 0, y = 0, width = 0, height = 0;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    bool empty() const { return width <= 0 || height <= 0; }

    bool contains(float px, float py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    bool intersects(const Rect2D& other) const {
        if (empty() || other.empty()) return false;
        return x < other.right() && right() > other.x &&
               y < other.bottom() && bottom() > other.y;
    }

    Rect2D intersection(const Rect2D& other) const {
        float ix = std::max(x, other.x);
        float iy = std::max(y, other.y);
        float ir = std::min(right(), other.right());
        float ib = std::min(bottom(), other.bottom());
        if (ir <= ix || ib <= iy) return {};
        return {ix, iy, ir - ix, ib - iy};
    }

    Rect2D enclosing_union(const Rect2D& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        float ux = std::min(x, other.x);
        float uy = std::min(y, other.y);
        float ur = std::max(right(), other.right());
        float ub = std::max(bottom(), other.bottom());
        return {ux, uy, ur - ux, ub - uy};
    }

    bool operator==(const Rect2D& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
};

/// List of non-overlapping rectangles — used for clip regions and dirty tracking.
class RectangleList {
public:
    RectangleList() = default;

    /// Add a rectangle to the list (may overlap existing)
    void add(const Rect2D& rect) {
        if (!rect.empty())
            rects_.push_back(rect);
    }

    /// Clear all rectangles
    void clear() { rects_.clear(); }

    /// Whether the list is empty
    bool empty() const { return rects_.empty(); }

    /// Number of rectangles
    int size() const { return static_cast<int>(rects_.size()); }

    /// Get a rectangle by index
    const Rect2D& operator[](int i) const { return rects_[static_cast<size_t>(i)]; }

    /// Whether a point is contained in any rectangle
    bool contains(float px, float py) const {
        for (auto& r : rects_)
            if (r.contains(px, py)) return true;
        return false;
    }

    /// Whether any rectangle intersects the given rect
    bool intersects(const Rect2D& rect) const {
        for (auto& r : rects_)
            if (r.intersects(rect)) return true;
        return false;
    }

    /// Compute the bounding box of all rectangles
    Rect2D bounding_box() const {
        if (rects_.empty()) return {};
        Rect2D result = rects_[0];
        for (size_t i = 1; i < rects_.size(); ++i)
            result = result.enclosing_union(rects_[i]);
        return result;
    }

    /// Total area (may double-count overlapping regions)
    float total_area() const {
        float area = 0;
        for (auto& r : rects_)
            area += r.width * r.height;
        return area;
    }

    /// Clip this list against a rectangle (keep only the intersection)
    RectangleList clipped(const Rect2D& clip) const {
        RectangleList result;
        for (auto& r : rects_) {
            auto i = r.intersection(clip);
            if (!i.empty())
                result.add(i);
        }
        return result;
    }

    /// Subtract a rectangle from all rectangles in the list
    void subtract(const Rect2D& sub) {
        std::vector<Rect2D> result;
        for (auto& r : rects_) {
            if (!r.intersects(sub)) {
                result.push_back(r);
                continue;
            }
            // Split r into up to 4 non-overlapping pieces
            // Top piece
            if (sub.y > r.y)
                result.push_back({r.x, r.y, r.width, sub.y - r.y});
            // Bottom piece
            if (sub.bottom() < r.bottom())
                result.push_back({r.x, sub.bottom(), r.width, r.bottom() - sub.bottom()});
            // Left piece (in the middle band)
            float mid_top = std::max(r.y, sub.y);
            float mid_bot = std::min(r.bottom(), sub.bottom());
            if (sub.x > r.x)
                result.push_back({r.x, mid_top, sub.x - r.x, mid_bot - mid_top});
            // Right piece
            if (sub.right() < r.right())
                result.push_back({sub.right(), mid_top, r.right() - sub.right(), mid_bot - mid_top});
        }
        rects_ = std::move(result);
    }

    /// Iterator support
    auto begin() const { return rects_.begin(); }
    auto end() const { return rects_.end(); }

    // ── Antialiased region operations ──────────────────────────────────
    //
    // The plain `add` / `subtract` / `clipped` API above is the
    // axis-aligned, integer-grid story — fine for dirty-tracking and
    // clip-band scanlines. The AA variants below treat each rectangle
    // as a half-open float region with fractional coverage at the
    // edges, so set operations on rectangles whose bounds don't fall
    // on the pixel grid don't round away sub-pixel area.
    //
    // The pure-CPU implementation here is the cross-platform contract.
    // When `PULP_HAS_SKIA` is defined, the equivalent calls may dispatch
    // to `SkRegion` for the integer-aligned fast path; the AA cases
    // still walk the CPU code path because `SkRegion` is integer-only.
    // The CPU implementations live in this header to keep RectangleList
    // header-only.

    /// Antialiased union — appends `rect`, then merges any pieces that
    /// share an edge to within `aa_tolerance`. Behaviorally equivalent
    /// to `add()` followed by a coalesce pass.
    ///
    /// NOT RT-safe — `std::vector::push_back` may reallocate.
    void union_aa(const Rect2D& rect, float aa_tolerance = 1.0f / 256.0f) {
        if (rect.empty()) return;
        rects_.push_back(rect);
        coalesce_aa(aa_tolerance);
    }

    /// Antialiased subtract — splits intersecting rectangles into
    /// non-overlapping pieces. Edges within `aa_tolerance` of `sub`'s
    /// edges are treated as coincident so a 1.001px wide sliver
    /// doesn't survive as a stray rectangle.
    void subtract_aa(const Rect2D& sub, float aa_tolerance = 1.0f / 256.0f) {
        if (sub.empty()) return;
        std::vector<Rect2D> out;
        out.reserve(rects_.size());
        for (auto& r : rects_) {
            if (!r.intersects(sub)) {
                out.push_back(r);
                continue;
            }
            // Top
            if (sub.y > r.y + aa_tolerance) {
                out.push_back({r.x, r.y, r.width, sub.y - r.y});
            }
            // Bottom
            if (sub.bottom() + aa_tolerance < r.bottom()) {
                out.push_back({r.x, sub.bottom(), r.width, r.bottom() - sub.bottom()});
            }
            float mid_top = std::max(r.y, sub.y);
            float mid_bot = std::min(r.bottom(), sub.bottom());
            float mid_h = mid_bot - mid_top;
            if (mid_h > aa_tolerance) {
                // Left
                if (sub.x > r.x + aa_tolerance) {
                    out.push_back({r.x, mid_top, sub.x - r.x, mid_h});
                }
                // Right
                if (sub.right() + aa_tolerance < r.right()) {
                    out.push_back({sub.right(), mid_top, r.right() - sub.right(), mid_h});
                }
            }
        }
        rects_ = std::move(out);
    }

    /// Antialiased intersect — keep only the intersection with `clip`,
    /// in-place. Edges within `aa_tolerance` of `clip`'s edges are
    /// treated as coincident.
    void intersect_aa(const Rect2D& clip, float aa_tolerance = 1.0f / 256.0f) {
        std::vector<Rect2D> out;
        out.reserve(rects_.size());
        for (auto& r : rects_) {
            auto i = r.intersection(clip);
            // Drop slivers below the AA tolerance — they would
            // contribute < 1/256 alpha and only bloat the list.
            if (i.empty()) continue;
            if (i.width < aa_tolerance || i.height < aa_tolerance) continue;
            out.push_back(i);
        }
        rects_ = std::move(out);
    }

    /// Coalesce adjacent rectangles that share a full edge (within
    /// `aa_tolerance`). The result is not guaranteed minimal — a true
    /// minimal cover requires a scanline merge — but this pass is
    /// cheap and handles the common "two abutting halves" case.
    void coalesce_aa(float aa_tolerance = 1.0f / 256.0f) {
        bool merged = true;
        while (merged && rects_.size() >= 2) {
            merged = false;
            for (std::size_t i = 0; i < rects_.size() && !merged; ++i) {
                for (std::size_t j = i + 1; j < rects_.size(); ++j) {
                    auto& a = rects_[i];
                    auto& b = rects_[j];
                    // Horizontal merge: same y/height, right edge of one
                    // == left edge of the other.
                    bool same_horiz =
                        std::abs(a.y - b.y) < aa_tolerance &&
                        std::abs(a.height - b.height) < aa_tolerance;
                    if (same_horiz) {
                        if (std::abs(a.right() - b.x) < aa_tolerance) {
                            a = {a.x, a.y, a.width + b.width, a.height};
                            rects_.erase(rects_.begin() + static_cast<long>(j));
                            merged = true;
                            break;
                        }
                        if (std::abs(b.right() - a.x) < aa_tolerance) {
                            a = {b.x, a.y, a.width + b.width, a.height};
                            rects_.erase(rects_.begin() + static_cast<long>(j));
                            merged = true;
                            break;
                        }
                    }
                    // Vertical merge.
                    bool same_vert =
                        std::abs(a.x - b.x) < aa_tolerance &&
                        std::abs(a.width - b.width) < aa_tolerance;
                    if (same_vert) {
                        if (std::abs(a.bottom() - b.y) < aa_tolerance) {
                            a = {a.x, a.y, a.width, a.height + b.height};
                            rects_.erase(rects_.begin() + static_cast<long>(j));
                            merged = true;
                            break;
                        }
                        if (std::abs(b.bottom() - a.y) < aa_tolerance) {
                            a = {a.x, b.y, a.width, a.height + b.height};
                            rects_.erase(rects_.begin() + static_cast<long>(j));
                            merged = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    /// Reports the implementation backend in use. Today this is always
    /// `"cpu"`; a future build with `PULP_HAS_SKIA` may report
    /// `"skia"` for the integer-aligned fast path. Tests and callers
    /// can branch on this to skip Skia-only checks on cross-platform
    /// CI lanes.
    static constexpr const char* aa_backend() {
#if defined(PULP_HAS_SKIA) && PULP_HAS_SKIA
        return "skia";
#else
        return "cpu";
#endif
    }

private:
    std::vector<Rect2D> rects_;
};

}  // namespace pulp::canvas
