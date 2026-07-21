#pragma once

/// @file canvas_widget.hpp
/// A View that replays recorded draw commands in paint().
/// Full Canvas 2D API equivalent — JS records commands, C++ replays through
/// the active Canvas backend.

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>
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
        // Canvas2D `strokeText(text, x, y, maxWidth)` recorded
        // as a distinct cmd so the paint loop can route it through the
        // dedicated `Canvas::stroke_text` (true outlined glyphs) instead
        // of the older fillText-with-stroke-color approximation.
        // Layout: text in `text`, (x,y) in `x`/`y`, maxWidth in `w`
        // (0 = no limit), font size in `extra`, color in `color`.
        stroke_text,
        // Canvas2D `ctx.font` full CSS font shorthand. The
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
        //   x2    = letter_spacing in px (0 when derived from `font`)
        set_font_full,
        // Style
        set_fill_color, set_stroke_color, set_line_width,
        set_line_cap, set_line_join,
        set_global_alpha, set_blend_mode,
        // Gradient
        set_fill_gradient_linear, set_fill_gradient_radial, clear_fill_gradient,
        /// Canvas2D `ctx.createRadialGradient(x0,y0,r0,x1,y1,r1)`
        /// two-circle form. Inner circle (x0,y0,r0) packed as (x, y, extra),
        /// outer circle (x1,y1,r1) packed as (x2, y2, w). Routes to
        /// `set_fill_gradient_radial_two_circles` (Skia: MakeTwoPointConical;
        /// CG: full CGContextDrawRadialGradient with both circles).
        set_fill_gradient_radial_two_circles,
        // Canvas2D ctx.createConicGradient.
        // cx/cy in (x, y), start_angle in `extra`, stops in
        // gradient_colors / gradient_positions (same shape as the
        // linear / radial entries above).
        set_fill_gradient_conic,
        // Canvas2D `ctx.strokeStyle =
        // createLinearGradient(...) | createRadialGradient(...) |
        // createConicGradient(...)`. Field layout matches the fill
        // counterparts:
        //   set_stroke_gradient_linear:        x/y/x2/y2 = (x0,y0,x1,y1)
        //   set_stroke_gradient_radial:        x/y/extra = (cx,cy,radius)
        //   set_stroke_gradient_radial_two_circles:
        //                                      x/y/extra = (x0,y0,r0)
        //                                      x2/y2/w   = (x1,y1,r1)
        //   set_stroke_gradient_conic:         x/y/extra = (cx,cy,startAngle)
        // Stops live in gradient_colors / gradient_positions.
        set_stroke_gradient_linear,
        set_stroke_gradient_radial,
        set_stroke_gradient_radial_two_circles,
        set_stroke_gradient_conic,
        clear_stroke_gradient,
        // Canvas2D ctx.createPattern.
        // Image source path / data URI in `text`, tile modes packed into
        // `int_val` (bit 0 = x, bit 1 = y; 0 = repeat, 1 = no-repeat).
        // Fill patterns route through the backend pattern implementation;
        // stroke patterns fall back to solid stroke color on backends that
        // do not support stroke-pattern shaders.
        set_fill_pattern, set_stroke_pattern,
        // Path
        begin_path, move_to, line_to, quad_to, cubic_to, close_path,
        fill_path, stroke_path, clip_path,
        // Native arc subpaths.
        // Layout in CanvasDrawCmd:
        //   path_arc:        x=cx, y=cy, extra=radius,
        //                    x2=startAngle, y2=endAngle,
        //                    int_val=anticlockwise (0/1)
        //   path_arc_to:     x=x1, y=y1, x2=x2, y2=y2, extra=radius
        //   path_ellipse:    x=cx, y=cy, w=rx, h=ry, extra=rotation,
        //                    x2=startAngle, y2=endAngle,
        //                    int_val=anticlockwise (0/1)
        //   path_round_rect: x=x, y=y, w=w, h=h,
        //                    gradient_positions packed [tl_x,tl_y,
        //                    tr_x,tr_y, br_x,br_y, bl_x,bl_y]
        path_arc,
        path_arc_to,
        path_ellipse,
        path_round_rect,
        // State
        save, restore,
        // Transform
        translate, scale, rotate, clip_rect,
        set_transform,             // replace transform with affine matrix
        clip,                      // intersect clip with current path
        // Image
        draw_image,
        // Canvas2D API coverage
        set_line_dash,             ///< pattern in `gradient_positions`, phase in `extra`
        put_image_data,            ///< RGBA pixels in `text` (binary), int_val=width, x2=height (as int)
        // Canvas2D shadow* sticky state setters
        set_shadow_color,          ///< color in `color`
        set_shadow_blur,           ///< blur (px) in `extra`
        set_shadow_offset_x,       ///< dx (px) in `extra`
        set_shadow_offset_y,       ///< dy (px) in `extra`
        // ctx.miterLimit and
        // ctx.imageSmoothingEnabled / Quality. Sticky state pushed by
        // the JS shim before the next stroke / drawImage.
        set_miter_limit,           ///< limit in `extra`
        set_image_smoothing,       ///< enabled in `int_val` (0/1), quality in `extra` (0=low,1=med,2=high)
        // Canvas2D ctx.direction / ctx.filter sticky state.
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
    /// When true on a fill_rect / stroke_rect cmd, the paint
    /// loop must NOT call set_fill_color / set_stroke_color from `color`
    /// before drawing. The most recent set_fill_color, set_stroke_color,
    /// or set_fill_gradient_* on the underlying canvas stays active.
    /// Bridge sets this when the JS caller omitted the color arg
    /// (e.g. `canvasRect(id, x, y, w, h)` — Canvas2D `ctx.fillRect()`
    /// shim that relies on a previously-set `ctx.fillStyle`, including
    /// gradients).
    bool use_active_style = false;

    /// When true on a `draw_image` cmd, x2/y2/x3/y3 carry
    /// the source rect (sx, sy, sw, sh) for the 9-arg
    /// `ctx.drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh)` form. The renderer
    /// routes through `Canvas::draw_image_from_*_rect` so the source
    /// sub-rectangle of the decoded image maps onto the destination rect
    /// (sprite-sheet slicing). Without the flag the dst-only path runs.
    bool has_source_rect = false;
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
    /// NaN / ±Infinity defense at the recording
    /// boundary. JS callers can produce non-finite numerics from any
    /// arithmetic mishap (divide-by-zero on a zero parent rect during a
    /// transient layout, NaN bubbling through pointer-event coords, etc).
    /// If a non-finite reaches Skia or CoreGraphics, it can taint the
    /// entire CGContext / Skia surface for the rest of the frame —
    /// Sanitize each cmd's numeric fields to 0 on non-finite. The fields
    /// cover every coord path the dispatch table consumes (move_to, line_to, quad/cubic,
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
    /// Accessor for tests asserting on the recorded JS command
    /// stream. Read-only; the bridge owns mutation via add_command /
    /// clear_commands. Callers must not retain the reference past the next
    /// add_command / clear_commands call.
    const std::vector<CanvasDrawCmd>& commands() const { return commands_; }
    bool last_native_gpu_texture_draw_succeeded() const { return last_native_gpu_texture_draw_succeeded_; }
    void set_native_gpu_texture_provider(NativeGpuTextureProvider provider) {
        native_gpu_texture_provider_ = std::move(provider);
    }

    /// Request a curated, named GPU post-effect over the whole canvas.
    ///
    /// This is the SAFE shader-effect surface for generated / scripted UIs
    /// (Forge): `name` selects one of a small vetted set of SkSL effects
    /// (`crt`, `grain`, `vignette`, `noise`, `brushed`, `bloom`) — never
    /// arbitrary shader source. `intensity` is a single strength knob,
    /// clamped to [0,1]. `"none"` / `""` clears the effect. The effect is
    /// sticky (declarative): it persists across command-list rebuilds until
    /// changed, so a static CRT/grain overlay needs no per-frame re-issue.
    /// Applied at paint() time to the per-canvas compositing layer; unknown
    /// names and non-Skia/CPU backends degrade to no effect (see
    /// Canvas::save_layer_with_shader_effect).
    void set_shader_effect(std::string name, float intensity = 1.0f) {
        shader_effect_name_ = std::move(name);
        shader_effect_intensity_ = std::clamp(intensity, 0.0f, 1.0f);
    }
    /// Active curated effect name (`"none"` / `""` when disabled). Read-only
    /// accessor for tests; the bridge owns mutation via set_shader_effect.
    const std::string& shader_effect() const { return shader_effect_name_; }
    float shader_effect_intensity() const { return shader_effect_intensity_; }

    void paint(canvas::Canvas& canvas) override;

private:
    /// Return 0 on NaN / ±Infinity, value otherwise.
    /// Inlined helper kept in the header so the compiler folds it into
    /// the move-constructor copy in add_command. <cmath>'s std::isfinite
    /// is constexpr-safe and consteval-eligible on C++20 toolchains.
    static float sanitize_finite(float v) noexcept {
        return std::isfinite(v) ? v : 0.0f;
    }

    std::vector<CanvasDrawCmd> commands_;
    NativeGpuTextureProvider native_gpu_texture_provider_;
    bool last_native_gpu_texture_draw_succeeded_ = false;
    // Curated named GPU post-effect state (see set_shader_effect). Sticky
    // across command-list rebuilds; "none"/"" means no effect.
    std::string shader_effect_name_ = "none";
    float shader_effect_intensity_ = 1.0f;
};

} // namespace pulp::view
