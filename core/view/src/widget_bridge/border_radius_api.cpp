// widget_bridge/border_radius_api.cpp - border radius style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>
#include <utility>

namespace pulp::view {

void BridgeRegistrars::register_widget_border_radius_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setBorderRadius(id, radius) - uniform corner radius. Per-corner
    // setters (setBorderTopLeftRadius / TopRight / BottomLeft / BottomRight)
    // override individual corners on top of the uniform value.
    //
    // Accepts either a number (px) or a string with % suffix (e.g. "50%").
    // Percent values are stored separately and resolved at
    // paint time as `pct * 0.01 * min(width, height)` so the radius
    // tracks the View's actual bounds.
    auto parseRadiusArg = [](choc::javascript::ArgumentList& args, int idx) -> std::pair<float, float> {
        // returns {px_radius, pct_radius}: at most one is non-zero.
        auto sval = args.get<std::string>(idx, "");
        if (!sval.empty() && sval.back() == '%') {
            try { return {0.0f, std::stof(sval.substr(0, sval.size() - 1))}; }
            catch (...) { return {0.0f, 0.0f}; }
        }
        return {static_cast<float>(args.get<double>(idx, 0.0)), 0.0f};
    };

    register_bridge_function(api, "setBorderRadius", [&self, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) {
            v->set_border_radius_pct(pct);
        } else {
            v->set_border_radius(px);
        }
        return choc::value::Value();
    });

    // Per-corner border-radius shorthands for RN parity. Equivalent to
    // `setCornerRadius(id, "TopLeft", r)` but matches the
    // RN style-prop name 1:1 so @pulp/react's prop-applier can bind them
    // without a translation layer. Sets the `has_corner_radii_` flag on
    // the View; paint_all() then routes background/border through the
    // per-corner path builder rather than fill_rounded_rect. Uses the same
    // %-string handling as setBorderRadius.
    register_bridge_function(api, "setBorderTopLeftRadius", [&self, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_tl_pct(pct);
        else v->set_corner_radius_tl(px);
        return choc::value::Value();
    });

    register_bridge_function(api, "setBorderTopRightRadius", [&self, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_tr_pct(pct);
        else v->set_corner_radius_tr(px);
        return choc::value::Value();
    });

    register_bridge_function(api, "setBorderBottomLeftRadius", [&self, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_bl_pct(pct);
        else v->set_corner_radius_bl(px);
        return choc::value::Value();
    });

    register_bridge_function(api, "setBorderBottomRightRadius", [&self, parseRadiusArg](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto [px, pct] = parseRadiusArg(args, 1);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (pct > 0.0f) v->set_corner_radius_br_pct(pct);
        else v->set_corner_radius_br(px);
        return choc::value::Value();
    });
}

} // namespace pulp::view
