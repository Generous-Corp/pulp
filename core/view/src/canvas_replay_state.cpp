#include "canvas_replay_state.hpp"

namespace pulp::view::detail {

namespace {

void apply_fill_paint(canvas::Canvas& canvas, const CanvasDrawCmd& cmd) {
    const auto* colors = cmd.gradient_colors.data();
    const auto* stops = cmd.gradient_positions.data();
    const int count = static_cast<int>(cmd.gradient_colors.size());
    using T = CanvasDrawCmd::Type;
    switch (cmd.type) {
        case T::set_fill_gradient_linear:
            if (count > 0)
                canvas.set_fill_gradient_linear(cmd.x, cmd.y, cmd.x2, cmd.y2,
                                                colors, stops, count);
            break;
        case T::set_fill_gradient_radial:
            if (count > 0)
                canvas.set_fill_gradient_radial(cmd.x, cmd.y, cmd.extra,
                                                colors, stops, count);
            break;
        // Inner circle in (x, y, extra), outer circle in (x2, y2, w).
        case T::set_fill_gradient_radial_two_circles:
            if (count > 0)
                canvas.set_fill_gradient_radial_two_circles(
                    cmd.x, cmd.y, cmd.extra, cmd.x2, cmd.y2, cmd.w,
                    colors, stops, count);
            break;
        // Skia routes through SkGradientShader::MakeSweep; CoreGraphics
        // software-rasterizes a conic image because it has no conic shader.
        case T::set_fill_gradient_conic:
            if (count > 0)
                canvas.set_fill_gradient_conic(cmd.x, cmd.y, cmd.extra,
                                               colors, stops, count);
            break;
        case T::clear_fill_gradient:
            canvas.clear_fill_gradient();
            break;
        // Image source in `text`; tile bits in int_val (bit 0 = x, bit 1 = y;
        // 0 = repeat, 1 = no-repeat).
        case T::set_fill_pattern: {
            using Tile = canvas::Canvas::PatternTileMode;
            canvas.set_fill_pattern(cmd.text,
                                    (cmd.int_val & 0x1) ? Tile::no_repeat : Tile::repeat,
                                    (cmd.int_val & 0x2) ? Tile::no_repeat : Tile::repeat);
            break;
        }
        default:
            break;
    }
}

// Stroke paint servers mirror the fill ones. SkiaCanvas stores them in
// `stroke_shader_`; backends without stroke shaders degrade to the first
// stop's colour, and stroke patterns without a backend implementation fall
// back to the solid stroke colour.
void apply_stroke_paint(canvas::Canvas& canvas, const CanvasDrawCmd& cmd) {
    const auto* colors = cmd.gradient_colors.data();
    const auto* stops = cmd.gradient_positions.data();
    const int count = static_cast<int>(cmd.gradient_colors.size());
    using T = CanvasDrawCmd::Type;
    switch (cmd.type) {
        case T::set_stroke_gradient_linear:
            if (count > 0)
                canvas.set_stroke_gradient_linear(cmd.x, cmd.y, cmd.x2, cmd.y2,
                                                  colors, stops, count);
            break;
        case T::set_stroke_gradient_radial:
            if (count > 0)
                canvas.set_stroke_gradient_radial(cmd.x, cmd.y, cmd.extra,
                                                  colors, stops, count);
            break;
        case T::set_stroke_gradient_radial_two_circles:
            if (count > 0)
                canvas.set_stroke_gradient_radial_two_circles(
                    cmd.x, cmd.y, cmd.extra, cmd.x2, cmd.y2, cmd.w,
                    colors, stops, count);
            break;
        case T::set_stroke_gradient_conic:
            if (count > 0)
                canvas.set_stroke_gradient_conic(cmd.x, cmd.y, cmd.extra,
                                                 colors, stops, count);
            break;
        case T::clear_stroke_gradient:
            canvas.clear_stroke_gradient();
            break;
        case T::set_stroke_pattern: {
            using Tile = canvas::Canvas::PatternTileMode;
            canvas.set_stroke_pattern(cmd.text,
                                      (cmd.int_val & 0x1) ? Tile::no_repeat : Tile::repeat,
                                      (cmd.int_val & 0x2) ? Tile::no_repeat : Tile::repeat);
            break;
        }
        default:
            break;
    }
}

} // namespace

std::optional<CanvasReplayState::Slot> CanvasReplayState::slot_for(CanvasDrawCmd::Type type) {
    using T = CanvasDrawCmd::Type;
    switch (type) {
        case T::set_font:
        case T::set_font_full:            return font;
        case T::set_text_align:           return text_align;
        case T::set_line_cap:             return line_cap;
        case T::set_line_join:            return line_join;
        case T::set_miter_limit:          return miter_limit;
        case T::set_image_smoothing:      return image_smoothing;
        case T::set_global_alpha:         return global_alpha;
        case T::set_blend_mode:           return blend_mode;
        case T::set_shadow_color:         return shadow_color;
        case T::set_shadow_blur:          return shadow_blur;
        case T::set_shadow_offset_x:      return shadow_offset_x;
        case T::set_shadow_offset_y:      return shadow_offset_y;
        case T::set_direction:            return direction;
        case T::set_filter:               return filter;
        case T::set_line_dash:            return line_dash;
        case T::set_fill_gradient_linear:
        case T::set_fill_gradient_radial:
        case T::set_fill_gradient_radial_two_circles:
        case T::set_fill_gradient_conic:
        case T::set_fill_pattern:
        case T::clear_fill_gradient:      return fill_paint;
        case T::set_stroke_gradient_linear:
        case T::set_stroke_gradient_radial:
        case T::set_stroke_gradient_radial_two_circles:
        case T::set_stroke_gradient_conic:
        case T::set_stroke_pattern:
        case T::clear_stroke_gradient:    return stroke_paint;
        default:                          return std::nullopt;
    }
}

void apply_canvas_state_setter(canvas::Canvas& canvas, const CanvasDrawCmd& cmd) {
    using T = CanvasDrawCmd::Type;
    switch (cmd.type) {
        case T::set_font:
            canvas.set_font(cmd.text, cmd.extra);
            break;
        // Full CSS font shorthand: weight in `x`, slant in `y`, letter
        // spacing in `x2`. Skia honours weight and slant; CoreGraphics falls
        // back to family and size through the base default.
        case T::set_font_full:
            canvas.set_font_full(cmd.text, cmd.extra, static_cast<int>(cmd.x),
                                 static_cast<int>(cmd.y), cmd.x2);
            break;
        case T::set_text_align:
            if (cmd.int_val == 1) canvas.set_text_align(canvas::TextAlign::center);
            else if (cmd.int_val == 2) canvas.set_text_align(canvas::TextAlign::right);
            else canvas.set_text_align(canvas::TextAlign::left);
            break;
        case T::set_line_cap:
            if (cmd.int_val == 1) canvas.set_line_cap(canvas::LineCap::round);
            else if (cmd.int_val == 2) canvas.set_line_cap(canvas::LineCap::square);
            else canvas.set_line_cap(canvas::LineCap::butt);
            break;
        case T::set_line_join:
            if (cmd.int_val == 1) canvas.set_line_join(canvas::LineJoin::round);
            else if (cmd.int_val == 2) canvas.set_line_join(canvas::LineJoin::bevel);
            else canvas.set_line_join(canvas::LineJoin::miter);
            break;
        case T::set_miter_limit:
            canvas.set_miter_limit(cmd.extra);
            break;
        // Enabled in int_val; quality in `extra` (0 = low, 1 = medium, 2 = high).
        case T::set_image_smoothing: {
            using Q = canvas::Canvas::ImageSmoothingQuality;
            const int q = static_cast<int>(cmd.extra);
            canvas.set_image_smoothing(cmd.int_val != 0,
                                       q == 1 ? Q::medium : q == 2 ? Q::high : Q::low);
            break;
        }
        case T::set_global_alpha:
            canvas.set_opacity(cmd.extra);
            break;
        case T::set_blend_mode:
            canvas.set_blend_mode(static_cast<canvas::Canvas::BlendMode>(cmd.int_val));
            break;
        case T::set_shadow_color:
            canvas.set_shadow_color(cmd.color);
            break;
        case T::set_shadow_blur:
            canvas.set_shadow_blur(cmd.extra);
            break;
        case T::set_shadow_offset_x:
            canvas.set_shadow_offset_x(cmd.extra);
            break;
        case T::set_shadow_offset_y:
            canvas.set_shadow_offset_y(cmd.extra);
            break;
        // Direction enum in int_val (0 = ltr, 1 = rtl, 2 = inherit).
        case T::set_direction: {
            using D = canvas::Canvas::TextDirection;
            canvas.set_direction(cmd.int_val == 1 ? D::rtl
                                 : cmd.int_val == 2 ? D::inherit : D::ltr);
            break;
        }
        // Raw CSS <filter-function-list> in `text`.
        case T::set_filter:
            canvas.set_filter(cmd.text);
            break;
        // Dash pattern in gradient_positions, phase in `extra`.
        case T::set_line_dash:
            canvas.set_line_dash(cmd.gradient_positions.data(),
                                 static_cast<int>(cmd.gradient_positions.size()),
                                 cmd.extra);
            break;
        default:
            if (auto slot = CanvasReplayState::slot_for(cmd.type)) {
                if (*slot == CanvasReplayState::fill_paint) apply_fill_paint(canvas, cmd);
                else if (*slot == CanvasReplayState::stroke_paint) apply_stroke_paint(canvas, cmd);
            }
            break;
    }
}

const CanvasDrawCmd& CanvasReplayState::clear_fill_paint() {
    static const CanvasDrawCmd cmd = [] {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_fill_gradient;
        return c;
    }();
    return cmd;
}

const CanvasDrawCmd& CanvasReplayState::clear_stroke_paint() {
    static const CanvasDrawCmd cmd = [] {
        CanvasDrawCmd c;
        c.type = CanvasDrawCmd::Type::clear_stroke_gradient;
        return c;
    }();
    return cmd;
}

void CanvasReplayState::restore(canvas::Canvas& canvas) {
    if (stack_.empty()) return;
    State saved = stack_.back();
    stack_.pop_back();

    // Colour first: a solid fill colour clears the fill paint server on
    // every backend, so a saved gradient or pattern must be re-applied after
    // it.
    bool fill_color_reapplied = false;
    if (saved.fill_color && saved.fill_color != current_.fill_color) {
        canvas.set_fill_color(*saved.fill_color);
        fill_color_reapplied = true;
    }
    if (saved.stroke_color && saved.stroke_color != current_.stroke_color)
        canvas.set_stroke_color(*saved.stroke_color);
    if (saved.line_width && saved.line_width != current_.line_width)
        canvas.set_line_width(*saved.line_width);

    for (std::size_t s = 0; s < slot_count; ++s) {
        const CanvasDrawCmd* want = saved.setters[s];
        const bool repaint_fill = (s == fill_paint) && fill_color_reapplied;
        if (want == current_.setters[s] && !repaint_fill) continue;
        if (!want) {
            if (s == fill_paint) want = &clear_fill_paint();
            else if (s == stroke_paint) want = &clear_stroke_paint();
            else continue;
            saved.setters[s] = want;
        }
        // set_fill_color above already cleared the fill paint server.
        if (repaint_fill && want == &clear_fill_paint()) continue;
        apply_canvas_state_setter(canvas, *want);
    }
    current_ = saved;
}

void CanvasReplayState::reassert_fill_and_font(canvas::Canvas& canvas) const {
    if (current_.fill_color) canvas.set_fill_color(*current_.fill_color);
    const auto* paint = current_.setters[fill_paint];
    if (paint && paint != &clear_fill_paint()) apply_canvas_state_setter(canvas, *paint);
    if (const auto* f = current_.setters[font]) apply_canvas_state_setter(canvas, *f);
}

} // namespace pulp::view::detail
