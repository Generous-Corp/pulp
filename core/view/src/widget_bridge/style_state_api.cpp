// widget_bridge/style_state_api.cpp - state and debug style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "bridge_dispatch.hpp"
#include "css_color.hpp"

#include <functional>
#include <cmath>
#include <string>
#include <utility>

namespace pulp::view {

void BridgeRegistrars::register_widget_style_state_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setStateStyle(id, state, property, value) - declarative state-driven styling.
    // Replaces manual hover callback wiring. States: hover, active, focus, disabled.
    register_bridge_function(api, "setStateStyle", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto state = args.get<std::string>(1, "hover");
        auto prop = args.get<std::string>(2, "");
        auto val_str = args.get<std::string>(3, "");
        auto* v = self.widget(id);
        if (!v || prop.empty()) return choc::value::Value();

        // Store the original value on first call.
        // Then register hover/focus callbacks that apply/revert.

        if (state == "hover") {
            auto alive = self.callback_alive_;
            // Capture current value as "normal" state.
            if (prop == "background") {
                auto target_color = parse_bridge_css_color(val_str);
                auto* view = v;
                // Wire hover enter/leave to apply/revert background.
                view->on_hover_enter = [&self, alive, id, view, target_color]() {
                    BridgeCallbackScope scope(alive);
                    view->set_background_color(target_color);
                    self.engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [&self, alive, id, view]() {
                    BridgeCallbackScope scope(alive);
                    view->clear_background_color();
                    self.engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "scale") {
                float target_scale = std::stof(val_str);
                auto* view = v;
                view->on_hover_enter = [&self, alive, id, view, target_scale]() {
                    BridgeCallbackScope scope(alive);
                    view->set_scale(target_scale);
                    self.engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [&self, alive, id, view]() {
                    BridgeCallbackScope scope(alive);
                    view->set_scale(1.0f);
                    self.engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "opacity") {
                float target_opacity = std::stof(val_str);
                auto* view = v;
                float original = view->opacity();
                view->on_hover_enter = [&self, alive, id, view, target_opacity]() {
                    BridgeCallbackScope scope(alive);
                    view->set_opacity(target_opacity);
                    self.engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [&self, alive, id, view, original]() {
                    BridgeCallbackScope scope(alive);
                    view->set_opacity(original);
                    self.engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            }
        }
        return choc::value::Value();
    });

    // setCapturedPaintRect(id, left, top, right, bottom, devicePixelRatio)
    // The CSS-px-snapped rect a browser capture recorded for this box's own
    // paint, in page CSS px, at the capture's DPR. A box (and a theme part)
    // then paints in that rect, as Chromium did, instead of in its fractional
    // layout box; see View::self_paint_bounds(). A rect that is not finite and
    // non-empty, or a DPR that is not positive, is ignored.
    register_bridge_function(
        api, "setCapturedPaintRect", [&self](choc::javascript::ArgumentList args) {
            auto* v = self.widget(args.get<std::string>(0, ""));
            const auto left = static_cast<float>(args.get<double>(1, 0.0));
            const auto top = static_cast<float>(args.get<double>(2, 0.0));
            const auto right = static_cast<float>(args.get<double>(3, 0.0));
            const auto bottom = static_cast<float>(args.get<double>(4, 0.0));
            const auto dpr = static_cast<float>(args.get<double>(5, 0.0));
            if (v && std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
                std::isfinite(bottom) && std::isfinite(dpr) && dpr > 0.0f && right > left &&
                bottom > top)
                v->set_captured_box_paint_rect(left, top, right, bottom, dpr);
            return choc::value::Value();
        });

    // setEnabled(id, bool) - CSS :disabled equivalent.
    register_bridge_function(api, "setEnabled", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto enabled = args.get<double>(1, 1) > 0.5;
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_enabled(enabled);
        return choc::value::Value();
    });

    // setDebugPaint(bool) - draw bounding box outlines on all views.
    register_bridge_function(api, "setDebugPaint", [&self](choc::javascript::ArgumentList args) {
        auto on = args.get<double>(0, 0) > 0.5;
        // Store as a dimension token on root theme.
        auto theme = self.root_.theme();
        theme.dimensions["debug.paint"] = on ? 1.0f : 0.0f;
        self.root_.set_theme(theme);
        return choc::value::Value();
    });
}

} // namespace pulp::view
