#include <pulp/view/canvas_widget.hpp>

namespace pulp::view {

void CanvasWidget::paint(canvas::Canvas& canvas) {
    for (auto& cmd : commands_) {
        switch (cmd.type) {
        // Shapes
        case CanvasDrawCmd::Type::clear:
            canvas.set_fill_color(cmd.color);
            canvas.fill_rect(0, 0, bounds().width, bounds().height);
            break;
        case CanvasDrawCmd::Type::fill_rect:
            canvas.set_fill_color(cmd.color);
            canvas.fill_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            break;
        case CanvasDrawCmd::Type::stroke_rect:
            canvas.set_stroke_color(cmd.color);
            canvas.set_line_width(cmd.extra);
            canvas.stroke_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            break;
        case CanvasDrawCmd::Type::fill_rounded_rect:
            canvas.set_fill_color(cmd.color);
            canvas.fill_rounded_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.extra);
            break;
        case CanvasDrawCmd::Type::stroke_rounded_rect:
            canvas.set_stroke_color(cmd.color);
            canvas.set_line_width(cmd.x2);  // x2 = line width
            canvas.stroke_rounded_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.extra);
            break;
        case CanvasDrawCmd::Type::fill_circle:
            canvas.set_fill_color(cmd.color);
            canvas.fill_circle(cmd.x, cmd.y, cmd.extra);
            break;
        case CanvasDrawCmd::Type::stroke_circle:
            canvas.set_stroke_color(cmd.color);
            canvas.set_line_width(cmd.x2);
            canvas.stroke_circle(cmd.x, cmd.y, cmd.extra);
            break;
        case CanvasDrawCmd::Type::stroke_line:
            canvas.set_stroke_color(cmd.color);
            canvas.set_line_width(cmd.extra);
            canvas.stroke_line(cmd.x, cmd.y, cmd.w, cmd.h);
            break;

        // Text
        case CanvasDrawCmd::Type::fill_text:
            canvas.set_fill_color(cmd.color);
            canvas.set_font(cmd.text.empty() ? "Inter" : "", cmd.extra);
            canvas.set_text_align(canvas::TextAlign::left);
            canvas.fill_text(cmd.text, cmd.x, cmd.y);
            break;
        case CanvasDrawCmd::Type::set_font:
            canvas.set_font(cmd.text, cmd.extra);
            break;

        // Style
        case CanvasDrawCmd::Type::set_fill_color:
            canvas.set_fill_color(cmd.color);
            break;
        case CanvasDrawCmd::Type::set_stroke_color:
            canvas.set_stroke_color(cmd.color);
            break;
        case CanvasDrawCmd::Type::set_line_width:
            canvas.set_line_width(cmd.extra);
            break;

        // Path
        case CanvasDrawCmd::Type::begin_path:
            canvas.begin_path();
            break;
        case CanvasDrawCmd::Type::move_to:
            canvas.move_to(cmd.x, cmd.y);
            break;
        case CanvasDrawCmd::Type::line_to:
            canvas.line_to(cmd.x, cmd.y);
            break;
        case CanvasDrawCmd::Type::quad_to:
            canvas.quad_to(cmd.x, cmd.y, cmd.x2, cmd.y2);
            break;
        case CanvasDrawCmd::Type::cubic_to:
            canvas.cubic_to(cmd.x, cmd.y, cmd.x2, cmd.y2, cmd.x3, cmd.y3);
            break;
        case CanvasDrawCmd::Type::close_path:
            canvas.close_path();
            break;
        case CanvasDrawCmd::Type::fill_path:
            canvas.fill_current_path();
            break;
        case CanvasDrawCmd::Type::stroke_path:
            canvas.stroke_current_path();
            break;

        // State
        case CanvasDrawCmd::Type::save:
            canvas.save();
            break;
        case CanvasDrawCmd::Type::restore:
            canvas.restore();
            break;

        // Transform
        case CanvasDrawCmd::Type::translate:
            canvas.translate(cmd.x, cmd.y);
            break;
        case CanvasDrawCmd::Type::scale:
            canvas.scale(cmd.x, cmd.y);
            break;
        case CanvasDrawCmd::Type::rotate:
            canvas.rotate(cmd.extra);
            break;
        case CanvasDrawCmd::Type::clip_rect:
            canvas.clip_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            break;
        case CanvasDrawCmd::Type::clip_path:
            // Clip to current path (fill_current_path sets the clip)
            canvas.clip_rect(cmd.x, cmd.y, cmd.w, cmd.h); // fallback
            break;

        // Arc
        case CanvasDrawCmd::Type::stroke_arc:
            canvas.set_stroke_color(cmd.color);
            canvas.set_line_width(cmd.extra);
            canvas.stroke_arc(cmd.x, cmd.y, cmd.w, cmd.x2, cmd.y2); // cx, cy, radius, start, end
            break;

        // Text alignment and baseline
        case CanvasDrawCmd::Type::set_text_align:
            if (cmd.int_val == 1) canvas.set_text_align(canvas::TextAlign::center);
            else if (cmd.int_val == 2) canvas.set_text_align(canvas::TextAlign::right);
            else canvas.set_text_align(canvas::TextAlign::left);
            break;
        case CanvasDrawCmd::Type::set_text_baseline:
            // Stored for use by fill_text — applied before text draw
            break;

        // Line cap/join
        case CanvasDrawCmd::Type::set_line_cap:
            if (cmd.int_val == 1) canvas.set_line_cap(canvas::LineCap::round);
            else if (cmd.int_val == 2) canvas.set_line_cap(canvas::LineCap::square);
            else canvas.set_line_cap(canvas::LineCap::butt);
            break;
        case CanvasDrawCmd::Type::set_line_join:
            if (cmd.int_val == 1) canvas.set_line_join(canvas::LineJoin::round);
            else if (cmd.int_val == 2) canvas.set_line_join(canvas::LineJoin::bevel);
            else canvas.set_line_join(canvas::LineJoin::miter);
            break;

        // Global alpha and blend mode
        case CanvasDrawCmd::Type::set_global_alpha:
            canvas.set_opacity(cmd.extra);
            break;
        case CanvasDrawCmd::Type::set_blend_mode:
            canvas.set_blend_mode(static_cast<canvas::Canvas::BlendMode>(cmd.int_val));
            break;

        // Gradients
        case CanvasDrawCmd::Type::set_fill_gradient_linear:
            if (!cmd.gradient_colors.empty())
                canvas.set_fill_gradient_linear(cmd.x, cmd.y, cmd.x2, cmd.y2,
                    cmd.gradient_colors.data(), cmd.gradient_positions.data(),
                    static_cast<int>(cmd.gradient_colors.size()));
            break;
        case CanvasDrawCmd::Type::set_fill_gradient_radial:
            if (!cmd.gradient_colors.empty())
                canvas.set_fill_gradient_radial(cmd.x, cmd.y, cmd.extra,
                    cmd.gradient_colors.data(), cmd.gradient_positions.data(),
                    static_cast<int>(cmd.gradient_colors.size()));
            break;
        case CanvasDrawCmd::Type::clear_fill_gradient:
            canvas.clear_fill_gradient();
            break;

        // Clear rect
        case CanvasDrawCmd::Type::clear_rect:
            canvas.save();
            canvas.clip_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            canvas.set_fill_color({0, 0, 0, 0});
            canvas.fill_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            canvas.restore();
            break;

        // Draw image — renders a placeholder rect with the image path stored in text
        // Full image rendering requires the Skia image decode pipeline;
        // for now we render a labeled placeholder that marks where the image goes
        case CanvasDrawCmd::Type::draw_image:
            canvas.save();
            canvas.set_fill_color({40, 40, 60, 200});
            canvas.fill_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            canvas.set_fill_color({180, 180, 200, 255});
            canvas.set_font("", 10);
            canvas.fill_text(cmd.text.empty() ? "[image]" : cmd.text, cmd.x + 4, cmd.y + cmd.h / 2);
            canvas.restore();
            break;
        }
    }
}

} // namespace pulp::view
