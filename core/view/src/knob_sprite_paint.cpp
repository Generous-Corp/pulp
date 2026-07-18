#include "knob_sprite_paint.hpp"

#include <algorithm>
#include <cmath>

namespace pulp::view {

// Rotating indicator notch, shared by the silver vector knob and the
// single-frame sprite-body knob. Draws a short radial line at the value's
// angle (value 0..1 → [-135°, +135°], the analog-synth convention), centered
// at (cx, cy): a dark backing stroke for contrast plus the bright pointer on
// top. `notch_r` is the extent — the line runs from 35% to 95% of it; the two
// stroke widths scale from `width_ref`. Factored out of the silver path so an
// imported sprite knob (a captured static disc + a separate pointer) can still
// show a turning pointer drawn natively over the disc.
void draw_knob_indicator_notch(canvas::Canvas& canvas,
                               float cx, float cy,
                               float notch_r, float width_ref,
                               float value) {
    const canvas::Color kBacking   = canvas::Color::rgba(0.10f, 0.11f, 0.13f, 0.85f);
    const canvas::Color kIndicator = canvas::Color::rgba(0.97f, 0.97f, 0.97f, 1.0f);
    float angle = -1.5707963f /* -90° */
                  + (value - 0.5f) * 4.7123890f /* 270° total range */;
    float outer_x = cx + notch_r * 0.95f * std::cos(angle);
    float outer_y = cy + notch_r * 0.95f * std::sin(angle);
    float inner_x = cx + notch_r * 0.35f * std::cos(angle);
    float inner_y = cy + notch_r * 0.35f * std::sin(angle);
    // Subtle dark backing line for contrast on the bright top arc.
    canvas.set_stroke_color(kBacking);
    canvas.set_line_width(std::max(2.5f, width_ref * 0.10f));
    canvas.stroke_line(inner_x, inner_y, outer_x, outer_y);
    // Bright top line — the actual indicator.
    canvas.set_stroke_color(kIndicator);
    canvas.set_line_width(std::max(1.5f, width_ref * 0.07f));
    canvas.stroke_line(inner_x, inner_y, outer_x, outer_y);
}

// Pointer reproduced from the design's OWN indicator node (set_captured_indicator).
// Same [-135°,+135°] value→angle arc as the synthetic notch, but the radii, width
// and color come from the imported art, and it pivots at the disc core center
// (cx, cy) — so the line rides the disc's baked min/center/max reference ticks
// instead of a guessed sweep. A faint dark backing keeps a hairline legible on a
// bright metallic face without reading as a second line.
void draw_knob_captured_pointer(canvas::Canvas& canvas,
                                float cx, float cy,
                                float r_in, float r_out, float width,
                                canvas::Color color, float value) {
    float angle = -1.5707963f + (value - 0.5f) * 4.7123890f;
    float ox = cx + r_out * std::cos(angle);
    float oy = cy + r_out * std::sin(angle);
    float ix = cx + r_in  * std::cos(angle);
    float iy = cy + r_in  * std::sin(angle);
    canvas.set_stroke_color(canvas::Color::rgba(0.10f, 0.11f, 0.13f, 0.45f));
    canvas.set_line_width(width + 1.25f);
    canvas.stroke_line(ix, iy, ox, oy);
    canvas.set_stroke_color(color);
    canvas.set_line_width(width);
    canvas.stroke_line(ix, iy, ox, oy);
}

}  // namespace pulp::view
