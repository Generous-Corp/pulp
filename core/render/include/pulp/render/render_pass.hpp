#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::render {

/// Defines the ordering of render passes in a frame.
/// Audio plugin UIs benefit from ordered compositing:
/// background → content → effects → overlays → post-effects.
enum class RenderPassType {
    background,     ///< Static backgrounds, panel fills
    content,        ///< Main widget content (knobs, faders, meters)
    effects,        ///< Per-layer effects (blur, bloom)
    overlay,        ///< Tooltips, dropdowns, modals (always on top)
    post_effects    ///< Full-frame post-processing (vignette, color grading)
};

/// Statistics for a single render pass.
///
/// `time_ms` is CPU wall-time measured around the pass's draw calls
/// (Phase 6.1). `gpu_time_ms` is the real GPU-side execution time
/// measured via WebGPU timestamp queries (Phase 6.5); it is only
/// meaningful when `gpu_time_valid` is true — the `timestamp-query`
/// feature may be unavailable on some adapters, in which case the
/// inspector falls back to the CPU number.
struct PassStats {
    RenderPassType type;
    int draw_calls = 0;
    float time_ms = 0;           ///< CPU wall-time around the pass (ms).
    float gpu_time_ms = 0;       ///< GPU-side execution time (ms); see flag.
    bool gpu_time_valid = false; ///< True when `gpu_time_ms` is real data.
};

/// Frame-level render pass manager.
/// Tracks pass ordering, budgeting, and statistics.
class RenderPassManager {
public:
    /// Begin a new frame. Resets all pass statistics.
    void begin_frame() {
        passes_.clear();
        frame_count_++;
        total_time_ms_ = 0;
    }

    /// Begin a render pass of the given type.
    void begin_pass(RenderPassType type) {
        current_pass_ = type;
        passes_.push_back({type, 0, 0});
    }

    /// End the current render pass.
    void end_pass(float time_ms = 0, int draw_calls = 0) {
        if (!passes_.empty()) {
            passes_.back().time_ms = time_ms;
            passes_.back().draw_calls = draw_calls;
            total_time_ms_ += time_ms;
        }
    }

    /// End the frame.
    void end_frame() {
        // Check if we exceeded budget
        over_budget_ = (budget_ms_ > 0 && total_time_ms_ > budget_ms_);
    }

    /// Attach a resolved GPU-side duration to the pass at `pass_index`
    /// (the index into `passes()`, which is begin_pass() call order).
    ///
    /// Phase 6.5: the GPU timing for frame N is only readable in frame
    /// N+1 (one frame of map-read lag), so the render loop carries the
    /// previous frame's resolved timestamps forward and applies them
    /// here after `begin_pass()` has rebuilt the pass list. `valid`
    /// reflects whether the timestamp pair resolved cleanly — when the
    /// `timestamp-query` feature is unavailable it stays false and the
    /// inspector keeps showing the CPU number.
    void set_pass_gpu_time(std::size_t pass_index, float gpu_time_ms,
                           bool valid) {
        if (pass_index < passes_.size()) {
            passes_[pass_index].gpu_time_ms = gpu_time_ms;
            passes_[pass_index].gpu_time_valid = valid;
        }
    }

    /// Whether GPU timestamp queries are available on this adapter.
    /// When false the inspector perf view shows CPU time only and labels
    /// GPU time as "unavailable" rather than reporting a misleading zero.
    bool gpu_timestamps_available() const { return gpu_timestamps_available_; }
    void set_gpu_timestamps_available(bool available) {
        gpu_timestamps_available_ = available;
    }

    /// Set the per-frame time budget in milliseconds (0 = no budget).
    /// At 60fps, budget should be ~16ms. At 120fps, ~8ms.
    void set_budget(float ms) { budget_ms_ = ms; }
    float budget() const { return budget_ms_; }

    /// Whether the last frame exceeded its time budget.
    bool over_budget() const { return over_budget_; }

    /// Total time for the last frame.
    float total_time_ms() const { return total_time_ms_; }

    /// Get statistics for all passes in the last frame.
    const std::vector<PassStats>& passes() const { return passes_; }

    /// Frame counter.
    uint64_t frame_count() const { return frame_count_; }

    /// Current pass type.
    RenderPassType current_pass() const { return current_pass_; }

private:
    std::vector<PassStats> passes_;
    RenderPassType current_pass_ = RenderPassType::background;
    float budget_ms_ = 16.67f;  // 60fps default
    float total_time_ms_ = 0;
    bool over_budget_ = false;
    bool gpu_timestamps_available_ = false;
    uint64_t frame_count_ = 0;
};

} // namespace pulp::render
