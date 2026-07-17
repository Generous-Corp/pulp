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
/// submission. There is deliberately NO per-pass GPU number: Skia Graphite
/// owns the command encoders, so Pulp cannot inject per-pass timestamp writes
/// from outside — per-pass GPU timing is structurally unavailable under
/// Graphite. The honest GPU clock is frame-level, whole-recording
/// (`RenderPassManager::gpu_render_time_ms()`), not per pass.
struct PassStats {
    RenderPassType type;
    int draw_calls = 0;
    float time_ms = 0;        ///< CPU wall-time around draw submission (ms).

    /// Explicit alias for the CPU number. `cpu_time_ms()` makes call sites
    /// that want the CPU clock self-documenting without changing the wire
    /// field name.
    float cpu_time_ms() const { return time_ms; }
};

/// Frame-level render pass manager.
/// Tracks pass ordering, budgeting, and statistics.
class RenderPassManager {
public:
    /// Begin a new frame. Resets all pass statistics.
    void begin_frame() {
        passes_.clear();
        current_pass_ = RenderPassType::background;
        frame_count_++;
        total_time_ms_ = 0;
        over_budget_ = false;
        // Invalidate last frame's whole-recording GPU sample; a fresh sample
        // (if any) is fed in via set_gpu_render_time_ms() for this frame. The
        // value itself is left as-is so a stale read between begin_frame() and
        // the next set is reported as invalid rather than as a bogus 0.
        gpu_render_time_valid_ = false;
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

    /// Feed the whole-recording GPU render time for this frame.
    ///
    /// This is FRAME-LEVEL — the entire render recording's GPU-side elapsed
    /// time (Skia Graphite GpuStats(kElapsedTime), surfaced by
    /// `SkiaSurface::gpu_render_time_ms()` / `WindowHost::gpu_render_time_ms()`).
    /// It is the ONLY GPU-clock granularity Pulp exposes: per-pass GPU timing is
    /// structurally unavailable under Graphite (Graphite owns the command
    /// encoders), so the whole-recording number is the honest place to record
    /// the GPU clock. `valid` should reflect
    /// `WindowHost::gpu_render_timing_available()`. Negative durations are
    /// clamped to "invalid".
    void set_gpu_render_time_ms(float ms, bool valid) {
        if (valid && ms >= 0.0f) {
            gpu_render_time_ms_ = ms;
            gpu_render_time_valid_ = true;
        } else {
            gpu_render_time_valid_ = false;
        }
    }

    /// Whole-recording GPU render time (ms) for the most recent frame that fed
    /// a valid sample. Only meaningful when gpu_render_timing_available() is
    /// true. Frame-level, NOT per-pass — see set_gpu_render_time_ms().
    float gpu_render_time_ms() const { return gpu_render_time_ms_; }

    /// Whether the frame-level whole-recording GPU render time holds a real,
    /// current sample. The inspector uses this to decide between showing the
    /// GPU render number and showing "GPU render timing unavailable".
    bool gpu_render_timing_available() const { return gpu_render_time_valid_; }

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
    // Frame-level (whole-recording) GPU render time, distinct from the
    // per-pass PassStats::gpu_time_ms. See set_gpu_render_time_ms().
    float gpu_render_time_ms_ = 0.0f;
    bool gpu_render_time_valid_ = false;
};

} // namespace pulp::render
