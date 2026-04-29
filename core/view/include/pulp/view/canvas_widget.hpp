#pragma once

/// @file canvas_widget.hpp
/// A View that replays recorded draw commands in paint().
/// Full Canvas 2D API equivalent — JS records commands, C++ replays via Skia.

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <functional>
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
        stroke_line, stroke_arc,
        // Text
        fill_text, set_font, set_text_align, set_text_baseline,
        // Style
        set_fill_color, set_stroke_color, set_line_width,
        set_line_cap, set_line_join,
        set_global_alpha, set_blend_mode,
        // Gradient
        set_fill_gradient_linear, set_fill_gradient_radial, clear_fill_gradient,
        // Path
        begin_path, move_to, line_to, quad_to, cubic_to, close_path,
        fill_path, stroke_path, clip_path,
        // State
        save, restore,
        // Transform
        translate, scale, rotate, clip_rect,
        set_transform,             // issue-896: replace transform with affine matrix
        clip,                      // issue-896: intersect clip with current path
        // Image
        draw_image,
        // issue-916: Canvas2D API gap closures
        set_line_dash,             ///< pattern in `gradient_positions`, phase in `extra`
        put_image_data,            ///< RGBA pixels in `text` (binary), int_val=width, x2=height (as int)
        // Clear
        clear, clear_rect
    };
    Type type = Type::clear;
    float x = 0, y = 0, w = 0, h = 0;
    float x2 = 0, y2 = 0;      // extra coords (line end, control point 1)
    float x3 = 0, y3 = 0;      // cubic control point 2
    canvas::Color color{255, 255, 255, 255};
    float extra = 0;            // radius, line width, font size, angle
    std::string text;           // for fill_text, set_font family
    int int_val = 0;            // for enum values (text align, baseline, blend mode, cap, join)
    std::vector<canvas::Color> gradient_colors;    // for gradient stops
    std::vector<float> gradient_positions;          // gradient stop positions
    /// pulp #968 — when true on a fill_rect / stroke_rect cmd, the paint
    /// loop must NOT call set_fill_color / set_stroke_color from `color`
    /// before drawing. The most recent set_fill_color, set_stroke_color,
    /// or set_fill_gradient_* on the underlying canvas stays active.
    /// Bridge sets this when the JS caller omitted the color arg
    /// (e.g. `canvasRect(id, x, y, w, h)` — Canvas2D `ctx.fillRect()`
    /// shim that relies on a previously-set `ctx.fillStyle`, including
    /// gradients).
    bool use_active_style = false;
};

/// A View whose paint() replays a list of recorded draw commands.
/// JS fills the command list via bridge functions, then the widget
/// renders them each frame. Hot-reloadable — JS rebuilds commands on reload.
class CanvasWidget : public View {
public:
    struct NativeGpuTextureFrame {
        void* texture_handle = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string format = "bgra8unorm";
        bool available = false;
    };
    using NativeGpuTextureProvider = std::function<NativeGpuTextureFrame()>;

    CanvasWidget() = default;

    void clear_commands() { commands_.clear(); }
    void add_command(CanvasDrawCmd cmd) { commands_.push_back(std::move(cmd)); }
    size_t command_count() const { return commands_.size(); }
    bool last_native_gpu_texture_draw_succeeded() const { return last_native_gpu_texture_draw_succeeded_; }
    void set_native_gpu_texture_provider(NativeGpuTextureProvider provider) {
        native_gpu_texture_provider_ = std::move(provider);
    }

    void paint(canvas::Canvas& canvas) override;

private:
    std::vector<CanvasDrawCmd> commands_;
    NativeGpuTextureProvider native_gpu_texture_provider_;
    bool last_native_gpu_texture_draw_succeeded_ = false;
};

} // namespace pulp::view
