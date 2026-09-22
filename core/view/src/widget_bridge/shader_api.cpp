// widget_bridge/shader_api.cpp - shader registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/custom_shader_host.hpp>
#include "api_registry.hpp"

#include <string>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <optional>
#include <functional>
#include <cstdint>
#include <sstream>
#include <regex>
#include <vector>

namespace pulp::view {
namespace {

choc::value::Value shader_result(bool success, const std::string& error) {
    auto result = choc::value::createObject("");
    result.addMember("success", choc::value::createBool(success));
    result.addMember("error", choc::value::createString(error));
    return result;
}

std::optional<canvas::Canvas::ShaderGeometry>
parse_shader_geometry(const choc::value::ValueView& options) {
    if (!options.isObject() || !options.hasObjectMember("geometry")) return std::nullopt;
    const auto& value = options["geometry"];
    canvas::Canvas::ShaderGeometry geometry;
    if (value.isString()) {
        const auto name = value.getWithDefault<std::string>("");
        if (name != "auto") return std::nullopt;
        geometry.shape = canvas::Canvas::SDFShape::flat_arc;
    } else if (value.isObject()) {
        const auto shape = value.hasObjectMember("shape")
                               ? value["shape"].getWithDefault<std::string>("flat_arc")
                               : "flat_arc";
        const std::pair<const char*, canvas::Canvas::SDFShape> shapes[] = {
            {"rect", canvas::Canvas::SDFShape::rect},
            {"circle", canvas::Canvas::SDFShape::circle},
            {"rounded_rect", canvas::Canvas::SDFShape::rounded_rect},
            {"diamond", canvas::Canvas::SDFShape::diamond},
            {"squircle", canvas::Canvas::SDFShape::squircle},
            {"triangle", canvas::Canvas::SDFShape::triangle},
            {"flat_arc", canvas::Canvas::SDFShape::flat_arc},
            {"ring", canvas::Canvas::SDFShape::ring},
            {"stadium", canvas::Canvas::SDFShape::stadium},
            {"cross", canvas::Canvas::SDFShape::cross},
            {"flat_segment", canvas::Canvas::SDFShape::flat_segment},
            {"rounded_segment", canvas::Canvas::SDFShape::rounded_segment},
            {"arc", canvas::Canvas::SDFShape::arc},
            {"quadratic_bezier", canvas::Canvas::SDFShape::quadratic_bezier},
        };
        bool found = false;
        for (const auto& candidate : shapes) {
            if (shape == candidate.first) {
                geometry.shape = candidate.second;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
        auto number = [&](const char* key, float fallback) {
            return value.hasObjectMember(key)
                       ? static_cast<float>(value[key].getWithDefault<double>(fallback))
                       : fallback;
        };
        geometry.style.corner_radius = number("cornerRadius", geometry.style.corner_radius);
        geometry.style.stroke_width = number("strokeWidth", geometry.style.stroke_width);
        geometry.style.arc_start = number("arcStart", geometry.style.arc_start);
        geometry.style.arc_sweep = number("arcSweep", geometry.style.arc_sweep);
        geometry.style.inner_radius = number("innerRadius", geometry.style.inner_radius);
        geometry.style.squircle_power = number("squirclePower", geometry.style.squircle_power);
        geometry.style.arm_width = number("armWidth", geometry.style.arm_width);
        geometry.style.bezier_cx = number("bezierCX", geometry.style.bezier_cx);
        geometry.style.bezier_cy = number("bezierCY", geometry.style.bezier_cy);
    } else {
        return std::nullopt;
    }
    return geometry;
}

struct GeometryValidation {
    std::string error;
    std::uint64_t topology_hash = 1469598103934665603ull;
    int leaves = 0;
    int max_depth = 0;
};

GeometryValidation validate_geometry_tree(const choc::value::ValueView& root) {
    GeometryValidation result;
    auto mix = [&](std::string_view text) {
        for (const auto c : text) {
            result.topology_hash ^= static_cast<std::uint8_t>(c);
            result.topology_hash *= 1099511628211ull;
        }
    };
    std::function<void(const choc::value::ValueView&, int, bool)> visit =
        [&](const choc::value::ValueView& node, int depth, bool right_child) {
            if (!result.error.empty()) return;
            result.max_depth = std::max(result.max_depth, depth);
            if (depth > 8) {
                result.error = "Shader geometry exceeds maximum depth 8";
                return;
            }
            if (!node.isObject()) {
                result.error = "Each shader geometry node must be an object";
                return;
            }
            if (right_child && node.hasObjectMember("op")) {
                result.error = "Shader geometry must be left-leaning; right child may not be a composite";
                return;
            }
            if (node.hasObjectMember("shape")) {
                const auto shape = node["shape"].getWithDefault<std::string>("");
                if (shape.empty()) {
                    result.error = "Shader geometry leaf shape must not be empty";
                    return;
                }
                ++result.leaves;
                if (result.leaves > 16) {
                    result.error = "Shader geometry exceeds maximum of 16 leaves";
                    return;
                }
                mix("leaf:"); mix(shape);
                return;
            }
            if (!node.hasObjectMember("op") || !node.hasObjectMember("children") ||
                !node["children"].isArray() || node["children"].size() != 2) {
                result.error = "Composite shader geometry needs an op and exactly two children";
                return;
            }
            const auto op = node["op"].getWithDefault<std::string>("");
            if (op != "union" && op != "intersect" && op != "subtract" &&
                op != "smoothUnion" && op != "smoothSubtract") {
                result.error = "Unknown shader geometry operator '" + op + "'";
                return;
            }
            mix("op:"); mix(op);
            visit(node["children"][0], depth + 1, false);
            visit(node["children"][1], depth + 1, true);
        };
    visit(root, 1, false);
    return result;
}

std::string geometry_number(const choc::value::ValueView& node, const char* key,
                            double fallback) {
    const auto value = node.hasObjectMember(key)
                           ? node[key].getWithDefault<double>(fallback)
                           : fallback;
    std::ostringstream out;
    out.setf(std::ios::scientific);
    out.precision(8);
    out << value;
    return out.str();
}

std::string emit_geometry_expression(const choc::value::ValueView& node,
                                     std::string& error) {
    if (node.hasObjectMember("shape")) {
        const auto shape = node["shape"].getWithDefault<std::string>("");
        const auto x = geometry_number(node, "x", 0.0);
        const auto y = geometry_number(node, "y", 0.0);
        const auto w = geometry_number(node, "w", 0.0);
        const auto h = geometry_number(node, "h", 0.0);
        const auto px = "(p - float2((" + x + "+" + w + "*0.5)-resolution.x*0.5, (" +
                        y + "+" + h + "*0.5)-resolution.y*0.5))";
        if (shape == "circle")
            return "sdCircle(" + px + ", min(" + w + "," + h + ")*0.5)";
        if (shape == "rect")
            return "sdBox(" + px + ", float2(" + w + "*0.5," + h + "*0.5))";
        if (shape == "rounded_rect")
            return "sdRoundBox(" + px + ", float2(" + w + "*0.5," + h + "*0.5), " +
                   geometry_number(node, "cornerRadius", 0.0) + ")";
        if (shape == "diamond")
            return "sdDiamond(" + px + ", min(" + w + "," + h + ")*0.5)";
        if (shape == "squircle")
            return "sdSquircle(" + px + ", float2(" + w + "*0.5," + h + "*0.5), " +
                   geometry_number(node, "squirclePower", 4.0) + ")";
        if (shape == "triangle")
            return "sdTriangle(" + px + ", min(" + w + "," + h + ")*0.5)";
        if (shape == "ring")
            return "sdRing(" + px + ", min(" + w + "," + h + ")*0.5, min(" + w + "," +
                   h + ")*0.5*" + geometry_number(node, "innerRadius", 0.5) + ")";
        if (shape == "stadium")
            return "sdStadium(" + px + ", float2(" + w + "*0.5," + h + "*0.5))";
        if (shape == "cross")
            return "sdCross(" + px + ", float2(" + w + "*0.5," + h + "*0.5), " +
                   geometry_number(node, "armWidth", 0.3) + ")";
        if (shape == "flat_segment")
            return "sdFlatSegment(" + px + ", float2(" + w + "*0.5," + h + "*0.5))";
        if (shape == "rounded_segment")
            return "sdRoundedSegment(" + px + ", " + w + "*0.5, " + h + ")";
        if (shape == "flat_arc" || shape == "arc")
            return "sdFlatArc(" + px + ", min(" + w + "," + h + ")*0.5, min(" + w + "," + h + ")*0.5*" +
                   geometry_number(node, "innerRadius", 0.5) + ", " +
                   geometry_number(node, "arcStart", 0.0) + ", " +
                   geometry_number(node, "arcSweep", 4.712) + ")";
        if (shape == "quadratic_bezier")
            return "sdQuadBezier(" + px + ", float2(-" + w + "*0.5,0), float2(" +
                   geometry_number(node, "bezierCX", 0.0) + "*" + w + "*0.5," +
                   geometry_number(node, "bezierCY", -1.0) + "*" + h + "*0.5), float2(" +
                   w + "*0.5,0), " + h + ")";
        error = "Unsupported shader geometry leaf shape '" + shape + "'";
        return {};
    }
    const auto op = node["op"].getWithDefault<std::string>("");
    const auto left = emit_geometry_expression(node["children"][0], error);
    const auto right = emit_geometry_expression(node["children"][1], error);
    if (!error.empty()) return {};
    if (op == "union") return "min(" + left + "," + right + ")";
    if (op == "intersect") return "max(" + left + "," + right + ")";
    if (op == "subtract") return "max(" + left + ",-((" + right + ")))";
    const auto k = geometry_number(node, "k", 0.0);
    if (op == "smoothUnion")
        return "pulp_smooth_union(" + left + "," + right + "," + k + ")";
    if (op == "smoothSubtract")
        return "pulp_smooth_subtract(" + left + "," + right + "," + k + ")";
    error = "Unknown shader geometry operator '" + op + "'";
    return {};
}

} // namespace

void BridgeRegistrars::register_shader_widget_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // compileShader(sksl_code) -> {success: bool, error: string}
    // Validates SkSL shader code by actually compiling via SkRuntimeEffect.
    register_bridge_function(api, "compileShader", [](choc::javascript::ArgumentList args) {
        auto code = args.get<std::string>(0, "");
        if (code.empty()) return shader_result(false, "Empty shader code");
        auto error = canvas::Canvas::compile_sksl(code);
        return shader_result(error.empty(), error);
    });

    // setWidgetShader(id, skslCode) -> {success: bool, error: string}
    //
    // Installs an SkSL body shader on a shader-capable widget (any
    // CustomShaderHost). The shader is compiled first and is NOT installed if
    // it fails: a widget holding un-compilable SkSL paints the CPU fallback
    // rect forever, which looks like a rendering bug rather than a shader
    // error. Every rejection path reports why — silently doing nothing was
    // the old behavior and it stranded callers.
    register_bridge_function(api, "setWidgetShader", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto sksl = args.get<std::string>(1, "");
        const bool has_options = args.numArgs >= 3 && args[2] != nullptr;

        auto* v = self.widget(id);
        if (!v) return shader_result(false, "No widget with id '" + id + "'");

        auto* host = dynamic_cast<CustomShaderHost*>(v);
        if (!host)
            return shader_result(
                false, "Widget '" + id + "' does not support custom shaders");

        if (sksl.empty())
            return shader_result(
                false, "Empty shader code — use clearWidgetShader() to remove a shader");

        std::optional<canvas::Canvas::ShaderGeometry> geometry;
        float reach = 0.0f;
        if (has_options) {
            const auto& options = *args[2];
            if (!options.isObject()) return shader_result(false, "Shader options must be an object");
            if (options.hasObjectMember("reach")) {
                reach = static_cast<float>(options["reach"].getWithDefault<double>(0.0));
                if (!std::isfinite(reach) || reach < 0.0f)
                    return shader_result(false, "Shader reach must be finite and non-negative");
            }
            if (options.hasObjectMember("geometry")) {
                geometry = parse_shader_geometry(options);
                if (!geometry)
                    return shader_result(false, "Invalid shader geometry; expected auto or a supported shape object");
            }
            if (options.hasObjectMember("feather") && options["feather"].isObject() &&
                options["feather"].hasObjectMember("sigma")) {
                const auto& feather = options["feather"];
                const float sigma = static_cast<float>(
                    feather["sigma"].getWithDefault<double>(0.0));
                if (!std::isfinite(sigma) || sigma < 0.0f)
                    return shader_result(false, "Feather sigma must be finite and non-negative");
                reach = std::max(reach, std::ceil(3.0f * sigma));
                if (geometry) {
                    geometry->style.feather_sigma = sigma;
                    const auto curve = feather.hasObjectMember("curve")
                                           ? feather["curve"].getWithDefault<std::string>("gaussian")
                                           : "gaussian";
                    geometry->style.feather_curve = curve == "linear" ? 1 : 0;
                    const auto mode = feather.hasObjectMember("mode")
                                          ? feather["mode"].getWithDefault<std::string>("uniform")
                                          : "uniform";
                    static constexpr const char* modes[] = {
                        "uniform", "glow", "inner", "outer", "inset", "radial", "sweep"};
                    geometry->style.feather_mode = 0;
                    for (int i = 0; i < 7; ++i)
                        if (mode == modes[i]) geometry->style.feather_mode = i;
                }
            }
        }
        const bool structured = sksl.find("PulpFragment shade") != std::string::npos;
        auto error = structured
                         ? canvas::Canvas::compile_sdf_chart_sksl(
                               geometry ? geometry->shape : canvas::Canvas::SDFShape::flat_arc, sksl)
                         : canvas::Canvas::compile_sksl(sksl);
        if (!error.empty()) return shader_result(false, error);

        host->set_custom_shader(std::move(sksl));
        host->set_shader_geometry(std::move(geometry));
        host->set_shader_reach(reach);
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "setWidgetShaderGeometry", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (args.numArgs < 2 || args[1] == nullptr)
            return shader_result(false, "Shader geometry must be an object");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host)
            return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        const auto validation = validate_geometry_tree(*args[1]);
        if (!validation.error.empty()) return shader_result(false, validation.error);
        std::string expression_error;
        const auto expression = emit_geometry_expression(*args[1], expression_error);
        if (!expression_error.empty()) return shader_result(false, expression_error);
        canvas::Canvas::ShaderGeometry geometry;
        geometry.sdf_expression = expression;
        geometry.topology_hash = validation.topology_hash;
        host->set_shader_geometry_spec(choc::json::toString(*args[1], false),
                                       validation.topology_hash);
        host->set_shader_geometry(std::move(geometry));
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "setWidgetShaderChart", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto sksl = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        auto error = canvas::Canvas::compile_sdf_chart_sksl(canvas::Canvas::SDFShape::flat_arc, sksl);
        if (!error.empty()) return shader_result(false, error);
        host->set_chart_shader(std::move(sksl));
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "setWidgetShaderUniforms", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (args.numArgs < 2 || args[1] == nullptr) return shader_result(false, "Uniforms must be an object");
        const auto& spec = *args[1];
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        if (!spec.isObject()) return shader_result(false, "Uniforms must be an object");
        std::vector<canvas::Canvas::NamedUniform> uniforms;
        bool valid = true;
        std::string failure;
        spec.getView().visitObjectMembers([&](std::string_view name, const choc::value::ValueView& value) {
            if (!valid) return;
            if (host->shader_uniform_bound(std::string(name))) {
                valid = false;
                failure = "Uniform '" + std::string(name) + "' is owned by a value binding";
                return;
            }
            canvas::Canvas::NamedUniform u;
            u.name = std::string(name);
            if (value.isArray()) {
                u.count = static_cast<int>(value.size());
                if (u.count < 1 || u.count > 4) { valid = false; failure = "Uniform '" + u.name + "' must have 1 to 4 components"; return; }
                for (int i = 0; i < u.count; ++i) u.v[i] = static_cast<float>(value[i].getWithDefault<double>(0.0));
            } else {
                u.count = 1;
                u.v[0] = static_cast<float>(value.getWithDefault<double>(0.0));
            }
            uniforms.push_back(std::move(u));
        });
        if (!valid) return shader_result(false, failure);
        host->set_shader_uniforms(std::move(uniforms));
        self.request_repaint();
        return shader_result(true, "");
    });

