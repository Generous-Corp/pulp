#include <pulp/view/canvas_widget.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#endif

namespace pulp::view {

namespace {

// pulp #1368 round 2 — env-gated paint trace. When `PULP_LOG_CANVAS_PAINT=1`
// is exported the live process logs one grep-able line per CanvasWidget paint
// to stderr with the widget id, its bounds, and the inbound canvas CTM. The
// Spectr filterbank repro (#1368) shows pr_1's first-instruction fillRect
// landing in the title-bar region above the visible window — this trace is
// the diagnostic that lets us confirm whether bounds_.y / CTM are off-window
// vs. the widget never being painted at all. Returns false when the variable
// is unset / "0" / empty so production builds incur a single getenv lookup
// per paint and nothing else.
inline bool canvas_paint_logging_enabled() {
    const char* v = std::getenv("PULP_LOG_CANVAS_PAINT");
    if (!v || !*v) return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
}

// pulp #1368 follow-up — translate CanvasDrawCmd::Type to a stable, grep-able
// short string so the env-gated trace can summarise commands_ without dragging
// a heavyweight reflection helper into the hot path. Names mirror the enum
// labels exactly so a Spectr-side `grep fill_rect` lines up with the source.
inline const char* canvas_cmd_type_name(CanvasDrawCmd::Type t) {
    switch (t) {
        case CanvasDrawCmd::Type::fill_rect: return "fill_rect";
        case CanvasDrawCmd::Type::stroke_rect: return "stroke_rect";
        case CanvasDrawCmd::Type::fill_rounded_rect: return "fill_rounded_rect";
        case CanvasDrawCmd::Type::stroke_rounded_rect: return "stroke_rounded_rect";
        case CanvasDrawCmd::Type::fill_circle: return "fill_circle";
        case CanvasDrawCmd::Type::stroke_circle: return "stroke_circle";
        case CanvasDrawCmd::Type::stroke_line: return "stroke_line";
        case CanvasDrawCmd::Type::stroke_arc: return "stroke_arc";
        case CanvasDrawCmd::Type::fill_text: return "fill_text";
        case CanvasDrawCmd::Type::set_font: return "set_font";
        case CanvasDrawCmd::Type::set_font_full: return "set_font_full";
        case CanvasDrawCmd::Type::set_text_align: return "set_text_align";
        case CanvasDrawCmd::Type::set_text_baseline: return "set_text_baseline";
        case CanvasDrawCmd::Type::set_fill_color: return "set_fill_color";
        case CanvasDrawCmd::Type::set_stroke_color: return "set_stroke_color";
        case CanvasDrawCmd::Type::set_line_width: return "set_line_width";
        case CanvasDrawCmd::Type::set_line_cap: return "set_line_cap";
        case CanvasDrawCmd::Type::set_line_join: return "set_line_join";
        case CanvasDrawCmd::Type::set_global_alpha: return "set_global_alpha";
        case CanvasDrawCmd::Type::set_blend_mode: return "set_blend_mode";
        case CanvasDrawCmd::Type::set_fill_gradient_linear: return "set_fill_gradient_linear";
        case CanvasDrawCmd::Type::set_fill_gradient_radial: return "set_fill_gradient_radial";
        case CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles: return "set_fill_gradient_radial_two_circles";
        case CanvasDrawCmd::Type::set_fill_gradient_conic: return "set_fill_gradient_conic";
        case CanvasDrawCmd::Type::set_fill_pattern: return "set_fill_pattern";
        case CanvasDrawCmd::Type::set_stroke_pattern: return "set_stroke_pattern";
        case CanvasDrawCmd::Type::clear_fill_gradient: return "clear_fill_gradient";
        case CanvasDrawCmd::Type::begin_path: return "begin_path";
        case CanvasDrawCmd::Type::move_to: return "move_to";
        case CanvasDrawCmd::Type::line_to: return "line_to";
        case CanvasDrawCmd::Type::quad_to: return "quad_to";
        case CanvasDrawCmd::Type::cubic_to: return "cubic_to";
        case CanvasDrawCmd::Type::close_path: return "close_path";
        case CanvasDrawCmd::Type::fill_path: return "fill_path";
        case CanvasDrawCmd::Type::stroke_path: return "stroke_path";
        case CanvasDrawCmd::Type::clip_path: return "clip_path";
        case CanvasDrawCmd::Type::save: return "save";
        case CanvasDrawCmd::Type::restore: return "restore";
        case CanvasDrawCmd::Type::translate: return "translate";
        case CanvasDrawCmd::Type::scale: return "scale";
        case CanvasDrawCmd::Type::rotate: return "rotate";
        case CanvasDrawCmd::Type::clip_rect: return "clip_rect";
        case CanvasDrawCmd::Type::set_transform: return "set_transform";
        case CanvasDrawCmd::Type::clip: return "clip";
        case CanvasDrawCmd::Type::draw_image: return "draw_image";
        case CanvasDrawCmd::Type::set_line_dash: return "set_line_dash";
        case CanvasDrawCmd::Type::put_image_data: return "put_image_data";
        case CanvasDrawCmd::Type::set_shadow_color: return "set_shadow_color";
        case CanvasDrawCmd::Type::set_shadow_blur: return "set_shadow_blur";
        case CanvasDrawCmd::Type::set_shadow_offset_x: return "set_shadow_offset_x";
        case CanvasDrawCmd::Type::set_shadow_offset_y: return "set_shadow_offset_y";
        case CanvasDrawCmd::Type::set_miter_limit: return "set_miter_limit";
        case CanvasDrawCmd::Type::set_image_smoothing: return "set_image_smoothing";
        case CanvasDrawCmd::Type::set_direction: return "set_direction";
        case CanvasDrawCmd::Type::set_filter: return "set_filter";
        case CanvasDrawCmd::Type::clear: return "clear";
        case CanvasDrawCmd::Type::clear_rect: return "clear_rect";
    }
    return "unknown";
}

} // namespace

void CanvasWidget::paint(canvas::Canvas& canvas) {
    // pulp #1368 round 2 — env-gated paint trace. Logged at entry, BEFORE any
    // baseline / save_count snapshots so the line reflects the matrix the
    // parent View::paint_all chain handed us. Format is grep-able:
    //   [pulp:canvas-paint] id=<id> bounds=(x,y,w,h) ctm=[a,b,c,d,e,f]
    //       cmd_total=<N> cmds={fill_rect=42,stroke_path=15,...}
    //
    // pulp #1368 follow-up — Spectr-agent narrowed the diagnosis to per-canvas
    // surface compositing (pr_1's painted content discarded between sibling
    // paints). The per-type summary distinguishes "rich command list processed
    // but never composited" (cmd_total>0, varied types) from "silently
    // truncated" (cmd_total tiny or 0). Tally is allocation-light: a single
    // std::map walk gated behind the same env var, so production paints incur
    // a single getenv lookup and nothing else.
    if (canvas_paint_logging_enabled()) {
        const auto& b = bounds();
        const auto m = canvas.current_transform();
        std::map<int, int> type_counts;
        for (const auto& cmd : commands_) {
            ++type_counts[static_cast<int>(cmd.type)];
        }
        std::string summary;
        summary.reserve(type_counts.size() * 24);
        bool first = true;
        for (const auto& [tk, count] : type_counts) {
            if (count <= 0) continue;
            if (!first) summary.push_back(',');
            first = false;
            summary.append(canvas_cmd_type_name(static_cast<CanvasDrawCmd::Type>(tk)));
            summary.push_back('=');
            summary.append(std::to_string(count));
        }
        std::fprintf(stderr,
                     "[pulp:canvas-paint] id=%s bounds=(%g,%g,%g,%g) "
                     "ctm=[%g,%g,%g,%g,%g,%g] cmd_total=%zu cmds={%s}\n",
                     id().c_str(), b.x, b.y, b.width, b.height,
                     static_cast<double>(m.a), static_cast<double>(m.b),
                     static_cast<double>(m.c), static_cast<double>(m.d),
                     static_cast<double>(m.e), static_cast<double>(m.f),
                     commands_.size(), summary.c_str());
        std::fflush(stderr);
    }

    // Snapshot the inbound device matrix so JS-driven setTransform() composes
    // onto the parent View transform rather than overwriting it (issue-897
    // P1 follow-up to issue-896).
    canvas.capture_paint_baseline_transform();
    last_native_gpu_texture_draw_succeeded_ = false;

    // pulp #1368 — defend against unbalanced JS save/restore. If the
    // draw script reaches an early-return path that skips a matching
    // `ctx.restore()`, the leftover save accumulates on the canvas's
    // GState stack every frame. The parent `View::paint_all`'s outer
    // `canvas.save()` / `canvas.restore()` only pops one level, so any
    // surplus GState (transform, clip) leaks into sibling siblings'
    // paint scopes and silently corrupts their drawing — concretely
    // observed in pulp #1368 / Spectr filterbank where the main canvas
    // child stopped painting visibly while an identically-configured
    // overlay sibling kept working. Snapshot depth at entry, then pop
    // back to it after replay so the parent always sees the canvas at
    // the depth it expects.
    //
    // pulp #1368 (root cause, follow-up to the save/restore depth
    // defense above) — the deeper bug behind the same issue: JS-driven
    // `clearRect()` lowers to a kClear blend on the underlying surface
    // (SkBlendMode::kClear / CGContextClearRect), which unconditionally
    // zeros destination texels regardless of the source alpha. With
    // multiple sibling CanvasWidgets sharing the parent View's paint
    // surface, sibling-2's clearRect at the start of its draw replay
    // erases pixels sibling-1 just painted. Concretely Spectr's main
    // canvas painted, then the overlay canvas's clearRect at frame
    // start blew it away again — visible only because the ratio of
    // visible-frames to invisible-frames depended on JSX ordering.
    //
    // HTML <canvas> semantics give every canvas its own backing store,
    // so clearRect on canvas A cannot affect canvas B. To match that,
    // wrap each CanvasWidget's replay in `save_layer()` covering the
    // local bounds. Inside the layer, kClear zeros only the layer's
    // texels; the matching `restore()` blends the layer back into the
    // parent surface using the default SrcOver, which is what the HTML
    // spec demands. Skip the layer when bounds are degenerate so we
    // don't open a zero-sized offscreen.
    const int saved_depth = canvas.save_count();
    const auto& widget_bounds = bounds();
    const bool open_layer = (widget_bounds.width > 0.0f && widget_bounds.height > 0.0f);
    if (open_layer) {
        canvas.save_layer(0.0f, 0.0f, widget_bounds.width, widget_bounds.height,
                          /*opacity=*/1.0f, /*blur_radius=*/0.0f);
    }

    // pulp #929 — Canvas widget paint contract:
    //   * The widget MUST NOT pre-fill its bounds with an opaque background
    //     before processing JS draw commands. The default state is fully
    //     transparent so the parent's paint surface (View::paint_self_and_children
    //     paints background + border before invoking this paint()) shows
    //     through wherever the JS code does not explicitly draw.
    //   * If the JS code wants an opaque background it must issue an explicit
    //     fill_rect / clear command.
    //   * If the host caller wants a regression check, the test below records
    //     against a RecordingCanvas and asserts no full-bounds fill_rect was
    //     emitted (test_canvas_widget.cpp [issue-929]).
    // Do NOT add any unconditional fill / clear here.
#ifdef PULP_HAS_SKIA
    if (native_gpu_texture_provider_) {
        auto frame = native_gpu_texture_provider_();
        if (frame.available) {
            if (auto* skia_canvas = dynamic_cast<canvas::SkiaCanvas*>(&canvas)) {
                last_native_gpu_texture_draw_succeeded_ =
                    skia_canvas->draw_native_dawn_texture(frame.texture_handle,
                                                          frame.width,
                                                          frame.height,
                                                          frame.format,
                                                          0.0f,
                                                          0.0f,
                                                          bounds().width,
                                                          bounds().height);
            }
        }
    }
#endif

    for (auto& cmd : commands_) {
        switch (cmd.type) {
        // Shapes
        case CanvasDrawCmd::Type::clear:
            canvas.set_fill_color(cmd.color);
            canvas.fill_rect(0, 0, bounds().width, bounds().height);
            break;
        case CanvasDrawCmd::Type::fill_rect:
            // pulp #968 — when use_active_style is set the bridge's caller
            // omitted the color arg (Canvas2D `ctx.fillRect(x,y,w,h)` shim),
            // so honour the active fillStyle (color OR gradient set most
            // recently on the canvas) instead of overwriting with cmd.color.
            if (!cmd.use_active_style) {
                canvas.set_fill_color(cmd.color);
            }
            canvas.fill_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            break;
        case CanvasDrawCmd::Type::stroke_rect:
            // pulp #968 — same fallback as fill_rect, applied to strokeStyle.
            if (!cmd.use_active_style) {
                canvas.set_stroke_color(cmd.color);
            }
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
            // pulp #1434 P1 — do NOT call canvas.set_font() / set_text_align
            // here. The JS shim's fillText path runs `_syncTextState()`
            // BEFORE canvasFillText, which records a `set_font` (legacy) or
            // `set_font_full` (rich CSS shorthand) cmd plus a
            // `set_text_align` cmd ahead of this fill_text cmd. Re-setting
            // the font here clobbers the rich state — Skia's
            // SkiaCanvas::set_font() resets weight/slant to normal/upright
            // (canvas.cpp:567-575), so `ctx.font = "italic bold 18px Inter"`
            // would record set_font_full(weight=700, slant=1) and then
            // immediately have weight/slant reset to 400/0 here, rendering
            // the text as plain upright Regular even though the rich state
            // was correctly captured. Same logic for text alignment: the
            // prior set_text_align cmd already configured the canvas; the
            // hard-coded TextAlign::left below would force every JS draw
            // back to left-align. Drop both calls — the prior state cmds
            // own the font + alignment.
            canvas.fill_text(cmd.text, cmd.x, cmd.y);
            break;
        case CanvasDrawCmd::Type::set_font:
            canvas.set_font(cmd.text, cmd.extra);
            break;
        case CanvasDrawCmd::Type::set_font_full:
            // pulp #1434 — full CSS font shorthand. Weight stored in
            // `cmd.x`, slant in `cmd.y`, letter_spacing in `cmd.x2`.
            // Skia's set_font_full honours weight / slant; CG falls
            // through to family+size via the base default.
            canvas.set_font_full(
                cmd.text,
                cmd.extra,
                static_cast<int>(cmd.x),
                static_cast<int>(cmd.y),
                cmd.x2);
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
            // pulp #1522 — int_val carries Canvas2D fillRule arg
            // (0 = nonzero/winding, 1 = evenodd). Threaded from
            // ctx.fill(rule) through canvasFillPath in widget_bridge.cpp.
            canvas.fill_current_path(cmd.int_val == 1
                                         ? canvas::FillRule::evenodd
                                         : canvas::FillRule::nonzero);
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
        case CanvasDrawCmd::Type::set_transform:
            // Affine matrix laid out as cmd.x = a, cmd.y = b, cmd.w = c,
            // cmd.h = d, cmd.x2 = e, cmd.y2 = f (issue-896).
            canvas.set_transform(cmd.x, cmd.y, cmd.w, cmd.h, cmd.x2, cmd.y2);
            break;
        case CanvasDrawCmd::Type::clip:
            // Intersect clip with current path (issue-896).
            // pulp #1522 — int_val carries Canvas2D fillRule arg
            // (0 = nonzero/winding, 1 = evenodd) from ctx.clip(rule).
            canvas.clip(cmd.int_val == 1
                            ? canvas::FillRule::evenodd
                            : canvas::FillRule::nonzero);
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
        // pulp #1524 — Canvas2D ctx.createRadialGradient(x0,y0,r0,x1,y1,r1)
        // two-circle form. Inner circle in (x, y, extra), outer in (x2, y2, w).
        case CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles:
            if (!cmd.gradient_colors.empty())
                canvas.set_fill_gradient_radial_two_circles(
                    cmd.x, cmd.y, cmd.extra,
                    cmd.x2, cmd.y2, cmd.w,
                    cmd.gradient_colors.data(), cmd.gradient_positions.data(),
                    static_cast<int>(cmd.gradient_colors.size()));
            break;
        // pulp #1434 bridge-thin gap-fill — ctx.createConicGradient. Skia
        // routes through SkGradientShader::MakeSweep; CG degrades to the
        // first-stop colour (no native conic shader). Stops in the same
        // gradient_colors / gradient_positions vectors as linear/radial.
        case CanvasDrawCmd::Type::set_fill_gradient_conic:
            if (!cmd.gradient_colors.empty())
                canvas.set_fill_gradient_conic(cmd.x, cmd.y, cmd.extra,
                    cmd.gradient_colors.data(), cmd.gradient_positions.data(),
                    static_cast<int>(cmd.gradient_colors.size()));
            break;
        case CanvasDrawCmd::Type::clear_fill_gradient:
            canvas.clear_fill_gradient();
            break;
        // pulp #1434 bridge-thin gap-fill — ctx.createPattern. Skia routes
        // through SkShader::MakeImage with SkTileMode per axis (real tiled
        // fill); CG degrades to the active fill colour (no native pattern
        // shader). tile_x = bit 0, tile_y = bit 1: 0 = repeat, 1 = no-repeat.
        case CanvasDrawCmd::Type::set_fill_pattern: {
            using Tile = canvas::Canvas::PatternTileMode;
            Tile tx = (cmd.int_val & 0x1) ? Tile::no_repeat : Tile::repeat;
            Tile ty = (cmd.int_val & 0x2) ? Tile::no_repeat : Tile::repeat;
            canvas.set_fill_pattern(cmd.text, tx, ty);
            break;
        }
        case CanvasDrawCmd::Type::set_stroke_pattern: {
            using Tile = canvas::Canvas::PatternTileMode;
            Tile tx = (cmd.int_val & 0x1) ? Tile::no_repeat : Tile::repeat;
            Tile ty = (cmd.int_val & 0x2) ? Tile::no_repeat : Tile::repeat;
            canvas.set_stroke_pattern(cmd.text, tx, ty);
            break;
        }

        // Clear rect (pulp #929) — replace pixels with transparent black, do
        // not SrcOver-blend a transparent fill (which is a no-op). The
        // canvas::Canvas API exposes clear_rect with a kClear-equivalent
        // implementation per backend; this is what CanvasRenderingContext2D
        // .clearRect must do per the HTML spec.
        case CanvasDrawCmd::Type::clear_rect:
            canvas.clear_rect(cmd.x, cmd.y, cmd.w, cmd.h);
            break;

        // Draw image — issue-916.
        // The bridge stores the image source in cmd.text (file path or
        // "data:" URL) and the destination rect in cmd.{x,y,w,h}. We
        // first try the real image decode path on the active canvas
        // backend; if that fails (e.g. on RecordingCanvas, missing
        // file, or encoded format Skia can't decode) we fall back to
        // the labeled placeholder so the canvas still shows *something*
        // and downstream layout/click code keeps working.
        case CanvasDrawCmd::Type::draw_image: {
            bool drawn = false;
            const std::string& src = cmd.text;
            // "data:image/png;base64,XXXX" — decode the base64 portion
            // and feed the encoded bytes into draw_image_from_data().
            constexpr std::string_view kDataUriPrefix = "data:";
            if (src.rfind(kDataUriPrefix, 0) == 0) {
                auto comma = src.find(',');
                if (comma != std::string::npos) {
                    std::string b64 = src.substr(comma + 1);
                    // Minimal base64 decode — Pulp ships pulp::runtime::base64_decode,
                    // but this path runs on the UI/audio thread and we don't
                    // want to add another transitive include. The bridge
                    // already validates and decodes data URIs before
                    // recording the command (see widget_bridge.cpp), so
                    // skip the decode here when present.
                    (void)b64;
                }
            } else if (!src.empty()) {
                drawn = canvas.draw_image_from_file(src, cmd.x, cmd.y, cmd.w, cmd.h);
            }
            if (!drawn) {
                canvas.save();
                canvas.set_fill_color({40, 40, 60, 200});
                canvas.fill_rect(cmd.x, cmd.y, cmd.w, cmd.h);
                canvas.set_fill_color({180, 180, 200, 255});
                canvas.set_font("", 10);
                canvas.fill_text(src.empty() ? "[image]" : src,
                                 cmd.x + 4, cmd.y + cmd.h / 2);
                canvas.restore();
            }
            break;
        }

        // setLineDash (issue-916). Pattern lives in gradient_positions
        // (an existing vector<float> reuse — avoids growing CanvasDrawCmd).
        case CanvasDrawCmd::Type::set_line_dash:
            canvas.set_line_dash(cmd.gradient_positions.data(),
                                 static_cast<int>(cmd.gradient_positions.size()),
                                 cmd.extra);
            break;

        // Canvas2D shadow* state (issue-1434 batch 7). Sticky values that
        // the underlying canvas honors on subsequent fill/stroke/text
        // draws. Stored in `color` (set_shadow_color) or `extra`
        // (set_shadow_blur / offset_x / offset_y) per the enum doc.
        case CanvasDrawCmd::Type::set_shadow_color:
            canvas.set_shadow_color(cmd.color);
            break;
        case CanvasDrawCmd::Type::set_shadow_blur:
            canvas.set_shadow_blur(cmd.extra);
            break;
        case CanvasDrawCmd::Type::set_shadow_offset_x:
            canvas.set_shadow_offset_x(cmd.extra);
            break;
        case CanvasDrawCmd::Type::set_shadow_offset_y:
            canvas.set_shadow_offset_y(cmd.extra);
            break;
        // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.miterLimit and
        // ctx.imageSmoothingEnabled / Quality. Sticky stroke / image state
        // pushed by the JS shim. SkiaCanvas / CoreGraphicsCanvas honour
        // them on the next stroke / drawImage; RecordingCanvas captures
        // a single setter cmd per change so tests can assert flush order.
        case CanvasDrawCmd::Type::set_miter_limit:
            canvas.set_miter_limit(cmd.extra);
            break;
        case CanvasDrawCmd::Type::set_image_smoothing: {
            using Q = canvas::Canvas::ImageSmoothingQuality;
            Q q = Q::low;
            int qi = static_cast<int>(cmd.extra);
            if (qi == 1) q = Q::medium;
            else if (qi == 2) q = Q::high;
            canvas.set_image_smoothing(cmd.int_val != 0, q);
            break;
        }

        // pulp #1520 — Canvas2D ctx.direction / ctx.filter sticky state.
        // Direction enum (0=ltr, 1=rtl, 2=inherit) packed into int_val;
        // filter raw CSS string in `text`. SkiaCanvas wraps the next
        // text/image draw with the corresponding shaper flag /
        // SkImageFilter chain. RecordingCanvas captures a single setter
        // cmd per change so canvas2d harness tests can assert flush
        // order. Other backends accept the no-op default.
        case CanvasDrawCmd::Type::set_direction: {
            using D = canvas::Canvas::TextDirection;
            D d = D::ltr;
            if (cmd.int_val == 1) d = D::rtl;
            else if (cmd.int_val == 2) d = D::inherit;
            canvas.set_direction(d);
            break;
        }
        case CanvasDrawCmd::Type::set_filter:
            canvas.set_filter(cmd.text);
            break;

        // putImageData (issue-916). Pixels packed in cmd.text as
        // raw RGBA bytes; int_val = width; x2 = height (as float, will round).
        case CanvasDrawCmd::Type::put_image_data: {
            int width  = cmd.int_val;
            int height = static_cast<int>(cmd.x2);
            const auto* px = reinterpret_cast<const uint8_t*>(cmd.text.data());
            canvas.write_pixels(px, width, height,
                                static_cast<int>(cmd.x),
                                static_cast<int>(cmd.y));
            break;
        }
        }
    }

    // pulp #1368 — restore back to the depth captured at entry. This
    // single call unconditionally pops:
    //   * any leftover `ctx.save()` the JS draw script forgot to match
    //     with `ctx.restore()` (the original #1369 fix), AND
    //   * the per-canvas `save_layer()` opened above, so the offscreen
    //     layer is composited back into the parent surface via
    //     SrcOver — matching HTML <canvas> per-element backing-store
    //     semantics where one canvas's clearRect cannot affect any
    //     sibling canvas. Without the layer the kClear blend in
    //     clear_rect() (SkBlendMode::kClear / CGContextClearRect)
    //     unconditionally zeros destination texels on the shared
    //     parent surface, erasing pixels that sibling canvases just
    //     painted. The single restore_to_count(N) handles both cases
    //     uniformly: target N == pre-entry depth, so everything pushed
    //     in this paint() — leftover JS saves + the save_layer — is
    //     popped together.
    //
    // On backends without an introspectable save stack (default
    // `restore_to_count` / `save_count() == 0` in the Canvas base) this
    // is a no-op; the live SkiaCanvas / CoreGraphicsCanvas backends
    // honor it. Tests use RecordingCanvas which models the same
    // contract.
    canvas.restore_to_count(saved_depth);
}

} // namespace pulp::view
