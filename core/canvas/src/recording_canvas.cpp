#include <pulp/canvas/canvas.hpp>

namespace pulp::canvas {

size_t RecordingCanvas::count(DrawCommand::Type type) const {
    size_t n = 0;
    for (auto& cmd : commands_)
        if (cmd.type == type) ++n;
    return n;
}

void RecordingCanvas::save() {
    commands_.push_back({DrawCommand::Type::save});
}

void RecordingCanvas::restore() {
    commands_.push_back({DrawCommand::Type::restore});
}

void RecordingCanvas::translate(float x, float y) {
    DrawCommand cmd{DrawCommand::Type::translate};
    cmd.f[0] = x; cmd.f[1] = y;
    commands_.push_back(cmd);
}

void RecordingCanvas::scale(float sx, float sy) {
    DrawCommand cmd{DrawCommand::Type::scale};
    cmd.f[0] = sx; cmd.f[1] = sy;
    commands_.push_back(cmd);
}

void RecordingCanvas::rotate(float radians) {
    DrawCommand cmd{DrawCommand::Type::rotate};
    cmd.f[0] = radians;
    commands_.push_back(cmd);
}

void RecordingCanvas::set_transform(float a, float b, float c,
                                    float d, float e, float f) {
    DrawCommand cmd{DrawCommand::Type::set_transform};
    cmd.f[0] = a; cmd.f[1] = b; cmd.f[2] = c;
    cmd.f[3] = d; cmd.f[4] = e; cmd.f[5] = f;
    commands_.push_back(cmd);
}

void RecordingCanvas::capture_paint_baseline_transform() {
    ++baseline_capture_count_;
}

void RecordingCanvas::clip_rect(float x, float y, float w, float h) {
    DrawCommand cmd{DrawCommand::Type::clip_rect};
    cmd.f[0] = x; cmd.f[1] = y; cmd.f[2] = w; cmd.f[3] = h;
    commands_.push_back(cmd);
}

void RecordingCanvas::clip() {
    commands_.push_back({DrawCommand::Type::clip});
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

// ── compile_sksl fallback for non-Skia builds ────────────────────────────────
#ifndef PULP_HAS_SKIA
std::string Canvas::compile_sksl(const std::string& sksl) {
    (void)sksl;
    return "Skia not available — shader compilation requires GPU build";
}
#endif

} // namespace pulp::canvas
