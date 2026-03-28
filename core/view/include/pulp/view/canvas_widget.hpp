#pragma once

/// @file canvas_widget.hpp
/// A View that replays recorded draw commands in paint().
/// Full Canvas 2D API equivalent — JS records commands, C++ replays via Skia.

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <string>
#include <vector>

namespace pulp::view {

/// A draw command recorded from JS for replay in paint().
/// Maps to CanvasRenderingContext2D methods.
struct CanvasDrawCmd {
    enum class Type {
        // Shapes
        fill_rect, stroke_rect, fill_rounded_rect, stroke_rounded_rect,
        fill_circle, stroke_circle,
        stroke_line,
        // Text
        fill_text, set_font,
        // Style
        set_fill_color, set_stroke_color, set_line_width,
        // Path
        begin_path, move_to, line_to, quad_to, cubic_to, close_path,
        fill_path, stroke_path,
        // State
        save, restore,
        // Transform
        translate, scale, rotate, clip_rect,
        // Clear
        clear
    };
    Type type = Type::clear;
    float x = 0, y = 0, w = 0, h = 0;
    float x2 = 0, y2 = 0;      // extra coords (line end, control point 1)
    float x3 = 0, y3 = 0;      // cubic control point 2
    canvas::Color color{255, 255, 255, 255};
    float extra = 0;            // radius, line width, font size, angle
    std::string text;           // for fill_text, set_font family
};

/// A View whose paint() replays a list of recorded draw commands.
/// JS fills the command list via bridge functions, then the widget
/// renders them each frame. Hot-reloadable — JS rebuilds commands on reload.
class CanvasWidget : public View {
public:
    CanvasWidget() = default;

    void clear_commands() { commands_.clear(); }
    void add_command(CanvasDrawCmd cmd) { commands_.push_back(std::move(cmd)); }
    size_t command_count() const { return commands_.size(); }

    void paint(canvas::Canvas& canvas) override;

private:
    std::vector<CanvasDrawCmd> commands_;
};

} // namespace pulp::view
