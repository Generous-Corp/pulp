// widget_bridge/svg_api.cpp - SVG primitive registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/widgets/svg_rect.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include "api_registry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_svg_api(std::function<canvas::Color(const std::string&)> parse_color) {
    BridgeApiContext api{engine_};
    auto parseHexColor = std::move(parse_color);

    // SvgPathWidget bridge. Mirrors the API surface of CanvasWidget but for
    // inline <svg><path> icons. JS registers the
    // widget and pushes its path-data + paint attributes; the native
    // widget parses path-data once on set_path() and replays as Canvas2D
    // path commands inside paint().
    register_bridge_function(api, "createSvgPath", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<SvgPathWidget>();
        w->set_id(id);
        widgets_[id] = w.get();
        resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "setSvgPath", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto data = args.get<std::string>(1, "");
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_path(data);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setSvgViewBox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vw = args.get<double>(1, 0.0);
        auto vh = args.get<double>(2, 0.0);
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_viewbox(static_cast<float>(vw), static_cast<float>(vh));
        }
        return choc::value::Value();
    });

    // setSvgFill / setSvgStroke / setSvgStrokeWidth are polymorphic across
    // all SVG-primitive widgets so JSX consumers see a uniform fill/stroke
    // surface. SvgRectWidget and SvgLineWidget mirror the path API with the
    // same hex / "none" / strokeWidth semantics.
    register_bridge_function(api, "setSvgFill", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        const bool clear = hex.empty() || hex == "none";
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            if (clear) w->clear_fill();
            else       w->set_fill_color(parseHexColor(hex));
            // Solid color path wins over any previous gradient. Clear the
            // gradient slot so a later re-render with a solid fill doesn't
            // accidentally pick up a stale linear-gradient string.
            w->clear_fill_gradient();
        } else if (auto* r = dynamic_cast<SvgRectWidget*>(widget(id))) {
            if (clear) r->clear_fill();
            else       r->set_fill_color(parseHexColor(hex));
        }
        // SvgLineWidget has no fill semantics; this is intentionally a
        // no-op for line widgets so JSX consumers can pass `fill="none"`.
        return choc::value::Value();
    });

    // Gradient-fill bridge fn for SvgPathWidget. Accepts a CSS
    // linear-gradient string verbatim; the widget parses
    // it at paint time. Intended JSX usage: `<SvgLinearGradient id="g"
    // stops={...}>` + `<SvgPath fill="url(#g)" .../>`; prop-applier
    // resolves the gradient JSX to a CSS string and calls this fn.
    // Direct C++ consumers can also call it for testing.
    register_bridge_function(api, "setSvgFillGradient", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto value = args.get<std::string>(1, "");
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            if (value.empty()) w->clear_fill_gradient();
            else               w->set_fill_gradient(std::move(value));
        }
        // SvgRect / SvgLine - gradient fills follow up; bridge fn is a
        // no-op for those widget types so consumers don't crash.
        return choc::value::Value();
    });

    // fill-rule (nonzero | evenodd) for SvgPathWidget. Some importers lower
    // a stroked ellipse to a compound
    // annular `M…Z M…Z` fill that only renders the ring's hole under
    // even-odd winding; surfacing the prop lets a captured editor ship
    // those paths verbatim. SvgPath-only - SvgRect / SvgLine have no
    // compound-fill case, so the bridge fn is a no-op for them.
    register_bridge_function(api, "setSvgFillRule", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto rule = args.get<std::string>(1, "");
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_fill_rule(rule == "evenodd" ? canvas::FillRule::evenodd
                                               : canvas::FillRule::nonzero);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setSvgStroke", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        const bool clear = hex.empty() || hex == "none";
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            if (clear) w->clear_stroke();
            else       w->set_stroke_color(parseHexColor(hex));
        } else if (auto* r = dynamic_cast<SvgRectWidget*>(widget(id))) {
            if (clear) r->clear_stroke();
            else       r->set_stroke_color(parseHexColor(hex));
        } else if (auto* l = dynamic_cast<SvgLineWidget*>(widget(id))) {
            if (clear) l->clear_stroke();
            else       l->set_stroke_color(parseHexColor(hex));
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setSvgStrokeWidth", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto width = args.get<double>(1, 1.0);
        const float fw = static_cast<float>(width);
        if (auto* w = dynamic_cast<SvgPathWidget*>(widget(id))) {
            w->set_stroke_width(fw);
        } else if (auto* r = dynamic_cast<SvgRectWidget*>(widget(id))) {
            r->set_stroke_width(fw);
        } else if (auto* l = dynamic_cast<SvgLineWidget*>(widget(id))) {
            l->set_stroke_width(fw);
        }
        return choc::value::Value();
    });

    // SvgRectWidget bridge. Mirrors createSvgPath / setSvgPath. Geometry is
    // local to the widget origin (not
    // bounds()-translated). x/y default to 0, w/h default to 0 so an
    // unset rect is invisible by default.
    register_bridge_function(api, "createSvgRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<SvgRectWidget>();
        w->set_id(id);
        widgets_[id] = w.get();
        resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "setSvgRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = args.get<double>(1, 0.0);
        auto y = args.get<double>(2, 0.0);
        auto width = args.get<double>(3, 0.0);
        auto height = args.get<double>(4, 0.0);
        if (auto* w = dynamic_cast<SvgRectWidget*>(widget(id))) {
            w->set_rect(static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(width), static_cast<float>(height));
        }
        return choc::value::Value();
    });

    // SvgLineWidget bridge. Mirrors createSvgPath / setSvgRect. Endpoints
    // are local to the widget origin.
    register_bridge_function(api, "createSvgLine", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<SvgLineWidget>();
        w->set_id(id);
        widgets_[id] = w.get();
        resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "setSvgLine", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x1 = args.get<double>(1, 0.0);
        auto y1 = args.get<double>(2, 0.0);
        auto x2 = args.get<double>(3, 0.0);
        auto y2 = args.get<double>(4, 0.0);
        if (auto* w = dynamic_cast<SvgLineWidget*>(widget(id))) {
            w->set_line(static_cast<float>(x1), static_cast<float>(y1),
                        static_cast<float>(x2), static_cast<float>(y2));
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view
