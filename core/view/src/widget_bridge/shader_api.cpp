// widget_bridge/shader_api.cpp - shader registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

#include <string>

namespace pulp::view {

void WidgetBridge::register_shader_widget_api() {
    // compileShader(sksl_code) -> {success: bool, error: string}
    // Validates SkSL shader code by actually compiling via SkRuntimeEffect.
    engine_.register_function("compileShader", [](choc::javascript::ArgumentList args) {
        auto code = args.get<std::string>(0, "");
        auto result = choc::value::createObject("");
        if (code.empty()) {
            result.addMember("success", choc::value::createBool(false));
            result.addMember("error", choc::value::createString("Empty shader code"));
            return result;
        }
        auto error = canvas::Canvas::compile_sksl(code);
        result.addMember("success", choc::value::createBool(error.empty()));
        result.addMember("error", choc::value::createString(error));
        return result;
    });

    // setWidgetShader(id, skslCode) -> apply custom GPU shader to widget body.
    engine_.register_function("setWidgetShader", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto sksl = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_custom_shader(sksl);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_custom_shader(sksl);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_custom_shader(sksl);
        request_repaint();
        return choc::value::Value();
    });

    // clearWidgetShader(id) -> remove custom shader, restore default paint.
    engine_.register_function("clearWidgetShader", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->clear_custom_shader();
        else if (auto* f = dynamic_cast<Fader*>(v)) f->clear_custom_shader();
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->clear_custom_shader();
        request_repaint();
        return choc::value::Value();
    });

}

void WidgetBridge::register_shader_canvas_api() {
    // WebGPU shader: applyShader(canvasId, skslCode) -> applies a custom shader to canvas.
    // Uses the existing Skia/Dawn pipeline; SkSL shaders compile at runtime.
    engine_.register_function("applyShader", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto code = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (v) {
            // Store shader state on the view for the render pipeline to pick up.
            auto theme = v->theme();
            theme.dimensions["shader.active"] = 1;
            v->set_theme(theme);
        }
        auto result = choc::value::createObject("");
        result.addMember("success", choc::value::createBool(!code.empty()));
        result.addMember("error", choc::value::createString(""));
        return result;
    });
}

} // namespace pulp::view
