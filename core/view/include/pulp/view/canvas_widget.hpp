#pragma once

/// @file canvas_widget.hpp
/// A View that replays recorded draw commands in paint().
/// Full Canvas 2D API equivalent — JS records commands, C++ replays via Skia.

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <cmath>
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
        // pulp #1434 — Canvas2D `ctx.font` full CSS font shorthand. The
        // legacy `set_font` only carries family + size; the JS shim now
        // parses `[<style>] [<variant>] [<weight>] <size>[/<lineHeight>]
        // <family>`. `set_font_full` carries the parsed weight / slant
        // through to `Canvas::set_font_full`, which Skia honours via
        // `make_font(family, size, weight, slant)`. CG falls back to
        // family+size (no slant override) — same as the base
        // `set_font_full` default. Layout in CanvasDrawCmd:
        //   text  = family
        //   extra = size (px)
        //   x     = weight (100..900, cast to int)
        //   y     = slant (0=upright, 1=italic/oblique)
        //   x2    = letter_spacing (0 from shorthand; reserved)
        set_font_full,
        // Style
        set_fill_color, set_stroke_color, set_line_width,
        set_line_cap, set_line_join,
        set_global_alpha, set_blend_mode,
        // Gradient
        set_fill_gradient_linear, set_fill_gradient_radial, clear_fill_gradient,
        // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createConicGradient.
        // cx/cy in (x, y), start_angle in `extra`, stops in
        // gradient_colors / gradient_positions (same shape as the
        // linear / radial entries above).
        set_fill_gradient_conic,
        // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createPattern.
        // Image source path / data URI in `text`, tile modes packed into
        // `int_val` (bit 0 = x, bit 1 = y; 0 = repeat, 1 = no-repeat).
        // Skia routes through SkShader::MakeImage; CG degrades to the
        // active fill colour (no CGPattern dance — file a follow-up if
        // a CG-targeted plugin actually needs tiled patterns).
        set_fill_pattern, set_stroke_pattern,
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
        // issue-1434 batch 7: Canvas2D shadow* sticky state setters
        set_shadow_color,          ///< color in `color`
        set_shadow_blur,           ///< blur (px) in `extra`
        set_shadow_offset_x,       ///< dx (px) in `extra`
        set_shadow_offset_y,       ///< dy (px) in `extra`
        // pulp #1434 bridge-thin gap-fill: ctx.miterLimit and
        // ctx.imageSmoothingEnabled / Quality. Sticky state pushed by
        // the JS shim before the next stroke / drawImage.
        set_miter_limit,           ///< limit in `extra`
        set_image_smoothing,       ///< enabled in `int_val` (0/1), quality in `extra` (0=low,1=med,2=high)
        // pulp #1520 — Canvas2D ctx.direction / ctx.filter sticky state.
        // direction enum (0=ltr, 1=rtl, 2=inherit) in `int_val`; filter
        // raw CSS <filter-function-list> string in `text`. Pushed by
        // the JS shim before fillText / strokeText / fill / stroke /
        // drawImage so the backend can wrap the next paint.
        set_direction,
        set_filter,
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
    /// pulp #1387 gap #2 — NaN / ±Infinity defense at the recording
    /// boundary. JS callers can produce non-finite numerics from any
    /// arithmetic mishap (divide-by-zero on a zero parent rect during a
    /// transient layout, NaN bubbling through pointer-event coords, etc).
    /// If a non-finite reaches Skia or CoreGraphics, it can taint the
    /// entire CGContext / Skia surface for the rest of the frame —
    /// Spectr saw this as bands rendering as solid grey blobs after an
    /// off-screen drag produced one NaN coord. Sanitize each cmd's
    /// numeric fields to 0 on non-finite. The fields cover every coord
    /// path the dispatch table consumes (move_to, line_to, quad/cubic,
    /// rects, circles, arcs, text, transforms, clip, image draw).
    /// Color / int_val / text fields are unaffected. Sanitizing at the
    /// recording boundary means every backend (Skia GPU, CG CPU,
    /// RecordingCanvas, headless capture) gets clean numerics without
    /// a per-backend retrofit.
    void add_command(CanvasDrawCmd cmd) {
        cmd.x       = sanitize_finite(cmd.x);
        cmd.y       = sanitize_finite(cmd.y);
        cmd.w       = sanitize_finite(cmd.w);
        cmd.h       = sanitize_finite(cmd.h);
        cmd.x2      = sanitize_finite(cmd.x2);
        cmd.y2      = sanitize_finite(cmd.y2);
        cmd.x3      = sanitize_finite(cmd.x3);
        cmd.y3      = sanitize_finite(cmd.y3);
        cmd.extra   = sanitize_finite(cmd.extra);
        for (auto& p : cmd.gradient_positions) p = sanitize_finite(p);
        commands_.push_back(std::move(cmd));
    }
    size_t command_count() const { return commands_.size(); }
    /// pulp #964 — accessor for tests asserting on the recorded JS command
    /// stream. Read-only; the bridge owns mutation via add_command /
    /// clear_commands. Callers must not retain the reference past the next
    /// add_command / clear_commands call.
    const std::vector<CanvasDrawCmd>& commands() const { return commands_; }
    bool last_native_gpu_texture_draw_succeeded() const { return last_native_gpu_texture_draw_succeeded_; }
    void set_native_gpu_texture_provider(NativeGpuTextureProvider provider) {
        native_gpu_texture_provider_ = std::move(provider);
    }

    void paint(canvas::Canvas& canvas) override;

private:
    /// pulp #1387 gap #2 — return 0 on NaN / ±Infinity, value otherwise.
    /// Inlined helper kept in the header so the compiler folds it into
    /// the move-constructor copy in add_command. <cmath>'s std::isfinite
    /// is constexpr-safe and consteval-eligible on C++20 toolchains.
    static float sanitize_finite(float v) noexcept {
        return std::isfinite(v) ? v : 0.0f;
    }

    std::vector<CanvasDrawCmd> commands_;
    NativeGpuTextureProvider native_gpu_texture_provider_;
    bool last_native_gpu_texture_draw_succeeded_ = false;
};

} // namespace pulp::view
