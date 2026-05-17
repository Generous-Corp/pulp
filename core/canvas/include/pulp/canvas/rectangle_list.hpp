#pragma once

// RectangleList — union of rectangles for clip regions and dirty tracking.
// Supports intersection, subtraction, containment, and bounding box queries.

#include <vector>
#include <algorithm>
#include <cmath>

namespace pulp::canvas {

struct Rect {
    float x = 0, y = 0, width = 0, height = 0;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    bool empty() const { return width <= 0 || height <= 0; }

    bool contains(float px, float py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    bool intersects(const Rect& other) const {
        if (empty() || other.empty()) return false;
        return x < other.right() && right() > other.x &&
               y < other.bottom() && bottom() > other.y;
    }

    Rect intersection(const Rect& other) const {
        float ix = std::max(x, other.x);
        float iy = std::max(y, other.y);
        float ir = std::min(right(), other.right());
        float ib = std::min(bottom(), other.bottom());
        if (ir <= ix || ib <= iy) return {};
        return {ix, iy, ir - ix, ib - iy};
    }

    Rect enclosing_union(const Rect& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        float ux = std::min(x, other.x);
        float uy = std::min(y, other.y);
        float ur = std::max(right(), other.right());
        float ub = std::max(bottom(), other.bottom());
        return {ux, uy, ur - ux, ub - uy};
    }

    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
};

/// List of non-overlapping rectangles — used for clip regions and dirty tracking.
class RectangleList {
public:
    RectangleList() = default;

    /// Add a rectangle to the list (may overlap existing)
    void add(const Rect& rect) {
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
    const Rect& operator[](int i) const { return rects_[static_cast<size_t>(i)]; }

    /// Whether a point is contained in any rectangle
    bool contains(float px, float py) const {
        for (auto& r : rects_)
            if (r.contains(px, py)) return true;
        return false;
    }

    /// Whether any rectangle intersects the given rect
    bool intersects(const Rect& rect) const {
        for (auto& r : rects_)
            if (r.intersects(rect)) return true;
        return false;
    }

    /// Compute the bounding box of all rectangles
    Rect bounding_box() const {
        if (rects_.empty()) return {};
        Rect result = rects_[0];
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
    RectangleList clipped(const Rect& clip) const {
        RectangleList result;
        for (auto& r : rects_) {
            auto i = r.intersection(clip);
            if (!i.empty())
                result.add(i);
        }
        return result;
    }

    /// Subtract a rectangle from all rectangles in the list
    void subtract(const Rect& sub) {
        std::vector<Rect> result;
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

private:
    std::vector<Rect> rects_;
};

}  // namespace pulp::canvas
