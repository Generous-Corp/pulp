#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/overlay_palette.hpp>
#include <pulp/vellum_components/render_pass_stats.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

namespace pulp::vellum_components {

bool InspectorOverlay::capture_pass_frame() {
    return pass_source_ && pass_history_.capture(*pass_source_);
}

void InspectorOverlay::paint_pass_panel(canvas::Canvas& canvas) {
    const float root_w = root_.bounds().width;
    const float root_h = root_.bounds().height;

    // Only passes that have recorded a frame get an entry.
    const auto attribution = pass_history_.attribution();
    std::size_t rows = 0;
    for (const auto& a : attribution)
        if (a.samples > 0)
            ++rows;

    const float w = kPanelWidth;
    const float x = root_w - w - kPanelMargin;
    const float y = kPanelMargin;
    const float h = std::min(overlay::pass_panel_height(rows, pass_source_ != nullptr),
                             std::max(0.0f, root_h - 2 * kPanelMargin));
    canvas.save();
    canvas.set_fill_color(overlay::kPanelBackground);
    canvas.fill_rounded_rect(x, y, w, h, 4.0f);
    canvas.clip_rect(x, y, w, h);

    const float left = x + kPanelPadding;
    const float inner_w = w - 2 * kPanelPadding;
    canvas.set_font("monospace", kFontSize);
    float line_y = y + 4;
    constexpr float kLine = 15.0f;

    canvas.set_fill_color(overlay::kPanelHeading);
    canvas.fill_text("Render Passes (P)", left, line_y + 11);
    line_y += kLine;
    // Say what the numbers are, so nobody reads them as GPU time.
    canvas.set_fill_color(overlay::kPanelDim);
    canvas.fill_text("cpu time \xc2\xb7 wall-clock, not GPU", left, line_y + 10);
    line_y += kLine + 2;

    if (!pass_source_) {
        canvas.fill_text("No render pass source attached", left, line_y + 11);
        canvas.restore();
        return;
    }

    {
        std::ostringstream totals;
        totals << std::fixed << std::setprecision(2) << pass_source_->total_time_ms() << "ms / "
               << std::setprecision(1) << pass_source_->budget_ms() << "ms budget";
        canvas.set_fill_color(pass_source_->over_budget() ? overlay::kPanelWarn
                                                          : overlay::kPanelText);
        canvas.fill_text(totals.str(), left, line_y + 11);
        line_y += kLine;

        std::ostringstream counts;
        counts << pass_history_.overrun_count() << " overruns \xc2\xb7 "
               << pass_history_.frames_captured() << " frames";
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text(counts.str(), left, line_y + 11);
        line_y += kLine + 4;
    }

    // Sparkline bars scale to the worst single sample of any pass, so the
    // entries are comparable with each other.
    float global_peak = 0.001f;
    for (const auto& a : attribution)
        global_peak = std::max(global_peak, a.peak_cpu_ms);

    bool any = false;
    for (const auto& a : attribution) {
        if (a.samples == 0)
            continue;
        any = true;
        if (line_y > y + h - kLine * 2)
            break; // out of room
        const auto& colour = overlay::kPassColors[a.type];

        // A colour chip and the name, dimmed when the pass skipped the latest
        // frame.
        canvas.set_fill_color(colour);
        canvas.fill_rounded_rect(left, line_y + 2, kPassChipWidth, 10, 2);
        canvas.set_fill_color(a.present ? overlay::kPanelText : overlay::kPanelDim);
        canvas.fill_text(a.name, left + 14, line_y + 11);

        std::ostringstream stat;
        stat << std::fixed << std::setprecision(2) << a.last_cpu_ms << " ~" << a.avg_cpu_ms << " ^"
             << a.peak_cpu_ms << "ms";
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text(stat.str(), left + 90, line_y + 11);
        line_y += kLine;

        std::ostringstream draws;
        draws << a.last_draw_calls << " draws (peak " << a.peak_draw_calls << ")";
        canvas.fill_text(draws.str(), left + 14, line_y + 10);
        line_y += kLine;

        // The history, oldest on the left.
        const auto history = pass_history_.cpu_ms_history(a.type);
        constexpr float kSparkHeight = 14.0f;
        const float spark_w = std::min(inner_w - 14.0f, 220.0f);
        const float bar_w = spark_w / static_cast<float>(overlay::kPassHistoryFrames);
        canvas.set_fill_color(overlay::kSparkTrack);
        canvas.fill_rect(left + 14, line_y, spark_w, kSparkHeight);
        canvas.set_fill_color(colour);
        for (std::size_t k = 0; k < history.size(); ++k) {
            const float bar_h = std::min(history[k] / global_peak, 1.0f) * kSparkHeight;
            canvas.fill_rect(left + 14 + static_cast<float>(k) * bar_w,
                             line_y + kSparkHeight - bar_h, std::max(bar_w - 1.0f, 1.0f), bar_h);
        }
        line_y += kSparkHeight + 6;
    }
    if (!any) {
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text("Awaiting render frames\xe2\x80\xa6", left, line_y + 11);
    }
    canvas.restore();
}

} // namespace pulp::vellum_components
