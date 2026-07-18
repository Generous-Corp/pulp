// widget_bridge/list_style_api.cpp - list-style CSS registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void BridgeRegistrars::register_list_style_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // list-style cluster (listStyle / listStyleType / listStyleImage /
    // listStylePosition). Pulp doesn't model <li>/<ul>/<ol> semantics, so
    // the bridge stores the values verbatim on the View for round-trip and
    // future marker rendering.
    //
    // setListStyleType(id, "disc"|"circle"|"square"|"decimal"|"none"
    //                       |"decimal-leading-zero"|"lower-roman"|"upper-roman"
    //                       |"lower-alpha"|"upper-alpha"|"lower-latin"|"upper-latin"
    //                       |"lower-greek"|"armenian"|"georgian").
    //
    // The counter-style keywords (lower-roman, etc.) are stored on the
    // View::ListStyleType enum; marker glyph rendering is not wired yet.
    // Unknown keywords fall back to `disc` to match the longstanding default.
    register_bridge_function(api, "setListStyleType", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "disc");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (s == "none")                       v->set_list_style_type(View::ListStyleType::none);
        else if (s == "circle")                v->set_list_style_type(View::ListStyleType::circle);
        else if (s == "square")                v->set_list_style_type(View::ListStyleType::square);
        else if (s == "decimal")               v->set_list_style_type(View::ListStyleType::decimal);
        else if (s == "decimal-leading-zero")  v->set_list_style_type(View::ListStyleType::decimal_leading_zero);
        else if (s == "lower-roman")           v->set_list_style_type(View::ListStyleType::lower_roman);
        else if (s == "upper-roman")           v->set_list_style_type(View::ListStyleType::upper_roman);
        else if (s == "lower-alpha")           v->set_list_style_type(View::ListStyleType::lower_alpha);
        else if (s == "upper-alpha")           v->set_list_style_type(View::ListStyleType::upper_alpha);
        else if (s == "lower-latin")           v->set_list_style_type(View::ListStyleType::lower_latin);
        else if (s == "upper-latin")           v->set_list_style_type(View::ListStyleType::upper_latin);
        else if (s == "lower-greek")           v->set_list_style_type(View::ListStyleType::lower_greek);
        else if (s == "armenian")              v->set_list_style_type(View::ListStyleType::armenian);
        else if (s == "georgian")              v->set_list_style_type(View::ListStyleType::georgian);
        else                                   v->set_list_style_type(View::ListStyleType::disc);
        return choc::value::Value();
    });

    // setListStyleImage(id, "url(...)" or "none"). Stored verbatim;
    // bullet-image rendering is deferred (same caveat as backgroundImage).
    register_bridge_function(api, "setListStyleImage", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto url = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (url == "none") v->set_list_style_image("");
        else               v->set_list_style_image(url);
        return choc::value::Value();
    });

    // setListStylePosition(id, "outside"|"inside").
    register_bridge_function(api, "setListStylePosition", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "outside");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (s == "inside") v->set_list_style_position(View::ListStylePosition::inside);
        else               v->set_list_style_position(View::ListStylePosition::outside);
        return choc::value::Value();
    });
}

} // namespace pulp::view
