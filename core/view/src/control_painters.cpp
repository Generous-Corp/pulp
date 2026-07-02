#include <pulp/view/control_painters.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::view::painters {

namespace {
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
}  // namespace

void paint_mod_ring_knob(canvas::Canvas& canvas, const Rect& rect, float value,
                         const KnobStyle& style) {
    value = clamp01(value);
    const Point c = rect.center();
    const float radius = std::min(rect.width, rect.height) * style.radius_scale;
    if (radius <= 0.0f) return;

    const float start = style.start_angle_deg * kDegToRad;
    const float full_end = (style.start_angle_deg + style.sweep_deg) * kDegToRad;
    const float active_end = (style.start_angle_deg + style.sweep_deg * value) * kDegToRad;

    canvas.set_line_width(style.ring_width);
    // Full inactive track arc.
    canvas.set_stroke_color(style.track);
    canvas.stroke_arc(c.x, c.y, radius, start, full_end);
    // Active value arc — emitted LAST so its sweep (active_end - start) is the
    // value-dependent geometry a test reads off the final stroke_arc.
    canvas.set_stroke_color(style.ring);
    canvas.stroke_arc(c.x, c.y, radius, start, active_end);

    // Pointer line from center out to the value angle.
    const float ix = c.x + radius * std::cos(active_end);
    const float iy = c.y + radius * std::sin(active_end);
    canvas.set_stroke_color(style.indicator);
    canvas.set_line_width(style.indicator_width);
    canvas.stroke_line(c.x, c.y, ix, iy);
}

void paint_level_fader(canvas::Canvas& canvas, const Rect& rect, float value,
                       const FaderStyle& style) {
    value = clamp01(value);
    const float half_track = style.track_thickness * 0.5f;

    if (style.horizontal) {
        const float cy = rect.center().y;
        // Track.
        canvas.set_fill_color(style.track);
        canvas.fill_rounded_rect(rect.x, cy - half_track, rect.width,
                                 style.track_thickness, style.corner_radius);
        // Fill from left to value.
        const float fill_w = rect.width * value;
        canvas.set_fill_color(style.fill);
        canvas.fill_rounded_rect(rect.x, cy - half_track, fill_w,
                                 style.track_thickness, style.corner_radius);
        // Thumb (LAST fill_rounded_rect) at value along x.
        const float tx = rect.x + rect.width * value - style.thumb_length * 0.5f;
        canvas.set_fill_color(style.thumb);
        canvas.fill_rounded_rect(tx, rect.y, style.thumb_length, rect.height,
                                 style.corner_radius);
    } else {
        const float cx = rect.center().x;
        canvas.set_fill_color(style.track);
        canvas.fill_rounded_rect(cx - half_track, rect.y, style.track_thickness,
                                 rect.height, style.corner_radius);
        // Fill from bottom up to value.
        const float fill_h = rect.height * value;
        canvas.set_fill_color(style.fill);
        canvas.fill_rounded_rect(cx - half_track, rect.bottom() - fill_h,
                                 style.track_thickness, fill_h, style.corner_radius);
        // Thumb (LAST fill_rounded_rect): value 1 -> top, 0 -> bottom.
        const float ty = rect.bottom() - rect.height * value - style.thumb_length * 0.5f;
        canvas.set_fill_color(style.thumb);
        canvas.fill_rounded_rect(rect.x, ty, rect.width, style.thumb_length,
                                 style.corner_radius);
    }
}

void paint_range_slider(canvas::Canvas& canvas, const Rect& rect,
                        float lo, float hi, const RangeSliderStyle& style) {
    lo = clamp01(lo);
    hi = clamp01(hi);
    if (hi < lo) std::swap(lo, hi);
    const float half_track = style.track_thickness * 0.5f;

    if (style.horizontal) {
        const float cy = rect.center().y;
        const float lo_x = rect.x + rect.width * lo;
        const float hi_x = rect.x + rect.width * hi;
        canvas.set_fill_color(style.track);
        canvas.fill_rounded_rect(rect.x, cy - half_track, rect.width,
                                 style.track_thickness, half_track);
        canvas.set_fill_color(style.fill);
        canvas.fill_rounded_rect(lo_x, cy - half_track, hi_x - lo_x,
                                 style.track_thickness, half_track);
        canvas.set_fill_color(style.thumb);
        canvas.fill_circle(lo_x, cy, style.thumb_radius);   // first = lo
        canvas.fill_circle(hi_x, cy, style.thumb_radius);   // second = hi
    } else {
        const float cx = rect.center().x;
        // Vertical: value 0 = bottom, so lo/hi map from the bottom up.
        const float lo_y = rect.bottom() - rect.height * lo;
        const float hi_y = rect.bottom() - rect.height * hi;
        canvas.set_fill_color(style.track);
        canvas.fill_rounded_rect(cx - half_track, rect.y, style.track_thickness,
                                 rect.height, half_track);
        canvas.set_fill_color(style.fill);
        canvas.fill_rounded_rect(cx - half_track, hi_y, style.track_thickness,
                                 lo_y - hi_y, half_track);
        canvas.set_fill_color(style.thumb);
        canvas.fill_circle(cx, lo_y, style.thumb_radius);   // first = lo
        canvas.fill_circle(cx, hi_y, style.thumb_radius);   // second = hi
    }
}

void paint_toggle(canvas::Canvas& canvas, const Rect& rect, bool on,
                  const ToggleStyle& style) {
    const float r = rect.height * 0.5f;
    canvas.set_fill_color(on ? style.track_on : style.track_off);
    canvas.fill_rounded_rect(rect.x, rect.y, rect.width, rect.height, r);
    // Knob (LAST fill_circle): left when off, right when on.
    const float knob_r = r - style.knob_inset;
    const float off_x = rect.x + style.knob_inset + knob_r;
    const float on_x = rect.right() - style.knob_inset - knob_r;
    canvas.set_fill_color(style.knob);
    canvas.fill_circle(on ? on_x : off_x, rect.center().y, knob_r);
}

void paint_waveform(canvas::Canvas& canvas, const Rect& rect,
                    const float* samples, std::size_t count,
                    const WaveformStyle& style) {
    if (!samples || count < 2 || rect.width <= 0.0f) return;
    const float cy = rect.center().y;
    const float half_h = rect.height * 0.5f;

    if (style.draw_baseline) {
        canvas.set_stroke_color(style.baseline);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(rect.x, cy, rect.right(), cy);
    }

    std::vector<canvas::Canvas::Point2D> pts;
    pts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        const float x = rect.x + rect.width * t;
        const float s = std::clamp(samples[i], -1.0f, 1.0f);
        pts.push_back({x, cy - s * half_h});   // +1 -> top, -1 -> bottom
    }
    canvas.set_stroke_color(style.line);
    canvas.set_line_width(style.line_width);
    canvas.stroke_path(pts.data(), pts.size());
}

}  // namespace pulp::view::painters
