// widget_bridge/border_side_api.cpp - per-side border style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_widget_border_side_api(std::function<canvas::Color(const std::string&)> parse_color) {
    auto parseHexColor = std::move(parse_color);

    // pulp #1026 - Per-side border color/width shorthands (RN parity).
    // RN exposes `borderTopColor`, `borderTopWidth`, etc. as separate
    // style props; pulp's existing `setBorderSide(id, side, width, color)`
    // sets both at once which is awkward for a prop-by-prop applier. The
    // setBorderTop/Right/Bottom/Left{Color,Width} setters route through
    // the per-side fields preserving whichever attribute the call doesn't
    // specify.
    auto applyBorderSide = [](View* v, const std::string& side,
                              std::optional<canvas::Color> color,
                              std::optional<float> width) {
        if (!v) return;
        // pulp #1026 - preserve the unrelated attribute when a per-side
        // setter is called for only color OR only width, matching how
        // RN's JSX prop-applier emits property updates one at a time.
        // pulp #1566 - route through the split color-only / width-only
        // setters so that `setBorderTopColor` does NOT mark the per-edge
        // WIDTH as explicitly set (which would let a stale 0 override
        // the uniform `borderWidth` shorthand). Symmetrically,
        // `setBorderTopWidth(0)` MUST mark the edge as explicitly set so
        // it overrides the shorthand on that edge per CSS / RN semantics.
        if (color.has_value()) {
            if (side == "top")         v->set_border_top_color(*color);
            else if (side == "right")  v->set_border_right_color(*color);
            else if (side == "bottom") v->set_border_bottom_color(*color);
            else if (side == "left")   v->set_border_left_color(*color);
        }
        if (width.has_value()) {
            if (side == "top")         v->set_border_top_width(*width);
            else if (side == "right")  v->set_border_right_width(*width);
            else if (side == "bottom") v->set_border_bottom_width(*width);
            else if (side == "left")   v->set_border_left_width(*width);
        }
    };

    engine_.register_function("setBorderTopColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "top", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });

    engine_.register_function("setBorderRightColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "right", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });

    engine_.register_function("setBorderBottomColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "bottom", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });

    engine_.register_function("setBorderLeftColor", [this, parseHexColor, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) applyBorderSide(v, "left", parseHexColor(hex), std::nullopt);
        return choc::value::Value();
    });

    engine_.register_function("setBorderTopWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "top", std::nullopt, w);
        return choc::value::Value();
    });

    engine_.register_function("setBorderRightWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "right", std::nullopt, w);
        return choc::value::Value();
    });

    engine_.register_function("setBorderBottomWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "bottom", std::nullopt, w);
        return choc::value::Value();
    });

    engine_.register_function("setBorderLeftWidth", [this, applyBorderSide](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto w = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) applyBorderSide(v, "left", std::nullopt, w);
        return choc::value::Value();
    });
}

} // namespace pulp::view