    // bindWidgetShaderUniform(id, uniform, "value:channel")
    // Connects one scalar shader uniform to a live scalar or meter channel.
    // The frame service owns the source lease and applies the existing
    // publish-sequence/staleness rules; this registration only records the
    // declarative edge.
    register_bridge_function(api, "bindWidgetShaderUniform", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto uniform_name = args.get<std::string>(1, "");
        auto source = args.get<std::string>(2, "");
        const auto fail = [&](BindingOutcome outcome, const std::string& message) {
            self.record_binding_attempt(id, source, BindingTarget::uniform, outcome);
            return shader_result(false, message);
        };
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host)
            return fail(v ? BindingOutcome::incompatible_widget : BindingOutcome::null_widget,
                        v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        constexpr std::string_view prefix = "value:";
        if (uniform_name.empty()) return fail(BindingOutcome::empty_param_name, "Uniform name must not be empty");
        if (!host->custom_shader().empty() &&
            !canvas::Canvas::sksl_declares_uniform(host->custom_shader(), uniform_name))
            return fail(BindingOutcome::undeclared_uniform,
                        "Shader does not declare uniform '" + uniform_name + "'");
        const bool is_channel = source.size() > prefix.size() && source.compare(0, prefix.size(), prefix) == 0;
        const std::string channel_name = is_channel ? source.substr(prefix.size()) : std::string{};
        bool found = false;
        float neutral = 0.0f;
        self.visit_value_channels([&](ValueChannelSet* channels) {
            if (!is_channel) return;
            if (!channels) return;
            for (const auto& info : channels->infos()) {
                if (info.name != channel_name) continue;
                if (info.shape != ValueChannelShape::scalar &&
                    info.shape != ValueChannelShape::meter) return;
                found = true;
                neutral = info.neutral;
                return;
            }
        });
        if (is_channel && !found)
            return fail(BindingOutcome::unknown_value_channel,
                        "No scalar or meter value channel named '" + channel_name + "'");
        if (!is_channel) {
            state::ParamID param_id = 0;
            if (!self.parameter_id_for_name(source, param_id))
                return fail(BindingOutcome::unknown_param,
                            "No parameter named '" + source + "'");
        }
        const choc::value::Value* transform =
            args.numArgs >= 4 && args[3] != nullptr ? args[3] : nullptr;
        if (!self.add_shader_uniform_binding(id, uniform_name, source, transform))
            return shader_result(false, "Shader uniform binding was rejected");
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "bindShaderUniform", [&self](choc::javascript::ArgumentList args) {
        const auto id = args.get<std::string>(0, "");
        const auto uniform_name = args.get<std::string>(1, "");
        const auto source = args.get<std::string>(2, "");
        auto* view = self.widget(id);
        auto* host = view ? dynamic_cast<CustomShaderHost*>(view) : nullptr;
        if (!host) return shader_result(false, view ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        if (uniform_name.empty()) return shader_result(false, "Uniform name must not be empty");
        if (!host->custom_shader().empty() &&
            !canvas::Canvas::sksl_declares_uniform(host->custom_shader(), uniform_name)) {
            self.record_binding_attempt(id, source, BindingTarget::uniform,
                                        BindingOutcome::undeclared_uniform);
            return shader_result(false, "Shader does not declare uniform '" + uniform_name + "'");
        }
        const choc::value::Value* transform =
            args.numArgs >= 4 && args[3] != nullptr ? args[3] : nullptr;
        if (!self.add_shader_uniform_binding(id, uniform_name, source, transform))
            return shader_result(false, "Shader uniform binding was rejected");
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "clearWidgetShaderUniformBindings", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        self.clear_shader_uniform_bindings(id);
        host->set_shader_value_bindings({});
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "bindWidgetShaderScope", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto source = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host)
            return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        constexpr std::string_view prefix = "value:";
        if (source.size() <= prefix.size() || source.compare(0, prefix.size(), prefix) != 0)
            return shader_result(false, "Shader scope source must be value:<vector-channel>");
        const auto channel_name = source.substr(prefix.size());
        bool found = false;
        float neutral = 0.0f;
        self.visit_value_channels([&](ValueChannelSet* channels) {
            if (!channels) return;
            for (const auto& info : channels->infos()) {
                if (info.name != channel_name) continue;
                if (info.shape != ValueChannelShape::vector) return;
                found = true;
                neutral = info.neutral;
                return;
            }
        });
        if (!found) return shader_result(false, "No vector value channel named '" + std::string(channel_name) + "'");
        CustomShaderHost::ShaderScopeBinding binding;
        binding.channel_name = std::string(channel_name);
        binding.neutral = neutral;
        host->set_shader_scope_binding(std::move(binding));
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "clearWidgetShaderScope", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        host->set_shader_scope_binding(std::nullopt);
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "getWidgetShaderUniforms", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        auto result = choc::value::createObject("");
        for (const auto& u : host->shader_uniforms()) {
            if (u.count == 1) result.addMember(u.name, choc::value::createFloat64(u.v[0]));
            else {
                std::vector<double> values;
                for (int i = 0; i < u.count; ++i) values.push_back(u.v[i]);
                auto array = choc::value::createArray(values);
                result.addMember(u.name, std::move(array));
            }
        }
        auto declared = choc::value::createEmptyArray();
        const std::regex uniform_pattern(R"(\buniform\s+(float[234]?|half[234]?|int|shader)\s+([A-Za-z_][A-Za-z0-9_]*))");
        for (std::sregex_iterator it(host->custom_shader().begin(), host->custom_shader().end(), uniform_pattern),
             end; it != end; ++it) {
            const auto type = (*it)[1].str();
            const auto name = (*it)[2].str();
            auto entry = choc::value::createObject("");
            entry.addMember("name", choc::value::createString(name));
            entry.addMember("type", choc::value::createString(type));
            const int count = type == "float2" || type == "half2" ? 2
                            : type == "float3" || type == "half3" ? 3
                            : type == "float4" || type == "half4" ? 4 : 1;
            entry.addMember("count", count);
            entry.addMember("bound", host->shader_uniform_bound(name));
            entry.addMember("reserved", name == "resolution" || name == "time" ||
                                         name == "value" || name.rfind("pulp_", 0) == 0);
            for (const auto& binding : host->shader_value_bindings()) {
                if (binding.uniform_name == name) {
                    entry.addMember("source", choc::value::createString(
                        binding.channel_name.empty() ? binding.param_name : "value:" + binding.channel_name));
                    break;
                }
            }
            declared.addArrayElement(std::move(entry));
        }
        result.addMember("declared", std::move(declared));
        return result;
    });

