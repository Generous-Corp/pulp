#pragma once

/// @file waveform_overview_view.hpp
/// Headless-drawable waveform view: paints min/max peak columns from a
/// `pulp::audio::WaveformOverview` through the 2D Canvas, over a caller-set
/// viewport. This is the CPU / headless counterpart to the GPU waveform path —
/// it draws through plain Canvas verbs (`fill_rect` + `stroke_line`), so it
/// renders identically on every backend and is fully assertable with a
/// `RecordingCanvas`.

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/audio/waveform_overview.hpp>

#include <algorithm>
#include <cstdint>

namespace pulp::view {

/// A View that renders a pre-decoded min/max peak overview as vertical columns.
///
/// It is viewport-aware: `set_viewport` selects a sub-range of source frames to
/// display, so one overview can be scrubbed and zoomed without rebuilding it.
/// Every pixel column asks the overview for the folded (min, max) over the
/// source-frame window that column covers, then paints one vertical stroke from
/// the column max down to the column min. Silence collapses to a one-pixel line
/// on the center axis; loud regions paint tall columns — so the painted heights
/// track the underlying peaks.
class WaveformOverviewView : public View {
public:
    WaveformOverviewView() = default;

    /// Point the view at an overview (borrowed — the caller keeps it alive, or
    /// calls `clear_overview`). Resets the viewport to the full source range.
    void set_overview(const pulp::audio::WaveformOverview* overview,
                      uint32_t channel = pulp::audio::WaveformOverview::kAllChannels) {
        overview_ = overview;
        channel_ = channel;
        view_all();
    }

    void clear_overview() {
        overview_ = nullptr;
        vp_start_ = 0;
        vp_end_ = 0;
    }

    bool has_overview() const {
        return overview_ != nullptr && !overview_->empty();
    }

    /// Total source frames behind the overview (0 when none is set).
    uint64_t source_frames() const {
        return overview_ ? overview_->info().num_source_frames : 0;
    }

    // ── Viewport ─────────────────────────────────────────────────────────

    /// Source-frame window [start, end) to display. Clamped to the overview's
    /// length; an inverted or empty range paints only the background.
    void set_viewport(uint64_t start_frame, uint64_t end_frame) {
        const uint64_t total = source_frames();
        vp_start_ = std::min(start_frame, total);
        vp_end_ = std::min(end_frame, total);
        if (vp_end_ < vp_start_) vp_end_ = vp_start_;
    }

    uint64_t viewport_start() const { return vp_start_; }
    uint64_t viewport_end() const { return vp_end_; }

    /// Reset the viewport to the whole source.
    void view_all() {
        vp_start_ = 0;
        vp_end_ = source_frames();
    }

    // ── Styling ──────────────────────────────────────────────────────────
    void set_line_color(canvas::Color c) { line_color_ = c; }
    void set_background_color(canvas::Color c) { bg_color_ = c; }
    canvas::Color line_color() const { return line_color_; }
    canvas::Color background_color() const { return bg_color_; }

    /// Folded (min, max) over the source-frame window [f0, f1), using the
    /// finest overview level. Returns false when there is no overview, the
    /// window is empty, or it falls outside the peak table. Public so tests can
    /// assert that the painted columns track the peaks directly.
    bool column_peak(uint64_t f0, uint64_t f1,
                     float& out_min, float& out_max) const {
        if (!has_overview() || f1 <= f0) return false;
        const auto& lvl = overview_->level(0);
        if (lvl.samples_per_peak == 0 || lvl.peaks.empty()) return false;

        uint64_t p0 = f0 / lvl.samples_per_peak;
        uint64_t p1 = (f1 + lvl.samples_per_peak - 1) / lvl.samples_per_peak;
        if (p1 <= p0) p1 = p0 + 1;
        if (p0 >= lvl.peaks_per_channel) return false;
        p1 = std::min<uint64_t>(p1, lvl.peaks_per_channel);

        float lo = 1.0f, hi = -1.0f;
        const auto fold = [&](uint32_t ch) {
            if (ch >= lvl.peaks.size()) return;
            const auto& col = lvl.peaks[ch];
            for (uint64_t i = p0; i < p1 && i < col.size(); ++i) {
                lo = std::min(lo, col[i].min());
                hi = std::max(hi, col[i].max());
            }
        };
        if (channel_ == pulp::audio::WaveformOverview::kAllChannels) {
            for (uint32_t ch = 0; ch < lvl.peaks.size(); ++ch) fold(ch);
        } else {
            fold(channel_);
        }
        if (hi < lo) return false;  // no peaks landed in the window
        out_min = lo;
        out_max = hi;
        return true;
    }

    void paint(canvas::Canvas& canvas) override {
        const Rect b = local_bounds();

        canvas.set_fill_color(bg_color_);
        canvas.fill_rect(0.0f, 0.0f, b.width, b.height);

        if (!has_overview() || vp_end_ <= vp_start_ || b.width < 1.0f) return;

        const float cy = b.height * 0.5f;
        const float half_h = b.height * 0.5f;
        canvas.set_stroke_color(line_color_);
        canvas.set_line_width(1.0f);

        const uint64_t span = vp_end_ - vp_start_;
        const int cols = static_cast<int>(b.width);
        for (int px = 0; px < cols; ++px) {
            const uint64_t cf0 =
                vp_start_ + span * static_cast<uint64_t>(px) / static_cast<uint64_t>(cols);
            uint64_t cf1 =
                vp_start_ + span * static_cast<uint64_t>(px + 1) / static_cast<uint64_t>(cols);
            if (cf1 <= cf0) cf1 = cf0 + 1;

            float lo = 0.0f, hi = 0.0f;
            if (!column_peak(cf0, cf1, lo, hi)) continue;

            const float x = static_cast<float>(px) + 0.5f;
            const float y_top = cy - hi * half_h;
            float y_bot = cy - lo * half_h;
            if (y_bot - y_top < 1.0f) y_bot = y_top + 1.0f;  // keep 1px minimum
            canvas.stroke_line(x, y_top, x, y_bot);
        }
    }

private:
    const pulp::audio::WaveformOverview* overview_ = nullptr;
    uint32_t channel_ = pulp::audio::WaveformOverview::kAllChannels;
    uint64_t vp_start_ = 0;
    uint64_t vp_end_ = 0;
    canvas::Color line_color_ = canvas::Color::rgba8(100, 180, 250);
    canvas::Color bg_color_ = canvas::Color::rgba8(20, 20, 30);
};

}  // namespace pulp::view
