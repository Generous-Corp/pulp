#pragma once

/// @file render_pass_stats.hpp
/// Per-pass render attribution: a rolling history of where a frame's CPU time
/// goes, broken down by render pass type.
///
/// A `RenderPassSource` is where the numbers come from. The overlay reads one
/// without knowing what produces it, so a test feeds synthetic frames and a
/// host adapts its own render loop; `ManagerPassSource` adapts Vellum's
/// `RenderPassManager`. `PassHistory` is pure state, so a panel or a test can
/// ask for a pass's average and peak without an overlay.
///
/// The timings are CPU wall-time around each pass's draw calls, not GPU time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vellum::render {
class RenderPassManager;
}

namespace pulp::vellum_components::overlay {

/// Background, content, effects, overlay, post-effects: the order a frame
/// composites them in. A pass's `type` is its index in this order.
inline constexpr std::size_t kPassTypeCount = 5;

/// Frames of history kept per pass type: one second at 60 fps.
inline constexpr std::size_t kPassHistoryFrames = 60;

/// The display name of pass type `index` ("background", "content", "effects",
/// "overlay", "post"), or "unknown" past the last.
const char* pass_type_name(std::size_t index);

/// One pass of one frame.
struct PassSample {
    std::size_t type = 0; ///< index in compositing order
    int draw_calls = 0;
    float time_ms = 0.0f; ///< CPU wall-time around the draw calls
};

/// Where a frame's pass statistics come from.
class RenderPassSource {
  public:
    virtual ~RenderPassSource() = default;

    /// Frames begun so far. Zero means no frame has run, and a count that has
    /// not moved means the passes are the ones already seen.
    virtual std::uint64_t frame_count() const = 0;
    /// Whether the last frame went over its time budget.
    virtual bool over_budget() const = 0;
    /// The last frame's total CPU time and the budget it is judged against.
    virtual float total_time_ms() const = 0;
    virtual float budget_ms() const = 0;
    /// The passes of the last frame, in the order they ran. A type may repeat.
    virtual std::vector<PassSample> passes() const = 0;
};

/// A source over Vellum's `RenderPassManager`, which it borrows.
class ManagerPassSource final : public RenderPassSource {
  public:
    explicit ManagerPassSource(const ::vellum::render::RenderPassManager& manager)
        : manager_(manager) {}

    std::uint64_t frame_count() const override;
    bool over_budget() const override;
    float total_time_ms() const override;
    float budget_ms() const override;
    std::vector<PassSample> passes() const override;

  private:
    const ::vellum::render::RenderPassManager& manager_;
};

/// The height of the pass panel: a heading and a note, then, with a source,
/// a frame summary and one entry for each of `rows` passes with history (or one
/// "awaiting frames" line when there are none); without a source, one line
/// saying so.
float pass_panel_height(std::size_t rows, bool has_source);

/// One pass type over the recorded history.
struct PassAttribution {
    std::size_t type = 0;
    const char* name = "";
    bool present = false;     ///< the pass ran in the most recent frame
    float last_cpu_ms = 0.0f; ///< the most recent frame
    float avg_cpu_ms = 0.0f;  ///< mean over the recorded history
    float peak_cpu_ms = 0.0f; ///< worst over the recorded history
    int last_draw_calls = 0;
    int peak_draw_calls = 0;
    std::size_t samples = 0; ///< frames recorded for this pass
};

/// A rolling per-pass history, fed one frame at a time.
class PassHistory {
  public:
    /// Record `source`'s last frame. False, and nothing recorded, when the
    /// source has run no frame or the frame was already recorded, so a repaint
    /// of the same frame never fills the history with copies of it. A pass
    /// type that ran more than once in the frame records its sum.
    bool capture(const RenderPassSource& source);

    /// Every pass type in compositing order, whether or not it has history.
    std::vector<PassAttribution> attribution() const;

    /// The recorded CPU times of pass type `type`, oldest first.
    std::vector<float> cpu_ms_history(std::size_t type) const;

    /// Frames in the history the source reported over budget.
    int overrun_count() const {
        return overruns_;
    }
    /// Frames recorded since construction. The per-pass detail kept is the
    /// last `kPassHistoryFrames`; this keeps counting.
    std::uint64_t frames_captured() const {
        return frames_captured_;
    }

  private:
    struct Ring {
        std::array<float, kPassHistoryFrames> cpu_ms{};
        std::array<int, kPassHistoryFrames> draw_calls{};
        std::size_t head = 0;
        std::size_t count = 0;
        bool present_last_frame = false;
    };

    std::array<Ring, kPassTypeCount> rings_{};
    std::uint64_t last_frame_ = 0;
    std::uint64_t frames_captured_ = 0;
    int overruns_ = 0;
};

} // namespace pulp::vellum_components::overlay