    register_bridge_function(api, "setWidgetShaderReach", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto reach = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
        if (!std::isfinite(reach) || reach < 0.0f)
            return shader_result(false, "Shader reach must be finite and non-negative");
        host->set_shader_reach(reach);
        self.request_repaint();
        return shader_result(true, "");
    });

    // clearWidgetShader(id) -> {success: bool, error: string}
    // Removes the custom shader and restores the default C++ paint path.
    register_bridge_function(api, "clearWidgetShader", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");

        auto* v = self.widget(id);
        if (!v) return shader_result(false, "No widget with id '" + id + "'");

        auto* host = dynamic_cast<CustomShaderHost*>(v);
        if (!host)
            return shader_result(
                false, "Widget '" + id + "' does not support custom shaders");

        host->clear_custom_shader();
        host->set_chart_shader({});
        host->set_shader_geometry(std::nullopt);
        host->set_shader_geometry_spec({}, 0);
        self.request_repaint();
        return shader_result(true, "");
    });

}

void BridgeRegistrars::register_shader_canvas_api(WidgetBridge& self) {
    // `applyShader(canvasId, skslCode)` used to live here. It never compiled or
    // applied anything — it set a `shader.active` theme dimension and returned
    // success for any non-empty string, including un-compilable SkSL and ids
    // that matched no widget. Canvas widgets have no shader path to apply a
    // shader to, so there was nothing for an honest version of it to do.
    //
    // Widget body shaders are reachable through setWidgetShader(); a genuine
    // view-level SkSL post-effect needs a child-shader compositor that does not
    // exist yet, and should arrive with a real consumer rather than as a
    // no-op that reports success.
}

} // namespace pulp::view
