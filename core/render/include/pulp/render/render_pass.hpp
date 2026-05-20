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
/// `time_ms` is CPU wall-time measured around the pass's draw-call
/// submission — it is what the inspector Performance tab has always
/// shown. Phase 6.5 adds `gpu_time_ms`: the *true* GPU-side execution
/// time of the pass, sampled via Dawn timestamp queries
/// (`pulp::render::GpuTimestamps`). The two numbers diverge exactly
/// where perf bugs hide — a pass cheap on the CPU but expensive on the
/// GPU (overdraw, expensive shaders) is invisible without the GPU clock.
///
/// `gpu_time_ms` is only meaningful when `gpu_time_valid` is true.
/// It stays false when the adapter lacks the `timestamp-query` feature,
/// when the resolved sample has not landed yet (timestamps lag the
/// submitting frame by one frame), or in a CPU-only build.
struct PassStats {
    RenderPassType type;
    int draw_calls = 0;
    float time_ms = 0;        ///< CPU wall-time around draw submission (ms).
    float gpu_time_ms = 0;    ///< True GPU execution time (ms); see gpu_time_valid.
    bool  gpu_time_valid = false;  ///< Whether gpu_time_ms holds a real sample.

    /// Explicit alias for the CPU number. Phase 6.5 introduced the
    /// CPU-vs-GPU split; `cpu_time_ms()` makes call sites that want the
    /// CPU clock self-documenting without changing the wire field name.
    float cpu_time_ms() const { return time_ms; }
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

    /// Phase 6.5: feed a resolved GPU-side duration into a pass.
    ///
    /// GPU timestamp queries are resolved one frame after the pass that
    /// wrote them was submitted, so this is called by the GPU-timestamp
    /// readback path with the *previous* frame's per-pass durations. The
    /// index is the pass's position within `passes()` for the frame the
    /// timestamps belong to. Out-of-range indices are ignored (the pass
    /// list may legitimately have changed between frames).
    void set_pass_gpu_time(std::size_t pass_index, float gpu_ms) {
        if (pass_index < passes_.size() && gpu_ms >= 0.0f) {
            passes_[pass_index].gpu_time_ms = gpu_ms;
            passes_[pass_index].gpu_time_valid = true;
        }
    }

    /// Whether any pass in the last frame carries a valid GPU timestamp.
    /// The inspector uses this to decide between showing GPU numbers and
    /// showing "GPU timestamps unavailable".
    bool has_gpu_timing() const {
        for (const auto& p : passes_) {
            if (p.gpu_time_valid) return true;
        }
        return false;
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
    uint64_t frame_count_ = 0;
};

} // namespace pulp::render
