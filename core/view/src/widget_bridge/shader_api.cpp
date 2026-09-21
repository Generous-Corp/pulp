// widget_bridge/shader_api.cpp - shader registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/custom_shader_host.hpp>
#include "api_registry.hpp"

#include <string>
#include <string_view>
#include <algorithm>
#include <vector>

namespace pulp::view {
namespace {

choc::value::Value shader_result(bool success, const std::string& error) {
    auto result = choc::value::createObject("");
    result.addMember("success", choc::value::createBool(success));
    result.addMember("error", choc::value::createString(error));
    return result;
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

        auto* v = self.widget(id);
        if (!v) return shader_result(false, "No widget with id '" + id + "'");

        auto* host = dynamic_cast<CustomShaderHost*>(v);
        if (!host)
            return shader_result(
                false, "Widget '" + id + "' does not support custom shaders");

        if (sksl.empty())
            return shader_result(
                false, "Empty shader code — use clearWidgetShader() to remove a shader");

        auto error = canvas::Canvas::compile_sksl(sksl);
        if (!error.empty()) return shader_result(false, error);

        host->set_custom_shader(std::move(sksl));
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
        auto bindings = host->shader_value_bindings();
        auto it = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.uniform_name == uniform_name;
        });
        CustomShaderHost::ShaderValueBinding binding;
        binding.uniform_name = std::move(uniform_name);
        if (is_channel) binding.channel_name = channel_name;
        else binding.param_name = std::move(source);
        binding.neutral = neutral;
        if (it != bindings.end()) *it = std::move(binding);
        else bindings.push_back(std::move(binding));
        host->set_shader_value_bindings(std::move(bindings));
        self.record_binding_attempt(id, is_channel ? channel_name : source,
                                    BindingTarget::uniform, BindingOutcome::ok);
        self.request_repaint();
        return shader_result(true, "");
    });

    register_bridge_function(api, "clearWidgetShaderUniformBindings", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
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
        return result;
    });

    register_bridge_function(api, "setWidgetShaderReach", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto reach = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = self.widget(id);
        auto* host = v ? dynamic_cast<CustomShaderHost*>(v) : nullptr;
        if (!host) return shader_result(false, v ? "Widget does not support custom shaders" : "No widget with id '" + id + "'");
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
