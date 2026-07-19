#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/css_gradient.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <algorithm>
#include <string>

namespace pulp::view {

void BridgeRegistrars::register_widget_style_background_color_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setBackground", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v && !hex.empty()) v->set_background_color(parse_bridge_css_color(hex));
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_style_shadow_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // RN-shaped shadow primitive. RN's View style-prop names are
    // { shadowColor, shadowOffset: {x,y}, shadowOpacity, shadowRadius } and
    // do not carry spread or inset. Lower these onto the existing box-shadow
    // primitive while leaving setBoxShadow unchanged.
    register_bridge_function(api, "setShadow", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "#000000ff");
        auto ox = static_cast<float>(args.get<double>(2, 0.0));
        auto oy = static_cast<float>(args.get<double>(3, 0.0));
        auto opacity = static_cast<float>(args.get<double>(4, 1.0));
        auto radius = static_cast<float>(args.get<double>(5, 0.0));
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        auto color = parse_bridge_css_color(hex);
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        color.a *= opacity;
        v->set_box_shadow(ox, oy, /*blur=*/radius, /*spread=*/0.0f,
                          color, /*inset=*/false);
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_style_opacity_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setOpacity", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto alpha = args.get<double>(1, 1.0);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_opacity(static_cast<float>(alpha));
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_style_overflow_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setOverflow", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "hidden");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (mode == "visible")      v->set_overflow(View::Overflow::visible);
        else if (mode == "scroll")  v->set_overflow(View::Overflow::scroll);
        else                        v->set_overflow(View::Overflow::hidden);
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_style_background_gradient_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setBackgroundGradient", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto gradient = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v || gradient.empty()) return choc::value::Value();

        apply_css_background_gradient(*v, gradient, parse_bridge_css_color);
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_style_box_shadow_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setBoxShadow REPLACES the stack; addBoxShadow APPENDS to it. A CSS
    // shadow list `A, B` lowers to setBoxShadow(A) + addBoxShadow(B), which
    // keeps a lone setBoxShadow call meaning exactly one shadow — the
    // pre-existing contract — while letting a multi-layer declaration through
    // intact. Both take identical args, so they share one parse.
    auto apply = [&self](choc::javascript::ArgumentList args, bool append) {
        auto id = args.get<std::string>(0, "");
        auto ox = static_cast<float>(args.get<double>(1, 0));
        auto oy = static_cast<float>(args.get<double>(2, 2));
        auto blur = static_cast<float>(args.get<double>(3, 4));
        auto spread = static_cast<float>(args.get<double>(4, 0));
        auto hex = args.get<std::string>(5, "#00000050");
        bool inset = false;
        if (args.numArgs > 6 && args[6] != nullptr) {
            const auto& v6 = *args[6];
            if (v6.isBool()) inset = v6.getBool();
            else if (v6.isInt32() || v6.isInt64()) inset = v6.getInt64() != 0;
            else if (v6.isFloat32() || v6.isFloat64()) inset = v6.getFloat64() != 0.0;
            else if (v6.isString()) {
                auto s = std::string(v6.getString());
                inset = (s == "inset" || s == "true" || s == "1");
            }
        }
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return;
        const auto color = parse_bridge_css_color(hex);
        if (append) v->add_box_shadow(ox, oy, blur, spread, color, inset);
        else        v->set_box_shadow(ox, oy, blur, spread, color, inset);
    };

    register_bridge_function(api, "setBoxShadow", [apply](choc::javascript::ArgumentList args) {
        apply(args, /*append=*/false);
        return choc::value::Value();
    });

    register_bridge_function(api, "addBoxShadow", [apply](choc::javascript::ArgumentList args) {
        apply(args, /*append=*/true);
        return choc::value::Value();
    });

    register_bridge_function(api, "clearBoxShadow", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->clear_box_shadow();
        return choc::value::Value();
    });
}

}  // namespace pulp::view
