#include "retained_shader_geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>

namespace pulp::view {
namespace {

struct State {
    std::string error;
    std::uint64_t topology_hash = 1469598103934665603ull;
    std::uint32_t leaves = 0;
    std::uint32_t leaf_index = 0;
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool have_bounds = false;

    void mix(std::string_view value) {
        for (const auto c : value) {
            topology_hash ^= static_cast<std::uint8_t>(c);
            topology_hash *= 1099511628211ull;
        }
    }
    void include(float x, float y, float w, float h) {
        if (!have_bounds) {
            min_x = x; min_y = y; max_x = x + w; max_y = y + h;
            have_bounds = true;
            return;
        }
        min_x = std::min(min_x, x); min_y = std::min(min_y, y);
        max_x = std::max(max_x, x + w); max_y = std::max(max_y, y + h);
    }
};

std::string number_text(double value) {
    std::ostringstream out;
    out.setf(std::ios::scientific);
    out.precision(8);
    out << value;
    return out.str();
}

std::string emit_leaf(const choc::value::ValueView& node, State& state,
                      std::vector<canvas::Canvas::NamedUniform>& uniforms) {
    const auto shape = node["shape"].getWithDefault<std::string>("");
    if (shape.empty()) {
        state.error = "Shader geometry leaf shape must not be empty";
        return {};
    }
    if (++state.leaves > 16) {
        state.error = "Shader geometry exceeds maximum of 16 leaves";
        return {};
    }
    const auto index = state.leaf_index++;
    const auto prefix = "pulp_leaf" + std::to_string(index) + "_";
    state.mix("leaf:"); state.mix(shape);
    auto number = [&](const char* key, float fallback) {
        const float value = node.hasObjectMember(key)
                                ? static_cast<float>(node[key].getWithDefault<double>(fallback))
                                : fallback;
        canvas::Canvas::NamedUniform uniform;
        uniform.name = prefix + key;
        uniform.count = 1;
        uniform.v[0] = value;
        uniforms.push_back(std::move(uniform));
        return value;
    };
    const auto x = number("x", 0.0f);
    const auto y = number("y", 0.0f);
    const auto w = number("w", 0.0f);
    const auto h = number("h", 0.0f);
    const auto corner = number("cornerRadius", 0.0f);
    const auto inner = number("innerRadius", 0.5f);
    const auto arc_start = number("arcStart", 0.0f);
    const auto arc_sweep = number("arcSweep", 4.712f);
    const auto power = number("squirclePower", 4.0f);
    const auto arm = number("armWidth", 0.3f);
    const auto cx = number("bezierCX", 0.0f);
    const auto cy = number("bezierCY", -1.0f);
    state.include(x, y, w, h);
    const auto p = "(p - float2((" + prefix + "x+" + prefix + "w*0.5)-resolution.x*0.5, (" +
                   prefix + "y+" + prefix + "h*0.5)-resolution.y*0.5))";
    (void)corner; (void)inner; (void)arc_start; (void)arc_sweep;
    (void)power; (void)arm; (void)cx; (void)cy;
    if (shape == "circle") return "sdCircle(" + p + ", min(" + prefix + "w," + prefix + "h)*0.5)";
    if (shape == "rect") return "sdBox(" + p + ", float2(" + prefix + "w*0.5," + prefix + "h*0.5))";
    if (shape == "rounded_rect") return "sdRoundBox(" + p + ", float2(" + prefix + "w*0.5," + prefix + "h*0.5), " + prefix + "cornerRadius)";
    if (shape == "diamond") return "sdDiamond(" + p + ", min(" + prefix + "w," + prefix + "h)*0.5)";
    if (shape == "squircle") return "sdSquircle(" + p + ", float2(" + prefix + "w*0.5," + prefix + "h*0.5), " + prefix + "squirclePower)";
    if (shape == "triangle") return "sdTriangle(" + p + ", min(" + prefix + "w," + prefix + "h)*0.5)";
    if (shape == "ring") return "sdRing(" + p + ", min(" + prefix + "w," + prefix + "h)*0.5, min(" + prefix + "w," + prefix + "h)*0.5*" + prefix + "innerRadius)";
    if (shape == "stadium") return "sdStadium(" + p + ", float2(" + prefix + "w*0.5," + prefix + "h*0.5))";
    if (shape == "cross") return "sdCross(" + p + ", float2(" + prefix + "w*0.5," + prefix + "h*0.5), " + prefix + "armWidth)";
    if (shape == "flat_segment") return "sdFlatSegment(" + p + ", float2(" + prefix + "w*0.5," + prefix + "h*0.5))";
    if (shape == "rounded_segment") return "sdRoundedSegment(" + p + ", " + prefix + "w*0.5, " + prefix + "h)";
    if (shape == "flat_arc" || shape == "arc") return "sdFlatArc(" + p + ", min(" + prefix + "w," + prefix + "h)*0.5, min(" + prefix + "w," + prefix + "h)*0.5*" + prefix + "innerRadius, " + prefix + "arcStart, " + prefix + "arcSweep)";
    if (shape == "quadratic_bezier") return "sdQuadBezier(" + p + ", float2(-" + prefix + "w*0.5,0), float2(" + prefix + "bezierCX*" + prefix + "w*0.5," + prefix + "bezierCY*" + prefix + "h*0.5), float2(" + prefix + "w*0.5,0), " + prefix + "h)";
    state.error = "Unsupported shader geometry leaf shape '" + shape + "'";
    return {};
}

std::string emit(const choc::value::ValueView& node, int depth, bool right,
                 State& state, std::vector<canvas::Canvas::NamedUniform>& uniforms) {
    if (state.error.size()) return {};
    if (!node.isObject()) { state.error = "Each shader geometry node must be an object"; return {}; }
    if (depth > 8) { state.error = "Shader geometry exceeds maximum depth 8"; return {}; }
    if (right && node.hasObjectMember("op")) {
        state.error = "Shader geometry must be left-leaning; right child may not be a composite";
        return {};
    }
    if (node.hasObjectMember("shape")) return emit_leaf(node, state, uniforms);
    if (!node.hasObjectMember("op") || !node.hasObjectMember("children") ||
        !node["children"].isArray() || node["children"].size() != 2) {
        state.error = "Composite shader geometry needs an op and exactly two children";
        return {};
    }
    const auto op = node["op"].getWithDefault<std::string>("");
    if (op != "union" && op != "intersect" && op != "subtract" &&
        op != "smoothUnion" && op != "smoothSubtract") {
        state.error = "Unknown shader geometry operator '" + op + "'";
        return {};
    }
    state.mix("op:"); state.mix(op);
    const auto left = emit(node["children"][0], depth + 1, false, state, uniforms);
    const auto right_expr = emit(node["children"][1], depth + 1, true, state, uniforms);
    if (state.error.size()) return {};
    if (op == "union") return "min(" + left + "," + right_expr + ")";
    if (op == "intersect") return "max(" + left + "," + right_expr + ")";
    if (op == "subtract") return "max(" + left + ",-((" + right_expr + ")))";
    const auto k = node.hasObjectMember("k") ? node["k"].getWithDefault<double>(0.0) : 0.0;
    if (op == "smoothUnion") return "pulp_smooth_union(" + left + "," + right_expr + "," + number_text(k) + ")";
    return "pulp_smooth_subtract(" + left + "," + right_expr + "," + number_text(k) + ")";
}

} // namespace

std::optional<RetainedShaderGeometry> parse_retained_shader_geometry(
    const choc::value::ValueView& root, std::string& error) {
    State state;
    RetainedShaderGeometry result;
    result.geometry.shape = canvas::Canvas::SDFShape::rect;
    result.geometry.sdf_expression = emit(root, 1, false, state, result.geometry.leaf_uniforms);
    if (!state.error.empty()) { error = state.error; return std::nullopt; }
    result.geometry.leaf_count = state.leaf_index;
    result.geometry.topology_hash = state.topology_hash;
    result.x = state.min_x; result.y = state.min_y;
    result.w = std::max(0.0f, state.max_x - state.min_x);
    result.h = std::max(0.0f, state.max_y - state.min_y);
    return result;
}

} // namespace pulp::view
