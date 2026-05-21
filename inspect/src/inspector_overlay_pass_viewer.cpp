// inspector_overlay_pass_viewer.cpp — Phase 6.1 per-pass GPU/render
// attribution viewer for the visual inspector overlay.
//
// Extracted from inspector_overlay.cpp in the 2026-05 refactor (roadmap
// P10-2). Pure mechanical move — the InspectorOverlay member methods
// below are byte-identical to their previous definitions in
// inspector_overlay.cpp; only their translation unit changed. The
// file-local kPassTypeNames / kPassTypeColors tables stay private to
// this TU. Shared color constants live in
// inspector_overlay_internal.hpp; the structural constants
// (kPassTypeCount, kPassHistoryFrames) are static-constexpr members of
// InspectorOverlay reached through the public header.

#include "inspector_overlay_internal.hpp"

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/render/render_pass.hpp>  // full RenderPassManager definition

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace pulp::inspect {

// ── Phase 6.1 — Per-pass GPU/render attribution viewer ──────────────────────
//
// Surfaces where render time goes, broken down by render pass, over a
// rolling 60-frame window. Reads RenderPassManager's existing per-pass
// PassStats — CPU wall-time + draw-call counts. True GPU timestamps are
// deferred to Phase 6.5 (Dawn timestamp queries); the panel labels its
// numbers "cpu" so the distinction is honest and explicit.

namespace {

// Stable human-readable names for the five RenderPassType values, in
// declaration order so index == static_cast<size_t>(type).
constexpr std::array<const char*, 5> kPassTypeNames = {
    "background", "content", "effects", "overlay", "post"
};

// Color-code each pass type so the panel reads at a glance — cool hues
// for the cheap structural passes, warm for the expensive ones.
const std::array<Color, 5> kPassTypeColors = {
    Color::rgba(0.40f, 0.55f, 0.85f, 1.0f),  // background — blue
    Color::rgba(0.45f, 0.80f, 0.55f, 1.0f),  // content    — green
    Color::rgba(0.90f, 0.70f, 0.30f, 1.0f),  // effects    — amber
    Color::rgba(0.85f, 0.45f, 0.80f, 1.0f),  // overlay    — magenta
    Color::rgba(0.90f, 0.45f, 0.35f, 1.0f),  // post       — red
};

} // namespace

bool InspectorOverlay::capture_pass_frame() {
    if (!rpm_) return false;

    // De-dup: only capture once per render-pass-manager frame. paint()
    // can run multiple times for the same frame (e.g. partial redraw),
    // and we don't want those to fill the history with copies of one
    // frame's numbers. A frame_count() of 0 means begin_frame() was
    // never called — treat that as "no frame to capture yet".
    const std::uint64_t frame = rpm_->frame_count();
    if (frame == 0 || frame == last_captured_frame_) return false;
    last_captured_frame_ = frame;
    ++pass_frames_captured_;

    if (rpm_->over_budget()) ++budget_overrun_count_;

    // Mark every pass type absent for this frame; the loop below flips
    // back on the ones that actually rendered.
    for (auto& ring : pass_rings_) ring.present_last_frame = false;

    // The manager can emit the same pass type more than once per frame
    // (e.g. two overlay passes). Accumulate per type so the history
    // sample is the frame's TOTAL cost for that pass type.
    std::array<float, kPassTypeCount> frame_cpu_ms{};
    std::array<int, kPassTypeCount> frame_draw_calls{};
    std::array<bool, kPassTypeCount> frame_seen{};
    for (const auto& p : rpm_->passes()) {
        auto idx = static_cast<std::size_t>(p.type);
        if (idx >= kPassTypeCount) continue;  // defensive — unknown enum.
        frame_cpu_ms[idx] += p.time_ms;
        frame_draw_calls[idx] += p.draw_calls;
        frame_seen[idx] = true;
    }

    for (std::size_t i = 0; i < kPassTypeCount; ++i) {
        if (!frame_seen[i]) continue;  // pass absent this frame — no sample.
        auto& ring = pass_rings_[i];
        ring.cpu_ms[ring.head] = frame_cpu_ms[i];
        ring.draw_calls[ring.head] = frame_draw_calls[i];
        ring.head = (ring.head + 1) % kPassHistoryFrames;
        if (ring.count < kPassHistoryFrames) ++ring.count;
        ring.present_last_frame = true;
    }
    return true;
}

std::vector<InspectorOverlay::PassAttribution>
InspectorOverlay::pass_attribution() const {
    std::vector<PassAttribution> out;
    out.reserve(kPassTypeCount);
    for (std::size_t i = 0; i < kPassTypeCount; ++i) {
        const auto& ring = pass_rings_[i];
        PassAttribution a;
        a.type = static_cast<int>(i);
        a.name = kPassTypeNames[i];
        a.samples = ring.count;
        a.present = ring.present_last_frame;
        if (ring.count > 0) {
            // The most recent sample sits one slot behind head (mod N).
            std::size_t last = (ring.head + kPassHistoryFrames - 1) % kPassHistoryFrames;
            a.last_cpu_ms = ring.cpu_ms[last];
            a.last_draw_calls = ring.draw_calls[last];
            float sum = 0.0f;
            for (std::size_t k = 0; k < ring.count; ++k) {
                float v = ring.cpu_ms[k];
                sum += v;
                if (v > a.peak_cpu_ms) a.peak_cpu_ms = v;
                if (ring.draw_calls[k] > a.peak_draw_calls)
                    a.peak_draw_calls = ring.draw_calls[k];
            }
            a.avg_cpu_ms = sum / static_cast<float>(ring.count);
        }
        out.push_back(a);
    }
    return out;
}

void InspectorOverlay::paint_pass_attribution(Canvas& canvas, float x, float y,
                                              float w, float h) {
    canvas.set_font("monospace", kFontSize);
    float line_y = y + 4;
    const float line_h = 15.0f;

    // Heading + honesty note about CPU-vs-GPU timing.
    canvas.set_fill_color(kHighlightStroke);
    canvas.fill_text("Render Passes (P)", x, line_y + 11);
    line_y += line_h;
    canvas.set_fill_color(kPanelDim);
    canvas.fill_text("cpu time \xc2\xb7 GPU timestamps: Phase 6.5", x, line_y + 10);
    line_y += line_h + 2;

    if (!rpm_) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("No RenderPassManager attached", x, line_y + 11);
        return;
    }

    // Frame summary line: total CPU time, budget, overrun count.
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << rpm_->total_time_ms() << "ms / "
           << std::setprecision(1) << rpm_->budget() << "ms budget";
        canvas.set_fill_color(rpm_->over_budget() ? kStatsWarn : kPanelText);
        canvas.fill_text(ss.str(), x, line_y + 11);
        line_y += line_h;

        std::ostringstream ss2;
        ss2 << budget_overrun_count_ << " overruns \xc2\xb7 "
            << pass_frames_captured_ << " frames";
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(ss2.str(), x, line_y + 11);
        line_y += line_h + 4;
    }

    auto attribution = pass_attribution();

    // The trend sparkline scales relative to the worst single-pass
    // CPU sample across all passes so bars are comparable to each other.
    float global_peak = 0.001f;
    for (const auto& a : attribution)
        if (a.peak_cpu_ms > global_peak) global_peak = a.peak_cpu_ms;

    bool any = false;
    for (std::size_t i = 0; i < attribution.size(); ++i) {
        const auto& a = attribution[i];
        if (a.samples == 0) continue;  // pass never rendered — skip row.
        any = true;
        if (line_y > y + h - line_h * 2) break;  // out of panel space.

        const Color& pass_color = kPassTypeColors[i];

        // Color-coded pass-type chip + name. Dim the name when the pass
        // was absent from the most recent frame (history but quiet now).
        canvas.set_fill_color(pass_color);
        canvas.fill_rounded_rect(x, line_y + 2, 8, 10, 2);
        canvas.set_fill_color(a.present ? kPanelText : kPanelDim);
        canvas.fill_text(a.name, x + 14, line_y + 11);

        // last / avg / peak CPU ms, right-aligned-ish in a fixed column.
        std::ostringstream stat;
        stat << std::fixed << std::setprecision(2)
             << a.last_cpu_ms << " ~" << a.avg_cpu_ms
             << " ^" << a.peak_cpu_ms << "ms";
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(stat.str(), x + 90, line_y + 11);
        line_y += line_h;

        // Draw-call line.
        std::ostringstream dc;
        dc << a.last_draw_calls << " draws (peak " << a.peak_draw_calls << ")";
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(dc.str(), x + 14, line_y + 10);
        line_y += line_h;

        // 60-frame CPU-time sparkline. Walk the ring oldest→newest so
        // the bars read left-to-right in chronological order.
        const auto& ring = pass_rings_[i];
        if (ring.count > 0) {
            const float spark_h = 14.0f;
            const float spark_w = std::min(w - 14.0f, 220.0f);
            const float bar_w = spark_w / static_cast<float>(kPassHistoryFrames);
            const float base_y = line_y + spark_h;
            canvas.set_fill_color(Color::rgba(0, 0, 0, 0.35f));
            canvas.fill_rect(x + 14, line_y, spark_w, spark_h);
            std::size_t start = (ring.head + kPassHistoryFrames - ring.count)
                                % kPassHistoryFrames;
            for (std::size_t k = 0; k < ring.count; ++k) {
                float v = ring.cpu_ms[(start + k) % kPassHistoryFrames];
                float frac = v / global_peak;
                if (frac > 1.0f) frac = 1.0f;
                float bh = frac * spark_h;
                canvas.set_fill_color(pass_color);
                canvas.fill_rect(x + 14 + static_cast<float>(k) * bar_w,
                                 base_y - bh, std::max(bar_w - 1.0f, 1.0f), bh);
            }
            line_y += spark_h + 6;
        }
    }

    if (!any) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("Awaiting render frames\xe2\x80\xa6", x, line_y + 11);
    }
}

} // namespace pulp::inspect
