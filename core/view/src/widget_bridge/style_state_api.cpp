// widget_bridge/style_state_api.cpp - state and debug style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_widget_style_state_api() {
    BridgeApiContext api{engine_};

    // setStateStyle(id, state, property, value) - declarative state-driven styling.
    // Replaces manual hover callback wiring. States: hover, active, focus, disabled.
    register_bridge_function(api, "setStateStyle", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto state = args.get<std::string>(1, "hover");
        auto prop = args.get<std::string>(2, "");
        auto val_str = args.get<std::string>(3, "");
        auto* v = widget(id);
        if (!v || prop.empty()) return choc::value::Value();

        // Store the original value on first call.
        // Then register hover/focus callbacks that apply/revert.

        if (state == "hover") {
            // Capture current value as "normal" state.
            if (prop == "background") {
                auto target_color = parse_bridge_css_color(val_str);
                auto* view = v;
                // Wire hover enter/leave to apply/revert background.
                view->on_hover_enter = [this, id, view, target_color]() {
                    view->set_background_color(target_color);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view]() {
                    view->clear_background_color();
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "scale") {
                float target_scale = std::stof(val_str);
                auto* view = v;
                view->on_hover_enter = [this, id, view, target_scale]() {
                    view->set_scale(target_scale);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view]() {
                    view->set_scale(1.0f);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "opacity") {
                float target_opacity = std::stof(val_str);
                auto* view = v;
                float original = view->opacity();
                view->on_hover_enter = [this, id, view, target_opacity]() {
                    view->set_opacity(target_opacity);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view, original]() {
                    view->set_opacity(original);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            }
        }
        return choc::value::Value();
    });

    // setEnabled(id, bool) - CSS :disabled equivalent.
    register_bridge_function(api, "setEnabled", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto enabled = args.get<double>(1, 1) > 0.5;
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_enabled(enabled);
        return choc::value::Value();
    });

    // setDebugPaint(bool) - draw bounding box outlines on all views.
    register_bridge_function(api, "setDebugPaint", [this](choc::javascript::ArgumentList args) {
        auto on = args.get<double>(0, 0) > 0.5;
        // Store as a dimension token on root theme.
        auto theme = root_.theme();
        theme.dimensions["debug.paint"] = on ? 1.0f : 0.0f;
        root_.set_theme(theme);
        return choc::value::Value();
    });
}

} // namespace pulp::view
