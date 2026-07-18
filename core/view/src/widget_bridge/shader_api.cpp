// widget_bridge/shader_api.cpp - shader registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/custom_shader_host.hpp>
#include "api_registry.hpp"

#include <string>

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
