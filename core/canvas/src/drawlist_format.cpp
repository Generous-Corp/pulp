#include <pulp/canvas/drawlist_format.hpp>

#include <cstdio>

namespace pulp::canvas {
namespace {

/// Fixed precision, so a golden does not flap on the last bit of a float.
void append_float(std::string& out, float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    out += buffer;
}

/// Channels are floats in [0,1] and may exceed 1.0 for HDR, so they print as
/// floats. Hex would truncate an HDR channel into a plausible wrong colour.
void append_color(std::string& out, const Color& color) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "rgba(%.3f %.3f %.3f %.3f)", color.r,
                  color.g, color.b, color.a);
    out += buffer;
}

/// Quote and escape, so a text run containing a quote or a newline stays on
/// one line and cannot forge a second command.
void append_text(std::string& out, const std::string& text) {
    out += '"';
    for (char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    out += '"';
}

}  // namespace

const char* draw_command_name(DrawCommand::Type type) {
    using T = DrawCommand::Type;
    // No `default`: a new Type must be named here or this stops compiling.
    switch (type) {
        case T::save: return "save";
        case T::restore: return "restore";
        case T::translate: return "translate";
        case T::scale: return "scale";
        case T::rotate: return "rotate";
        case T::clip_rect: return "clip_rect";
        case T::set_transform: return "set_transform";
        case T::clip: return "clip";
        case T::set_blend_mode: return "set_blend_mode";
        case T::concat_transform: return "concat_transform";
        case T::set_fill_color: return "set_fill_color";
        case T::set_stroke_color: return "set_stroke_color";
        case T::set_line_width: return "set_line_width";
        case T::set_line_cap: return "set_line_cap";
        case T::set_line_join: return "set_line_join";
        case T::fill_rect: return "fill_rect";
        case T::stroke_rect: return "stroke_rect";
        case T::fill_rounded_rect: return "fill_rounded_rect";
        case T::stroke_rounded_rect: return "stroke_rounded_rect";
        case T::fill_circle: return "fill_circle";
        case T::stroke_circle: return "stroke_circle";
        case T::stroke_arc: return "stroke_arc";
        case T::stroke_line: return "stroke_line";
        case T::set_font: return "set_font";
        case T::set_text_align: return "set_text_align";
        case T::fill_text: return "fill_text";
        case T::stroke_text: return "stroke_text";
        case T::set_font_full: return "set_font_full";
        case T::set_line_dash: return "set_line_dash";
        case T::draw_image: return "draw_image";
        case T::write_pixels: return "write_pixels";
        case T::draw_box_shadow: return "draw_box_shadow";
        case T::set_shadow_color: return "set_shadow_color";
        case T::set_shadow_blur: return "set_shadow_blur";
        case T::set_shadow_offset_x: return "set_shadow_offset_x";
        case T::set_shadow_offset_y: return "set_shadow_offset_y";
        case T::set_miter_limit: return "set_miter_limit";
        case T::set_image_smoothing: return "set_image_smoothing";
        case T::set_direction: return "set_direction";
        case T::set_filter: return "set_filter";
        case T::set_fill_pattern: return "set_fill_pattern";
        case T::set_stroke_pattern: return "set_stroke_pattern";
        case T::set_stroke_gradient_linear: return "set_stroke_gradient_linear";
        case T::set_stroke_gradient_radial: return "set_stroke_gradient_radial";
        case T::set_stroke_gradient_radial_two_circles: return "set_stroke_gradient_radial_two_circles";
        case T::set_stroke_gradient_conic: return "set_stroke_gradient_conic";
        case T::clear_stroke_gradient: return "clear_stroke_gradient";
        case T::set_fill_gradient_linear: return "set_fill_gradient_linear";
        case T::set_fill_gradient_radial: return "set_fill_gradient_radial";
        case T::set_fill_gradient_radial_two_circles: return "set_fill_gradient_radial_two_circles";
        case T::set_fill_gradient_conic: return "set_fill_gradient_conic";
        case T::clear_fill_gradient: return "clear_fill_gradient";
        case T::fill_path_object: return "fill_path_object";
        case T::stroke_path_object: return "stroke_path_object";
        case T::clip_path_object: return "clip_path_object";
        case T::draw_path_shadow: return "draw_path_shadow";
        case T::begin_layer: return "begin_layer";
        case T::end_layer: return "end_layer";
        case T::draw_layer: return "draw_layer";
        case T::draw_layer_fitted: return "draw_layer_fitted";
        case T::draw_layer_rotated: return "draw_layer_rotated";
        case T::invalidate_layer: return "invalidate_layer";
        case T::save_backdrop_filter: return "save_backdrop_filter";
        case T::clip_path_svg: return "clip_path_svg";
        case T::clear_rect: return "clear_rect";
        case T::begin_path: return "begin_path";
        case T::move_to: return "move_to";
        case T::line_to: return "line_to";
        case T::quad_to: return "quad_to";
        case T::cubic_to: return "cubic_to";
        case T::close_path: return "close_path";
        case T::fill_current_path: return "fill_current_path";
        case T::stroke_current_path: return "stroke_current_path";
        case T::arc: return "arc";
        case T::arc_to: return "arc_to";
        case T::ellipse: return "ellipse";
        case T::round_rect: return "round_rect";
        case T::save_layer: return "save_layer";
        case T::save_layer_blend: return "save_layer_blend";
        case T::save_layer_filters: return "save_layer_filters";
        case T::save_layer_mask: return "save_layer_mask";
        case T::save_layer_bloom: return "save_layer_bloom";
        case T::draw_sksl: return "draw_sksl";
    }
    return "unknown";
}

std::string format_command(const DrawCommand& command) {
    std::string out = draw_command_name(command.type);
    for (float value : command.f) {
        out += ' ';
        append_float(out, value);
    }
    // Always printed, for the same reason every float is: omitting a
    // "default" value hides it, and opaque black is a colour a command can
    // legitimately carry.
    out += ' ';
    append_color(out, command.color);
    if (!command.text.empty()) {
        out += ' ';
        append_text(out, command.text);
    }
    if (!command.floats.empty()) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), " +%zu floats", command.floats.size());
        out += buffer;
    }
    return out;
}

std::string format_commands(const std::vector<DrawCommand>& commands) {
    std::string out;
    for (const DrawCommand& command : commands) {
        out += format_command(command);
        out += '\n';
    }
    return out;
}

}  // namespace pulp::canvas
