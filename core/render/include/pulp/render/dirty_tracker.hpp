#pragma once

#include <algorithm>
#include <vector>

namespace pulp::render {

/// Tracks rectangular regions that need repainting.
/// Audio plugin UIs have large static areas (panels, labels) with small
/// dynamic regions (meters, waveforms). Only repainting dirty regions is
/// a major performance win.
class DirtyTracker {
public:
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;

        bool intersects(const Rect& other) const {
            return x < other.x + other.w && x + w > other.x &&
                   y < other.y + other.h && y + h > other.y;
        }

        /// Merge two rects into their bounding box
        Rect merged(const Rect& other) const {
            float nx = std::min(x, other.x);
            float ny = std::min(y, other.y);
            return {nx, ny,
                    std::max(x + w, other.x + other.w) - nx,
                    std::max(y + h, other.y + other.h) - ny};
        }

        float area() const { return w * h; }
    };

    /// Mark a rectangular region as needing repaint.
    void invalidate(const Rect& rect) {
        if (rect.w <= 0 || rect.h <= 0) return;
        dirty_rects_.push_back(rect);
        coalesce_if_needed();
    }

    /// Mark a rectangular region as needing repaint (convenience overload).
    void invalidate(float x, float y, float w, float h) {
        invalidate({x, y, w, h});
    }

    /// Mark the entire viewport as dirty (forces full repaint).
    void invalidate_all() {
        full_repaint_ = true;
        dirty_rects_.clear();
    }

    /// Returns true if any region needs repainting.
    bool is_dirty() const { return full_repaint_ || !dirty_rects_.empty(); }

    /// Returns true if the entire viewport needs repainting.
    bool needs_full_repaint() const { return full_repaint_; }

    /// Get the list of dirty rectangles (empty if full repaint).
    const std::vector<Rect>& dirty_rects() const { return dirty_rects_; }

    /// Clear all dirty state after a frame has been painted.
    /// Call this after submitting the frame to the GPU.
    void clear() {
        dirty_rects_.clear();
        full_repaint_ = false;
        ++frame_count_;
    }

    /// Get the bounding rect of all dirty regions (for partial repaints).
    Rect bounds() const {
        if (dirty_rects_.empty()) return {};
        Rect b = dirty_rects_[0];
        for (size_t i = 1; i < dirty_rects_.size(); ++i)
            b = b.merged(dirty_rects_[i]);
        return b;
    }

    /// Set the viewport size. If dirty area exceeds this percentage of the
    /// viewport, the tracker switches to full repaint mode.
    void set_viewport(float width, float height, float full_repaint_threshold = 0.6f) {
        viewport_w_ = width;
        viewport_h_ = height;
        full_repaint_threshold_ = full_repaint_threshold;
    }

    /// Enable/disable debug overlay (draws dirty regions with colored flash).
    void set_debug_overlay(bool enabled) { debug_overlay_ = enabled; }
    bool debug_overlay() const { return debug_overlay_; }

    /// Frame counter for multi-frame history tracking.
    uint64_t frame_count() const { return frame_count_; }

private:
    void coalesce_if_needed() {
        // If we have too many small rects, merge nearby ones
        if (dirty_rects_.size() > max_rects_) {
            coalesce();
        }

        // If dirty area exceeds threshold, switch to full repaint
        if (viewport_w_ > 0 && viewport_h_ > 0) {
            float total_area = 0;
            for (auto& r : dirty_rects_) total_area += r.area();
            if (total_area > viewport_w_ * viewport_h_ * full_repaint_threshold_) {
                invalidate_all();
            }
        }
    }

    void coalesce() {
        // Simple greedy merge: combine overlapping/adjacent rects
        bool merged = true;
        while (merged) {
            merged = false;
            for (size_t i = 0; i < dirty_rects_.size(); ++i) {
                for (size_t j = i + 1; j < dirty_rects_.size(); ++j) {
                    auto combined = dirty_rects_[i].merged(dirty_rects_[j]);
                    // Merge if the bounding box isn't much larger than the sum
                    float sum_area = dirty_rects_[i].area() + dirty_rects_[j].area();
                    if (combined.area() < sum_area * 1.5f ||
                        dirty_rects_[i].intersects(dirty_rects_[j])) {
                        dirty_rects_[i] = combined;
                        dirty_rects_.erase(dirty_rects_.begin() + static_cast<long>(j));
                        merged = true;
                        break;
                    }
                }
                if (merged) break;
            }
        }
    }

    std::vector<Rect> dirty_rects_;
    bool full_repaint_ = true;  // First frame is always full
    bool debug_overlay_ = false;
    float viewport_w_ = 0;
    float viewport_h_ = 0;
    float full_repaint_threshold_ = 0.6f;
    uint64_t frame_count_ = 0;
    static constexpr size_t max_rects_ = 16;
};

} // namespace pulp::render
