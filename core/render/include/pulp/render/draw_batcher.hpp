#pragma once

#include <pulp/canvas/canvas.hpp>
#include <vector>

namespace pulp::render {

/// Groups compatible draw calls to reduce GPU state changes.
/// Sits logically between the View paint system and the Canvas backend.
/// Non-overlapping draws with the same state (color, shader, blend mode)
/// can be submitted in a single batch.
///
/// Usage:
///   DrawBatcher batcher;
///   batcher.begin();
///   // ... canvas draws happen, batcher observes ...
///   auto stats = batcher.end();
///   // stats.draws_before / stats.draws_after for profiling
class DrawBatcher {
public:
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
    };

    struct BatchStats {
        int draws_before = 0;   ///< Total draw calls submitted
        int draws_after = 0;    ///< Draw calls after batching
        int batches_merged = 0; ///< Number of batches that were merged
    };

    /// Start a batch recording pass.
    void begin() {
        entries_.clear();
        stats_ = {};
        active_ = true;
    }

    /// End batch recording and return statistics.
    BatchStats end() {
        active_ = false;
        stats_.draws_before = static_cast<int>(entries_.size());
        coalesce();
        stats_.draws_after = static_cast<int>(entries_.size());
        stats_.batches_merged = stats_.draws_before - stats_.draws_after;
        return stats_;
    }

    /// Record a draw call with its bounds and state key.
    /// The state_key identifies compatible draws (same shader/color/blend).
    void record(const Rect& bounds, uint64_t state_key) {
        if (!active_) return;
        entries_.push_back({bounds, state_key});
    }

    /// Returns true if batching is currently active.
    bool is_active() const { return active_; }

    /// Manual batch hint — marks the start of a group that should be batched.
    void begin_batch() { manual_batch_active_ = true; }

    /// Manual batch hint — ends the current batch group.
    void end_batch() { manual_batch_active_ = false; }

    bool in_manual_batch() const { return manual_batch_active_; }

    /// Get the coalesced entries after end().
    struct Entry {
        Rect bounds;
        uint64_t state_key = 0;
    };
    const std::vector<Entry>& entries() const { return entries_; }

private:
    static bool overlaps(const Rect& a, const Rect& b) {
        return a.x < b.x + b.w && a.x + a.w > b.x &&
               a.y < b.y + b.h && a.y + a.h > b.y;
    }

    static Rect merge_bounds(const Rect& a, const Rect& b) {
        float nx = std::min(a.x, b.x);
        float ny = std::min(a.y, b.y);
        return {nx, ny,
                std::max(a.x + a.w, b.x + b.w) - nx,
                std::max(a.y + a.h, b.y + b.h) - ny};
    }

    void coalesce() {
        // Merge non-overlapping entries with the same state key
        bool merged = true;
        while (merged) {
            merged = false;
            for (size_t i = 0; i < entries_.size(); ++i) {
                for (size_t j = i + 1; j < entries_.size(); ++j) {
                    if (entries_[i].state_key != entries_[j].state_key)
                        continue;

                    // Check that no entry with a different key overlaps between them
                    bool can_merge = true;
                    auto combined = merge_bounds(entries_[i].bounds, entries_[j].bounds);
                    for (size_t k = i + 1; k < j; ++k) {
                        if (entries_[k].state_key != entries_[i].state_key &&
                            overlaps(entries_[k].bounds, combined)) {
                            can_merge = false;
                            break;
                        }
                    }

                    if (can_merge) {
                        entries_[i].bounds = combined;
                        entries_.erase(entries_.begin() + static_cast<long>(j));
                        merged = true;
                        break;
                    }
                }
                if (merged) break;
            }
        }
    }

    std::vector<Entry> entries_;
    BatchStats stats_;
    bool active_ = false;
    bool manual_batch_active_ = false;
};

} // namespace pulp::render
