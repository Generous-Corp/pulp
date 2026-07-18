// widget_bridge/border_box_api.cpp - border-box style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <string>

namespace pulp::view {

void BridgeRegistrars::register_widget_border_box_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setBorder", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto width = args.get<double>(2, 1.0);
        auto radius = args.get<double>(3, 0.0);
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v && !hex.empty()) v->set_border(parse_bridge_css_color(hex), (float)width, (float)radius);
        return choc::value::Value();
    });

    // setBorderSide(id, side, width, color) — per-side border
    register_bridge_function(api, "setBorderSide", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto side = args.get<std::string>(1, "");
        auto width = static_cast<float>(args.get<double>(2, 1.0));
        auto hex = args.get<std::string>(3, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) {
            auto c = hex.empty() ? canvas::Color::rgba8(128,128,128) : parse_bridge_css_color(hex);
            if (side == "top") v->set_border_top(c, width);
            else if (side == "right") v->set_border_right(c, width);
            else if (side == "bottom") v->set_border_bottom(c, width);
            else if (side == "left") v->set_border_left(c, width);
        }
        return choc::value::Value();
    });

    // setCornerRadius(id, corner, radius) — per-corner border-radius.
    // "All" (uniform shorthand) is the codegen default for Figma frames
    // that carry a single border-radius value; per-corner identifiers
    // are emitted only when a frame has asymmetric radii. Without the
    // "All" branch, the figma-plugin lane's setCornerRadius calls fell
    // through silently and panel corners stayed sharp.
    register_bridge_function(api, "setCornerRadius", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto corner = args.get<std::string>(1, "");
        auto r = static_cast<float>(args.get<double>(2, 0));
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) {
            if (corner == "All") v->set_border_radius(r);
            else if (corner == "TopLeft") v->set_corner_radius_tl(r);
            else if (corner == "TopRight") v->set_corner_radius_tr(r);
            else if (corner == "BottomLeft") v->set_corner_radius_bl(r);
            else if (corner == "BottomRight") v->set_corner_radius_br(r);
        }
        return choc::value::Value();
    });

    // Standalone border setters for RN parity. The shorthand setBorder(id,
    // color, width, radius) is convenient for atomic style application, but
    // @pulp/react's prop-applier needs per-attribute
    // setters so individual JSX style props (`borderColor`, `borderWidth`,
    // `borderRadius`) can update without recomputing the others.
    //
    // setBorderColor(id, hex)
    register_bridge_function(api, "setBorderColor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v && !hex.empty()) v->set_border_color(parse_bridge_css_color(hex));
        return choc::value::Value();
    });

    // setBorderWidth(id, width)
    register_bridge_function(api, "setBorderWidth", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto width = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_border_width(width);
        return choc::value::Value();
    });

    // setBorderStyle(id, "solid"|"dashed"|"dotted"|...) maps the CSS
    // border-style keyword to View::BorderStyle.
    // Skia paint installs the dashed / dotted SkDashPathEffect at stroke
    // time; double / groove / ridge / inset / outset currently degrade to
    // solid. none / hidden short-circuit the stroke entirely.
    register_bridge_function(api, "setBorderStyle", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "solid");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (s == "dashed")        v->set_border_style(View::BorderStyle::dashed);
        else if (s == "dotted")   v->set_border_style(View::BorderStyle::dotted);
        else if (s == "double")   v->set_border_style(View::BorderStyle::double_);
        else if (s == "groove")   v->set_border_style(View::BorderStyle::groove);
        else if (s == "ridge")    v->set_border_style(View::BorderStyle::ridge);
        else if (s == "inset")    v->set_border_style(View::BorderStyle::inset);
        else if (s == "outset")   v->set_border_style(View::BorderStyle::outset);
        else if (s == "none")     v->set_border_style(View::BorderStyle::none);
        else if (s == "hidden")   v->set_border_style(View::BorderStyle::hidden);
        else                      v->set_border_style(View::BorderStyle::solid);
        return choc::value::Value();
    });
}

} // namespace pulp::view
