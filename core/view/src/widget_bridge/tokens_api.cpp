// widget_bridge/tokens_api.cpp - theme-token registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_tokens_api(std::function<canvas::Color(const std::string&)> parse_color) {
    BridgeApiContext api{engine_};
    auto parseColor = std::move(parse_color);

    // setMotionToken(tokenName, value)
    register_bridge_function(api, "setMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = value;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // getMotionToken(tokenName) -> value
    register_bridge_function(api, "getMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto d = root_.theme().dimension(name);
        return choc::value::createFloat64(d.value_or(0.0f));
    });

    // String-valued theme-token lookup. setStringToken(name, value) /
    // getStringToken(name) parallel the
    // motion-token / color-token APIs but back onto `theme.strings`
    // (the same map that already stores design-system font names
    // imported via Stitch / W3C tokens - see design_import.cpp). The
    // React-side prop-applier consults this to resolve `var(--mono)`
    // in `fontFamily` / `color` / `borderColor` etc. before forwarding to
    // setFontFamily / setTextColor; otherwise Skia's font matcher receives
    // the literal string "var(--mono)" as a family name.
    register_bridge_function(api, "setStringToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<std::string>(1, "");
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.strings[name] = value;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // getStringToken(tokenName) -> string. Returns empty string when the
    // token isn't defined - callers (e.g. prop-applier resolveVar) treat
    // empty as "miss" and try the next lookup tier.
    register_bridge_function(api, "getStringToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto s = root_.theme().string_token(name);
        return choc::value::createString(s.value_or(std::string()));
    });

    // setColorToken(name, color) - set a color token on the root theme
    register_bridge_function(api, "setColorToken", [this, parseColor](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto color_str = args.get<std::string>(1, "");
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        auto c = parseColor(color_str);
        theme.colors[name] = c;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setDimensionToken(name, value) - set a dimension token on the root theme
    register_bridge_function(api, "setDimensionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto val = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = val;
        root_.set_theme(theme);
        return choc::value::Value();
    });
}

} // namespace pulp::view
