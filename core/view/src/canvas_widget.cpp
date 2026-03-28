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
        }
    }
}

} // namespace pulp::view
