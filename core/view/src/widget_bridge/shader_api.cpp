// widget_bridge/shader_api.cpp - shader registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/custom_shader_host.hpp>
#include "api_registry.hpp"

#include <string>
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
