#include <pulp/canvas/canvas.hpp>

#include <cmath>

namespace pulp::canvas {

namespace {

// Right-multiply current = current * other. Matches the semantics of
// SkCanvas::concat / CGContextConcatCTM where the supplied transform is
// applied to user-space coordinates *before* the existing CTM, so points
// flow current(other(p)). Layout matches CanvasRenderingContext2D affine:
//   [ a c e ]
//   [ b d f ]
//   [ 0 0 1 ]
inline Canvas::AffineTransform2x3 concat(const Canvas::AffineTransform2x3& cur,
                                         const Canvas::AffineTransform2x3& m) {
    Canvas::AffineTransform2x3 out;
    out.a = cur.a * m.a + cur.c * m.b;
    out.b = cur.b * m.a + cur.d * m.b;
    out.c = cur.a * m.c + cur.c * m.d;
    out.d = cur.b * m.c + cur.d * m.d;
    out.e = cur.a * m.e + cur.c * m.f + cur.e;
    out.f = cur.b * m.e + cur.d * m.f + cur.f;
    return out;
}

} // namespace

size_t RecordingCanvas::count(DrawCommand::Type type) const {
    size_t n = 0;
    for (auto& cmd : commands_)
        if (cmd.type == type) ++n;
    return n;
}

void RecordingCanvas::save() {
    commands_.push_back({DrawCommand::Type::save});
    ++save_depth_;
    ctm_stack_.push_back(ctm_);
}

void RecordingCanvas::restore() {
    commands_.push_back({DrawCommand::Type::restore});
    if (save_depth_ > 0) --save_depth_;
    if (!ctm_stack_.empty()) {
        ctm_ = ctm_stack_.back();
        ctm_stack_.pop_back();
    }
}

void RecordingCanvas::restore_to_count(int target) {
    // pulp #1368 — pop saves until depth matches `target`. Mirrors
    // SkCanvas::restoreToCount semantics so CanvasWidget::paint() can
    // defend against an unbalanced JS draw script. We record one
    // DrawCommand::Type::restore per popped level so tests can assert
    // on the count.
    if (target < 0) target = 0;
    while (save_depth_ > target) {
        commands_.push_back({DrawCommand::Type::restore});
        --save_depth_;
        if (!ctm_stack_.empty()) {
            ctm_ = ctm_stack_.back();
            ctm_stack_.pop_back();
        }
    }
}

void RecordingCanvas::translate(float x, float y) {
    DrawCommand cmd{DrawCommand::Type::translate};
    cmd.f[0] = x; cmd.f[1] = y;
    commands_.push_back(cmd);
    AffineTransform2x3 t{1, 0, 0, 1, x, y};
    ctm_ = concat(ctm_, t);
}

void RecordingCanvas::scale(float sx, float sy) {
    DrawCommand cmd{DrawCommand::Type::scale};
    cmd.f[0] = sx; cmd.f[1] = sy;
    commands_.push_back(cmd);
    AffineTransform2x3 t{sx, 0, 0, sy, 0, 0};
    ctm_ = concat(ctm_, t);
}

void RecordingCanvas::rotate(float radians) {
    DrawCommand cmd{DrawCommand::Type::rotate};
    cmd.f[0] = radians;
    commands_.push_back(cmd);
    float cs = std::cos(radians);
    float sn = std::sin(radians);
    AffineTransform2x3 t{cs, sn, -sn, cs, 0, 0};
    ctm_ = concat(ctm_, t);
}

void RecordingCanvas::set_transform(float a, float b, float c,
                                    float d, float e, float f) {
    DrawCommand cmd{DrawCommand::Type::set_transform};
    cmd.f[0] = a; cmd.f[1] = b; cmd.f[2] = c;
    cmd.f[3] = d; cmd.f[4] = e; cmd.f[5] = f;
    commands_.push_back(cmd);
    ctm_ = AffineTransform2x3{a, b, c, d, e, f};
}

void RecordingCanvas::capture_paint_baseline_transform() {
    ++baseline_capture_count_;
}

void RecordingCanvas::concat_transform(float a, float b, float c,
                                       float d, float e, float f) {
    DrawCommand cmd{DrawCommand::Type::concat_transform};
    cmd.f[0] = a; cmd.f[1] = b; cmd.f[2] = c;
    cmd.f[3] = d; cmd.f[4] = e; cmd.f[5] = f;
    commands_.push_back(cmd);
    AffineTransform2x3 m{a, b, c, d, e, f};
    ctm_ = concat(ctm_, m);
}

Canvas::AffineTransform2x3 RecordingCanvas::current_transform() const {
    return ctm_;
}

void RecordingCanvas::clip_rect(float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::clip_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    commands_.push_back(cmd);
}

void RecordingCanvas::clip() {
    commands_.push_back({DrawCommand::Type::clip});
}

void RecordingCanvas::clip_path_svg(const std::string& svg_path_d) {
    DrawCommand cmd{DrawCommand::Type::clip_path_svg};
    cmd.text = svg_path_d;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_blend_mode(BlendMode mode) {
    DrawCommand cmd{DrawCommand::Type::set_blend_mode};
    cmd.f[0] = static_cast<float>(static_cast<int>(mode));
    commands_.push_back(cmd);
}

void RecordingCanvas::set_fill_color(Color c) {
    DrawCommand cmd{DrawCommand::Type::set_fill_color};
    cmd.color = c;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_stroke_color(Color c) {
    DrawCommand cmd{DrawCommand::Type::set_stroke_color};
    cmd.color = c;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_line_width(float w) {
    DrawCommand cmd{DrawCommand::Type::set_line_width};
    cmd.f[0] = w;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_line_cap(LineCap cap) {
    DrawCommand cmd{DrawCommand::Type::set_line_cap};
    cmd.f[0] = static_cast<float>(cap);
    commands_.push_back(cmd);
}

void RecordingCanvas::set_line_join(LineJoin join) {
    DrawCommand cmd{DrawCommand::Type::set_line_join};
    cmd.f[0] = static_cast<float>(join);
    commands_.push_back(cmd);
}

void RecordingCanvas::fill_rect(float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::fill_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    commands_.push_back(cmd);
}

// pulp #929 — record clearRect distinctly from fill_rect so tests can assert
// CanvasWidget::paint() does not pre-fill its bounds with a solid background.
void RecordingCanvas::clear_rect(float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::clear_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    commands_.push_back(cmd);
}

void RecordingCanvas::stroke_rect(float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::stroke_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    commands_.push_back(cmd);
}

void RecordingCanvas::fill_rounded_rect(float x, float y, float w, float h, float radius) {
    DrawCommand cmd{DrawCommand::Type::fill_rounded_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h; cmd.f[4] = radius;
    commands_.push_back(cmd);
}

void RecordingCanvas::stroke_rounded_rect(float x, float y, float w, float h, float radius) {
    DrawCommand cmd{DrawCommand::Type::stroke_rounded_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h; cmd.f[4] = radius;
    commands_.push_back(cmd);
}

void RecordingCanvas::fill_circle(float cx, float cy, float radius) {
    DrawCommand cmd{DrawCommand::Type::fill_circle};
    cmd.f[0] = cx; cmd.f[1] = cy; cmd.f[2] = radius;
    commands_.push_back(cmd);
}

void RecordingCanvas::stroke_circle(float cx, float cy, float radius) {
    DrawCommand cmd{DrawCommand::Type::stroke_circle};
    cmd.f[0] = cx; cmd.f[1] = cy; cmd.f[2] = radius;
    commands_.push_back(cmd);
}

void RecordingCanvas::stroke_arc(float cx, float cy, float radius,
                                float start_angle, float end_angle) {
    DrawCommand cmd{DrawCommand::Type::stroke_arc};
    cmd.f[0] = cx; cmd.f[1] = cy; cmd.f[2] = radius;
    cmd.f[3] = start_angle; cmd.f[4] = end_angle;
    commands_.push_back(cmd);
}

void RecordingCanvas::stroke_line(float x0, float y0, float x1, float y1) {
    DrawCommand cmd{DrawCommand::Type::stroke_line};
    cmd.f[0] = x0; cmd.f[1] = y0; cmd.f[2] = x1; cmd.f[3] = y1;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_font(const std::string& family, float size) {
    font_size_ = size;
    DrawCommand cmd{DrawCommand::Type::set_font};
    cmd.text = family;
    cmd.f[0] = size;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_font_full(const std::string& family, float size,
                                    int weight, int slant, float letter_spacing) {
    font_size_ = size;
    // Record both the legacy set_font (for back-compat with existing tests
    // that count fonts) and the rich set_font_full so callers can assert
    // on the propagated weight / slant / letter_spacing. pulp #927.
    {
        DrawCommand cmd{DrawCommand::Type::set_font};
        cmd.text = family;
        cmd.f[0] = size;
        commands_.push_back(cmd);
    }
    DrawCommand full{DrawCommand::Type::set_font_full};
    full.text = family;
    full.f[0] = size;
    full.f[1] = static_cast<float>(weight);
    full.f[2] = static_cast<float>(slant);
    full.f[3] = letter_spacing;
    commands_.push_back(full);
}

void RecordingCanvas::set_text_align(TextAlign align) {
    DrawCommand cmd{DrawCommand::Type::set_text_align};
    cmd.f[0] = static_cast<float>(align);
    commands_.push_back(cmd);
}

void RecordingCanvas::fill_text(const std::string& text, float x, float y) {
    DrawCommand cmd{DrawCommand::Type::fill_text};
    cmd.text = text;
    cmd.f[0] = x; cmd.f[1] = y;
    commands_.push_back(cmd);
}

float RecordingCanvas::measure_text(const std::string& text) {
    // Approximate: 7px per character at default size
    return static_cast<float>(text.size()) * 7.0f;
}

// ── issue-916: Canvas2D API gap closures ────────────────────────────────────

void RecordingCanvas::set_line_dash(const float* intervals, int count, float phase) {
    DrawCommand cmd{DrawCommand::Type::set_line_dash};
    cmd.f[0] = phase;
    cmd.floats.assign(intervals, intervals + (count > 0 ? count : 0));
    commands_.push_back(std::move(cmd));
}

bool RecordingCanvas::draw_image_from_data(const uint8_t* data, size_t size,
                                            float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::draw_image};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    cmd.text.assign(reinterpret_cast<const char*>(data), size);
    commands_.push_back(std::move(cmd));
    // Recording backend doesn't actually rasterize anything but we
    // succeeded at capturing the intent.
    return true;
}

bool RecordingCanvas::draw_image_from_file(const std::string& path,
                                            float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::draw_image};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    cmd.text = path;
    commands_.push_back(std::move(cmd));
    return true;
}

bool RecordingCanvas::write_pixels(const uint8_t* data, int width, int height,
                                    int dx, int dy) {
    DrawCommand cmd{DrawCommand::Type::write_pixels};
    cmd.f[0] = static_cast<float>(width);
    cmd.f[1] = static_cast<float>(height);
    cmd.f[2] = static_cast<float>(dx);
    cmd.f[3] = static_cast<float>(dy);
    if (data && width > 0 && height > 0) {
        const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        cmd.text.assign(reinterpret_cast<const char*>(data), n);
    }
    commands_.push_back(std::move(cmd));
    return true;
}

void RecordingCanvas::save_backdrop_filter(float x, float y, float w, float h,
                                            float blur_radius) {
    DrawCommand cmd{DrawCommand::Type::save_backdrop_filter};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    cmd.f[4] = blur_radius;
    commands_.push_back(cmd);
}

// ── issue-925: box-shadow primitive ─────────────────────────────────────────

void Canvas::draw_box_shadow(float x, float y, float w, float h,
                              float dx, float dy, float blur, float spread,
                              Color color, bool inset, float corner_radius) {
    if (color.a <= 0.0f || (w <= 0.0f && h <= 0.0f)) return;
    const int steps = std::max(1, static_cast<int>(std::ceil(blur / 2.0f)));
    const float base_alpha = color.a;
    if (!inset) {
        // Outset: stacked rounded rects expanding outward, fading at edges.
        for (int i = steps; i >= 0; --i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float expand = spread + blur * t;
            float alpha = base_alpha * (1.0f - t) * (1.0f - t);
            set_fill_color(Color::rgba(color.r, color.g, color.b, alpha));
            fill_rounded_rect(x + dx - expand,
                              y + dy - expand,
                              w + expand * 2.0f,
                              h + expand * 2.0f,
                              corner_radius + expand * 0.5f);
        }
    } else {
        // Inset: stack inset rects shrinking inward, clipped to the box.
        save();
        clip_rect(x, y, w, h);
        for (int i = steps; i >= 0; --i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float inset_amount = spread + blur * t;
            float alpha = base_alpha * (1.0f - t) * (1.0f - t);
            set_fill_color(Color::rgba(color.r, color.g, color.b, alpha));
            // Top band
            fill_rect(x, y + dy - inset_amount,
                      w, std::max(1.0f, inset_amount));
            // Bottom band
            fill_rect(x, y + h + dy + inset_amount - std::max(1.0f, inset_amount),
                      w, std::max(1.0f, inset_amount));
            // Left band
            fill_rect(x + dx - inset_amount, y,
                      std::max(1.0f, inset_amount), h);
            // Right band
            fill_rect(x + w + dx + inset_amount - std::max(1.0f, inset_amount),
                      y, std::max(1.0f, inset_amount), h);
        }
        restore();
    }
}

void RecordingCanvas::draw_box_shadow(float x, float y, float w, float h,
                                       float dx, float dy, float blur, float spread,
                                       Color color, bool inset, float corner_radius) {
    DrawCommand cmd{DrawCommand::Type::draw_box_shadow};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    cmd.f[4] = inset ? 1.0f : 0.0f;
    cmd.f[5] = corner_radius;
    // dx, dy, blur, spread go in `floats` so we don't need 10+ float slots.
    cmd.floats = {dx, dy, blur, spread};
    cmd.color = color;
    commands_.push_back(std::move(cmd));
}

// ── issue-1434 batch 7: Canvas2D shadow* sticky state ──────────────────────
// Each setter records one command so widget-level tests can assert that the
// bridge flushed the JS-side `ctx.shadowColor` / `ctx.shadowBlur` /
// `ctx.shadowOffsetX` / `ctx.shadowOffsetY` writes through to the canvas
// before the next draw command. The sticky behavior — that the shadow
// applies to subsequent draws until cleared — is observed by the live
// SkiaCanvas / CoreGraphicsCanvas backends that consume the same setters
// and translate them into per-paint SkImageFilters::DropShadow / CG shadow
// state. RecordingCanvas only models the command stream, not the visual
// effect.
void RecordingCanvas::set_shadow_color(Color color) {
    DrawCommand cmd{DrawCommand::Type::set_shadow_color};
    cmd.color = color;
    commands_.push_back(std::move(cmd));
}

void RecordingCanvas::set_shadow_blur(float blur) {
    DrawCommand cmd{DrawCommand::Type::set_shadow_blur};
    cmd.f[0] = blur;
    commands_.push_back(std::move(cmd));
}

void RecordingCanvas::set_shadow_offset_x(float dx) {
    DrawCommand cmd{DrawCommand::Type::set_shadow_offset_x};
    cmd.f[0] = dx;
    commands_.push_back(std::move(cmd));
}

void RecordingCanvas::set_shadow_offset_y(float dy) {
    DrawCommand cmd{DrawCommand::Type::set_shadow_offset_y};
    cmd.f[0] = dy;
    commands_.push_back(std::move(cmd));
}

// ── issue-1434 bridge-thin gap-fill: Canvas2D state setters ─────────────────
// Mirror the shadow-state recording shape — one cmd per setter so the
// canvas2d bridge harness can assert flush order without rasterizing.
void RecordingCanvas::set_miter_limit(float limit) {
    DrawCommand cmd{DrawCommand::Type::set_miter_limit};
    cmd.f[0] = limit;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_image_smoothing(bool enabled,
                                          ImageSmoothingQuality quality) {
    DrawCommand cmd{DrawCommand::Type::set_image_smoothing};
    cmd.f[0] = enabled ? 1.0f : 0.0f;
    cmd.f[1] = static_cast<float>(quality);
    commands_.push_back(cmd);
}

// pulp #1520 — capture Canvas2D ctx.direction. Enum packed into f[0]
// (0=ltr, 1=rtl, 2=inherit). RecordingCanvas doesn't shape text — it
// models the bridge intent so tests can assert the JS shim flushed
// the direction setter at the right point in the command stream.
void RecordingCanvas::set_direction(TextDirection direction) {
    DrawCommand cmd{DrawCommand::Type::set_direction};
    cmd.f[0] = static_cast<float>(direction);
    commands_.push_back(cmd);
}

// pulp #1520 — capture Canvas2D ctx.filter raw CSS string. The Skia
// backend parses this into an SkImageFilter chain at draw time;
// RecordingCanvas stores the source string verbatim so harness tests
// can round-trip the bridge value without depending on a parser.
void RecordingCanvas::set_filter(const std::string& filter) {
    DrawCommand cmd{DrawCommand::Type::set_filter};
    cmd.text = filter;
    commands_.push_back(std::move(cmd));
}

// pulp #1434 bridge-thin gap-fill — capture Canvas2D ctx.createPattern
// flushes. Image source string lives in `text`; tile modes go in
// f[0] (x) and f[1] (y) as 0 = repeat, 1 = no_repeat. RecordingCanvas
// doesn't decode the image — it models intent only, so the canvas2d
// adapter can assert that the bridge issued the right setter at the
// right point in the command stream.
void RecordingCanvas::set_fill_pattern(const std::string& image_src,
                                        PatternTileMode tile_x,
                                        PatternTileMode tile_y) {
    DrawCommand cmd{DrawCommand::Type::set_fill_pattern};
    cmd.text = image_src;
    cmd.f[0] = (tile_x == PatternTileMode::no_repeat) ? 1.0f : 0.0f;
    cmd.f[1] = (tile_y == PatternTileMode::no_repeat) ? 1.0f : 0.0f;
    commands_.push_back(std::move(cmd));
}

void RecordingCanvas::set_stroke_pattern(const std::string& image_src,
                                          PatternTileMode tile_x,
                                          PatternTileMode tile_y) {
    DrawCommand cmd{DrawCommand::Type::set_stroke_pattern};
    cmd.text = image_src;
    cmd.f[0] = (tile_x == PatternTileMode::no_repeat) ? 1.0f : 0.0f;
    cmd.f[1] = (tile_y == PatternTileMode::no_repeat) ? 1.0f : 0.0f;
    commands_.push_back(std::move(cmd));
}

// ── issue-965: Canvas2D path API recording ──────────────────────────────────
void RecordingCanvas::begin_path() {
    commands_.push_back({DrawCommand::Type::begin_path});
}

void RecordingCanvas::move_to(float x, float y) {
    DrawCommand cmd{DrawCommand::Type::move_to};
    cmd.f[0] = x; cmd.f[1] = y;
    commands_.push_back(cmd);
}

void RecordingCanvas::line_to(float x, float y) {
    DrawCommand cmd{DrawCommand::Type::line_to};
    cmd.f[0] = x; cmd.f[1] = y;
    commands_.push_back(cmd);
}

void RecordingCanvas::quad_to(float cpx, float cpy, float x, float y) {
    DrawCommand cmd{DrawCommand::Type::quad_to};
    cmd.f[0] = cpx; cmd.f[1] = cpy;
    cmd.f[2] = x;   cmd.f[3] = y;
    commands_.push_back(cmd);
}

void RecordingCanvas::cubic_to(float cp1x, float cp1y, float cp2x, float cp2y,
                               float x, float y) {
    DrawCommand cmd{DrawCommand::Type::cubic_to};
    cmd.f[0] = cp1x; cmd.f[1] = cp1y;
    cmd.f[2] = cp2x; cmd.f[3] = cp2y;
    cmd.f[4] = x;    cmd.f[5] = y;
    commands_.push_back(cmd);
}

void RecordingCanvas::close_path() {
    commands_.push_back({DrawCommand::Type::close_path});
}

void RecordingCanvas::fill_current_path() {
    commands_.push_back({DrawCommand::Type::fill_current_path});
}

void RecordingCanvas::stroke_current_path() {
    commands_.push_back({DrawCommand::Type::stroke_current_path});
}

// ── pulp #1521: native arc subpath recording ────────────────────────────────
//
// These mirror the Skia / CG implementations as a pure capture so widget
// tests can assert on the emitted command stream without a raster surface.
// The legacy bezier/lineTo approximations are gone — the arc commands are
// preserved verbatim so consumers see the developer's intent (an arc, not
// N cubic-bezier segments).
void RecordingCanvas::arc(float cx, float cy, float radius,
                          float start_angle, float end_angle,
                          bool anticlockwise) {
    DrawCommand cmd{DrawCommand::Type::arc};
    cmd.f[0] = cx;          cmd.f[1] = cy;
    cmd.f[2] = radius;
    cmd.f[3] = start_angle; cmd.f[4] = end_angle;
    cmd.f[5] = anticlockwise ? 1.0f : 0.0f;
    commands_.push_back(cmd);
}

void RecordingCanvas::arc_to(float x1, float y1, float x2, float y2,
                             float radius) {
    DrawCommand cmd{DrawCommand::Type::arc_to};
    cmd.f[0] = x1; cmd.f[1] = y1;
    cmd.f[2] = x2; cmd.f[3] = y2;
    cmd.f[4] = radius;
    commands_.push_back(cmd);
}

void RecordingCanvas::ellipse(float cx, float cy, float rx, float ry,
                              float rotation,
                              float start_angle, float end_angle,
                              bool anticlockwise) {
    DrawCommand cmd{DrawCommand::Type::ellipse};
    cmd.f[0] = cx;       cmd.f[1] = cy;
    cmd.f[2] = rx;       cmd.f[3] = ry;
    cmd.f[4] = rotation;
    cmd.f[5] = start_angle;
    cmd.floats.push_back(end_angle);
    cmd.floats.push_back(anticlockwise ? 1.0f : 0.0f);
    commands_.push_back(cmd);
}

void RecordingCanvas::round_rect(float x, float y, float w, float h,
                                  float tl_x, float tl_y,
                                  float tr_x, float tr_y,
                                  float br_x, float br_y,
                                  float bl_x, float bl_y) {
    DrawCommand cmd{DrawCommand::Type::round_rect};
    cmd.f[0] = x;    cmd.f[1] = y;
    cmd.f[2] = w;    cmd.f[3] = h;
    cmd.f[4] = tl_x; cmd.f[5] = tl_y;
    cmd.floats = {tr_x, tr_y, br_x, br_y, bl_x, bl_y};
    commands_.push_back(cmd);
}

// ── compile_sksl fallback for non-Skia builds ────────────────────────────────
#ifndef PULP_HAS_SKIA
std::string Canvas::compile_sksl(const std::string& sksl) {
    (void)sksl;
    return "Skia not available — shader compilation requires GPU build";
}
#endif

} // namespace pulp::canvas
